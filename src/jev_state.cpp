#include "jev_state.hpp"

#include "duckdb/main/client_context.hpp"

#include "json.hpp"

#include <thread>

namespace duckdb {

JevThreadPool::JevThreadPool(idx_t worker_count_p) : worker_count(worker_count_p) {
	workers.reserve(worker_count);
	for (idx_t i = 0; i < worker_count; i++) {
		workers.emplace_back([this]() { WorkerLoop(); });
	}
}

JevThreadPool::~JevThreadPool() {
	{
		std::unique_lock<std::mutex> guard(lock);
		shutting_down = true;
	}
	work_available.notify_all();
	for (auto &worker : workers) {
		if (worker.joinable()) {
			worker.join();
		}
	}
}

void JevThreadPool::WorkerLoop() {
	while (true) {
		std::packaged_task<void()> task;
		{
			std::unique_lock<std::mutex> guard(lock);
			work_available.wait(guard, [this]() { return shutting_down || !tasks.empty(); });
			if (tasks.empty()) {
				// Shutting down: the queue is drained, so no future is left unfulfilled.
				return;
			}
			task = std::move(tasks.front());
			tasks.pop_front();
		}
		task();
	}
}

std::future<void> JevThreadPool::Submit(std::function<void()> task) {
	std::packaged_task<void()> packaged(std::move(task));
	auto future = packaged.get_future();
	{
		std::unique_lock<std::mutex> guard(lock);
		tasks.push_back(std::move(packaged));
	}
	work_available.notify_one();
	return future;
}

JevState &JevState::Get() {
	// Deliberately leaked: the pool's threads must not be joined from a static
	// destructor while DuckDB is still tearing down.
	static JevState *state = new JevState();
	return *state;
}

bool JevState::Lookup(const string &key, string &answer) {
	std::unique_lock<std::mutex> guard(lock);
	auto entry = cache.find(key);
	if (entry == cache.end()) {
		return false;
	}
	answer = entry->second;
	return true;
}

void JevState::Store(const string &key, const string &answer, idx_t max_entries) {
	std::unique_lock<std::mutex> guard(lock);
	auto inserted = cache.insert(make_pair(key, answer));
	if (!inserted.second) {
		inserted.first->second = answer;
		return;
	}
	insertion_order.push_back(key);
	while (max_entries > 0 && cache.size() > max_entries && !insertion_order.empty()) {
		auto &oldest = insertion_order.front();
		if (oldest != key) {
			cache.erase(oldest);
		}
		insertion_order.pop_front();
	}
}

void JevState::Clear() {
	std::unique_lock<std::mutex> guard(lock);
	cache.clear();
	insertion_order.clear();
}

idx_t JevState::CachedAnswers() {
	std::unique_lock<std::mutex> guard(lock);
	return cache.size();
}

JevClaim JevState::LookupOrClaim(const string &key, string &answer, std::shared_future<string> &in_flight_answer) {
	std::unique_lock<std::mutex> guard(lock);
	auto entry = cache.find(key);
	if (entry != cache.end()) {
		answer = entry->second;
		return JevClaim::HIT;
	}
	auto sending = in_flight.find(key);
	if (sending != in_flight.end()) {
		in_flight_answer = sending->second->answer;
		return JevClaim::WAIT;
	}
	auto claim = make_uniq<InFlight>();
	claim->answer = claim->promise.get_future().share();
	in_flight.insert(make_pair(key, std::move(claim)));
	return JevClaim::CLAIMED;
}

void JevState::Complete(const string &key, const string &answer, idx_t max_entries) {
	unique_ptr<InFlight> claim;
	{
		std::unique_lock<std::mutex> guard(lock);
		auto entry = in_flight.find(key);
		if (entry == in_flight.end()) {
			return;
		}
		claim = std::move(entry->second);
		in_flight.erase(entry);
	}
	// Cache first, so a query that looks the key up after the claim is gone finds it.
	if (!answer.empty()) {
		Store(key, answer, max_entries);
	}
	claim->promise.set_value(answer);
}

uint64_t JevState::AcquireRequestPermit(idx_t per_minute) {
	if (per_minute == 0) {
		return 0;
	}
	auto rate = static_cast<double>(per_minute) / 60.0; // requests a second
	auto capacity = MaxValue<double>(1.0, static_cast<double>(per_minute) / 20.0);
	double wait_seconds = 0;
	{
		std::unique_lock<std::mutex> guard(limiter_lock);
		auto now = std::chrono::steady_clock::now();
		if (limiter_tokens < 0 && limiter_refilled.time_since_epoch().count() == 0) {
			limiter_tokens = capacity;
		} else {
			std::chrono::duration<double> elapsed = now - limiter_refilled;
			limiter_tokens = MinValue<double>(capacity, limiter_tokens + elapsed.count() * rate);
		}
		limiter_refilled = now;
		// Reserve the token now, even when it is not there yet: the wait is then the time until
		// it will be, and requests are served in the order they asked.
		limiter_tokens -= 1;
		if (limiter_tokens < 0) {
			wait_seconds = -limiter_tokens / rate;
		}
	}
	if (wait_seconds <= 0) {
		return 0;
	}
	auto waited_ms = static_cast<uint64_t>(wait_seconds * 1000);
	stats.rate_limited_ms += waited_ms;
	std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(wait_seconds * 1000000)));
	return waited_ms;
}

shared_ptr<JevQueryState> JevQueryState::Get(ClientContext &context) {
	return context.registered_state->GetOrCreate<JevQueryState>("snx_jev_query_state");
}

void JevQueryState::QueryBegin(ClientContext &context) {
	current.requests = 0;
	current.input_tokens = 0;
	current.output_tokens = 0;
	current.rows_sent = 0;
	current.cache_hits = 0;
	current.shared_in_flight = 0;
	current.retries = 0;
	current.rate_limited_ms = 0;
	used = false;
}

void JevQueryState::QueryEnd(ClientContext &context) {
	if (!used) {
		// A query that never touched the extension (snx_jev_last_query_stats() itself, say)
		// leaves the last report alone.
		return;
	}
	auto input_tokens = current.input_tokens.load();
	nlohmann::ordered_json report;
	report["requests"] = current.requests.load();
	report["rows_sent"] = current.rows_sent.load();
	report["cache_hits"] = current.cache_hits.load();
	report["shared_in_flight"] = current.shared_in_flight.load();
	report["retries"] = current.retries.load();
	report["rate_limited_ms"] = current.rate_limited_ms.load();
	report["input_tokens"] = input_tokens;
	report["output_tokens"] = current.output_tokens.load();
	// jev-1.13 list price; output tokens are free.
	report["estimated_cost_usd"] = static_cast<double>(input_tokens) * 0.042 / 1000000.0;
	std::unique_lock<std::mutex> guard(lock);
	last_json = report.dump();
}

string JevQueryState::LastQueryJSON() {
	std::unique_lock<std::mutex> guard(lock);
	return last_json.empty() ? string("null") : last_json;
}

shared_ptr<JevThreadPool> JevState::Pool(idx_t workers) {
	std::unique_lock<std::mutex> guard(lock);
	if (!pool || pool->WorkerCount() != workers) {
		pool = make_shared_ptr<JevThreadPool>(workers);
	}
	return pool;
}

} // namespace duckdb
