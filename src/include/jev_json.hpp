#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Serialises a DuckDB value to JSON the way `to_json` would: struct field names and
//! their declared order are preserved, because the column names are part of what the
//! model reads. Used for the row payload and for the cache key.
//!
//! Every string in the value is cut to its first `max_chars` characters (0 = no limit):
//! intent is almost always evident early, and a bounded row keeps a batch inside the
//! API's context window. The cut happens before the cache key is taken, so two prompts
//! that differ only past the limit share an answer, as they would share a request.
string JevValueToJSON(const Value &value, idx_t max_chars = 0);

//! Appends `text` to `out` as a quoted JSON string.
void JevWriteJSONString(const string &text, string &out);

//! `text` as a quoted JSON string.
string JevQuoteJSONString(const string &text);

} // namespace duckdb
