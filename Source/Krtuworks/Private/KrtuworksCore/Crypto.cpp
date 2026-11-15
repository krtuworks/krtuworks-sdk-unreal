// The core's cryptographic primitives (see Crypto.h). File-local names carry a sha/hex/base64/random/aes prefix:
// Unreal's unity build pastes several .cpp files of the module into one translation unit.
#include "KrtuworksCore/Crypto.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <random>
#include <stdexcept>
#include <utility>

namespace
{
constexpr std::array<std::uint32_t, 64> shaRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::uint32_t shaRotateRight(std::uint32_t value, int bits) noexcept
{
    return (value >> bits) | (value << (32 - bits));
}

void shaCompress(std::array<std::uint32_t, 8>& state, const std::array<unsigned char, 64>& block) noexcept
{
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
    {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) | (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) | static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i)
    {
        const std::uint32_t sigma0 = shaRotateRight(w[i - 15], 7) ^ shaRotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t sigma1 = shaRotateRight(w[i - 2], 17) ^ shaRotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + sigma0 + w[i - 7] + sigma1;
    }
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (std::size_t i = 0; i < 64; ++i)
    {
        const std::uint32_t bigSigma1 = shaRotateRight(e, 6) ^ shaRotateRight(e, 11) ^ shaRotateRight(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + bigSigma1 + choice + shaRoundConstants[i] + w[i];
        const std::uint32_t bigSigma0 = shaRotateRight(a, 2) ^ shaRotateRight(a, 13) ^ shaRotateRight(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = bigSigma0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

// Incremental SHA-256, so HMAC hashes its pads and the data without joining them into one copy.
class ShaHasher
{
public:
    void update(std::string_view data) noexcept
    {
        totalLength += data.size();
        for (const char byte : data)
        {
            block[blockLength++] = static_cast<unsigned char>(byte);
            if (blockLength == block.size())
            {
                shaCompress(state, block);
                blockLength = 0;
            }
        }
    }

    std::string finish()
    {
        const std::uint64_t bitLength = totalLength * 8;
        block[blockLength++] = 0x80;
        if (blockLength > 56)
        {
            while (blockLength < block.size()) block[blockLength++] = 0;
            shaCompress(state, block);
            blockLength = 0;
        }
        while (blockLength < 56) block[blockLength++] = 0;
        for (std::size_t i = 0; i < 8; ++i) block[56 + i] = static_cast<unsigned char>(bitLength >> (56 - 8 * i));
        shaCompress(state, block);
        std::string digest(32, '\0');
        for (std::size_t i = 0; i < 8; ++i)
        {
            for (std::size_t j = 0; j < 4; ++j)
            {
                digest[4 * i + j] = static_cast<char>(static_cast<unsigned char>(state[i] >> (24 - 8 * j)));
            }
        }
        return digest;
    }

private:
    std::array<std::uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> block{};
    std::size_t blockLength = 0;
    std::uint64_t totalLength = 0;
};

constexpr char hexDigits[] = "0123456789abcdef";

int hexDigitValue(char digit) noexcept
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

constexpr char base64StandardAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char base64UrlAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::uint32_t base64ByteAt(std::string_view bytes, std::size_t index) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]));
}

std::string base64Encode(std::string_view bytes, const char* alphabet, bool pad)
{
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3)
    {
        const std::uint32_t group = (base64ByteAt(bytes, i) << 16) | (base64ByteAt(bytes, i + 1) << 8) | base64ByteAt(bytes, i + 2);
        text.push_back(alphabet[(group >> 18) & 0x3f]);
        text.push_back(alphabet[(group >> 12) & 0x3f]);
        text.push_back(alphabet[(group >> 6) & 0x3f]);
        text.push_back(alphabet[group & 0x3f]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1)
    {
        const std::uint32_t group = base64ByteAt(bytes, i) << 16;
        text.push_back(alphabet[(group >> 18) & 0x3f]);
        text.push_back(alphabet[(group >> 12) & 0x3f]);
        if (pad) text.append("==");
    }
    else if (rest == 2)
    {
        const std::uint32_t group = (base64ByteAt(bytes, i) << 16) | (base64ByteAt(bytes, i + 1) << 8);
        text.push_back(alphabet[(group >> 18) & 0x3f]);
        text.push_back(alphabet[(group >> 12) & 0x3f]);
        text.push_back(alphabet[(group >> 6) & 0x3f]);
        if (pad) text.push_back('=');
    }
    return text;
}

int base64DigitValue(char digit, char digit62, char digit63) noexcept
{
    if (digit >= 'A' && digit <= 'Z') return digit - 'A';
    if (digit >= 'a' && digit <= 'z') return digit - 'a' + 26;
    if (digit >= '0' && digit <= '9') return digit - '0' + 52;
    if (digit == digit62) return 62;
    if (digit == digit63) return 63;
    return -1;
}

// Unused low bits of the last digit are ignored (RFC 4648 §3.5 leaves rejecting them to the decoder).
std::string base64Decode(std::string_view text, char digit62, char digit63)
{
    std::size_t padding = 0;
    while (padding < text.size() && text[text.size() - 1 - padding] == '=') ++padding;
    if (padding > 0 && (padding > 2 || text.size() % 4 != 0)) throw std::invalid_argument("base64: wrong padding");
    const std::size_t digits = text.size() - padding;
    if (digits % 4 == 1) throw std::invalid_argument("base64: no encoding has this length");
    std::string bytes;
    bytes.reserve(digits * 3 / 4);
    std::uint32_t buffer = 0;
    int bufferedBits = 0;
    for (std::size_t i = 0; i < digits; ++i)
    {
        const int value = base64DigitValue(text[i], digit62, digit63);
        if (value < 0) throw std::invalid_argument("base64: character outside the alphabet");
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bufferedBits += 6;
        if (bufferedBits >= 8)
        {
            bufferedBits -= 8;
            bytes.push_back(static_cast<char>(static_cast<unsigned char>(buffer >> bufferedBits)));
        }
    }
    return bytes;
}

using RandomSourceFunction = std::function<void(unsigned char*, std::size_t)>;

struct RandomSourceSlot
{
    std::mutex mutex;
    RandomSourceFunction source;
};

RandomSourceSlot& randomSourceSlot()
{
    static RandomSourceSlot slot;
    return slot;
}

static_assert((std::random_device::min)() == 0 && (std::random_device::max)() >= 0xffffffffu,
    "each std::random_device draw is expected to give 32 random bits");

// Called with the slot's mutex held: std::random_device is not required to be thread-safe.
void randomFromDevice(unsigned char* out, std::size_t count)
{
    static std::random_device device;
    std::size_t filled = 0;
    while (filled < count)
    {
        std::uint32_t word = static_cast<std::uint32_t>(device());
        for (int i = 0; i < 4 && filled < count; ++i)
        {
            out[filled++] = static_cast<unsigned char>(word);
            word >>= 8;
        }
    }
}

// Constant-time AES: no table lookups and no branches on key or data bytes. Each S-box value is computed (the
// inverse in GF(2^8) by a fixed exponentiation, then the affine map) and every reduction is a mask, not a test,
// so the instructions run and the memory addresses touched depend only on the lengths.
constexpr std::size_t aesRounds = 14;
using AesBlock = std::array<unsigned char, 16>;
using AesRoundKeys = std::array<unsigned char, 16 * (aesRounds + 1)>;

// Multiplication by x modulo the AES polynomial x^8 + x^4 + x^3 + x + 1.
unsigned char aesTimesX(unsigned char value) noexcept
{
    const unsigned int widened = value;
    return static_cast<unsigned char>((widened << 1) ^ (0x1bu & (0u - (widened >> 7))));
}

// The product in GF(2^8): eight steps whatever the factor, each bit of it selecting by mask.
unsigned char aesMultiply(unsigned char value, unsigned char factor) noexcept
{
    unsigned int product = 0;
    unsigned int bits = factor;
    for (int step = 0; step < 8; ++step)
    {
        product ^= value & (0u - (bits & 1u));
        value = aesTimesX(value);
        bits >>= 1;
    }
    return static_cast<unsigned char>(product);
}

// value^254 by a fixed addition chain: the inverse of a non-zero value, and 0 for 0, as the S-box needs.
unsigned char aesInverse(unsigned char value) noexcept
{
    const unsigned char power2 = aesMultiply(value, value);
    const unsigned char power3 = aesMultiply(power2, value);
    const unsigned char power6 = aesMultiply(power3, power3);
    const unsigned char power12 = aesMultiply(power6, power6);
    const unsigned char power15 = aesMultiply(power12, power3);
    const unsigned char power30 = aesMultiply(power15, power15);
    const unsigned char power60 = aesMultiply(power30, power30);
    const unsigned char power120 = aesMultiply(power60, power60);
    const unsigned char power126 = aesMultiply(power120, power6);
    const unsigned char power127 = aesMultiply(power126, value);
    return aesMultiply(power127, power127);
}

unsigned char aesRotateLeft(unsigned char value, int bits) noexcept
{
    return static_cast<unsigned char>((value << bits) | (value >> (8 - bits)));
}

// FIPS 197 SubBytes: the inverse, then the affine map.
unsigned char aesSubByte(unsigned char value) noexcept
{
    const unsigned char inverse = aesInverse(value);
    return static_cast<unsigned char>(inverse ^ aesRotateLeft(inverse, 1) ^ aesRotateLeft(inverse, 2) ^ aesRotateLeft(inverse, 3) ^
                                      aesRotateLeft(inverse, 4) ^ 0x63);
}

// InvSubBytes: the inverse affine map, then the inverse.
unsigned char aesInverseSubByte(unsigned char value) noexcept
{
    return aesInverse(static_cast<unsigned char>(aesRotateLeft(value, 1) ^ aesRotateLeft(value, 3) ^ aesRotateLeft(value, 6) ^ 0x05));
}

AesRoundKeys aesExpandKey(std::string_view key32) noexcept
{
    AesRoundKeys roundKeys{};
    for (std::size_t i = 0; i < 32; ++i) roundKeys[i] = static_cast<unsigned char>(key32[i]);
    unsigned char roundConstant = 0x01;
    for (std::size_t i = 32; i < roundKeys.size(); i += 4)
    {
        std::array<unsigned char, 4> word = {roundKeys[i - 4], roundKeys[i - 3], roundKeys[i - 2], roundKeys[i - 1]};
        if (i % 32 == 0)
        {
            const std::array<unsigned char, 4> rotated = {word[1], word[2], word[3], word[0]};
            for (std::size_t j = 0; j < 4; ++j) word[j] = aesSubByte(rotated[j]);
            word[0] = static_cast<unsigned char>(word[0] ^ roundConstant);
            roundConstant = aesTimesX(roundConstant);
        }
        else if (i % 32 == 16)
        {
            for (std::size_t j = 0; j < 4; ++j) word[j] = aesSubByte(word[j]);
        }
        for (std::size_t j = 0; j < 4; ++j) roundKeys[i + j] = static_cast<unsigned char>(roundKeys[i - 32 + j] ^ word[j]);
    }
    return roundKeys;
}

void aesAddRoundKey(AesBlock& state, const AesRoundKeys& roundKeys, std::size_t round) noexcept
{
    for (std::size_t i = 0; i < 16; ++i) state[i] = static_cast<unsigned char>(state[i] ^ roundKeys[16 * round + i]);
}

// The state is column-major, as in FIPS 197: byte (row r, column c) is state[r + 4c].
void aesMixColumns(AesBlock& state) noexcept
{
    for (std::size_t column = 0; column < 4; ++column)
    {
        unsigned char* cell = &state[4 * column];
        const unsigned char a0 = cell[0];
        const unsigned char a1 = cell[1];
        const unsigned char a2 = cell[2];
        const unsigned char a3 = cell[3];
        const auto all = static_cast<unsigned char>(a0 ^ a1 ^ a2 ^ a3);
        cell[0] = static_cast<unsigned char>(a0 ^ all ^ aesTimesX(static_cast<unsigned char>(a0 ^ a1)));
        cell[1] = static_cast<unsigned char>(a1 ^ all ^ aesTimesX(static_cast<unsigned char>(a1 ^ a2)));
        cell[2] = static_cast<unsigned char>(a2 ^ all ^ aesTimesX(static_cast<unsigned char>(a2 ^ a3)));
        cell[3] = static_cast<unsigned char>(a3 ^ all ^ aesTimesX(static_cast<unsigned char>(a3 ^ a0)));
    }
}

void aesInverseMixColumns(AesBlock& state) noexcept
{
    for (std::size_t column = 0; column < 4; ++column)
    {
        unsigned char* cell = &state[4 * column];
        const unsigned char a0 = cell[0];
        const unsigned char a1 = cell[1];
        const unsigned char a2 = cell[2];
        const unsigned char a3 = cell[3];
        cell[0] = static_cast<unsigned char>(aesMultiply(a0, 14) ^ aesMultiply(a1, 11) ^ aesMultiply(a2, 13) ^ aesMultiply(a3, 9));
        cell[1] = static_cast<unsigned char>(aesMultiply(a0, 9) ^ aesMultiply(a1, 14) ^ aesMultiply(a2, 11) ^ aesMultiply(a3, 13));
        cell[2] = static_cast<unsigned char>(aesMultiply(a0, 13) ^ aesMultiply(a1, 9) ^ aesMultiply(a2, 14) ^ aesMultiply(a3, 11));
        cell[3] = static_cast<unsigned char>(aesMultiply(a0, 11) ^ aesMultiply(a1, 13) ^ aesMultiply(a2, 9) ^ aesMultiply(a3, 14));
    }
}

void aesEncryptBlock(AesBlock& state, const AesRoundKeys& roundKeys) noexcept
{
    aesAddRoundKey(state, roundKeys, 0);
    for (std::size_t round = 1; round <= aesRounds; ++round)
    {
        // SubBytes and ShiftRows in one pass: row r moves r columns to the left.
        AesBlock shifted{};
        for (std::size_t column = 0; column < 4; ++column)
        {
            for (std::size_t row = 0; row < 4; ++row) shifted[row + 4 * column] = aesSubByte(state[row + 4 * ((column + row) % 4)]);
        }
        state = shifted;
        if (round != aesRounds) aesMixColumns(state);
        aesAddRoundKey(state, roundKeys, round);
    }
}

void aesDecryptBlock(AesBlock& state, const AesRoundKeys& roundKeys) noexcept
{
    aesAddRoundKey(state, roundKeys, aesRounds);
    for (std::size_t round = aesRounds; round-- > 0;)
    {
        // InvShiftRows and InvSubBytes in one pass: row r moves r columns back to the right.
        AesBlock shifted{};
        for (std::size_t column = 0; column < 4; ++column)
        {
            for (std::size_t row = 0; row < 4; ++row) shifted[row + 4 * ((column + row) % 4)] = aesInverseSubByte(state[row + 4 * column]);
        }
        state = shifted;
        aesAddRoundKey(state, roundKeys, round);
        if (round != 0) aesInverseMixColumns(state);
    }
}

void aesRequireSizes(std::string_view key32, std::string_view iv16)
{
    if (key32.size() != 32) throw std::invalid_argument("AES-256 needs a 32-byte key");
    if (iv16.size() != 16) throw std::invalid_argument("AES-CBC needs a 16-byte IV");
}

AesBlock aesBlockOf(std::string_view bytes, std::size_t offset) noexcept
{
    AesBlock block{};
    for (std::size_t i = 0; i < 16; ++i) block[i] = static_cast<unsigned char>(bytes[offset + i]);
    return block;
}
}  // namespace

namespace krtu
{
std::string sha256(std::string_view data)
{
    ShaHasher hasher;
    hasher.update(data);
    return hasher.finish();
}

std::string hmacSha256(std::string_view key, std::string_view data)
{
    constexpr std::size_t blockSize = 64;
    const std::string hashedKey = key.size() > blockSize ? sha256(key) : std::string();
    const std::string_view blockKey = key.size() > blockSize ? std::string_view(hashedKey) : key;
    std::string innerPad(blockSize, '\x36');
    std::string outerPad(blockSize, '\x5c');
    for (std::size_t i = 0; i < blockKey.size(); ++i)
    {
        const auto keyByte = static_cast<unsigned char>(blockKey[i]);
        innerPad[i] = static_cast<char>(keyByte ^ 0x36u);
        outerPad[i] = static_cast<char>(keyByte ^ 0x5cu);
    }
    ShaHasher inner;
    inner.update(innerPad);
    inner.update(data);
    const std::string innerDigest = inner.finish();
    ShaHasher outer;
    outer.update(outerPad);
    outer.update(innerDigest);
    return outer.finish();
}

std::string toHex(std::string_view bytes)
{
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const char byte : bytes)
    {
        const auto value = static_cast<unsigned char>(byte);
        hex.push_back(hexDigits[value >> 4]);
        hex.push_back(hexDigits[value & 0x0f]);
    }
    return hex;
}

std::string fromHex(std::string_view hex)
{
    if (hex.size() % 2 != 0) throw std::invalid_argument("hex: odd length");
    std::string bytes(hex.size() / 2, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        const int high = hexDigitValue(hex[2 * i]);
        const int low = hexDigitValue(hex[2 * i + 1]);
        if (high < 0 || low < 0) throw std::invalid_argument("hex: not a hex digit");
        bytes[i] = static_cast<char>((high << 4) | low);
    }
    return bytes;
}

std::string base64(std::string_view bytes)
{
    return base64Encode(bytes, base64StandardAlphabet, true);
}

std::string fromBase64(std::string_view text)
{
    return base64Decode(text, '+', '/');
}

std::string base64Url(std::string_view bytes)
{
    return base64Encode(bytes, base64UrlAlphabet, false);
}

std::string fromBase64Url(std::string_view text)
{
    return base64Decode(text, '-', '_');
}

bool fixedTimeEquals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) return false;
    unsigned int difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        difference |= static_cast<unsigned int>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    }
    return difference == 0;
}

std::string randomBytes(std::size_t count)
{
    std::string bytes(count, '\0');
    if (count == 0) return bytes;
    RandomSourceSlot& slot = randomSourceSlot();
    const std::lock_guard<std::mutex> guard(slot.mutex);
    auto* out = reinterpret_cast<unsigned char*>(&bytes[0]);
    if (slot.source)
    {
        slot.source(out, count);
    }
    else
    {
        randomFromDevice(out, count);
    }
    return bytes;
}

void setRandomSource(std::function<void(unsigned char* out, std::size_t count)> source)
{
    RandomSourceSlot& slot = randomSourceSlot();
    const std::lock_guard<std::mutex> guard(slot.mutex);
    slot.source = std::move(source);
}

std::string aes256CbcEncrypt(std::string_view key32, std::string_view iv16, std::string_view plain)
{
    aesRequireSizes(key32, iv16);
    const AesRoundKeys roundKeys = aesExpandKey(key32);
    const std::size_t padding = 16 - plain.size() % 16;
    std::string cipher(plain.size() + padding, '\0');
    AesBlock chain = aesBlockOf(iv16, 0);
    for (std::size_t offset = 0; offset < cipher.size(); offset += 16)
    {
        for (std::size_t i = 0; i < 16; ++i)
        {
            const std::size_t at = offset + i;
            const auto byte = at < plain.size() ? static_cast<unsigned char>(plain[at]) : static_cast<unsigned char>(padding);
            chain[i] = static_cast<unsigned char>(chain[i] ^ byte);
        }
        aesEncryptBlock(chain, roundKeys);
        for (std::size_t i = 0; i < 16; ++i) cipher[offset + i] = static_cast<char>(chain[i]);
    }
    return cipher;
}

std::optional<std::string> aes256CbcDecrypt(std::string_view key32, std::string_view iv16, std::string_view cipher)
{
    aesRequireSizes(key32, iv16);
    if (cipher.empty() || cipher.size() % 16 != 0) return std::nullopt;
    const AesRoundKeys roundKeys = aesExpandKey(key32);
    std::string plain(cipher.size(), '\0');
    AesBlock chain = aesBlockOf(iv16, 0);
    for (std::size_t offset = 0; offset < cipher.size(); offset += 16)
    {
        const AesBlock encrypted = aesBlockOf(cipher, offset);
        AesBlock block = encrypted;
        aesDecryptBlock(block, roundKeys);
        for (std::size_t i = 0; i < 16; ++i) plain[offset + i] = static_cast<char>(block[i] ^ chain[i]);
        chain = encrypted;
    }
    // PKCS#7: 1 <= padding <= 16 and the last `padding` bytes equal to it. All 16 bytes of the last block are
    // checked with masks; only the verdict, which the caller sees anyway, is branched on.
    const unsigned int padding = static_cast<unsigned char>(plain.back());
    unsigned int wrong = ((padding - 1u) >> 8) | ((16u - padding) >> 8);
    for (std::size_t i = 1; i <= 16; ++i)
    {
        const unsigned int byte = static_cast<unsigned char>(plain[plain.size() - i]);
        const unsigned int inPadding = (static_cast<unsigned int>(i) - 1u - padding) >> 8;
        wrong |= inPadding & (byte ^ padding);
    }
    if (wrong != 0) return std::nullopt;
    plain.resize(plain.size() - padding);
    return plain;
}
}  // namespace krtu
