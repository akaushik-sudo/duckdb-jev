#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! What this process may send, and where: read from the environment once, when the
//! extension loads, and never again.
//!
//! Nothing in SQL can change either: there is no URL or key setting. The endpoint is
//! TypeSafe's API; SNX_JEV_API_URL may replace it, but only with a loopback address (the
//! test mock). Any other value is recorded as a refusal, and every request fails with it
//! before a connection is opened.
//!
//! The key comes from TYPESAFE_API_KEY. That keeps it out of current_setting() and
//! duckdb_settings(), but not out of /proc/self/environ on Linux, which any query can read
//! while DuckDB's external access is enabled. The deployment has to switch that off.
struct JevEnvironment {
	//! Canonical "scheme://host:port"; empty when the endpoint was refused.
	string origin;
	//! The request path, e.g. "/v1/systemone".
	string path;
	//! Why the endpoint was refused; empty when origin is set.
	string refusal;
	//! TYPESAFE_API_KEY as it was at load; empty when unset.
	string api_key;

	//! Resolved once, on first use (the extension calls it at load).
	static const JevEnvironment &Get();
};

//! Every snx_jev_* setting, resolved for one query.
//!
//! Settings are read once per bind (i.e. per query), so `SET snx_jev_batch_size = 40`
//! takes effect on the next statement and never changes mid-scan.
struct JevConfig {
	//! JevEnvironment::Get().api_key. There is deliberately no setting for it: a setting's
	//! value can be read back by any query through current_setting() / duckdb_settings().
	string api_key;
	//! Namespaces the answer cache: the connection's `scope_customer_id` variable, which
	//! the search service sets from the verified token. Empty when the variable is unset
	//! (a hand-run session), which is a namespace of its own.
	string cache_scope;
	string model = "jev-latest";
	double threshold = 0.5;
	idx_t batch_size = 20;
	idx_t concurrency = 16;
	double timeout = 30.0;
	idx_t max_retries = 6;
	idx_t max_rows_per_statement = 0;
	idx_t max_chars_per_statement = 0;
	idx_t cache_max_entries = 200000;

	//! Registers the settings on the database config. Called once, when the extension loads.
	static void RegisterSettings(DBConfig &config);
	//! Resolves the settings as they stand for this query.
	static JevConfig FromContext(ClientContext &context);
	//! Throws if the endpoint was refused or no API key was configured. Called when a
	//! request is about to be sent, never at bind time, so a query answered entirely from
	//! cache needs neither.
	void RequireSendable() const;
};

} // namespace duckdb
