// JSON for the engine-independent core: build, read, parse and write JSON with the C++ standard library
// alone (the core links no JSON library and includes no engine header).
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace krtu
{
/// One JSON value: null, a boolean, a number, a string, an array or an object. Copies are deep.
/// Numbers keep integers exactly as 64-bit integers and everything else as a double. Objects keep their
/// members in insertion order and dump() writes compact, deterministic text: a stored queue is parsed
/// and written again before it is resent, and the server hashes a frozen batch, so the same value must
/// always become the same bytes.
class Json
{
public:
    /// The JSON kinds. Integers and doubles are both Number.
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    /// An object's members in insertion order. A key appears at most once: every member function keeps
    /// it so, and code that edits members() directly must keep it so too.
    using Members = std::vector<std::pair<std::string, Json>>;
    /// An array's items.
    using Items = std::vector<Json>;

private:
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Items, Members>;

public:
    /// null.
    Json() noexcept = default;
    /// null.
    Json(std::nullptr_t) noexcept {}
    /// true or false.
    Json(bool value) noexcept : storage(std::in_place_type<bool>, value) {}
    /// Any integral type except bool, kept exactly as a 64-bit integer (character types too: Json('A') is
    /// 65). An unsigned value above INT64_MAX does not fit and becomes a double.
    template <class T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value, int>::type = 0>
    Json(T value) noexcept : storage(integralStorage(value))
    {
    }
    /// A double (a float promotes to it). NaN and the infinities are kept, but dump() writes them as null.
    Json(double value) noexcept : storage(std::in_place_type<double>, value) {}
    /// A string. A null pointer is JSON null.
    Json(const char* value) : storage(value == nullptr ? Storage() : Storage(std::in_place_type<std::string>, value)) {}
    /// A string.
    Json(std::string value) noexcept : storage(std::in_place_type<std::string>, std::move(value)) {}
    /// A string.
    Json(std::string_view value) : storage(std::in_place_type<std::string>, value) {}

    /// A deep copy.
    Json(const Json& other) = default;
    /// Takes over `other`'s value; `other` is left valid but unspecified.
    Json(Json&& other) noexcept = default;
    /// Copies `other` before replacing this value, so `node = node["inner"]` is safe although `other`
    /// lives inside this value.
    Json& operator=(const Json& other);
    /// Takes `other` out before replacing this value, so `node = std::move(node["inner"])` is safe.
    Json& operator=(Json&& other) noexcept;

    /// An array of `values`, e.g. Json::array({1, "two", nullptr}).
    static Json array(std::initializer_list<Json> values = {});
    /// An object of `entries` in their order, e.g. Json::object({{"id", 7}, {"name", "x"}}). A repeated key
    /// keeps its first position and its last value, as parse() does.
    static Json object(std::initializer_list<std::pair<std::string, Json>> entries = {});

    /// The kind of this value.
    Type type() const noexcept;
    /// True for null.
    bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(storage); }
    /// True for true and false.
    bool isBool() const noexcept { return std::holds_alternative<bool>(storage); }
    /// True for any number, integer or double.
    bool isNumber() const noexcept { return std::holds_alternative<std::int64_t>(storage) || std::holds_alternative<double>(storage); }
    /// True for a number whose value is integral and fits int64: 3 and 3.0 both are, 3.5 and 1e19 are not.
    bool isInteger() const noexcept;
    /// True for a string.
    bool isString() const noexcept { return std::holds_alternative<std::string>(storage); }
    /// True for an array.
    bool isArray() const noexcept { return std::holds_alternative<Items>(storage); }
    /// True for an object.
    bool isObject() const noexcept { return std::holds_alternative<Members>(storage); }

    /// The boolean. std::logic_error for any other type.
    bool asBool() const;
    /// The number as a double. std::logic_error for any other type.
    double asDouble() const;
    /// The number as an integer. std::logic_error for any other type, and for a number that is not
    /// integral or does not fit int64 (it is never rounded or clamped).
    std::int64_t asInt() const;
    /// The string. std::logic_error for any other type.
    const std::string& asString() const;

    /// The member named `key`, or nullptr when this is not an object or has no such member.
    const Json* find(std::string_view key) const noexcept;
    /// The member named `key`, or nullptr when this is not an object or has no such member.
    Json* find(std::string_view key) noexcept;
    /// True when this is an object with a member named `key`.
    bool contains(std::string_view key) const noexcept;
    /// The member named `key` for writing: null first turns into an empty object and a missing key is
    /// appended as null. std::logic_error for any other type. The reference stays valid until the object
    /// gains or loses a member.
    Json& operator[](std::string_view key);
    /// The member named `key`, or a shared null when it is missing or this is not an object, so chained
    /// reads such as answer["error"]["code"] never throw.
    const Json& operator[](std::string_view key) const noexcept;
    /// Removes the member named `key`; the others keep their order. True when a member was removed.
    bool erase(std::string_view key);
    /// The members in insertion order; empty for anything that is not an object.
    const Members& members() const noexcept;
    /// The members for editing: null first turns into an empty object. std::logic_error for any other type.
    Members& members();

    /// The member `key` when it is a string, otherwise std::nullopt.
    std::optional<std::string> getString(std::string_view key) const;
    /// The member `key` when it is a number, otherwise std::nullopt.
    std::optional<double> getNumber(std::string_view key) const;
    /// The member `key` when it is an integer (isInteger()), otherwise std::nullopt.
    std::optional<std::int64_t> getInt(std::string_view key) const;
    /// The member `key` when it is a boolean, otherwise std::nullopt.
    std::optional<bool> getBool(std::string_view key) const;

    /// Appends `value` to this array: null first turns into an empty array. std::logic_error for any
    /// other type. (There is no operator[](std::size_t): j[0] would be ambiguous with the key overload.)
    void push(Json value);
    /// The item at `index`. std::out_of_range when this is not an array or the index is past its end.
    const Json& at(std::size_t index) const;
    /// The item at `index`. std::out_of_range when this is not an array or the index is past its end.
    Json& at(std::size_t index);
    /// The items; empty for anything that is not an array.
    const Items& items() const noexcept;
    /// The items for editing: null first turns into an empty array. std::logic_error for any other type.
    Items& items();

    /// The number of members or items; 0 for anything that is not an object or an array.
    std::size_t size() const noexcept;
    /// True when size() is 0.
    bool empty() const noexcept { return size() == 0; }

    /// Compact JSON: no whitespace, members in insertion order, integers as plain digits, a double with an
    /// integral value below 2^53 in magnitude without a fraction (3.0 is "3", -0.0 is "0"), any other
    /// double with the fewest of 15, 16 or 17 significant digits that read back to the same double (0.1 is
    /// "0.1", 1e-7 is "1e-7", 1.5e300 is "1.5e+300"), NaN and the infinities as null. Strings escape only
    /// what JSON requires and pass UTF-8 through. The result never depends on the C locale.
    std::string dump() const;
    /// Reads one strict RFC 8259 value with optional surrounding whitespace, nested at most 256 deep.
    /// An integer literal that fits int64 is kept as an integer, any other number as a double (one too
    /// large for a double is an error). A lone surrogate escape becomes U+FFFD. A key that appears twice
    /// keeps its first position and its last value. Any error, including running out of memory, gives
    /// std::nullopt: server answers are untrusted, so this never throws and never reads past `text`.
    static std::optional<Json> parse(std::string_view text);

    /// Deep equality. Numbers compare by exact value (3 == 3.0, NaN equals nothing), arrays in order,
    /// objects as unordered sets of members.
    friend bool operator==(const Json& a, const Json& b) noexcept { return equals(a, b); }
    /// Deep inequality, see operator==.
    friend bool operator!=(const Json& a, const Json& b) noexcept { return !equals(a, b); }

private:
    template <class T>
    static Storage integralStorage(T value) noexcept
    {
        if constexpr (std::is_signed<T>::value)
        {
            return Storage(std::in_place_type<std::int64_t>, static_cast<std::int64_t>(value));
        }
        else
        {
            return unsignedStorage(static_cast<std::uint64_t>(value));
        }
    }

    static Storage unsignedStorage(std::uint64_t value) noexcept;
    static bool equals(const Json& a, const Json& b) noexcept;
    void writeTo(std::string& out) const;

    Storage storage;
};
}  // namespace krtu
