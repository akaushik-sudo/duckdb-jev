#include "jev_functions.hpp"

#include "jev_client.hpp"
#include "jev_config.hpp"
#include "jev_json.hpp"
#include "jev_state.hpp"

#include "duckdb/common/types/hash.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "json.hpp"

#include <unordered_map>

namespace duckdb {

using nlohmann::ordered_json;

//! Which field of the answer the caller wants.
enum class JevResult : uint8_t { PREDICATE, PROBABILITY, SCORE, SCORE_NORM, CHOICE, CONFIDENCE, EVAL };
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
	return std::move(data);
}

//===--------------------------------------------------------------------===//
// Execute
//===--------------------------------------------------------------------===//

//! Rows judged together: one question, the distinct row payloads under it, and where
//! each answer has to be written back to.
struct JevGroup {
	JevQuestion question;
	vector<string> rows;
	vector<string> cache_keys;
	vector<vector<idx_t>> targets;
	vector<string> answers;
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
//! The scope (the connection's customer, see JevConfig::cache_scope) comes first and
//! verbatim, so one customer's answers are never a cache hit for another's: a hit would
//! show, through its speed or through jev_stats(), that someone else sent the same text.
static string CacheEntryKey(const string &scope, const string &question_key, const string &row_json) {
	return scope + "\x1f" + to_string(Hash(question_key.c_str(), question_key.size())) + ":" +
	       to_string(Hash(row_json.c_str(), row_json.size())) + ":" + to_string(row_json.size());
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
static bool ResolveOptions(const JevBindData &data, DataChunk &args, idx_t row, const string &kind,
                           vector<string> &options) {
	options.clear();
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
	for (auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw InvalidInputException("jev: the options of a '%s' question cannot contain NULL.", kind);
		}
		options.push_back(child.ToString());
	}
	if (options.empty()) {
		throw InvalidInputException("jev: a '%s' question needs at least one option.", kind);
	}
	return true;
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
static std::exception_ptr RunBatches(const JevBindData &data, vector<JevBatch> &batches) {
	auto &state = JevState::Get();
	auto pool = state.Pool(data.config.concurrency);

	vector<std::future<void>> futures;
	futures.reserve(batches.size());
	for (auto &batch : batches) {
		auto *batch_ptr = &batch;
		auto *config = &data.config;
		futures.push_back(pool->Submit([batch_ptr, config, &state]() {
			auto &group = *batch_ptr->group;
			vector<string> rows(group.rows.begin() + batch_ptr->start,
			                    group.rows.begin() + batch_ptr->start + batch_ptr->count);
			state.stats.in_flight++;
			try {
				auto answers = JevCallAPI(*config, group.question, rows);
				for (idx_t i = 0; i < answers.size(); i++) {
					group.answers[batch_ptr->start + i] = std::move(answers[i]);
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

static void WriteAnswer(const JevBindData &data, Vector &result, idx_t row, const string &answer_json, double threshold,
                        idx_t level_count) {
	auto &validity = FlatVector::Validity(result);
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

static void JevExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &data = func_expr.bind_info->Cast<JevBindData>();
	auto &session = JevState::Get();
	auto count = args.size();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &validity = FlatVector::Validity(result);

	vector<string> answers(count);
	vector<bool> resolved(count, false);
	vector<double> thresholds(count, data.config.threshold);
	vector<idx_t> level_counts(count, 1);
	std::unordered_map<string, unique_ptr<JevGroup>> groups;

	// Pass 1: serialise every row, answer what the cache already knows and group the rest.
	for (idx_t i = 0; i < count; i++) {
		auto row_value = args.data[0].GetValue(i);
		auto query_value = args.data[1].GetValue(i);
		if (row_value.IsNull() || query_value.IsNull()) {
			validity.SetInvalid(i);
			continue;
		}
		if (data.threshold_arg != NO_ARG) {
			auto threshold_value = args.data[data.threshold_arg].GetValue(i);
			if (!threshold_value.IsNull()) {
				thresholds[i] = threshold_value.GetValue<double>();
			}
		}

		JevQuestion question;
		if (!ResolveKind(data, args, i, question.kind) ||
		    !ResolveOptions(data, args, i, question.kind, question.options)) {
			validity.SetInvalid(i);
			continue;
		}
		question.query = query_value.ToString();
		level_counts[i] = MaxValue<idx_t>(question.options.size(), 1);

		auto row_json = JevValueToJSON(row_value);
		auto question_key = question.CacheKey();
		auto entry_key = CacheEntryKey(data.config.cache_scope, question_key, row_json);

		string cached;
		if (session.Lookup(entry_key, cached)) {
			session.stats.cache_hits++;
			answers[i] = std::move(cached);
			resolved[i] = true;
			continue;
		}

		auto group_entry = groups.find(question_key);
		if (group_entry == groups.end()) {
			auto group = make_uniq<JevGroup>();
			group->question = std::move(question);
			group_entry = groups.insert(make_pair(question_key, std::move(group))).first;
		}
		auto &group = *group_entry->second;
		auto known = group.row_index.find(row_json);
		idx_t row_slot;
		if (known == group.row_index.end()) {
			row_slot = group.rows.size();
			group.row_index.insert(make_pair(row_json, row_slot));
			group.cache_keys.push_back(entry_key);
			group.rows.push_back(std::move(row_json));
			group.targets.emplace_back();
		} else {
			row_slot = known->second;
		}
		group.targets[row_slot].push_back(i);
	}

	// Pass 2: everything the cache could not answer goes out in batches.
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
		for (idx_t start = 0; start < group.rows.size(); start += data.config.batch_size) {
			auto batch_size = MinValue<idx_t>(data.config.batch_size, group.rows.size() - start);
			batches.push_back(JevBatch {&group, start, batch_size});
		}
	}

	if (!batches.empty()) {
		// Fail before opening a connection when the endpoint, the key or the budget is missing.
		data.config.RequireSendable();
		CheckSpendGuards(data, pending_rows, pending_chars);
		auto error = RunBatches(data, batches);

		for (auto &entry : groups) {
			auto &group = *entry.second;
			for (idx_t slot = 0; slot < group.rows.size(); slot++) {
				if (group.answers[slot].empty()) {
					continue; // its batch failed
				}
				session.Store(group.cache_keys[slot], group.answers[slot], data.config.cache_max_entries);
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

	// Pass 3: pick the field this function returns out of each answer.
	for (idx_t i = 0; i < count; i++) {
		if (!resolved[i]) {
			continue;
		}
		WriteAnswer(data, result, i, answers[i], thresholds[i], level_counts[i]);
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
	report["cached_answers"] = session.CachedAnswers();
	// jev-1.13 list price; output tokens are free.
	report["estimated_cost_usd"] = static_cast<double>(input_tokens) * 0.042 / 1000000.0;
	SetConstantString(result, report.dump());
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

void JevRegisterFunctions(ExtensionLoader &loader) {
	auto any = LogicalType::ANY;
	auto text = LogicalType::VARCHAR;
	auto text_list = LogicalType::LIST(LogicalType::VARCHAR);

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

	// jev_choice(row, question, options) -> text
	loader.RegisterFunction(MakeFunction("jev_choice", {any, text, text_list}, LogicalType::VARCHAR,
	                                     JevBind<JevResult::CHOICE, JevKindSource::CHOICE, NO_ARG, 2, NO_ARG>));

	// jev_confidence(row, question, kind, options) -> double
	loader.RegisterFunction(MakeFunction("jev_confidence", {any, text, text, text_list}, LogicalType::DOUBLE,
	                                     JevBind<JevResult::CONFIDENCE, JevKindSource::FROM_ARGUMENT, 2, 3, NO_ARG>));

	// jev_eval(row, question [, kind [, options]]) -> json
	ScalarFunctionSet eval("jev_eval");
	eval.AddFunction(MakeFunction("jev_eval", {any, text}, JevJSONType(),
	                              JevBind<JevResult::EVAL, JevKindSource::NOUL, NO_ARG, NO_ARG, NO_ARG>));
	eval.AddFunction(MakeFunction("jev_eval", {any, text, text}, JevJSONType(),
	                              JevBind<JevResult::EVAL, JevKindSource::FROM_ARGUMENT, 2, NO_ARG, NO_ARG>));
	eval.AddFunction(MakeFunction("jev_eval", {any, text, text, text_list}, JevJSONType(),
	                              JevBind<JevResult::EVAL, JevKindSource::FROM_ARGUMENT, 2, 3, NO_ARG>));
	loader.RegisterFunction(eval);

	// Session helpers.
	ScalarFunction stats("jev_stats", {}, JevJSONType(), JevStatsFunction);
	stats.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(stats);

	ScalarFunction cache_clear("jev_cache_clear", {}, LogicalType::BOOLEAN, JevCacheClearFunction);
	cache_clear.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(cache_clear);

	loader.RegisterFunction(ScalarFunction("jev_version", {}, LogicalType::VARCHAR, JevVersionFunction));
}

} // namespace duckdb
