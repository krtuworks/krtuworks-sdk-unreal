#include "KrtuworksCore/Json.h"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace krtu
{
namespace
{
// Every helper here carries a json prefix: Unreal's unity builds merge the anonymous namespaces of a
// module's files into one.

constexpr int jsonMaxDepth = 256;
// While parsing, an object below this size finds a repeated key by a linear scan; a bigger one indexes
// its keys, so a large object still parses in linear time.
constexpr std::size_t jsonIndexThreshold = 32;
constexpr double jsonTwoPow53 = 9007199254740992.0;
constexpr double jsonTwoPow63 = 9223372036854775808.0;

const char* jsonTypeName(Json::Type kind) noexcept
{
    switch (kind)
    {
        case Json::Type::Null: return "null";
        case Json::Type::Bool: return "a boolean";
        case Json::Type::Number: return "a number";
        case Json::Type::String: return "a string";
        case Json::Type::Array: return "an array";
        case Json::Type::Object: return "an object";
    }
    return "unknown";
}

[[noreturn]] void jsonThrowType(const char* wanted, Json::Type actual)
{
    throw std::logic_error(std::string("JSON value is ") + jsonTypeName(actual) + ", not " + wanted);
}

std::uint64_t jsonBitsOf(double value) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

// NaN and infinity are told by the bits, not by std::isfinite, std::isnan or a comparison: fast-math
// builds may assume neither exists and fold those to a constant.
bool jsonIsFinite(double value) noexcept
{
    return ((jsonBitsOf(value) >> 52) & 0x7FF) != 0x7FF;
}

bool jsonIsNaN(double value) noexcept
{
    return (jsonBitsOf(value) & 0x7FFFFFFFFFFFFFFFu) > 0x7FF0000000000000u;
}

// Integral and inside int64's range, so a cast to int64 is exact.
bool jsonIsWholeInt64(double value) noexcept
{
    return jsonIsFinite(value) && value >= -jsonTwoPow63 && value < jsonTwoPow63 && std::trunc(value) == value;
}

// Exact comparison: converting the integer to a double instead would call 2^53 + 1 equal to 2^53.
bool jsonDoubleEqualsInteger(double value, std::int64_t whole) noexcept
{
    return jsonIsWholeInt64(value) && static_cast<std::int64_t>(value) == whole;
}

// The C locale's decimal point, which snprintf writes and strtod expects. A game may have set a locale
// whose point is a comma or a multi-byte character.
std::string_view jsonLocalePoint() noexcept
{
    const std::lconv* conventions = std::localeconv();
    if (conventions == nullptr || conventions->decimal_point == nullptr || conventions->decimal_point[0] == '\0')
    {
        return ".";
    }
    return conventions->decimal_point;
}

// Reads a whole decimal number written with "." as its point, whatever the locale. False when strtod
// does not take all of it.
bool jsonReadDouble(std::string_view canonical, double& result)
{
    const std::string_view point = jsonLocalePoint();
    std::string buffer;
    buffer.reserve(canonical.size() + point.size());
    for (const char c : canonical)
    {
        if (c == '.')
        {
            buffer.append(point.data(), point.size());
        }
        else
        {
            buffer.push_back(c);
        }
    }
    char* end = nullptr;
    result = std::strtod(buffer.c_str(), &end);
    return end == buffer.c_str() + buffer.size();
}

// Reads an integer literal (optional minus, digits) when it fits int64.
bool jsonReadInteger(std::string_view literal, std::int64_t& result) noexcept
{
    const bool negative = !literal.empty() && literal[0] == '-';
    const std::uint64_t limit = negative ? std::uint64_t{9223372036854775808u} : std::uint64_t{9223372036854775807u};
    std::uint64_t magnitude = 0;
    for (std::size_t i = negative ? 1 : 0; i < literal.size(); ++i)
    {
        const std::uint64_t digit = static_cast<std::uint64_t>(literal[i] - '0');
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (negative && magnitude == limit)
    {
        result = (std::numeric_limits<std::int64_t>::min)();
    }
    else
    {
        result = negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
    }
    return true;
}

// `value` with `precision` significant digits in canonical form: "." as the point whatever the locale, and
// the exponent without leading zeros (C libraries pad it to two or three digits, so the bytes would
// differ between platforms).
std::string jsonFormatDouble(double value, int precision)
{
    char printed[64];
    const int length = std::snprintf(printed, sizeof printed, "%.*g", precision, value);
    if (length <= 0 || length >= static_cast<int>(sizeof printed)) return std::string();
    std::string text(printed, static_cast<std::size_t>(length));
    const std::string_view point = jsonLocalePoint();
    if (point != ".")
    {
        const std::size_t at = text.find(point.data(), 0, point.size());
        if (at != std::string::npos) text.replace(at, point.size(), ".");
    }
    const std::size_t exponent = text.find('e');
    if (exponent != std::string::npos)
    {
        std::size_t firstDigit = exponent + 1;
        if (firstDigit < text.size() && (text[firstDigit] == '+' || text[firstDigit] == '-')) ++firstDigit;
        std::size_t firstKept = firstDigit;
        while (firstKept + 1 < text.size() && text[firstKept] == '0') ++firstKept;
        text.erase(firstDigit, firstKept - firstDigit);
    }
    return text;
}

void jsonWriteInteger(std::string& out, std::int64_t value)
{
    char digits[24];
    std::size_t count = 0;
    std::uint64_t magnitude = value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    do
    {
        digits[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0) out.push_back('-');
    while (count > 0) out.push_back(digits[--count]);
}

void jsonWriteDouble(std::string& out, double value)
{
    if (!jsonIsFinite(value))
    {
        out += "null";
        return;
    }
    // Like JavaScript's JSON.stringify: an integral value below 2^53 is written as an integer (3.0 as "3",
    // -0.0 as "0").
    if (std::fabs(value) < jsonTwoPow53 && std::trunc(value) == value)
    {
        jsonWriteInteger(out, static_cast<std::int64_t>(value));
        return;
    }
    std::string text;
    for (int precision = 15; precision <= 17; ++precision)
    {
        text = jsonFormatDouble(value, precision);
        double back = 0;
        if (jsonReadDouble(text, back) && back == value) break;
    }
    out += text.empty() ? "null" : text;
}

void jsonWriteString(std::string& out, std::string_view text)
{
    static const char hexDigits[] = "0123456789abcdef";
    out.push_back('"');
    std::size_t runStart = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        out.append(text.data() + runStart, i - runStart);
        runStart = i + 1;
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                out += "\\u00";
                out.push_back(hexDigits[c >> 4]);
                out.push_back(hexDigits[c & 0xF]);
                break;
        }
    }
    out.append(text.data() + runStart, text.size() - runStart);
    out.push_back('"');
}

void jsonAppendUtf8(std::string& out, std::uint32_t codePoint)
{
    if (codePoint < 0x80)
    {
        out.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// Recursive descent over the RFC 8259 grammar. Every read checks the position against the end first, and
// the depth cap bounds the recursion (and that of writing, comparing and destroying the result).
class JsonParser
{
public:
    explicit JsonParser(std::string_view source) noexcept : input(source) {}

    std::optional<Json> run()
    {
        Json result;
        skipSpace();
        if (!parseValue(result, 0)) return std::nullopt;
        skipSpace();
        if (position != input.size()) return std::nullopt;
        return result;
    }

private:
    bool atEnd() const noexcept { return position >= input.size(); }

    bool isDigitAt(std::size_t at) const noexcept { return at < input.size() && input[at] >= '0' && input[at] <= '9'; }

    // JSON whitespace is these four only; a form feed or a no-break space is an error.
    void skipSpace() noexcept
    {
        while (!atEnd())
        {
            const char c = input[position];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
            ++position;
        }
    }

    bool parseValue(Json& out, int depth)
    {
        if (atEnd()) return false;
        switch (input[position])
        {
            case '{': return parseObject(out, depth + 1);
            case '[': return parseArray(out, depth + 1);
            case '"':
            {
                std::string text;
                if (!parseString(text)) return false;
                out = Json(std::move(text));
                return true;
            }
            case 't': return parseWord("true", Json(true), out);
            case 'f': return parseWord("false", Json(false), out);
            case 'n': return parseWord("null", Json(), out);
            default: return parseNumber(out);
        }
    }

    bool parseWord(std::string_view word, Json meaning, Json& out)
    {
        if (input.substr(position, word.size()) != word) return false;
        position += word.size();
        out = std::move(meaning);
        return true;
    }

    bool parseArray(Json& out, int depth)
    {
        if (depth > jsonMaxDepth) return false;
        ++position;
        out = Json::array();
        Json::Items& list = out.items();
        skipSpace();
        if (!atEnd() && input[position] == ']')
        {
            ++position;
            return true;
        }
        for (;;)
        {
            list.emplace_back();
            if (!parseValue(list.back(), depth)) return false;
            skipSpace();
            if (atEnd()) return false;
            const char next = input[position++];
            if (next == ']') return true;
            if (next != ',') return false;
            skipSpace();
        }
    }

    bool parseObject(Json& out, int depth)
    {
        if (depth > jsonMaxDepth) return false;
        ++position;
        out = Json::object();
        Json::Members& list = out.members();
        // Built only once the object grows past jsonIndexThreshold (some C++ libraries allocate even
        // for an empty map).
        std::optional<std::unordered_map<std::string, std::size_t>> index;
        skipSpace();
        if (!atEnd() && input[position] == '}')
        {
            ++position;
            return true;
        }
        for (;;)
        {
            if (atEnd() || input[position] != '"') return false;
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (atEnd() || input[position] != ':') return false;
            ++position;
            skipSpace();
            Json member;
            if (!parseValue(member, depth)) return false;
            std::size_t existing = list.size();
            if (!index && list.size() < jsonIndexThreshold)
            {
                for (std::size_t i = 0; i < list.size(); ++i)
                {
                    if (list[i].first == key)
                    {
                        existing = i;
                        break;
                    }
                }
            }
            else
            {
                if (!index)
                {
                    index.emplace();
                    for (std::size_t i = 0; i < list.size(); ++i) index->emplace(list[i].first, i);
                }
                const auto found = index->find(key);
                if (found != index->end())
                {
                    existing = found->second;
                }
                else
                {
                    index->emplace(key, list.size());
                }
            }
            if (existing < list.size())
            {
                list[existing].second = std::move(member);
            }
            else
            {
                list.emplace_back(std::move(key), std::move(member));
            }
            skipSpace();
            if (atEnd()) return false;
            const char next = input[position++];
            if (next == '}') return true;
            if (next != ',') return false;
            skipSpace();
        }
    }

    bool readHex4(std::uint32_t& unit) noexcept
    {
        if (input.size() - position < 4) return false;
        std::uint32_t result = 0;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const char c = input[position + i];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9')
            {
                digit = static_cast<std::uint32_t>(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            }
            else if (c >= 'A' && c <= 'F')
            {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            }
            else
            {
                return false;
            }
            result = (result << 4) | digit;
        }
        position += 4;
        unit = result;
        return true;
    }

    // After "\u": one UTF-16 unit, or a surrogate pair written as two escapes. A high surrogate without
    // a low one after it, or a low one alone, is U+FFFD; whatever follows is read on its own.
    bool parseUnicodeEscape(std::string& out)
    {
        std::uint32_t unit = 0;
        if (!readHex4(unit)) return false;
        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            const std::size_t resume = position;
            std::uint32_t low = 0;
            if (input.size() - position >= 2 && input[position] == '\\' && input[position + 1] == 'u')
            {
                position += 2;
                if (readHex4(low) && low >= 0xDC00 && low <= 0xDFFF)
                {
                    jsonAppendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                    return true;
                }
            }
            position = resume;
            jsonAppendUtf8(out, 0xFFFD);
            return true;
        }
        jsonAppendUtf8(out, unit >= 0xDC00 && unit <= 0xDFFF ? 0xFFFD : unit);
        return true;
    }

    // From the opening quote. Bytes from 0x80 up are copied as they are; a raw control character is an error.
    bool parseString(std::string& out)
    {
        ++position;
        for (;;)
        {
            const std::size_t runStart = position;
            while (!atEnd())
            {
                const unsigned char c = static_cast<unsigned char>(input[position]);
                if (c == '"' || c == '\\' || c < 0x20) break;
                ++position;
            }
            out.append(input.data() + runStart, position - runStart);
            if (atEnd()) return false;
            const char stop = input[position++];
            if (stop == '"') return true;
            if (stop != '\\' || atEnd()) return false;
            const char escape = input[position++];
            switch (escape)
            {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    if (!parseUnicodeEscape(out)) return false;
                    break;
                default: return false;
            }
        }
    }

    // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? and nothing looser: no "+1", ".5", "1.", "01".
    bool parseNumber(Json& out)
    {
        const std::size_t start = position;
        if (input[position] == '-') ++position;
        if (!isDigitAt(position)) return false;
        if (input[position] == '0')
        {
            ++position;
        }
        else
        {
            while (isDigitAt(position)) ++position;
        }
        bool integral = true;
        if (!atEnd() && input[position] == '.')
        {
            integral = false;
            ++position;
            if (!isDigitAt(position)) return false;
            while (isDigitAt(position)) ++position;
        }
        if (!atEnd() && (input[position] == 'e' || input[position] == 'E'))
        {
            integral = false;
            ++position;
            if (!atEnd() && (input[position] == '+' || input[position] == '-')) ++position;
            if (!isDigitAt(position)) return false;
            while (isDigitAt(position)) ++position;
        }
        const std::string_view literal = input.substr(start, position - start);
        std::int64_t whole = 0;
        if (integral && jsonReadInteger(literal, whole))
        {
            out = Json(whole);
            return true;
        }
        double number = 0;
        if (!jsonReadDouble(literal, number) || !jsonIsFinite(number)) return false;
        out = Json(number);
        return true;
    }

    std::string_view input;
    std::size_t position = 0;
};
}  // namespace

Json& Json::operator=(const Json& other)
{
    Storage copy(other.storage);
    storage = std::move(copy);
    return *this;
}

Json& Json::operator=(Json&& other) noexcept
{
    Storage taken(std::move(other.storage));
    storage = std::move(taken);
    return *this;
}

Json Json::array(std::initializer_list<Json> values)
{
    Json result;
    result.storage.emplace<Items>(values);
    return result;
}

Json Json::object(std::initializer_list<std::pair<std::string, Json>> entries)
{
    Json result;
    result.storage.emplace<Members>();
    for (const auto& entry : entries) result[entry.first] = entry.second;
    return result;
}

Json::Type Json::type() const noexcept
{
    static constexpr Type kinds[] = {Type::Null, Type::Bool, Type::Number, Type::Number, Type::String, Type::Array, Type::Object};
    static_assert(sizeof kinds / sizeof kinds[0] == std::variant_size<Storage>::value, "one kind per stored alternative");
    const std::size_t slot = storage.index();
    return slot < std::variant_size<Storage>::value ? kinds[slot] : Type::Null;
}

bool Json::isInteger() const noexcept
{
    if (std::holds_alternative<std::int64_t>(storage)) return true;
    const double* number = std::get_if<double>(&storage);
    return number != nullptr && jsonIsWholeInt64(*number);
}

bool Json::asBool() const
{
    const bool* flag = std::get_if<bool>(&storage);
    if (flag == nullptr) jsonThrowType("a boolean", type());
    return *flag;
}

double Json::asDouble() const
{
    if (const std::int64_t* whole = std::get_if<std::int64_t>(&storage)) return static_cast<double>(*whole);
    const double* number = std::get_if<double>(&storage);
    if (number == nullptr) jsonThrowType("a number", type());
    return *number;
}

std::int64_t Json::asInt() const
{
    if (const std::int64_t* whole = std::get_if<std::int64_t>(&storage)) return *whole;
    const double* number = std::get_if<double>(&storage);
    if (number == nullptr) jsonThrowType("an integer", type());
    if (!jsonIsWholeInt64(*number)) throw std::logic_error("JSON number is not an integer that fits int64");
    return static_cast<std::int64_t>(*number);
}

const std::string& Json::asString() const
{
    const std::string* text = std::get_if<std::string>(&storage);
    if (text == nullptr) jsonThrowType("a string", type());
    return *text;
}

const Json* Json::find(std::string_view key) const noexcept
{
    const Members* list = std::get_if<Members>(&storage);
    if (list == nullptr) return nullptr;
    for (const auto& member : *list)
    {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

Json* Json::find(std::string_view key) noexcept
{
    return const_cast<Json*>(static_cast<const Json&>(*this).find(key));
}

bool Json::contains(std::string_view key) const noexcept
{
    return find(key) != nullptr;
}

Json& Json::operator[](std::string_view key)
{
    Members& list = members();
    for (auto& member : list)
    {
        if (member.first == key) return member.second;
    }
    list.emplace_back(std::string(key), Json());
    return list.back().second;
}

const Json& Json::operator[](std::string_view key) const noexcept
{
    static const Json missing;
    const Json* member = find(key);
    return member != nullptr ? *member : missing;
}

bool Json::erase(std::string_view key)
{
    Members* list = std::get_if<Members>(&storage);
    if (list == nullptr) return false;
    for (auto at = list->begin(); at != list->end(); ++at)
    {
        if (at->first == key)
        {
            list->erase(at);
            return true;
        }
    }
    return false;
}

const Json::Members& Json::members() const noexcept
{
    static const Members none;
    const Members* list = std::get_if<Members>(&storage);
    return list != nullptr ? *list : none;
}

Json::Members& Json::members()
{
    if (isNull()) storage.emplace<Members>();
    Members* list = std::get_if<Members>(&storage);
    if (list == nullptr) jsonThrowType("an object", type());
    return *list;
}

std::optional<std::string> Json::getString(std::string_view key) const
{
    const Json* member = find(key);
    if (member == nullptr || !member->isString()) return std::nullopt;
    return member->asString();
}

std::optional<double> Json::getNumber(std::string_view key) const
{
    const Json* member = find(key);
    if (member == nullptr || !member->isNumber()) return std::nullopt;
    return member->asDouble();
}

std::optional<std::int64_t> Json::getInt(std::string_view key) const
{
    const Json* member = find(key);
    if (member == nullptr || !member->isInteger()) return std::nullopt;
    return member->asInt();
}

std::optional<bool> Json::getBool(std::string_view key) const
{
    const Json* member = find(key);
    if (member == nullptr || !member->isBool()) return std::nullopt;
    return member->asBool();
}

void Json::push(Json value)
{
    items().push_back(std::move(value));
}

const Json& Json::at(std::size_t index) const
{
    const Items& list = items();
    if (index >= list.size()) throw std::out_of_range("JSON array index out of range");
    return list[index];
}

Json& Json::at(std::size_t index)
{
    Items* list = std::get_if<Items>(&storage);
    if (list == nullptr || index >= list->size()) throw std::out_of_range("JSON array index out of range");
    return (*list)[index];
}

const Json::Items& Json::items() const noexcept
{
    static const Items none;
    const Items* list = std::get_if<Items>(&storage);
    return list != nullptr ? *list : none;
}

Json::Items& Json::items()
{
    if (isNull()) storage.emplace<Items>();
    Items* list = std::get_if<Items>(&storage);
    if (list == nullptr) jsonThrowType("an array", type());
    return *list;
}

std::size_t Json::size() const noexcept
{
    if (const Items* list = std::get_if<Items>(&storage)) return list->size();
    if (const Members* list = std::get_if<Members>(&storage)) return list->size();
    return 0;
}

std::string Json::dump() const
{
    std::string out;
    writeTo(out);
    return out;
}

std::optional<Json> Json::parse(std::string_view text)
{
    try
    {
        return JsonParser(text).run();
    }
    catch (const std::exception&)
    {
        // Out of memory, or a string longer than std::string allows: the text is untrusted, so this is a
        // failed parse like any other, never an exception into the game.
        return std::nullopt;
    }
}

Json::Storage Json::unsignedStorage(std::uint64_t value) noexcept
{
    if (value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
    {
        return Storage(std::in_place_type<double>, static_cast<double>(value));
    }
    return Storage(std::in_place_type<std::int64_t>, static_cast<std::int64_t>(value));
}

bool Json::equals(const Json& a, const Json& b) noexcept
{
    const std::int64_t* aWhole = std::get_if<std::int64_t>(&a.storage);
    const std::int64_t* bWhole = std::get_if<std::int64_t>(&b.storage);
    const double* aNumber = std::get_if<double>(&a.storage);
    const double* bNumber = std::get_if<double>(&b.storage);
    if ((aWhole != nullptr || aNumber != nullptr) && (bWhole != nullptr || bNumber != nullptr))
    {
        if (aWhole != nullptr && bWhole != nullptr) return *aWhole == *bWhole;
        if (aNumber != nullptr && bNumber != nullptr) return !jsonIsNaN(*aNumber) && !jsonIsNaN(*bNumber) && *aNumber == *bNumber;
        return aWhole != nullptr ? jsonDoubleEqualsInteger(*bNumber, *aWhole) : jsonDoubleEqualsInteger(*aNumber, *bWhole);
    }
    if (a.storage.index() != b.storage.index()) return false;
    if (const bool* flag = std::get_if<bool>(&a.storage)) return *flag == *std::get_if<bool>(&b.storage);
    if (const std::string* text = std::get_if<std::string>(&a.storage)) return *text == *std::get_if<std::string>(&b.storage);
    if (const Items* left = std::get_if<Items>(&a.storage))
    {
        const Items& right = *std::get_if<Items>(&b.storage);
        if (left->size() != right.size()) return false;
        for (std::size_t i = 0; i < left->size(); ++i)
        {
            if (!equals((*left)[i], right[i])) return false;
        }
        return true;
    }
    if (const Members* left = std::get_if<Members>(&a.storage))
    {
        // Keys are unique and the sizes match, so every member of a found equal in b means the same set.
        // The same position is tried first: two objects built the same way share their order.
        const Members& right = *std::get_if<Members>(&b.storage);
        if (left->size() != right.size()) return false;
        for (std::size_t i = 0; i < left->size(); ++i)
        {
            const auto& member = (*left)[i];
            const Json* match = right[i].first == member.first ? &right[i].second : b.find(member.first);
            if (match == nullptr || !equals(member.second, *match)) return false;
        }
        return true;
    }
    return true;
}

void Json::writeTo(std::string& out) const
{
    if (const bool* flag = std::get_if<bool>(&storage))
    {
        out += *flag ? "true" : "false";
    }
    else if (const std::int64_t* whole = std::get_if<std::int64_t>(&storage))
    {
        jsonWriteInteger(out, *whole);
    }
    else if (const double* number = std::get_if<double>(&storage))
    {
        jsonWriteDouble(out, *number);
    }
    else if (const std::string* text = std::get_if<std::string>(&storage))
    {
        jsonWriteString(out, *text);
    }
    else if (const Items* list = std::get_if<Items>(&storage))
    {
        out.push_back('[');
        for (std::size_t i = 0; i < list->size(); ++i)
        {
            if (i > 0) out.push_back(',');
            (*list)[i].writeTo(out);
        }
        out.push_back(']');
    }
    else if (const Members* entries = std::get_if<Members>(&storage))
    {
        out.push_back('{');
        for (std::size_t i = 0; i < entries->size(); ++i)
        {
            if (i > 0) out.push_back(',');
            jsonWriteString(out, (*entries)[i].first);
            out.push_back(':');
            (*entries)[i].second.writeTo(out);
        }
        out.push_back('}');
    }
    else
    {
        out += "null";
    }
}
}  // namespace krtu
