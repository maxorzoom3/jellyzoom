/* A small QR code encoder: byte mode, error correction M (or L when the text is long), versions 1 to 10.
 * It makes the matrix of dark and light modules; drawing it is the screen's business.
 * The method is the one in the QR standard (ISO 18004): Reed-Solomon over GF(256), the eight masks,
 * and the penalty rules to choose one. */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace jfqr
{

struct Code
{
    int size = 0;                      // modules along one side, 0 when the text did not fit
    std::vector<std::uint8_t> modules; // size * size, 1 = dark, row by row

    bool dark(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < size && y < size && modules[static_cast<std::size_t>(y * size + x)] != 0;
    }
};

namespace detail
{

// per error-correction level (0 = L, 1 = M), per version 1..10
inline int eccPerBlock(int level, int version)
{
    static const int table[2][11] = {{-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18},
                                     {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26}};
    return table[level][version];
}

inline int blockCount(int level, int version)
{
    static const int table[2][11] = {{-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4}, {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5}};
    return table[level][version];
}

inline int rawModules(int version)
{
    int result = (16 * version + 128) * version + 64;
    if (version >= 2)
    {
        const int align = version / 7 + 2;
        result -= (25 * align - 10) * align - 55;
        if (version >= 7)
            result -= 36;
    }
    return result;
}

inline int dataCodewords(int level, int version)
{
    return rawModules(version) / 8 - eccPerBlock(level, version) * blockCount(level, version);
}

inline std::uint8_t gfMultiply(std::uint8_t x, std::uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; --i)
    {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<std::uint8_t>(z);
}

inline std::vector<std::uint8_t> divisor(int degree)
{
    std::vector<std::uint8_t> result(static_cast<std::size_t>(degree), 0);
    result[static_cast<std::size_t>(degree - 1)] = 1;
    std::uint8_t root = 1;
    for (int i = 0; i < degree; ++i)
    {
        for (int j = 0; j < degree; ++j)
        {
            result[static_cast<std::size_t>(j)] = gfMultiply(result[static_cast<std::size_t>(j)], root);
            if (j + 1 < degree)
                result[static_cast<std::size_t>(j)] ^= result[static_cast<std::size_t>(j + 1)];
        }
        root = gfMultiply(root, 0x02);
    }
    return result;
}

inline std::vector<std::uint8_t> remainder(const std::vector<std::uint8_t> &data, const std::vector<std::uint8_t> &div)
{
    std::vector<std::uint8_t> result(div.size(), 0);
    for (const std::uint8_t b : data)
    {
        const std::uint8_t factor = static_cast<std::uint8_t>(b ^ result[0]);
        result.erase(result.begin());
        result.push_back(0);
        for (std::size_t i = 0; i < div.size(); ++i)
            result[i] ^= gfMultiply(div[i], factor);
    }
    return result;
}

struct Builder
{
    int version;
    int size;
    std::vector<std::uint8_t> modules;
    std::vector<std::uint8_t> isFunction;

    Builder(int v) : version(v), size(v * 4 + 17)
    {
        modules.assign(static_cast<std::size_t>(size * size), 0);
        isFunction.assign(static_cast<std::size_t>(size * size), 0);
    }

    void set(int x, int y, bool dark)
    {
        modules[static_cast<std::size_t>(y * size + x)] = dark ? 1 : 0;
    }
    bool get(int x, int y) const
    {
        return modules[static_cast<std::size_t>(y * size + x)] != 0;
    }
    void setFunction(int x, int y, bool dark)
    {
        set(x, y, dark);
        isFunction[static_cast<std::size_t>(y * size + x)] = 1;
    }
    static bool bit(unsigned value, int i)
    {
        return ((value >> i) & 1) != 0;
    }

    void finder(int cx, int cy)
    {
        for (int dy = -4; dy <= 4; ++dy)
        {
            for (int dx = -4; dx <= 4; ++dx)
            {
                const int dist = std::max(std::abs(dx), std::abs(dy));
                const int x = cx + dx;
                const int y = cy + dy;
                if (x >= 0 && x < size && y >= 0 && y < size)
                    setFunction(x, y, dist != 2 && dist != 4);
            }
        }
    }

    void alignment(int cx, int cy)
    {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                setFunction(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }

    std::vector<int> alignmentPositions() const
    {
        std::vector<int> result;
        if (version == 1)
            return result;
        const int count = version / 7 + 2;
        const int step = (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
        result.push_back(6);
        std::vector<int> rest;
        for (int i = 0, pos = size - 7; i < count - 1; ++i, pos -= step)
            rest.insert(rest.begin(), pos);
        result.insert(result.end(), rest.begin(), rest.end());
        return result;
    }

    void formatBits(int level, int mask)
    {
        const unsigned eccBits = level == 0 ? 1u : 0u; // L = 01, M = 00
        const unsigned data = (eccBits << 3) | static_cast<unsigned>(mask);
        unsigned rem = data;
        for (int i = 0; i < 10; ++i)
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const unsigned bits = ((data << 10) | rem) ^ 0x5412;
        for (int i = 0; i <= 5; ++i)
            setFunction(8, i, bit(bits, i));
        setFunction(8, 7, bit(bits, 6));
        setFunction(8, 8, bit(bits, 7));
        setFunction(7, 8, bit(bits, 8));
        for (int i = 9; i < 15; ++i)
            setFunction(14 - i, 8, bit(bits, i));
        for (int i = 0; i < 8; ++i)
            setFunction(size - 1 - i, 8, bit(bits, i));
        for (int i = 8; i < 15; ++i)
            setFunction(8, size - 15 + i, bit(bits, i));
        setFunction(8, size - 8, true);
    }

    void versionBits()
    {
        if (version < 7)
            return;
        unsigned rem = static_cast<unsigned>(version);
        for (int i = 0; i < 12; ++i)
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        const unsigned bits = (static_cast<unsigned>(version) << 12) | rem;
        for (int i = 0; i < 18; ++i)
        {
            const bool color = bit(bits, i);
            const int a = size - 11 + i % 3;
            const int b = i / 3;
            setFunction(a, b, color);
            setFunction(b, a, color);
        }
    }

    void functionPatterns(int level)
    {
        for (int i = 0; i < size; ++i)
        {
            setFunction(6, i, i % 2 == 0);
            setFunction(i, 6, i % 2 == 0);
        }
        finder(3, 3);
        finder(size - 4, 3);
        finder(3, size - 4);
        const std::vector<int> pos = alignmentPositions();
        const int n = static_cast<int>(pos.size());
        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j < n; ++j)
            {
                if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))
                    continue;
                alignment(pos[static_cast<std::size_t>(i)], pos[static_cast<std::size_t>(j)]);
            }
        }
        formatBits(level, 0);
        versionBits();
    }

    void place(const std::vector<std::uint8_t> &data)
    {
        std::size_t i = 0;
        for (int right = size - 1; right >= 1; right -= 2)
        {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < size; ++vert)
            {
                for (int j = 0; j < 2; ++j)
                {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size - 1 - vert : vert;
                    if (!isFunction[static_cast<std::size_t>(y * size + x)] && i < data.size() * 8)
                    {
                        set(x, y, bit(data[i >> 3], 7 - static_cast<int>(i & 7)));
                        ++i;
                    }
                }
            }
        }
    }

    void applyMask(int mask)
    {
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                bool invert = false;
                switch (mask)
                {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (invert && !isFunction[static_cast<std::size_t>(y * size + x)])
                    modules[static_cast<std::size_t>(y * size + x)] ^= 1;
            }
        }
    }

    // The standard's four penalty rules (runs, blocks, finder-like patterns, balance)
    int penalty() const
    {
        int result = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int a = 0; a < size; ++a)
            {
                int run = 0;
                bool last = false;
                std::uint32_t window = 0;
                for (int b = 0; b < size; ++b)
                {
                    const bool dark = pass == 0 ? get(b, a) : get(a, b);
                    if (b > 0 && dark == last)
                    {
                        ++run;
                        if (run == 5)
                            result += 3;
                        else if (run > 5)
                            result += 1;
                    }
                    else
                        run = 1;
                    last = dark;
                    window = ((window << 1) | (dark ? 1u : 0u)) & 0x7FF;
                    if (b >= 10 && (window == 0x05D || window == 0x5D0))
                        result += 40;
                }
            }
        }
        for (int y = 0; y + 1 < size; ++y)
        {
            for (int x = 0; x + 1 < size; ++x)
            {
                const bool c = get(x, y);
                if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1))
                    result += 3;
            }
        }
        int dark = 0;
        for (const std::uint8_t m : modules)
            dark += m;
        const int total = size * size;
        const int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
        result += std::max(0, k) * 10;
        return result;
    }
};

} // namespace detail

// Makes the code for `text`; size is 0 when it is too long for version 10
inline Code encode(const std::string &text)
{
    Code out;
    const int length = static_cast<int>(text.size());
    int level = 1;
    int version = 0;
    for (int tryLevel = 1; tryLevel >= 0 && version == 0; --tryLevel)
    {
        for (int v = 1; v <= 10; ++v)
        {
            const int countBits = v < 10 ? 8 : 16;
            const int needBits = 4 + countBits + 8 * length;
            if (needBits <= detail::dataCodewords(tryLevel, v) * 8)
            {
                version = v;
                level = tryLevel;
                break;
            }
        }
    }
    if (version == 0)
        return out;

    // the bit stream: mode, count, bytes, terminator, padding
    std::vector<std::uint8_t> bits;
    auto append = [&](unsigned value, int count) {
        for (int i = count - 1; i >= 0; --i)
            bits.push_back(static_cast<std::uint8_t>((value >> i) & 1));
    };
    append(4, 4);
    append(static_cast<unsigned>(length), version < 10 ? 8 : 16);
    for (const char c : text)
        append(static_cast<unsigned>(static_cast<unsigned char>(c)), 8);
    const std::size_t capacity = static_cast<std::size_t>(detail::dataCodewords(level, version)) * 8;
    for (int i = 0; i < 4 && bits.size() < capacity; ++i)
        bits.push_back(0);
    while (bits.size() % 8 != 0)
        bits.push_back(0);
    std::vector<std::uint8_t> data(capacity / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i)
        data[i >> 3] = static_cast<std::uint8_t>(data[i >> 3] | (bits[i] << (7 - (i & 7))));
    for (std::size_t i = bits.size() / 8, pad = 0; i < data.size(); ++i, ++pad)
        data[i] = (pad % 2 == 0) ? 0xEC : 0x11;

    // error correction, blocks interleaved
    const int blocks = detail::blockCount(level, version);
    const int eccLen = detail::eccPerBlock(level, version);
    const int raw = detail::rawModules(version) / 8;
    const int shortBlocks = blocks - raw % blocks;
    const int shortLen = raw / blocks;
    const std::vector<std::uint8_t> div = detail::divisor(eccLen);
    std::vector<std::vector<std::uint8_t>> all;
    std::size_t k = 0;
    for (int i = 0; i < blocks; ++i)
    {
        const int dataLen = shortLen - eccLen + (i < shortBlocks ? 0 : 1);
        std::vector<std::uint8_t> block(data.begin() + static_cast<std::ptrdiff_t>(k),
                                        data.begin() + static_cast<std::ptrdiff_t>(k) + dataLen);
        k += static_cast<std::size_t>(dataLen);
        const std::vector<std::uint8_t> ecc = detail::remainder(block, div);
        if (i < shortBlocks)
            block.push_back(0);
        block.insert(block.end(), ecc.begin(), ecc.end());
        all.push_back(block);
    }
    std::vector<std::uint8_t> result;
    for (std::size_t i = 0; i < all[0].size(); ++i)
    {
        for (int j = 0; j < blocks; ++j)
        {
            if (i != static_cast<std::size_t>(shortLen - eccLen) || j >= shortBlocks)
                result.push_back(all[static_cast<std::size_t>(j)][i]);
        }
    }

    // draw, and keep the mask with the lowest penalty
    int bestMask = 0;
    int bestPenalty = 1 << 30;
    detail::Builder best(version);
    for (int mask = 0; mask < 8; ++mask)
    {
        detail::Builder b(version);
        b.functionPatterns(level);
        b.place(result);
        b.applyMask(mask);
        b.formatBits(level, mask);
        const int p = b.penalty();
        if (p < bestPenalty)
        {
            bestPenalty = p;
            bestMask = mask;
            best = b;
        }
    }
    (void)bestMask;
    out.size = best.size;
    out.modules = best.modules;
    return out;
}

} // namespace jfqr
