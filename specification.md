# fgsfs: zero-fsck-giving, zero-frills, read-only, near-zero-copy filesystem image format

This document defines a compact, memory-friendly filesystem image format.

## Scope

0fs is designed for:
- FreeBSD 14+, amd64 and arm64.
- Read-only mounts.
- RAM-backed images (`md(4)` workflow).
- Fast O(1) lookup by hashed directory entries.
- `mmap(2)` with at most one copied page per file object.

Out of scope:
- In-place writes.
- Journaling.
- ACLs and rich metadata.

Hard limits:
- Total image size `< 2^32` bytes.
- Number of objects (files + directories + links) `< 2^16`.
- Entries in a single directory `< 2^15`.

---

## On-disk ABI Rules

All integers are little-endian and fixed-width:
- `uint8_t`
- `uint16_t`
- `uint32_t`

Never use native-width types (`int`, `long`, `size_t`, `off_t`) in on-disk
records.

C implementations must enforce layout with compile-time checks:

```c
_Static_assert(sizeof(struct zerofs_header) == 128, "zerofs_header size");
_Static_assert(sizeof(struct zerofs_object) == 16, "zerofs_object size");
```

---

## Format Overview

The image is a single contiguous blob with three regions, in order:

1. Header (fixed, 128 bytes).
2. Object table (fixed records, 16 bytes each).
3. Content pool (file bytes, filenames, symlink targets).

All offsets are byte offsets relative to image start.

The object table serves a dual role:
- object metadata (`file`, `dir`, `symlink`, `hardlink`)
- directory hash buckets (stored as `zerofs_object` entries too)

There is no separate hash-table region.

---

## Header (`zerofs_header`)

```c
struct zerofs_header {
    uint8_t  magic[4];       // "0FS\0"
    uint16_t version;        // format version
    uint16_t reserved;       // must be 0

    uint32_t object_count;   // number of entries in object table
    uint32_t content_off;    // content pool byte offset (must be page-aligned)
    uint32_t meta_size;      // object table size in bytes
    uint32_t meta_hash;      // integrity hash over object table

    uint32_t root_first;     // root bucket slice start index in object table
    uint32_t root_size;      // root tablesize
    uint32_t root_real;      // root N (actual entry count)
    uint8_t  root_p;         // root polynomial base p
    uint8_t  root_pad[3];    // must be 0

    uint8_t  author[88];     // NUL-terminated, zero-padded
};                           // exactly 128 bytes
```

Notes:
- Header starts at byte 0.
- Object table starts at byte 128.
- `meta_hash` is a sanity check (non-cryptographic hash such as FNV-1a or
  xxHash32).
- Root descriptor is in header because root has no parent bucket entry.

---

## Object Table (`zerofs_object`)

```c
struct zerofs_object {
    uint8_t  type;        // ZEROFS_TYPE_*
    uint8_t  hash_p;      // directories: polynomial base p; otherwise 0
    uint16_t mode_flags;  // unix mode bits + object flags
    uint32_t data_off;    // role-specific (see below)
    uint32_t size;        // role-specific (see below)
    uint32_t realsize;    // role-specific (see below)
};                        // exactly 16 bytes
```

```c
#define ZEROFS_TYPE_NULL      0
#define ZEROFS_TYPE_FILE      1
#define ZEROFS_TYPE_DIR       2
#define ZEROFS_TYPE_SYMLINK   3
#define ZEROFS_TYPE_HARDLINK  4
```

### Directory Metadata Packing (`realsize`)

For `ZEROFS_TYPE_DIR`, `realsize` packs both parent pointer and `N`:

- bits `[15:0]`: `parent_index` (`uint16_t`)
- bits `[30:16]`: `N` (actual entry count), so `N < 2^15`
- bit `[31]`: reserved, must be `0`

`parent_index = 0xFFFF` is reserved as `ZEROFS_PARENT_ROOT` (parent is root
descriptor in header). This avoids cross-filesystem `..` links in on-disk
metadata.

```c
#define ZEROFS_PARENT_ROOT      0xFFFFu
#define ZEROFS_DIR_PARENT_MASK  0x0000FFFFu
#define ZEROFS_DIR_N_MASK       0x7FFF0000u
#define ZEROFS_DIR_N_SHIFT      16

#define ZEROFS_DIR_GET_N(x) \
    ((uint16_t)(((x) & ZEROFS_DIR_N_MASK) >> ZEROFS_DIR_N_SHIFT))
#define ZEROFS_DIR_GET_PARENT(x) \
    ((uint16_t)((x) & ZEROFS_DIR_PARENT_MASK))
#define ZEROFS_DIR_PACK(parent, n) \
    (((uint32_t)(parent) & ZEROFS_DIR_PARENT_MASK) | \
     ((((uint32_t)(n)) & 0x7FFFu) << ZEROFS_DIR_N_SHIFT))
```

### Entry Roles

`ZEROFS_TYPE_DIR`:
- `data_off`: first bucket index in object table
- `size`: `tablesize` (bucket count)
- `realsize`: packed `{ parent_index, N }`

`ZEROFS_TYPE_FILE` and `ZEROFS_TYPE_SYMLINK`:
- `data_off`: byte offset into content pool
- `size`: byte length
- `realsize`: `0`

`ZEROFS_TYPE_HARDLINK`:
- `data_off`: target object index (not byte offset)
- `size`: `0`
- `realsize`: `0`
- `data_off` storage is `uint32_t`, but the valid domain is `< 2^16`
  (`object_count < 2^16`), so this is logically a `u16` index carried in
  a `u32` field (upper bits must be zero).

Hardlink constraints:
- Target index must be `< object_count`.
- Target must be a non-hardlink regular file object.
- No hardlinks to directories.

### Bucket Entries (Directory Hash Slice)

Bucket entries are `zerofs_object` records in a directory's bucket slice.

Occupied bucket:
- `type`: referenced object type (non-null)
- `hash_p`: `0` (unused in bucket role)
- `mode_flags`: `0` (unused in bucket role)
- `data_off`: referenced object index
- `size`: filename offset in content pool (NUL-terminated string)
- `realsize`: next bucket index in chain, or `tablesize` as end sentinel

Empty bucket:
- `type = ZEROFS_TYPE_NULL`
- `realsize = tablesize`

---

## Inode and Hardlink Semantics

Inode identity is based on canonical object index, never on `data_off`
payload offsets.

Canonical index:
- for non-hardlink object at index `i`: canonical index is `i`
- for hardlink object at index `i`: canonical index is `obj[i].data_off`

Recommended exported inode numbers:
- root: `st_ino = 1`
- non-root: `st_ino = canonical_index + 2`

`st_nlink` is derived by counting directory entries resolving to the same
canonical index.

---

## Directory Lookup and `.` / `..`

### Hash Function

```c
uint32_t zerofs_hash(const char *name, uint8_t p, uint32_t tablesize) {
    if (tablesize == 0) return 0;
    if (tablesize == 1) return 0;
    uint32_t h = 0;
    while (*name)
        h = h * p + (unsigned char)*name++;
    return h % tablesize;
}
```

### Lookup Algorithm

```c
// root uses header fields; non-root uses dir object fields
if (strcmp(query, ".") == 0)
    return current_directory;

if (strcmp(query, "..") == 0) {
    if (is_root_directory)
        return root_directory;
    parent = ZEROFS_DIR_GET_PARENT(dir->realsize);
    if (parent == ZEROFS_PARENT_ROOT)
        return root_directory;
    return &obj_table[parent];
}

buckets = &obj_table[dir->data_off];
tablesize = dir->size;

if (tablesize == 0)
    return ENOENT;

idx = zerofs_hash(query, dir->hash_p, tablesize);
if (buckets[idx].type == ZEROFS_TYPE_NULL)
    return ENOENT;

for (;;) {
    name = content_pool + buckets[idx].size;
    if (strcmp(name, query) == 0)
        return &obj_table[buckets[idx].data_off];
    if (buckets[idx].realsize == tablesize)
        return ENOENT;
    idx = buckets[idx].realsize;
}
```

### Directory Listing

`readdir` synthesizes:
- `.`
- `..`

Then it scans occupied buckets (`type != ZEROFS_TYPE_NULL`) in the directory
slice. Listing order for hash buckets is undefined.

---

## Choosing `p` and `tablesize` at Build Time

Goal: minimize `sum_of_squares(chain_lengths) / N` while keeping table small.

Definitions:

```c
MAX_TABLESIZE_PRIME = 65521   // largest prime < 2^16
```

Algorithm:

```text
if N == 0:
    return (p=0, tablesize=0)

if N == 1:
    return (p=0, tablesize=1)

best_p      = 0
best_size   = N
best_score  = +inf
tablesize   = N                           // phase 1: fully packed
ceiling     = min(next_prime(2 * N), MAX_TABLESIZE_PRIME)

loop:
    for each p in SMALL_PRIMES:           // fixed list, e.g. up to 251
        simulate coalesced hash with (p, tablesize)
        score = sum_of_squares(chain_lengths) / N

        if score == 1.0:
            return (p, tablesize)

        if score < best_score:
            best_score = score
            best_p = p
            best_size = tablesize

    if best_score < 1.1:
        return (best_p, best_size)

    next = next_prime(tablesize)
    if next > ceiling:
        return (best_p, best_size)        // best-effort fallback at ceiling
    tablesize = next
```

Notes:
- `N < 2^15` and `MAX_TABLESIZE_PRIME = 65521` guarantee representable
  indices and sentinel behavior.
- For `N = 32767`, `next_prime(2*N) = 65537` is out of bound; algorithm
  clamps to `65521` and returns best result seen.
- Phase 1 uses `tablesize = N` even if `N` is not prime.
- Phase 2 grows through primes only.

---

## Content Pool

The content pool is an unstructured byte arena containing:
- regular file data
- filenames (NUL-terminated, deduplicated)
- symlink target strings

Interpretation is determined by references, not embedded tags.

### Packing Algorithm

All contents are sorted descending by size.
A sorted free-hole set (by hole size, then offset) tracks page-padding gaps.

For each content:

1. Try smallest fitting hole.
2. If no hole fits, append at tail.
3. If append would straddle page, advance tail to next page and record gap.

Consequences:
- Contents `>= PAGE_SIZE` become page-aligned by construction.
- Holes are always `< PAGE_SIZE`.
- Sub-page contents stay page-contained.

### Deduplication

Identical byte sequences are stored once (SHA-512 keyed dedup in builder).
Multiple objects may reference the same content bytes.

---

## `mmap(2)` Contract (No-Leak Policy)

0fs intentionally uses stricter policy than generic FreeBSD `mmap` behavior.

Accepted mapping constraints:
- `offset` must be page-aligned, otherwise `EINVAL`.
- filesystem is read-only; writeable mappings are rejected.

Serving policy:
- File size `< PAGE_SIZE`: map from one cached anonymous page
  containing file bytes + zero-fill remainder.
- File size `>= PAGE_SIZE` and not page-multiple: map full leading pages
  directly from image, map final partial page from one cached
  anonymous page (tail bytes + zero-fill).
- File size page-multiple: fully direct-mapped from image.

Anonymous-page cache key:
- key by stable source location of the copied bytes, i.e. image-relative
  offset of the first copied byte (`src_first_byte_off`), not by raw
  on-disk `data_off` field and not by transient physical address.
- this naturally shares cached pages across hardlinks and deduplicated
  file contents whenever the copied source region is identical.
- implementation may also record `copied_len` in the cache entry as a
  runtime consistency check.

Security and performance properties:
- No bytes from neighboring packed contents are exposed through file mappings.
- Returned addresses are page-aligned under accepted constraints.
- At most one page is copied per unique copied source region (lazy first
  use, then reused across subsequent mappings).

---

## Alignment and Padding

Header and object table:
- `zerofs_header` is exactly 128 bytes.
- `zerofs_object` is exactly 16 bytes.
- No inter-record padding.

Content pool:
- `content_off` must be page-aligned.
- Contents `>= PAGE_SIZE` are page-aligned by packing algorithm.
- Contents `< PAGE_SIZE` must remain page-contained.

