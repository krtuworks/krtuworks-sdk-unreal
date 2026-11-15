// The core's cryptographic primitives in plain standard C++: the core links nothing but the standard library,
// so it builds the same with MSVC, Apple clang and the Android NDK. Bytes travel as std::string holding raw
// bytes; inputs are taken as std::string_view.
#pragma once

#include "KrtuworksCore/Export.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace krtu
{
/// SHA-256 (FIPS 180-4) of data: 32 raw bytes.
KRTU_API std::string sha256(std::string_view data);

/// HMAC-SHA256 (RFC 2104) of data under key: 32 raw bytes. Any key length; a key longer than the 64-byte
/// block is hashed first.
KRTU_API std::string hmacSha256(std::string_view key, std::string_view data);

/// The bytes as lowercase hex.
KRTU_API std::string toHex(std::string_view bytes);

/// The bytes written as hex (either case). Throws std::invalid_argument on an odd length or a non-hex character.
KRTU_API std::string fromHex(std::string_view hex);

/// Standard base64 (RFC 4648 §4) with '=' padding.
KRTU_API std::string base64(std::string_view bytes);

/// The bytes written as standard base64, padded or not. Padding, when present, must be exactly what the length
/// needs. Throws std::invalid_argument on any other character (whitespace included), wrong padding or a length
/// no encoding produces.
KRTU_API std::string fromBase64(std::string_view text);

/// URL-safe base64 (RFC 4648 §5) without padding, the form JWS and PKCE use.
KRTU_API std::string base64Url(std::string_view bytes);

/// The bytes written as URL-safe base64, padded or not; throws std::invalid_argument as fromBase64 does.
KRTU_API std::string fromBase64Url(std::string_view text);

/// Equal bytes, compared in a time that does not depend on where they differ (MACs, secrets). Different lengths
/// are unequal at once: a length is not a secret.
bool fixedTimeEquals(std::string_view a, std::string_view b) noexcept;

/// count bytes from the current random source.
KRTU_API std::string randomBytes(std::size_t count);

/// Replaces the random source: the engine layer installs the platform's cryptographic generator. An empty
/// function restores the default, std::random_device (cryptographically strong on MSVC, libstdc++ and libc++).
/// A mutex serializes every call, so the source need not be thread-safe; it must not call randomBytes itself.
KRTU_API void setRandomSource(std::function<void(unsigned char* out, std::size_t count)> source);

/// AES-256-CBC with PKCS#7 padding (a whole block of padding when plain is a multiple of 16), constant-time: no
/// table lookup or branch depends on key or data bytes. Throws std::invalid_argument unless key32 is 32 bytes and
/// iv16 is 16.
KRTU_API std::string aes256CbcEncrypt(std::string_view key32, std::string_view iv16, std::string_view plain);

/// The plaintext of an aes256CbcEncrypt result; std::nullopt when the length is not a positive multiple of 16
/// or the padding is wrong. Not authenticated: callers check a MAC over IV and ciphertext first
/// (encrypt-then-MAC), so a padding answer never reaches an attacker. Throws std::invalid_argument unless key32
/// is 32 bytes and iv16 is 16.
KRTU_API std::optional<std::string> aes256CbcDecrypt(std::string_view key32, std::string_view iv16, std::string_view cipher);
}  // namespace krtu
