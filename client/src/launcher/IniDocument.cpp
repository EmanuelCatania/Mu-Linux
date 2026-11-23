#include "IniDocument.h"

#include "update/FileUtil.h"

#include <cctype>

namespace Launcher {

namespace {

bool EqualNoCase(const std::string& a, const std::string& b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
	return true;
}

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

} // namespace

IniDocument::IniDocument(std::string separator) : m_Separator(std::move(separator)) {}

bool IniDocument::Load(const std::string& path)
{
	std::string text;
	if (!Update::FileExists(path))
	{
		Parse(std::string());
		return true;
	}
	if (!Update::ReadWholeFile(path, text)) return false;
	Parse(text);
	return true;
}

void IniDocument::Parse(const std::string& text)
{
	m_Lines.clear();
	m_Eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
	m_FinalEol = text.empty() || text.back() == '\n';
	std::string section;
	std::size_t pos = 0;
	while (pos < text.size())
	{
		std::size_t nl = text.find('\n', pos);
		std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		pos = nl == std::string::npos ? text.size() : nl + 1;
		if (!raw.empty() && raw.back() == '\r') raw.pop_back();

		Line line;
		line.text = raw;
		std::size_t a = 0;
		while (a < raw.size() && IsSpace(raw[a])) ++a;
		if (a < raw.size() && raw[a] == '[')
		{
			const std::size_t close = raw.find(']', a);
			if (close != std::string::npos)
			{
				std::size_t s = a + 1, e = close;
				while (s < e && IsSpace(raw[s])) ++s;
				while (e > s && IsSpace(raw[e - 1])) --e;
				section = raw.substr(s, e - s);
			}
		}
		else if (a < raw.size() && raw[a] != ';' && raw[a] != '#')
		{
			const std::size_t eq = raw.find('=', a);
			if (eq != std::string::npos)
			{
				std::size_t ke = eq;
				while (ke > a && IsSpace(raw[ke - 1])) --ke;
				std::size_t vs = eq + 1, ve = raw.size();
				while (vs < ve && IsSpace(raw[vs])) ++vs;
				while (ve > vs && IsSpace(raw[ve - 1])) --ve;
				line.isKey = ke > a;
				line.key = raw.substr(a, ke - a);
				line.valueStart = vs;
				line.valueEnd = ve;
			}
		}
		line.section = section;
		m_Lines.push_back(line);
	}
}

std::string IniDocument::Text() const
{
	std::string out;
	for (std::size_t i = 0; i < m_Lines.size(); ++i)
	{
		out += m_Lines[i].text;
		if (i + 1 < m_Lines.size() || m_FinalEol) out += m_Eol;
	}
	return out;
}

bool IniDocument::Save(const std::string& path) const
{
	const std::string text = Text();
	return Update::WriteFileAtomic(path, text.data(), text.size());
}

int IniDocument::Find(const std::string& section, const std::string& key) const
{
	for (std::size_t i = 0; i < m_Lines.size(); ++i)
		if (m_Lines[i].isKey && EqualNoCase(m_Lines[i].section, section) && EqualNoCase(m_Lines[i].key, key))
			return static_cast<int>(i);
	return -1;
}

bool IniDocument::Get(const std::string& section, const std::string& key, std::string& value) const
{
	const int i = Find(section, key);
	if (i < 0) return false;
	const Line& l = m_Lines[static_cast<std::size_t>(i)];
	value = l.text.substr(l.valueStart, l.valueEnd - l.valueStart);
	return true;
}

bool IniDocument::Set(const std::string& section, const std::string& key, const std::string& value)
{
	const int found = Find(section, key);
	if (found >= 0)
	{
		Line& l = m_Lines[static_cast<std::size_t>(found)];
		if (l.text.compare(l.valueStart, l.valueEnd - l.valueStart, value) == 0) return false;
		l.text = l.text.substr(0, l.valueStart) + value + l.text.substr(l.valueEnd);
		l.valueEnd = l.valueStart + value.size();
		return true;
	}

	Line nl;
	nl.text = key + m_Separator + value;
	nl.section = section;
	nl.isKey = true;
	nl.key = key;
	nl.valueStart = key.size() + m_Separator.size();
	nl.valueEnd = nl.text.size();

	// Después de la última clave (o del encabezado) de la sección: los
	// comentarios y líneas vacías que siguen quedan para la sección siguiente.
	int after = -1;
	bool sectionSeen = section.empty();
	for (std::size_t i = 0; i < m_Lines.size(); ++i)
	{
		const Line& l = m_Lines[i];
		if (!EqualNoCase(l.section, section)) continue;
		const bool header = !l.isKey && !l.text.empty() && l.text.find('[') != std::string::npos &&
		                    l.text.find_first_not_of(" \t") == l.text.find('[');
		if (header) sectionSeen = true;
		if (l.isKey || header) after = static_cast<int>(i);
	}
	if (section.empty() && after < 0)
	{
		// Sección raíz sin claves: antes de la primera sección.
		std::size_t at = 0;
		while (at < m_Lines.size() && m_Lines[at].section.empty()) ++at;
		while (at > 0 && m_Lines[at - 1].text.empty()) --at;
		m_Lines.insert(m_Lines.begin() + static_cast<std::ptrdiff_t>(at), nl);
		m_FinalEol = true;
		return true;
	}
	if (!sectionSeen || after < 0)
	{
		Line header;
		header.text = "[" + section + "]";
		header.section = section;
		m_Lines.push_back(header);
		m_Lines.push_back(nl);
		m_FinalEol = true;
		return true;
	}
	m_Lines.insert(m_Lines.begin() + after + 1, nl);
	if (static_cast<std::size_t>(after) + 2 == m_Lines.size()) m_FinalEol = true;
	return true;
}

} // namespace Launcher
