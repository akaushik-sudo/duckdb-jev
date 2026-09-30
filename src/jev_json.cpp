#include "jev_json.hpp"

#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/value.hpp"

#include <cmath>

namespace duckdb {

void JevWriteJSONString(const string &text, string &out) {
	out += '"';
	for (auto c : text) {
		auto byte = static_cast<unsigned char>(c);
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		default:
			if (byte < 0x20) {
				// Control characters have no short escape; \u00XX is the only legal form.
				static const char *HEX = "0123456789abcdef";
				out += "\\u00";
				out += HEX[(byte >> 4) & 0xF];
				out += HEX[byte & 0xF];
			} else {
				// Anything else is passed through: DuckDB VARCHARs are already valid UTF-8.
				out += c;
			}
			break;
		}
	}
	out += '"';
}

string JevQuoteJSONString(const string &text) {
	string out;
	JevWriteJSONString(text, out);
	return out;
}

static void WriteValue(const Value &value, idx_t max_chars, string &out);

static void WriteQuoted(const Value &value, string &out) {
	JevWriteJSONString(value.ToString(), out);
}

//! `text` cut to its first `max_chars` characters (UTF-8 code points, not bytes, so a
//! multi-byte character is never split). 0 keeps everything.
static string Truncate(const string &text, idx_t max_chars) {
	if (max_chars == 0 || text.size() <= max_chars) {
		return text;
	}
	idx_t chars = 0;
	for (idx_t i = 0; i < text.size(); i++) {
		// A byte that is not a continuation byte (10xxxxxx) starts a character.
		if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
			if (chars == max_chars) {
				return text.substr(0, i);
			}
			chars++;
		}
	}
	return text;
}

static void WriteChildren(const vector<Value> &children, idx_t max_chars, string &out) {
	out += '[';
	for (idx_t i = 0; i < children.size(); i++) {
		if (i > 0) {
			out += ',';
		}
		WriteValue(children[i], max_chars, out);
	}
	out += ']';
}

static void WriteValue(const Value &value, idx_t max_chars, string &out) {
	if (value.IsNull()) {
		out += "null";
		return;
	}
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		out += value.GetValue<bool>() ? "true" : "false";
		return;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DECIMAL:
		// Decimal strings round-trip exactly and stay numbers in JSON.
		out += value.ToString();
		return;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		auto number = value.GetValue<double>();
		if (!std::isfinite(number)) {
			// JSON has no inf/nan literal; a string would change the type the model sees.
			out += "null";
			return;
		}
		out += value.ToString();
		return;
	}
	case LogicalTypeId::VARCHAR:
		JevWriteJSONString(Truncate(StringValue::Get(value), max_chars), out);
		return;
	case LogicalTypeId::BLOB: {
		auto blob = value.GetValueUnsafe<string_t>();
		JevWriteJSONString(Blob::ToBase64(blob), out);
		return;
	}
	case LogicalTypeId::STRUCT: {
		auto &children = StructValue::GetChildren(value);
		auto &child_types = StructType::GetChildTypes(type);
		out += '{';
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				out += ',';
			}
			JevWriteJSONString(child_types[i].first, out);
			out += ':';
			WriteValue(children[i], max_chars, out);
		}
		out += '}';
		return;
	}
	case LogicalTypeId::LIST:
		WriteChildren(ListValue::GetChildren(value), max_chars, out);
		return;
	case LogicalTypeId::ARRAY:
		WriteChildren(ArrayValue::GetChildren(value), max_chars, out);
		return;
	case LogicalTypeId::MAP: {
		// A MAP becomes a JSON object; keys that are not strings are stringified.
		auto &entries = MapValue::GetChildren(value);
		out += '{';
		for (idx_t i = 0; i < entries.size(); i++) {
			auto &entry = StructValue::GetChildren(entries[i]);
			if (i > 0) {
				out += ',';
			}
			JevWriteJSONString(entry[0].ToString(), out);
			out += ':';
			WriteValue(entry[1], max_chars, out);
		}
		out += '}';
		return;
	}
	case LogicalTypeId::UNION:
		WriteValue(UnionValue::GetValue(value), max_chars, out);
		return;
	default:
		// Dates, timestamps, intervals, uuids, enums, bits: their text form is what a
		// reader would expect to see.
		WriteQuoted(value, out);
		return;
	}
}

string JevValueToJSON(const Value &value, idx_t max_chars) {
	string out;
	WriteValue(value, max_chars, out);
	return out;
}

} // namespace duckdb
