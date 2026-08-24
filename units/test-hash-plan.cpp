// File: units/test-hash-plan.cpp
// Created by Andrea "Nemesi" Cocito on 24/08/2026
// Verify shared directory hash scoring and deterministic policy choices.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <femtofs/hash_plan.h>

TEST_CASE("hash planner grows through the shared strict-prime rule", "[hash-plan]")
{
    const std::vector<std::string> names{"a", "c"};
    constexpr std::array<uint32_t, 1> primes{3};
    const femtofs::hashplan::Choice choice =
        femtofs::hashplan::chooseSingle(names, primes);

    CHECK(femtofs::hashplan::nextPrimeStrict(4) == 5);
    CHECK(choice.tableSize == 3);
    CHECK(choice.prime == 3);
    CHECK(choice.sumSquares == 2);
    CHECK(choice.maxChain == 1);
    CHECK(choice.ceiling == 5);
}

TEST_CASE("single and dual scoring use deterministic chain metrics", "[hash-plan]")
{
    const std::vector<std::string> names{"a", "b", "c"};
    const femtofs::hashplan::Score single =
        femtofs::hashplan::scoreSingle(names, 3, 3);
    const femtofs::hashplan::Score dual =
        femtofs::hashplan::scoreDual(names, 3, 5, 3);

    CHECK(single.sumSquares == 3);
    CHECK(single.maxChain == 1);
    CHECK(dual.sumSquares == 3);
    CHECK(dual.maxChain == 1);
}

TEST_CASE("dual mode requires a strict per-directory quality improvement",
          "[hash-plan]")
{
    using femtofs::hashplan::Score;

    CHECK(femtofs::hashplan::improvesQuality(Score{12, 2}, Score{14, 2}));
    CHECK(femtofs::hashplan::improvesQuality(Score{20, 2}, Score{12, 3}));
    CHECK_FALSE(femtofs::hashplan::improvesQuality(Score{12, 3}, Score{12, 3}));
    CHECK_FALSE(femtofs::hashplan::improvesQuality(Score{14, 3}, Score{12, 3}));
}

TEST_CASE("prime candidate sampling is shared and reproducible", "[hash-plan]")
{
    const std::vector<uint32_t> first = femtofs::hashplan::samplePrimes(
        257, 1u << 24, 100, 0x0F5F2026u);
    const std::vector<uint32_t> second = femtofs::hashplan::samplePrimes(
        257, 1u << 24, 100, 0x0F5F2026u);

    REQUIRE(first.size() == 100);
    CHECK(first == second);
    for (const uint32_t prime : first) {
        CHECK(prime > (1u << 8));
        CHECK(prime < (1u << 24));
        CHECK(utilities::isPrime(prime));
    }
}

// END File: units/test-hash-plan.cpp
