#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <utilities/isPrime.h>

namespace {

constexpr uint32_t kMaxTablesizePrime = 65521u;
constexpr uint32_t kDirEntryHardLimit = 1u << 15; // N < 2^15

struct FsEntry {
    uint64_t inode = 0;
    char type = '?'; // d, -, l
    uint64_t size = 0;
    std::string path;
    std::string parent;
    std::string name;
    std::string symlinkTarget;
};

struct HashChoice {
    uint32_t tablesize = 0;
    uint8_t p = 0;
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

struct DirReport {
    std::string path;
    uint32_t n = 0;
    HashChoice choice;
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
        R"(^\s*([0-9]+)\s+([0-9]+)\s+([bcdlps-][rwxstST-]{9})\s+([0-9]+)\s+\S+\s+\S+\s+([0-9]+)\s+[A-Za-z]{3}\s+[0-9]{1,2}\s+([0-9]{2}:[0-9]{2}|[0-9]{4})\s+(.+)$)");

    std::smatch m;
    if (!std::regex_match(line, m, re)) {
        return false;
    }

    FsEntry e;
    e.inode = std::stoull(m[1].str());
    const std::string mode = m[3].str();
    e.type = mode.empty() ? '?' : mode[0];
    e.size = std::stoull(m[5].str());

    std::string pathField = m[7].str();
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

std::vector<uint8_t> smallPrimesUpTo251() {
    std::vector<uint8_t> out;
    for (uint32_t p = 2; p <= 251; ++p) {
        if (utilities::isPrime(p)) {
            out.push_back(static_cast<uint8_t>(p));
        }
    }
    return out;
}

uint32_t zerofsHash(const std::string& name, uint8_t p, uint32_t tablesize) {
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

Score scoreDirectory(const std::vector<std::string>& names, uint8_t p, uint32_t tablesize) {
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

HashChoice chooseHash(const std::vector<std::string>& names, const std::vector<uint8_t>& smallPrimes) {
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
    uint8_t bestP = 0;
    uint32_t bestSize = n;
    uint32_t bestMaxChain = 0;
    uint64_t bestSumSquares = 0;

    for (;;) {
        for (uint8_t p : smallPrimes) {
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
    const std::string inputPath = (argc >= 2) ? argv[1] : "misc/list";
    const uint32_t pageSize = (argc >= 3) ? static_cast<uint32_t>(std::stoul(argv[2])) : 4096u;
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

    std::set<std::string> directories;
    directories.insert(""); // root

    std::unordered_map<std::string, std::vector<std::string>> dirChildren;
    dirChildren.reserve(1024);

    std::unordered_map<uint64_t, uint64_t> filePayloadByInode;
    std::unordered_map<uint64_t, uint32_t> filePathCountByInode;
    filePayloadByInode.reserve(4096);
    filePathCountByInode.reserve(4096);

    std::set<std::string> uniqueNames;
    std::set<std::string> uniqueSymlinkTargets;

    uint64_t countDir = 0;
    uint64_t countFilePath = 0;
    uint64_t countSymlink = 0;
    uint64_t unsupported = 0;

    uint64_t duplicateInodeSizeMismatch = 0;

    for (const auto& e : entries) {
        if (e.type == 'd') {
            ++countDir;
            directories.insert(e.path);
            if (!e.path.empty()) {
                dirChildren[e.parent].push_back(e.name);
                uniqueNames.insert(e.name);
            }
            continue;
        }
        if (e.type == '-') {
            ++countFilePath;
            dirChildren[e.parent].push_back(e.name);
            uniqueNames.insert(e.name);
            auto [it, inserted] = filePayloadByInode.emplace(e.inode, e.size);
            if (!inserted && it->second != e.size) {
                ++duplicateInodeSizeMismatch;
            }
            ++filePathCountByInode[e.inode];
            continue;
        }
        if (e.type == 'l') {
            ++countSymlink;
            dirChildren[e.parent].push_back(e.name);
            uniqueNames.insert(e.name);
            uniqueSymlinkTargets.insert(e.symlinkTarget);
            continue;
        }
        ++unsupported;
    }

    // Ensure every parsed directory has a child vector, including empty dirs.
    for (const auto& d : directories) {
        (void)dirChildren[d];
    }

    std::vector<DirReport> dirs;
    dirs.reserve(directories.size());

    uint64_t totalDirEntries = 0;
    uint64_t totalBuckets = 0;
    uint64_t totalEmptyBuckets = 0;
    uint64_t totalSumSquares = 0;
    uint32_t globalMaxChain = 0;
    uint64_t dirsPerfect = 0;
    uint64_t dirsHitCeiling = 0;
    uint64_t dirsFallback = 0;
    uint64_t dirsOverHardLimit = 0;

    for (const auto& d : directories) {
        const auto& names = dirChildren[d];
        const uint32_t n = static_cast<uint32_t>(names.size());
        if (n >= kDirEntryHardLimit) {
            ++dirsOverHardLimit;
        }

        HashChoice hc = chooseHash(names, smallPrimes);
        if (hc.score == 1.0 && n > 0) {
            ++dirsPerfect;
        }
        if (hc.hitCeiling) {
            ++dirsHitCeiling;
            if (hc.score >= 1.1) {
                ++dirsFallback;
            }
        }

        totalDirEntries += n;
        totalBuckets += hc.tablesize;
        totalEmptyBuckets += hc.empties;
        totalSumSquares += hc.sumSquares;
        globalMaxChain = std::max(globalMaxChain, hc.maxChain);

        dirs.push_back(DirReport{d.empty() ? "/" : d, n, hc});
    }

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

    // Content sizes for packing simulation.
    std::vector<uint32_t> contentSizes;
    contentSizes.reserve(filePayloadByInode.size() + uniqueNames.size() + uniqueSymlinkTargets.size());

    uint64_t zeroLenFilePayloads = 0;
    uint64_t smallFilePayloads = 0;
    uint64_t largePageAlignedPayloads = 0;
    uint64_t largePartialPayloads = 0;

    for (const auto& [inode, size] : filePayloadByInode) {
        (void)inode;
        if (size == 0) {
            ++zeroLenFilePayloads;
            continue;
        }
        if (size <= std::numeric_limits<uint32_t>::max()) {
            contentSizes.push_back(static_cast<uint32_t>(size));
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
    }
    for (const auto& target : uniqueSymlinkTargets) {
        uniqueStringContents.insert(target);
    }
    for (const auto& s : uniqueStringContents) {
        const uint64_t bytes = static_cast<uint64_t>(s.size()) + 1u;
        if (bytes <= std::numeric_limits<uint32_t>::max()) {
            contentSizes.push_back(static_cast<uint32_t>(bytes));
        }
    }

    const PackingStats packing = packContents(contentSizes, pageSize);

    const uint64_t objectTableEntries = metadataObjects + totalBuckets;
    const uint64_t objectTableBytes = objectTableEntries * 16u;
    const uint32_t headerBytes = 128u;
    const uint32_t contentOff = alignUp(headerBytes + static_cast<uint32_t>(objectTableBytes), pageSize);
    const uint64_t imageBytesEstimate = static_cast<uint64_t>(contentOff) + packing.paddedBytes;

    std::sort(dirs.begin(), dirs.end(), [](const DirReport& a, const DirReport& b) {
        if (a.choice.score != b.choice.score) {
            return a.choice.score > b.choice.score;
        }
        if (a.choice.maxChain != b.choice.maxChain) {
            return a.choice.maxChain > b.choice.maxChain;
        }
        return a.n > b.n;
    });

    const size_t topN = std::min<size_t>(12, dirs.size());

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
    std::cout << "\n";

    std::cout << "0fs object model estimate\n";
    std::cout << "  file objects (unique inodes): " << fileObjectCount << "\n";
    std::cout << "  hardlink objects:             " << hardlinkObjectCount << "\n";
    std::cout << "  dir objects (non-root):       " << dirObjectCount << "\n";
    std::cout << "  symlink objects:              " << symlinkObjectCount << "\n";
    std::cout << "  metadata objects total:       " << metadataObjects << "\n";
    std::cout << "  hash buckets total:           " << totalBuckets << "\n";
    std::cout << "  object table entries total:   " << objectTableEntries
              << " (limit < 65536)\n";
    std::cout << "  object table bytes:           " << prettyBytes(objectTableBytes) << "\n";
    std::cout << "\n";

    const double weightedMeanSquare =
        (totalDirEntries == 0) ? 0.0 : static_cast<double>(totalSumSquares) / static_cast<double>(totalDirEntries);
    const double avgEmptiesPerDir =
        directories.empty() ? 0.0 : static_cast<double>(totalEmptyBuckets) / static_cast<double>(directories.size());
    const double globalLoadFactor =
        (totalBuckets == 0) ? 0.0 : static_cast<double>(totalDirEntries) / static_cast<double>(totalBuckets);

    std::cout << "Directory hashing simulation\n";
    std::cout << "  ceiling policy:              all sizes in [N, ceil(1.5*N)], cap " << kMaxTablesizePrime << "\n";
    std::cout << "  directories simulated:        " << directories.size() << "\n";
    std::cout << "  total dir entries (N sum):    " << totalDirEntries << "\n";
    std::cout << "  total buckets:                " << totalBuckets << "\n";
    std::cout << "  total empty buckets:          " << totalEmptyBuckets << "\n";
    std::cout << "  avg empty buckets / dir:      " << std::fixed << std::setprecision(2) << avgEmptiesPerDir << "\n";
    std::cout << "  global load factor N/T:       " << std::fixed << std::setprecision(4) << globalLoadFactor << "\n";
    std::cout << "  weighted mean-square chain:   " << std::fixed << std::setprecision(4) << weightedMeanSquare << "\n";
    std::cout << "  perfect-hash dirs (score=1):  " << dirsPerfect << "\n";
    std::cout << "  dirs hitting ceiling:         " << dirsHitCeiling << "\n";
    std::cout << "  dirs fallback(score>=1.1):    " << dirsFallback << "\n";
    std::cout << "  max chain observed:           " << globalMaxChain << "\n";
    std::cout << "  dirs violating N<2^15:        " << dirsOverHardLimit << "\n";
    std::cout << "\n";

    std::cout << "Worst directories by score\n";
    std::cout << "  path | N | tablesize | p | score | max_chain | empties | hit_ceiling\n";
    for (size_t i = 0; i < topN; ++i) {
        const auto& d = dirs[i];
        std::cout << "  " << d.path
                  << " | " << d.n
                  << " | " << d.choice.tablesize
                  << " | " << static_cast<uint32_t>(d.choice.p)
                  << " | " << std::fixed << std::setprecision(4) << d.choice.score
                  << " | " << d.choice.maxChain
                  << " | " << d.choice.empties
                  << " | " << (d.choice.hitCeiling ? "yes" : "no")
                  << "\n";
    }
    std::cout << "\n";

    std::cout << "Fallback directories (hit ceiling with score >= 1.1)\n";
    std::cout << "  path | N | score\n";
    for (const auto& d : dirs) {
        if (!(d.choice.hitCeiling && d.choice.score >= 1.1)) {
            continue;
        }
        std::cout << "  " << d.path
                  << " | " << d.n
                  << " | " << std::fixed << std::setprecision(4) << d.choice.score
                  << "\n";
    }
    std::cout << "\n";

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

    std::cout << "Whole-image size estimate\n";
    std::cout << "  header bytes:                 " << headerBytes << "\n";
    std::cout << "  object table bytes:           " << objectTableBytes << "\n";
    std::cout << "  content_off (aligned):        " << contentOff << "\n";
    std::cout << "  content pool bytes:           " << packing.paddedBytes << "\n";
    std::cout << "  total image estimate:         " << prettyBytes(imageBytesEstimate)
              << " (" << imageBytesEstimate << " bytes)\n";
    std::cout << "\n";

    std::cout << "Assumptions\n";
    std::cout << "  - regular-file payload dedup estimated by inode identity (captures hardlinks).\n";
    std::cout << "  - cross-inode byte-identical dedup is unknown from find -ls and not modeled.\n";
    std::cout << "  - directory hash quality is exact for names present in input list.\n";

    return 0;
}

