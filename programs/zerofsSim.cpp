#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <isPrime.h>

namespace {

constexpr uint32_t kMaxTablesizePrime = 65521u;
constexpr uint32_t kDirEntryHardLimit = 1u << 15; // N < 2^15

struct FsEntry {
    uint64_t inode = 0;
    char type = '?'; // d, -, l
    std::string mode;
    std::string owner;
    std::string group;
    uint64_t size = 0;
    std::string path;
    std::string parent;
    std::string name;
    std::string symlinkTarget;
};

struct HashChoice {
    uint32_t tablesize = 0;
    uint32_t p = 0;
    double score = 0.0;
    uint32_t maxChain = 0;
    uint64_t sumSquares = 0;
    uint32_t empties = 0;
    bool hitCeiling = false;
    uint32_t ceiling = 0;
};

struct PackingStats {
    uint64_t rawBytes = 0;
    uint64_t paddedBytes = 0;
    uint64_t paddingBytes = 0;
    size_t contentCount = 0;
};

struct ClassifiedContent {
    uint32_t size = 0;
    bool isPublic = false;
};

struct VisibilitySplitPackingStats {
    PackingStats combined;
    PackingStats publicPool;
    PackingStats privatePool;
};

struct DirReport {
    std::string path;
    uint32_t n = 0;
    HashChoice choice;
};

struct HashSimulationSummary {
    std::vector<DirReport> dirs;
    uint64_t totalDirEntries = 0;
    uint64_t totalBuckets = 0;
    uint64_t totalEmptyBuckets = 0;
    uint64_t totalSumSquares = 0;
    uint32_t globalMaxChain = 0;
    uint64_t dirsPerfect = 0;
    uint64_t dirsHitCeiling = 0;
    uint64_t dirsFallback = 0;
    uint64_t dirsOverHardLimit = 0;
};

struct ProgramOptions {
    std::string inputPath = "misc/list";
    uint32_t pageSize = 4096u;
    bool fixedBaseExperiment = false;
    size_t fixedBaseSamples = 100;
    uint32_t fixedBaseSeed = 0x0F5F2026u;
};

std::string normalizePath(const std::string& raw) {
    if (raw == "./") {
        return "";
    }
    if (raw.rfind("./", 0) == 0) {
        return raw.substr(2);
    }
    return raw;
}

std::pair<std::string, std::string> splitParentName(const std::string& path) {
    if (path.empty()) {
        return {"", ""};
    }
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) {
        return {"", path};
    }
    return {path.substr(0, slash), path.substr(slash + 1)};
}

bool parseFindLsLine(const std::string& line, FsEntry& out) {
    static const std::regex re(
        R"(^\s*([0-9]+)\s+([0-9]+)\s+([bcdlps-][rwxstST-]{9})\s+([0-9]+)\s+(\S+)\s+(\S+)\s+([0-9]+)\s+[A-Za-z]{3}\s+[0-9]{1,2}\s+([0-9]{2}:[0-9]{2}|[0-9]{4})\s+(.+)$)");

    std::smatch m;
    if (!std::regex_match(line, m, re)) {
        return false;
    }

    FsEntry e;
    e.inode = std::stoull(m[1].str());
    const std::string mode = m[3].str();
    e.type = mode.empty() ? '?' : mode[0];
    e.mode = mode;
    e.owner = m[5].str();
    e.group = m[6].str();
    e.size = std::stoull(m[7].str());

    std::string pathField = m[9].str();
    if (e.type == 'l') {
        const std::string arrow = " -> ";
        const auto pos = pathField.find(arrow);
        if (pos != std::string::npos) {
            e.symlinkTarget = pathField.substr(pos + arrow.size());
            pathField = pathField.substr(0, pos);
        }
    }

    e.path = normalizePath(pathField);
    const auto [parent, name] = splitParentName(e.path);
    e.parent = parent;
    e.name = name;

    out = std::move(e);
    return true;
}

std::vector<uint32_t> smallPrimesUpTo251() {
    std::vector<uint32_t> out;
    for (uint32_t p = 2; p <= 251; ++p) {
        if (utilities::isPrime(p)) {
            out.push_back(p);
        }
    }
    return out;
}

uint32_t zerofsHash(const std::string& name, uint32_t p, uint32_t tablesize) {
    if (tablesize <= 1) {
        return 0;
    }
    uint32_t h = 0;
    for (unsigned char c : name) {
        h = h * static_cast<uint32_t>(p) + static_cast<uint32_t>(c);
    }
    return h % tablesize;
}

struct Score {
    uint64_t sumSquares = 0;
    uint32_t maxChain = 0;
};

Score scoreDirectory(const std::vector<std::string>& names, uint32_t p, uint32_t tablesize) {
    std::vector<uint32_t> buckets(tablesize, 0);
    for (const auto& name : names) {
        ++buckets[zerofsHash(name, p, tablesize)];
    }
    Score s;
    for (uint32_t c : buckets) {
        if (c == 0) {
            continue;
        }
        s.sumSquares += static_cast<uint64_t>(c) * static_cast<uint64_t>(c);
        s.maxChain = std::max(s.maxChain, c);
    }
    return s;
}

HashChoice chooseHash(const std::vector<std::string>& names, const std::vector<uint32_t>& bases) {
    HashChoice out;
    const uint32_t n = static_cast<uint32_t>(names.size());

    if (n == 0) {
        out.tablesize = 0;
        out.p = 0;
        out.score = 0.0;
        out.maxChain = 0;
        out.sumSquares = 0;
        out.empties = 0;
        out.ceiling = 0;
        return out;
    }
    if (n == 1) {
        out.tablesize = 1;
        out.p = 0;
        out.score = 1.0;
        out.maxChain = 1;
        out.sumSquares = 1;
        out.empties = 0;
        out.ceiling = 1;
        return out;
    }

    uint32_t tablesize = n;
    const uint32_t target = (3u * n + 1u) / 2u; // ceil(1.5 * N)
    const uint32_t ceiling = std::min(target, kMaxTablesizePrime);

    double bestScore = std::numeric_limits<double>::infinity();
    uint32_t bestP = 0;
    uint32_t bestSize = n;
    uint32_t bestMaxChain = 0;
    uint64_t bestSumSquares = 0;

    for (;;) {
        for (uint32_t p : bases) {
            const Score s = scoreDirectory(names, p, tablesize);
            const double score = static_cast<double>(s.sumSquares) / static_cast<double>(n);

            if (s.sumSquares == n) {
                out.tablesize = tablesize;
                out.p = p;
                out.score = 1.0;
                out.maxChain = s.maxChain;
                out.sumSquares = s.sumSquares;
                out.empties = tablesize - n;
                out.ceiling = ceiling;
                return out;
            }
            if (score < bestScore) {
                bestScore = score;
                bestP = p;
                bestSize = tablesize;
                bestMaxChain = s.maxChain;
                bestSumSquares = s.sumSquares;
            }
        }

        if (bestScore < 1.1) {
            out.tablesize = bestSize;
            out.p = bestP;
            out.score = bestScore;
            out.maxChain = bestMaxChain;
            out.sumSquares = bestSumSquares;
            out.empties = bestSize - n;
            out.ceiling = ceiling;
            return out;
        }

        if (tablesize >= ceiling) {
            out.tablesize = bestSize;
            out.p = bestP;
            out.score = bestScore;
            out.maxChain = bestMaxChain;
            out.sumSquares = bestSumSquares;
            out.empties = bestSize - n;
            out.ceiling = ceiling;
            out.hitCeiling = true;
            return out;
        }
        ++tablesize;
    }
}

HashChoice chooseHashWithFixedFallback(const std::vector<std::string>& names,
                                       const std::vector<uint32_t>& bases,
                                       uint32_t fallbackBase) {
    HashChoice primary = chooseHash(names, bases);
    if (!primary.hitCeiling) {
        return primary;
    }

    HashChoice fallback = chooseHash(names, {fallbackBase});
    fallback.hitCeiling = true;
    return fallback;
}

ProgramOptions parseArgs(int argc, char** argv) {
    ProgramOptions opts;
    bool inputSet = false;
    bool pageSizeSet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--fixed-base-experiment") {
            opts.fixedBaseExperiment = true;
            continue;
        }
        if (arg.rfind("--fixed-base-samples=", 0) == 0) {
            opts.fixedBaseExperiment = true;
            opts.fixedBaseSamples = static_cast<size_t>(std::stoul(arg.substr(21)));
            continue;
        }
        if (arg.rfind("--fixed-base-seed=", 0) == 0) {
            opts.fixedBaseExperiment = true;
            opts.fixedBaseSeed = static_cast<uint32_t>(std::stoul(arg.substr(18)));
            continue;
        }
        if (!inputSet) {
            opts.inputPath = arg;
            inputSet = true;
            continue;
        }
        if (!pageSizeSet) {
            opts.pageSize = static_cast<uint32_t>(std::stoul(arg));
            pageSizeSet = true;
            continue;
        }
        throw std::runtime_error("usage: zerofsSim [inputPath] [pageSize] [--fixed-base-experiment] "
                                 "[--fixed-base-samples=N] [--fixed-base-seed=N]");
    }

    return opts;
}

std::vector<uint32_t> sampleRandomPrimes(uint32_t minInclusive,
                                         uint32_t maxExclusive,
                                         size_t count,
                                         uint32_t seed) {
    std::vector<uint32_t> primes;
    if (minInclusive >= maxExclusive || count == 0) {
        return primes;
    }

    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint32_t> dist(minInclusive, maxExclusive - 1);
    std::unordered_set<uint32_t> seen;
    seen.reserve(count * 2);

    while (primes.size() < count) {
        uint32_t candidate = dist(rng);
        candidate |= 1u;
        if (candidate >= maxExclusive) {
            candidate -= 2u;
        }
        if (candidate < minInclusive) {
            candidate = minInclusive | 1u;
        }
        if (!utilities::isPrime(candidate)) {
            continue;
        }
        if (seen.insert(candidate).second) {
            primes.push_back(candidate);
        }
    }

    return primes;
}

HashSimulationSummary simulateHashing(const std::set<std::string>& directories,
                                      const std::unordered_map<std::string, std::vector<std::string>>& dirChildren,
                                      const std::vector<uint32_t>& bases) {
    HashSimulationSummary summary;
    summary.dirs.reserve(directories.size());

    for (const auto& d : directories) {
        const auto& names = dirChildren.at(d);
        const uint32_t n = static_cast<uint32_t>(names.size());
        if (n >= kDirEntryHardLimit) {
            ++summary.dirsOverHardLimit;
        }

        HashChoice hc = chooseHash(names, bases);
        if (hc.score == 1.0 && n > 0) {
            ++summary.dirsPerfect;
        }
        if (hc.hitCeiling) {
            ++summary.dirsHitCeiling;
            if (hc.score >= 1.1) {
                ++summary.dirsFallback;
            }
        }

        summary.totalDirEntries += n;
        summary.totalBuckets += hc.tablesize;
        summary.totalEmptyBuckets += hc.empties;
        summary.totalSumSquares += hc.sumSquares;
        summary.globalMaxChain = std::max(summary.globalMaxChain, hc.maxChain);

        summary.dirs.push_back(DirReport{d.empty() ? "/" : d, n, hc});
    }

    std::sort(summary.dirs.begin(), summary.dirs.end(), [](const DirReport& a, const DirReport& b) {
        if (a.choice.score != b.choice.score) {
            return a.choice.score > b.choice.score;
        }
        if (a.choice.maxChain != b.choice.maxChain) {
            return a.choice.maxChain > b.choice.maxChain;
        }
        return a.n > b.n;
    });

    return summary;
}

HashSimulationSummary simulateHashingWithFixedFallback(
    const std::set<std::string>& directories,
    const std::unordered_map<std::string, std::vector<std::string>>& dirChildren,
    const std::vector<uint32_t>& bases,
    uint32_t fallbackBase) {
    HashSimulationSummary summary;
    summary.dirs.reserve(directories.size());

    for (const auto& d : directories) {
        const auto& names = dirChildren.at(d);
        const uint32_t n = static_cast<uint32_t>(names.size());
        if (n >= kDirEntryHardLimit) {
            ++summary.dirsOverHardLimit;
        }

        HashChoice hc = chooseHashWithFixedFallback(names, bases, fallbackBase);
        if (hc.score == 1.0 && n > 0) {
            ++summary.dirsPerfect;
        }
        if (hc.hitCeiling) {
            ++summary.dirsHitCeiling;
            if (hc.score >= 1.1) {
                ++summary.dirsFallback;
            }
        }

        summary.totalDirEntries += n;
        summary.totalBuckets += hc.tablesize;
        summary.totalEmptyBuckets += hc.empties;
        summary.totalSumSquares += hc.sumSquares;
        summary.globalMaxChain = std::max(summary.globalMaxChain, hc.maxChain);

        summary.dirs.push_back(DirReport{d.empty() ? "/" : d, n, hc});
    }

    std::sort(summary.dirs.begin(), summary.dirs.end(), [](const DirReport& a, const DirReport& b) {
        if (a.choice.score != b.choice.score) {
            return a.choice.score > b.choice.score;
        }
        if (a.choice.maxChain != b.choice.maxChain) {
            return a.choice.maxChain > b.choice.maxChain;
        }
        return a.n > b.n;
    });

    return summary;
}

double weightedMeanSquare(const HashSimulationSummary& summary) {
    return (summary.totalDirEntries == 0)
        ? 0.0
        : static_cast<double>(summary.totalSumSquares) / static_cast<double>(summary.totalDirEntries);
}

double globalLoadFactor(const HashSimulationSummary& summary) {
    return (summary.totalBuckets == 0)
        ? 0.0
        : static_cast<double>(summary.totalDirEntries) / static_cast<double>(summary.totalBuckets);
}

bool otherRead(const FsEntry& e) {
    return e.mode.size() >= 10 && e.mode[7] == 'r';
}

bool otherExec(const FsEntry& e) {
    return e.mode.size() >= 10 && (e.mode[9] == 'x' || e.mode[9] == 's' || e.mode[9] == 't');
}

void addHole(std::multimap<uint32_t, uint64_t>& holes, uint64_t off, uint32_t size) {
    if (size == 0) {
        return;
    }
    holes.emplace(size, off);
}

PackingStats packContents(std::vector<uint32_t> sizes, uint32_t pageSize) {
    PackingStats ps;
    if (pageSize == 0) {
        return ps;
    }

    sizes.erase(std::remove(sizes.begin(), sizes.end(), 0u), sizes.end());
    std::sort(sizes.begin(), sizes.end(), std::greater<uint32_t>());

    std::multimap<uint32_t, uint64_t> holes; // key=size, value=offset
    uint64_t tail = 0;

    for (uint32_t s : sizes) {
        ps.rawBytes += s;
        ++ps.contentCount;

        auto it = holes.lower_bound(s);
        if (it != holes.end()) {
            const uint32_t holeSize = it->first;
            const uint64_t holeOff = it->second;
            holes.erase(it);

            const uint32_t rem = holeSize - s;
            if (rem > 0) {
                addHole(holes, holeOff + s, rem);
            }
            continue;
        }

        const uint32_t pageOff = static_cast<uint32_t>(tail % pageSize);
        if (pageOff + s > pageSize) {
            const uint32_t gap = pageSize - pageOff;
            addHole(holes, tail, gap);
            tail += gap;
        }
        tail += s;
    }

    ps.paddedBytes = tail;
    ps.paddingBytes = ps.paddedBytes - ps.rawBytes;
    return ps;
}

PackingStats packClassifiedContents(const std::vector<ClassifiedContent>& contents,
                                    uint32_t pageSize,
                                    bool targetPublicPool) {
    std::vector<uint32_t> sizes;
    sizes.reserve(contents.size());
    for (const auto& content : contents) {
        if (content.isPublic == targetPublicPool && content.size != 0) {
            sizes.push_back(content.size);
        }
    }
    return packContents(std::move(sizes), pageSize);
}

VisibilitySplitPackingStats packContentsWithVisibilitySplit(const std::vector<ClassifiedContent>& contents,
                                                            uint32_t pageSize) {
    VisibilitySplitPackingStats stats;
    stats.publicPool = packClassifiedContents(contents, pageSize, true);
    stats.privatePool = packClassifiedContents(contents, pageSize, false);
    stats.combined.rawBytes = stats.publicPool.rawBytes + stats.privatePool.rawBytes;
    stats.combined.paddedBytes = stats.publicPool.paddedBytes + stats.privatePool.paddedBytes;
    stats.combined.paddingBytes = stats.publicPool.paddingBytes + stats.privatePool.paddingBytes;
    stats.combined.contentCount = stats.publicPool.contentCount + stats.privatePool.contentCount;
    return stats;
}

std::string prettyBytes(uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < (sizeof(units) / sizeof(units[0]))) {
        v /= 1024.0;
        ++u;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision((u == 0) ? 0 : 2) << v << ' ' << units[u];
    return oss.str();
}

uint32_t alignUp(uint32_t x, uint32_t a) {
    if (a == 0) {
        return x;
    }
    const uint32_t r = x % a;
    return r == 0 ? x : (x + (a - r));
}

} // namespace

int main(int argc, char** argv) {
    const ProgramOptions options = parseArgs(argc, argv);
    const std::string& inputPath = options.inputPath;
    const uint32_t pageSize = options.pageSize;
    const auto smallPrimes = smallPrimesUpTo251();

    std::ifstream in(inputPath);
    if (!in) {
        std::cerr << "Cannot open input file: " << inputPath << "\n";
        return 1;
    }

    std::vector<FsEntry> entries;
    entries.reserve(8192);

    size_t parseErrors = 0;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) {
            continue;
        }
        FsEntry e;
        if (!parseFindLsLine(line, e)) {
            ++parseErrors;
            continue;
        }
        entries.push_back(std::move(e));
    }

    std::unordered_map<std::string, const FsEntry*> entryByPath;
    entryByPath.reserve(entries.size());
    for (const auto& e : entries) {
        entryByPath[e.path] = &e;
    }

    std::unordered_map<std::string, bool> publicDirectoryByPath;
    publicDirectoryByPath.reserve(entries.size());

    std::function<bool(const std::string&)> isPublicDirectory = [&](const std::string& path) -> bool {
        const auto memoIt = publicDirectoryByPath.find(path);
        if (memoIt != publicDirectoryByPath.end()) {
            return memoIt->second;
        }

        const auto entryIt = entryByPath.find(path);
        if (entryIt == entryByPath.end() || entryIt->second->type != 'd') {
            publicDirectoryByPath[path] = false;
            return false;
        }

        const FsEntry& e = *entryIt->second;
        bool visible = false;
        if (path.empty()) {
            visible = otherRead(e) && otherExec(e);
        } else {
            visible = isPublicDirectory(e.parent) && otherRead(e) && otherExec(e);
        }

        publicDirectoryByPath[path] = visible;
        return visible;
    };

    auto isPublicEntryPath = [&](const FsEntry& e) -> bool {
        if (e.path.empty()) {
            return e.type == 'd' && otherRead(e) && otherExec(e);
        }
        if (!isPublicDirectory(e.parent) || !otherRead(e)) {
            return false;
        }
        return e.type != 'd' || otherExec(e);
    };

    std::set<std::string> directories;
    directories.insert(""); // root

    std::unordered_map<std::string, std::vector<std::string>> dirChildren;
    dirChildren.reserve(1024);

    std::unordered_map<uint64_t, uint64_t> filePayloadByInode;
    std::unordered_map<uint64_t, uint32_t> filePathCountByInode;
    std::unordered_map<uint64_t, bool> filePayloadPublicByInode;
    std::unordered_map<uint64_t, std::string> fileAttrKeyByInode;
    filePayloadByInode.reserve(4096);
    filePathCountByInode.reserve(4096);
    filePayloadPublicByInode.reserve(4096);
    fileAttrKeyByInode.reserve(4096);

    std::set<std::string> uniqueNames;
    std::set<std::string> uniqueSymlinkTargets;
    std::unordered_map<std::string, bool> namePublicByValue;
    std::unordered_map<std::string, bool> stringContentPublicByValue;
    namePublicByValue.reserve(4096);
    stringContentPublicByValue.reserve(4096);

    uint64_t countDir = 0;
    uint64_t countFilePath = 0;
    uint64_t countSymlink = 0;
    uint64_t unsupported = 0;

    uint64_t duplicateInodeSizeMismatch = 0;
    uint64_t duplicateInodeAttrMismatch = 0;
    std::set<std::string> uniqueAttrKeys;

    auto makeAttrKey = [](const FsEntry& e) {
        return e.mode + '\x1f' + e.owner + '\x1f' + e.group;
    };

    for (const auto& e : entries) {
        if (e.type == 'd') {
            ++countDir;
            directories.insert(e.path);
            if (!e.path.empty()) {
                dirChildren[e.parent].push_back(e.name);
                uniqueNames.insert(e.name);
                namePublicByValue[e.name] = namePublicByValue[e.name] || isPublicEntryPath(e);
                stringContentPublicByValue[e.name] = stringContentPublicByValue[e.name] || isPublicEntryPath(e);
                uniqueAttrKeys.insert(makeAttrKey(e));
            }
            continue;
        }
        if (e.type == '-') {
            ++countFilePath;
            dirChildren[e.parent].push_back(e.name);
            uniqueNames.insert(e.name);
            const bool publicPath = isPublicEntryPath(e);
            namePublicByValue[e.name] = namePublicByValue[e.name] || publicPath;
            stringContentPublicByValue[e.name] = stringContentPublicByValue[e.name] || publicPath;
            auto [it, inserted] = filePayloadByInode.emplace(e.inode, e.size);
            if (!inserted && it->second != e.size) {
                ++duplicateInodeSizeMismatch;
            }
            const std::string attrKey = makeAttrKey(e);
            auto [attrIt, attrInserted] = fileAttrKeyByInode.emplace(e.inode, attrKey);
            if (!attrInserted && attrIt->second != attrKey) {
                ++duplicateInodeAttrMismatch;
            }
            ++filePathCountByInode[e.inode];
            filePayloadPublicByInode[e.inode] = filePayloadPublicByInode[e.inode] || publicPath;
            continue;
        }
        if (e.type == 'l') {
            ++countSymlink;
            dirChildren[e.parent].push_back(e.name);
            uniqueNames.insert(e.name);
            namePublicByValue[e.name] = namePublicByValue[e.name] || isPublicEntryPath(e);
            stringContentPublicByValue[e.name] = stringContentPublicByValue[e.name] || isPublicEntryPath(e);
            uniqueSymlinkTargets.insert(e.symlinkTarget);
            (void)stringContentPublicByValue[e.symlinkTarget];
            uniqueAttrKeys.insert(makeAttrKey(e));
            continue;
        }
        ++unsupported;
    }

    for (const auto& [inode, attrKey] : fileAttrKeyByInode) {
        (void)inode;
        uniqueAttrKeys.insert(attrKey);
    }

    // Ensure every parsed directory has a child vector, including empty dirs.
    for (const auto& d : directories) {
        (void)dirChildren[d];
    }

    const HashSimulationSummary baseline = simulateHashing(directories, dirChildren, smallPrimes);

    uint64_t fileObjectCount = filePayloadByInode.size();
    uint64_t hardlinkObjectCount = 0;
    for (const auto& [inode, count] : filePathCountByInode) {
        (void)inode;
        if (count > 1) {
            hardlinkObjectCount += (count - 1);
        }
    }
    const uint64_t dirObjectCount = (countDir > 0) ? (countDir - 1) : 0; // root in header
    const uint64_t symlinkObjectCount = countSymlink;
    const uint64_t metadataObjects = fileObjectCount + hardlinkObjectCount + dirObjectCount + symlinkObjectCount;
    const uint64_t attrObjectCount = uniqueAttrKeys.size();
    const uint64_t attrCellsReused = std::min<uint64_t>(attrObjectCount, baseline.totalEmptyBuckets);
    const uint64_t attrCellsExtended = attrObjectCount - attrCellsReused;

    // Content sizes for packing simulation.
    std::vector<uint32_t> contentSizes;
    std::vector<ClassifiedContent> classifiedContents;
    contentSizes.reserve(filePayloadByInode.size() + uniqueNames.size() + uniqueSymlinkTargets.size());
    classifiedContents.reserve(filePayloadByInode.size() + uniqueNames.size() + uniqueSymlinkTargets.size());

    uint64_t zeroLenFilePayloads = 0;
    uint64_t smallFilePayloads = 0;
    uint64_t largePageAlignedPayloads = 0;
    uint64_t largePartialPayloads = 0;
    uint64_t publicFilePayloads = 0;
    uint64_t privateFilePayloads = 0;
    uint64_t publicFilenameStrings = 0;
    uint64_t privateFilenameStrings = 0;
    uint64_t publicStringContents = 0;
    uint64_t privateStringContents = 0;

    for (const auto& [inode, size] : filePayloadByInode) {
        const bool isPublic = filePayloadPublicByInode[inode];
        if (size == 0) {
            ++zeroLenFilePayloads;
            continue;
        }
        if (size <= std::numeric_limits<uint32_t>::max()) {
            contentSizes.push_back(static_cast<uint32_t>(size));
            classifiedContents.push_back(ClassifiedContent{static_cast<uint32_t>(size), isPublic});
            if (isPublic) {
                ++publicFilePayloads;
            } else {
                ++privateFilePayloads;
            }
        } else {
            std::cerr << "Skipping payload > 4GiB (unsupported by format): " << size << "\n";
        }
        if (size < pageSize) {
            ++smallFilePayloads;
        } else if ((size % pageSize) == 0) {
            ++largePageAlignedPayloads;
        } else {
            ++largePartialPayloads;
        }
    }

    std::set<std::string> uniqueStringContents;
    for (const auto& name : uniqueNames) {
        uniqueStringContents.insert(name);
        if (namePublicByValue[name]) {
            ++publicFilenameStrings;
        } else {
            ++privateFilenameStrings;
        }
    }
    for (const auto& target : uniqueSymlinkTargets) {
        uniqueStringContents.insert(target);
    }
    for (const auto& s : uniqueStringContents) {
        const uint64_t bytes = static_cast<uint64_t>(s.size()) + 1u;
        if (bytes <= std::numeric_limits<uint32_t>::max()) {
            contentSizes.push_back(static_cast<uint32_t>(bytes));
            const bool isPublic = stringContentPublicByValue[s];
            classifiedContents.push_back(ClassifiedContent{static_cast<uint32_t>(bytes), isPublic});
            if (isPublic) {
                ++publicStringContents;
            } else {
                ++privateStringContents;
            }
        }
    }

    const PackingStats packing = packContents(contentSizes, pageSize);
    const VisibilitySplitPackingStats splitPacking = packContentsWithVisibilitySplit(classifiedContents, pageSize);

    const uint64_t objectTableEntries = metadataObjects + baseline.totalBuckets + attrCellsExtended;
    const uint64_t objectTableBytes = objectTableEntries * 16u;
    const uint32_t headerBytes = 128u;
    const uint32_t contentOff = alignUp(headerBytes + static_cast<uint32_t>(objectTableBytes), pageSize);
    const uint64_t imageBytesEstimate = static_cast<uint64_t>(contentOff) + packing.paddedBytes;
    const uint64_t splitImageBytesEstimate = static_cast<uint64_t>(contentOff) + splitPacking.combined.paddedBytes;

    const size_t topN = std::min<size_t>(12, baseline.dirs.size());

    std::cout << "Input file: " << inputPath << "\n";
    std::cout << "Parsed lines: " << entries.size() << " (parse errors: " << parseErrors << ")\n\n";

    std::cout << "Filesystem inventory\n";
    std::cout << "  directories (including root): " << countDir << "\n";
    std::cout << "  regular file paths:           " << countFilePath << "\n";
    std::cout << "  symlinks:                     " << countSymlink << "\n";
    std::cout << "  unsupported entry types:      " << unsupported << "\n";
    if (duplicateInodeSizeMismatch != 0) {
        std::cout << "  WARNING: inode size mismatches: " << duplicateInodeSizeMismatch << "\n";
    }
    if (duplicateInodeAttrMismatch != 0) {
        std::cout << "  WARNING: inode attr mismatches: " << duplicateInodeAttrMismatch << "\n";
    }
    std::cout << "\n";

    std::cout << "0fs object model estimate\n";
    std::cout << "  file objects (unique inodes): " << fileObjectCount << "\n";
    std::cout << "  hardlink objects:             " << hardlinkObjectCount << "\n";
    std::cout << "  dir objects (non-root):       " << dirObjectCount << "\n";
    std::cout << "  symlink objects:              " << symlinkObjectCount << "\n";
    std::cout << "  metadata objects total:       " << metadataObjects << "\n";
    std::cout << "  deduped attr objects:         " << attrObjectCount << "\n";
    std::cout << "  attr cells reusing empties:   " << attrCellsReused << "\n";
    std::cout << "  attr cells extending table:   " << attrCellsExtended << "\n";
    std::cout << "  hash buckets total:           " << baseline.totalBuckets << "\n";
    std::cout << "  metadata table cells total:   " << objectTableEntries
              << " (limit < 65536)\n";
    std::cout << "  metadata table bytes:         " << prettyBytes(objectTableBytes) << "\n";
    std::cout << "\n";

    const double baselineWeightedMeanSquare = weightedMeanSquare(baseline);
    const double avgEmptiesPerDir =
        directories.empty() ? 0.0 : static_cast<double>(baseline.totalEmptyBuckets) / static_cast<double>(directories.size());
    const double baselineGlobalLoadFactor = globalLoadFactor(baseline);

    std::cout << "Directory hashing simulation\n";
    std::cout << "  ceiling policy:              all sizes in [N, ceil(1.5*N)], cap " << kMaxTablesizePrime << "\n";
    std::cout << "  directories simulated:        " << directories.size() << "\n";
    std::cout << "  total dir entries (N sum):    " << baseline.totalDirEntries << "\n";
    std::cout << "  total buckets:                " << baseline.totalBuckets << "\n";
    std::cout << "  total empty buckets:          " << baseline.totalEmptyBuckets << "\n";
    std::cout << "  avg empty buckets / dir:      " << std::fixed << std::setprecision(2) << avgEmptiesPerDir << "\n";
    std::cout << "  global load factor N/T:       " << std::fixed << std::setprecision(4) << baselineGlobalLoadFactor << "\n";
    std::cout << "  weighted mean-square chain:   " << std::fixed << std::setprecision(4) << baselineWeightedMeanSquare << "\n";
    std::cout << "  perfect-hash dirs (score=1):  " << baseline.dirsPerfect << "\n";
    std::cout << "  dirs hitting ceiling:         " << baseline.dirsHitCeiling << "\n";
    std::cout << "  dirs fallback(score>=1.1):    " << baseline.dirsFallback << "\n";
    std::cout << "  max chain observed:           " << baseline.globalMaxChain << "\n";
    std::cout << "  dirs violating N<2^15:        " << baseline.dirsOverHardLimit << "\n";
    std::cout << "\n";

    std::cout << "Worst directories by score\n";
    std::cout << "  path | N | tablesize | p | score | max_chain | empties | hit_ceiling\n";
    for (size_t i = 0; i < topN; ++i) {
        const auto& d = baseline.dirs[i];
        std::cout << "  " << d.path
                  << " | " << d.n
                  << " | " << d.choice.tablesize
                  << " | " << d.choice.p
                  << " | " << std::fixed << std::setprecision(4) << d.choice.score
                  << " | " << d.choice.maxChain
                  << " | " << d.choice.empties
                  << " | " << (d.choice.hitCeiling ? "yes" : "no")
                  << "\n";
    }
    std::cout << "\n";

    std::cout << "Fallback directories (hit ceiling with score >= 1.1)\n";
    std::cout << "  path | N | score\n";
    for (const auto& d : baseline.dirs) {
        if (!(d.choice.hitCeiling && d.choice.score >= 1.1)) {
            continue;
        }
        std::cout << "  " << d.path
                  << " | " << d.n
                  << " | " << std::fixed << std::setprecision(4) << d.choice.score
                  << "\n";
    }
    std::cout << "\n";

    if (options.fixedBaseExperiment) {
        constexpr uint32_t kLargePrimeMin = 1u << 16;
        constexpr uint32_t kLargePrimeMax = 1u << 23;
        constexpr uint32_t kHybridFallbackBase = 1197923u;

        struct FixedBaseTrial {
            uint32_t prime = 0;
            HashSimulationSummary summary;
        };

        const auto sampledPrimes = sampleRandomPrimes(
            kLargePrimeMin,
            kLargePrimeMax,
            options.fixedBaseSamples,
            options.fixedBaseSeed);

        std::vector<FixedBaseTrial> trials;
        trials.reserve(sampledPrimes.size());

        double sumWeightedMeanSquare = 0.0;
        double sumLoadFactor = 0.0;
        uint64_t sumPerfectDirs = 0;
        uint64_t sumFallbackDirs = 0;
        uint64_t sumBuckets = 0;

        for (uint32_t p : sampledPrimes) {
            HashSimulationSummary summary = simulateHashing(directories, dirChildren, {p});
            sumWeightedMeanSquare += weightedMeanSquare(summary);
            sumLoadFactor += globalLoadFactor(summary);
            sumPerfectDirs += summary.dirsPerfect;
            sumFallbackDirs += summary.dirsFallback;
            sumBuckets += summary.totalBuckets;
            trials.push_back(FixedBaseTrial{p, std::move(summary)});
        }

        std::sort(trials.begin(), trials.end(), [](const FixedBaseTrial& a, const FixedBaseTrial& b) {
            const double aw = weightedMeanSquare(a.summary);
            const double bw = weightedMeanSquare(b.summary);
            if (aw != bw) {
                return aw < bw;
            }
            if (a.summary.globalMaxChain != b.summary.globalMaxChain) {
                return a.summary.globalMaxChain < b.summary.globalMaxChain;
            }
            if (a.summary.dirsFallback != b.summary.dirsFallback) {
                return a.summary.dirsFallback < b.summary.dirsFallback;
            }
            return a.prime < b.prime;
        });

        const auto& bestFixed = trials.front();
        const double bestWeightedMeanSquare = weightedMeanSquare(bestFixed.summary);
        const double bestLoadFactor = globalLoadFactor(bestFixed.summary);
        const double avgWeightedMeanSquare =
            sumWeightedMeanSquare / static_cast<double>(trials.size());
        const double avgLoadFactor =
            sumLoadFactor / static_cast<double>(trials.size());
        const double avgPerfectDirs =
            static_cast<double>(sumPerfectDirs) / static_cast<double>(trials.size());
        const double avgFallbackDirs =
            static_cast<double>(sumFallbackDirs) / static_cast<double>(trials.size());
        const double avgBuckets =
            static_cast<double>(sumBuckets) / static_cast<double>(trials.size());

        std::cout << "Fixed-base large-prime experiment\n";
        std::cout << "  sampled primes:               " << trials.size() << "\n";
        std::cout << "  prime range tested:           [" << kLargePrimeMin << ", " << kLargePrimeMax << ")\n";
        std::cout << "  RNG seed:                     " << options.fixedBaseSeed << "\n";
        std::cout << "  best fixed prime:             " << bestFixed.prime << "\n";
        std::cout << "  best weighted mean-square:    " << std::fixed << std::setprecision(4) << bestWeightedMeanSquare << "\n";
        std::cout << "  current weighted mean-square: " << std::fixed << std::setprecision(4) << baselineWeightedMeanSquare << "\n";
        std::cout << "  delta vs current:             " << std::showpos << std::fixed << std::setprecision(4)
                  << (bestWeightedMeanSquare - baselineWeightedMeanSquare) << std::noshowpos << "\n";
        std::cout << "  best load factor N/T:         " << std::fixed << std::setprecision(4) << bestLoadFactor << "\n";
        std::cout << "  current load factor N/T:      " << std::fixed << std::setprecision(4) << baselineGlobalLoadFactor << "\n";
        std::cout << "  best perfect-hash dirs:       " << bestFixed.summary.dirsPerfect << "\n";
        std::cout << "  current perfect-hash dirs:    " << baseline.dirsPerfect << "\n";
        std::cout << "  best fallback dirs:           " << bestFixed.summary.dirsFallback << "\n";
        std::cout << "  current fallback dirs:        " << baseline.dirsFallback << "\n";
        std::cout << "  best total buckets:           " << bestFixed.summary.totalBuckets << "\n";
        std::cout << "  current total buckets:        " << baseline.totalBuckets << "\n";
        std::cout << "  best max chain observed:      " << bestFixed.summary.globalMaxChain << "\n";
        std::cout << "  current max chain observed:   " << baseline.globalMaxChain << "\n";
        std::cout << "  average weighted mean-square: " << std::fixed << std::setprecision(4) << avgWeightedMeanSquare << "\n";
        std::cout << "  average load factor N/T:      " << std::fixed << std::setprecision(4) << avgLoadFactor << "\n";
        std::cout << "  average perfect-hash dirs:    " << std::fixed << std::setprecision(2) << avgPerfectDirs << "\n";
        std::cout << "  average fallback dirs:        " << std::fixed << std::setprecision(2) << avgFallbackDirs << "\n";
        std::cout << "  average total buckets:        " << std::fixed << std::setprecision(2) << avgBuckets << "\n";
        std::cout << "\n";

        std::cout << "Best fixed-prime worst directories\n";
        std::cout << "  path | N | tablesize | score | max_chain\n";
        for (size_t i = 0; i < topN; ++i) {
            const auto& d = bestFixed.summary.dirs[i];
            std::cout << "  " << d.path
                      << " | " << d.n
                      << " | " << d.choice.tablesize
                      << " | " << std::fixed << std::setprecision(4) << d.choice.score
                      << " | " << d.choice.maxChain
                      << "\n";
        }
        std::cout << "\n";

        const HashSimulationSummary hybrid =
            simulateHashingWithFixedFallback(directories, dirChildren, smallPrimes, kHybridFallbackBase);
        const double hybridWeightedMeanSquare = weightedMeanSquare(hybrid);
        const double hybridLoadFactor = globalLoadFactor(hybrid);

        std::cout << "Hybrid fixed-fallback experiment\n";
        std::cout << "  fallback base:                " << kHybridFallbackBase << "\n";
        std::cout << "  weighted mean-square chain:   " << std::fixed << std::setprecision(4) << hybridWeightedMeanSquare << "\n";
        std::cout << "  current weighted mean-square: " << std::fixed << std::setprecision(4) << baselineWeightedMeanSquare << "\n";
        std::cout << "  delta vs current:             " << std::showpos << std::fixed << std::setprecision(4)
                  << (hybridWeightedMeanSquare - baselineWeightedMeanSquare) << std::noshowpos << "\n";
        std::cout << "  load factor N/T:              " << std::fixed << std::setprecision(4) << hybridLoadFactor << "\n";
        std::cout << "  current load factor N/T:      " << std::fixed << std::setprecision(4) << baselineGlobalLoadFactor << "\n";
        std::cout << "  perfect-hash dirs:            " << hybrid.dirsPerfect << "\n";
        std::cout << "  current perfect-hash dirs:    " << baseline.dirsPerfect << "\n";
        std::cout << "  dirs using fallback path:     " << hybrid.dirsHitCeiling << "\n";
        std::cout << "  current ceiling-hit dirs:     " << baseline.dirsHitCeiling << "\n";
        std::cout << "  fallback dirs score>=1.1:     " << hybrid.dirsFallback << "\n";
        std::cout << "  current fallback dirs:        " << baseline.dirsFallback << "\n";
        std::cout << "  total buckets:                " << hybrid.totalBuckets << "\n";
        std::cout << "  current total buckets:        " << baseline.totalBuckets << "\n";
        std::cout << "  max chain observed:           " << hybrid.globalMaxChain << "\n";
        std::cout << "  current max chain observed:   " << baseline.globalMaxChain << "\n";
        std::cout << "\n";

        std::cout << "Hybrid fallback worst directories\n";
        std::cout << "  path | N | tablesize | p | score | max_chain | hit_ceiling\n";
        for (size_t i = 0; i < topN; ++i) {
            const auto& d = hybrid.dirs[i];
            std::cout << "  " << d.path
                      << " | " << d.n
                      << " | " << d.choice.tablesize
                      << " | " << d.choice.p
                      << " | " << std::fixed << std::setprecision(4) << d.choice.score
                      << " | " << d.choice.maxChain
                      << " | " << (d.choice.hitCeiling ? "yes" : "no")
                      << "\n";
        }
        std::cout << "\n";
    }

    const uint64_t copiedPageEligibleObjects = smallFilePayloads + largePartialPayloads;
    std::cout << "mmap copy-page profile (per canonical file object)\n";
    std::cout << "  small (<PAGE_SIZE):           " << smallFilePayloads << "\n";
    std::cout << "  large page-multiple:          " << largePageAlignedPayloads << "\n";
    std::cout << "  large with tail page:         " << largePartialPayloads << "\n";
    std::cout << "  zero-length payload objects:  " << zeroLenFilePayloads << "\n";
    std::cout << "  objects requiring copied page:" << copiedPageEligibleObjects << "\n";
    std::cout << "\n";

    std::cout << "Content pool packing simulation (page size " << pageSize << ")\n";
    std::cout << "  unique payload objects:       " << filePayloadByInode.size() << "\n";
    std::cout << "  unique filename strings:      " << uniqueNames.size() << "\n";
    std::cout << "  unique symlink targets:       " << uniqueSymlinkTargets.size() << "\n";
    std::cout << "  unique string contents total: " << uniqueStringContents.size() << "\n";
    std::cout << "  packed contents count:        " << packing.contentCount << "\n";
    std::cout << "  raw content bytes:            " << prettyBytes(packing.rawBytes) << "\n";
    std::cout << "  padded content bytes:         " << prettyBytes(packing.paddedBytes) << "\n";
    std::cout << "  padding overhead:             " << prettyBytes(packing.paddingBytes)
              << " (" << std::fixed << std::setprecision(4)
              << ((packing.rawBytes == 0) ? 0.0 :
                  (100.0 * static_cast<double>(packing.paddingBytes) / static_cast<double>(packing.rawBytes)))
              << "%)\n";
    std::cout << "\n";

    std::cout << "Visibility-split content packing\n";
    std::cout << "  assumption:                   public means world-reachable path; symlink targets stay non-public unless also public names\n";
    std::cout << "  public file payload objects:  " << publicFilePayloads << "\n";
    std::cout << "  private file payload objects: " << privateFilePayloads << "\n";
    std::cout << "  public filename strings:      " << publicFilenameStrings << "\n";
    std::cout << "  private filename strings:     " << privateFilenameStrings << "\n";
    std::cout << "  public string contents total: " << publicStringContents << "\n";
    std::cout << "  private string contents total:" << privateStringContents << "\n";
    std::cout << "  public pool raw bytes:        " << prettyBytes(splitPacking.publicPool.rawBytes) << "\n";
    std::cout << "  private pool raw bytes:       " << prettyBytes(splitPacking.privatePool.rawBytes) << "\n";
    std::cout << "  public pool padded bytes:     " << prettyBytes(splitPacking.publicPool.paddedBytes) << "\n";
    std::cout << "  private pool padded bytes:    " << prettyBytes(splitPacking.privatePool.paddedBytes) << "\n";
    std::cout << "  split padded bytes total:     " << prettyBytes(splitPacking.combined.paddedBytes) << "\n";
    std::cout << "  split padding overhead:       " << prettyBytes(splitPacking.combined.paddingBytes)
              << " (" << std::fixed << std::setprecision(4)
              << ((splitPacking.combined.rawBytes == 0) ? 0.0 :
                  (100.0 * static_cast<double>(splitPacking.combined.paddingBytes) /
                   static_cast<double>(splitPacking.combined.rawBytes)))
              << "%)\n";
    std::cout << "  delta padded bytes:           " << std::showpos
              << static_cast<int64_t>(splitPacking.combined.paddedBytes) - static_cast<int64_t>(packing.paddedBytes)
              << std::noshowpos << " B\n";
    std::cout << "\n";

    std::cout << "Whole-image size estimate\n";
    std::cout << "  header bytes:                 " << headerBytes << "\n";
    std::cout << "  metadata table bytes:         " << objectTableBytes << "\n";
    std::cout << "  content_off (aligned):        " << contentOff << "\n";
    std::cout << "  content pool bytes:           " << packing.paddedBytes << "\n";
    std::cout << "  total image estimate:         " << prettyBytes(imageBytesEstimate)
              << " (" << imageBytesEstimate << " bytes)\n";
    std::cout << "  split content pool bytes:     " << splitPacking.combined.paddedBytes << "\n";
    std::cout << "  split image estimate:         " << prettyBytes(splitImageBytesEstimate)
              << " (" << splitImageBytesEstimate << " bytes)\n";
    std::cout << "\n";

    std::cout << "Assumptions\n";
    std::cout << "  - regular-file payload dedup estimated by inode identity (captures hardlinks).\n";
    std::cout << "  - cross-inode byte-identical dedup is unknown from find -ls and not modeled.\n";
    std::cout << "  - directory hash quality is exact for names present in input list.\n";

    return 0;
}
