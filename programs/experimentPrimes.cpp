//
// Created by Andrea Cocito on 14/03/26.
//

// File: tools/search_fj32_table.cpp
//
// Offline search tool for a custom FJ-style uint32_t backend compatible with
// the current utilities::isPrime prefilter up to 311.
//
// Compile example:
//   c++ -std=c++20 -O3 -DNDEBUG -I include tools/search_fj32_table.cpp -o search_fj32_table
//
// This tool does NOT modify include/utilities/isPrime.h.
// It uses the current utilities::isPrime only as an oracle while building / probing candidates.

#include <utilities/isPrime.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

/* ============================================================================
 *  Exact copy of the current uint32 prefilter contract (up to 311)
 * ========================================================================== */

enum class PrefilterResult : uint8_t {
    composite,
    prime,
    undecided
};

inline constexpr std::array<uint16_t, 63> kSmallPrimes{
    3, 5, 7, 11, 13, 17, 19,
    23, 29, 31, 37, 41, 43, 47, 53,
    59, 61, 67, 71, 73, 79, 83, 89,
    97, 101, 103, 107, 109, 113, 127, 131,
    137, 139, 149, 151, 157, 163, 167, 173,
    179, 181, 191, 193, 197, 199, 211, 223,
    227, 229, 233, 239, 241, 251, 257, 263,
    269, 271, 277, 281, 283, 293, 307, 311
};

[[nodiscard]] PrefilterResult prefilter32(uint32_t n) noexcept
{
    if (n < 2u)
        return PrefilterResult::composite;

    if ((n & 1u) == 0u)
        return n == 2u ? PrefilterResult::prime
                       : PrefilterResult::composite;

    if (n <= static_cast<uint32_t>(kSmallPrimes.back())) {
        const auto needle = static_cast<uint16_t>(n);
        return std::binary_search(kSmallPrimes.begin(), kSmallPrimes.end(), needle)
            ? PrefilterResult::prime
            : PrefilterResult::composite;
    }

    for (const uint16_t p : kSmallPrimes) {
        if (n % static_cast<uint32_t>(p) == 0u)
            return PrefilterResult::composite;
    }

    const uint32_t last = static_cast<uint32_t>(kSmallPrimes.back());
    if (last > n / last)
        return PrefilterResult::prime;

    return PrefilterResult::undecided;
}

/* ============================================================================
 *  Single-base SPRP (strong probable prime) test for uint32_t
 * ========================================================================== */

[[nodiscard]] uint32_t mulMod32(uint32_t a, uint32_t b, uint32_t mod) noexcept
{
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) % mod
    );
}

[[nodiscard]] uint32_t powMod32(uint32_t base, uint32_t exp, uint32_t mod) noexcept
{
    uint32_t result = 1u;

    while (exp != 0u) {
        if ((exp & 1u) != 0u)
            result = mulMod32(result, base, mod);

        base = mulMod32(base, base, mod);
        exp >>= 1;
    }

    return result;
}

/// Returns true iff n passes the strong probable-prime test to base a.
[[nodiscard]] bool sprp32(uint32_t n, uint32_t a) noexcept
{
    a %= n;
    if (a == 0u)
        return true;

    uint32_t d = n - 1u;
    unsigned s = 0;

    while ((d & 1u) == 0u) {
        d >>= 1u;
        ++s;
    }

    uint32_t x = powMod32(a, d, n);

    if (x == 1u || x == n - 1u)
        return true;

    for (unsigned r = 1; r < s; ++r) {
        x = mulMod32(x, x, n);
        if (x == n - 1u)
            return true;
    }

    return false;
}

/* ============================================================================
 *  Candidate table
 * ========================================================================== */

struct CandidateTable {
    unsigned bucketBits = 0;              // 6, 7, 8  => 64, 128, 256 buckets
    uint32_t multiplier = 0;              // odd hash multiplier
    std::vector<uint16_t> bucketBase;     // size = 1u << bucketBits
};

[[nodiscard]] uint32_t hashBucket(uint32_t n,
                                  uint32_t multiplier,
                                  unsigned bucketBits) noexcept
{
    // FJ-style hash bucketing: multiply in uint32_t domain (wraparound),
    // then use the top bucketBits of the 32-bit mixed value.
    const uint32_t mixed = static_cast<uint32_t>(n * multiplier);
    return mixed >> (32u - bucketBits);
}

[[nodiscard]] bool candidateIsPrime32(uint32_t n, const CandidateTable &table) noexcept
{
    switch (prefilter32(n)) {
        case PrefilterResult::prime:
            return true;

        case PrefilterResult::composite:
            return false;

        case PrefilterResult::undecided:
            break;
    }

    const uint32_t b = hashBucket(n, table.multiplier, table.bucketBits);
    const uint16_t a = table.bucketBase[b];
    return sprp32(n, static_cast<uint32_t>(a));
}

/* ============================================================================
 *  Utilities
 * ========================================================================== */

template <typename T>
void appendUnique(std::vector<T> &dst,
                  std::unordered_set<T> &seen,
                  T value)
{
    if (seen.insert(value).second)
        dst.push_back(value);
}

template <typename T>
void appendUniqueRange(std::vector<T> &dst,
                       std::unordered_set<T> &seen,
                       const std::vector<T> &src)
{
    for (const T x : src)
        appendUnique(dst, seen, x);
}

[[nodiscard]] std::vector<uint16_t> sievePrimes16(uint32_t limit)
{
    std::vector<bool> composite(limit + 1u, false);
    std::vector<uint16_t> primes;

    for (uint32_t p = 2u; p <= limit; ++p) {
        if (composite[p])
            continue;

        primes.push_back(static_cast<uint16_t>(p));

        if (p <= limit / p) {
            for (uint32_t q = p * p; q <= limit; q += p)
                composite[q] = true;
        }
    }

    return primes;
}

[[nodiscard]] std::vector<uint16_t> makeBasePool16()
{
    // Full prime base pool up to uint16_t max.
    // Includes 2; the runtime code always handles a %= n anyway.
    return sievePrimes16(65535u);
}

[[nodiscard]] std::vector<uint32_t> makeMultiplierCandidates(std::size_t randomCount,
                                                             uint64_t seed)
{
    std::vector<uint32_t> out{
        0x9e3779b1u, 0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2du,
        0x165667b1u, 0xd2511f53u, 0xcd9e8d57u, 0x94d049bbu,
        0xf1357ae5u, 0x7feb352du, 0x846ca68bu, 0x5bd1e995u
    };

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint32_t> dist(
        1u, std::numeric_limits<uint32_t>::max()
    );

    std::unordered_set<uint32_t> seen(out.begin(), out.end());

    while (out.size() < randomCount + seen.size()) {
        uint32_t m = dist(rng) | 1u; // odd only
        if (seen.insert(m).second)
            out.push_back(m);
        if (out.size() >= randomCount + 12u)
            break;
    }

    return out;
}

[[nodiscard]] bool isUndecidedCompositeOracle(uint32_t n)
{
    return prefilter32(n) == PrefilterResult::undecided && !utilities::isPrime(n);
}

[[nodiscard]] std::vector<uint32_t> collectRandomUndecidedComposites(std::size_t count,
                                                                     std::mt19937_64 &rng,
                                                                     uint32_t lo = 0u,
                                                                     uint32_t hi = std::numeric_limits<uint32_t>::max())
{
    std::vector<uint32_t> out;
    std::unordered_set<uint32_t> seen;
    out.reserve(count);

    std::uniform_int_distribution<uint32_t> dist(lo, hi);

    while (out.size() < count) {
        uint32_t n = dist(rng) | 1u;
        if (!isUndecidedCompositeOracle(n))
            continue;
        appendUnique(out, seen, n);
    }

    return out;
}

[[nodiscard]] std::vector<uint32_t> collectDynamicHardPseudoprimes(const std::vector<uint32_t> &criticalBases,
                                                                   std::size_t wantPerBase,
                                                                   std::size_t maxAttemptsPerBase,
                                                                   std::mt19937_64 &rng)
{
    std::vector<uint32_t> out;
    std::unordered_set<uint32_t> seen;
    std::uniform_int_distribution<uint32_t> dist(0u, std::numeric_limits<uint32_t>::max());

    for (const uint32_t a : criticalBases) {
        std::size_t found = 0;

        for (std::size_t attempts = 0; attempts < maxAttemptsPerBase && found < wantPerBase; ++attempts) {
            uint32_t n = dist(rng) | 1u;

            if (!isUndecidedCompositeOracle(n))
                continue;

            if (!sprp32(n, a))
                continue;

            if (seen.insert(n).second) {
                out.push_back(n);
                ++found;
            }
        }
    }

    return out;
}

[[nodiscard]] std::vector<uint32_t> makeSeedTroublemakers()
{
    // These are just seed composites that are often annoying in primality contexts.
    // The tool filters them through the current oracle before using them.
    static constexpr std::array<uint32_t, 52> raw{
        341u, 561u, 645u, 1105u, 1387u, 1729u, 1905u, 2047u, 2465u, 2701u,
        2821u, 3277u, 4033u, 4369u, 4681u, 5461u, 6601u, 7957u, 8321u, 8481u,
        8911u, 10261u, 10585u, 11305u, 12801u, 13741u, 15841u, 16705u, 18721u,
        19951u, 29341u, 30121u, 30889u, 314821u, 42799u, 49141u, 52633u, 62745u,
        63973u, 65281u, 74665u, 75361u, 80581u, 83333u, 85489u, 88357u, 90751u,
        104653u, 130561u, 162401u, 188191u, 252601u
    };

    std::vector<uint32_t> out;
    out.reserve(raw.size());

    for (const uint32_t n : raw) {
        if (isUndecidedCompositeOracle(n))
            out.push_back(n);
    }

    return out;
}

[[nodiscard]] bool verifyAgainstOracleSet(const CandidateTable &table,
                                          const std::vector<uint32_t> &values,
                                          std::vector<uint32_t> *counterexamples = nullptr,
                                          std::size_t maxCounterexamples = 32)
{
    bool ok = true;

    for (const uint32_t n : values) {
        const bool got = candidateIsPrime32(n, table);
        const bool want = utilities::isPrime(n);

        if (got != want) {
            ok = false;
            if (counterexamples != nullptr && counterexamples->size() < maxCounterexamples)
                counterexamples->push_back(n);
            if (counterexamples == nullptr || counterexamples->size() >= maxCounterexamples)
                break;
        }
    }

    return ok;
}

/* ============================================================================
 *  Table synthesis for a fixed multiplier
 * ========================================================================== */

[[nodiscard]] bool findBucketBase(const std::vector<uint32_t> &bucketComposites,
                                  const std::vector<uint16_t> &basePool,
                                  uint16_t *selectedBase)
{
    if (bucketComposites.empty()) {
        *selectedBase = 2u;
        return true;
    }

    // Short first-pass list to fail fast on easy buckets.
    static constexpr std::array<uint16_t, 28> shortList{
        2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u,
        23u, 29u, 31u, 37u, 41u, 43u, 47u, 53u,
        59u, 61u, 67u, 71u, 73u, 79u, 83u, 89u,
        97u, 101u, 103u, 107u
    };

    auto testBase = [&](uint32_t a) -> bool {
        for (const uint32_t n : bucketComposites) {
            if (sprp32(n, a))
                return false; // composite survives => bad base for this bucket
        }
        return true;
    };

    for (const uint16_t a : shortList) {
        if (testBase(a)) {
            *selectedBase = a;
            return true;
        }
    }

    for (const uint16_t a : basePool) {
        if (testBase(static_cast<uint32_t>(a))) {
            *selectedBase = a;
            return true;
        }
    }

    return false;
}

[[nodiscard]] bool synthesizeForMultiplier(unsigned bucketBits,
                                           uint32_t multiplier,
                                           const std::vector<uint32_t> &trainingComposites,
                                           const std::vector<uint16_t> &basePool,
                                           CandidateTable *out)
{
    const uint32_t bucketCount = 1u << bucketBits;
    std::vector<std::vector<uint32_t>> buckets(bucketCount);

    for (const uint32_t n : trainingComposites) {
        const uint32_t b = hashBucket(n, multiplier, bucketBits);
        buckets[b].push_back(n);
    }

    CandidateTable candidate;
    candidate.bucketBits = bucketBits;
    candidate.multiplier = multiplier;
    candidate.bucketBase.assign(bucketCount, 2u);

    for (uint32_t b = 0; b < bucketCount; ++b) {
        uint16_t base = 2u;
        if (!findBucketBase(buckets[b], basePool, &base))
            return false;
        candidate.bucketBase[b] = base;
    }

    *out = std::move(candidate);
    return true;
}

/* ============================================================================
 *  Full exhaustive verification by segmented sieve over odd uint32_t values
 * ========================================================================== */

[[nodiscard]] bool exhaustiveVerify32(const CandidateTable &table,
                                      std::size_t segmentOddCount,
                                      std::vector<uint32_t> *counterexamples = nullptr,
                                      std::size_t maxCounterexamples = 32)
{
    // Verify:
    //   - 0,1 => false
    //   - 2   => true
    //   - odd n >= 3 via segmented sieve
    if (candidateIsPrime32(0u, table) != false) {
        if (counterexamples != nullptr) counterexamples->push_back(0u);
        return false;
    }
    if (candidateIsPrime32(1u, table) != false) {
        if (counterexamples != nullptr) counterexamples->push_back(1u);
        return false;
    }
    if (candidateIsPrime32(2u, table) != true) {
        if (counterexamples != nullptr) counterexamples->push_back(2u);
        return false;
    }

    const std::vector<uint16_t> primes16 = sievePrimes16(65535u);
    std::vector<uint32_t> oddPrimes;
    oddPrimes.reserve(primes16.size());

    for (const uint16_t p : primes16) {
        if (p >= 3u)
            oddPrimes.push_back(static_cast<uint32_t>(p));
    }

    std::vector<uint8_t> composite(segmentOddCount, 0u);

    const uint64_t maxOdd = std::numeric_limits<uint32_t>::max(); // odd
    uint64_t lo = 3u;
    std::size_t segmentIndex = 0;

    while (lo <= maxOdd) {
        uint64_t hi = lo + 2u * static_cast<uint64_t>(segmentOddCount - 1u);
        if (hi > maxOdd)
            hi = maxOdd;

        const std::size_t count = static_cast<std::size_t>(((hi - lo) / 2u) + 1u);
        std::fill(composite.begin(), composite.begin() + count, 0u);

        for (const uint32_t p : oddPrimes) {
            uint64_t start = static_cast<uint64_t>(p) * static_cast<uint64_t>(p);
            if (start < lo) {
                start = ((lo + p - 1u) / p) * static_cast<uint64_t>(p);
            }

            if ((start & 1u) == 0u)
                start += p;

            if (start > hi)
                continue;

            for (uint64_t m = start; m <= hi; m += 2u * static_cast<uint64_t>(p)) {
                composite[static_cast<std::size_t>((m - lo) / 2u)] = 1u;
            }
        }

        for (std::size_t i = 0; i < count; ++i) {
            const uint32_t n = static_cast<uint32_t>(lo + 2u * static_cast<uint64_t>(i));
            const bool expectPrime = (composite[i] == 0u);
            const bool gotPrime = candidateIsPrime32(n, table);

            if (gotPrime != expectPrime) {
                if (counterexamples != nullptr && counterexamples->size() < maxCounterexamples)
                    counterexamples->push_back(n);
                if (counterexamples == nullptr || counterexamples->size() >= maxCounterexamples)
                    return false;
            }
        }

        ++segmentIndex;
        if ((segmentIndex % 128u) == 0u) {
            const double done = 100.0 * static_cast<double>(hi) / static_cast<double>(maxOdd);
            std::cout << "    exhaustive: " << std::fixed << std::setprecision(2)
                      << done << "%\r" << std::flush;
        }

        if (hi == maxOdd)
            break;

        lo = hi + 2u;
    }

    std::cout << "    exhaustive: 100.00%\n";
    return counterexamples == nullptr || counterexamples->empty();
}

/* ============================================================================
 *  Pretty-print of a generated solution
 * ========================================================================== */

void printSolutionAsHeaderSnippet(const CandidateTable &table)
{
    const uint32_t bucketCount = 1u << table.bucketBits;

    std::cout << "\n=== SUCCESS ===\n";
    std::cout << "bucket count : " << bucketCount << "\n";
    std::cout << "bucket bits  : " << table.bucketBits << "\n";
    std::cout << "multiplier   : 0x"
              << std::hex << std::setw(8) << std::setfill('0') << table.multiplier
              << std::dec << "\n\n";

    std::cout << "static constexpr uint32_t fj32Multiplier = 0x"
              << std::hex << std::setw(8) << std::setfill('0') << table.multiplier
              << "u;\n" << std::dec;

    std::cout << "static constexpr std::array<uint16_t, " << bucketCount << "> fj32Bases{\n    ";

    for (std::size_t i = 0; i < table.bucketBase.size(); ++i) {
        std::cout << table.bucketBase[i] << 'u';
        if (i + 1u != table.bucketBase.size())
            std::cout << ", ";
        if ((i + 1u) % 16u == 0u && i + 1u != table.bucketBase.size())
            std::cout << "\n    ";
    }

    std::cout << "\n};\n";
}

/* ============================================================================
 *  Config
 * ========================================================================== */

struct Config {
    std::array<unsigned, 3> bucketBitsToTry{6u, 7u, 8u}; // 64, 128, 256
    std::size_t multiplierCandidates = 4000u;
    std::size_t initialRandomTraining = 50000u;
    std::size_t probeRandomTraining = 50000u;
    std::size_t hardPseudoPerBase = 128u;
    std::size_t hardPseudoAttemptsPerBase = 200000u;
    std::size_t maxCounterexamplesKept = 32u;
    std::size_t maxSearchPassesPerBucketCount = 3u;
    std::size_t exhaustiveSegmentOddCount = 1u << 22; // ~4 million odd numbers / segment
    uint64_t seed = 0x7f8d9c0b12345678ULL;
};

/* ============================================================================
 *  Main
 * ========================================================================== */

} // namespace

int main()
{
    Config cfg;

    std::cout << "search_fj32_table — offline search for a one-base FJ-style uint32 table\n";
    std::cout << "prefilter fixed at primes <= " << kSmallPrimes.back() << "\n\n";

    std::mt19937_64 rng(cfg.seed);

    const auto basePool = makeBasePool16();
    const auto multipliers = makeMultiplierCandidates(cfg.multiplierCandidates, cfg.seed ^ 0x3141592653589793ULL);

    static constexpr std::array<uint32_t, 12> criticalBases{
        2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u, 29u, 31u, 61u
    };

    std::cout << "base pool size      : " << basePool.size() << "\n";
    std::cout << "multiplier count    : " << multipliers.size() << "\n";

    std::vector<uint32_t> training;
    std::unordered_set<uint32_t> trainingSeen;

    const auto seedTroublemakers = makeSeedTroublemakers();
    appendUniqueRange(training, trainingSeen, seedTroublemakers);

    const auto dynamicHard = collectDynamicHardPseudoprimes(
        std::vector<uint32_t>(criticalBases.begin(), criticalBases.end()),
        cfg.hardPseudoPerBase,
        cfg.hardPseudoAttemptsPerBase,
        rng
    );
    appendUniqueRange(training, trainingSeen, dynamicHard);

    const auto initialRandom = collectRandomUndecidedComposites(cfg.initialRandomTraining, rng);
    appendUniqueRange(training, trainingSeen, initialRandom);

    std::cout << "initial training set: " << training.size() << " composites\n\n";

    for (const unsigned bucketBits : cfg.bucketBitsToTry) {
        const uint32_t bucketCount = 1u << bucketBits;
        std::cout << "=== Trying " << bucketCount << " buckets ===\n";

        for (std::size_t pass = 0; pass < cfg.maxSearchPassesPerBucketCount; ++pass) {
            std::cout << "pass " << (pass + 1u) << "/" << cfg.maxSearchPassesPerBucketCount
                      << ", training size = " << training.size() << "\n";

            std::size_t multiplierIndex = 0;

            for (const uint32_t mult : multipliers) {
                ++multiplierIndex;

                if ((multiplierIndex % 128u) == 0u) {
                    std::cout << "  multiplier " << multiplierIndex << "/" << multipliers.size()
                              << "\r" << std::flush;
                }

                CandidateTable candidate;
                if (!synthesizeForMultiplier(bucketBits, mult, training, basePool, &candidate))
                    continue;

                // First fail-fast probe set: current training set.
                std::vector<uint32_t> cex;
                if (!verifyAgainstOracleSet(candidate, training, &cex, cfg.maxCounterexamplesKept)) {
                    appendUniqueRange(training, trainingSeen, cex);
                    continue;
                }

                // Second fail-fast probe: fresh random undecided composites.
                const auto freshRandom = collectRandomUndecidedComposites(cfg.probeRandomTraining, rng);
                cex.clear();
                if (!verifyAgainstOracleSet(candidate, freshRandom, &cex, cfg.maxCounterexamplesKept)) {
                    appendUniqueRange(training, trainingSeen, cex);
                    continue;
                }

                // Third fail-fast probe: freshly collected "hard" pseudoprimes to common bases.
                const auto freshHard = collectDynamicHardPseudoprimes(
                    std::vector<uint32_t>(criticalBases.begin(), criticalBases.end()),
                    16u,
                    50000u,
                    rng
                );
                cex.clear();
                if (!verifyAgainstOracleSet(candidate, freshHard, &cex, cfg.maxCounterexamplesKept)) {
                    appendUniqueRange(training, trainingSeen, cex);
                    continue;
                }

                std::cout << "\n  candidate survived probes"
                          << " (buckets=" << bucketCount
                          << ", multiplier=0x" << std::hex << std::setw(8) << std::setfill('0')
                          << mult << std::dec << ")\n";

                // Final full exhaustive verification over odd uint32_t via segmented sieve.
                cex.clear();
                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = exhaustiveVerify32(candidate,
                                                   cfg.exhaustiveSegmentOddCount,
                                                   &cex,
                                                   cfg.maxCounterexamplesKept);
                const auto t1 = std::chrono::steady_clock::now();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                if (ok) {
                    std::cout << "  exhaustive verification passed in " << ms << " ms\n";
                    printSolutionAsHeaderSnippet(candidate);
                    return EXIT_SUCCESS;
                }

                std::cout << "  exhaustive verification FAILED in " << ms << " ms";
                if (!cex.empty())
                    std::cout << " ; first counterexample = " << cex.front();
                std::cout << "\n";

                appendUniqueRange(training, trainingSeen, cex);
            }

            std::cout << "\n";
        }

        std::cout << "no verified solution found for " << bucketCount << " buckets\n\n";
    }

    std::cout << "No verified solution found for 64/128/256 buckets with current search budget.\n";
    return EXIT_FAILURE;
}
