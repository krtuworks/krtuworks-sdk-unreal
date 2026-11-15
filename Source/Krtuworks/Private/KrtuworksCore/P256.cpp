// ECDSA on P-256 (see P256.h). 256-bit integers are 8 little-endian 32-bit limbs with 64-bit products;
// arithmetic modulo p (coordinates) and modulo n (scalars) is Montgomery multiplication with R = 2^256; points
// are Jacobian. File-local names carry a p256 prefix: Unreal's unity build pastes several .cpp files of the
// module into one translation unit.
#include "KrtuworksCore/P256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "KrtuworksCore/Crypto.h"

namespace
{
// Limb 0 is the least significant.
using P256Limbs = std::array<std::uint32_t, 8>;

// Reads a 64-digit lowercase hex constant at compile time.
constexpr P256Limbs p256Constant(const char (&hex)[65])
{
    P256Limbs limbs{};
    for (std::size_t i = 0; i < 64; ++i)
    {
        const char digit = hex[i];
        const auto value = static_cast<std::uint32_t>(digit <= '9' ? digit - '0' : digit - 'a' + 10);
        const std::size_t fromEnd = 63 - i;
        limbs[fromEnd / 8] |= value << (4 * (fromEnd % 8));
    }
    return limbs;
}

constexpr P256Limbs p256FieldPrime = p256Constant("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
constexpr P256Limbs p256GroupOrder = p256Constant("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
constexpr P256Limbs p256CurveB = p256Constant("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b");
constexpr P256Limbs p256GeneratorX = p256Constant("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296");
constexpr P256Limbs p256GeneratorY = p256Constant("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");

P256Limbs p256FromBytes(std::string_view bytes32) noexcept
{
    P256Limbs limbs{};
    for (std::size_t i = 0; i < 32; ++i)
    {
        const std::size_t fromEnd = 31 - i;
        limbs[fromEnd / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes32[i])) << (8 * (fromEnd % 4));
    }
    return limbs;
}

std::string p256ToBytes(const P256Limbs& limbs)
{
    std::string bytes(32, '\0');
    for (std::size_t i = 0; i < 32; ++i)
    {
        const std::size_t fromEnd = 31 - i;
        bytes[i] = static_cast<char>(static_cast<unsigned char>(limbs[fromEnd / 4] >> (8 * (fromEnd % 4))));
    }
    return bytes;
}

bool p256IsZero(const P256Limbs& a) noexcept
{
    std::uint32_t any = 0;
    for (const std::uint32_t limb : a) any |= limb;
    return any == 0;
}

bool p256Less(const P256Limbs& a, const P256Limbs& b) noexcept
{
    for (std::size_t i = 8; i-- > 0;)
    {
        if (a[i] != b[i]) return a[i] < b[i];
    }
    return false;
}

bool p256Bit(const P256Limbs& a, std::size_t bit) noexcept
{
    return ((a[bit / 32] >> (bit % 32)) & 1u) != 0;
}

// a += b; returns the carry out of the top limb.
std::uint32_t p256AddInto(P256Limbs& a, const P256Limbs& b) noexcept
{
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < 8; ++i)
    {
        carry += static_cast<std::uint64_t>(a[i]) + b[i];
        a[i] = static_cast<std::uint32_t>(carry);
        carry >>= 32;
    }
    return static_cast<std::uint32_t>(carry);
}

// a -= b; returns the borrow out of the top limb.
std::uint32_t p256SubtractFrom(P256Limbs& a, const P256Limbs& b) noexcept
{
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < 8; ++i)
    {
        const std::uint64_t difference = static_cast<std::uint64_t>(a[i]) - b[i] - borrow;
        a[i] = static_cast<std::uint32_t>(difference);
        borrow = difference >> 63;
    }
    return static_cast<std::uint32_t>(borrow);
}

// An odd modulus above 2^255 with what Montgomery multiplication needs.
struct P256Modulus
{
    P256Limbs value{};
    std::uint32_t negativeInverse = 0;  // -value^-1 mod 2^32
    P256Limbs one{};                    // R mod value: 1 in Montgomery form
    P256Limbs rSquared{};               // R^2 mod value: multiplying by it enters Montgomery form
    P256Limbs inverseExponent{};        // value - 2: Fermat inversion
};

// Inputs below the modulus; the output is too.
P256Limbs p256AddMod(P256Limbs a, const P256Limbs& b, const P256Modulus& mod) noexcept
{
    const std::uint32_t carry = p256AddInto(a, b);
    if (carry != 0 || !p256Less(a, mod.value)) p256SubtractFrom(a, mod.value);
    return a;
}

P256Limbs p256SubMod(P256Limbs a, const P256Limbs& b, const P256Modulus& mod) noexcept
{
    if (p256SubtractFrom(a, b) != 0) p256AddInto(a, mod.value);
    return a;
}

// Any value below 2^256 reduced once: the moduli are above 2^255, so one subtraction is enough.
P256Limbs p256Reduce(P256Limbs a, const P256Modulus& mod) noexcept
{
    if (!p256Less(a, mod.value)) p256SubtractFrom(a, mod.value);
    return a;
}

// a * b / R mod value (CIOS: multiply and reduce one limb at a time).
P256Limbs p256MontgomeryMultiply(const P256Limbs& a, const P256Limbs& b, const P256Modulus& mod) noexcept
{
    std::array<std::uint32_t, 10> t{};
    for (std::size_t i = 0; i < 8; ++i)
    {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < 8; ++j)
        {
            const std::uint64_t sum = static_cast<std::uint64_t>(a[j]) * b[i] + t[j] + carry;
            t[j] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
        std::uint64_t top = static_cast<std::uint64_t>(t[8]) + carry;
        t[8] = static_cast<std::uint32_t>(top);
        t[9] = static_cast<std::uint32_t>(top >> 32);

        // Adding m * value makes the lowest limb zero; dropping it divides by 2^32.
        const std::uint32_t m = t[0] * mod.negativeInverse;
        carry = (static_cast<std::uint64_t>(m) * mod.value[0] + t[0]) >> 32;
        for (std::size_t j = 1; j < 8; ++j)
        {
            const std::uint64_t sum = static_cast<std::uint64_t>(m) * mod.value[j] + t[j] + carry;
            t[j - 1] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
        top = static_cast<std::uint64_t>(t[8]) + carry;
        t[7] = static_cast<std::uint32_t>(top);
        t[8] = t[9] + static_cast<std::uint32_t>(top >> 32);
    }
    P256Limbs result{};
    for (std::size_t i = 0; i < 8; ++i) result[i] = t[i];
    if (t[8] != 0 || !p256Less(result, mod.value)) p256SubtractFrom(result, mod.value);
    return result;
}

P256Limbs p256ToMontgomery(const P256Limbs& a, const P256Modulus& mod) noexcept
{
    return p256MontgomeryMultiply(a, mod.rSquared, mod);
}

P256Limbs p256FromMontgomery(const P256Limbs& a, const P256Modulus& mod) noexcept
{
    P256Limbs plainOne{};
    plainOne[0] = 1;
    return p256MontgomeryMultiply(a, plainOne, mod);
}

// a^-1 as a^(value - 2) (Fermat; the moduli are prime), in and out in Montgomery form.
P256Limbs p256Invert(const P256Limbs& a, const P256Modulus& mod) noexcept
{
    P256Limbs result = mod.one;
    for (std::size_t bit = 256; bit-- > 0;)
    {
        result = p256MontgomeryMultiply(result, result, mod);
        if (p256Bit(mod.inverseExponent, bit)) result = p256MontgomeryMultiply(result, a, mod);
    }
    return result;
}

P256Modulus p256MakeModulus(const P256Limbs& value) noexcept
{
    P256Modulus mod;
    mod.value = value;
    // Newton's step doubles the correct low bits of value^-1 mod 2^32: 1, 2, 4, 8, 16, 32 (value is odd).
    std::uint32_t inverse = 1;
    for (int i = 0; i < 5; ++i) inverse *= 2u - value[0] * inverse;
    mod.negativeInverse = 0u - inverse;
    // 2^256 and 2^512 modulo value, by doubling 1 that many times.
    P256Limbs power{};
    power[0] = 1;
    for (int i = 1; i <= 512; ++i)
    {
        power = p256AddMod(power, power, mod);
        if (i == 256) mod.one = power;
    }
    mod.rSquared = power;
    P256Limbs two{};
    two[0] = 2;
    mod.inverseExponent = value;
    p256SubtractFrom(mod.inverseExponent, two);
    return mod;
}

// Jacobian coordinates in Montgomery form modulo p: x = X/Z^2, y = Y/Z^3; Z = 0 is the point at infinity.
struct P256Point
{
    P256Limbs x{};
    P256Limbs y{};
    P256Limbs z{};
};

struct P256Curve
{
    P256Modulus field;
    P256Modulus order;
    P256Limbs b{};
    P256Point generator;
};

P256Curve p256MakeCurve() noexcept
{
    P256Curve curve;
    curve.field = p256MakeModulus(p256FieldPrime);
    curve.order = p256MakeModulus(p256GroupOrder);
    curve.b = p256ToMontgomery(p256CurveB, curve.field);
    curve.generator.x = p256ToMontgomery(p256GeneratorX, curve.field);
    curve.generator.y = p256ToMontgomery(p256GeneratorY, curve.field);
    curve.generator.z = curve.field.one;
    return curve;
}

const P256Curve& p256TheCurve() noexcept
{
    static const P256Curve curve = p256MakeCurve();
    return curve;
}

P256Point p256Double(const P256Point& point, const P256Modulus& field) noexcept
{
    if (p256IsZero(point.z) || p256IsZero(point.y)) return P256Point{};
    // a = -3: M = 3 (X - Z^2)(X + Z^2)
    const P256Limbs zz = p256MontgomeryMultiply(point.z, point.z, field);
    const P256Limbs difference = p256SubMod(point.x, zz, field);
    const P256Limbs product = p256MontgomeryMultiply(difference, p256AddMod(point.x, zz, field), field);
    const P256Limbs m = p256AddMod(p256AddMod(product, product, field), product, field);
    const P256Limbs yy = p256MontgomeryMultiply(point.y, point.y, field);
    const P256Limbs xyy = p256MontgomeryMultiply(point.x, yy, field);
    const P256Limbs xyy2 = p256AddMod(xyy, xyy, field);
    const P256Limbs s = p256AddMod(xyy2, xyy2, field);
    P256Point result;
    result.x = p256SubMod(p256SubMod(p256MontgomeryMultiply(m, m, field), s, field), s, field);
    const P256Limbs yyyy = p256MontgomeryMultiply(yy, yy, field);
    const P256Limbs yyyy2 = p256AddMod(yyyy, yyyy, field);
    const P256Limbs yyyy4 = p256AddMod(yyyy2, yyyy2, field);
    const P256Limbs yyyy8 = p256AddMod(yyyy4, yyyy4, field);
    result.y = p256SubMod(p256MontgomeryMultiply(m, p256SubMod(s, result.x, field), field), yyyy8, field);
    const P256Limbs yz = p256MontgomeryMultiply(point.y, point.z, field);
    result.z = p256AddMod(yz, yz, field);
    return result;
}

P256Point p256Add(const P256Point& p, const P256Point& q, const P256Modulus& field) noexcept
{
    if (p256IsZero(p.z)) return q;
    if (p256IsZero(q.z)) return p;
    const P256Limbs z1z1 = p256MontgomeryMultiply(p.z, p.z, field);
    const P256Limbs z2z2 = p256MontgomeryMultiply(q.z, q.z, field);
    const P256Limbs u1 = p256MontgomeryMultiply(p.x, z2z2, field);
    const P256Limbs u2 = p256MontgomeryMultiply(q.x, z1z1, field);
    const P256Limbs s1 = p256MontgomeryMultiply(p256MontgomeryMultiply(p.y, q.z, field), z2z2, field);
    const P256Limbs s2 = p256MontgomeryMultiply(p256MontgomeryMultiply(q.y, p.z, field), z1z1, field);
    if (u1 == u2) return s1 == s2 ? p256Double(p, field) : P256Point{};
    const P256Limbs h = p256SubMod(u2, u1, field);
    const P256Limbs r = p256SubMod(s2, s1, field);
    const P256Limbs hh = p256MontgomeryMultiply(h, h, field);
    const P256Limbs hhh = p256MontgomeryMultiply(h, hh, field);
    const P256Limbs v = p256MontgomeryMultiply(u1, hh, field);
    P256Point result;
    result.x = p256SubMod(p256SubMod(p256SubMod(p256MontgomeryMultiply(r, r, field), hhh, field), v, field), v, field);
    result.y = p256SubMod(p256MontgomeryMultiply(r, p256SubMod(v, result.x, field), field),
        p256MontgomeryMultiply(s1, hhh, field), field);
    result.z = p256MontgomeryMultiply(p256MontgomeryMultiply(p.z, q.z, field), h, field);
    return result;
}

// scalar * point, double-and-add from the top bit.
P256Point p256Multiply(const P256Limbs& scalar, const P256Point& point, const P256Modulus& field) noexcept
{
    P256Point result{};
    for (std::size_t bit = 256; bit-- > 0;)
    {
        result = p256Double(result, field);
        if (p256Bit(scalar, bit)) result = p256Add(result, point, field);
    }
    return result;
}

struct P256Affine
{
    P256Limbs x{};
    P256Limbs y{};
};

// Plain integers below p; the point must not be at infinity.
P256Affine p256ToAffine(const P256Point& point, const P256Modulus& field) noexcept
{
    const P256Limbs zInverse = p256Invert(point.z, field);
    const P256Limbs zInverse2 = p256MontgomeryMultiply(zInverse, zInverse, field);
    P256Affine affine;
    affine.x = p256FromMontgomery(p256MontgomeryMultiply(point.x, zInverse2, field), field);
    affine.y = p256FromMontgomery(p256MontgomeryMultiply(p256MontgomeryMultiply(point.y, zInverse2, field), zInverse, field), field);
    return affine;
}

// y^2 = x^3 - 3x + b with both coordinates below p.
bool p256OnCurve(const P256Limbs& x, const P256Limbs& y, const P256Curve& curve) noexcept
{
    const P256Modulus& field = curve.field;
    if (!p256Less(x, field.value) || !p256Less(y, field.value)) return false;
    const P256Limbs xm = p256ToMontgomery(x, field);
    const P256Limbs ym = p256ToMontgomery(y, field);
    const P256Limbs x3 = p256MontgomeryMultiply(p256MontgomeryMultiply(xm, xm, field), xm, field);
    const P256Limbs threeX = p256AddMod(p256AddMod(xm, xm, field), xm, field);
    const P256Limbs rightSide = p256AddMod(p256SubMod(x3, threeX, field), curve.b, field);
    return p256MontgomeryMultiply(ym, ym, field) == rightSide;
}

// The digest as an integer modulo n (RFC 6979 bits2int, then one reduction: qlen = hlen = 256).
P256Limbs p256DigestScalar(std::string_view message, const P256Modulus& order)
{
    return p256Reduce(p256FromBytes(krtu::sha256(message)), order);
}
}  // namespace

namespace krtu::p256
{
bool validPrivateKey(std::string_view d32) noexcept
{
    if (d32.size() != 32) return false;
    const P256Limbs d = p256FromBytes(d32);
    return !p256IsZero(d) && p256Less(d, p256GroupOrder);
}

std::string newPrivateKey()
{
    for (;;)
    {
        std::string d = randomBytes(32);
        if (validPrivateKey(d)) return d;
    }
}

std::pair<std::string, std::string> publicKey(std::string_view d32)
{
    if (!validPrivateKey(d32)) throw std::invalid_argument("P-256: not a private key in [1, n-1]");
    const P256Curve& curve = p256TheCurve();
    const P256Affine point = p256ToAffine(p256Multiply(p256FromBytes(d32), curve.generator, curve.field), curve.field);
    return {p256ToBytes(point.x), p256ToBytes(point.y)};
}

std::string sign(std::string_view d32, std::string_view message)
{
    if (!validPrivateKey(d32)) throw std::invalid_argument("P-256: not a private key in [1, n-1]");
    const P256Curve& curve = p256TheCurve();
    const P256Modulus& order = curve.order;
    const P256Limbs z = p256DigestScalar(message, order);
    const P256Limbs d = p256FromBytes(d32);
    // RFC 6979 §3.2: K and V seeded from int2octets(d) and bits2octets(digest), each candidate is the next V.
    const std::string seed = std::string(d32) + p256ToBytes(z);
    std::string v(32, '\x01');
    std::string k(32, '\x00');
    k = hmacSha256(k, v + '\x00' + seed);
    v = hmacSha256(k, v);
    k = hmacSha256(k, v + '\x01' + seed);
    v = hmacSha256(k, v);
    for (;;)
    {
        v = hmacSha256(k, v);
        const P256Limbs nonce = p256FromBytes(v);
        if (!p256IsZero(nonce) && p256Less(nonce, order.value))
        {
            const P256Limbs r = p256Reduce(p256ToAffine(p256Multiply(nonce, curve.generator, curve.field), curve.field).x, order);
            if (!p256IsZero(r))
            {
                // s = k^-1 (z + r d) mod n
                const P256Limbs nonceInverse = p256Invert(p256ToMontgomery(nonce, order), order);
                const P256Limbs rd = p256MontgomeryMultiply(p256ToMontgomery(r, order), p256ToMontgomery(d, order), order);
                const P256Limbs sum = p256AddMod(p256ToMontgomery(z, order), rd, order);
                const P256Limbs s = p256FromMontgomery(p256MontgomeryMultiply(nonceInverse, sum, order), order);
                if (!p256IsZero(s)) return p256ToBytes(r) + p256ToBytes(s);
            }
        }
        k = hmacSha256(k, v + '\x00');
        v = hmacSha256(k, v);
    }
}

bool verifySignature(std::string_view x32, std::string_view y32, std::string_view message, std::string_view signature64) noexcept
{
    try
    {
        if (x32.size() != 32 || y32.size() != 32 || signature64.size() != 64) return false;
        const P256Curve& curve = p256TheCurve();
        const P256Modulus& order = curve.order;
        const P256Limbs qx = p256FromBytes(x32);
        const P256Limbs qy = p256FromBytes(y32);
        if (!p256OnCurve(qx, qy, curve)) return false;
        const P256Limbs r = p256FromBytes(signature64.substr(0, 32));
        const P256Limbs s = p256FromBytes(signature64.substr(32, 32));
        if (p256IsZero(r) || !p256Less(r, order.value) || p256IsZero(s) || !p256Less(s, order.value)) return false;
        const P256Limbs z = p256DigestScalar(message, order);
        const P256Limbs w = p256Invert(p256ToMontgomery(s, order), order);
        const P256Limbs u1 = p256FromMontgomery(p256MontgomeryMultiply(p256ToMontgomery(z, order), w, order), order);
        const P256Limbs u2 = p256FromMontgomery(p256MontgomeryMultiply(p256ToMontgomery(r, order), w, order), order);
        P256Point q;
        q.x = p256ToMontgomery(qx, curve.field);
        q.y = p256ToMontgomery(qy, curve.field);
        q.z = curve.field.one;
        const P256Point sum = p256Add(p256Multiply(u1, curve.generator, curve.field), p256Multiply(u2, q, curve.field), curve.field);
        if (p256IsZero(sum.z)) return false;
        return p256Reduce(p256ToAffine(sum, curve.field).x, order) == r;
    }
    catch (...)
    {
        // Only allocation can throw here; a signature that cannot be checked is not accepted.
        return false;
    }
}
}  // namespace krtu::p256
