#pragma once

#include "duckdb.hpp"

#include <atomic>
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
	shared_ptr<JevThreadPool> pool;
};

} // namespace duckdb
