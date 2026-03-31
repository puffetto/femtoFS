//
// Created by Andrea Cocito on 14/03/26.
//
// File: tools/search_fj32_parallel.cpp
//
// Offline threaded search tool for a custom FJ-style uint32_t backend
// compatible with the current utilities::isPrime prefilter up to 311.
//
// The current header include/utilities/isPrime.h is used as an oracle for
// training/probing only. This tool does not modify that header.

#include <utilities/isPrime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
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

inline static constexpr std::array<uint16_t, 63> kSmallPrimes{
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
 *  Single-base SPRP (strong probable-prime) test for uint32_t
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
        exp >>= 1u;
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
    unsigned s = 0u;

    while ((d & 1u) == 0u) {
        d >>= 1u;
        ++s;
    }

    uint32_t x = powMod32(a, d, n);

    if (x == 1u || x == n - 1u)
        return true;

    for (unsigned r = 1u; r < s; ++r) {
        x = mulMod32(x, x, n);
        if (x == n - 1u)
            return true;
    }

    return false;
}

/* ============================================================================
 *  Candidate table and runtime
 * ========================================================================== */

struct CandidateTable {
    unsigned bucketBits = 0u;              // 6, 7, 8 => 64, 128, 256 buckets
    uint32_t multiplier = 0u;              // odd hash multiplier
    std::vector<uint16_t> bucketBase;      // size = 1u << bucketBits
};

[[nodiscard]] uint32_t hashBucket(uint32_t n,
                                  uint32_t multiplier,
                                  unsigned bucketBits) noexcept
{
    // Multiply in the uint32_t ring, then take the top bucketBits.
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
 *  Small helpers
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
    std::vector<uint8_t> composite(limit + 1u, 0u);
    std::vector<uint16_t> primes;

    for (uint32_t p = 2u; p <= limit; ++p) {
        if (composite[p] != 0u)
            continue;

        primes.push_back(static_cast<uint16_t>(p));

        if (p <= limit / p) {
            for (uint32_t q = p * p; q <= limit; q += p)
                composite[q] = 1u;
        }
    }

    return primes;
}

[[nodiscard]] std::vector<uint16_t> makeBasePool16()
{
    return sievePrimes16(65535u);
}

[[nodiscard]] std::vector<uint32_t> makeMultiplierCandidates(std::size_t randomCount,
                                                             uint64_t seed)
{
    std::vector<uint32_t> out{
        0x9e3779b1u, 0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2du,
        0x165667b1u, 0xd2511f53u, 0xcd9e8d57u, 0x94d049bbu,
        0xf1357ae5u, 0x7feb352du, 0x846ca68bu, 0x5bd1e995u,
        0x47219879u, 0xb8786589u, 0x2687cf3bu, 0x1beb4605u
    };

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint32_t> dist(
        1u, std::numeric_limits<uint32_t>::max()
    );

    std::unordered_set<uint32_t> seen(out.begin(), out.end());

    while (out.size() < randomCount) {
        uint32_t m = dist(rng) | 1u; // odd only
        if (seen.insert(m).second)
            out.push_back(m);
    }

    return out;
}

[[nodiscard]] bool isUndecidedCompositeOracle(uint32_t n)
{
    return prefilter32(n) == PrefilterResult::undecided && !utilities::isPrime(n);
}

[[nodiscard]] std::vector<uint32_t> collectRandomUndecidedComposites(std::size_t count,
                                                                     std::mt19937_64 &rng)
{
    std::vector<uint32_t> out;
    std::unordered_set<uint32_t> seen;
    out.reserve(count);

    std::uniform_int_distribution<uint32_t> dist(0u, std::numeric_limits<uint32_t>::max());

    while (out.size() < count) {
        const uint32_t n = dist(rng) | 1u;
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
        std::size_t found = 0u;

        for (std::size_t attempts = 0u;
             attempts < maxAttemptsPerBase && found < wantPerBase;
             ++attempts) {
            const uint32_t n = dist(rng) | 1u;

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

/* ============================================================================
 *  Prefix truth table
 * ========================================================================== */

struct PrefixTruth {
    uint32_t limit = 0u;
    std::vector<uint8_t> isPrime; // index by value, 1 => prime
};

[[nodiscard]] PrefixTruth makePrefixTruth(uint32_t limit)
{
    PrefixTruth out;
    out.limit = limit;
    out.isPrime.assign(static_cast<std::size_t>(limit) + 1u, 1u);

    if (limit >= 0u) out.isPrime[0] = 0u;
    if (limit >= 1u) out.isPrime[1] = 0u;

    for (uint32_t p = 2u; p <= limit / p; ++p) {
        if (out.isPrime[p] == 0u)
            continue;
        for (uint32_t q = p * p; q <= limit; q += p)
            out.isPrime[q] = 0u;
    }

    return out;
}

/* ============================================================================
 *  Seed counterexamples I/O
 * ========================================================================== */

[[nodiscard]] std::optional<uint32_t> parseFirstUint32FromLine(const std::string &line)
{
    const char *s = line.c_str();
    char *end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || errno != 0 || v > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    return static_cast<uint32_t>(v);
}

[[nodiscard]] std::vector<uint32_t> loadCounterexamplesFile(const std::string &path)
{
    std::ifstream in(path);
    std::vector<uint32_t> out;

    if (!in)
        return out;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        if (line[0] == '#')
            continue;

        // Accept either:
        //   12345
        // or:
        //   CEX 12345
        std::string payload = line;
        if (payload.rfind("CEX", 0) == 0) {
            payload.erase(0, 3);
            while (!payload.empty() && std::isspace(static_cast<unsigned char>(payload.front())))
                payload.erase(payload.begin());
        }

        const auto parsed = parseFirstUint32FromLine(payload);
        if (parsed.has_value())
            out.push_back(*parsed);
    }

    return out;
}

void appendCounterexamplesFile(const std::string &path, const std::vector<uint32_t> &cex)
{
    if (path.empty() || cex.empty())
        return;

    std::ofstream out(path, std::ios::app);
    if (!out)
        return;

    for (const uint32_t n : cex)
        out << "CEX " << n << '\n';
}

/* ============================================================================
 *  Candidate synthesis
 * ========================================================================== */

[[nodiscard]] bool findBucketBase(const std::vector<uint32_t> &bucketComposites,
                                  const std::vector<uint16_t> &basePool,
                                  uint16_t *selectedBase)
{
    if (bucketComposites.empty()) {
        *selectedBase = 2u;
        return true;
    }

    static constexpr std::array<uint16_t, 28> shortList{
        2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u,
        23u, 29u, 31u, 37u, 41u, 43u, 47u, 53u,
        59u, 61u, 67u, 71u, 73u, 79u, 83u, 89u,
        97u, 101u, 103u, 107u
    };

    const auto testBase = [&](uint32_t a) -> bool {
        for (const uint32_t n : bucketComposites) {
            if (sprp32(n, a))
                return false;
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

    for (uint32_t b = 0u; b < bucketCount; ++b) {
        uint16_t base = 2u;
        if (!findBucketBase(buckets[b], basePool, &base))
            return false;
        candidate.bucketBase[b] = base;
    }

    *out = std::move(candidate);
    return true;
}

/* ============================================================================
 *  Validation
 * ========================================================================== */

[[nodiscard]] bool validateAgainstCompositeProbeSet(const CandidateTable &table,
                                                    const std::vector<uint32_t> &probeComposites,
                                                    uint32_t *firstCounterexample)
{
    for (const uint32_t n : probeComposites) {
        if (candidateIsPrime32(n, table)) {
            *firstCounterexample = n;
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool validatePrefix(const CandidateTable &table,
                                  const PrefixTruth &truth,
                                  uint32_t *firstCounterexample)
{
    for (uint32_t n = 0u; n <= truth.limit; ++n) {
        const bool wantPrime = truth.isPrime[n] != 0u;
        const bool gotPrime = candidateIsPrime32(n, table);

        if (wantPrime != gotPrime) {
            *firstCounterexample = n;
            return false;
        }

        if (n == std::numeric_limits<uint32_t>::max())
            break;
    }

    return true;
}

struct ExhaustiveResult {
    bool ok = false;
    uint32_t firstCounterexample = 0u;
};

[[nodiscard]] ExhaustiveResult exhaustiveVerify32From(const CandidateTable &table,
                                                      uint32_t verifiedPrefixLimit,
                                                      std::size_t segmentOddCount,
                                                      const std::vector<uint32_t> &oddPrimes)
{
    // All values <= verifiedPrefixLimit have already been checked exactly.
    uint64_t lo = verifiedPrefixLimit < 3u
        ? 3u
        : (static_cast<uint64_t>(verifiedPrefixLimit) + 1u);

    if ((lo & 1u) == 0u)
        ++lo;

    const uint64_t maxOdd = std::numeric_limits<uint32_t>::max();
    std::vector<uint8_t> composite(segmentOddCount, 0u);

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

        for (std::size_t i = 0u; i < count; ++i) {
            const uint32_t n = static_cast<uint32_t>(lo + 2u * static_cast<uint64_t>(i));
            const bool wantPrime = (composite[i] == 0u);
            const bool gotPrime = candidateIsPrime32(n, table);

            if (wantPrime != gotPrime) {
                return ExhaustiveResult{false, n};
            }
        }

        if (hi == maxOdd)
            break;

        lo = hi + 2u;
    }

    return ExhaustiveResult{true, 0u};
}

/* ============================================================================
 *  Config and parsing
 * ========================================================================== */

struct Config {
    std::vector<unsigned> bucketBitsToTry{6u, 7u, 8u}; // 64, 128, 256
    std::size_t multiplierCount = 4012u;
    std::size_t batchSize = 256u;
    std::size_t initialRandomTraining = 50000u;
    std::size_t hardPseudoPerBase = 128u;
    std::size_t hardPseudoAttemptsPerBase = 200000u;
    uint32_t prefixLimit = 10000000u;
    std::size_t exhaustiveSegmentOddCount = 1u << 22;
    unsigned workerThreads = std::max(1u, std::thread::hardware_concurrency());
    unsigned exhaustiveThreads = 1u;
    uint64_t seed = 0x7f8d9c0b12345678ULL;
    std::vector<std::string> seedCounterexampleFiles;
    std::string saveCounterexamplesPath;
};

[[nodiscard]] std::vector<unsigned> parseBucketBitsSpec(const std::string &spec)
{
    std::vector<unsigned> out;
    std::stringstream ss(spec);
    std::string item;

    while (std::getline(ss, item, ',')) {
        if (item.empty())
            continue;

        const unsigned long long raw = std::strtoull(item.c_str(), nullptr, 10);
        if (raw == 64ull)
            out.push_back(6u);
        else if (raw == 128ull)
            out.push_back(7u);
        else if (raw == 256ull)
            out.push_back(8u);
        else if (raw == 6ull || raw == 7ull || raw == 8ull)
            out.push_back(static_cast<unsigned>(raw));
    }

    if (out.empty())
        out = {6u, 7u, 8u};

    return out;
}

void printUsage(const char *argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "Options:\n"
        << "  --buckets 64,128,256      Bucket counts to try (or bits 6,7,8)\n"
        << "  --threads N               Worker threads for synthesis/probes\n"
        << "  --exhaustive-threads N    Max concurrent full exhaustive verifications\n"
        << "  --multiplier-count N      Number of odd multipliers to try\n"
        << "  --batch-size N            Multipliers per epoch\n"
        << "  --prefix-limit N          Deterministic exact prefix validation limit\n"
        << "  --seed N                  RNG seed\n"
        << "  --seed-cex PATH           Repeatable; load known counterexamples from file\n"
        << "  --save-cex PATH           Append newly discovered counterexamples to file\n"
        << "  --initial-random N        Initial random undecided composites in corpus\n"
        << "  --hard-per-base N         Hard pseudoprimes per critical base\n"
        << "  --hard-attempts N         Max random attempts per critical base\n"
        << "  --segment-odds N          Odd values per exhaustive sieve segment\n";
}

[[nodiscard]] bool parseArgs(int argc, char **argv, Config *cfg)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto requireValue = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << '\n';
                std::exit(EXIT_FAILURE);
            }
            return argv[++i];
        };

        if (arg == "--buckets") {
            cfg->bucketBitsToTry = parseBucketBitsSpec(requireValue("--buckets"));
        } else if (arg == "--threads") {
            cfg->workerThreads = static_cast<unsigned>(std::strtoul(requireValue("--threads"), nullptr, 10));
        } else if (arg == "--exhaustive-threads") {
            cfg->exhaustiveThreads = static_cast<unsigned>(std::strtoul(requireValue("--exhaustive-threads"), nullptr, 10));
        } else if (arg == "--multiplier-count") {
            cfg->multiplierCount = static_cast<std::size_t>(std::strtoull(requireValue("--multiplier-count"), nullptr, 10));
        } else if (arg == "--batch-size") {
            cfg->batchSize = static_cast<std::size_t>(std::strtoull(requireValue("--batch-size"), nullptr, 10));
        } else if (arg == "--prefix-limit") {
            cfg->prefixLimit = static_cast<uint32_t>(std::strtoull(requireValue("--prefix-limit"), nullptr, 10));
        } else if (arg == "--seed") {
            cfg->seed = static_cast<uint64_t>(std::strtoull(requireValue("--seed"), nullptr, 10));
        } else if (arg == "--seed-cex") {
            cfg->seedCounterexampleFiles.emplace_back(requireValue("--seed-cex"));
        } else if (arg == "--save-cex") {
            cfg->saveCounterexamplesPath = requireValue("--save-cex");
        } else if (arg == "--initial-random") {
            cfg->initialRandomTraining = static_cast<std::size_t>(std::strtoull(requireValue("--initial-random"), nullptr, 10));
        } else if (arg == "--hard-per-base") {
            cfg->hardPseudoPerBase = static_cast<std::size_t>(std::strtoull(requireValue("--hard-per-base"), nullptr, 10));
        } else if (arg == "--hard-attempts") {
            cfg->hardPseudoAttemptsPerBase = static_cast<std::size_t>(std::strtoull(requireValue("--hard-attempts"), nullptr, 10));
        } else if (arg == "--segment-odds") {
            cfg->exhaustiveSegmentOddCount = static_cast<std::size_t>(std::strtoull(requireValue("--segment-odds"), nullptr, 10));
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            printUsage(argv[0]);
            return false;
        }
    }

    if (cfg->workerThreads == 0u)
        cfg->workerThreads = 1u;
    if (cfg->exhaustiveThreads == 0u)
        cfg->exhaustiveThreads = 1u;
    if (cfg->batchSize == 0u)
        cfg->batchSize = 1u;
    if (cfg->prefixLimit < 2u)
        cfg->prefixLimit = 2u;

    return true;
}

/* ============================================================================
 *  Snapshot
 * ========================================================================== */

struct ValidationSnapshot {
    std::vector<uint32_t> compositeProbes;
};

[[nodiscard]] ValidationSnapshot buildSnapshot(const std::vector<uint32_t> &persistentCorpus,
                                               const Config &cfg,
                                               uint64_t epochSeed)
{
    ValidationSnapshot snapshot;
    std::unordered_set<uint32_t> seen;

    snapshot.compositeProbes.reserve(
        persistentCorpus.size() + cfg.initialRandomTraining + cfg.hardPseudoPerBase * 12u
    );

    appendUniqueRange(snapshot.compositeProbes, seen, persistentCorpus);

    std::mt19937_64 rng(epochSeed);

    const auto randoms = collectRandomUndecidedComposites(cfg.initialRandomTraining, rng);
    appendUniqueRange(snapshot.compositeProbes, seen, randoms);

    static constexpr std::array<uint32_t, 12> criticalBases{
        2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u, 29u, 31u, 61u
    };

    const auto hard = collectDynamicHardPseudoprimes(
        std::vector<uint32_t>(criticalBases.begin(), criticalBases.end()),
        cfg.hardPseudoPerBase,
        cfg.hardPseudoAttemptsPerBase,
        rng
    );
    appendUniqueRange(snapshot.compositeProbes, seen, hard);

    return snapshot;
}

/* ============================================================================
 *  Permit pool for exhaustive stage
 * ========================================================================== */

class PermitPool
{
public:
    explicit PermitPool(unsigned permits)
        : permits_(permits)
    {
    }

    void acquire()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return permits_ > 0u; });
        --permits_;
    }

    void release()
    {
        {
            std::lock_guard lock(mutex_);
            ++permits_;
        }
        cv_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    unsigned permits_;
};

/* ============================================================================
 *  Worker runtime
 * ========================================================================== */

struct SharedState {
    std::atomic<std::size_t> nextJob{0u};
    std::atomic<std::size_t> jobsStarted{0u};
    std::atomic<std::size_t> jobsFinished{0u};
    std::atomic<std::size_t> synthesized{0u};
    std::atomic<std::size_t> passedProbeSet{0u};
    std::atomic<std::size_t> passedPrefix{0u};
    std::atomic<std::size_t> exhaustiveStarted{0u};
    std::atomic<bool> successFound{false};

    std::mutex logMutex;
    std::mutex successMutex;
    std::optional<CandidateTable> successCandidate;
};

struct WorkerAccum {
    std::vector<uint32_t> discoveredCounterexamples;
};

void logLine(SharedState &shared, const std::string &line)
{
    std::lock_guard lock(shared.logMutex);
    std::cout << line << std::flush;
}

void mergeDiscovered(std::vector<uint32_t> &dst,
                     std::unordered_set<uint32_t> &seen,
                     const std::vector<uint32_t> &src)
{
    for (const uint32_t x : src)
        appendUnique(dst, seen, x);
}

void workerRunEpoch(unsigned workerId,
                    unsigned bucketBits,
                    const std::vector<uint32_t> &jobs,
                    const ValidationSnapshot &snapshot,
                    const PrefixTruth &prefixTruth,
                    const std::vector<uint16_t> &basePool,
                    const std::vector<uint32_t> &oddPrimes,
                    const Config &cfg,
                    PermitPool &exhaustivePermits,
                    SharedState &shared,
                    WorkerAccum &accum)
{
    (void)workerId;

    while (!shared.successFound.load(std::memory_order_relaxed)) {
        const std::size_t idx = shared.nextJob.fetch_add(1u, std::memory_order_relaxed);
        if (idx >= jobs.size())
            break;

        ++shared.jobsStarted;
        const uint32_t multiplier = jobs[idx];

        CandidateTable candidate;
        if (!synthesizeForMultiplier(bucketBits, multiplier, snapshot.compositeProbes, basePool, &candidate)) {
            ++shared.jobsFinished;
            continue;
        }

        ++shared.synthesized;

        uint32_t cex = 0u;
        if (!validateAgainstCompositeProbeSet(candidate, snapshot.compositeProbes, &cex)) {
            accum.discoveredCounterexamples.push_back(cex);
            ++shared.jobsFinished;
            continue;
        }

        ++shared.passedProbeSet;

        if (!validatePrefix(candidate, prefixTruth, &cex)) {
            accum.discoveredCounterexamples.push_back(cex);
            ++shared.jobsFinished;
            continue;
        }

        ++shared.passedPrefix;

        {
            std::ostringstream oss;
            oss << "  candidate survived probes/prefix (buckets=" << (1u << bucketBits)
                << ", multiplier=0x"
                << std::hex << std::setw(8) << std::setfill('0') << multiplier
                << std::dec << ")\n";
            logLine(shared, oss.str());
        }

        exhaustivePermits.acquire();
        ++shared.exhaustiveStarted;

        const auto t0 = std::chrono::steady_clock::now();
        const ExhaustiveResult ex = exhaustiveVerify32From(
            candidate,
            prefixTruth.limit,
            cfg.exhaustiveSegmentOddCount,
            oddPrimes
        );
        const auto t1 = std::chrono::steady_clock::now();
        exhaustivePermits.release();

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (!ex.ok) {
            accum.discoveredCounterexamples.push_back(ex.firstCounterexample);

            std::ostringstream oss;
            oss << "  exhaustive verification FAILED (buckets=" << (1u << bucketBits)
                << ", multiplier=0x"
                << std::hex << std::setw(8) << std::setfill('0') << multiplier
                << std::dec << ") in " << ms
                << " ms ; first counterexample = " << ex.firstCounterexample << '\n';
            logLine(shared, oss.str());

            ++shared.jobsFinished;
            continue;
        }

        {
            std::lock_guard lock(shared.successMutex);
            if (!shared.successFound.load(std::memory_order_relaxed)) {
                shared.successCandidate = std::move(candidate);
                shared.successFound.store(true, std::memory_order_relaxed);
            }
        }

        {
            std::ostringstream oss;
            oss << "\n=== SUCCESS ===\n"
                << "bucket count : " << (1u << bucketBits) << '\n'
                << "bucket bits  : " << bucketBits << '\n'
                << "multiplier   : 0x"
                << std::hex << std::setw(8) << std::setfill('0')
                << multiplier << std::dec << '\n';
            logLine(shared, oss.str());
        }

        ++shared.jobsFinished;
        break;
    }
}

void progressThread(const std::size_t totalJobs,
                    const unsigned bucketBits,
                    SharedState &shared)
{
    using namespace std::chrono_literals;

    while (!shared.successFound.load(std::memory_order_relaxed)) {
        const std::size_t done = shared.jobsFinished.load(std::memory_order_relaxed);
        if (done >= totalJobs)
            break;

        {
            std::ostringstream oss;
            oss << "\r"
                << "  progress buckets=" << (1u << bucketBits)
                << " jobs " << done << "/" << totalJobs
                << " synth=" << shared.synthesized.load(std::memory_order_relaxed)
                << " passProbe=" << shared.passedProbeSet.load(std::memory_order_relaxed)
                << " passPrefix=" << shared.passedPrefix.load(std::memory_order_relaxed)
                << " exhaustive=" << shared.exhaustiveStarted.load(std::memory_order_relaxed)
                << "    ";
            logLine(shared, oss.str());
        }

        std::this_thread::sleep_for(1s);
    }

    logLine(shared, "\r");
}

/* ============================================================================
 *  Solution print
 * ========================================================================== */

void printSolutionAsHeaderSnippet(const CandidateTable &table)
{
    const uint32_t bucketCount = 1u << table.bucketBits;

    std::cout << "\nstatic constexpr uint32_t fj32Multiplier = 0x"
              << std::hex << std::setw(8) << std::setfill('0') << table.multiplier
              << "u;\n" << std::dec;

    std::cout << "static constexpr std::array<uint16_t, " << bucketCount << "> fj32Bases{\n    ";

    for (std::size_t i = 0u; i < table.bucketBase.size(); ++i) {
        std::cout << table.bucketBase[i] << 'u';
        if (i + 1u != table.bucketBase.size())
            std::cout << ", ";
        if ((i + 1u) % 16u == 0u && i + 1u != table.bucketBase.size())
            std::cout << "\n    ";
    }

    std::cout << "\n};\n";
}

/* ============================================================================
 *  Main
 * ========================================================================== */

} // namespace

int main(int argc, char **argv)
{
    Config cfg;
    if (!parseArgs(argc, argv, &cfg))
        return EXIT_SUCCESS;

    std::cout << "search_fj32_parallel — threaded offline search for one-base FJ-style uint32 table\n";
    std::cout << "prefilter fixed at primes <= " << kSmallPrimes.back() << '\n';
    std::cout << "threads=" << cfg.workerThreads
              << " exhaustive-threads=" << cfg.exhaustiveThreads
              << " multiplier-count=" << cfg.multiplierCount
              << " batch-size=" << cfg.batchSize
              << " prefix-limit=" << cfg.prefixLimit << "\n\n";

    const auto basePool = makeBasePool16();
    const auto multipliers = makeMultiplierCandidates(cfg.multiplierCount, cfg.seed ^ 0x3141592653589793ULL);

    std::vector<uint32_t> oddPrimes;
    {
        const auto primes16 = sievePrimes16(65535u);
        oddPrimes.reserve(primes16.size());
        for (const uint16_t p : primes16) {
            if (p >= 3u)
                oddPrimes.push_back(static_cast<uint32_t>(p));
        }
    }

    const PrefixTruth prefixTruth = makePrefixTruth(cfg.prefixLimit);

    std::vector<uint32_t> persistentCorpus;
    std::unordered_set<uint32_t> persistentSeen;

    // Built-in troublesome composites.
    {
        const auto builtin = makeSeedTroublemakers();
        appendUniqueRange(persistentCorpus, persistentSeen, builtin);
    }

    // Load user-provided seed counterexamples.
    for (const std::string &path : cfg.seedCounterexampleFiles) {
        const auto raw = loadCounterexamplesFile(path);
        std::size_t accepted = 0u;
        for (const uint32_t n : raw) {
            if (isUndecidedCompositeOracle(n)) {
                appendUnique(persistentCorpus, persistentSeen, n);
                ++accepted;
            }
        }
        std::cout << "loaded " << accepted << " valid seed counterexamples from " << path << '\n';
    }

    // Initial random/hard corpus.
    {
        std::mt19937_64 rng(cfg.seed);

        const auto randoms = collectRandomUndecidedComposites(cfg.initialRandomTraining, rng);
        appendUniqueRange(persistentCorpus, persistentSeen, randoms);

        static constexpr std::array<uint32_t, 12> criticalBases{
            2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u, 29u, 31u, 61u
        };

        const auto hard = collectDynamicHardPseudoprimes(
            std::vector<uint32_t>(criticalBases.begin(), criticalBases.end()),
            cfg.hardPseudoPerBase,
            cfg.hardPseudoAttemptsPerBase,
            rng
        );
        appendUniqueRange(persistentCorpus, persistentSeen, hard);
    }

    std::cout << "initial persistent corpus size = " << persistentCorpus.size() << " composites\n\n";

    for (const unsigned bucketBits : cfg.bucketBitsToTry) {
        const uint32_t bucketCount = 1u << bucketBits;
        std::cout << "=== Trying " << bucketCount << " buckets ===\n";

        for (std::size_t batchStart = 0u; batchStart < multipliers.size(); batchStart += cfg.batchSize) {
            const std::size_t batchEnd = std::min(batchStart + cfg.batchSize, multipliers.size());
            const std::vector<uint32_t> batch(multipliers.begin() + static_cast<std::ptrdiff_t>(batchStart),
                                              multipliers.begin() + static_cast<std::ptrdiff_t>(batchEnd));

            const uint64_t epochSeed = cfg.seed ^ (static_cast<uint64_t>(bucketBits) << 56u) ^ static_cast<uint64_t>(batchStart);
            const ValidationSnapshot snapshot = buildSnapshot(persistentCorpus, cfg, epochSeed);

            std::cout << "epoch buckets=" << bucketCount
                      << " multipliers [" << batchStart << ", " << batchEnd
                      << ") snapshot-size=" << snapshot.compositeProbes.size() << '\n';

            SharedState shared;
            PermitPool exhaustivePermits(cfg.exhaustiveThreads);

            std::vector<WorkerAccum> accum(cfg.workerThreads);
            std::vector<std::thread> workers;
            workers.reserve(cfg.workerThreads);

            std::thread progress(progressThread, batch.size(), bucketBits, std::ref(shared));

            for (unsigned t = 0u; t < cfg.workerThreads; ++t) {
                workers.emplace_back(workerRunEpoch,
                                     t,
                                     bucketBits,
                                     std::cref(batch),
                                     std::cref(snapshot),
                                     std::cref(prefixTruth),
                                     std::cref(basePool),
                                     std::cref(oddPrimes),
                                     std::cref(cfg),
                                     std::ref(exhaustivePermits),
                                     std::ref(shared),
                                     std::ref(accum[t]));
            }

            for (auto &th : workers)
                th.join();

            progress.join();

            if (shared.successFound.load(std::memory_order_relaxed)) {
                std::cout << "\nVerified solution found.\n";
                printSolutionAsHeaderSnippet(*shared.successCandidate);
                return EXIT_SUCCESS;
            }

            // Merge discovered counterexamples into the persistent corpus.
            std::vector<uint32_t> newCounterexamples;
            std::unordered_set<uint32_t> newSeen;
            for (const WorkerAccum &wa : accum)
                mergeDiscovered(newCounterexamples, newSeen, wa.discoveredCounterexamples);

            std::size_t acceptedNew = 0u;
            std::vector<uint32_t> toPersist;
            toPersist.reserve(newCounterexamples.size());

            for (const uint32_t n : newCounterexamples) {
                if (!isUndecidedCompositeOracle(n))
                    continue;

                if (persistentSeen.insert(n).second) {
                    persistentCorpus.push_back(n);
                    toPersist.push_back(n);
                    ++acceptedNew;
                }
            }

            appendCounterexamplesFile(cfg.saveCounterexamplesPath, toPersist);

            std::cout << "epoch done: jobs=" << batch.size()
                      << " synthesized=" << shared.synthesized.load(std::memory_order_relaxed)
                      << " passProbe=" << shared.passedProbeSet.load(std::memory_order_relaxed)
                      << " passPrefix=" << shared.passedPrefix.load(std::memory_order_relaxed)
                      << " exhaustive=" << shared.exhaustiveStarted.load(std::memory_order_relaxed)
                      << " new-cex=" << acceptedNew
                      << " persistent-corpus=" << persistentCorpus.size()
                      << "\n\n";
        }

        std::cout << "No verified solution found for " << bucketCount << " buckets.\n\n";
    }

    std::cout << "No verified solution found for any requested bucket count.\n";
    return EXIT_FAILURE;
}
