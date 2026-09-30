#include "jev_config.hpp"

#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <cstdlib>

namespace duckdb {

static constexpr const char *DEFAULT_API_URL = "https://api.typesafe.ai/v1/systemone";
//! Name of the variable the search service pins per connection (ClassicSearchEngineV2).
static constexpr const char *CACHE_SCOPE_VARIABLE = "scope_customer_id";

//! True for http(s)://localhost, 127.0.0.1 or [::1], with any port and path. Anything
//! that needs DNS or leaves the machine is not loopback.
static bool IsLoopbackURL(const string &url) {
	for (auto c : url) {
		// '@' is userinfo ("http://localhost:80@evil.example" goes to evil.example), '\\' is
		// normalised to '/' by some parsers, and whitespace or controls can split a request line.
		if (c == '@' || c == '\\' || static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) {
			return false;
		}
	}
	string rest;
	if (StringUtil::StartsWith(url, "http://")) {
		rest = url.substr(7);
	} else if (StringUtil::StartsWith(url, "https://")) {
		rest = url.substr(8);
	} else {
		return false;
	}
	string host;
	if (!rest.empty() && rest[0] == '[') {
		auto close = rest.find(']');
		if (close == string::npos) {
			return false;
		}
		host = rest.substr(0, close + 1);
		rest = rest.substr(close + 1);
	} else {
		auto end = rest.find_first_of(":/?#");
		host = rest.substr(0, end);
		rest = end == string::npos ? string() : rest.substr(end);
	}
	if (!rest.empty() && rest[0] == ':') {
		// The port is digits only, up to the path.
		auto port_end = rest.find('/');
		auto port = rest.substr(1, port_end == string::npos ? string::npos : port_end - 1);
		if (port.empty() || port.find_first_not_of("0123456789") != string::npos) {
			return false;
		}
		rest = port_end == string::npos ? string() : rest.substr(port_end);
	}
	if (!rest.empty() && rest[0] != '/') {
		return false;
	}
	host = StringUtil::Lower(host);
	return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

static JevEndpoint ResolveEndpoint() {
	JevEndpoint endpoint;
	auto from_env = std::getenv("SNX_JEV_API_URL");
	string requested = from_env ? string(from_env) : string();
	if (requested.empty() || requested == DEFAULT_API_URL) {
		endpoint.url = DEFAULT_API_URL;
		return endpoint;
	}
	if (IsLoopbackURL(requested)) {
		endpoint.url = requested;
		return endpoint;
	}
	endpoint.refusal = "snx_jev: refusing SNX_JEV_API_URL '" + requested + "'. Requests go to " + DEFAULT_API_URL +
	                   ", or to a loopback address for tests.";
	return endpoint;
}

const JevEndpoint &JevEndpoint::Get() {
	static const JevEndpoint endpoint = ResolveEndpoint();
	return endpoint;
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
	    Value::UBIGINT(20));
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

	auto key = std::getenv("TYPESAFE_API_KEY");
	config.api_key = key ? string(key) : string();
	config.api_url = JevEndpoint::Get().url;
	if (ClientConfig::GetConfig(context).GetUserVariable(CACHE_SCOPE_VARIABLE, value) && !value.IsNull()) {
		config.cache_scope = value.ToString();
	}

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
	return config;
}

void JevConfig::RequireSendable() const {
	auto &endpoint = JevEndpoint::Get();
	if (endpoint.url.empty()) {
		throw InvalidInputException(endpoint.refusal);
	}
	if (api_key.empty()) {
		throw InvalidInputException("snx_jev: no API key. Start DuckDB with TYPESAFE_API_KEY set in the environment.");
	}
}

} // namespace duckdb
