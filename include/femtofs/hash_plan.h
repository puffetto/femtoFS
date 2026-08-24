// File: include/femtofs/hash_plan.h
// Created by Andrea "Nemesi" Cocito on 24/08/2026
// Shared deterministic directory hash scoring and selection.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <femtofs/format.h>
#include <isPrime.h>

namespace femtofs::hashplan {

struct Score {
    uint64_t sumSquares = 0;
    uint32_t maxChain = 0;
};

struct Choice {
    uint32_t tableSize = 0;
    uint32_t primeIndex = 0;
    uint32_t prime = 0;
    uint64_t sumSquares = 0;
    uint32_t maxChain = 0;
    bool dual = false;
    bool hitCeiling = false;
    uint32_t ceiling = 0;
};

[[nodiscard]] inline uint32_t nextPrimeStrict(uint32_t value)
{
    if (value < 2)
        return 2;
    uint32_t candidate = value + 1u;
    if (candidate > 2 && (candidate & 1u) == 0)
        ++candidate;
    while (!utilities::isPrime(candidate))
        candidate += 2u;
    return candidate;
}

[[nodiscard]] inline Score scoreSingle(
    const std::vector<std::string>& names, uint32_t prime, uint32_t tableSize)
{
    std::vector<uint32_t> counts(tableSize, 0);
    for (const std::string& name : names)
        ++counts[femtofs::hashName(name, prime, tableSize)];

    Score score;
    for (const uint32_t count : counts) {
        score.sumSquares += static_cast<uint64_t>(count) * count;
        score.maxChain = std::max(score.maxChain, count);
    }
    return score;
}

[[nodiscard]] inline Score scoreDual(
    const std::vector<std::string>& names, uint32_t prime1, uint32_t prime2,
    uint32_t tableSize)
{
    std::vector<uint32_t> counts(tableSize, 0);
    for (const std::string& name : names) {
        const uint32_t h1 = femtofs::hashName(name, prime1, tableSize);
        const uint32_t h2 = femtofs::hashName(name, prime2, tableSize);
        const uint32_t chosen = counts[h2] < counts[h1] ||
                (counts[h2] == counts[h1] && h2 < h1) ? h2 : h1;
        ++counts[chosen];
    }

    Score score;
    for (const uint32_t count : counts) {
        score.sumSquares += static_cast<uint64_t>(count) * count;
        score.maxChain = std::max(score.maxChain, count);
    }
    return score;
}

[[nodiscard]] inline Choice bestSingleForTable(
    const std::vector<std::string>& names, std::span<const uint32_t> primes,
    uint32_t tableSize)
{
    if (names.empty())
        return {};
    if (names.size() == 1)
        return Choice{tableSize, 0, 0, 1, 1, false, false, tableSize};
    if (primes.empty())
        throw std::invalid_argument("directory hash planner has no base primes");

    Choice best{tableSize, 0, primes.front(), UINT64_MAX, UINT32_MAX,
                false, false, tableSize};
    for (size_t index = 0; index < primes.size(); ++index) {
        const Score score = scoreSingle(names, primes[index], tableSize);
        if (score.sumSquares < best.sumSquares) {
            best.primeIndex = static_cast<uint32_t>(index);
            best.prime = primes[index];
            best.sumSquares = score.sumSquares;
            best.maxChain = score.maxChain;
        }
    }
    return best;
}

[[nodiscard]] inline Choice chooseSingle(
    const std::vector<std::string>& names, std::span<const uint32_t> primes)
{
    const uint32_t entries = static_cast<uint32_t>(names.size());
    if (entries == 0)
        return {};
    if (entries == 1)
        return Choice{1, 0, 0, 1, 1, false, false, 1};
    if (primes.empty())
        throw std::invalid_argument("directory hash planner has no base primes");

    const uint64_t doubled = static_cast<uint64_t>(entries) * 2u;
    uint32_t ceiling = femtofs::kMaxTableSize;
    if (doubled < femtofs::kMaxTableSize) {
        ceiling = std::min(nextPrimeStrict(static_cast<uint32_t>(doubled)),
                           femtofs::kMaxTableSize);
    }

    Choice best{entries, 0, primes.front(), UINT64_MAX, UINT32_MAX,
                false, false, ceiling};
    uint32_t tableSize = entries;
    for (;;) {
        for (size_t index = 0; index < primes.size(); ++index) {
            const Score score = scoreSingle(names, primes[index], tableSize);
            if (score.sumSquares == entries) {
                return Choice{tableSize, static_cast<uint32_t>(index),
                              primes[index], score.sumSquares,
                              score.maxChain, false, false, ceiling};
            }
            if (score.sumSquares < best.sumSquares) {
                best = Choice{tableSize, static_cast<uint32_t>(index),
                              primes[index], score.sumSquares,
                              score.maxChain, false, false, ceiling};
            }
        }
        if (static_cast<long double>(best.sumSquares) /
                static_cast<long double>(entries) < 1.1L) {
            return best;
        }
        const uint32_t next = nextPrimeStrict(tableSize);
        if (next > ceiling) {
            best.hitCeiling = true;
            return best;
        }
        tableSize = next;
    }
}

[[nodiscard]] inline Choice chooseDual(
    const std::vector<std::string>& names, std::span<const uint32_t> primes,
    uint32_t prime2)
{
    const uint32_t entries = static_cast<uint32_t>(names.size());
    if (entries < 2)
        return chooseSingle(names, primes);
    if (primes.empty())
        throw std::invalid_argument("directory hash planner has no base primes");

    const uint64_t doubled = static_cast<uint64_t>(entries) * 2u;
    uint32_t ceiling = femtofs::kMaxTableSize;
    if (doubled < femtofs::kMaxTableSize) {
        ceiling = std::min(nextPrimeStrict(static_cast<uint32_t>(doubled)),
                           femtofs::kMaxTableSize);
    }

    Choice best{entries, 0, primes.front(), UINT64_MAX, UINT32_MAX,
                true, false, ceiling};
    uint32_t tableSize = entries;
    for (;;) {
        for (size_t index = 0; index < primes.size(); ++index) {
            const Score score = scoreDual(
                names, primes[index], prime2, tableSize);
            const bool better = score.maxChain < best.maxChain ||
                (score.maxChain == best.maxChain &&
                 score.sumSquares < best.sumSquares) ||
                (score.maxChain == best.maxChain &&
                 score.sumSquares == best.sumSquares &&
                 tableSize < best.tableSize) ||
                (score.maxChain == best.maxChain &&
                 score.sumSquares == best.sumSquares &&
                 tableSize == best.tableSize && primes[index] < best.prime);
            if (better) {
                best = Choice{tableSize, static_cast<uint32_t>(index),
                              primes[index], score.sumSquares,
                              score.maxChain, true, false, ceiling};
            }
        }
        if (best.maxChain <= 1)
            break;
        const uint32_t next = nextPrimeStrict(tableSize);
        if (next > ceiling)
            break;
        tableSize = next;
    }
    best.hitCeiling = best.tableSize == ceiling &&
        static_cast<long double>(best.sumSquares) /
            static_cast<long double>(entries) >= 1.1L;
    return best;
}

[[nodiscard]] inline bool improvesQuality(const Score& candidate,
                                          const Score& baseline)
{
    return candidate.maxChain < baseline.maxChain ||
        (candidate.maxChain == baseline.maxChain &&
         candidate.sumSquares < baseline.sumSquares);
}

[[nodiscard]] inline bool improvesQuality(const Choice& candidate,
                                          const Choice& baseline)
{
    return improvesQuality(Score{candidate.sumSquares, candidate.maxChain},
                           Score{baseline.sumSquares, baseline.maxChain});
}

[[nodiscard]] inline std::vector<uint32_t> samplePrimes(
    uint32_t minimum, uint32_t maximum, size_t count, uint32_t seed)
{
    std::vector<uint32_t> primes;
    if (minimum >= maximum || count == 0)
        return primes;

    std::mt19937 random(seed);
    std::uniform_int_distribution<uint32_t> distribution(minimum, maximum - 1u);
    std::set<uint32_t> seen;
    primes.reserve(count);
    while (primes.size() < count) {
        uint32_t candidate = distribution(random) | 1u;
        if (candidate >= maximum)
            candidate -= 2u;
        if (utilities::isPrime(candidate) && seen.insert(candidate).second)
            primes.push_back(candidate);
    }
    return primes;
}

} // namespace femtofs::hashplan

// END File: include/femtofs/hash_plan.h
