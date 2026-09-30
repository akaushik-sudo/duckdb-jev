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
	// Descriptions change what is asked, so they are part of the key; a label list with no
	// descriptions keeps the key it had before descriptions existed.
	for (auto &description : descriptions) {
		key += "\x1e";
		key += description;
	}
	return key;
}

string JevQuestionSet::CacheKey() const {
	string key;
	for (auto &question : questions) {
		key += question.CacheKey();
		key += "\x1d";
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

static string RubricKey(idx_t question) {
	return "q" + to_string(question);
}

static string AnswerKey(idx_t row, idx_t question) {
	return "r" + to_string(row) + "_" + RubricKey(question);
}

//! One question in full, written into the state once per request: its text, and for a
//! choice every label with its description. Every row's question points here instead of
//! repeating it (the compact layout: about 70% fewer input tokens than repeating the full
//! question per row, at the same accuracy, measured against the live API).
static void WriteRubricEntry(const JevQuestion &question, string &out) {
	out += "{\"type\":";
	JevWriteJSONString(question.kind, out);
	if (question.kind == "noul") {
		out += ",\"condition\":";
		JevWriteJSONString(question.query, out);
		if (question.descriptions.size() == 2) {
			out += ",\"true_means\":";
			JevWriteJSONString(question.descriptions[0], out);
			out += ",\"false_means\":";
			JevWriteJSONString(question.descriptions[1], out);
		}
	} else if (question.kind == "score") {
		out += ",\"question\":";
		JevWriteJSONString(question.query, out);
		out += ",\"levels\":[";
		for (idx_t i = 0; i < question.options.size(); i++) {
			if (i > 0) {
				out += ',';
			}
			JevWriteJSONString(question.options[i], out);
		}
		out += ']';
	} else {
		out += ",\"question\":";
		JevWriteJSONString(question.query, out);
		out += ",\"options\":{";
		for (idx_t i = 0; i < question.options.size(); i++) {
			if (i > 0) {
				out += ',';
			}
			JevWriteJSONString(question.options[i], out);
			out += ':';
			if (i < question.descriptions.size() && !question.descriptions[i].empty()) {
				JevWriteJSONString(question.descriptions[i], out);
			} else {
				out += "null";
			}
		}
		out += '}';
	}
	out += '}';
}

//! The per-row question: a pointer to the rubric entry and to the row, plus the bare
//! criteria the answer has to use.
static void WriteRowQuestion(const JevQuestion &question, idx_t question_index, idx_t row, string &out) {
	auto rubric = "`rubric." + RubricKey(question_index) + "`";
	auto row_ref = "`rows[" + to_string(row) + "]`";
	out += "{\"type\":";
	JevWriteJSONString(question.kind, out);
	out += ",\"instructions\":";
	if (question.kind == "noul") {
		JevWriteJSONString("Does the record " + row_ref + " satisfy the condition in " + rubric + "?", out);
	} else if (question.kind == "score") {
		JevWriteJSONString("Answer " + rubric + " for the record " + row_ref + ".", out);
		out += ",\"criteria\":[";
		for (idx_t i = 0; i < question.options.size(); i++) {
			if (i > 0) {
				out += ',';
			}
			JevWriteJSONString(question.options[i], out);
		}
		out += ']';
	} else {
		JevWriteJSONString("Answer " + rubric + " for the record " + row_ref + ", using its options.", out);
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

//! The request every batch sends: one state holding the rubric and all the rows, and one
//! question per (row, question) that refers to both by key. The model answers them over the
//! one state, which is what makes a batch cheaper than the same rows sent one at a time,
//! and what lets a second question on the same row ride in the same request.
static string BuildRequestBody(const JevConfig &config, const JevQuestionSet &set, const vector<string> &rows_json) {
	string body = "{\"model\":";
	JevWriteJSONString(config.model, body);
	body += ",\"state\":{\"rubric\":{";
	for (idx_t q = 0; q < set.questions.size(); q++) {
		if (q > 0) {
			body += ',';
		}
		JevWriteJSONString(RubricKey(q), body);
		body += ':';
		WriteRubricEntry(set.questions[q], body);
	}
	body += "},\"rows\":[";
	for (idx_t i = 0; i < rows_json.size(); i++) {
		if (i > 0) {
			body += ',';
		}
		body += rows_json[i];
	}
	body += "]},\"questions\":{";
	bool first = true;
	for (idx_t i = 0; i < rows_json.size(); i++) {
		for (idx_t q = 0; q < set.questions.size(); q++) {
			if (!first) {
				body += ',';
			}
			first = false;
			JevWriteJSONString(AnswerKey(i, q), body);
			body += ':';
			WriteRowQuestion(set.questions[q], q, i, body);
		}
	}
	body += "}}";
	return body;
}

JevRequestSize JevMeasureRequest(const JevConfig &config, const JevQuestionSet &set) {
	// Measured on real request bodies rather than estimated from the pieces, so the numbers
	// follow the layout if it changes. A placeholder row "0" stands in for the row JSON;
	// the index digits of a large batch add a few bytes more, which the token estimate's
	// margin covers.
	auto empty = BuildRequestBody(config, set, {}).size();
	auto one = BuildRequestBody(config, set, {"0"}).size();
	return JevRequestSize {empty, one - empty - 1};
}

idx_t JevEstimateTokens(idx_t bytes) {
	return (bytes + 2) / 3;
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

vector<vector<string>> JevCallAPI(const JevConfig &config, const JevQuestionSet &set, const vector<string> &rows_json) {
	config.RequireSendable();

	// RequireSendable() has checked it was not refused; origin is canonical scheme://host:port.
	auto &environment = JevEnvironment::Get();
	auto &origin = environment.origin;
	auto &path = environment.path;
	auto body = BuildRequestBody(config, set, rows_json);
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
		vector<vector<string>> result(rows_json.size());
		for (idx_t i = 0; i < rows_json.size(); i++) {
			result[i].reserve(set.questions.size());
			for (idx_t q = 0; q < set.questions.size(); q++) {
				auto answer = answers->find(AnswerKey(i, q));
				if (answer == answers->end()) {
					stats.errors++;
					throw IOException("jev: the API answered %llu of %llu questions in this batch",
					                  static_cast<uint64_t>(answers->size()),
					                  static_cast<uint64_t>(rows_json.size() * set.questions.size()));
				}
				result[i].push_back(answer->dump());
			}
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
