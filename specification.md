# fgsfs: zero-fsck-giving, zero-frills, read-only, near-zero-copy filesystem image format

This document defines a compact, memory-friendly filesystem image format.

## Scope

0fs is designed for:
- FreeBSD 14+, amd64 and arm64.
- Read-only mounts.
- RAM-backed images (`md(4)` workflow).
- Fast O(1) lookup by hashed directory entries.
- `mmap(2)` with at most one copied page per file object.

Design priorities:
- Runtime lookup, read, and `mmap(2)` behavior take precedence over image-build speed.
- Builder work must remain reasonable, but offline search and packing cost is acceptable.
- Primary optimization targets are runtime performance and final image size.

Out of scope:
- In-place writes.
- Journaling.
- ACLs and rich metadata.

Hard limits:
- Total image size `< 2^32` bytes.
- Metadata-table cells `< 2^16`.
- Theoretical guaranteed visible filesystem-object bound: `< 2^15`.
- In practical trees, attr deduplication and non-pathological hash occupancy should
  allow visible object counts much closer to `< 2^16`.
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
_Static_assert(sizeof(struct zerofs_attr) == 16, "zerofs_attr size");
```

---

## Format Overview

The image is a single contiguous blob with three regions, in order:

1. Header (fixed, 128 bytes).
2. Metadata table (fixed cells, 16 bytes each).
3. Content pool (file bytes, filenames, symlink targets).

All offsets are byte offsets relative to image start.

The metadata table serves a triple role:
- object metadata (`file`, `dir`, `symlink`, `hardlink`)
- directory hash buckets
- deduplicated attribute records

There is no separate hash-table region.
There is no separate attribute-table region; attribute records are cells in the
same 16-byte metadata table.

---

## Header (`zerofs_header`)

```c
struct zerofs_header {
    uint8_t  magic[4];       // "0FS\0"
    uint16_t version;        // format version
    uint16_t reserved;       // must be 0

    uint32_t cell_count;     // number of entries in metadata table
    uint32_t content_off;    // content pool byte offset (must be page-aligned)
    uint32_t meta_size;      // metadata table size in bytes
    uint32_t meta_hash;      // integrity hash over metadata table

    uint32_t root_first;     // root bucket slice start index in metadata table
    uint32_t root_size;      // root tablesize
    uint32_t root_real;      // root N (actual entry count)
    uint16_t root_attr;      // attribute cell index for root directory
    uint8_t  root_p;         // root polynomial base p
    uint8_t  root_pad;       // must be 0

    uint8_t  author[88];     // NUL-terminated, zero-padded
};                           // exactly 128 bytes
```

Notes:
- Header starts at byte 0.
- Metadata table starts at byte 128.
- `meta_hash` is a sanity check (non-cryptographic hash such as FNV-1a or
  xxHash32).
- Root descriptor is in header because root has no parent bucket entry.
- Attribute cells may appear anywhere in the metadata table.

---

## Metadata Table Cells

```c
struct zerofs_object {
    uint8_t  type;        // ZEROFS_TYPE_*
    uint8_t  hash_p;      // directories: polynomial base p; otherwise 0
    uint16_t attr_index;  // attribute cell index; ignored for buckets
    uint32_t data_off;    // role-specific (see below)
    uint32_t size;        // role-specific (see below)
    uint32_t realsize;    // role-specific (see below)
};                        // exactly 16 bytes

struct zerofs_attr {
    uint8_t  type;        // ZEROFS_TYPE_ATTR
    uint8_t  reserved0;   // must be 0
    uint16_t mode;        // st_mode bits (type + perms + suid/sgid/sticky)
    uint32_t uid;         // numeric owner
    uint32_t gid;         // numeric group
    uint32_t ext_off;     // 0 or byte offset of zerofs_metaext in content pool
};                        // exactly 16 bytes
```

```c
#define ZEROFS_TYPE_NULL      0
#define ZEROFS_TYPE_FILE      1
#define ZEROFS_TYPE_DIR       2
#define ZEROFS_TYPE_SYMLINK   3
#define ZEROFS_TYPE_HARDLINK  4
#define ZEROFS_TYPE_AUX       0x80
#define ZEROFS_TYPE_ATTR      (ZEROFS_TYPE_AUX | 1)
```

Object cells and attribute cells share the same 16-byte table. Their role is
identified by the leading `type` byte. Bucket cells are represented using the
`zerofs_object` layout with bucket semantics described below.

Auxiliary cell rule:
- Any cell whose `type` has `ZEROFS_TYPE_AUX` set is not a directory bucket
  occupant and must be treated as empty by hash lookup and directory iteration.
- `ZEROFS_TYPE_ATTR` is the only defined auxiliary cell type in this version.

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

Metadata rules:
- Every non-root non-bucket object references a deduplicated attribute record
  via `attr_index`.
- `zerofs_attr.mode` stores the exported `st_mode` bits, including permission
  bits, setuid, setgid, sticky, and file type bits.
- `type` and the file-type bits in the resolved `mode` must agree.
- `zerofs_attr.uid` and `zerofs_attr.gid` store numeric FreeBSD owner/group IDs
  directly.
- `zerofs_attr.ext_off == 0` means there is no extended metadata.
- `zerofs_attr.ext_off != 0` points to a structured metadata-extension record
  in the content pool (see `zerofs_metaext` below).

`ZEROFS_TYPE_DIR`:
- `attr_index`: attribute cell index for this directory
- `data_off`: first bucket index in metadata table
- `size`: `tablesize` (bucket count)
- `realsize`: packed `{ parent_index, N }`

`ZEROFS_TYPE_FILE` and `ZEROFS_TYPE_SYMLINK`:
- `attr_index`: attribute cell index for this object
- `data_off`: byte offset into content pool
- `size`: byte length
- `realsize`: `0`

`ZEROFS_TYPE_HARDLINK`:
- `attr_index`: `0` (ignored; attributes come from target object)
- `data_off`: target object index (not byte offset)
- `size`: `0`
- `realsize`: `0`
- `data_off` storage is `uint32_t`, but the valid domain is `< 2^16`
  (`cell_count < 2^16`), so this is logically a `u16` index carried in
  a `u32` field (upper bits must be zero).

Hardlink constraints:
- Target index must be `< cell_count`.
- Target must be a non-hardlink regular file object.
- No hardlinks to directories.

`ZEROFS_TYPE_ATTR`:
- auxiliary metadata cells that may appear anywhere in the metadata table
- never referenced by directory buckets directly
- never resolved as filesystem objects through path lookup

Attribute-cell placement policy (builder):
1. Build object cells and directory bucket slices first.
2. Deduplicate attribute tuples by exact `(mode, uid, gid, ext_off)` equality.
3. Place each attribute cell:
   - first choice: first unused cell on the same page as the first referencing object
   - second choice: first unused cell on the same page as the next referencing object
   - third choice: if no more referencing objects remain, first unused cell anywhere in the existing table
   - last choice: extend the metadata table
4. Record only the chosen `attr_index`; readers must not assume any contiguous
   attribute range.

### Bucket Entries (Directory Hash Slice)

Bucket entries are 16-byte cells using the `zerofs_object` layout in a
directory's bucket slice.

Occupied bucket:
- `type`: referenced object type (non-null)
- `hash_p`: `0` (unused in bucket role)
- `attr_index`: `0` (unused in bucket role)
- `data_off`: referenced object index
- `size`: filename offset in content pool (NUL-terminated string)
- `realsize`: next bucket index in chain, or `tablesize` as end sentinel

Empty bucket:
- `type = ZEROFS_TYPE_NULL`
- `realsize = tablesize`

Bucket interpretation rules:
- In directory hash lookup, a cell is treated as empty if `type` is
  `ZEROFS_TYPE_NULL` or if `type & ZEROFS_TYPE_AUX` is nonzero.
- Builders must never emit chain links that target an auxiliary cell.

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

Ownership and mode resolution:
- root attributes come from attribute cell `root_attr`.
- non-hardlink object attributes come from attribute cell `obj[i].attr_index`.
- hardlink object attributes resolve through the canonical target object.

Attribute constraints:
- `root_attr` must be `< cell_count` and refer to a `ZEROFS_TYPE_ATTR` cell.
- For `ZEROFS_TYPE_FILE`, `ZEROFS_TYPE_DIR`, and `ZEROFS_TYPE_SYMLINK`,
  `attr_index` must be `< cell_count` and refer to a `ZEROFS_TYPE_ATTR` cell.
- Attribute records may be shared by multiple objects.

### Metadata Extension Hook (`zerofs_metaext`)

`zerofs_attr.ext_off` reserves an optional extension pointer for richer
metadata without affecting the common case.

```c
struct zerofs_metaext {
    uint16_t version;      // extension record version
    uint16_t kind_flags;   // ZEROFS_METAEXT_* bits
    uint32_t acl_off;      // 0 or content-pool offset of ACL payload
    uint32_t xattr_off;    // 0 or content-pool offset of xattr payload
    uint32_t payload_size; // total bytes owned by this extension record
};                         // exactly 16 bytes
```

```c
#define ZEROFS_METAEXT_ACL    0x0001u
#define ZEROFS_METAEXT_XATTR  0x0002u
```

Rules:
- `ext_off == 0` means no ACLs and no extended attributes.
- `ext_off != 0` points to `zerofs_metaext` stored in the content pool.
- ACL and xattr payload encoding is versioned by `zerofs_metaext.version`.
- Readers that do not support a referenced extension record must reject mount
  or object activation as unsupported.

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
    return &cell_table[parent];
}

buckets = &cell_table[dir->data_off];
tablesize = dir->size;

if (tablesize == 0)
    return ENOENT;

idx = zerofs_hash(query, dir->hash_p, tablesize);
if (buckets[idx].type == ZEROFS_TYPE_NULL)
    return ENOENT;

for (;;) {
    name = content_pool + buckets[idx].size;
    if (strcmp(name, query) == 0)
        return &cell_table[buckets[idx].data_off];
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
  containing file bytes + zero-fill remainder. This page is the
  object's final page and therefore also the only copied page.
- File size `>= PAGE_SIZE` and not page-multiple: map full leading pages
  directly from image, map final partial page from one cached
  anonymous page (tail bytes + zero-fill).
- File size page-multiple: fully direct-mapped from image.

Operational summary:
- A mapped file object needs at most one synthetic cached page: its last page.
- That cached page is populated lazily on first use, then reused across later
  mappings of the same copied source region.
- For file objects smaller than `PAGE_SIZE`, the last-page cache page is the
  entire mapping payload.

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
- At most one page is copied per mapped file object, namely the final page.
- The copied final page is cached lazily on first use and reused across
  subsequent mappings of the same copied source region.

---

## Alignment and Padding

Header and metadata table:
- `zerofs_header` is exactly 128 bytes.
- `zerofs_object` is exactly 16 bytes.
- `zerofs_attr` is exactly 16 bytes.
- No inter-record padding.

Content pool:
- `content_off` must be page-aligned.
- Contents `>= PAGE_SIZE` are page-aligned by packing algorithm.
- Contents `< PAGE_SIZE` must remain page-contained.
