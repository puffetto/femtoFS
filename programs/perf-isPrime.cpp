// File: units/perf-isPrime.cpp
// Performance tests for include/utilities/isPrime.h
//
// Measures CPU time and wall-clock time for three scenarios:
//   1. One million random uint32_t values.
//   2. One million random uint64_t values (full 64-bit range).
//   3. One million random uint64_t values that fit in uint32_t (delegation path).
//
// Build with optimisations (-O2 / -O3) for meaningful numbers.
// The volatile sink prevents the compiler from dead-stripping the calls.

#include "utilities/isPrime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string_view>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Timing helpers
// ---------------------------------------------------------------------------

struct Timing {
    double wallSeconds;
    double cpuSeconds;
};

struct BenchResult {
    Timing      timing;
    std::size_t trueCount; // number of primes found (prevents dead-code elimination)
};

// RAII timer: records wall and CPU time between construction and stop().
class Timer {
public:
    Timer() { reset(); }

    void reset()
    {
        wallStart_ = std::chrono::steady_clock::now();
        cpuStart_  = std::clock();
    }

    Timing stop() const
    {
        const auto wallEnd = std::chrono::steady_clock::now();
        const auto cpuEnd  = std::clock();

        const double wall =
            std::chrono::duration<double>(wallEnd - wallStart_).count();
        const double cpu =
            static_cast<double>(cpuEnd - cpuStart_) / CLOCKS_PER_SEC;

        return {wall, cpu};
    }

private:
    std::chrono::steady_clock::time_point wallStart_;
    std::clock_t                          cpuStart_;
};

// ---------------------------------------------------------------------------
// Benchmark runners
// ---------------------------------------------------------------------------

template <typename UInt>
BenchResult runBench(const std::vector<UInt> &inputs)
{
    volatile std::size_t sink = 0; // volatile prevents the loop being optimised away

    Timer t;
    for (const UInt v : inputs)
        sink += utilities::isPrime(v) ? 1u : 0u;

    return {t.stop(), sink};
}

// ---------------------------------------------------------------------------
// Input generation
// ---------------------------------------------------------------------------

std::vector<uint32_t> makeUint32Inputs(std::size_t n, std::mt19937_64 &rng)
{
    std::uniform_int_distribution<uint32_t> dist(
        0, std::numeric_limits<uint32_t>::max()
    );
    std::vector<uint32_t> v(n);
    std::ranges::generate(v, [&]{ return dist(rng); });
    return v;
}

// Full 64-bit range — exercises the 64-bit Miller-Rabin path.
std::vector<uint64_t> makeUint64FullInputs(std::size_t n, std::mt19937_64 &rng)
{
    std::uniform_int_distribution<uint64_t> dist(
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1ULL,
        std::numeric_limits<uint64_t>::max()
    );
    std::vector<uint64_t> v(n);
    std::ranges::generate(v, [&]{ return dist(rng); });
    return v;
}

    // Values in [0, UINT32_MAX] stored as uint64_t — exercises the delegation path.
    std::vector<uint64_t> makeUint64DelegatedInputs(std::size_t n, std::mt19937_64 &rng)
{
    std::uniform_int_distribution<uint32_t> dist(
        0, std::numeric_limits<uint32_t>::max()
    );
    std::vector<uint64_t> v(n);
    std::ranges::generate(v, [&]{ return static_cast<uint64_t>(dist(rng)); });
    return v;
}

    // Values in [0, UINT32_MAX] stored as uint64_t — exercises the delegation path.
    std::vector<uint64_t> makeUint64ShortInputs(std::size_t n, std::mt19937_64 &rng)
{
    std::uniform_int_distribution<uint32_t> dist(
        0, std::numeric_limits<uint16_t>::max()
    );
    std::vector<uint64_t> v(n);
    std::ranges::generate(v, [&]{ return static_cast<uint64_t>(dist(rng)); });
    return v;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void printHeader()
{
    std::cout << std::left
              << std::setw(42) << "Scenario"
              << std::right
              << std::setw(14) << "Wall (s)"
              << std::setw(14) << "CPU (s)"
              << std::setw(14) << "ns/call"
              << std::setw(14) << "Primes found"
              << "\n"
              << std::string(98, '-') << "\n";
}

void printRow(std::string_view label, std::size_t n, const BenchResult &r)
{
    const double nsPerCall = r.timing.wallSeconds * 1e9 / static_cast<double>(n);

    std::cout << std::left  << std::setw(42) << label
              << std::right << std::fixed    << std::setprecision(6)
              << std::setw(14) << r.timing.wallSeconds
              << std::setw(14) << r.timing.cpuSeconds
              << std::setprecision(2)
              << std::setw(14) << nsPerCall
              << std::setw(14) << r.trueCount
              << "\n";
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    constexpr std::size_t N    = 10'000'000;
    constexpr uint64_t    seed = 0xDEADBEEFCAFEBABEULL;

    std::mt19937_64 rng{seed};

    std::cout << "\nutilities::isPrime — performance test  (N = "
              << N << ")\n\n";

    // Generate all inputs before timing anything.
    const auto u32Inputs       = makeUint32Inputs(N, rng);
    const auto u64FullInputs   = makeUint64FullInputs(N, rng);
    const auto u64DelInputs    = makeUint64DelegatedInputs(N, rng);
    const auto u64ShortInputs    = makeUint64ShortInputs(N, rng);

    printHeader();

    printRow("uint32_t  (random full range)",
             N, runBench(u32Inputs));

    printRow("uint64_t  (random full 64-bit range)",
             N, runBench(u64FullInputs));

    printRow("uint64_t  (values fit in uint32)",
             N, runBench(u64DelInputs));

    printRow("uint64_t  (values fit in uint16)",
         N, runBench(u64ShortInputs));

    std::cout << "\n";
    return 0;
}

// END File: units/perf-isPrime.cpp
