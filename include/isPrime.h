// File: include/utilities/isPrime.h
// Created by Andrea Cocito on 14/03/26.
//

#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <limits>

#if !defined(__SIZEOF_INT128__)
#error "utilities::isPrime requires compiler support for unsigned __int128 in the uint64_t overload."
#endif

/**
 * @file isPrime.h
 * @brief Header-only deterministic primality test for integral values up to 64 bits.
 */

namespace utilities {

/**
 * @brief Stateless deterministic primality-test functor backing @ref utilities::isPrime.
 *
 * The intended public API is the global object @ref utilities::isPrime, used as:
 *
 * @code{.cpp}
 * #include <utilities/isPrime.h>
 * #include <cstdint>
 *
 * bool a = utilities::isPrime(uint32_t{97});                      // true
 * bool b = utilities::isPrime(uint32_t{100});                     // false
 * bool c = utilities::isPrime(uint64_t{18446744073709551557ULL}); // true
 * bool d = utilities::isPrime(uint64_t{18446744073709551556ULL}); // false
 * @endcode
 *
 * Internally, the implementation is split into two stages:
 *
 * - a shared prefilter based on:
 *   - small-value handling,
 *   - parity rejection,
 *   - trial division by a fixed table of small odd primes,
 *   - and a final shortcut based on the square of the largest small prime;
 * - a deterministic second stage:
 *   - FJ32_256 (hash + one SPRP base) for @c uint32_t,
 *   - 7-base Miller-Rabin for @c uint64_t.
 *
 * Properties:
 *
 * - no dynamic allocation;
 * - one shared compile-time small-prime table;
 * - deterministic over the full @c uint32_t and @c uint64_t domains;
 * - the @c uint64_t overload delegates to the @c uint32_t overload whenever
 *   the input fits in 32 bits.
 *
 * @note The @c uint64_t overload uses @c unsigned __int128 for exact modular
 *       multiplication without overflow.
 */
class PrimalityTestFunctor final
{
public:
    /**
     * @brief Disallow boolean inputs explicitly.
     */
    [[nodiscard]] bool operator()(bool) const noexcept = delete;

    /**
     * @brief Forwarding overload for signed integer types.
     *
     * Negative values, 0 and 1 are not prime and therefore return false.
     * Positive values are widened to uint32_t/uint64_t and forwarded to
     * the unsigned overloads.
     */
    template <std::signed_integral Int>
    [[nodiscard]] bool operator()(Int n) const noexcept
    {
        if (n < 2)
            return false;

        if constexpr (sizeof(Int) <= sizeof(uint32_t)) {
            return (*this)(static_cast<uint32_t>(n));
        } else if constexpr (sizeof(Int) <= sizeof(uint64_t)) {
            return (*this)(static_cast<uint64_t>(n));
        } else {
            static_assert(sizeof(Int) <= sizeof(uint64_t),
                          "utilities::isPrime supports signed integers up to 64 bits");
            return false;
        }
    }

    /**
     * @brief Forwarding overload for unsigned integer types other than uint32_t/uint64_t.
     *
     * Values up to 32 bits are widened to @c uint32_t, values up to 64 bits to
     * @c uint64_t. Wider unsigned types are rejected at compile time.
     */
    template <std::unsigned_integral UInt>
    requires (!std::same_as<UInt, bool> &&
              !std::same_as<UInt, uint32_t> &&
              !std::same_as<UInt, uint64_t>)
    [[nodiscard]] bool operator()(UInt n) const noexcept
    {
        if constexpr (sizeof(UInt) <= sizeof(uint32_t)) {
            return (*this)(static_cast<uint32_t>(n));
        } else if constexpr (sizeof(UInt) <= sizeof(uint64_t)) {
            return (*this)(static_cast<uint64_t>(n));
        } else {
            static_assert(sizeof(UInt) <= sizeof(uint64_t),
                          "utilities::isPrime supports unsigned integers up to 64 bits");
            return false;
        }
    }

    /**
     * @brief Test whether a 32-bit unsigned integer is prime.
     *
     * This overload is deterministic over the full @c uint32_t domain.
     *
     * @param n Value to test.
     * @return @c true if @p n is prime, @c false otherwise.
     */
    [[nodiscard]] bool operator()(uint32_t n) const noexcept
    {
        switch (prefilter<uint32_t>(n)) {
            case PrefilterResult::prime:
                return true;

            case PrefilterResult::composite:
                return false;

            case PrefilterResult::undecided:
                break;
        }

        return fj32_256(n);
    }

    /**
     * @brief Test whether a 64-bit unsigned integer is prime.
     *
     * This overload is deterministic over the full @c uint64_t domain.
     *
     * Values fitting in 32 bits are delegated to the @c uint32_t overload,
     * which avoids unnecessary 64-bit Miller-Rabin work.
     *
     * @param n Value to test.
     * @return @c true if @p n is prime, @c false otherwise.
     */
    [[nodiscard]] bool operator()(uint64_t n) const noexcept
    {
        if (n <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
            return (*this)(static_cast<uint32_t>(n));

        switch (prefilter<uint64_t>(n)) {
            case PrefilterResult::prime:
                return true;

            case PrefilterResult::composite:
                return false;

            case PrefilterResult::undecided:
                break;
        }

        return millerRabin64(n);
    }

private:
    /**
     * @brief Result of the shared small-prime prefilter.
     */
    enum class PrefilterResult : uint8_t {
        composite, /**< Input is definitely composite. */
        prime,     /**< Input is definitely prime. */
        undecided  /**< Further Miller-Rabin testing is required. */
    };

    /**
     * @brief Shared table of small odd primes used by the prefilter.
     *
     * The table starts at 3 because even numbers are handled separately.
     * After trial division, the test can immediately accept any candidate
     * @c n such that @c n < smallPrimes.back()^2.
     */
    inline static constexpr std::array<uint16_t, 63> smallPrimes{
        3, 5, 7, 11, 13, 17, 19,
        23, 29, 31, 37, 41, 43, 47, 53,
        59, 61, 67, 71, 73, 79, 83, 89,
        97, 101, 103, 107, 109, 113, 127, 131 ,
        137, 139, 149, 151, 157, 163, 167, 173,
        179, 181, 191, 193, 197, 199, 211, 223,
        227, 229, 233, 239, 241, 251, 257, 263,
        269, 271, 277, 281, 283, 293, 307, 311
    };

    /**
     * @brief Shared prefilter used by both overloads.
     *
     * The prefilter performs:
     *
     * - rejection of values < 2;
     * - exact handling of 2 and all even numbers;
     * - direct exact lookup for values up to the largest small prime;
     * - trial division by all small odd primes in @ref smallPrimes;
     * - a final shortcut using the fact that if no prime divisor <= sqrt(n)
     *   exists, then @p n is prime.
     *
     * @tparam UInt Unsigned integer type, expected to be @c uint32_t or @c uint64_t.
     * @param n Value to test.
     * @return A three-state result indicating whether @p n is already decided
     *         or must proceed to the second-stage primality backend.
     */
    template <std::unsigned_integral UInt>
    [[nodiscard]] static constexpr PrefilterResult prefilter(UInt n) noexcept
    {
        if (n < UInt{2})
            return PrefilterResult::composite;

        if ((n & UInt{1}) == UInt{0})
            return n == UInt{2} ? PrefilterResult::prime
                                : PrefilterResult::composite;

        if (n <= static_cast<UInt>(smallPrimes.back())) {
            const auto needle = static_cast<uint16_t>(n);
            return std::ranges::binary_search(smallPrimes, needle)
                ? PrefilterResult::prime
                : PrefilterResult::composite;
        }

        for (const uint16_t p : smallPrimes) {
            if (n % static_cast<UInt>(p) == UInt{0})
                return PrefilterResult::composite;
        }

        const UInt last = static_cast<UInt>(smallPrimes.back());
        if (last > n / last)
            return PrefilterResult::prime;

        return PrefilterResult::undecided;
    }

    /**
     * @brief Compute modular exponentiation by repeated squaring.
     *
     * @tparam UInt Unsigned integer type.
     * @tparam MulMod Callable type implementing @c mulMod(a, b) % n without overflow.
     * @param base Base.
     * @param exp Exponent.
     * @param mulMod Modular multiplication functor.
     * @return @c base^exp mod n.
     */
    template <std::unsigned_integral UInt, typename MulMod>
    [[nodiscard]] static UInt powMod(UInt base,
                                     UInt exp,
                                     const MulMod &mulMod) noexcept
    {
        UInt result = UInt{1};

        while (exp != UInt{0}) {
            if ((exp & UInt{1}) != UInt{0})
                result = mulMod(result, base);

            base = mulMod(base, base);
            exp >>= 1;
        }

        return result;
    }

    /**
     * @brief Check whether a base is an SPRP witness for compositeness.
     *
     * @tparam UInt Unsigned integer type.
     * @tparam MulMod Callable type implementing @c mulMod(a, b) % n without overflow.
     * @param n Odd candidate greater than 2.
     * @param d Odd part of @c n - 1.
     * @param s Exponent such that @c n - 1 = d * 2^s.
     * @param a SPRP base.
     * @param mulMod Modular multiplication functor.
     * @return @c true if @p a proves @p n composite, @c false otherwise.
     */
    template <std::unsigned_integral UInt, typename MulMod>
    [[nodiscard]] static bool isWitness(UInt n,
                                        UInt d,
                                        unsigned s,
                                        UInt a,
                                        const MulMod &mulMod) noexcept
    {
        a %= n;
        if (a == UInt{0})
            return false;

        UInt x = powMod<UInt>(a, d, mulMod);

        if (x == UInt{1} || x == n - UInt{1})
            return false;

        for (unsigned r = 1; r < s; ++r) {
            x = mulMod(x, x);
            if (x == n - UInt{1})
                return false;
        }

        return true;
    }

    /**
     * @brief FJ32_256 base table (Forisek-Jančina) for 32-bit primality.
     *
     * Combined with the FJ32 hash and one SPRP round, this is deterministic
     * over the full @c uint32_t domain.
     */
    inline static constexpr std::array<uint16_t, 256> fj32Bases{
        15591u, 2018u, 166u, 7429u, 8064u, 16045u, 10503u, 4399u,
        1949u, 1295u, 2776u, 3620u, 560u, 3128u, 5212u, 2657u,
        2300u, 2021u, 4652u, 1471u, 9336u, 4018u, 2398u, 20462u,
        10277u, 8028u, 2213u, 6219u, 620u, 3763u, 4852u, 5012u,
        3185u, 1333u, 6227u, 5298u, 1074u, 2391u, 5113u, 7061u,
        803u, 1269u, 3875u, 422u, 751u, 580u, 4729u, 10239u,
        746u, 2951u, 556u, 2206u, 3778u, 481u, 1522u, 3476u,
        481u, 2487u, 3266u, 5633u, 488u, 3373u, 6441u, 3344u,
        17u, 15105u, 1490u, 4154u, 2036u, 1882u, 1813u, 467u,
        3307u, 14042u, 6371u, 658u, 1005u, 903u, 737u, 1887u,
        7447u, 1888u, 2848u, 1784u, 7559u, 3400u, 951u, 13969u,
        4304u, 177u, 41u, 19875u, 3110u, 13221u, 8726u, 571u,
        7043u, 6943u, 1199u, 352u, 6435u, 165u, 1169u, 3315u,
        978u, 233u, 3003u, 2562u, 2994u, 10587u, 10030u, 2377u,
        1902u, 5354u, 4447u, 1555u, 263u, 27027u, 2283u, 305u,
        669u, 1912u, 601u, 6186u, 429u, 1930u, 14873u, 1784u,
        1661u, 524u, 3577u, 236u, 2360u, 6146u, 2850u, 55637u,
        1753u, 4178u, 8466u, 222u, 2579u, 2743u, 2031u, 2226u,
        2276u, 374u, 2132u, 813u, 23788u, 1610u, 4422u, 5159u,
        1725u, 3597u, 3366u, 14336u, 579u, 165u, 1375u, 10018u,
        12616u, 9816u, 1371u, 536u, 1867u, 10864u, 857u, 2206u,
        5788u, 434u, 8085u, 17618u, 727u, 3639u, 1595u, 4944u,
        2129u, 2029u, 8195u, 8344u, 6232u, 9183u, 8126u, 1870u,
        3296u, 7455u, 8947u, 25017u, 541u, 19115u, 368u, 566u,
        5674u, 411u, 522u, 1027u, 8215u, 2050u, 6544u, 10049u,
        614u, 774u, 2333u, 3007u, 35201u, 4706u, 1152u, 1785u,
        1028u, 1540u, 3743u, 493u, 4474u, 2521u, 26845u, 8354u,
        864u, 18915u, 5465u, 2447u, 42u, 4511u, 1660u, 166u,
        1249u, 6259u, 2553u, 304u, 272u, 7286u, 73u, 6554u,
        899u, 2816u, 5197u, 13330u, 7054u, 2818u, 3199u, 811u,
        922u, 350u, 7514u, 4452u, 3449u, 2663u, 4708u, 418u,
        1621u, 1171u, 3471u, 88u, 11345u, 412u, 1559u, 194u
    };

    /**
     * @brief FJ32 hash used to select one base out of @ref fj32Bases.
     */
    [[nodiscard]] static uint8_t hashFj32_256(uint32_t n) noexcept
    {
        uint64_t h = n;
        h = ((h >> 16u) ^ h) * 0x45d9f3bu;
        h = ((h >> 16u) ^ h) * 0x45d9f3bu;
        h = ((h >> 16u) ^ h);
        return static_cast<uint8_t>(h & 255u);
    }

    /**
     * @brief Deterministic FJ32_256 backend for @c uint32_t.
     *
     * @param n Odd candidate that survived the prefilter.
     * @return @c true if @p n is prime, @c false otherwise.
     */
    [[nodiscard]] static bool fj32_256(uint32_t n) noexcept
    {
        const auto mulMod = [n](uint32_t a, uint32_t b) noexcept -> uint32_t {
            return static_cast<uint32_t>(
                (static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) % n
            );
        };

        uint32_t d = n - 1u;
        unsigned s = 0u;

        while ((d & 1u) == 0u) {
            d >>= 1u;
            ++s;
        }

        const uint32_t bucket = hashFj32_256(n);
        const uint32_t base = static_cast<uint32_t>(fj32Bases[bucket]);
        return !isWitness<uint32_t>(n, d, s, base, mulMod);
    }

    /**
     * @brief Deterministic Miller-Rabin backend for @c uint32_t.
     *
     * Uses base set {2, 7, 61}, deterministic over the full @c uint32_t domain.
     */
    [[nodiscard]] static bool millerRabin32(uint32_t n) noexcept
    {
        const auto mulMod = [n](uint32_t a, uint32_t b) noexcept -> uint32_t {
            return static_cast<uint32_t>(
                (static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) % n
            );
        };

        uint32_t d = n - 1u;
        unsigned s = 0u;

        while ((d & 1u) == 0u) {
            d >>= 1u;
            ++s;
        }

        static constexpr std::array<uint32_t, 3> bases{2u, 7u, 61u};
        for (const uint32_t a : bases) {
            if (isWitness<uint32_t>(n, d, s, a, mulMod))
                return false;
        }

        return true;
    }

    /**
     * @brief Deterministic Miller-Rabin backend for @c uint64_t.
     *
     * Uses the standard 7-base deterministic set:
     * {2, 325, 9375, 28178, 450775, 9780504, 1795265022}.
     *
     * Modular multiplication is performed with @c unsigned __int128 to avoid overflow.
     *
     * @param n Odd candidate that survived the prefilter.
     * @return @c true if @p n is prime, @c false otherwise.
     */
    [[nodiscard]] static bool millerRabin64(uint64_t n) noexcept
    {
        const auto mulMod = [n](uint64_t a, uint64_t b) noexcept -> uint64_t {
            return static_cast<uint64_t>(
                (static_cast<unsigned __int128>(a) *
                 static_cast<unsigned __int128>(b)) % n
            );
        };

        uint64_t d = n - 1;
        unsigned s = 0;

        while ((d & 1u) == 0u) {
            d >>= 1;
            ++s;
        }

        static constexpr std::array<uint64_t, 7> bases{
            2ULL, 325ULL, 9375ULL, 28178ULL,
            450775ULL, 9780504ULL, 1795265022ULL
        };

        for (const uint64_t a : bases) {
            if (isWitness<uint64_t>(n, d, s, a, mulMod))
                return false;
        }

        return true;
    }
};

/**
 * @brief Global stateless deterministic primality-test functor.
 *
 * This is the intended public API of this header.
 *
 * @code{.cpp}
 * #include <utilities/isPrime.h>
 *
 * bool p1 = utilities::isPrime(uint32_t{97});
 * bool p2 = utilities::isPrime(uint64_t{18446744073709551557ULL});
 * @endcode
 */
inline constexpr PrimalityTestFunctor isPrime{};

} // namespace utilities

// END File: include/utilities/isPrime.h
