#pragma once

#include "duckdb.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace duckdb {

//! Counters for everything that left this process, reported by jev_stats().
struct JevStats {
	std::atomic<uint64_t> requests {0};
	std::atomic<uint64_t> input_tokens {0};
	std::atomic<uint64_t> output_tokens {0};
	std::atomic<uint64_t> rows_evaluated {0};
	std::atomic<uint64_t> cache_hits {0};
	std::atomic<uint64_t> api_ms {0};
	std::atomic<uint64_t> errors {0};
	std::atomic<uint64_t> retries {0};
	std::atomic<uint64_t> in_flight {0};
	//! Rows answered by another query's request for the same prompt that was already in flight
	std::atomic<uint64_t> shared_in_flight {0};
	//! Rows claimed and sent again after the query sending them failed
	std::atomic<uint64_t> reclaimed {0};
	//! Time requests spent waiting for the rate limiter
	std::atomic<uint64_t> rate_limited_ms {0};
};

//! The same counters for one query on one connection, reported by snx_jev_last_query_stats().
struct JevQueryCounters {
	std::atomic<uint64_t> requests {0};
	std::atomic<uint64_t> input_tokens {0};
	std::atomic<uint64_t> output_tokens {0};
	std::atomic<uint64_t> rows_sent {0};
	std::atomic<uint64_t> cache_hits {0};
	std::atomic<uint64_t> shared_in_flight {0};
	std::atomic<uint64_t> retries {0};
	std::atomic<uint64_t> rate_limited_ms {0};
};

//! Per connection: what the current query sent, and what the last query that used the
//! extension sent. Search reads the latter right after its query, on the same connection,
//! to report and log the spend. Nothing here is shared between connections.
class JevQueryState : public ClientContextState {
public:
	static shared_ptr<JevQueryState> Get(ClientContext &context);

	void QueryBegin(ClientContext &context) override;
	void QueryEnd(ClientContext &context) override;
	//! The last completed query that sent or looked up anything, as JSON.
	string LastQueryJSON();

	JevQueryCounters current;
	//! Set by any lookup or request in the current query.
	std::atomic<bool> used {false};

private:
	std::mutex lock;
	string last_json;
};

//! What a cache lookup found for a key.
enum class JevClaim : uint8_t {
	//! answered from the cache
	HIT,
	//! another query is sending this key right now: wait for its answer
	WAIT,
	//! nobody has it: the caller now owns sending it, and must Complete() it
	CLAIMED
};

//! Bounded pool of request threads.
//!
//! DuckDB already runs the scan on several threads, so a semaphore per query would not
//! bound anything: the ceiling has to be shared. Every request in the process goes
//! through this pool, which is what makes snx_jev_concurrency mean what it says.
class JevThreadPool {
public:
	explicit JevThreadPool(idx_t worker_count);
	~JevThreadPool();

	//! Queues a task. The returned future rethrows whatever the task threw.
	std::future<void> Submit(std::function<void()> task);
	idx_t WorkerCount() const {
		return worker_count;
	}

private:
	void WorkerLoop();

	idx_t worker_count;
	vector<std::thread> workers;
	std::deque<std::packaged_task<void()>> tasks;
	std::mutex lock;
	std::condition_variable work_available;
	bool shutting_down = false;
};

//! The answer cache, the counters and the request pool, shared by every connection in
//! the process. pg-jev keeps one cache per backend session; a DuckDB process is the
//! closest equivalent.
class JevState {
public:
	static JevState &Get();

	bool Lookup(const string &key, string &answer);
	void Store(const string &key, const string &answer, idx_t max_entries);
	//! Single-flight lookup: a cache hit, a request already in flight to wait on, or a claim.
	//! Two queries that meet the same uncached key at once send it once.
	JevClaim LookupOrClaim(const string &key, string &answer, std::shared_future<string> &in_flight_answer);
	//! Ends a claim: caches a non-empty answer and hands it to every waiter. An empty answer
	//! means the request failed; the waiters get it and fail too.
	void Complete(const string &key, const string &answer, idx_t max_entries);

	//! Blocks until the process-wide rate limit allows one more request. A token bucket:
	//! up to per_minute / 20 requests at once, refilled at per_minute a minute. 0 = no limit.
	//! Returns how long it waited, in milliseconds.
	uint64_t AcquireRequestPermit(idx_t per_minute);
	void Clear();
	idx_t CachedAnswers();

	//! The shared pool, rebuilt when snx_jev_concurrency changes. Callers keep the
	//! shared_ptr for as long as they have tasks in flight, so a resize never pulls
	//! the pool out from under a running query.
	shared_ptr<JevThreadPool> Pool(idx_t workers);

	JevStats stats;

private:
	std::mutex lock;
	std::unordered_map<string, string> cache;
	std::deque<string> insertion_order;
	struct InFlight {
		std::promise<string> promise;
		std::shared_future<string> answer;
	};
	std::unordered_map<string, unique_ptr<InFlight>> in_flight;
	shared_ptr<JevThreadPool> pool;

	std::mutex limiter_lock;
	double limiter_tokens = -1;
	std::chrono::steady_clock::time_point limiter_refilled;
};

} // namespace duckdb
