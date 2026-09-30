#pragma once

#include "duckdb.hpp"
#include "jev_config.hpp"

namespace duckdb {

//! Extension version, reported by jev_version() and sent as the User-Agent.
#define JEV_VERSION "0.1.0"

//! One question, asked of every row in a batch.
struct JevQuestion {
	//! Its key in the rubric and in the answers: "q" for the single-question functions, the
	//! caller's name in jev_ask, "intent" / "malicious" for snx_prompt_intent. A meaningful
	//! name is part of what the model reads ("Using rubric.malicious ...").
	string name = "q";
	//! Per-item instructions, with {rubric} and {item} filled in; empty for the default
	//! wording of the kind.
	string pointer;
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

//! What one batch came back with.
struct JevCallResult {
	//! answers[row][question]: the raw answer JSON, in the order given
	vector<vector<string>> answers;
	uint64_t input_tokens = 0;
	uint64_t output_tokens = 0;
	uint64_t retries = 0;
	uint64_t rate_limited_ms = 0;
};

//! Judges one batch of rows: one request, one shared state, one question per (row, question).
//! Every attempt first waits for the process-wide rate limit. Retries the retryable failures
//! (429, 5xx, dropped connections) and throws otherwise.
JevCallResult JevCallAPI(const JevConfig &config, const JevQuestionSet &set, const vector<string> &rows_json);

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
