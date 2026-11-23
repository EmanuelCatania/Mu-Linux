#pragma once

// JSON mínimo para los manifiestos del actualizador.  Sólo lo que hace falta:
// objetos (conservan el orden de las claves), arreglos, strings, enteros de 64
// bits, booleanos y null.  Los números con decimales o exponente se rechazan:
// ningún campo del manifiesto los usa, y así un valor no se redondea en silencio.
//
// La firma cubre los bytes del archivo tal cual, así que no hace falta una forma
// canónica: el parser sólo tiene que ser estricto.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Update {

class JsonValue
{
public:
	enum class Type { Null, Bool, Int, String, Array, Object };

	JsonValue() = default;
	static JsonValue MakeBool(bool v);
	static JsonValue MakeInt(std::int64_t v);
	static JsonValue MakeString(std::string v);
	static JsonValue MakeArray();
	static JsonValue MakeObject();

	Type GetType() const { return m_Type; }
	bool IsNull() const { return m_Type == Type::Null; }
	bool IsBool() const { return m_Type == Type::Bool; }
	bool IsInt() const { return m_Type == Type::Int; }
	bool IsString() const { return m_Type == Type::String; }
	bool IsArray() const { return m_Type == Type::Array; }
	bool IsObject() const { return m_Type == Type::Object; }

	bool AsBool() const { return m_Bool; }
	std::int64_t AsInt() const { return m_Int; }
	const std::string& AsString() const { return m_String; }
	const std::vector<JsonValue>& Items() const { return m_Array; }
	const std::vector<std::pair<std::string, JsonValue>>& Members() const { return m_Object; }

	// Objeto: devuelve nullptr si la clave no está.
	const JsonValue* Find(const char* key) const;

	// Constructores (sólo arreglos / objetos).
	void Push(JsonValue v);
	void Set(std::string key, JsonValue v);

	// Serializa con sangría de 2 espacios y saltos de línea "\n".
	std::string Dump() const;

private:
	void DumpTo(std::string& out, int depth) const;

	Type m_Type = Type::Null;
	bool m_Bool = false;
	std::int64_t m_Int = 0;
	std::string m_String;
	std::vector<JsonValue> m_Array;
	std::vector<std::pair<std::string, JsonValue>> m_Object;
};

// Parsea un documento entero.  false (y el motivo en *error) si no es JSON
// válido, si sobra texto al final, si una clave se repite o si se pasa de la
// profundidad máxima.
bool ParseJson(const std::string& text, JsonValue& out, std::string* error);

} // namespace Update
