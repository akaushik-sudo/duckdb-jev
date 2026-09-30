#include "jev_client.hpp"

#include "jev_json.hpp"
#include "jev_state.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "jev_httplib.hpp"
#include "json.hpp"

#include <chrono>
#include <random>
#include <thread>
#include <unordered_map>

namespace duckdb {

namespace http = duckdb_jev_httplib;
// ordered_json keeps the answer's key order, so a rubric's "10" does not sort before its "2".
using nlohmann::ordered_json;

string JevQuestion::CacheKey() const {
	string key = kind + "\x1f" + query;
	for (auto &option : options) {
		key += "\x1f";
		key += option;
	}
	return key;
}

//! One client per thread per origin. httplib keeps the connection alive between
//! requests on the same client, and a client is not safe to share between threads,
//! so this is both the connection pool and the thread-safety story.
static http::Client &GetClient(const string &origin, const JevConfig &config) {
	static thread_local std::unordered_map<string, unique_ptr<http::Client>> clients;
	auto entry = clients.find(origin);
	if (entry == clients.end()) {
		auto client = make_uniq<http::Client>(origin);
		client->set_keep_alive(true);
		client->set_follow_location(false);
		entry = clients.insert(make_pair(origin, std::move(client))).first;
	}
	auto seconds = static_cast<time_t>(config.timeout);
	auto micros = static_cast<time_t>((config.timeout - static_cast<double>(seconds)) * 1000000);
	entry->second->set_connection_timeout(seconds, micros);
	entry->second->set_read_timeout(seconds, micros);
	entry->second->set_write_timeout(seconds, micros);
	return *entry->second;
}

static void WriteQuestion(const JevQuestion &question, idx_t index, string &out) {
	auto row_ref = "rows[" + to_string(index) + "]";
	out += "{\"type\":";
	JevWriteJSONString(question.kind, out);
	out += ",\"instructions\":";
	if (question.kind == "noul") {
		JevWriteJSONString("Does the record `" + row_ref + "` satisfy the condition stated in `condition`?", out);
	} else if (question.kind == "score") {
		JevWriteJSONString("Rate the record `" + row_ref + "`: " + question.query, out);
		out += ",\"criteria\":[";
		for (idx_t i = 0; i < question.options.size(); i++) {
			if (i > 0) {
				out += ',';
			}
			JevWriteJSONString(question.options[i], out);
		}
		out += ']';
	} else {
		JevWriteJSONString("For the record `" + row_ref + "`: " + question.query, out);
		out += ",\"criteria\":{";
		for (idx_t i = 0; i < question.options.size(); i++) {
			if (i > 0) {
				out += ',';
			}
			JevWriteJSONString(question.options[i], out);
			out += ":null";
		}
		out += '}';
	}
	out += '}';
}

//! The request every batch sends: one state holding all the rows, and one question per
//! row that refers to its row by position. The model answers them over the one state,
//! which is what makes a batch cheaper than the same rows sent one at a time.
static string BuildRequestBody(const JevConfig &config, const JevQuestion &question, const vector<string> &rows_json) {
	string body = "{\"model\":";
	JevWriteJSONString(config.model, body);
	body += ",\"state\":{";
	if (question.kind == "noul") {
		body += "\"condition\":";
		JevWriteJSONString(question.query, body);
		body += ',';
	}
	body += "\"rows\":[";
	for (idx_t i = 0; i < rows_json.size(); i++) {
		if (i > 0) {
			body += ',';
		}
		body += rows_json[i];
	}
	body += "]},\"questions\":{";
	for (idx_t i = 0; i < rows_json.size(); i++) {
		if (i > 0) {
			body += ',';
		}
		JevWriteJSONString("r" + to_string(i), body);
		body += ':';
		WriteQuestion(question, i, body);
	}
	body += "}}";
	return body;
}

static bool IsRetryable(int status) {
	return status == 408 || status == 429 || status == 529 || status >= 500;
}

//! Retry-after-ms wins over retry-after, as the API sends the precise one when it has it.
static double RetryAfterSeconds(const http::Result &response) {
	auto millis = response->get_header_value("retry-after-ms");
	if (!millis.empty()) {
		try {
			return std::stod(millis) / 1000.0;
		} catch (std::exception &) { // NOLINT: a malformed header is simply ignored
		}
	}
	auto seconds = response->get_header_value("retry-after");
	if (!seconds.empty()) {
		try {
			return std::stod(seconds);
		} catch (std::exception &) { // NOLINT
		}
	}
	return -1;
}

static void Sleep(double seconds) {
	std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000)));
}

static double Jitter() {
	static thread_local std::mt19937 generator(std::random_device {}());
	std::uniform_real_distribution<double> distribution(0.0, 0.25);
	return distribution(generator);
}

vector<string> JevCallAPI(const JevConfig &config, const JevQuestion &question, const vector<string> &rows_json) {
	config.RequireSendable();

	// RequireSendable() has checked it was not refused; origin is canonical scheme://host:port.
	auto &environment = JevEnvironment::Get();
	auto &origin = environment.origin;
	auto &path = environment.path;
	auto body = BuildRequestBody(config, question, rows_json);
	http::Headers headers = {{"Authorization", "Bearer " + config.api_key}, {"User-Agent", "snx-jev/" JEV_VERSION}};

	auto &stats = JevState::Get().stats;
	string last_error = "no attempt was made";
	double delay = 0.5;

	for (idx_t attempt = 0; attempt < config.max_retries; attempt++) {
		auto &client = GetClient(origin, config);
		auto started = std::chrono::steady_clock::now();
		auto response = client.Post(path, headers, body, "application/json");
		auto elapsed =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

		if (!response) {
			// Connection refused, TLS failure, timeout, or a pooled connection the peer
			// had already closed. The first of those on a reused connection is worth an
			// immediate retry; the rest back off.
			last_error = http::to_string(response.error());
			stats.retries++;
			if (attempt + 1 < config.max_retries) {
				Sleep(attempt == 0 ? 0.0 : delay + Jitter());
				delay = MinValue<double>(delay * 2, 8);
			}
			continue;
		}
		if (response->status != 200) {
			last_error = to_string(response->status) + " " + response->body.substr(0, 300);
			if (!IsRetryable(response->status)) {
				stats.errors++;
				throw IOException("jev: API error " + last_error);
			}
			stats.retries++;
			if (attempt + 1 < config.max_retries) {
				auto retry_after = RetryAfterSeconds(response);
				Sleep(MinValue<double>(retry_after >= 0 ? retry_after : delay, 30) + Jitter());
				delay = MinValue<double>(delay * 2, 8);
			}
			continue;
		}

		ordered_json parsed;
		try {
			parsed = ordered_json::parse(response->body);
		} catch (std::exception &error) {
			stats.errors++;
			throw IOException("jev: could not parse the API response: %s", error.what());
		}
		auto answers = parsed.find("answers");
		if (answers == parsed.end() || !answers->is_object()) {
			stats.errors++;
			throw IOException("jev: the API response has no answers object: %s", response->body.substr(0, 300));
		}
		vector<string> result;
		result.reserve(rows_json.size());
		for (idx_t i = 0; i < rows_json.size(); i++) {
			auto answer = answers->find("r" + to_string(i));
			if (answer == answers->end()) {
				stats.errors++;
				throw IOException("jev: the API answered %llu of %llu rows in this batch",
				                  static_cast<uint64_t>(answers->size()), static_cast<uint64_t>(rows_json.size()));
			}
			result.push_back(answer->dump());
		}

		auto usage = parsed.find("usage");
		if (usage != parsed.end() && usage->is_object()) {
			stats.input_tokens += usage->value("input_tokens", 0);
			stats.output_tokens += usage->value("output_tokens", 0);
		}
		stats.requests++;
		stats.rows_evaluated += rows_json.size();
		stats.api_ms += static_cast<uint64_t>(elapsed.count());
		return result;
	}

	stats.errors++;
	throw IOException("jev: the API is unreachable after %llu attempts: %s", static_cast<uint64_t>(config.max_retries),
	                  last_error);
}

} // namespace duckdb
