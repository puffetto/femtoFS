// File: units/test-isPrime.cpp
// Unit tests for include/utilities/isPrime.h

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "isPrime.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace {

template <typename UInt>
concept ForwardedUnsigned =
    std::unsigned_integral<UInt> &&
    (!std::same_as<UInt, bool>) &&
    (!std::same_as<UInt, uint32_t>) &&
    (!std::same_as<UInt, uint64_t>);

template <ForwardedUnsigned UInt>
void checkForwardedUnsignedPrimeComposite(const UInt primeCandidate,
                                          const UInt compositeCandidate)
{
    REQUIRE(utilities::isPrime(primeCandidate));
    REQUIRE_FALSE(utilities::isPrime(compositeCandidate));
}

template <typename UInt>
void maybeCheckWiderForwardedUnsigned(bool &exercisedWiderForwarding)
{
    if constexpr (ForwardedUnsigned<UInt> &&
                  (sizeof(UInt) > sizeof(uint32_t))) {
        exercisedWiderForwarding = true;
        checkForwardedUnsignedPrimeComposite<UInt>(
            static_cast<UInt>(97),
            static_cast<UInt>(100)
        );
    }
}

struct ExhaustivePrimeHashResult {
    uint64_t primeCount{};
    std::string sha256Hex;
    double elapsedSeconds{};
};

[[nodiscard]] std::string toHex(const std::array<unsigned char, 32> &digest)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const unsigned char b : digest)
        oss << std::setw(2) << static_cast<unsigned>(b);
    return oss.str();
}

[[nodiscard]] ExhaustivePrimeHashResult hashAllUint32Primes()
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == nullptr)
        throw std::runtime_error("EVP_MD_CTX_new failed");
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestInit_ex failed");
    }

    constexpr uint64_t limit = (1ULL << 32);
    uint64_t primeCount = 0;

    const auto t0 = std::chrono::steady_clock::now();

    for (uint64_t n = 0; n < limit; ++n) {
        const uint32_t v = static_cast<uint32_t>(n);
        if (!utilities::isPrime(v))
            continue;

        ++primeCount;
        const unsigned char be[4]{
            static_cast<unsigned char>((v >> 24u) & 0xFFu),
            static_cast<unsigned char>((v >> 16u) & 0xFFu),
            static_cast<unsigned char>((v >> 8u) & 0xFFu),
            static_cast<unsigned char>((v >> 0u) & 0xFFu)
        };
        if (EVP_DigestUpdate(ctx, be, sizeof(be)) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestUpdate failed");
        }
    }

    std::array<unsigned char, 32> digest{};
    unsigned outLen = 0;
    if (EVP_DigestFinal_ex(ctx, digest.data(), &outLen) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestFinal_ex failed");
    }
    if (outLen != digest.size()) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("unexpected SHA-256 digest length");
    }
    EVP_MD_CTX_free(ctx);

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(t1 - t0).count();

    return {primeCount, toHex(digest), elapsed};
}

} // namespace

static_assert(std::invocable<decltype(utilities::isPrime), signed char>);
static_assert(std::invocable<decltype(utilities::isPrime), short>);
static_assert(std::invocable<decltype(utilities::isPrime), int>);
static_assert(std::invocable<decltype(utilities::isPrime), long>);
static_assert(std::invocable<decltype(utilities::isPrime), long long>);
static_assert(std::invocable<decltype(utilities::isPrime), uint8_t>);
static_assert(std::invocable<decltype(utilities::isPrime), uint16_t>);
static_assert(std::invocable<decltype(utilities::isPrime), uint32_t>);
static_assert(std::invocable<decltype(utilities::isPrime), uint64_t>);
static_assert(!std::invocable<decltype(utilities::isPrime), bool>);

TEST_CASE("isPrime covers uint32 prefilter and FJ32_256 paths", "[isPrime]")
{
    using utilities::isPrime;

    REQUIRE_FALSE(isPrime(uint32_t{0}));
    REQUIRE_FALSE(isPrime(uint32_t{1}));
    REQUIRE(isPrime(uint32_t{2}));
    REQUIRE_FALSE(isPrime(uint32_t{4}));

    REQUIRE(isPrime(uint32_t{311}));
    REQUIRE_FALSE(isPrime(uint32_t{309}));
    REQUIRE_FALSE(isPrime(uint32_t{939}));
    REQUIRE(isPrime(uint32_t{313}));
    REQUIRE_FALSE(isPrime(uint32_t{96721})); // 311^2

    REQUIRE(isPrime(uint32_t{1000003}));     // Undecided -> FJ32_256 -> prime
    REQUIRE_FALSE(isPrime(uint32_t{1022117})); // Undecided -> FJ32_256 -> composite
}

TEST_CASE("isPrime covers uint64 delegation, prefilter, and Miller-Rabin", "[isPrime]")
{
    using utilities::isPrime;

    REQUIRE(isPrime(uint64_t{97})); // Delegates to uint32 overload
    REQUIRE_FALSE(isPrime(uint64_t{std::numeric_limits<uint32_t>::max()})); // Delegates

    REQUIRE_FALSE(isPrime(uint64_t{4294967301ULL})); // > 32-bit, prefilter composite (divisible by 3)
    REQUIRE_FALSE(isPrime(uint64_t{18446744073709551556ULL})); // > 32-bit, even composite

    REQUIRE_FALSE(isPrime(uint64_t{4294967297ULL})); // > 32-bit, MR64 composite
    REQUIRE(isPrime(uint64_t{18446744073709551557ULL})); // > 32-bit, MR64 prime
}

TEST_CASE("isPrime covers signed forwarding overload", "[isPrime]")
{
    using utilities::isPrime;

    REQUIRE_FALSE(isPrime(static_cast<signed char>(-7)));
    REQUIRE_FALSE(isPrime(static_cast<short>(0)));
    REQUIRE_FALSE(isPrime(1));

    REQUIRE(isPrime(static_cast<signed char>(97)));
    REQUIRE(isPrime(static_cast<short>(97)));
    REQUIRE(isPrime(97));
    REQUIRE(isPrime(static_cast<long>(97)));
    REQUIRE(isPrime(static_cast<long long>(97)));
}

TEST_CASE("isPrime covers unsigned forwarding overload", "[isPrime]")
{
    checkForwardedUnsignedPrimeComposite<uint8_t>(uint8_t{97}, uint8_t{100});
    checkForwardedUnsignedPrimeComposite<uint16_t>(uint16_t{97}, uint16_t{100});

    bool exercisedWiderForwarding = false;

    maybeCheckWiderForwardedUnsigned<unsigned long>(exercisedWiderForwarding);
    maybeCheckWiderForwardedUnsigned<unsigned long long>(exercisedWiderForwarding);
    maybeCheckWiderForwardedUnsigned<std::size_t>(exercisedWiderForwarding);

    if (!exercisedWiderForwarding)
        SUCCEED("No distinct forwarded unsigned type wider than uint32_t on this ABI");
}

TEST_CASE("isPrime internal helpers can be directly exercised", "[isPrime][internal]")
{
    using Prime = utilities::PrimalityTestFunctor;
    using PrefilterResult = Prime::PrefilterResult;

    REQUIRE(Prime::prefilter<uint64_t>(uint64_t{313}) == PrefilterResult::prime);
    REQUIRE(Prime::prefilter<uint64_t>(uint64_t{4294967297ULL}) == PrefilterResult::undecided);
    REQUIRE(Prime::prefilter<uint64_t>(uint64_t{4294967301ULL}) == PrefilterResult::composite);

    {
        constexpr uint32_t n = 61;
        const auto mulMod = [](const uint32_t a, const uint32_t b) noexcept -> uint32_t {
            return static_cast<uint32_t>(
                (static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) % n
            );
        };

        REQUIRE(Prime::powMod<uint32_t>(uint32_t{7}, uint32_t{0}, mulMod) == uint32_t{1});
        REQUIRE(Prime::powMod<uint32_t>(uint32_t{7}, uint32_t{1}, mulMod) == uint32_t{7});
        REQUIRE(Prime::powMod<uint32_t>(uint32_t{7}, uint32_t{20}, mulMod) == uint32_t{47});

        REQUIRE_FALSE(Prime::isWitness<uint32_t>(
            n, uint32_t{15}, 2u, n, mulMod
        )); // a %= n => 0 path

        REQUIRE_FALSE(Prime::isWitness<uint32_t>(
            n, uint32_t{15}, 2u, uint32_t{2}, mulMod
        )); // x == 1 or n - 1 path
    }

    {
        constexpr uint32_t n = 15;
        const auto mulMod = [](const uint32_t a, const uint32_t b) noexcept -> uint32_t {
            return static_cast<uint32_t>(
                (static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) % n
            );
        };

        REQUIRE(Prime::isWitness<uint32_t>(
            n, uint32_t{7}, 1u, uint32_t{2}, mulMod
        )); // witness path
    }
}

TEST_CASE("isPrime exhaustive uint32 prime-stream hash", "[isPrime][exhaustive][slow]")
{
    constexpr uint64_t kExpectedPrimeCount = 203280221ULL;
    constexpr std::string_view kExpectedSha256 =
        "60a21e2a7397a9e3ad8bc09ccff80a44ccde2c5018432e2bb94cf0fc72130e44";

    const auto result = hashAllUint32Primes();
    INFO("elapsedSeconds=" << result.elapsedSeconds);

    REQUIRE(result.primeCount == kExpectedPrimeCount);
    REQUIRE(result.sha256Hex == kExpectedSha256);
}

// END File: units/test-isPrime.cpp
