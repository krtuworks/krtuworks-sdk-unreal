// ECDSA on P-256 with SHA-256 in plain standard C++: no crypto library is available on every platform the core
// runs on, and a device key must sign the same everywhere. Nonces are deterministic (RFC 6979), so no platform
// random source is trusted for them; the signature is the raw r||s JWS (ES256) wants. Not constant-time: the key
// only guards a guest account on the player's own device. Keys and coordinates are 32-byte big-endian strings.
#pragma once

#include "KrtuworksCore/Export.h"

#include <string>
#include <string_view>
#include <utility>

namespace krtu::p256
{
/// True when d32 is a private scalar: exactly 32 bytes with 1 <= d <= n-1.
bool validPrivateKey(std::string_view d32) noexcept;

/// A new private scalar from krtu::randomBytes, drawn again until it is valid.
KRTU_API std::string newPrivateKey();

/// The public point of private scalar d32, as affine x and y. Throws std::invalid_argument on an invalid key.
KRTU_API std::pair<std::string, std::string> publicKey(std::string_view d32);

/// Signs SHA-256(message) with d32: r||s, 64 bytes. The nonce comes from the key and the digest (RFC 6979 with
/// HMAC-SHA256), so a weak or repeating random source can never reveal the key. Throws std::invalid_argument on an
/// invalid key.
KRTU_API std::string sign(std::string_view d32, std::string_view message);

/// Checks an r||s signature of SHA-256(message) by the point (x32, y32). False for a point not on the curve, r or
/// s outside [1, n-1], a wrong length or a bad signature. Not named `verify`: Unreal defines a macro of that name.
bool verifySignature(std::string_view x32, std::string_view y32, std::string_view message, std::string_view signature64) noexcept;
}  // namespace krtu::p256
