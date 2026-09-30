#include "jev_functions.hpp"

#include "jev_client.hpp"
#include "jev_config.hpp"
#include "jev_json.hpp"
#include "jev_state.hpp"

#include "duckdb/common/types/hash.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "json.hpp"

#include <unordered_map>
#include <chrono>
#include <unordered_set>

namespace duckdb {

using nlohmann::ordered_json;

//! Which field of the answer the caller wants.
enum class JevResult : uint8_t {
	PREDICATE,
	PROBABILITY,
	SCORE,
	SCORE_NORM,
	CHOICE,
	CONFIDENCE,
	EVAL,
	ASK,
	PROMPT_INTENT
};
//! Where the question kind comes from: the function itself, or one of its arguments.
enum class JevKindSource : uint8_t { NOUL, SCORE, CHOICE, FROM_ARGUMENT };

static constexpr idx_t NO_ARG = DConstants::INVALID_INDEX;

struct JevBindData : public FunctionData {
	JevConfig config;
	JevResult result = JevResult::PREDICATE;
	JevKindSource kind_source = JevKindSource::NOUL;
	idx_t kind_arg = NO_ARG;
	idx_t options_arg = NO_ARG;
	idx_t threshold_arg = NO_ARG;
	//! What this call has already sent in this statement, for the spend guards.
	shared_ptr<std::atomic<uint64_t>> rows_sent = make_shared_ptr<std::atomic<uint64_t>>(0);
	shared_ptr<std::atomic<uint64_t>> chars_sent = make_shared_ptr<std::atomic<uint64_t>>(0);

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<JevBindData>();
		copy->config = config;
		copy->result = result;
		copy->kind_source = kind_source;
		copy->kind_arg = kind_arg;
		copy->options_arg = options_arg;
		copy->threshold_arg = threshold_arg;
		// The copy is the same call in the same statement, so it shares the spend counters.
		copy->rows_sent = rows_sent;
		copy->chars_sent = chars_sent;
		return std::move(copy);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<JevBindData>();
		return result == other.result && kind_source == other.kind_source && kind_arg == other.kind_arg &&
		       options_arg == other.options_arg && threshold_arg == other.threshold_arg;
	}
};

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//

template <JevResult RESULT, JevKindSource KIND, idx_t KIND_ARG, idx_t OPTIONS_ARG, idx_t THRESHOLD_ARG>
static unique_ptr<FunctionData> JevBind(ClientContext &context, ScalarFunction &bound_function,
                                        vector<unique_ptr<Expression>> &arguments) {
	auto data = make_uniq<JevBindData>();
	// Settings are read here, so they are fixed for the whole statement.
	data->config = JevConfig::FromContext(context);
	data->result = RESULT;
	data->kind_source = KIND;
	data->kind_arg = KIND_ARG;
	data->options_arg = OPTIONS_ARG;
	data->threshold_arg = THRESHOLD_ARG;
	if (arguments[0]->return_type.id() == LogicalTypeId::SQLNULL) {
		// `jev(NULL, ...)`: nothing to judge, but the plan still needs a type.
		bound_function.arguments[0] = LogicalType::SQLNULL;
	}
	if (OPTIONS_ARG != NO_ARG && bound_function.arguments[OPTIONS_ARG].id() == LogicalTypeId::ANY) {
		// One signature for both forms, so an untyped NULL is not ambiguous between them:
		// a list of labels, or a MAP of label -> description.
		auto &type = arguments[OPTIONS_ARG]->return_type;
		if (type.id() == LogicalTypeId::UNKNOWN || type.id() == LogicalTypeId::VARCHAR ||
		    type.id() == LogicalTypeId::STRING_LITERAL) {
			// A prepared-statement parameter, or a string such as '[a, b]': a list of labels, as
			// before options took a MAP too. DuckDB casts to it.
			bound_function.arguments[OPTIONS_ARG] = LogicalType::LIST(LogicalType::VARCHAR);
			return std::move(data);
		}
		if (type.id() != LogicalTypeId::LIST && type.id() != LogicalTypeId::MAP &&
		    type.id() != LogicalTypeId::SQLNULL) {
			throw BinderException("%s: the options are a list of labels or a MAP of label -> description, not %s",
			                      bound_function.name, type.ToString());
		}
		bound_function.arguments[OPTIONS_ARG] = type;
	}
	return std::move(data);
}

//===--------------------------------------------------------------------===//
// Execute
//===--------------------------------------------------------------------===//

//! Rows judged together: one question set, the distinct row payloads under it, and where
//! each row's answers have to be written back to.
struct JevGroup {
	JevQuestionSet set;
	//! set.CacheKey() and its hash, computed once when the group is made, not per row
	string set_key;
	string set_key_hash;
	//! jev_ask only: the name each question's answer goes under, parallel to set.questions
	vector<string> names;
	vector<string> rows;
	vector<string> cache_keys;
	vector<vector<idx_t>> targets;
	//! answers[slot][question]; empty for a slot whose batch failed
	vector<vector<string>> answers;
	std::unordered_map<string, idx_t> row_index;
};

//! One API request: a slice of a group.
struct JevBatch {
	JevGroup *group;
	idx_t start;
	idx_t count;
};

//! Keyed by content, not by row id: an UPDATE changes the payload and the row is
//! judged again, while two identical rows are judged once.
//!
//! The scope (the connection's customer, see JevConfig::CacheScope) comes first and
//! verbatim, so one customer's answers are never a cache hit for another's: a hit would
//! show, through its speed or through jev_stats(), that someone else sent the same text.
//! The model is part of the key too: an answer from one model is not an answer from another.
static string CacheEntryKey(const string &scope, const string &model, const JevGroup &group, const string &row_json) {
	return scope + "\x1f" + model + "\x1f" + group.set_key_hash + ":" +
	       to_string(Hash(row_json.c_str(), row_json.size())) + ":" + to_string(row_json.size());
}

static unique_ptr<JevGroup> MakeGroup(JevQuestionSet set, vector<string> names, string set_key) {
	auto group = make_uniq<JevGroup>();
	group->set = std::move(set);
	group->names = std::move(names);
	group->set_key = std::move(set_key);
	group->set_key_hash = to_string(Hash(group->set_key.c_str(), group->set_key.size()));
	return group;
}

//! A row's answers, one per question, as one cache value (a JSON array).
static string EncodeAnswers(const vector<string> &answers) {
	string out = "[";
	for (idx_t i = 0; i < answers.size(); i++) {
		if (i > 0) {
			out += ',';
		}
		out += answers[i];
	}
	out += ']';
	return out;
}

static vector<string> DecodeAnswers(const string &encoded) {
	vector<string> answers;
	for (auto &answer : ordered_json::parse(encoded)) {
		answers.push_back(answer.dump());
	}
	return answers;
}

//! Returns false when the kind argument is NULL, which makes the answer NULL - the
//! way every other DuckDB function treats a NULL argument.
static bool ResolveKind(const JevBindData &data, DataChunk &args, idx_t row, string &kind) {
	if (data.kind_source == JevKindSource::NOUL) {
		kind = "noul";
		return true;
	}
	if (data.kind_source == JevKindSource::SCORE) {
		kind = "score";
		return true;
	}
	if (data.kind_source == JevKindSource::CHOICE) {
		kind = "choice";
		return true;
	}
	auto value = args.data[data.kind_arg].GetValue(row);
	if (value.IsNull()) {
		return false;
	}
	kind = StringUtil::Lower(value.ToString());
	if (kind != "noul" && kind != "score" && kind != "choice") {
		throw InvalidInputException("jev: unknown question kind '%s'. Use 'noul', 'score' or 'choice'.", kind);
	}
	return true;
}

//! Returns false when the options argument is NULL: a NULL answer, not an error. An
//! empty list is an error, because it is a question nobody can answer.
//!
//! Options are either a list of labels, or a MAP of label -> description; the
//! description is what separates close labels, and it is sent with the question.
static bool ResolveOptions(const JevBindData &data, DataChunk &args, idx_t row, JevQuestion &question) {
	question.options.clear();
	question.descriptions.clear();
	auto &kind = question.kind;
	if (kind == "noul") {
		return true;
	}
	if (data.options_arg == NO_ARG) {
		throw InvalidInputException("jev: a '%s' question needs its options; call jev_eval(row, question, '%s', "
		                            "['...', '...']).",
		                            kind, kind);
	}
	auto value = args.data[data.options_arg].GetValue(row);
	if (value.IsNull()) {
		return false;
	}
	if (value.type().id() == LogicalTypeId::MAP) {
		if (kind != "choice") {
			throw InvalidInputException("jev: only a 'choice' question takes label descriptions; give a '%s' "
			                            "question a list of levels.",
			                            kind);
		}
		for (auto &entry : MapValue::GetChildren(value)) {
			auto &pair = StructValue::GetChildren(entry);
			if (pair[0].IsNull()) {
				throw InvalidInputException("jev: the options of a '%s' question cannot contain NULL.", kind);
			}
			question.options.push_back(pair[0].ToString());
			question.descriptions.push_back(pair[1].IsNull() ? string() : pair[1].ToString());
		}
	} else {
		for (auto &child : ListValue::GetChildren(value)) {
			if (child.IsNull()) {
				throw InvalidInputException("jev: the options of a '%s' question cannot contain NULL.", kind);
			}
			question.options.push_back(child.ToString());
		}
	}
	if (question.options.empty()) {
		throw InvalidInputException("jev: a '%s' question needs at least one option.", kind);
	}
	return true;
}

//! Limits the API sets on a question (and a sanity bound on how many ride in one request).
static constexpr idx_t MAX_ASK_QUESTIONS = 16;
static constexpr idx_t MAX_CHOICE_OPTIONS = 255;
static constexpr idx_t MIN_SCORE_LEVELS = 2;
static constexpr idx_t MAX_SCORE_LEVELS = 10;

static string RequireText(const ordered_json &object, const char *field, const string &name) {
	auto entry = object.find(field);
	if (entry == object.end() || !entry->is_string() || entry->get<string>().empty()) {
		throw InvalidInputException("jev_ask: question '%s' needs a non-empty string \"%s\".", name, field);
	}
	return entry->get<string>();
}

//! The questions argument of jev_ask, e.g.
//!   {"intent":    {"type": "choice", "question": "...", "options": {"work": "...", "personal": "..."}},
//!    "malicious": {"type": "noul",   "question": "...", "criteria": {"true": "...", "false": "..."}},
//!    "urgency":   {"type": "score",  "question": "...", "levels": ["low", "medium", "high"]}}
//! Strict: unknown fields are errors, so a typo cannot silently drop a description.
static void ParseQuestionSet(const string &text, JevQuestionSet &set, vector<string> &names) {
	ordered_json parsed;
	try {
		parsed = ordered_json::parse(text);
	} catch (std::exception &error) {
		throw InvalidInputException("jev_ask: the questions are not valid JSON: %s", error.what());
	}
	if (!parsed.is_object() || parsed.empty()) {
		throw InvalidInputException("jev_ask: the questions must be a non-empty JSON object of name -> question.");
	}
	if (parsed.size() > MAX_ASK_QUESTIONS) {
		throw InvalidInputException("jev_ask: at most %llu questions per call.",
		                            static_cast<uint64_t>(MAX_ASK_QUESTIONS));
	}
	for (auto &item : parsed.items()) {
		auto &name = item.key();
		auto &spec = item.value();
		// The name is pasted into what the model reads ("Using rubric.<name> ..."), so it must be
		// a plain identifier: a dot would read as a nested path, a space or a quote would break it.
		if (name.empty() || name.size() > 64 ||
		    name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != string::npos) {
			throw InvalidInputException("jev_ask: question name '%s' must be 1-64 letters, digits or underscores.",
			                            name);
		}
		if (!spec.is_object()) {
			throw InvalidInputException("jev_ask: question '%s' must be an object.", name);
		}
		JevQuestion question;
		question.name = name;
		// Case-insensitive, like jev_eval's kind argument.
		question.kind = StringUtil::Lower(RequireText(spec, "type", name));
		if (question.kind != "noul" && question.kind != "score" && question.kind != "choice") {
			throw InvalidInputException(
			    "jev_ask: question '%s' has unknown type '%s'. Use 'noul', 'score' or 'choice'.", name, question.kind);
		}
		question.query = RequireText(spec, "question", name);

		for (auto &field : spec.items()) {
			auto &key = field.key();
			bool known = key == "type" || key == "question" || (question.kind == "choice" && key == "options") ||
			             (question.kind == "score" && key == "levels") ||
			             (question.kind == "noul" && key == "criteria");
			if (!known) {
				throw InvalidInputException("jev_ask: question '%s' (%s) has an unexpected field \"%s\".", name,
				                            question.kind, key);
			}
		}

		if (question.kind == "choice") {
			auto options = spec.find("options");
			if (options == spec.end()) {
				throw InvalidInputException("jev_ask: choice question '%s' needs \"options\".", name);
			}
			if (options->is_array()) {
				for (auto &label : *options) {
					if (!label.is_string() || label.get<string>().empty()) {
						throw InvalidInputException("jev_ask: the options of '%s' must be non-empty strings.", name);
					}
					question.options.push_back(label.get<string>());
				}
			} else if (options->is_object()) {
				for (auto &label : options->items()) {
					if (label.key().empty() || !(label.value().is_string() || label.value().is_null())) {
						throw InvalidInputException(
						    "jev_ask: the options of '%s' map non-empty labels to a description string or null.", name);
					}
					question.options.push_back(label.key());
					question.descriptions.push_back(label.value().is_string() ? label.value().get<string>() : string());
				}
			} else {
				throw InvalidInputException(
				    "jev_ask: the options of '%s' are a list of labels or an object of label -> description.", name);
			}
			if (question.options.empty() || question.options.size() > MAX_CHOICE_OPTIONS) {
				throw InvalidInputException("jev_ask: choice question '%s' needs 1 to %llu options.", name,
				                            static_cast<uint64_t>(MAX_CHOICE_OPTIONS));
			}
			for (idx_t i = 0; i < question.options.size(); i++) {
				for (idx_t j = 0; j < i; j++) {
					if (question.options[i] == question.options[j]) {
						throw InvalidInputException("jev_ask: choice question '%s' repeats the option '%s'.", name,
						                            question.options[i]);
					}
				}
			}
		} else if (question.kind == "score") {
			auto levels = spec.find("levels");
			if (levels == spec.end() || !levels->is_array()) {
				throw InvalidInputException("jev_ask: score question '%s' needs \"levels\", a list.", name);
			}
			for (auto &level : *levels) {
				if (!level.is_string() || level.get<string>().empty()) {
					throw InvalidInputException("jev_ask: the levels of '%s' must be non-empty strings.", name);
				}
				question.options.push_back(level.get<string>());
			}
			if (question.options.size() < MIN_SCORE_LEVELS || question.options.size() > MAX_SCORE_LEVELS) {
				throw InvalidInputException("jev_ask: score question '%s' needs %llu to %llu levels.", name,
				                            static_cast<uint64_t>(MIN_SCORE_LEVELS),
				                            static_cast<uint64_t>(MAX_SCORE_LEVELS));
			}
		} else if (question.kind == "noul") {
			auto criteria = spec.find("criteria");
			if (criteria != spec.end()) {
				auto yes = criteria->is_object() ? criteria->find("true") : criteria->end();
				auto no = criteria->is_object() ? criteria->find("false") : criteria->end();
				if (!criteria->is_object() || criteria->size() != 2 || yes == criteria->end() ||
				    no == criteria->end() || !yes->is_string() || !no->is_string()) {
					throw InvalidInputException(
					    "jev_ask: the criteria of noul question '%s' are {\"true\": \"...\", \"false\": \"...\"}.",
					    name);
				}
				question.descriptions = {yes->get<string>(), no->get<string>()};
			}
		} else {
			throw InvalidInputException(
			    "jev_ask: question '%s' has unknown type '%s'. Use 'noul', 'score' or 'choice'.", name, question.kind);
		}
		names.push_back(name);
		set.questions.push_back(std::move(question));
	}
}

//! Refuses to send anything once this statement has spent its budget.
static void CheckSpendGuards(const JevBindData &data, idx_t rows, idx_t chars) {
	auto total_rows = data.rows_sent->fetch_add(rows) + rows;
	auto total_chars = data.chars_sent->fetch_add(chars) + chars;
	if (data.config.max_rows_per_statement > 0 && total_rows > data.config.max_rows_per_statement) {
		throw InvalidInputException("jev: this statement would send %llu rows to the API, above "
		                            "snx_jev_max_rows_per_statement = %llu",
		                            static_cast<uint64_t>(total_rows),
		                            static_cast<uint64_t>(data.config.max_rows_per_statement));
	}
	if (data.config.max_chars_per_statement > 0 && total_chars > data.config.max_chars_per_statement) {
		throw InvalidInputException("jev: this statement would send %llu characters of row data to the API, above "
		                            "snx_jev_max_chars_per_statement = %llu",
		                            static_cast<uint64_t>(total_chars),
		                            static_cast<uint64_t>(data.config.max_chars_per_statement));
	}
}

//! Runs every batch through the shared pool and waits for all of them. The first
//! failure is handed back rather than thrown, so the caller can still keep the answers
//! the other batches already paid for; it waits for all of them either way, because the
//! workers write into the groups.
static std::exception_ptr RunBatches(const JevBindData &data, vector<JevBatch> &batches, JevQueryState &query) {
	auto &state = JevState::Get();
	auto pool = state.Pool(data.config.concurrency);

	vector<std::future<void>> futures;
	futures.reserve(batches.size());
	for (auto &batch : batches) {
		auto *batch_ptr = &batch;
		auto *config = &data.config;
		futures.push_back(pool->Submit([batch_ptr, config, &state, &query]() {
			auto &group = *batch_ptr->group;
			vector<string> rows(group.rows.begin() + batch_ptr->start,
			                    group.rows.begin() + batch_ptr->start + batch_ptr->count);
			state.stats.in_flight++;
			try {
				auto call = JevCallAPI(*config, group.set, rows);
				query.current.requests++;
				query.current.input_tokens += call.input_tokens;
				query.current.output_tokens += call.output_tokens;
				query.current.retries += call.retries;
				query.current.rate_limited_ms += call.rate_limited_ms;
				for (idx_t i = 0; i < call.answers.size(); i++) {
					group.answers[batch_ptr->start + i] = std::move(call.answers[i]);
				}
			} catch (...) {
				state.stats.in_flight--;
				throw;
			}
			state.stats.in_flight--;
		}));
	}

	std::exception_ptr first_error;
	for (auto &future : futures) {
		try {
			future.get();
		} catch (...) {
			if (!first_error) {
				first_error = std::current_exception();
			}
		}
	}
	return first_error;
}

//! Slices each group into requests: a batch closes at snx_jev_batch_size rows, or before
//! its estimated input tokens would pass snx_jev_max_batch_tokens, whichever comes first.
//! A single row over the budget still goes out, alone.
static void PlanBatches(const JevConfig &config, JevGroup &group, vector<JevBatch> &batches) {
	auto size = JevMeasureRequest(config, group.set);
	auto fixed_tokens = size.fixed_tokens;
	idx_t start = 0;
	idx_t count = 0;
	idx_t tokens = fixed_tokens;
	for (idx_t slot = 0; slot < group.rows.size(); slot++) {
		auto row_tokens = JevEstimateTokens(group.rows[slot]) + size.per_row_tokens;
		if (count > 0 && (count == config.batch_size || tokens + row_tokens > config.max_batch_tokens)) {
			batches.push_back(JevBatch {&group, start, count});
			start = slot;
			count = 0;
			tokens = fixed_tokens;
		}
		count++;
		tokens += row_tokens;
	}
	if (count > 0) {
		batches.push_back(JevBatch {&group, start, count});
	}
}

static void WriteAnswer(const JevBindData &data, Vector &result, idx_t row, const vector<string> &answers,
                        const vector<string> &names, double threshold, idx_t level_count) {
	auto &validity = FlatVector::Validity(result);
	if (data.result == JevResult::ASK) {
		// {"<name>": <answer>, ...}, in the order the questions were given
		string out = "{";
		for (idx_t q = 0; q < answers.size(); q++) {
			if (q > 0) {
				out += ',';
			}
			JevWriteJSONString(names[q], out);
			out += ':';
			out += answers[q];
		}
		out += '}';
		FlatVector::GetData<string_t>(result)[row] = StringVector::AddString(result, out);
		return;
	}
	auto &answer_json = answers[0];
	if (data.result == JevResult::EVAL) {
		FlatVector::GetData<string_t>(result)[row] = StringVector::AddString(result, answer_json);
		return;
	}

	ordered_json answer;
	try {
		answer = ordered_json::parse(answer_json);
	} catch (std::exception &error) {
		throw IOException("jev: could not parse the answer for a row: %s", error.what());
	}

	auto number_field = [&](const char *name, double &out) {
		auto field = answer.find(name);
		if (field == answer.end() || !field->is_number()) {
			return false;
		}
		out = field->get<double>();
		return true;
	};

	double number = 0;
	switch (data.result) {
	case JevResult::PREDICATE:
		if (!number_field("noul", number)) {
			validity.SetInvalid(row);
			return;
		}
		FlatVector::GetData<bool>(result)[row] = number >= threshold;
		return;
	case JevResult::PROBABILITY:
		if (!number_field("noul", number)) {
			validity.SetInvalid(row);
			return;
		}
		FlatVector::GetData<double>(result)[row] = number;
		return;
	case JevResult::SCORE:
		if (!number_field("score", number)) {
			validity.SetInvalid(row);
			return;
		}
		FlatVector::GetData<double>(result)[row] = number;
		return;
	case JevResult::SCORE_NORM:
		if (!number_field("score", number)) {
			validity.SetInvalid(row);
			return;
		}
		// Levels are positions 0..n-1, so the last one is the divisor.
		FlatVector::GetData<double>(result)[row] = number / static_cast<double>(MaxValue<idx_t>(level_count - 1, 1));
		return;
	case JevResult::CONFIDENCE:
		if (!number_field("confidence", number)) {
			validity.SetInvalid(row);
			return;
		}
		FlatVector::GetData<double>(result)[row] = number;
		return;
	case JevResult::CHOICE: {
		auto field = answer.find("choice");
		if (field == answer.end() || !field->is_string()) {
			validity.SetInvalid(row);
			return;
		}
		FlatVector::GetData<string_t>(result)[row] = StringVector::AddString(result, field->get<string>());
		return;
	}
	default:
		throw InternalException("jev: unhandled result kind");
	}
}

//===--------------------------------------------------------------------===//
// snx_prompt_intent: the taxonomy
//===--------------------------------------------------------------------===//

//! The questions snx_prompt_intent asks, compiled in: SQL cannot change what is asked.
//! Labels, descriptions and wording are analytics-schema's prompt intent spec §1 (taxonomy
//! version 1). Changing any of them changes the cache key, so old answers are not mixed
//! with new ones.
//!
//! The wording, including the per-item pointers, is Phase 1's compact layout, the one
//! measured against the live API (spec §11).
static const JevQuestionSet &PromptIntentQuestions() {
	static const JevQuestionSet set = []() {
		JevQuestionSet questions;
		JevQuestion intent;
		intent.name = "intent";
		intent.kind = "choice";
		intent.query = "What is the primary intent of this user prompt sent to an AI assistant?";
		intent.options = {"work_related", "personal", "other"};
		intent.descriptions = {
		    "Asks for help with a job task: code, data, documents, analysis, business communication",
		    "Personal life, hobbies, health, relationships, shopping, entertainment \xE2\x80\x94 not a job task",
		    "Too short, empty, or ambiguous to place in either label above"};
		questions.questions.push_back(std::move(intent));

		JevQuestion malicious;
		malicious.name = "malicious";
		// Phase 1's compact wording, which kept every malicious prompt at >= 0.92 while
		// every other stayed <= 0.2 (spec §11). A generic "is it true of" pointer lost that margin.
		malicious.pointer = "Using {rubric} in the state: is {item} malicious?";
		malicious.kind = "noul";
		malicious.query = "Does this user prompt try to misuse, attack, or cause harm through an AI assistant?";
		malicious.descriptions = {"Tries to cause harm or misuse the assistant: jailbreaks, prompt injection, "
		                          "malware, fraud, harassment, extracting secrets or credentials",
		                          "An ordinary request, whatever its topic"};
		questions.questions.push_back(std::move(malicious));
		return questions;
	}();
	return set;
}

static LogicalType PromptIntentType() {
	child_list_t<LogicalType> fields;
	fields.push_back(make_pair("intent", LogicalType::VARCHAR));
	fields.push_back(make_pair("intent_confidence", LogicalType::DOUBLE));
	fields.push_back(make_pair("intent_probabilities", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE)));
	fields.push_back(make_pair("malicious_probability", LogicalType::DOUBLE));
	return LogicalType::STRUCT(std::move(fields));
}

//! One row's answers as the snx_prompt_intent struct. A field the answer lacks is NULL,
//! never a default: 'other' and 0 are real answers.
static Value PromptIntentValue(const vector<string> &answers) {
	auto parse = [](const string &json) {
		try {
			return ordered_json::parse(json);
		} catch (std::exception &error) {
			throw IOException("snx_prompt_intent: could not parse an answer: %s", error.what());
		}
	};
	auto intent = parse(answers[0]);
	auto malicious = parse(answers[1]);

	auto text = [](const ordered_json &answer, const char *field) {
		auto entry = answer.find(field);
		return entry != answer.end() && entry->is_string() ? Value(entry->get<string>()) : Value(LogicalType::VARCHAR);
	};
	auto number = [](const ordered_json &answer, const char *field) {
		auto entry = answer.find(field);
		return entry != answer.end() && entry->is_number() ? Value::DOUBLE(entry->get<double>())
		                                                   : Value(LogicalType::DOUBLE);
	};

	auto map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE);
	Value probabilities(map_type);
	auto entry = intent.find("probabilities");
	if (entry != intent.end() && entry->is_object()) {
		vector<Value> keys;
		vector<Value> values;
		for (auto &item : entry->items()) {
			if (item.value().is_number()) {
				keys.emplace_back(item.key());
				values.push_back(Value::DOUBLE(item.value().get<double>()));
			}
		}
		probabilities = Value::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE, std::move(keys), std::move(values));
	}

	child_list_t<Value> fields;
	fields.push_back(make_pair("intent", text(intent, "choice")));
	fields.push_back(make_pair("intent_confidence", number(intent, "confidence")));
	fields.push_back(make_pair("intent_probabilities", std::move(probabilities)));
	fields.push_back(make_pair("malicious_probability", number(malicious, "noul")));
	return Value::STRUCT(std::move(fields));
}

//===--------------------------------------------------------------------===//
// Execute
//===--------------------------------------------------------------------===//

//! Claims this statement made in the single-flight registry. Every claim is completed
//! exactly once - with the answer, or empty when it could not be sent - even when the
//! statement throws, so no other query waits on it forever.
class JevClaims {
public:
	JevClaims(JevState &session_p, idx_t max_entries_p) : session(session_p), max_entries(max_entries_p) {
	}
	~JevClaims() {
		for (auto &key : open) {
			session.Complete(key, string(), max_entries);
		}
	}
	void Add(const string &key) {
		open.insert(key);
	}
	void Complete(const string &key, const string &answer) {
		if (open.erase(key) > 0) {
			session.Complete(key, answer, max_entries);
		}
	}

private:
	JevState &session;
	idx_t max_entries;
	std::unordered_set<string> open;
};

//! Rows whose key another query was already sending: they take that query's answer, or,
//! when that query failed, claim the key and send it themselves.
struct JevWait {
	std::shared_future<string> answer;
	vector<idx_t> targets;
	string key;
	string row_json;
	JevGroup *group;
};

//! Waits for another query's answer. The wait can be cancelled (it checks the connection's
//! interrupt flag every 100 ms), and gives up once the other query has had longer than all
//! its attempts could take. Returns "" when that query failed.
static string AwaitInFlight(ClientContext &context, const JevConfig &config, const std::shared_future<string> &answer) {
	auto deadline = std::chrono::steady_clock::now() +
	                std::chrono::milliseconds(static_cast<int64_t>((config.timeout + 30.0) * 1000.0) *
	                                          static_cast<int64_t>(MaxValue<idx_t>(config.max_retries, 1)));
	while (answer.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
		if (context.interrupted) {
			throw InterruptException();
		}
		if (std::chrono::steady_clock::now() > deadline) {
			throw IOException("jev: timed out waiting for another query that was sending the same row");
		}
	}
	return answer.get();
}

//! Sends every row the groups hold (all claimed by this statement), completes the claims and
//! writes the answers back. Rethrows the first failed batch after keeping what came back.
static void SendGroups(const JevBindData &data, std::unordered_map<string, unique_ptr<JevGroup>> &groups,
                       JevClaims &claims, JevQueryState &query, vector<vector<string>> &answers,
                       vector<bool> &resolved) {
	vector<JevBatch> batches;
	idx_t pending_rows = 0;
	idx_t pending_chars = 0;
	for (auto &entry : groups) {
		auto &group = *entry.second;
		group.answers.resize(group.rows.size());
		for (auto &row : group.rows) {
			pending_chars += row.size();
		}
		pending_rows += group.rows.size();
		PlanBatches(data.config, group, batches);
	}
	if (batches.empty()) {
		return;
	}
	// Fail before opening a connection when the endpoint, the key or the budget is missing.
	// Throwing here completes every claim empty (JevClaims); a waiter then claims the row itself.
	data.config.RequireSendable();
	CheckSpendGuards(data, pending_rows, pending_chars);
	query.current.rows_sent += pending_rows;
	auto error = RunBatches(data, batches, query);

	for (auto &entry : groups) {
		auto &group = *entry.second;
		for (idx_t slot = 0; slot < group.rows.size(); slot++) {
			if (group.answers[slot].empty()) {
				continue; // its batch failed; the claim is completed empty when the statement ends
			}
			claims.Complete(group.cache_keys[slot], EncodeAnswers(group.answers[slot]));
			for (auto target : group.targets[slot]) {
				answers[target] = group.answers[slot];
				resolved[target] = true;
			}
		}
	}
	if (error) {
		// The query fails, but the batches that did come back are cached, so a retry
		// pays for the rows that are still missing and not for all of them again.
		std::rethrow_exception(error);
	}
}

//! Adds a claimed row to `group`, or its target to the slot of an identical payload.
static void AddClaimedRow(JevGroup &group, const string &key, string row_json, const vector<idx_t> &targets) {
	auto known = group.row_index.find(row_json);
	if (known != group.row_index.end()) {
		for (auto target : targets) {
			group.targets[known->second].push_back(target);
		}
		return;
	}
	auto row_slot = group.rows.size();
	group.row_index.insert(make_pair(row_json, row_slot));
	group.cache_keys.push_back(key);
	group.rows.push_back(std::move(row_json));
	group.targets.push_back(targets);
}

static void JevExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &data = func_expr.bind_info->Cast<JevBindData>();
	auto &session = JevState::Get();
	auto &context = state.GetContext();
	auto query = JevQueryState::Get(context);
	// Read now, not at bind: see JevConfig::CacheScope.
	auto scope = JevConfig::CacheScope(context);
	auto count = args.size();
	bool prompt_intent = data.result == JevResult::PROMPT_INTENT;

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &validity = FlatVector::Validity(result);

	vector<vector<string>> answers(count);
	vector<bool> resolved(count, false);
	vector<JevGroup *> row_groups(count, nullptr);
	vector<double> thresholds(count, data.config.threshold);
	vector<idx_t> level_counts(count, 1);
	std::unordered_map<string, unique_ptr<JevGroup>> groups;
	// jev_ask: each distinct questions text is parsed once per chunk
	std::unordered_map<string, JevGroup *> parsed_sets;
	JevClaims claims(session, data.config.cache_max_entries);
	vector<JevWait> waits;
	std::unordered_map<string, idx_t> wait_index;
	std::unordered_map<string, vector<string>> hits;
	JevGroup *prompt_group = nullptr;

	// Pass 1: serialise every row, answer what the cache already knows, wait on what another
	// query is already sending, and group the rest.
	for (idx_t i = 0; i < count; i++) {
		auto row_value = args.data[0].GetValue(i);
		if (row_value.IsNull()) {
			validity.SetInvalid(i);
			continue;
		}
		Value query_value;
		if (!prompt_intent) {
			query_value = args.data[1].GetValue(i);
			if (query_value.IsNull()) {
				validity.SetInvalid(i);
				continue;
			}
		}
		if (data.threshold_arg != NO_ARG) {
			auto threshold_value = args.data[data.threshold_arg].GetValue(i);
			if (!threshold_value.IsNull()) {
				thresholds[i] = threshold_value.GetValue<double>();
			}
		}

		JevGroup *group = nullptr;
		auto find_or_add = [&](JevQuestionSet set, vector<string> names) {
			auto set_key = set.CacheKey();
			auto entry = groups.find(set_key);
			if (entry == groups.end()) {
				auto key_copy = set_key;
				entry = groups.insert(make_pair(key_copy, MakeGroup(std::move(set), std::move(names), set_key))).first;
			}
			return entry->second.get();
		};
		if (prompt_intent) {
			if (!prompt_group) {
				prompt_group = find_or_add(PromptIntentQuestions(), {});
			}
			group = prompt_group;
		} else if (data.result == JevResult::ASK) {
			auto text = query_value.ToString();
			auto known = parsed_sets.find(text);
			if (known != parsed_sets.end()) {
				group = known->second;
			} else {
				JevQuestionSet set;
				vector<string> names;
				ParseQuestionSet(text, set, names);
				group = find_or_add(std::move(set), std::move(names));
				parsed_sets.insert(make_pair(text, group));
			}
		} else {
			JevQuestion question;
			if (!ResolveKind(data, args, i, question.kind) || !ResolveOptions(data, args, i, question)) {
				validity.SetInvalid(i);
				continue;
			}
			question.query = query_value.ToString();
			level_counts[i] = MaxValue<idx_t>(question.options.size(), 1);
			JevQuestionSet set;
			set.questions.push_back(std::move(question));
			group = find_or_add(std::move(set), {});
		}
		row_groups[i] = group;
		query->used = true;

		auto row_json = JevValueToJSON(row_value, data.config.max_value_chars);
		auto entry_key = CacheEntryKey(scope, data.config.model, *group, row_json);

		// The same payload earlier in this chunk: share its slot or its wait.
		auto known = group->row_index.find(row_json);
		if (known != group->row_index.end()) {
			group->targets[known->second].push_back(i);
			continue;
		}
		auto waiting = wait_index.find(entry_key);
		if (waiting != wait_index.end()) {
			waits[waiting->second].targets.push_back(i);
			continue;
		}

		auto decoded_hit = hits.find(entry_key);
		if (decoded_hit != hits.end()) {
			session.stats.cache_hits++;
			query->current.cache_hits++;
			answers[i] = decoded_hit->second;
			resolved[i] = true;
			continue;
		}

		string cached;
		std::shared_future<string> in_flight;
		switch (session.LookupOrClaim(entry_key, cached, in_flight)) {
		case JevClaim::HIT:
			session.stats.cache_hits++;
			query->current.cache_hits++;
			// decoded once per distinct key in the chunk, however many rows repeat it
			answers[i] = hits.insert(make_pair(entry_key, DecodeAnswers(cached))).first->second;
			resolved[i] = true;
			continue;
		case JevClaim::WAIT:
			wait_index.insert(make_pair(entry_key, waits.size()));
			waits.push_back(JevWait {std::move(in_flight), {i}, entry_key, std::move(row_json), group});
			continue;
		case JevClaim::CLAIMED:
			claims.Add(entry_key);
			break;
		}

		AddClaimedRow(*group, entry_key, std::move(row_json), {i});
	}

	// Pass 2: everything this statement claimed goes out in batches.
	SendGroups(data, groups, claims, *query, answers, resolved);

	// Pass 3: take the answers another query was sending. Our own claims are all complete by
	// now, so two queries waiting on each other's keys cannot deadlock. When the other query
	// failed (for its own reasons, perhaps: its row cap, a bad row of its own), claim the row
	// and send it here, once.
	std::unordered_map<string, unique_ptr<JevGroup>> retry_groups;
	vector<JevWait> retry_waits;
	auto take = [&](JevWait &wait, const string &encoded) {
		session.stats.shared_in_flight += wait.targets.size();
		query->current.shared_in_flight += wait.targets.size();
		auto decoded = DecodeAnswers(encoded);
		for (auto target : wait.targets) {
			answers[target] = decoded;
			resolved[target] = true;
		}
	};
	for (auto &wait : waits) {
		auto encoded = AwaitInFlight(context, data.config, wait.answer);
		if (!encoded.empty()) {
			take(wait, encoded);
			continue;
		}
		string cached;
		std::shared_future<string> in_flight;
		switch (session.LookupOrClaim(wait.key, cached, in_flight)) {
		case JevClaim::HIT:
			take(wait, cached);
			break;
		case JevClaim::WAIT:
			wait.answer = std::move(in_flight);
			retry_waits.push_back(std::move(wait));
			break;
		case JevClaim::CLAIMED: {
			claims.Add(wait.key);
			auto &source = *wait.group;
			auto entry = retry_groups.find(source.set_key);
			if (entry == retry_groups.end()) {
				entry =
				    retry_groups.insert(make_pair(source.set_key, MakeGroup(source.set, source.names, source.set_key)))
				        .first;
			}
			AddClaimedRow(*entry->second, wait.key, wait.row_json, wait.targets);
			break;
		}
		}
	}
	SendGroups(data, retry_groups, claims, *query, answers, resolved);
	for (auto &wait : retry_waits) {
		auto encoded = AwaitInFlight(context, data.config, wait.answer);
		if (encoded.empty()) {
			throw IOException("jev: two other queries sending the same row both failed; run this query again");
		}
		take(wait, encoded);
	}

	// Pass 4: pick the field this function returns out of each answer.
	if (prompt_intent) {
		for (idx_t i = 0; i < count; i++) {
			if (resolved[i]) {
				result.SetValue(i, PromptIntentValue(answers[i]));
			} else {
				result.SetValue(i, Value(result.GetType()));
			}
		}
		return;
	}
	static const vector<string> NO_NAMES;
	for (idx_t i = 0; i < count; i++) {
		if (!resolved[i]) {
			continue;
		}
		WriteAnswer(data, result, i, answers[i], row_groups[i] ? row_groups[i]->names : NO_NAMES, thresholds[i],
		            level_counts[i]);
	}
}

//===--------------------------------------------------------------------===//
// Session helpers
//===--------------------------------------------------------------------===//

static void SetConstantString(Vector &result, const string &text) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, text);
}

static void JevStatsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &session = JevState::Get();
	auto &stats = session.stats;
	auto input_tokens = stats.input_tokens.load();

	ordered_json report;
	report["requests"] = stats.requests.load();
	report["input_tokens"] = input_tokens;
	report["output_tokens"] = stats.output_tokens.load();
	report["rows_evaluated"] = stats.rows_evaluated.load();
	report["cache_hits"] = stats.cache_hits.load();
	report["api_ms"] = stats.api_ms.load();
	report["errors"] = stats.errors.load();
	report["retries"] = stats.retries.load();
	report["in_flight"] = stats.in_flight.load();
	report["shared_in_flight"] = stats.shared_in_flight.load();
	report["rate_limited_ms"] = stats.rate_limited_ms.load();
	report["cached_answers"] = session.CachedAnswers();
	// jev-1.13 list price; output tokens are free.
	report["estimated_cost_usd"] = static_cast<double>(input_tokens) * 0.042 / 1000000.0;
	SetConstantString(result, report.dump());
}

static void JevLastQueryStatsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	SetConstantString(result, JevQueryState::Get(state.GetContext())->LastQueryJSON());
}

static void JevCacheClearFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JevState::Get().Clear();
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<bool>(result)[0] = true;
}

static void JevVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	SetConstantString(result, JEV_VERSION);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

static LogicalType JevJSONType() {
	// The same VARCHAR-with-an-alias the json extension uses, so ->> and friends work.
	LogicalType type(LogicalTypeId::VARCHAR);
	type.SetAlias("JSON");
	return type;
}

static ScalarFunction MakeFunction(const char *name, vector<LogicalType> arguments, LogicalType return_type,
                                   bind_scalar_function_t bind) {
	ScalarFunction function(name, std::move(arguments), std::move(return_type), JevExecute, bind);
	// Same answer within a statement, a new one in the next: STABLE, in Postgres terms.
	function.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	// Says the function can fail, which also keeps it from being evaluated on rows a
	// cheaper predicate in the same WHERE has already thrown away.
	function.SetErrorMode(FunctionErrors::CAN_THROW_RUNTIME_ERROR);
	return function;
}

//! snx_prompt_intent(user_prompt) needs no bind of its own beyond the settings; the questions
//! are compiled in.
static unique_ptr<FunctionData> PromptIntentBind(ClientContext &context, ScalarFunction &bound_function,
                                                 vector<unique_ptr<Expression>> &arguments) {
	auto data = make_uniq<JevBindData>();
	data->config = JevConfig::FromContext(context);
	data->result = JevResult::PROMPT_INTENT;
	return std::move(data);
}

static void RegisterGenericFunctions(ExtensionLoader &loader);

void JevRegisterFunctions(ExtensionLoader &loader) {
	// Always: the one function a process serving generated SQL may call, and this connection's
	// own spend report.
	// snx_prompt_intent(user_prompt) -> STRUCT(intent, intent_confidence, intent_probabilities,
	// malicious_probability). VARCHAR only, so a whole row cannot be passed.
	loader.RegisterFunction(
	    MakeFunction("snx_prompt_intent", {LogicalType::VARCHAR}, PromptIntentType(), PromptIntentBind));
	ScalarFunction last_query_stats("snx_jev_last_query_stats", {}, JevJSONType(), JevLastQueryStatsFunction);
	last_query_stats.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(last_query_stats);
	loader.RegisterFunction(ScalarFunction("jev_version", {}, LogicalType::VARCHAR, JevVersionFunction));

	// Only with SNX_JEV_ENABLE_GENERIC=1 at load: every other function takes its question, or
	// what it reports, from the caller.
	if (JevEnvironment::Get().enable_generic) {
		RegisterGenericFunctions(loader);
	}
}

static void RegisterGenericFunctions(ExtensionLoader &loader) {
	auto any = LogicalType::ANY;
	auto text = LogicalType::VARCHAR;
	auto text_list = LogicalType::LIST(LogicalType::VARCHAR);
	// labels (a list), or label -> what the label means (a MAP); checked in JevBind
	auto labels = LogicalType::ANY;

	// jev(row, condition [, threshold]) -> boolean
	ScalarFunctionSet predicate("jev");
	predicate.AddFunction(MakeFunction("jev", {any, text}, LogicalType::BOOLEAN,
	                                   JevBind<JevResult::PREDICATE, JevKindSource::NOUL, NO_ARG, NO_ARG, NO_ARG>));
	predicate.AddFunction(MakeFunction("jev", {any, text, LogicalType::DOUBLE}, LogicalType::BOOLEAN,
	                                   JevBind<JevResult::PREDICATE, JevKindSource::NOUL, NO_ARG, NO_ARG, 2>));
	loader.RegisterFunction(predicate);

	// jev_prob(row, condition) -> double
	loader.RegisterFunction(MakeFunction("jev_prob", {any, text}, LogicalType::DOUBLE,
	                                     JevBind<JevResult::PROBABILITY, JevKindSource::NOUL, NO_ARG, NO_ARG, NO_ARG>));

	// jev_score(row, question, levels) -> double, and its normalised twin
	loader.RegisterFunction(MakeFunction("jev_score", {any, text, text_list}, LogicalType::DOUBLE,
	                                     JevBind<JevResult::SCORE, JevKindSource::SCORE, NO_ARG, 2, NO_ARG>));
	loader.RegisterFunction(MakeFunction("jev_score_norm", {any, text, text_list}, LogicalType::DOUBLE,
	                                     JevBind<JevResult::SCORE_NORM, JevKindSource::SCORE, NO_ARG, 2, NO_ARG>));

	// jev_choice(row, question, options) -> text; options are labels, or a MAP of label -> description
	loader.RegisterFunction(MakeFunction("jev_choice", {any, text, labels}, LogicalType::VARCHAR,
	                                     JevBind<JevResult::CHOICE, JevKindSource::CHOICE, NO_ARG, 2, NO_ARG>));

	// jev_confidence(row, question, kind, options) -> double
	loader.RegisterFunction(MakeFunction("jev_confidence", {any, text, text, labels}, LogicalType::DOUBLE,
	                                     JevBind<JevResult::CONFIDENCE, JevKindSource::FROM_ARGUMENT, 2, 3, NO_ARG>));

	// jev_eval(row, question [, kind [, options]]) -> json
	ScalarFunctionSet eval("jev_eval");
	eval.AddFunction(MakeFunction("jev_eval", {any, text}, JevJSONType(),
	                              JevBind<JevResult::EVAL, JevKindSource::NOUL, NO_ARG, NO_ARG, NO_ARG>));
	eval.AddFunction(MakeFunction("jev_eval", {any, text, text}, JevJSONType(),
	                              JevBind<JevResult::EVAL, JevKindSource::FROM_ARGUMENT, 2, NO_ARG, NO_ARG>));
	eval.AddFunction(MakeFunction("jev_eval", {any, text, text, labels}, JevJSONType(),
	                              JevBind<JevResult::EVAL, JevKindSource::FROM_ARGUMENT, 2, 3, NO_ARG>));
	loader.RegisterFunction(eval);

	// jev_ask(row, questions) -> json: several named questions about one row, answered in one
	// request over one state. questions is a JSON object of name -> question (see ParseQuestionSet).
	loader.RegisterFunction(MakeFunction("jev_ask", {any, text}, JevJSONType(),
	                                     JevBind<JevResult::ASK, JevKindSource::NOUL, NO_ARG, NO_ARG, NO_ARG>));

	// Session helpers.
	ScalarFunction stats("jev_stats", {}, JevJSONType(), JevStatsFunction);
	stats.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(stats);

	ScalarFunction cache_clear("jev_cache_clear", {}, LogicalType::BOOLEAN, JevCacheClearFunction);
	cache_clear.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(cache_clear);
}

} // namespace duckdb
