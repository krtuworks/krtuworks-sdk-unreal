// Deflate (RFC 1951): LZ77 over a 32 KiB window with hash chains and one-step lazy matching, then each
// block coded as the smallest of stored, fixed Huffman and dynamic Huffman. Gzip (RFC 1952) wraps it.
#include "KrtuworksCore/Gzip.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace krtu
{
namespace
{
constexpr std::array<std::uint32_t, 256> gzipMakeCrcTable() noexcept
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n)
    {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k)
        {
            c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> gzipCrcTable = gzipMakeCrcTable();

// ID1 ID2, CM = deflate, FLG = no name/comment/extra, MTIME = 0 (none), XFL = 0, OS = 255 (unknown).
constexpr std::array<unsigned char, 10> gzipHeader = {{0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}};

constexpr std::size_t deflateWindow = 32768;
constexpr std::size_t deflateMinMatch = 3;
constexpr std::size_t deflateMaxMatch = 258;
// Chains link positions whose next 4 bytes hash alike, not 3: a match of only 3 bytes saves a few bits at
// best, and leaving those candidates out keeps the chains to ones likely to match longer, so the same search
// budget goes deeper. On event JSON that gave a better ratio and a third less time than a 3-byte hash (on
// binaries the ratio is ~2% worse; the SDK compresses only JSON).
constexpr std::size_t deflateHashBytes = 4;
constexpr unsigned deflateHashBits = 15;
constexpr std::size_t deflateHashSize = std::size_t{1} << deflateHashBits;
// The hash table and chains hold positions modulo 2^32 (half the cache of size_t); an empty entry sits one
// byte beyond the window from position 0, so it ends every chain at least until the input passes 4 GiB.
constexpr std::uint32_t deflateNoPosition = static_cast<std::uint32_t>((std::numeric_limits<std::uint32_t>::max)() - deflateWindow);

// Search effort: zlib level 6's chain and good length, but always looking one byte ahead and searching on
// until the longest possible match (zlib level 9's lazy and nice lengths). Measured on event JSON: ~6%
// smaller than zlib level 6's settings for ~5% more time.
constexpr std::size_t deflateMaxChain = 128;              // candidates tried per position
constexpr std::size_t deflateGoodLength = 8;              // holding a match this long, the look ahead tries a quarter
constexpr std::size_t deflateLazyLength = deflateMaxMatch;  // a match this long is taken without looking ahead
constexpr std::size_t deflateNiceLength = deflateMaxMatch;  // a match this long ends the search
// A 3-byte match farther back than this costs more bits (its distance's extra bits) than its three literals.
constexpr std::size_t deflateTooFar = 4096;

constexpr std::size_t deflateStoredMax = 65535;
// Input per block. One stored block holds 65535 bytes. A block is cut once it covers this much, and the
// step that crosses the mark adds less than 2 x 258 bytes (lazy literals, each for a longer match, then the
// match), so incompressible input costs one 5-byte stored header per block.
constexpr std::size_t deflateBlockInput = deflateStoredMax - 2 * deflateMaxMatch;

constexpr std::size_t deflateLiteralCodes = 286;  // 0-255 bytes, 256 end of block, 257-285 match lengths
constexpr std::size_t deflateFixedLiteralCodes = 288;  // the fixed code also assigns 286 and 287 (never sent)
constexpr std::size_t deflateDistanceCodes = 30;
constexpr std::size_t deflateCodeLengthCodes = 19;
constexpr std::size_t deflateEndOfBlock = 256;
constexpr unsigned deflateMaxBits = 15;
constexpr unsigned deflateMaxCodeLengthBits = 7;

constexpr std::array<std::uint16_t, 29> deflateLengthBase = {
    {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258}};
constexpr std::array<std::uint8_t, 29> deflateLengthExtra = {
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0}};
constexpr std::array<std::uint16_t, 30> deflateDistanceBase = {{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                                                513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577}};
constexpr std::array<std::uint8_t, 30> deflateDistanceExtra = {
    {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13}};
// The order the code length code's lengths are sent in (RFC 1951 3.2.7).
constexpr std::array<std::uint8_t, 19> deflateCodeLengthOrder = {{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15}};

/// Match length (3-258) to its index in deflateLengthBase.
constexpr std::array<std::uint8_t, 259> deflateMakeLengthIndex() noexcept
{
    std::array<std::uint8_t, 259> index{};
    for (std::size_t code = 0; code < deflateLengthBase.size(); ++code)
    {
        const std::size_t first = deflateLengthBase[code];
        const std::size_t last = (std::min)(first + (std::size_t{1} << deflateLengthExtra[code]) - 1, std::size_t{257});
        for (std::size_t length = first; length <= last; ++length)
        {
            index[length] = static_cast<std::uint8_t>(code);
        }
    }
    // 258 has a code of its own (285); 284's five extra bits could spell it too, but RFC 1951 stops 284 at 257.
    index[258] = 28;
    return index;
}

/// Distance d's code: index[d - 1] up to 256, index[256 + ((d - 1) >> 7)] beyond (from distance 257 on, every
/// code spans whole multiples of 128), the 512-entry table zlib uses instead of one entry per distance.
constexpr std::array<std::uint8_t, 512> deflateMakeDistanceIndex() noexcept
{
    std::array<std::uint8_t, 512> index{};
    for (std::size_t code = 0; code < deflateDistanceBase.size(); ++code)
    {
        const std::size_t first = std::size_t{deflateDistanceBase[code]} - 1;
        const std::size_t end = first + (std::size_t{1} << deflateDistanceExtra[code]);
        if (first < 256)
        {
            for (std::size_t d = first; d < end; ++d) index[d] = static_cast<std::uint8_t>(code);
        }
        else
        {
            for (std::size_t d = first >> 7; d < end >> 7; ++d) index[256 + d] = static_cast<std::uint8_t>(code);
        }
    }
    return index;
}

constexpr std::array<std::uint8_t, 259> deflateLengthIndex = deflateMakeLengthIndex();
constexpr std::array<std::uint8_t, 512> deflateDistanceIndex = deflateMakeDistanceIndex();

std::size_t deflateDistanceCode(std::size_t distance) noexcept
{
    const std::size_t d = distance - 1;
    return d < 256 ? deflateDistanceIndex[d] : deflateDistanceIndex[256 + (d >> 7)];
}

/// 8 bytes at `p` as one integer, in whatever byte order: it is only compared for equality.
std::uint64_t deflateLoad64(const unsigned char* p) noexcept
{
    std::uint64_t value = 0;
    std::memcpy(&value, p, sizeof value);
    return value;
}

/// Hash of the deflateHashBytes (4) bytes at `p` (multiplicative, top bits kept).
std::size_t deflateHash(const unsigned char* p) noexcept
{
    const std::uint32_t v = std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
    return static_cast<std::size_t>(static_cast<std::uint32_t>(v * 2654435761u) >> (32 - deflateHashBits));
}

/// Code lengths of a prefix code for `freqs` with no code longer than `maxBits`: Huffman's, with the
/// weights halved (a used symbol keeps at least 1) and the tree rebuilt until the deepest code fits. Equal
/// weights give a tree ceil(log2(count)) deep, so it always ends for deflate's alphabets (286 symbols in 15
/// bits, 19 in 7). The code is always complete: a lone used symbol would get an incomplete one, which
/// zlib-family decoders refuse, so the lowest unused symbols join with weight 0 (they are never sent).
void deflateCodeLengths(const std::uint32_t* freqs, std::size_t count, unsigned maxBits, std::uint8_t* lengths)
{
    std::vector<std::size_t> symbols;
    for (std::size_t s = 0; s < count; ++s)
    {
        lengths[s] = 0;
        if (freqs[s] != 0) symbols.push_back(s);
    }
    for (std::size_t s = 0; symbols.size() < 2 && s < count; ++s)
    {
        if (freqs[s] == 0) symbols.push_back(s);
    }
    const std::size_t leaves = symbols.size();
    std::vector<std::uint64_t> weights(leaves);
    for (std::size_t i = 0; i < leaves; ++i) weights[i] = freqs[symbols[i]];

    std::vector<std::size_t> order(leaves);
    std::vector<std::uint64_t> nodeWeight(2 * leaves - 1);
    std::vector<std::size_t> parent(2 * leaves - 1);
    std::vector<unsigned> depth(2 * leaves - 1);
    for (;;)
    {
        for (std::size_t i = 0; i < leaves; ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b)
                  {
                      return weights[a] != weights[b] ? weights[a] < weights[b] : symbols[a] < symbols[b];
                  });
        for (std::size_t i = 0; i < leaves; ++i) nodeWeight[i] = weights[order[i]];

        // Two queues: leaves sorted by weight, and inner nodes, which are made in increasing weight.
        std::size_t nextLeaf = 0;
        std::size_t nextNode = leaves;
        for (std::size_t made = leaves; made < 2 * leaves - 1; ++made)
        {
            std::array<std::size_t, 2> pick{};
            for (std::size_t& p : pick)
            {
                if (nextLeaf < leaves && (nextNode == made || nodeWeight[nextLeaf] <= nodeWeight[nextNode])) p = nextLeaf++;
                else p = nextNode++;
            }
            nodeWeight[made] = nodeWeight[pick[0]] + nodeWeight[pick[1]];
            parent[pick[0]] = made;
            parent[pick[1]] = made;
        }
        // Every parent comes after its children, so one pass down from the root gives every depth.
        depth[2 * leaves - 2] = 0;
        unsigned deepest = 0;
        for (std::size_t i = 2 * leaves - 2; i-- > 0;)
        {
            depth[i] = depth[parent[i]] + 1;
            if (i < leaves) deepest = (std::max)(deepest, depth[i]);
        }
        if (deepest <= maxBits)
        {
            for (std::size_t i = 0; i < leaves; ++i) lengths[symbols[order[i]]] = static_cast<std::uint8_t>(depth[i]);
            return;
        }
        for (std::uint64_t& w : weights) w = (w + 1) / 2;
    }
}

/// Canonical codes for `lengths` (RFC 1951 3.2.2), bit-reversed: deflate sends a Huffman code from its most
/// significant bit, and the bit writer packs from the least significant.
void deflateCanonicalCodes(const std::uint8_t* lengths, std::size_t count, std::uint16_t* codes)
{
    std::array<std::uint32_t, 16> lengthCount{};
    for (std::size_t s = 0; s < count; ++s) ++lengthCount[lengths[s]];
    lengthCount[0] = 0;
    std::array<std::uint32_t, 16> next{};
    std::uint32_t code = 0;
    for (std::size_t bits = 1; bits < 16; ++bits)
    {
        code = (code + lengthCount[bits - 1]) << 1;
        next[bits] = code;
    }
    for (std::size_t s = 0; s < count; ++s)
    {
        const unsigned length = lengths[s];
        std::uint32_t value = length == 0 ? 0 : next[length]++;
        std::uint32_t reversed = 0;
        for (unsigned k = 0; k < length; ++k)
        {
            reversed = (reversed << 1) | (value & 1u);
            value >>= 1;
        }
        codes[s] = static_cast<std::uint16_t>(reversed);
    }
}

/// One entry of the run-length coded lengths in a dynamic block header.
struct DeflateRun
{
    std::uint8_t symbol;  // 0-15 a length, 16 the previous length 3-6 times, 17 zero 3-10 times, 18 zero 11-138 times
    std::uint8_t extra;   // the repeat count minus its minimum
};

constexpr std::array<std::uint8_t, 19> deflateRunExtraBits = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7}};

/// The code lengths of a dynamic block header, run-length coded with symbols 16-18 (RFC 1951 3.2.7).
void deflateRunLengths(const std::uint8_t* lengths, std::size_t count, std::vector<DeflateRun>& runs)
{
    runs.clear();
    std::size_t i = 0;
    while (i < count)
    {
        const std::uint8_t value = lengths[i];
        std::size_t run = 1;
        while (i + run < count && lengths[i + run] == value) ++run;
        i += run;
        if (value == 0)
        {
            while (run >= 11)
            {
                const std::size_t take = (std::min)(run, std::size_t{138});
                runs.push_back({18, static_cast<std::uint8_t>(take - 11)});
                run -= take;
            }
            if (run >= 3)
            {
                runs.push_back({17, static_cast<std::uint8_t>(run - 3)});
                run = 0;
            }
        }
        else
        {
            runs.push_back({value, 0});
            --run;
            while (run >= 3)
            {
                const std::size_t take = (std::min)(run, std::size_t{6});
                runs.push_back({16, static_cast<std::uint8_t>(take - 3)});
                run -= take;
            }
        }
        for (; run > 0; --run) runs.push_back({value, 0});
    }
}

/// Bits into the output, least significant first, as deflate packs them.
class DeflateBitWriter
{
public:
    explicit DeflateBitWriter(std::string& target) : out(target) {}

    void put(std::uint32_t value, unsigned count)
    {
        pending |= std::uint64_t{value} << held;
        held += count;
        while (held >= 8)
        {
            out.push_back(static_cast<char>(pending & 0xFFu));
            pending >>= 8;
            held -= 8;
        }
    }

    /// Zero bits up to the next byte boundary, where a stored block's length and bytes start.
    void align()
    {
        if (held > 0) put(0, 8 - held);
    }

    /// Raw bytes, after align().
    void bytes(const unsigned char* data, std::size_t count) { out.append(reinterpret_cast<const char*>(data), count); }

    unsigned heldBits() const noexcept { return held; }

private:
    std::string& out;
    std::uint64_t pending = 0;
    unsigned held = 0;
};

/// One LZ77 output: a literal byte or a match.
struct DeflateToken
{
    std::uint16_t length;  // 0 for a literal, else the match length (3-258)
    std::uint16_t value;   // the literal byte, or the match distance (1-32768)
};

struct DeflateMatch
{
    std::size_t length;
    std::size_t distance;
};

/// A prefix code as the writer needs it: a length and a bit-reversed code per symbol.
struct DeflateCode
{
    std::vector<std::uint8_t> lengths;
    std::vector<std::uint16_t> codes;

    void build()
    {
        codes.assign(lengths.size(), 0);
        deflateCanonicalCodes(lengths.data(), lengths.size(), codes.data());
    }
};

/// LZ77 over the whole input (it is all in memory: the window is simply the 32 KiB before each position),
/// cut into blocks of about 64 KiB of input that are each coded on their own.
class DeflateEncoder
{
public:
    DeflateEncoder(std::string_view input, std::string& output)
        : data(reinterpret_cast<const unsigned char*>(input.data())),
          size(input.size()),
          writer(output),
          head(deflateHashSize, deflateNoPosition),
          prev(deflateWindow, deflateNoPosition)
    {
        // The fixed code (RFC 1951 3.2.6) is the canonical code of these lengths.
        fixedLiterals.lengths.assign(deflateFixedLiteralCodes, 8);
        std::fill(fixedLiterals.lengths.begin() + 144, fixedLiterals.lengths.begin() + 256, std::uint8_t{9});
        std::fill(fixedLiterals.lengths.begin() + 256, fixedLiterals.lengths.begin() + 280, std::uint8_t{7});
        fixedLiterals.build();
        fixedDistances.lengths.assign(deflateDistanceCodes, 5);
        fixedDistances.build();
        // A block covers at most 65535 input bytes (see deflateBlockInput), so it never holds more tokens.
        tokens.reserve(deflateStoredMax);
    }

    /// Writes the whole input as deflate blocks, the last one marked final, and pads the last byte.
    void run()
    {
        std::size_t pos = 0;
        while (pos < size)
        {
            DeflateMatch found = find(pos, 0, deflateMaxChain);
            insert(pos);
            // Lazy matching: when the next position has a longer match, this byte goes out as a literal and
            // that match is weighed against the one after it in turn.
            while (found.length != 0 && found.length < deflateLazyLength)
            {
                const std::size_t chain = found.length >= deflateGoodLength ? deflateMaxChain / 4 : deflateMaxChain;
                const DeflateMatch ahead = find(pos + 1, found.length, chain);
                if (ahead.length == 0) break;
                addLiteral(pos);
                ++pos;
                insert(pos);
                found = ahead;
            }
            if (found.length != 0)
            {
                tokens.push_back({static_cast<std::uint16_t>(found.length), static_cast<std::uint16_t>(found.distance)});
                for (std::size_t p = pos + 1; p < pos + found.length; ++p) insert(p);
                pos += found.length;
            }
            else
            {
                addLiteral(pos);
                ++pos;
            }
            if (pos < size && pos - blockStart >= deflateBlockInput) writeBlock(pos, false);
        }
        writeBlock(size, true);
        writer.align();
    }

private:
    void addLiteral(std::size_t pos) { tokens.push_back({0, data[pos]}); }

    void insert(std::size_t pos)
    {
        if (size - pos < deflateHashBytes) return;
        const std::size_t h = deflateHash(data + pos);
        prev[pos & (deflateWindow - 1)] = head[h];
        head[h] = static_cast<std::uint32_t>(pos);
    }

    /// The longest match for `pos` longer than `beat` (length 0 if none), walking at most `chain` earlier
    /// positions with the same hash, nearest first. Every candidate is compared byte by byte at
    /// pos - distance, so a stale, colliding or wrapped chain entry can only cost time, never correctness.
    DeflateMatch find(std::size_t pos, std::size_t beat, std::size_t chain) const noexcept
    {
        DeflateMatch best{0, 0};
        if (size - pos < deflateHashBytes) return best;
        const std::size_t limit = (std::min)(deflateMaxMatch, size - pos);
        std::size_t bestLength = (std::max)(beat, deflateMinMatch - 1);
        if (bestLength >= limit) return best;
        const unsigned char* here = data + pos;
        std::uint32_t candidate = head[deflateHash(here)];
        std::size_t previousDistance = 0;
        for (std::size_t tries = 0; tries < chain; ++tries)
        {
            // Chains run from near to far. An entry no further back than the one before belongs to a newer
            // position that reused the slot 32 KiB later; beyond the window or before the input there is
            // nothing to match. Either ends the chain.
            const std::size_t distance = static_cast<std::uint32_t>(static_cast<std::uint32_t>(pos) - candidate);
            if (distance <= previousDistance || distance > deflateWindow || distance > pos) break;
            previousDistance = distance;
            const unsigned char* there = here - distance;
            // The byte at the current best length and the one before it rule out most candidates: test them first.
            if (there[bestLength] == here[bestLength] && there[bestLength - 1] == here[bestLength - 1] && there[0] == here[0] &&
                there[1] == here[1] && there[2] == here[2])
            {
                std::size_t length = deflateMinMatch;
                while (length + 8 <= limit && deflateLoad64(there + length) == deflateLoad64(here + length)) length += 8;
                while (length < limit && there[length] == here[length]) ++length;
                if (length > bestLength)
                {
                    bestLength = length;
                    best = {length, distance};
                    if (length >= deflateNiceLength || length == limit) break;
                }
            }
            candidate = prev[candidate & (deflateWindow - 1)];
        }
        if (best.length == deflateMinMatch && best.distance > deflateTooFar) return {0, 0};
        return best;
    }

    void writeTokens(const DeflateCode& literals, const DeflateCode& distances)
    {
        for (const DeflateToken& t : tokens)
        {
            if (t.length == 0)
            {
                writer.put(literals.codes[t.value], literals.lengths[t.value]);
                continue;
            }
            const std::size_t lengthCode = deflateLengthIndex[t.length];
            writer.put(literals.codes[257 + lengthCode], literals.lengths[257 + lengthCode]);
            writer.put(static_cast<std::uint32_t>(t.length - deflateLengthBase[lengthCode]), deflateLengthExtra[lengthCode]);
            const std::size_t distanceCode = deflateDistanceCode(t.value);
            writer.put(distances.codes[distanceCode], distances.lengths[distanceCode]);
            writer.put(static_cast<std::uint32_t>(t.value - deflateDistanceBase[distanceCode]), deflateDistanceExtra[distanceCode]);
        }
        writer.put(literals.codes[deflateEndOfBlock], literals.lengths[deflateEndOfBlock]);
    }

    /// Codes the tokens since blockStart (the input up to `end`) as one block, of whichever type is smallest.
    void writeBlock(std::size_t end, bool last)
    {
        std::array<std::uint32_t, deflateLiteralCodes> literalFreq{};
        std::array<std::uint32_t, deflateDistanceCodes> distanceFreq{};
        std::uint64_t extraBits = 0;
        for (const DeflateToken& t : tokens)
        {
            if (t.length == 0)
            {
                ++literalFreq[t.value];
                continue;
            }
            const std::size_t lengthCode = deflateLengthIndex[t.length];
            ++literalFreq[257 + lengthCode];
            extraBits += deflateLengthExtra[lengthCode];
            const std::size_t distanceCode = deflateDistanceCode(t.value);
            ++distanceFreq[distanceCode];
            extraBits += deflateDistanceExtra[distanceCode];
        }
        literalFreq[deflateEndOfBlock] = 1;

        std::uint64_t fixedBits = 3 + extraBits;
        for (std::size_t s = 0; s < deflateLiteralCodes; ++s) fixedBits += std::uint64_t{literalFreq[s]} * fixedLiterals.lengths[s];
        for (std::size_t s = 0; s < deflateDistanceCodes; ++s) fixedBits += std::uint64_t{distanceFreq[s]} * fixedDistances.lengths[s];

        DeflateCode literals;
        literals.lengths.resize(deflateLiteralCodes);
        deflateCodeLengths(literalFreq.data(), deflateLiteralCodes, deflateMaxBits, literals.lengths.data());
        DeflateCode distances;
        distances.lengths.resize(deflateDistanceCodes);
        deflateCodeLengths(distanceFreq.data(), deflateDistanceCodes, deflateMaxBits, distances.lengths.data());
        std::size_t literalCount = deflateLiteralCodes;
        while (literalCount > 257 && literals.lengths[literalCount - 1] == 0) --literalCount;
        std::size_t distanceCount = deflateDistanceCodes;
        while (distanceCount > 1 && distances.lengths[distanceCount - 1] == 0) --distanceCount;
        // Both length lists go through one run-length pass: RFC 1951 lets a repeat cross from one to the other.
        std::vector<std::uint8_t> allLengths(literals.lengths.begin(), literals.lengths.begin() + static_cast<std::ptrdiff_t>(literalCount));
        allLengths.insert(allLengths.end(), distances.lengths.begin(), distances.lengths.begin() + static_cast<std::ptrdiff_t>(distanceCount));
        deflateRunLengths(allLengths.data(), allLengths.size(), runs);
        std::array<std::uint32_t, deflateCodeLengthCodes> runFreq{};
        for (const DeflateRun& r : runs) ++runFreq[r.symbol];
        DeflateCode runCode;
        runCode.lengths.resize(deflateCodeLengthCodes);
        deflateCodeLengths(runFreq.data(), deflateCodeLengthCodes, deflateMaxCodeLengthBits, runCode.lengths.data());
        std::size_t runCodeCount = deflateCodeLengthCodes;
        while (runCodeCount > 4 && runCode.lengths[deflateCodeLengthOrder[runCodeCount - 1]] == 0) --runCodeCount;

        std::uint64_t dynamicBits = 3 + 5 + 5 + 4 + 3 * std::uint64_t{runCodeCount} + extraBits;
        for (std::size_t s = 0; s < deflateCodeLengthCodes; ++s)
        {
            dynamicBits += std::uint64_t{runFreq[s]} * (std::uint64_t{runCode.lengths[s]} + deflateRunExtraBits[s]);
        }
        for (std::size_t s = 0; s < deflateLiteralCodes; ++s) dynamicBits += std::uint64_t{literalFreq[s]} * literals.lengths[s];
        for (std::size_t s = 0; s < deflateDistanceCodes; ++s) dynamicBits += std::uint64_t{distanceFreq[s]} * distances.lengths[s];

        // Stored: per 65535-byte piece 3 header bits, padding to a byte, LEN and NLEN, then the bytes. Only
        // the first piece's padding depends on where the previous block ended.
        const std::size_t length = end - blockStart;
        const std::uint64_t pieces = length == 0 ? 1 : (std::uint64_t{length} + deflateStoredMax - 1) / deflateStoredMax;
        const std::uint64_t firstPadding = (8 - (writer.heldBits() + 3) % 8) % 8;
        const std::uint64_t storedBits = pieces * (3 + 32) + firstPadding + (pieces - 1) * 5 + 8 * std::uint64_t{length};

        if (storedBits < fixedBits && storedBits < dynamicBits)
        {
            writeStored(blockStart, length, last);
        }
        else if (dynamicBits < fixedBits)
        {
            literals.lengths.resize(literalCount);
            literals.build();
            distances.lengths.resize(distanceCount);
            distances.build();
            runCode.build();
            writer.put(last ? 1u : 0u, 1);
            writer.put(2, 2);
            writer.put(static_cast<std::uint32_t>(literalCount - 257), 5);
            writer.put(static_cast<std::uint32_t>(distanceCount - 1), 5);
            writer.put(static_cast<std::uint32_t>(runCodeCount - 4), 4);
            for (std::size_t i = 0; i < runCodeCount; ++i) writer.put(runCode.lengths[deflateCodeLengthOrder[i]], 3);
            for (const DeflateRun& r : runs)
            {
                writer.put(runCode.codes[r.symbol], runCode.lengths[r.symbol]);
                writer.put(r.extra, deflateRunExtraBits[r.symbol]);
            }
            writeTokens(literals, distances);
        }
        else
        {
            writer.put(last ? 1u : 0u, 1);
            writer.put(1, 2);
            writeTokens(fixedLiterals, fixedDistances);
        }
        tokens.clear();
        blockStart = end;
    }

    /// Incompressible input goes out as is, in pieces of at most 65535 bytes (LEN is 16 bits).
    void writeStored(std::size_t start, std::size_t length, bool last)
    {
        do
        {
            const std::size_t piece = (std::min)(length, deflateStoredMax);
            length -= piece;
            writer.put(last && length == 0 ? 1u : 0u, 1);
            writer.put(0, 2);
            writer.align();
            writer.put(static_cast<std::uint32_t>(piece), 16);
            writer.put(static_cast<std::uint32_t>(piece) ^ 0xFFFFu, 16);
            writer.bytes(data + start, piece);
            start += piece;
        } while (length > 0);
    }

    const unsigned char* data;
    std::size_t size;
    DeflateBitWriter writer;
    std::vector<std::uint32_t> head;  // per hash: the latest position
    std::vector<std::uint32_t> prev;  // per position modulo the window: the previous position with its hash
    std::vector<DeflateToken> tokens;
    std::vector<DeflateRun> runs;
    std::size_t blockStart = 0;
    DeflateCode fixedLiterals;
    DeflateCode fixedDistances;
};

void gzipPutLittleEndian(std::string& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<char>(value & 0xFFu));
        value >>= 8;
    }
}
}  // namespace

std::uint32_t crc32(std::string_view data) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char c : data) crc = gzipCrcTable[(crc ^ static_cast<unsigned char>(c)) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::string gzip(std::string_view data)
{
    std::string out;
    out.reserve(data.size() / 4 + 64);
    out.append(reinterpret_cast<const char*>(gzipHeader.data()), gzipHeader.size());
    DeflateEncoder(data, out).run();
    gzipPutLittleEndian(out, crc32(data));
    // ISIZE is the input size modulo 2^32.
    gzipPutLittleEndian(out, static_cast<std::uint32_t>(data.size()));
    return out;
}
}  // namespace krtu
