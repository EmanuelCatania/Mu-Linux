#include "update/Json.h"

#include <cstdio>
#include <cstring>

namespace Update {

JsonValue JsonValue::MakeBool(bool v)
{
	JsonValue j;
	j.m_Type = Type::Bool;
	j.m_Bool = v;
	return j;
}

JsonValue JsonValue::MakeInt(std::int64_t v)
{
	JsonValue j;
	j.m_Type = Type::Int;
	j.m_Int = v;
	return j;
}

JsonValue JsonValue::MakeString(std::string v)
{
	JsonValue j;
	j.m_Type = Type::String;
	j.m_String = std::move(v);
	return j;
}

JsonValue JsonValue::MakeArray()
{
	JsonValue j;
	j.m_Type = Type::Array;
	return j;
}

JsonValue JsonValue::MakeObject()
{
	JsonValue j;
	j.m_Type = Type::Object;
	return j;
}

const JsonValue* JsonValue::Find(const char* key) const
{
	if (m_Type != Type::Object) return nullptr;
	for (const auto& m : m_Object)
		if (m.first == key) return &m.second;
	return nullptr;
}

void JsonValue::Push(JsonValue v)
{
	m_Array.push_back(std::move(v));
}

void JsonValue::Set(std::string key, JsonValue v)
{
	for (auto& m : m_Object)
	{
		if (m.first == key)
		{
			m.second = std::move(v);
			return;
		}
	}
	m_Object.emplace_back(std::move(key), std::move(v));
}

namespace {

void DumpString(std::string& out, const std::string& s)
{
	out += '"';
	for (unsigned char c : s)
	{
		switch (c)
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20)
			{
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			}
			else
			{
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
}

void Indent(std::string& out, int depth)
{
	out += '\n';
	out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

// Un objeto o arreglo sin anidar se escribe en una sola línea: con miles de
// archivos en el manifiesto, una línea por archivo se lee y se compara mejor.
bool IsFlat(const JsonValue& v)
{
	if (v.IsArray())
	{
		for (const auto& i : v.Items())
			if (i.IsArray() || i.IsObject()) return false;
		return true;
	}
	if (v.IsObject())
	{
		for (const auto& m : v.Members())
			if (m.second.IsArray() || m.second.IsObject()) return false;
		return true;
	}
	return true;
}

} // namespace

void JsonValue::DumpTo(std::string& out, int depth) const
{
	switch (m_Type)
	{
	case Type::Null: out += "null"; break;
	case Type::Bool: out += m_Bool ? "true" : "false"; break;
	case Type::Int:
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(m_Int));
		out += buf;
		break;
	}
	case Type::String: DumpString(out, m_String); break;
	case Type::Array:
	{
		if (m_Array.empty()) { out += "[]"; break; }
		const bool flat = IsFlat(*this);
		out += '[';
		for (std::size_t i = 0; i < m_Array.size(); ++i)
		{
			if (i) out += flat ? ", " : ",";
			if (!flat) Indent(out, depth + 1);
			m_Array[i].DumpTo(out, depth + 1);
		}
		if (!flat) Indent(out, depth);
		out += ']';
		break;
	}
	case Type::Object:
	{
		if (m_Object.empty()) { out += "{}"; break; }
		const bool flat = IsFlat(*this) && depth > 0;
		out += flat ? "{ " : "{";
		for (std::size_t i = 0; i < m_Object.size(); ++i)
		{
			if (i) out += flat ? ", " : ",";
			if (!flat) Indent(out, depth + 1);
			DumpString(out, m_Object[i].first);
			out += ": ";
			m_Object[i].second.DumpTo(out, depth + 1);
		}
		if (flat) out += " }";
		else { Indent(out, depth); out += '}'; }
		break;
	}
	}
}

std::string JsonValue::Dump() const
{
	std::string out;
	DumpTo(out, 0);
	out += '\n';
	return out;
}

namespace {

constexpr int kMaxDepth = 32;

struct Parser
{
	const char* p;
	const char* end;
	std::string error;

	bool Fail(const char* what)
	{
		if (error.empty()) error = what;
		return false;
	}

	void SkipSpace()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
	}

	bool Literal(const char* word)
	{
		const std::size_t n = std::strlen(word);
		if (static_cast<std::size_t>(end - p) < n || std::memcmp(p, word, n) != 0) return Fail("literal no válido");
		p += n;
		return true;
	}

	static void AppendUtf8(std::string& s, std::uint32_t cp)
	{
		if (cp < 0x80) s += static_cast<char>(cp);
		else if (cp < 0x800)
		{
			s += static_cast<char>(0xC0 | (cp >> 6));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			s += static_cast<char>(0xE0 | (cp >> 12));
			s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else
		{
			s += static_cast<char>(0xF0 | (cp >> 18));
			s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}

	bool Hex4(std::uint32_t& out)
	{
		if (end - p < 4) return Fail("\\u incompleto");
		out = 0;
		for (int i = 0; i < 4; ++i)
		{
			const char c = *p++;
			out <<= 4;
			if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
			else return Fail("\\u no válido");
		}
		return true;
	}

	bool String(std::string& out)
	{
		if (p >= end || *p != '"') return Fail("se esperaba un string");
		++p;
		while (p < end)
		{
			const unsigned char c = static_cast<unsigned char>(*p++);
			if (c == '"') return true;
			if (c < 0x20) return Fail("carácter de control en un string");
			if (c != '\\') { out += static_cast<char>(c); continue; }
			if (p >= end) break;
			const char e = *p++;
			switch (e)
			{
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u':
			{
				std::uint32_t cp;
				if (!Hex4(cp)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF)
				{
					std::uint32_t lo;
					if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return Fail("surrogate sin pareja");
					p += 2;
					if (!Hex4(lo)) return false;
					if (lo < 0xDC00 || lo > 0xDFFF) return Fail("surrogate sin pareja");
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				else if (cp >= 0xDC00 && cp <= 0xDFFF)
				{
					return Fail("surrogate sin pareja");
				}
				AppendUtf8(out, cp);
				break;
			}
			default: return Fail("escape no válido");
			}
		}
		return Fail("string sin cerrar");
	}

	bool Number(JsonValue& out)
	{
		bool negative = false;
		if (*p == '-') { negative = true; ++p; }
		if (p >= end || *p < '0' || *p > '9') return Fail("número no válido");
		if (*p == '0' && p + 1 < end && p[1] >= '0' && p[1] <= '9') return Fail("número con cero a la izquierda");
		std::uint64_t v = 0;
		const std::uint64_t limit = negative ? 9223372036854775808ull : 9223372036854775807ull;
		while (p < end && *p >= '0' && *p <= '9')
		{
			const std::uint64_t d = static_cast<std::uint64_t>(*p - '0');
			if (v > (limit - d) / 10) return Fail("número fuera de rango");
			v = v * 10 + d;
			++p;
		}
		if (p < end && (*p == '.' || *p == 'e' || *p == 'E')) return Fail("sólo se aceptan enteros");
		out = JsonValue::MakeInt(negative ? static_cast<std::int64_t>(0 - v) : static_cast<std::int64_t>(v));
		return true;
	}

	bool Value(JsonValue& out, int depth)
	{
		if (depth > kMaxDepth) return Fail("demasiado anidado");
		SkipSpace();
		if (p >= end) return Fail("fin inesperado");
		switch (*p)
		{
		case 'n': if (!Literal("null")) return false; out = JsonValue(); return true;
		case 't': if (!Literal("true")) return false; out = JsonValue::MakeBool(true); return true;
		case 'f': if (!Literal("false")) return false; out = JsonValue::MakeBool(false); return true;
		case '"':
		{
			std::string s;
			if (!String(s)) return false;
			out = JsonValue::MakeString(std::move(s));
			return true;
		}
		case '[':
		{
			++p;
			out = JsonValue::MakeArray();
			SkipSpace();
			if (p < end && *p == ']') { ++p; return true; }
			for (;;)
			{
				JsonValue item;
				if (!Value(item, depth + 1)) return false;
				out.Push(std::move(item));
				SkipSpace();
				if (p >= end) return Fail("arreglo sin cerrar");
				if (*p == ',') { ++p; continue; }
				if (*p == ']') { ++p; return true; }
				return Fail("se esperaba , o ]");
			}
		}
		case '{':
		{
			++p;
			out = JsonValue::MakeObject();
			SkipSpace();
			if (p < end && *p == '}') { ++p; return true; }
			for (;;)
			{
				SkipSpace();
				std::string key;
				if (!String(key)) return false;
				if (out.Find(key.c_str())) return Fail("clave repetida");
				SkipSpace();
				if (p >= end || *p != ':') return Fail("se esperaba :");
				++p;
				JsonValue item;
				if (!Value(item, depth + 1)) return false;
				out.Set(std::move(key), std::move(item));
				SkipSpace();
				if (p >= end) return Fail("objeto sin cerrar");
				if (*p == ',') { ++p; continue; }
				if (*p == '}') { ++p; return true; }
				return Fail("se esperaba , o }");
			}
		}
		default:
			return Number(out);
		}
	}
};

} // namespace

bool ParseJson(const std::string& text, JsonValue& out, std::string* error)
{
	Parser parser{ text.data(), text.data() + text.size(), {} };
	JsonValue value;
	bool ok = parser.Value(value, 0);
	if (ok)
	{
		parser.SkipSpace();
		if (parser.p != parser.end) ok = parser.Fail("texto sobrante al final");
	}
	if (!ok)
	{
		if (error)
		{
			char buf[64];
			std::snprintf(buf, sizeof(buf), " (byte %lld)", static_cast<long long>(parser.p - text.data()));
			*error = parser.error + buf;
		}
		return false;
	}
	out = std::move(value);
	return true;
}

} // namespace Update
