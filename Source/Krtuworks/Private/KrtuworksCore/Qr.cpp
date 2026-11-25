#include "KrtuworksCore/Qr.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace krtu
{
namespace
{
using Bytes = std::vector<int>;

// Error correction codewords per block and the number of blocks, level M, versions 1..10.
constexpr int ecPerBlock[] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
constexpr int blockCount[] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
constexpr int maxVersion = 10;
// Format information's two bits for level M.
constexpr int levelM = 0;

int rawDataModules(int version)
{
    int result = (16 * version + 128) * version + 64;
    if (version >= 2)
    {
        const int align = version / 7 + 2;
        result -= (25 * align - 10) * align - 55;
        if (version >= 7) result -= 36;
    }
    return result;
}

int totalCodewords(int version)
{
    return rawDataModules(version) / 8;
}

int dataCodewords(int version)
{
    return totalCodewords(version) - ecPerBlock[version] * blockCount[version];
}

int gfMultiply(int x, int y)
{
    int z = 0;
    for (int i = 7; i >= 0; --i)
    {
        z = (z << 1) ^ ((z >> 7) * 0x11d);
        z ^= ((y >> i) & 1) * x;
    }
    return z & 0xff;
}

Bytes rsDivisor(int degree)
{
    Bytes result(static_cast<std::size_t>(degree), 0);
    result[static_cast<std::size_t>(degree - 1)] = 1;
    int root = 1;
    for (int i = 0; i < degree; ++i)
    {
        for (int j = 0; j < degree; ++j)
        {
            const auto at = static_cast<std::size_t>(j);
            result[at] = gfMultiply(result[at], root);
            if (j + 1 < degree) result[at] ^= result[at + 1];
        }
        root = gfMultiply(root, 0x02);
    }
    return result;
}

Bytes rsRemainder(const Bytes& data, const Bytes& divisor)
{
    Bytes result(divisor.size(), 0);
    for (const int byte : data)
    {
        const int factor = byte ^ result.front();
        result.erase(result.begin());
        result.push_back(0);
        for (std::size_t i = 0; i < divisor.size(); ++i) result[i] ^= gfMultiply(divisor[i], factor);
    }
    return result;
}

/// The data codewords: mode, count, bytes, terminator, then the 0xEC/0x11 padding to the version's capacity.
Bytes dataBytes(const std::string& text, int version)
{
    std::vector<int> bits;
    const auto put = [&bits](int value, int length) {
        for (int i = length - 1; i >= 0; --i) bits.push_back((value >> i) & 1);
    };
    put(0x4, 4);
    put(static_cast<int>(text.size()), version <= 9 ? 8 : 16);
    for (const char c : text) put(static_cast<unsigned char>(c), 8);
    const int capacity = dataCodewords(version) * 8;
    put(0, (std::min)(4, capacity - static_cast<int>(bits.size())));
    put(0, (8 - static_cast<int>(bits.size()) % 8) % 8);
    Bytes out;
    for (std::size_t i = 0; i < bits.size(); i += 8)
    {
        int value = 0;
        for (std::size_t k = 0; k < 8; ++k) value = (value << 1) | bits[i + k];
        out.push_back(value);
    }
    for (int pad = 0xec; static_cast<int>(out.size()) < dataCodewords(version); pad ^= 0xec ^ 0x11) out.push_back(pad);
    return out;
}

/// Splits the data into the version's blocks, adds each block's error correction and interleaves them.
Bytes codewords(const Bytes& data, int version)
{
    const int blocks = blockCount[version];
    const int ecLength = ecPerBlock[version];
    const int total = totalCodewords(version);
    const int shortBlocks = blocks - total % blocks;
    const int shortLength = total / blocks;
    const Bytes divisor = rsDivisor(ecLength);
    struct Block
    {
        Bytes data;
        Bytes ec;
    };
    std::vector<Block> split;
    std::size_t at = 0;
    for (int i = 0; i < blocks; ++i)
    {
        const auto length = static_cast<std::size_t>(shortLength - ecLength + (i < shortBlocks ? 0 : 1));
        Block block;
        block.data.assign(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        block.ec = rsRemainder(block.data, divisor);
        split.push_back(std::move(block));
    }
    Bytes out;
    for (int i = 0; i <= shortLength - ecLength; ++i)
    {
        for (const Block& block : split)
        {
            if (static_cast<std::size_t>(i) < block.data.size()) out.push_back(block.data[static_cast<std::size_t>(i)]);
        }
    }
    for (int i = 0; i < ecLength; ++i)
    {
        for (const Block& block : split) out.push_back(block.ec[static_cast<std::size_t>(i)]);
    }
    return out;
}

class Matrix
{
public:
    explicit Matrix(int sizeIn)
        : size(sizeIn), dark(static_cast<std::size_t>(sizeIn), std::vector<bool>(static_cast<std::size_t>(sizeIn), false)), fixed(dark)
    {
    }

    void set(int x, int y, bool value)
    {
        dark[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = value;
        fixed[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = true;
    }

    int size;
    std::vector<std::vector<bool>> dark;
    std::vector<std::vector<bool>> fixed;
};

std::vector<int> alignmentPositions(int version, int size)
{
    if (version == 1) return {};
    const int count = version / 7 + 2;
    const int step = ((version * 4 + 4 + (count * 2 - 2) - 1) / (count * 2 - 2)) * 2;
    std::vector<int> out{6};
    for (int position = size - 7; static_cast<int>(out.size()) < count; position -= step) out.insert(out.begin() + 1, position);
    return out;
}

void drawFormat(Matrix& m, int mask)
{
    const int data = (levelM << 3) | mask;
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = ((data << 10) | rem) ^ 0x5412;
    const auto bit = [bits](int i) { return ((bits >> i) & 1) == 1; };
    for (int i = 0; i <= 5; ++i) m.set(8, i, bit(i));
    m.set(8, 7, bit(6));
    m.set(8, 8, bit(7));
    m.set(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) m.set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) m.set(m.size - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) m.set(8, m.size - 15 + i, bit(i));
    m.set(8, m.size - 8, true);
}

void drawFunctionPatterns(Matrix& m, int version)
{
    const int size = m.size;
    for (int i = 0; i < size; ++i)
    {
        m.set(6, i, i % 2 == 0);
        m.set(i, 6, i % 2 == 0);
    }
    const int centres[3][2] = {{3, 3}, {size - 4, 3}, {3, size - 4}};
    for (const auto& centre : centres)
    {
        for (int dy = -4; dy <= 4; ++dy)
        {
            for (int dx = -4; dx <= 4; ++dx)
            {
                const int x = centre[0] + dx;
                const int y = centre[1] + dy;
                const int distance = (std::max)(std::abs(dx), std::abs(dy));
                if (x >= 0 && x < size && y >= 0 && y < size) m.set(x, y, distance != 2 && distance != 4);
            }
        }
    }
    const std::vector<int> positions = alignmentPositions(version, size);
    const std::size_t last = positions.empty() ? 0 : positions.size() - 1;
    for (std::size_t i = 0; i < positions.size(); ++i)
    {
        for (std::size_t j = 0; j < positions.size(); ++j)
        {
            if ((i == 0 && j == 0) || (i == 0 && j == last) || (i == last && j == 0)) continue;
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx) m.set(positions[i] + dx, positions[j] + dy, (std::max)(std::abs(dx), std::abs(dy)) != 1);
            }
        }
    }
    drawFormat(m, 0);
    if (version >= 7)
    {
        int rem = version;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
        const int bits = (version << 12) | rem;
        for (int i = 0; i < 18; ++i)
        {
            const bool dark = ((bits >> i) & 1) == 1;
            const int a = size - 11 + i % 3;
            const int b = i / 3;
            m.set(a, b, dark);
            m.set(b, a, dark);
        }
    }
}

void drawData(Matrix& m, const Bytes& data)
{
    std::size_t i = 0;
    for (int right = m.size - 1; right >= 1; right -= 2)
    {
        if (right == 6) right = 5;
        for (int vertical = 0; vertical < m.size; ++vertical)
        {
            for (int j = 0; j < 2; ++j)
            {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? m.size - 1 - vertical : vertical;
                const auto row = static_cast<std::size_t>(y);
                const auto col = static_cast<std::size_t>(x);
                if (!m.fixed[row][col] && i < data.size() * 8)
                {
                    m.dark[row][col] = ((data[i >> 3] >> (7 - static_cast<int>(i & 7))) & 1) == 1;
                    ++i;
                }
            }
        }
    }
}

bool masked(int mask, int x, int y)
{
    switch (mask)
    {
    case 0:
        return (x + y) % 2 == 0;
    case 1:
        return y % 2 == 0;
    case 2:
        return x % 3 == 0;
    case 3:
        return (x + y) % 3 == 0;
    case 4:
        return (x / 3 + y / 2) % 2 == 0;
    case 5:
        return (x * y) % 2 + (x * y) % 3 == 0;
    case 6:
        return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
    default:
        return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
    }
}

void applyMask(Matrix& m, int mask)
{
    for (int y = 0; y < m.size; ++y)
    {
        for (int x = 0; x < m.size; ++x)
        {
            const auto row = static_cast<std::size_t>(y);
            const auto col = static_cast<std::size_t>(x);
            if (!m.fixed[row][col] && masked(mask, x, y)) m.dark[row][col] = !m.dark[row][col];
        }
    }
}

constexpr bool finderLike[2][11] = {
    {true, false, true, true, true, false, true, false, false, false, false},
    {false, false, false, false, true, false, true, true, true, false, true},
};

/// The standard's four penalty rules; the mask with the lowest score is drawn.
int penalty(const std::vector<std::vector<bool>>& dark)
{
    const int size = static_cast<int>(dark.size());
    const auto at = [&dark](int x, int y, bool transpose) {
        return transpose ? static_cast<bool>(dark[static_cast<std::size_t>(x)][static_cast<std::size_t>(y)]) : static_cast<bool>(dark[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)]);
    };
    int score = 0;
    for (const bool transpose : {false, true})
    {
        for (int y = 0; y < size; ++y)
        {
            int run = 1;
            for (int x = 1; x <= size; ++x)
            {
                if (x < size && at(x, y, transpose) == at(x - 1, y, transpose))
                {
                    ++run;
                    continue;
                }
                if (run >= 5) score += 3 + (run - 5);
                run = 1;
            }
            for (int x = 0; x + 11 <= size; ++x)
            {
                for (const auto& pattern : finderLike)
                {
                    bool same = true;
                    for (int k = 0; k < 11 && same; ++k) same = at(x + k, y, transpose) == pattern[k];
                    if (same) score += 40;
                }
            }
        }
    }
    for (int y = 0; y + 1 < size; ++y)
    {
        for (int x = 0; x + 1 < size; ++x)
        {
            const bool c = dark[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            if (c == dark[static_cast<std::size_t>(y)][static_cast<std::size_t>(x + 1)] && c == dark[static_cast<std::size_t>(y + 1)][static_cast<std::size_t>(x)] &&
                c == dark[static_cast<std::size_t>(y + 1)][static_cast<std::size_t>(x + 1)])
                score += 3;
        }
    }
    int darkCount = 0;
    for (const auto& row : dark) darkCount += static_cast<int>(std::count(row.begin(), row.end(), true));
    const int total = size * size;
    score += ((std::abs(darkCount * 20 - total * 10) + total - 1) / total - 1) * 10;
    return score;
}
}  // namespace

std::vector<std::vector<bool>> qrModules(const std::string& text, int mask)
{
    if (mask < -1 || mask > 7) throw std::invalid_argument("a QR mask is 0 to 7");
    int version = 1;
    while (version <= maxVersion && (static_cast<int>(text.size()) + (version <= 9 ? 2 : 3)) * 8 - 4 > dataCodewords(version) * 8) ++version;
    if (version > maxVersion) throw std::length_error(std::to_string(text.size()) + " bytes do not fit a version " + std::to_string(maxVersion) + " QR code");
    const Bytes data = codewords(dataBytes(text, version), version);
    const int size = version * 4 + 17;
    std::vector<std::vector<bool>> best;
    int bestScore = (std::numeric_limits<int>::max)();
    for (int candidate = 0; candidate < 8; ++candidate)
    {
        if (mask != -1 && candidate != mask) continue;
        Matrix m(size);
        drawFunctionPatterns(m, version);
        drawData(m, data);
        applyMask(m, candidate);
        drawFormat(m, candidate);
        const int score = mask == -1 ? penalty(m.dark) : 0;
        if (score < bestScore)
        {
            best = m.dark;
            bestScore = score;
        }
    }
    return best;
}
}  // namespace krtu
