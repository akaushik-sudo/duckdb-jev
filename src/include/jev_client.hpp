#pragma once

#include "duckdb.hpp"
#include "jev_config.hpp"

namespace duckdb {

//! Extension version, reported by jev_version() and sent as the User-Agent.
#define JEV_VERSION "0.1.0"

//! One question, asked of every row in a batch.
struct JevQuestion {
	//! "noul" (yes/no probability), "score" (ordered levels) or "choice" (one of n)
	string kind;
	//! The condition or question, in plain language
	string query;
	//! Levels for "score", labels for "choice"; empty for "noul"
	vector<string> options;
	//! What each label means, parallel to `options` for "choice" ("" = no description);
	//! for "noul", either empty or {what true means, what false means}. The descriptions
	//! are what separate close labels, so they travel with the question.
	vector<string> descriptions;

	//! Identifies this question for the answer cache. Two calls with the same
	//! (kind, query, options, descriptions) share cached answers, so jev_choice() and
	//! jev_confidence() on the same arguments cost one request, not two.
	string CacheKey() const;
};

//! Every question asked of a row, answered in the same request over the same state.
//! Asking two questions of a row this way costs one request, not two.
struct JevQuestionSet {
	vector<JevQuestion> questions;

	string CacheKey() const;
};

//! Judges one batch of rows: one request, one shared state, one question per (row, question).
//! Returns the raw answer JSON per row and question: result[row][question], in the order given.
//! Retries the retryable failures (429, 5xx, dropped connections) and throws otherwise.
vector<vector<string>> JevCallAPI(const JevConfig &config, const JevQuestionSet &set, const vector<string> &rows_json);

//! Size of a request for `set`, so batches can be closed before they outgrow the API's
//! context window: the request with no rows, and what each row adds on top of its own JSON.
struct JevRequestSize {
	idx_t fixed_bytes;
	idx_t per_row_bytes;
};
JevRequestSize JevMeasureRequest(const JevConfig &config, const JevQuestionSet &set);

//! Conservative token estimate for `bytes` of request JSON (English runs ~4 bytes a token;
//! this assumes 3, so a batch closes early rather than late).
idx_t JevEstimateTokens(idx_t bytes);

} // namespace duckdb
