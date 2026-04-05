# femtoFS: zero-fsck-giving, zero-frills, read-only, near-zero-copy filesystem image format

This document defines a compact, memory-friendly filesystem image format.

## Scope

The aim is to provide a filesystem image format optimal for booting a "firmware-like"
FreeBSD system in a highly resource-constrained environment.

femtoFS is designed for:
- FreeBSD 14+, amd64 and arm64.
- Read-only mounts.
- RAM-backed images (`md(4)` workflow).
- Fast O(1) lookup by hashed directory entries.
- Near-zero-copy `mmap(2)`.

Design priorities:
- Runtime performance for lookup, read, and `mmap(2)` behavior take precedence over image build speed.
- Builder work must remain reasonable, but offline search and packing costs are acceptable.
- Primary optimization targets are runtime performance and final image size.

Out of scope:
- In-place writes.
- Journaling.
- ACLs and rich metadata (but see `femtofs_metaext` below).

Hard limits:
- Total image size `< 2^32` bytes.
- Metadata table cells `< 2^16`.
- Guaranteed upper bound on visible filesystem objects: `< 2^15`.
- In practical trees, attribute deduplication and non-pathological hash occupancy
  should allow visible object counts much closer to `< 2^16`.
- Entries in a single directory `< 2^15`.

Near-zero-copy `mmap(2)` is exposed through three mount modes:
- `dirty`: superset of `leaking` for small public files. For files in the
  public part referenced by public-eligible file objects with
  `size < PAGE_SIZE`, implementation may accept non-page-aligned offsets and
  may use shifted mappings to preserve zero-copy behavior. For non-public file
  objects and for all files with `size >= PAGE_SIZE`, `dirty` keeps the
  `leaking` alignment contract.
- `leaking`: page-aligned mappings only. Preserves zero-copy behavior for
  practically mappable public files (typically `>= PAGE_SIZE`) and may expose
  neighboring bytes outside the requested range, again only from the public
  part.
- `clean` (default): POSIX-style behavior. Returned mappings are page-aligned
  and bytes outside file bounds are zero-filled. This may require copying at
  most one page per mapped file object.

Security note:
- In this version, public/private classification is defined only by classic
  Unix ownership+permission visibility rules on exported paths.
- If a future version makes ACL/MAC policy semantics active for access control,
  implementations should force `clean` mode unless the public/private
  classifier is extended to include those policies.
- Otherwise, `leaking`/`dirty` could expose neighboring bytes that are
  world-visible by mode bits but denied by ACL/MAC policy.

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
_Static_assert(sizeof(struct femtofs_header) == 128, "femtofs_header size");
_Static_assert(sizeof(union femtofs_object_role) == 12, "femtofs_object_role size");
_Static_assert(sizeof(struct femtofs_object) == 16, "femtofs_object size");
_Static_assert(sizeof(struct femtofs_attr) == 16, "femtofs_attr size");
```

---

## Format Overview

The image is a single contiguous blob with three regions, in order:

1. Header (fixed, 128 bytes).
2. Metadata table (fixed cells, 16 bytes each).
3. Content region, split into:
   - public part: `[public_off, private_off)`
   - private part: `[private_off, image_size)`

All on-disk 32-bit byte offsets are relative to the start of the image,
including `public_off`, `private_off`, object `data_off` when used as a byte
offset, bucket filename offsets, `ext_off`, `acl_off`, and `xattr_off`.

Fields documented as indices (e.g., `cell_count`, `root_first`,
directory `data_off`, hardlink `data_off`, bucket `data_off`, bucket
`realsize`) are metadata-table indices, not byte offsets.

The metadata table serves a triple role:
- object metadata (`file`, `dir`, `symlink`, `fifo`, `hardlink`)
- directory hash buckets
- deduplicated attribute records

There is no separate hash-table region.
There is no separate attribute-table region; attribute records are cells in the
same 16-byte metadata table.

---

## Header (`femtofs_header`)

```c
struct femtofs_header {
    uint8_t  magic[4];       // "0FS\0"
    uint16_t version;        // format version
    uint16_t reserved;       // must be 0

    uint32_t cell_count;     // number of entries in metadata table
    uint32_t public_off;     // content-region start (public part, page-aligned)
    uint32_t private_off;    // split point: private-part start (page-aligned)
    uint32_t meta_size;      // metadata table size in bytes
    uint32_t meta_hash;      // FNV-1 32-bit hash over metadata table

    uint32_t root_first;     // root bucket slice start index in metadata table
    uint32_t root_size;      // root tablesize
    uint32_t root_real;      // root N (actual entry count)
    uint16_t root_attr;      // attribute cell index for root directory
    uint8_t  root_p;         // root hash control byte (mode + small-prime index)
    uint8_t  root_pad;       // must be 0
    uint32_t hash2_base;     // global second-hash base prime for dual-hash mode

    uint8_t  author[80];     // NUL-terminated, zero-padded
};                           // exactly 128 bytes
```

Notes:
- Header starts at byte 0.
- Metadata table starts at byte 128.
- Content region starts at `public_off`.
- Public part is `[public_off, private_off)`.
- Private part is `[private_off, image_size)`.
- `private_off >= public_off`.
- `public_off >= 128 + meta_size`.
- `private_off` is the only boundary needed to classify any content pointer:
  `off < private_off` is public, `off >= private_off` is private.
- Split-content layout changes the on-disk header and therefore requires a
  format `version` bump relative to the single-part layout.
- `meta_hash` is a non-cryptographic integrity/sanity check.
- Root descriptor is in header because root has no parent bucket entry.
- `root_p` uses the same hash-control encoding as directory `hash_p` fields.
- `hash2_base` is used only when at least one directory (including root) is in
  dual-hash mode; otherwise it may be `0`.
- Attribute cells may appear anywhere in the metadata table.

### Metadata Integrity Hash (`meta_hash`)

`meta_hash` is deterministic and algorithm-fixed:
- Hash function: FreeBSD kernel `fnv_32_buf()` from `<sys/fnv_hash.h>`
  (FNV-1, 32-bit state).
- Initial value: `FNV1_32_INIT`.
- Input bytes: exact metadata-table byte range
  `image[128 .. 128 + meta_size)`.
- Header bytes (including `meta_hash`) are not part of the hash input.
- No salt/nonce/secret key is used.

Builder rule:
- After finalizing all metadata-table bytes, compute:
  `meta_hash = fnv_32_buf(meta_bytes, meta_size, FNV1_32_INIT)`.

Reader rule:
- Before trusting metadata-table contents, recompute the same hash and require
  exact equality with header `meta_hash`; mismatch means corrupt/invalid image.

Offset model:
- Every on-disk 32-bit pointer to content bytes is image-relative.
- `private_off` is the sole public/private classifier for those pointers.
- Pointer validity for content bytes is:
  `public_off <= off < image_size`.
- Private-only records (`ext_off`, `acl_off`, `xattr_off`) must satisfy
  `off == 0 || off >= private_off`.

---

## Metadata Table Cells

```c
union femtofs_object_role {
    struct {
        uint32_t data_off;
        uint32_t size;
        uint32_t realsize;
    } raw;

    struct {
        uint32_t bucket_first;
        uint32_t bucket_count;
        uint32_t parent_n;
    } dir;

    struct {
        uint32_t image_off;
        uint32_t content_size;
        uint32_t reserved_flags;
    } filelike;

    struct {
        uint32_t target_index;
        uint32_t reserved0;
        uint32_t reserved1;
    } hardlink;

    struct {
        uint32_t object_index;
        uint32_t name_off;
        uint32_t next_index;
    } bucket;
};

struct femtofs_object {
    uint8_t  type;        // FEMTOFS_TYPE_* (kept first as on-disk discriminator)
    uint8_t  hash_p;      // directories: hash control byte; buckets: reserved (0)
    uint16_t attr_index;  // attribute cell index; ignored for buckets
    union femtofs_object_role role; // role-specific payload (12 bytes)
};                        // exactly 16 bytes

struct femtofs_attr {
    uint8_t  type;        // FEMTOFS_TYPE_ATTR
    uint8_t  reserved0;   // must be 0
    uint16_t mode;        // st_mode bits (type + perms + suid/sgid/sticky)
    uint32_t uid;         // numeric owner
    uint32_t gid;         // numeric group
    uint32_t ext_off;     // 0 or image-relative offset of femtofs_metaext
};                        // exactly 16 bytes
```

```c
#define FEMTOFS_TYPE_NULL      0
#define FEMTOFS_TYPE_FILE      1
#define FEMTOFS_TYPE_DIR       2
#define FEMTOFS_TYPE_SYMLINK   3
#define FEMTOFS_TYPE_HARDLINK  4
#define FEMTOFS_TYPE_FIFO      5
#define FEMTOFS_TYPE_AUX       0x80
#define FEMTOFS_TYPE_ATTR      (FEMTOFS_TYPE_AUX | 1)
```

Object cells and attribute cells share the same 16-byte table. Their role is
identified by the leading `type` byte. Bucket cells are represented using the
`femtofs_object` layout with bucket semantics described below.

For compact notation in the rest of this document, `data_off`, `size`, and
`realsize` refer to `role.raw.data_off`, `role.raw.size`, and
`role.raw.realsize`, respectively.

`type` intentionally remains at byte 0. Reordering all members by width would
move the discriminator, change the on-disk ABI, and invalidate the existing
bucket/type interpretation rules.

Auxiliary cell rule:
- Any cell whose `type` has `FEMTOFS_TYPE_AUX` set is not a directory bucket
  occupant and must be treated as empty by hash lookup and directory iteration.
- `FEMTOFS_TYPE_ATTR` is the only defined auxiliary cell type in this version.

### Field-Width Utilization Map (`femtofs_object`, v1)

This subsection makes explicit where on-disk fields carry values with a smaller
effective domain than their storage width. It is intended to show where future
extensions could add meaning.

Builder hygiene:
- Builders should write `0` in all currently unused bytes/bits.
- Readers should only require zero where this specification already marks fields
  as reserved and checks them in validation rules.

`femtofs_object` header fields:
- `type` (`uint8_t`): this version defines only
  `NULL`, `FILE`, `DIR`, `SYMLINK`, `HARDLINK`, `FIFO`, `ATTR`
  (plus `AUX` namespace bit). Other values are unassigned.
- `hash_p` (`uint8_t`):
  - `DIR`: hash-control byte:
    - bits `[5:0]` encode `p1_index` into `SMALL_PRIMES`
    - bits `[7:6]` encode hash mode (`00` single-hash, `01` dual-hash,
      `10/11` reserved)
  - Bucket cells: reserved and required `0`.
  - `FILE`/`SYMLINK`/`FIFO`/`HARDLINK`: currently unused by semantics.
- `root_p` in the header uses the same encoding as directory `hash_p`.
- `attr_index` (`uint16_t`):
  - `FILE`/`DIR`/`SYMLINK`/`FIFO`: attribute-cell index `< cell_count` where
    `cell_count < 2^16`.
  - `HARDLINK` and bucket cells: required `0`.

`union femtofs_object_role` payload fields (stored as three `uint32_t` words):
- Word 0 (`data_off` / `bucket_first` / `image_off` / `target_index` / `object_index`):
  - `FILE`/`SYMLINK` `image_off`: image-relative byte offset (`< 2^32`), full
    32-bit offset domain.
  - `DIR` `bucket_first`: metadata index (`< 2^16`), logically `u16` in `u32`.
  - `HARDLINK` `target_index`: metadata index (`< 2^16`), logically `u16` in
    `u32`.
  - Bucket `object_index`: metadata index (`< 2^16`), logically `u16` in `u32`.
  - `FIFO`: required `0`.
- Word 1 (`size` / `bucket_count` / `content_size` / `reserved0` / `name_off`):
  - `FILE`/`SYMLINK` `content_size`: announced size (`< 2^32`), full 32-bit
    length domain.
  - `DIR` `bucket_count`: `tablesize`, bounded by `<= 65521`, logically `u16`
    in `u32`.
  - Bucket `name_off`: image-relative byte offset (`< 2^32`), full 32-bit
    offset domain.
  - `HARDLINK` `reserved0`: required `0`.
  - `FIFO`: required `0`.
- Word 2 (`realsize` / `parent_n` / `reserved_flags` / `reserved1` / `next_index`):
  - `DIR` `parent_n`:
    - bits `[15:0]` parent index
    - bits `[30:16]` `N`
    - bit `[31]` reserved (`0`)
  - `FILE`/`SYMLINK` `reserved_flags`: required `0` in this version (entire
    word currently unused).
  - `HARDLINK` `reserved1`: required `0`.
  - Bucket `next_index`: chain next index or end sentinel `tablesize`,
    bounded by `<= 65521`, logically `u16` in `u32`.
  - `FIFO`: required `0`.

### Directory Hash-Control Encoding (`hash_p`, `root_p`)

Directory hash policy is encoded in one byte:
- low 6 bits: `p1_index` (index into `SMALL_PRIMES`)
- high 2 bits: hash mode

```c
#define FEMTOFS_HASH_P1IDX_MASK   0x3Fu
#define FEMTOFS_HASH_MODE_MASK    0xC0u
#define FEMTOFS_HASH_MODE_SHIFT   6u

#define FEMTOFS_HASH_MODE_SINGLE  0u
#define FEMTOFS_HASH_MODE_DUAL    1u
/* 2 and 3 are reserved */
```

Interpretation:
- `SINGLE` mode: one anchor hash (`h1`) using `p1 = SMALL_PRIMES[p1_index]`.
- `DUAL` mode: two anchor hashes (`h1`, `h2`):
  - `h1` uses `p1 = SMALL_PRIMES[p1_index]`
  - `h2` uses `hash2_base` from the image header.
- `SMALL_PRIMES` table content and order are defined in
  "Choosing Hash Parameters at Build Time".

`hash2_base` rules:
- If any directory (including root) is in `DUAL` mode, `hash2_base` must be a
  prime satisfying `2^8 < hash2_base < 2^24`.
- If no directory uses `DUAL` mode, `hash2_base` may be `0`.

Builder policy flexibility:
- Builder implementations MAY choose per-directory mode (`SINGLE` vs `DUAL`)
  and MAY classify "hard" directories using their own heuristic.
- Builder implementations MAY test multiple candidate `hash2_base` primes in
  the allowed range and select the best candidate.
- Builders SHOULD choose parameters to balance lookup performance (worst-case
  chain length and average comparisons) against metadata size (bucket count).
- This specification does not mandate one optimizer algorithm.

### Directory Metadata Packing (`realsize`)

For `FEMTOFS_TYPE_DIR`, `realsize` packs both parent pointer and `N`:

- bits `[15:0]`: `parent_index` (`uint16_t`)
- bits `[30:16]`: `N` (actual entry count), so `N < 2^15`
- bit `[31]`: reserved, must be `0`

`parent_index = 0xFFFF` is reserved as `FEMTOFS_PARENT_ROOT` (the parent is the
root descriptor in the header). This avoids cross-filesystem `..` links in on-disk
metadata.

```c
#define FEMTOFS_PARENT_ROOT      0xFFFFu
#define FEMTOFS_DIR_PARENT_MASK  0x0000FFFFu
#define FEMTOFS_DIR_N_MASK       0x7FFF0000u
#define FEMTOFS_DIR_N_SHIFT      16

#define FEMTOFS_DIR_GET_N(x) \
    ((uint16_t)(((x) & FEMTOFS_DIR_N_MASK) >> FEMTOFS_DIR_N_SHIFT))
#define FEMTOFS_DIR_GET_PARENT(x) \
    ((uint16_t)((x) & FEMTOFS_DIR_PARENT_MASK))
#define FEMTOFS_DIR_PACK(parent, n) \
    (((uint32_t)(parent) & FEMTOFS_DIR_PARENT_MASK) | \
     ((((uint32_t)(n)) & 0x7FFFu) << FEMTOFS_DIR_N_SHIFT))

#define FEMTOFS_PART_PRIVATE       0u
#define FEMTOFS_PART_PUBLIC        1u
#define FEMTOFS_OFF_IS_PUBLIC(off, private_off) \
    ((uint8_t)(((uint32_t)(off) < (uint32_t)(private_off)) ? \
               FEMTOFS_PART_PUBLIC : FEMTOFS_PART_PRIVATE))
#define FEMTOFS_OFF_IS_PRIVATE(off, private_off) \
    ((uint8_t)(((uint32_t)(off) >= (uint32_t)(private_off)) ? 1u : 0u))
```

### Entry Roles

Metadata rules:
- Every non-root non-bucket object references a deduplicated attribute record
  via `attr_index`.
- `femtofs_attr.mode` stores the exported `st_mode` bits, including permission
  bits, setuid, setgid, sticky, and file type bits.
- `type` and the file-type bits in the resolved `mode` must agree.
- `femtofs_attr.uid` and `femtofs_attr.gid` store numeric FreeBSD owner/group IDs
  directly.
- `femtofs_attr.ext_off == 0` means there is no extended metadata.
- `femtofs_attr.ext_off != 0` points to a structured metadata-extension record
  in the private part of the content region (see `femtofs_metaext` below).

`FEMTOFS_TYPE_DIR`:
- `hash_p`: hash-control byte (`p1_index` + mode, see encoding section)
- `attr_index`: attribute cell index for this directory
- `data_off`: first bucket index in metadata table
- `size`: `tablesize` (bucket count)
- `realsize`: packed `{ parent_index, N }`

`FEMTOFS_TYPE_FILE` and `FEMTOFS_TYPE_SYMLINK`:
- `attr_index`: attribute cell index for this object
- `data_off`: image-relative byte offset into content region
- `size`: announced byte length (original object size; excludes stored
  terminator and alignment padding)
- `realsize`: reserved for future filelike flags, must be `0` in this version
- part classification is derived from header boundary:
  - `data_off < private_off` => public part
  - `data_off >= private_off` => private part

`FEMTOFS_TYPE_FIFO`:
- `attr_index`: attribute cell index for this object
- `data_off`: `0` (no payload)
- `size`: `0`
- `realsize`: `0`

`FEMTOFS_TYPE_HARDLINK`:
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
- No hardlinks to directories, symlinks, or FIFOs.

`FEMTOFS_TYPE_ATTR`:
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

Bucket entries are 16-byte cells using the `femtofs_object` layout in a
directory's bucket slice.

Occupied bucket:
- `type`: referenced object type (non-null)
- `hash_p`: reserved in bucket role, must be `0`
- `attr_index`: `0` (unused in bucket role)
- `data_off`: referenced object index
- `size`: image-relative filename offset (NUL-terminated string)
- `realsize`: next bucket index in chain, or `tablesize` as end sentinel

Empty bucket:
- `type = FEMTOFS_TYPE_NULL`
- `realsize = tablesize`

Bucket interpretation rules:
- In directory hash lookup, a cell is treated as empty if `type` is
  `FEMTOFS_TYPE_NULL` or if `type & FEMTOFS_TYPE_AUX` is nonzero.
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
- `root_attr` must be `< cell_count` and refer to a `FEMTOFS_TYPE_ATTR` cell.
- For `FEMTOFS_TYPE_FILE`, `FEMTOFS_TYPE_DIR`, `FEMTOFS_TYPE_SYMLINK`, and
  `FEMTOFS_TYPE_FIFO`,
  `attr_index` must be `< cell_count` and refer to a `FEMTOFS_TYPE_ATTR` cell.
- Attribute records may be shared by multiple objects.

### Metadata Extension Hook (`femtofs_metaext`)

`femtofs_attr.ext_off` reserves an optional extension pointer for richer
metadata without affecting the common case.

```c
struct femtofs_metaext {
    uint16_t version;      // extension record version
    uint16_t kind_flags;   // FEMTOFS_METAEXT_* bits
    uint32_t acl_off;      // 0 or image-relative offset of ACL payload
    uint32_t xattr_off;    // 0 or image-relative offset of xattr payload
    uint32_t payload_size; // total bytes owned by this extension record
};                         // exactly 16 bytes
```

```c
#define FEMTOFS_METAEXT_ACL    0x0001u
#define FEMTOFS_METAEXT_XATTR  0x0002u
```

Rules:
- `ext_off == 0` means no ACLs and no extended attributes.
- `ext_off != 0` points to `femtofs_metaext` stored in the private part
  (`ext_off >= private_off`).
- ACL and xattr payload encoding is versioned by `femtofs_metaext.version`.
- If `acl_off != 0` or `xattr_off != 0`, those offsets must also satisfy
  `off >= private_off`.
- Readers that do not support a referenced extension record must reject mount
  or object activation as unsupported.

---

## Directory Lookup and `.` / `..`

### Hash Function

```c
uint32_t femtofs_hash(const char *name, uint8_t p, uint32_t tablesize) {
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
    parent = FEMTOFS_DIR_GET_PARENT(dir->realsize);
    if (parent == FEMTOFS_PARENT_ROOT)
        return root_directory;
    return &cell_table[parent];
}

buckets = &cell_table[dir->data_off];
tablesize = dir->size;

if (tablesize == 0)
    return ENOENT;

hash_ctrl = is_root_directory ? header->root_p : dir->hash_p;
mode = (hash_ctrl & FEMTOFS_HASH_MODE_MASK) >> FEMTOFS_HASH_MODE_SHIFT;
p1_index = hash_ctrl & FEMTOFS_HASH_P1IDX_MASK;
p1 = SMALL_PRIMES[p1_index];

anchors[0] = femtofs_hash(query, p1, tablesize);
anchor_count = 1;
if (mode == FEMTOFS_HASH_MODE_DUAL) {
    h2 = femtofs_hash(query, header->hash2_base, tablesize);
    if (h2 != anchors[0])
        anchors[anchor_count++] = h2;
}

for (a = 0; a < anchor_count; ++a) {
    idx = anchors[a];
    if (buckets[idx].type == FEMTOFS_TYPE_NULL || (buckets[idx].type & FEMTOFS_TYPE_AUX))
        continue;

    for (;;) {
        name = image_base + buckets[idx].size;
        if (strcmp(name, query) == 0)
            return &cell_table[buckets[idx].data_off];
        if (buckets[idx].realsize == tablesize)
            break;
        idx = buckets[idx].realsize;
    }
}

return ENOENT;
```

### Directory Listing

`readdir` synthesizes:
- `.`
- `..`

Then it scans occupied buckets (`type != FEMTOFS_TYPE_NULL`) in the directory
slice. Listing order for hash buckets is undefined.

---

## Choosing Hash Parameters at Build Time

Goal: minimize directory lookup cost while keeping metadata table growth small.

Reference objective for single-hash mode:
- minimize `sum_of_squares(chain_lengths) / N`
- subject to bounded `tablesize`.

Definitions:

```c
MAX_TABLESIZE_PRIME = 65521   // largest prime < 2^16
SMALL_PRIMES = [
    3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47,
    53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107,
    109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167,
    173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229,
    233, 239, 241, 251
]                       // fixed on-disk index map for `p1_index`
SMALL_PRIMES_COUNT = 54
```

### Single-Hash Reference Policy

The following algorithm is informative (reference policy), not mandatory.
Builder implementations MAY use different tuning logic, provided encoded
outputs satisfy this specification's validation rules.

Reference algorithm:

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
    for each p in SMALL_PRIMES:
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
- For `N = 32767`, `next_prime(2*N) = 65537` is out of range; algorithm
  clamps to `65521` and returns best result seen.
- Phase 1 uses `tablesize = N` even if `N` is not prime.
- Phase 2 grows through primes only.

### Mixed Single/Dual Policy (Recommended, Not Mandated)

Builder implementations MAY use a mixed policy:
- keep `SINGLE` mode for directories that already meet target lookup quality
- switch selected "hard" directories to `DUAL` mode.

For `DUAL` mode, builder implementations MAY:
- pick `hash2_base` as any prime in `2^8 < p < 2^24`
- test multiple candidate primes (for example random samples) and keep the best.

Builders SHOULD tune these choices to balance:
- lookup quality (worst-case chain length and average comparisons)
- metadata growth (extra buckets / bytes).

This specification intentionally does not mandate one optimizer algorithm.

---

## Content Region (Public/Private Split)

The image has one content region split into two contiguous parts:
- Public part: `[public_off, private_off)`, bytes that are safe to expose in
  `dirty`/`leaking` neighbor spill (publicly readable and publicly reachable
  regular-file data).
- Private part: `[private_off, image_size)`, all other bytes.

Content-pointer visibility is determined only by comparison with `private_off`.

Exported path model:
- An exported path is an absolute path from the image root descriptor to one
  directory-entry occurrence, following bucket references.
- If multiple hardlink paths reach the same canonical file object, each path is
  an occurrence for visibility classification.

Visibility classification:
- A regular-file object is public-eligible only if it has world-read
  (`S_IROTH`) and every directory on at least one exported path has world-
  search (`S_IXOTH`).
- A directory-entry name occurrence is public-visible only if at least one
  exported path to its containing directory is world-readable and world-
  searchable (`S_IROTH|S_IXOTH`) on each directory along that path.

Shared blob deduplication domain:
- Regular-file payload bytes and filename bytes are blobs in one shared dedup
  class domain.
- Blob-class key is exact byte identity of announced bytes.
- Part assignment is by class promotion: if any occurrence in a blob class is
  public (public-eligible payload reference or public-visible filename
  occurrence), that blob class is stored in the public part; otherwise it is
  stored in the private part.
- As a result, non-public file objects and private-tree entries may reference
  public-part blobs whenever the referenced bytes are shared with at least one
  public occurrence.

By policy, symlink targets and metadata-extension payloads are always stored in
the private part and are outside this shared payload/filename blob domain.

Interpretation is determined by references, not embedded tags.

### Blob Encoding And Word Alignment

All payload and filename blobs stored in content parts use one canonical
encoded form:
1. Write announced bytes.
2. Append one `0x00` terminator byte.
3. Append extra `0x00` bytes until next blob offset is aligned to a 4-byte word.

Definitions:
- `FEMTOFS_BLOB_WORD = 4`
- `stored_blob_bytes = align_up(announced_bytes + 1, FEMTOFS_BLOB_WORD)`

Consequences:
- Every stored blob is NUL-terminated in its content part.
- Every next blob begins at a 4-byte aligned offset.
- If `announced_bytes % 4 == 0`, the suffix is one full zero word.
- Otherwise the suffix is `1..3` zero bytes that simultaneously terminate and
  align.
- Object-reported size remains the original announced size (`size` for file
  objects); suffix bytes are never part of object-visible length.

### Packing Algorithm (Per Content Part)

Public and private parts are packed independently with the same algorithm.
All contents are sorted descending by size.
A sorted free-hole set (by hole size, then offset) tracks page-padding gaps.

For each content:

1. Try smallest fitting hole.
2. If no hole fits, append at tail.
3. If append would straddle page, advance tail to next page and record gap.

Packing unit:
- `content` size used by this algorithm is encoded blob size
  `align_up(announced_bytes + 1, 4)`, not announced size.

Consequences (per part):
- Contents `>= PAGE_SIZE` become page-aligned by construction.
- Holes are always `< PAGE_SIZE`.
- Sub-page contents stay page-contained.

### Deduplication

Deduplication may cross object kinds by promotion:
- One shared blob class domain is used for regular-file payload bytes and
  filename bytes.
- For any shared blob class, store exactly one copy in exactly one content part:
  the public part if the class has at least one public occurrence, otherwise
  the private part.
- Cross-part duplication of a shared payload/filename blob class is not
  allowed.
- Stored blob bytes are derived deterministically from announced bytes by the
  canonical terminator/alignment rule above.

---

## `mmap(2)` Contract by Mount Mode

Common constraints:
- Filesystem is read-only; writable mappings are rejected.
- Only regular files are mappable.

Public/private eligibility is derived from the `private_off` boundary:
- A file payload is public when `data_off < private_off`.
- A file payload is private when `data_off >= private_off`.
- Public payloads may use direct mapping with neighbor spill according to rules
  below.
- Private payloads must use clean behavior (no private-byte spill), even when
  mount mode is `dirty` or `leaking`.
- The `dirty` unaligned optimization applies only to public-eligible file
  objects with `size < PAGE_SIZE`, even if additional non-public objects
  reference the same promoted public bytes.

`clean` mode (default):
- `offset` must be page-aligned, otherwise `EINVAL`.
- File size `< PAGE_SIZE`: map from one cached anonymous page containing file
  bytes plus zero-fill remainder.
- File size `>= PAGE_SIZE` and not page-multiple: map full leading pages
  directly from image, map final partial page from one cached anonymous page
  (tail bytes plus zero-fill).
- File size page-multiple: fully direct-mapped from image.

`leaking` mode:
- `offset` must be page-aligned, otherwise `EINVAL`.
- Public-part files can skip tail-page copy and map directly even when the
  final page is partial.
- Any bytes outside file bounds that become visible must originate from the
  public part.

`dirty` mode:
- For private payload files and public payload files with `size >= PAGE_SIZE`,
  `offset` must be page-aligned (same as `leaking`).
- For public-part files with `size < PAGE_SIZE`, non-page-aligned `offset` may
  be accepted and direct shifted mapping may be used instead of copy.
- Any bytes outside file bounds that become visible must originate from the
  public part.

Operational summary:
- Clean behavior copies at most one page per mapped file object (its final
  page), populated lazily and cacheable.
- Leaking mode can avoid that copy for public-part files.
- Dirty mode extends that by also allowing shifted zero-copy for
  `public && size < PAGE_SIZE`.
- Private-part files never leak neighboring bytes in any mount mode.

Anonymous-page cache key for clean behavior:
- Key by stable source location of copied bytes, i.e. image-relative offset of
  the first copied byte (`src_first_byte_off`).
- Public/private classification is derivable from `private_off`, so separate
  part-selector bits are not required in the key.
- This naturally shares cached pages across hardlinks and deduplicated file
  contents whenever copied source region is identical.
- Implementation may also record `copied_len` as a consistency check.

---

## Alignment and Padding

Header and metadata table:
- `femtofs_header` is exactly 128 bytes.
- `femtofs_object` is exactly 16 bytes.
- `femtofs_attr` is exactly 16 bytes.
- No inter-record padding.

Content region split:
- `public_off` and `private_off` must be page-aligned.
- `private_off` marks the start of the private part and therefore the end of
  the public part.
- Contents `>= PAGE_SIZE` are page-aligned by packing algorithm.
- Contents `< PAGE_SIZE` must remain page-contained.

---

## Normative Validation Rules (Builder/Reader/Kernel)

This section defines mandatory checks for deterministic interoperability and
safe mounting.

Header and top-level bounds:
- `magic` must be exactly `{'0','F','S','\0'}`.
- `version` must be supported by the reader; unsupported versions must be
  rejected.
- `reserved == 0` and `root_pad == 0`.
- `meta_size == cell_count * 16`.
- `cell_count > 0`.
- `128 + meta_size <= public_off <= private_off <= image_size`.
- `public_off` and `private_off` must be page-aligned.
- `root_attr < cell_count` and `cell[root_attr].type == FEMTOFS_TYPE_ATTR`.
- `root_size < 2^15`, `root_real <= root_size`.
- If `root_size == 0`, then `root_real == 0`.
- If `root_size > 0`, then `root_first + root_size <= cell_count`.
- Root hash control (`root_p`) must decode to:
  - mode in `{FEMTOFS_HASH_MODE_SINGLE, FEMTOFS_HASH_MODE_DUAL}`
  - `p1_index < SMALL_PRIMES_COUNT`.
- `meta_hash` must exactly match
  `fnv_32_buf(image + 128, meta_size, FNV1_32_INIT)`.

Cell typing and reserved-bit rules:
- Valid `type` values in this version are:
  `NULL`, `FILE`, `DIR`, `SYMLINK`, `HARDLINK`, `FIFO`, `ATTR`.
- Unknown non-aux types and unknown aux types must be rejected.
- For `FILE`, `DIR`, `SYMLINK`, and `FIFO` cells:
  `attr_index < cell_count` and `cell[attr_index].type == FEMTOFS_TYPE_ATTR`.
- For each `DIR` cell, `hash_p` must decode to:
  - mode in `{FEMTOFS_HASH_MODE_SINGLE, FEMTOFS_HASH_MODE_DUAL}`
  - `p1_index < SMALL_PRIMES_COUNT`.
- For `FEMTOFS_TYPE_DIR`, `realsize` bit `[31]` must be `0`.
- For `FEMTOFS_TYPE_DIR`, parent from `FEMTOFS_DIR_GET_PARENT(realsize)` must
  be either `FEMTOFS_PARENT_ROOT` or an in-range directory-object index.
- For `FEMTOFS_TYPE_FILE` and `FEMTOFS_TYPE_SYMLINK`, `realsize == 0`.
- For `FEMTOFS_TYPE_FIFO`, `data_off == 0`, `size == 0`, `realsize == 0`.
- For `FEMTOFS_TYPE_HARDLINK`: `attr_index == 0`, `size == 0`,
  `realsize == 0`.
- For `FEMTOFS_TYPE_ATTR`, `reserved0 == 0`.
- For bucket-role occupied cells: `hash_p == 0` and `attr_index == 0`.
- If any directory (including root) uses `FEMTOFS_HASH_MODE_DUAL`, then
  `hash2_base` must be prime and satisfy `2^8 < hash2_base < 2^24`.

Directory-slice and bucket bounds:
- For each non-root `DIR` object:
  `tablesize = size`, `bucket_first = data_off`.
- For every directory descriptor (including root descriptor), if
  `tablesize > 0`, then `bucket_first + tablesize <= cell_count`.
- For every directory descriptor, packed `N` must satisfy `N <= tablesize`.
- If `tablesize == 0`, then `N == 0`.
- For every bucket in a directory slice:
  - `realsize == tablesize` (end sentinel) or `realsize < tablesize`.
  - if occupied (`type != NULL` and non-aux), `data_off < cell_count`.
  - if occupied, target `cell[data_off]` must be a non-aux, non-null
    filesystem object cell.
- Chain traversal from hash anchor must terminate in at most `tablesize` steps
  (no cycles/out-of-slice jumps).
- For each directory slice, `N` must equal the number of occupied buckets.
- Duplicate names within one directory are forbidden; images containing
  duplicates must be rejected.

Filename and path-component rules:
- Bucket `size` (filename offset) must satisfy `public_off <= size < image_size`.
- Filename must be NUL-terminated before `image_size`.
- Filename byte length must be `1..255` (excluding terminator).
- Filename bytes must not contain `'/'` or `'\0'` before terminator.
- On-disk directory entries named `"."` or `".."` are forbidden.

File/symlink payload bounds:
- For `FILE`/`SYMLINK`, `public_off <= data_off < image_size`.
- Let `stored_blob_bytes = align_up(size + 1, 4)`, computed in 64-bit
  arithmetic and required to fit in `uint32_t` without overflow.
- If `data_off < private_off` (public part), require
  `data_off + stored_blob_bytes <= private_off`.
- If `data_off >= private_off` (private part), require
  `data_off + stored_blob_bytes <= image_size`.
- `SYMLINK` payloads are private-only by policy: `data_off >= private_off`.

Hardlink rules:
- `data_off` (target index) must be `< cell_count`.
- Target must be `FEMTOFS_TYPE_FILE` (never `DIR`, `SYMLINK`, `FIFO`,
  `HARDLINK`, `ATTR`, or `NULL`).

Metadata-extension bounds:
- `ext_off == 0` means no extension record.
- If `ext_off != 0`, then `ext_off >= private_off` and
  `ext_off + sizeof(struct femtofs_metaext) <= image_size`.
- If `acl_off != 0` or `xattr_off != 0`, each such offset must satisfy
  `private_off <= off < image_size`.

Builder source-tree mapping policy:
- Supported source inode kinds are regular file, directory, symlink, and FIFO.
- Additional directory entries that reference the same regular-file inode are
  encoded as `FEMTOFS_TYPE_HARDLINK`.
- Hardlinks to non-regular files are not supported.
- If multiple source paths reference the same FIFO inode, image build must fail
  explicitly.
- Unsupported source inode kinds (device nodes, socket, whiteout, etc.) must
  make image build fail explicitly.

Reader behavior on violation:
- Any violation above is a hard format error; mount/open/regeneration must
  reject the image rather than applying repair heuristics.
