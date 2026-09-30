#include "jev_config.hpp"

#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <cstdlib>

namespace duckdb {

static constexpr const char *DEFAULT_API_URL = "https://api.typesafe.ai/v1/systemone";
//! Name of the variable the search service pins per connection (ClassicSearchEngineV2).
static constexpr const char *CACHE_SCOPE_VARIABLE = "scope_customer_id";

//! A URL taken apart once. Everything downstream (the loopback check and the HTTP client)
//! uses these fields, never the original string, so no second parser can read the URL
//! differently from the one that approved it.
struct ParsedURL {
	string scheme;
	//! Lower-case, without brackets for IPv6
	string host;
	int port = 0;
	string path;
};

//! Strict on purpose: only http(s), a hostname or bracketed IPv6 literal, an optional port
//! in 1..65535 and a plain path. No userinfo, query, fragment, backslash, percent-escape,
//! whitespace or control character: none is needed to reach an API endpoint, and each is
//! a way for two URL parsers to disagree about the host.
static bool ParseURL(const string &url, ParsedURL &out, string &reason) {
	string rest;
	if (StringUtil::StartsWith(url, "http://")) {
		out.scheme = "http";
		rest = url.substr(7);
	} else if (StringUtil::StartsWith(url, "https://")) {
		out.scheme = "https";
		rest = url.substr(8);
	} else {
		reason = "not an http:// or https:// URL";
		return false;
	}
	auto authority_end = rest.find('/');
	auto authority = rest.substr(0, authority_end);
	out.path = authority_end == string::npos ? string("/") : rest.substr(authority_end);

	string port;
	if (!authority.empty() && authority[0] == '[') {
		auto close = authority.find(']');
		if (close == string::npos) {
			reason = "malformed IPv6 host";
			return false;
		}
		out.host = authority.substr(1, close - 1);
		if (out.host.empty() || out.host.find_first_not_of("0123456789abcdefABCDEF:") != string::npos) {
			reason = "malformed IPv6 host";
			return false;
		}
		auto after = authority.substr(close + 1);
		if (!after.empty()) {
			if (after[0] != ':') {
				reason = "unexpected text after the host";
				return false;
			}
			port = after.substr(1);
		}
	} else {
		auto colon = authority.find(':');
		out.host = authority.substr(0, colon);
		if (colon != string::npos) {
			port = authority.substr(colon + 1);
		}
		if (out.host.empty() ||
		    out.host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") !=
		        string::npos) {
			reason = "invalid host";
			return false;
		}
	}
	out.host = StringUtil::Lower(out.host);

	if (port.empty()) {
		if (authority.find(':') != string::npos && authority.back() == ':') {
			reason = "invalid port";
			return false;
		}
		out.port = out.scheme == "https" ? 443 : 80;
	} else {
		if (port.size() > 5 || port.find_first_not_of("0123456789") != string::npos) {
			reason = "invalid port";
			return false;
		}
		out.port = std::stoi(port);
		if (out.port < 1 || out.port > 65535) {
			reason = "invalid port";
			return false;
		}
	}

	if (out.path.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~/") !=
	    string::npos) {
		reason = "invalid path";
		return false;
	}
	return true;
}

static bool IsLoopbackHost(const string &host) {
	return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

static JevEnvironment ResolveEnvironment() {
	JevEnvironment environment;

	auto key = std::getenv("TYPESAFE_API_KEY");
	environment.api_key = key ? string(key) : string();
	auto generic = std::getenv("SNX_JEV_ENABLE_GENERIC");
	environment.enable_generic = generic && string(generic) == "1";

	auto from_env = std::getenv("SNX_JEV_API_URL");
	string requested = from_env ? string(from_env) : string();
	bool overridden = !requested.empty() && requested != DEFAULT_API_URL;

	ParsedURL url;
	string reason;
	if (!ParseURL(overridden ? requested : string(DEFAULT_API_URL), url, reason)) {
		// The value itself is never repeated: it would reach every caller whose query touched
		// jev(), and a proxy URL can carry credentials.
		environment.refusal = "snx_jev: refusing SNX_JEV_API_URL (" + reason + "). Requests go to " +
		                      string(DEFAULT_API_URL) + ", or to a loopback address for tests.";
		return environment;
	}
	if (overridden && !IsLoopbackHost(url.host)) {
		environment.refusal = "snx_jev: refusing SNX_JEV_API_URL (not a loopback host). Requests go to " +
		                      string(DEFAULT_API_URL) + ", or to a loopback address for tests.";
		return environment;
	}
	auto host = url.host.find(':') != string::npos ? "[" + url.host + "]" : url.host;
	// Canonical and fully explicit, so the HTTP library's own parser has nothing to interpret.
	environment.origin = url.scheme + "://" + host + ":" + to_string(url.port);
	environment.path = url.path;
	return environment;
}

const JevEnvironment &JevEnvironment::Get() {
	static const JevEnvironment environment = ResolveEnvironment();
	return environment;
}

void JevConfig::RegisterSettings(DBConfig &config) {
	auto add = [&](const char *name, const char *description, LogicalType type, Value default_value) {
		if (config.HasExtensionOption(name)) {
			return;
		}
		config.AddExtensionOption(name, description, std::move(type), std::move(default_value));
	};

	add("snx_jev_model", "Model name, or a pinned version such as jev-1.13.0", LogicalType::VARCHAR,
	    Value("jev-latest"));
	add("snx_jev_threshold", "Probability at which jev() returns true", LogicalType::DOUBLE, Value::DOUBLE(0.5));
	add("snx_jev_batch_size", "Rows per API request. Accuracy drops measurably above ~20-25", LogicalType::UBIGINT,
	    Value::UBIGINT(25));
	add("snx_jev_concurrency", "Requests in flight at once, across all DuckDB threads", LogicalType::UBIGINT,
	    Value::UBIGINT(16));
	add("snx_jev_timeout", "Seconds a single API request may take", LogicalType::DOUBLE, Value::DOUBLE(30.0));
	add("snx_jev_max_retries", "Attempts for a retryable failure (429, 5xx, dropped connection)", LogicalType::UBIGINT,
	    Value::UBIGINT(6));
	add("snx_jev_max_rows_per_statement", "Refuse to send more rows than this per statement. 0 = no limit",
	    LogicalType::UBIGINT, Value::UBIGINT(0));
	add("snx_jev_max_chars_per_statement",
	    "Refuse to send more characters of row data than this per statement. 0 = no limit", LogicalType::UBIGINT,
	    Value::UBIGINT(0));
	add("snx_jev_cache_max_entries", "Answers kept in the process cache before the oldest are dropped",
	    LogicalType::UBIGINT, Value::UBIGINT(200000));
	add("snx_jev_max_value_chars", "Characters of each string in a row that are sent; the rest is cut. 0 = no limit",
	    LogicalType::UBIGINT, Value::UBIGINT(2000));
	add("snx_jev_max_requests_per_minute",
	    "Requests a minute across the whole process, below Jev's 1,200; requests over it wait. 0 = no limit",
	    LogicalType::UBIGINT, Value::UBIGINT(1000));
	add("snx_jev_max_batch_tokens",
	    "Estimated input tokens at which a batch closes, even below snx_jev_batch_size (Jev allows 32k of state)",
	    LogicalType::UBIGINT, Value::UBIGINT(24000));
}

static bool TryGet(ClientContext &context, const char *name, Value &result) {
	if (!context.TryGetCurrentSetting(name, result)) {
		return false;
	}
	return !result.IsNull();
}

JevConfig JevConfig::FromContext(ClientContext &context) {
	JevConfig config;
	Value value;

	config.api_key = JevEnvironment::Get().api_key;

	if (TryGet(context, "snx_jev_model", value) && !value.ToString().empty()) {
		config.model = value.ToString();
	}
	if (TryGet(context, "snx_jev_threshold", value)) {
		config.threshold = value.GetValue<double>();
	}
	if (TryGet(context, "snx_jev_batch_size", value)) {
		config.batch_size = MaxValue<idx_t>(1, value.GetValue<uint64_t>());
	}
	if (TryGet(context, "snx_jev_concurrency", value)) {
		config.concurrency = MaxValue<idx_t>(1, value.GetValue<uint64_t>());
	}
	if (TryGet(context, "snx_jev_timeout", value)) {
		config.timeout = MaxValue<double>(0.1, value.GetValue<double>());
	}
	if (TryGet(context, "snx_jev_max_retries", value)) {
		config.max_retries = MaxValue<idx_t>(1, value.GetValue<uint64_t>());
	}
	if (TryGet(context, "snx_jev_max_rows_per_statement", value)) {
		config.max_rows_per_statement = value.GetValue<uint64_t>();
	}
	if (TryGet(context, "snx_jev_max_chars_per_statement", value)) {
		config.max_chars_per_statement = value.GetValue<uint64_t>();
	}
	if (TryGet(context, "snx_jev_cache_max_entries", value)) {
		config.cache_max_entries = value.GetValue<uint64_t>();
	}
	if (TryGet(context, "snx_jev_max_value_chars", value)) {
		config.max_value_chars = value.GetValue<uint64_t>();
	}
	if (TryGet(context, "snx_jev_max_requests_per_minute", value)) {
		config.max_requests_per_minute = value.GetValue<uint64_t>();
	}
	if (TryGet(context, "snx_jev_max_batch_tokens", value)) {
		config.max_batch_tokens = MaxValue<idx_t>(1, value.GetValue<uint64_t>());
	}
	return config;
}

string JevConfig::CacheScope(ClientContext &context) {
	Value value;
	if (ClientConfig::GetConfig(context).GetUserVariable(CACHE_SCOPE_VARIABLE, value) && !value.IsNull()) {
		return value.ToString();
	}
	return string();
}

void JevConfig::RequireSendable() const {
	auto &environment = JevEnvironment::Get();
	if (environment.origin.empty()) {
		throw InvalidInputException(environment.refusal);
	}
	if (api_key.empty()) {
		throw InvalidInputException("snx_jev: no API key. Start DuckDB with TYPESAFE_API_KEY set in the environment.");
	}
}

} // namespace duckdb
