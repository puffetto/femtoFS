/*
 * fsck_femtofs.c
 *
 * Offline structural/consistency checker for femtoFS images, following
 * "femtoFS: zero-fsck-giving, zero-frills, read-only, near-zero-copy
 * filesystem image format" (specification.md).
 *
 * Aligned against the revision that made object discovery, hash-anchor
 * chain validation, content-interval disjointness, and orphan detection
 * fully normative (previously several of these were this checker's own
 * best-effort interpretation, marked "[implied]"; they are now spelled
 * out directly in "Normative Validation Rules" and this file follows
 * that wording rather than its own prior judgment calls).
 *
 * All multi-byte fields are read as little-endian regardless of host
 * byte order, per "Never use native-width types ... in on-disk records"
 * and the explicit little-endian requirement in "On-disk ABI Rules".
 *
 * Exit status: 0 if no ERRORs were found, 1 if any ERROR was found,
 * 2 for usage/I/O failure before any checking could begin.
 *
 * Build: cc -O2 -Wall -Wextra -std=c11 -o fsck_femtofs fsck_femtofs.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Constants from the specification                                    */
/* ------------------------------------------------------------------ */

#define FEMTOFS_HEADER_SIZE     128u
#define FEMTOFS_CELL_SIZE       16u
#define FEMTOFS_BLOB_WORD       4u
#define FEMTOFS_PAGE_SIZE       4096u   /* format[1] == 4 => 2^(4+8) */

#define FEMTOFS_TYPE_NULL       0x00u
#define FEMTOFS_TYPE_FILE       0x01u
#define FEMTOFS_TYPE_DIR        0x02u
#define FEMTOFS_TYPE_SYMLINK    0x03u
#define FEMTOFS_TYPE_HARDLINK   0x04u
#define FEMTOFS_TYPE_FIFO       0x05u
#define FEMTOFS_TYPE_AUX        0x80u
#define FEMTOFS_TYPE_ATTR       (FEMTOFS_TYPE_AUX | 1u)   /* 0x81 */

#define FEMTOFS_HASH_P1IDX_MASK  0x3Fu
#define FEMTOFS_HASH_MODE_MASK   0xC0u
#define FEMTOFS_HASH_MODE_SHIFT  6u
#define FEMTOFS_HASH_MODE_SINGLE 0u
#define FEMTOFS_HASH_MODE_DUAL   1u

#define FEMTOFS_PARENT_ROOT       0xFFFFu
#define FEMTOFS_DIR_PARENT_MASK   0x0000FFFFu
#define FEMTOFS_DIR_N_MASK        0x7FFF0000u
#define FEMTOFS_DIR_N_SHIFT       16u
#define FEMTOFS_DIR_RESERVED_BIT  0x80000000u

#define MAX_TABLESIZE_PRIME      65521u   /* largest prime < 2^16 */
#define MAX_OBJECT_COUNT         32768u   /* non-root visible objects must be < 2^15 */
#define MAX_CELL_COUNT           65536u   /* cell_count must be < 2^16 */

static const uint32_t SMALL_PRIMES[] = {
    3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47,
    53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107,
    109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167,
    173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229,
    233, 239, 241, 251
};
#define SMALL_PRIMES_COUNT 53u

#define FNV1_32_INIT   0x811c9dc5u
#define FNV_32_PRIME   0x01000193u

/* S_IFMT-family bits packed into femtofs_attr.mode (standard POSIX values) */
#define S_IFMT_BITS   0xF000u
#define S_IFIFO_BITS  0x1000u
#define S_IFDIR_BITS  0x4000u
#define S_IFREG_BITS  0x8000u
#define S_IFLNK_BITS  0xA000u

/* header.author must equal this exact 56-byte, NUL-padded identifier
 * (49 ASCII bytes + NUL + 6 zero bytes; the string literal initializer
 * zero-fills the remainder of the array automatically). */
static const uint8_t AUTHOR_FIELD[56] =
    "Andrea \"Nemesi\" Cocito - blackye at gmail dot com";

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static long g_error_count = 0;
static bool g_verbose = false;

#define ERR(...) do { \
        g_error_count++; \
        fprintf(stderr, "ERROR: " __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } while (0)

#define INFO(...) do { \
        if (g_verbose) { \
            fprintf(stderr, "INFO:  " __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

/* ------------------------------------------------------------------ */
/* Little-endian accessors, bounds-checked against the backing file    */
/* ------------------------------------------------------------------ */

static const uint8_t *g_image;
static uint64_t g_image_len; /* actual backing file size */

static bool in_bounds(uint64_t off, uint64_t n) {
    if (off > g_image_len) return false;
    if (n > g_image_len - off) return false;
    return true;
}

static uint8_t rd_u8(uint64_t off) { return g_image[off]; }

static uint16_t rd_u16(uint64_t off) {
    return (uint16_t)(g_image[off] | ((uint16_t)g_image[off + 1] << 8));
}

static uint32_t rd_u32(uint64_t off) {
    return (uint32_t)g_image[off]
         | ((uint32_t)g_image[off + 1] << 8)
         | ((uint32_t)g_image[off + 2] << 16)
         | ((uint32_t)g_image[off + 3] << 24);
}

/* ------------------------------------------------------------------ */
/* Header                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  magic[4];
    uint8_t  format[2];
    uint16_t version;
    uint32_t image_hash;
    uint32_t meta_hash;
    uint32_t uuid[4];
    uint32_t image_size;
    uint32_t cell_count;
    uint32_t public_off;
    uint32_t private_off;
    uint32_t meta_size;
    uint32_t root_first;
    uint32_t root_size;
    uint32_t root_real;
    uint16_t root_attr;
    uint8_t  root_p;
    uint8_t  root_pad;
    uint32_t hash2_base;
} header_t;

static void parse_header(header_t *h) {
    memcpy(h->magic, g_image + 0, 4);
    memcpy(h->format, g_image + 4, 2);
    h->version     = rd_u16(6);
    h->image_hash  = rd_u32(8);
    h->meta_hash   = rd_u32(12);
    for (int i = 0; i < 4; i++) h->uuid[i] = rd_u32(16 + i * 4);
    h->image_size  = rd_u32(32);
    h->cell_count  = rd_u32(36);
    h->public_off  = rd_u32(40);
    h->private_off = rd_u32(44);
    h->meta_size   = rd_u32(48);
    h->root_first  = rd_u32(52);
    h->root_size   = rd_u32(56);
    h->root_real   = rd_u32(60);
    h->root_attr   = rd_u16(64);
    h->root_p      = rd_u8(66);
    h->root_pad    = rd_u8(67);
    h->hash2_base  = rd_u32(68);
}

/* ------------------------------------------------------------------ */
/* Raw cell (femtofs_object / femtofs_attr share this 16-byte shape)   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  type;
    uint8_t  b1;      /* hash_p (object) / reserved0 (attr) */
    uint16_t f2;      /* attr_index (object) / mode (attr) */
    uint32_t w0;
    uint32_t w1;
    uint32_t w2;
} cell_t;

static cell_t read_cell(uint32_t idx) {
    cell_t c;
    uint64_t off = FEMTOFS_HEADER_SIZE + (uint64_t)idx * FEMTOFS_CELL_SIZE;
    c.type = rd_u8(off + 0);
    c.b1   = rd_u8(off + 1);
    c.f2   = rd_u16(off + 2);
    c.w0   = rd_u32(off + 4);
    c.w1   = rd_u32(off + 8);
    c.w2   = rd_u32(off + 12);
    return c;
}

static bool is_aux_type(uint8_t t) { return (t & FEMTOFS_TYPE_AUX) != 0; }

static const char *type_name(uint8_t t) {
    switch (t) {
        case FEMTOFS_TYPE_NULL:     return "NULL";
        case FEMTOFS_TYPE_FILE:     return "FILE";
        case FEMTOFS_TYPE_DIR:      return "DIR";
        case FEMTOFS_TYPE_SYMLINK:  return "SYMLINK";
        case FEMTOFS_TYPE_HARDLINK: return "HARDLINK";
        case FEMTOFS_TYPE_FIFO:     return "FIFO";
        case FEMTOFS_TYPE_ATTR:     return "ATTR";
        default:                    return "UNKNOWN";
    }
}

static bool type_is_known(uint8_t t) {
    switch (t) {
        case FEMTOFS_TYPE_NULL:
        case FEMTOFS_TYPE_FILE:
        case FEMTOFS_TYPE_DIR:
        case FEMTOFS_TYPE_SYMLINK:
        case FEMTOFS_TYPE_HARDLINK:
        case FEMTOFS_TYPE_FIFO:
        case FEMTOFS_TYPE_ATTR:
            return true;
        default:
            return false;
    }
}

/* ------------------------------------------------------------------ */
/* Hash-control byte decoding                                          */
/* ------------------------------------------------------------------ */

static bool decode_hash_ctrl(uint8_t byte, uint32_t *mode_out, uint32_t *p1_index_out) {
    uint32_t mode = (byte & FEMTOFS_HASH_MODE_MASK) >> FEMTOFS_HASH_MODE_SHIFT;
    uint32_t p1_index = byte & FEMTOFS_HASH_P1IDX_MASK;
    *mode_out = mode;
    *p1_index_out = p1_index;
    if (mode != FEMTOFS_HASH_MODE_SINGLE && mode != FEMTOFS_HASH_MODE_DUAL)
        return false;
    if (p1_index >= SMALL_PRIMES_COUNT)
        return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* FNV-1 (not FNV-1a), 32-bit, matching FreeBSD's fnv_32_buf()          */
/* ------------------------------------------------------------------ */

static uint32_t fnv1_32_buf(const uint8_t *buf, uint64_t len, uint32_t hval) {
    for (uint64_t i = 0; i < len; i++) {
        hval *= FNV_32_PRIME;
        hval ^= buf[i];
    }
    return hval;
}

static bool is_prime_u32(uint32_t n) {
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    for (uint32_t d = 3; (uint64_t)d * d <= n; d += 2)
        if (n % d == 0) return false;
    return true;
}

/* femtofs_hash(), matching "Directory Lookup and . / .." byte-for-byte.
 * The spec now correctly types `p` as uint32_t (an earlier revision had
 * it as uint8_t while calling the function with hash2_base, which can
 * exceed 2^8 -- that mismatch has since been fixed upstream). */
static uint32_t femtofs_hash(const uint8_t *name, uint32_t namelen, uint32_t p, uint32_t tablesize) {
    if (tablesize == 0 || tablesize == 1) return 0;
    uint32_t h = 0;
    for (uint32_t i = 0; i < namelen; i++) h = h * p + name[i];
    return h % tablesize;
}

/* Walks the chain rooted at slice-relative index `anchor_rel`, exactly as
 * the reference lookup algorithm would: if the anchor cell itself is
 * empty/auxiliary this anchor contributes nothing (not an error). If
 * occupied, follow `realsize` links to the end sentinel -- continuing
 * even after `target_rel` is found, per "must traverse through the end
 * sentinel even after finding the bucket being checked" -- flagging
 * `bad` on a cycle, an out-of-slice/over-tablesize jump, or a link into
 * a NULL/auxiliary cell. */
typedef struct { bool found; bool bad; } chain_walk_t;

static chain_walk_t walk_anchor_chain(uint32_t bucket_first, uint32_t tablesize,
                                       uint32_t anchor_rel, uint32_t target_rel) {
    chain_walk_t r = { false, false };
    cell_t c0 = read_cell(bucket_first + anchor_rel);
    if (c0.type == FEMTOFS_TYPE_NULL || is_aux_type(c0.type)) return r;

    bool *visited = calloc(tablesize, 1);
    if (!visited) { fprintf(stderr, "out of memory\n"); exit(2); }
    uint32_t rel = anchor_rel;
    for (uint32_t steps = 0; ; steps++) {
        if (visited[rel]) { r.bad = true; break; }
        visited[rel] = true;
        if (rel == target_rel) r.found = true;
        cell_t cc = read_cell(bucket_first + rel);
        if (cc.type == FEMTOFS_TYPE_NULL || is_aux_type(cc.type)) { r.bad = true; break; }
        if (cc.w2 == tablesize) break; /* sentinel: clean end of chain */
        if (cc.w2 > tablesize) { r.bad = true; break; }
        if (steps >= tablesize) { r.bad = true; break; }
        rel = cc.w2;
    }
    free(visited);
    return r;
}

/* ------------------------------------------------------------------ */
/* Coverage bitmap for the "every byte outside a stored interval is    */
/* 0x00" canonical-byte check                                          */
/* ------------------------------------------------------------------ */

static uint8_t *g_covered;   /* one byte per image byte; nonzero = accounted for */

static void mark_covered(uint64_t off, uint64_t len) {
    if (!in_bounds(off, len)) return; /* already reported elsewhere */
    for (uint64_t i = 0; i < len; i++) g_covered[off + i] = 1;
}

/* ------------------------------------------------------------------ */
/* Stored-interval overlap tracking ("distinct stored intervals must   */
/* not overlap"; exact duplicates in the shared domain are legitimate  */
/* dedup, a symlink target must never alias anything)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t start;
    uint32_t len;
    bool     is_symlink;
} interval_t;

static interval_t *g_intervals;
static uint32_t g_interval_count, g_interval_cap;

static void record_interval(uint32_t start, uint32_t len, bool is_symlink) {
    if (g_interval_count == g_interval_cap) {
        g_interval_cap = g_interval_cap ? g_interval_cap * 2 : 256;
        g_intervals = realloc(g_intervals, g_interval_cap * sizeof(*g_intervals));
        if (!g_intervals) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    g_intervals[g_interval_count].start = start;
    g_intervals[g_interval_count].len = len;
    g_intervals[g_interval_count].is_symlink = is_symlink;
    g_interval_count++;
}

static int interval_cmp(const void *pa, const void *pb) {
    const interval_t *a = pa, *b = pb;
    if (a->start != b->start) return (a->start < b->start) ? -1 : 1;
    if (a->len != b->len) return (a->len < b->len) ? -1 : 1;
    return (int)a->is_symlink - (int)b->is_symlink;
}

/* Proper sweep (tracks the running max end across ALL prior intervals,
 * not just the immediately preceding one, so a nested overlap is still
 * caught even when an exact-duplicate run sits between the two ends). */
static void check_interval_overlaps(void) {
    if (g_interval_count == 0) return;
    qsort(g_intervals, g_interval_count, sizeof(interval_t), interval_cmp);
    uint64_t max_end = 0;
    bool have_prev = false;
    interval_t prev = {0, 0, false};
    for (uint32_t i = 0; i < g_interval_count; i++) {
        interval_t cur = g_intervals[i];
        if (have_prev && cur.start == prev.start && cur.len == prev.len &&
            !cur.is_symlink && !prev.is_symlink) {
            prev = cur;
            continue; /* legitimate shared-domain dedup */
        }
        if ((uint64_t)cur.start < max_end) {
            ERR("content intervals conflict: [%u,+%u)%s overlaps a previously-claimed "
                "range ending at %" PRIu64 "%s",
                cur.start, cur.len, cur.is_symlink ? " (symlink target)" : "", max_end,
                cur.is_symlink ? " -- a symlink target must never alias another interval"
                               : " -- distinct stored intervals must not overlap");
        }
        uint64_t cur_end = (uint64_t)cur.start + cur.len;
        if (cur_end > max_end) max_end = cur_end;
        prev = cur;
        have_prev = true;
    }
}

/* ------------------------------------------------------------------ */
/* ATTR (mode,uid,gid,ext_off) dedup tracking                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t mode;
    uint32_t uid, gid, ext_off;
    uint32_t cell_idx;
} attr_tuple_t;

static attr_tuple_t *g_attr_tuples;
static uint32_t g_attr_tuple_count, g_attr_tuple_cap;

static void record_attr_tuple(uint16_t mode, uint32_t uid, uint32_t gid, uint32_t ext_off, uint32_t cell_idx) {
    if (g_attr_tuple_count == g_attr_tuple_cap) {
        g_attr_tuple_cap = g_attr_tuple_cap ? g_attr_tuple_cap * 2 : 128;
        g_attr_tuples = realloc(g_attr_tuples, g_attr_tuple_cap * sizeof(*g_attr_tuples));
        if (!g_attr_tuples) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    g_attr_tuples[g_attr_tuple_count++] = (attr_tuple_t){mode, uid, gid, ext_off, cell_idx};
}

static int attr_tuple_cmp(const void *pa, const void *pb) {
    const attr_tuple_t *a = pa, *b = pb;
    if (a->mode != b->mode) return (a->mode < b->mode) ? -1 : 1;
    if (a->uid != b->uid) return (a->uid < b->uid) ? -1 : 1;
    if (a->gid != b->gid) return (a->gid < b->gid) ? -1 : 1;
    if (a->ext_off != b->ext_off) return (a->ext_off < b->ext_off) ? -1 : 1;
    return 0;
}

static void check_attr_dedup(void) {
    if (g_attr_tuple_count < 2) return;
    qsort(g_attr_tuples, g_attr_tuple_count, sizeof(attr_tuple_t), attr_tuple_cmp);
    for (uint32_t i = 1; i < g_attr_tuple_count; i++) {
        attr_tuple_t *a = &g_attr_tuples[i - 1], *b = &g_attr_tuples[i];
        if (a->mode == b->mode && a->uid == b->uid && a->gid == b->gid && a->ext_off == b->ext_off)
            ERR("cell %u (ATTR): duplicate (mode,uid,gid,ext_off) tuple, also at cell %u "
                "(each distinct tuple must be represented by exactly one ATTR cell)",
                b->cell_idx, a->cell_idx);
    }
}

/* ------------------------------------------------------------------ */
/* Directory descriptor, gathered for root (pseudo) + every DIR cell   */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t  owner_idx;     /* -1 for root, else the DIR cell's own index */
    uint32_t bucket_first;
    uint32_t tablesize;
    uint32_t n;
} dir_desc_t;

static dir_desc_t *g_dirs;
static uint32_t g_dir_count;
static uint32_t g_dir_cap;
static uint32_t g_queue_head;

static void add_dir(int32_t owner_idx, uint32_t bucket_first, uint32_t tablesize, uint32_t n) {
    if (g_dir_count == g_dir_cap) {
        g_dir_cap = g_dir_cap ? g_dir_cap * 2 : 64;
        g_dirs = realloc(g_dirs, g_dir_cap * sizeof(*g_dirs));
        if (!g_dirs) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    g_dirs[g_dir_count].owner_idx = owner_idx;
    g_dirs[g_dir_count].bucket_first = bucket_first;
    g_dirs[g_dir_count].tablesize = tablesize;
    g_dirs[g_dir_count].n = n;
    g_dir_count++;
}

/* which directory slice (index into g_dirs) owns each cell; UINT32_MAX = none */
static uint32_t *g_slice_owner;
static bool *g_obj_visited;      /* standalone object cell reached via tree walk */
static bool *g_bucket_targeted;  /* already targeted by one occupied bucket */

static header_t g_h;
static uint32_t g_cell_count;
static uint32_t g_image_size;
static bool g_any_dual_mode;
static uint32_t g_object_count;  /* discovered non-root objects */

/* ------------------------------------------------------------------ */
/* Shared blob-layout validation (filenames and file/symlink payloads  */
/* use one canonical encoded form, per "Blob Encoding And Word         */
/* Alignment")                                                         */
/* ------------------------------------------------------------------ */

/* Validates offset alignment, part-containment, tail-zero padding, and
 * the page-crossing rule for a stored blob of `announced_len` bytes at
 * `off`. On success, returns true and sets *stored_out to
 * stored_blob_bytes. Does not record the interval or mark coverage --
 * callers do that so filenames and payloads can tag the interval's
 * domain (shared vs. symlink-private) themselves. */
static bool check_blob_layout(uint32_t off, uint32_t announced_len, const char *ctx, uint64_t *stored_out) {
    if (off % FEMTOFS_BLOB_WORD != 0) {
        ERR("%s: offset (%u) is not 4-byte aligned", ctx, off);
        return false;
    }
    if (!(g_h.public_off <= off && off < g_image_size)) {
        ERR("%s: offset (%u) out of [public_off, image_size)", ctx, off);
        return false;
    }
    uint64_t stored = (uint64_t)announced_len + 1;
    stored = (stored + (FEMTOFS_BLOB_WORD - 1)) & ~(uint64_t)(FEMTOFS_BLOB_WORD - 1);
    if (stored > UINT32_MAX) {
        ERR("%s: stored_blob_bytes overflows uint32_t (announced_len=%u)", ctx, announced_len);
        return false;
    }
    uint32_t part_end = (off < g_h.private_off) ? g_h.private_off : g_image_size;
    if (off + stored > part_end) {
        ERR("%s: stored range [%u,+%" PRIu64 ") crosses out of its content part (part end %u)",
            ctx, off, stored, part_end);
        return false;
    }
    for (uint64_t p = (uint64_t)off + announced_len; p < (uint64_t)off + stored; p++) {
        if (g_image[p] != 0) {
            ERR("%s: stored-blob tail byte at offset %" PRIu64 " is not zero", ctx, p);
            return false;
        }
    }
    if (stored < FEMTOFS_PAGE_SIZE) {
        uint64_t start_page = off / FEMTOFS_PAGE_SIZE;
        uint64_t end_page = ((uint64_t)off + stored - 1) / FEMTOFS_PAGE_SIZE;
        if (start_page != end_page) {
            ERR("%s: sub-page stored range [%u,+%" PRIu64 ") crosses an image-page boundary",
                ctx, off, stored);
            return false;
        }
    } else if (off % FEMTOFS_PAGE_SIZE != 0) {
        ERR("%s: stored range >= PAGE_SIZE must start page-aligned (off=%u)", ctx, off);
        return false;
    }
    *stored_out = stored;
    return true;
}

/* Measures and validates a NUL-terminated, '/'-free filename stored at
 * `off`; returns its length (excluding terminator) or -1 on failure.
 * The terminator must occur within the same content part as `off`. */
static long scan_filename_length(uint32_t off, const char *ctx) {
    if (off % FEMTOFS_BLOB_WORD != 0) {
        ERR("%s: name_off (%u) is not 4-byte aligned", ctx, off);
        return -1;
    }
    if (!(g_h.public_off <= off && off < g_image_size)) {
        ERR("%s: name_off (%u) out of [public_off, image_size)", ctx, off);
        return -1;
    }
    uint32_t part_end = (off < g_h.private_off) ? g_h.private_off : g_image_size;
    uint32_t i = off;
    bool saw_slash = false;
    while (i < part_end && g_image[i] != 0) {
        if (g_image[i] == '/') saw_slash = true;
        i++;
    }
    if (i >= part_end) {
        ERR("%s: filename at offset %u is not NUL-terminated within its content part (part end %u)",
            ctx, off, part_end);
        return -1;
    }
    uint32_t len = i - off;
    if (len < 1 || len > 255) {
        ERR("%s: filename at offset %u has invalid length %u (must be 1..255)", ctx, off, len);
        return -1;
    }
    if (saw_slash) {
        ERR("%s: filename at offset %u contains '/'", ctx, off);
        return -1;
    }
    if ((len == 1 && g_image[off] == '.') ||
        (len == 2 && g_image[off] == '.' && g_image[off + 1] == '.')) {
        ERR("%s: filename at offset %u is \".\" or \"..\", forbidden on-disk", ctx, off);
        return -1;
    }
    return (long)len;
}

/* ------------------------------------------------------------------ */
/* Tree-walk object validation                                          */
/* ------------------------------------------------------------------ */

/* Validates the standalone object cell at idx (reached via someone's
 * data_off, i.e. NOT interpreted as a directory-bucket entry), exactly
 * once per index. `expected_parent` is -1 if idx was targeted by a root
 * bucket, else the metadata index of the directory whose bucket targeted
 * it -- used only for the DIR case's exact parent-pointer check. For
 * DIR objects, also enqueues them for the Phase 2 BFS in main() via
 * add_dir(). For HARDLINK objects, also recurses into the canonical
 * FILE target so it gets validated/covered even if this hardlink is
 * (irregularly) the only path that reaches it; that recursive call
 * passes an unused expected_parent since FILE ignores it.
 *
 * Precondition: idx < g_cell_count, and cell[idx] is a known, non-null,
 * non-auxiliary type outside every directory slice (callers already
 * checked this). */
static void visit_object(uint32_t idx, int32_t expected_parent) {
    if (g_obj_visited[idx]) return; /* dedup: HARDLINK's internal recursion into its FILE target */
    g_obj_visited[idx] = true;
    g_object_count++;

    cell_t c = read_cell(idx);

    switch (c.type) {
    case FEMTOFS_TYPE_FILE:
    case FEMTOFS_TYPE_SYMLINK: {
        if (c.w2 != 0)
            ERR("cell %u (%s): realsize must be 0, got %u", idx, type_name(c.type), c.w2);

        if (c.f2 >= g_cell_count) {
            ERR("cell %u (%s): attr_index (%u) >= cell_count (%u)", idx, type_name(c.type), c.f2, g_cell_count);
        } else {
            cell_t ac = read_cell(c.f2);
            if (ac.type != FEMTOFS_TYPE_ATTR) {
                ERR("cell %u (%s): attr_index (%u) does not point to an ATTR cell (type=%s)",
                    idx, type_name(c.type), c.f2, type_name(ac.type));
            } else {
                uint32_t mode = ac.f2;
                uint32_t expect = (c.type == FEMTOFS_TYPE_FILE) ? S_IFREG_BITS : S_IFLNK_BITS;
                if ((mode & S_IFMT_BITS) != expect)
                    ERR("cell %u (%s): attr mode 0%04o file-type bits disagree with cell type",
                        idx, type_name(c.type), mode);
            }
        }

        char ctxbuf[48];
        snprintf(ctxbuf, sizeof(ctxbuf), "cell %u (%s payload)", idx, type_name(c.type));
        uint64_t stored;
        if (check_blob_layout(c.w0, c.w1, ctxbuf, &stored)) {
            bool is_symlink = (c.type == FEMTOFS_TYPE_SYMLINK);
            if (is_symlink && c.w0 < g_h.private_off)
                ERR("cell %u (SYMLINK): payload must be private (data_off %u < private_off %u)",
                    idx, c.w0, g_h.private_off);
            if (is_symlink) {
                for (uint64_t p = c.w0; p < (uint64_t)c.w0 + c.w1; p++) {
                    if (g_image[p] == 0) {
                        ERR("cell %u (SYMLINK): announced payload bytes contain an embedded NUL "
                            "at offset %" PRIu64, idx, p);
                        break;
                    }
                }
            }
            mark_covered(c.w0, stored);
            record_interval(c.w0, (uint32_t)stored, is_symlink);
        }
        break;
    }

    case FEMTOFS_TYPE_DIR: {
        uint32_t mode, p1;
        if (!decode_hash_ctrl(c.b1, &mode, &p1))
            ERR("cell %u (DIR): hash_p (0x%02x) decodes to invalid mode=%u or p1_index=%u",
                idx, c.b1, mode, p1);
        else if (mode == FEMTOFS_HASH_MODE_DUAL)
            g_any_dual_mode = true;

        if (c.w2 & FEMTOFS_DIR_RESERVED_BIT)
            ERR("cell %u (DIR): realsize bit[31] must be 0", idx);
        uint32_t parent = (uint32_t)(c.w2 & FEMTOFS_DIR_PARENT_MASK);
        uint32_t n = (uint32_t)((c.w2 & FEMTOFS_DIR_N_MASK) >> FEMTOFS_DIR_N_SHIFT);

        if (expected_parent == -1) {
            if (parent != FEMTOFS_PARENT_ROOT)
                ERR("cell %u (DIR): targeted by a root bucket, so parent must be "
                    "FEMTOFS_PARENT_ROOT, got %u", idx, parent);
        } else if (parent != (uint32_t)expected_parent) {
            ERR("cell %u (DIR): targeted by a bucket in directory %d, so parent must equal "
                "%d, got %u", idx, expected_parent, expected_parent, parent);
        }

        if (c.f2 >= g_cell_count) {
            ERR("cell %u (DIR): attr_index (%u) >= cell_count (%u)", idx, c.f2, g_cell_count);
        } else {
            cell_t ac = read_cell(c.f2);
            if (ac.type != FEMTOFS_TYPE_ATTR) {
                ERR("cell %u (DIR): attr_index (%u) does not point to an ATTR cell (type=%s)",
                    idx, c.f2, type_name(ac.type));
            } else if ((ac.f2 & S_IFMT_BITS) != S_IFDIR_BITS) {
                ERR("cell %u (DIR): attr mode 0%04o file-type bits disagree with cell type", idx, ac.f2);
            }
        }

        uint32_t tablesize = c.w1;
        uint32_t bucket_first = c.w0;
        if (tablesize > MAX_TABLESIZE_PRIME)
            ERR("cell %u (DIR): tablesize (%u) exceeds MAX_TABLESIZE_PRIME (%u)", idx, tablesize, MAX_TABLESIZE_PRIME);
        if (n >= 32768u)
            ERR("cell %u (DIR): N (%u) must be < 2^15", idx, n);
        if (n > tablesize)
            ERR("cell %u (DIR): N (%u) > tablesize (%u)", idx, n, tablesize);
        if (tablesize == 0) {
            if (n != 0)
                ERR("cell %u (DIR): tablesize == 0 but N (%u) != 0", idx, n);
            if (bucket_first != 0)
                ERR("cell %u (DIR): tablesize == 0 but bucket_first (%u) != 0", idx, bucket_first);
        } else if ((uint64_t)bucket_first + tablesize > g_cell_count) {
            ERR("cell %u (DIR): bucket slice [%u,%u) exceeds cell_count (%u)",
                idx, bucket_first, bucket_first + tablesize, g_cell_count);
        }

        add_dir((int32_t)idx, bucket_first, tablesize, n);
        break;
    }

    case FEMTOFS_TYPE_FIFO: {
        if (c.w0 != 0 || c.w1 != 0 || c.w2 != 0)
            ERR("cell %u (FIFO): data_off/size/realsize must all be 0 (got %u,%u,%u)",
                idx, c.w0, c.w1, c.w2);
        if (c.f2 >= g_cell_count) {
            ERR("cell %u (FIFO): attr_index (%u) >= cell_count (%u)", idx, c.f2, g_cell_count);
        } else {
            cell_t ac = read_cell(c.f2);
            if (ac.type != FEMTOFS_TYPE_ATTR)
                ERR("cell %u (FIFO): attr_index (%u) does not point to an ATTR cell (type=%s)",
                    idx, c.f2, type_name(ac.type));
            else if ((ac.f2 & S_IFMT_BITS) != S_IFIFO_BITS)
                ERR("cell %u (FIFO): attr mode 0%04o file-type bits disagree with cell type", idx, ac.f2);
        }
        break;
    }

    case FEMTOFS_TYPE_HARDLINK: {
        if (c.f2 != 0)
            ERR("cell %u (HARDLINK): attr_index must be 0, got %u", idx, c.f2);
        if (c.w1 != 0)
            ERR("cell %u (HARDLINK): size must be 0, got %u", idx, c.w1);
        if (c.w2 != 0)
            ERR("cell %u (HARDLINK): realsize must be 0, got %u", idx, c.w2);
        uint32_t target = c.w0;
        if (target >= g_cell_count) {
            ERR("cell %u (HARDLINK): target index (%u) >= cell_count (%u)", idx, target, g_cell_count);
        } else {
            cell_t tc = read_cell(target);
            if (tc.type != FEMTOFS_TYPE_FILE) {
                ERR("cell %u (HARDLINK): target (%u) must be FEMTOFS_TYPE_FILE, got %s",
                    idx, target, type_name(tc.type));
            } else if (g_slice_owner[target] != UINT32_MAX) {
                ERR("cell %u (HARDLINK): target %u coincides with a directory bucket slice",
                    idx, target);
            } else {
                visit_object(target, 0 /* unused: FILE ignores expected_parent */);
            }
        }
        break;
    }

    default:
        break; /* NULL/ATTR never reach here */
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-v] <image>\n", prog);
}

int main(int argc, char **argv) {
    bool opt_v = false;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-v") == 0) { opt_v = true; argi++; }
        else if (strcmp(argv[argi], "--") == 0) { argi++; break; }
        else { usage(argv[0]); return 2; }
    }
    if (argi >= argc) { usage(argv[0]); return 2; }
    const char *path = argv[argi];
    g_verbose = opt_v;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }

    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 2; }
    if (st.st_size <= 0) {
        fprintf(stderr, "ERROR: %s: empty or invalid file\n", path);
        close(fd);
        return 2;
    }
    g_image_len = (uint64_t)st.st_size;

    void *map = mmap(NULL, (size_t)g_image_len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 2; }
    close(fd);
    g_image = (const uint8_t *)map;

    if (g_image_len < FEMTOFS_HEADER_SIZE) {
        ERR("file is smaller than the 128-byte femtoFS header (%" PRIu64 " bytes)", g_image_len);
        fprintf(stderr, "\n%ld error(s)\n", g_error_count);
        return 1;
    }

    if (sizeof(SMALL_PRIMES) / sizeof(SMALL_PRIMES[0]) != SMALL_PRIMES_COUNT) {
        fprintf(stderr, "BUG: this checker's SMALL_PRIMES array (%zu entries) "
                "does not match SMALL_PRIMES_COUNT (%u)\n",
                sizeof(SMALL_PRIMES) / sizeof(SMALL_PRIMES[0]), SMALL_PRIMES_COUNT);
        return 2;
    }

    printf("fsck_femtofs: checking %s (%" PRIu64 " bytes)\n", path, g_image_len);

    /* ---------------- Header and top-level bounds ---------------- */

    header_t h;
    parse_header(&h);

    if (memcmp(h.magic, "0FS\0", 4) != 0)
        ERR("magic is not {'0','F','S','\\0'} (got %02x %02x %02x %02x)",
            h.magic[0], h.magic[1], h.magic[2], h.magic[3]);
    if (!(h.format[0] == 0 && h.format[1] == 4))
        ERR("format field is not {0,4} for version 0x0100 (got {%u,%u})",
            h.format[0], h.format[1]);
    uint8_t vmaj = (uint8_t)(h.version >> 8);
    uint8_t vrev = (uint8_t)(h.version & 0xFF);
    if (vmaj != 1)
        ERR("unsupported major version %u (this checker understands major 1 only)", vmaj);
    else
        INFO("version 0x%04x (major %u, revision %u)", h.version, vmaj, vrev);

    if (!in_bounds(72, 56) || memcmp(g_image + 72, AUTHOR_FIELD, 56) != 0)
        ERR("author field does not match the required format-author identifier");

    if (h.root_pad != 0)
        ERR("root_pad must be 0, got %u", h.root_pad);

    if (h.image_size == 0)
        ERR("image_size must be > 0");
    if ((uint64_t)h.image_size > g_image_len)
        ERR("image_size (%u) exceeds backing file size (%" PRIu64 ")", h.image_size, g_image_len);
    if (h.image_size % FEMTOFS_PAGE_SIZE != 0)
        ERR("image_size (%u) is not page-aligned (page size %u)", h.image_size, FEMTOFS_PAGE_SIZE);
    uint32_t image_size = h.image_size;

    if (h.cell_count == 0)
        ERR("cell_count must be > 0");
    if (h.cell_count >= MAX_CELL_COUNT)
        ERR("cell_count (%u) must be < 2^16 (%u)", h.cell_count, MAX_CELL_COUNT);
    if (h.meta_size != (uint64_t)h.cell_count * FEMTOFS_CELL_SIZE)
        ERR("meta_size (%u) != cell_count * 16 (%u * 16 = %" PRIu64 ")",
            h.meta_size, h.cell_count, (uint64_t)h.cell_count * FEMTOFS_CELL_SIZE);

    bool bounds_ok = true;
    if (!((uint64_t)FEMTOFS_HEADER_SIZE + h.meta_size <= h.public_off &&
          h.public_off <= h.private_off && h.private_off <= image_size)) {
        ERR("region ordering violated: need 128 + meta_size(%u) <= public_off(%u) "
            "<= private_off(%u) <= image_size(%u)",
            h.meta_size, h.public_off, h.private_off, image_size);
        bounds_ok = false;
    }
    if (h.public_off % FEMTOFS_PAGE_SIZE != 0)
        ERR("public_off (%u) is not page-aligned (page size %u)", h.public_off, FEMTOFS_PAGE_SIZE);
    if (h.private_off % FEMTOFS_PAGE_SIZE != 0)
        ERR("private_off (%u) is not page-aligned (page size %u)", h.private_off, FEMTOFS_PAGE_SIZE);

    if (h.cell_count == 0 || h.cell_count >= MAX_CELL_COUNT ||
        !in_bounds(FEMTOFS_HEADER_SIZE, (uint64_t)h.cell_count * FEMTOFS_CELL_SIZE) ||
        !bounds_ok || image_size == 0 || (uint64_t)image_size > g_image_len) {
        ERR("header is too damaged to continue structural checks safely; stopping here");
        fprintf(stderr, "\n%ld error(s)\n", g_error_count);
        munmap(map, (size_t)g_image_len);
        return 1;
    }
    uint32_t cell_count = h.cell_count;

    if (h.root_attr >= cell_count) {
        ERR("root_attr (%u) >= cell_count (%u)", h.root_attr, cell_count);
    } else {
        cell_t rac = read_cell(h.root_attr);
        if (rac.type != FEMTOFS_TYPE_ATTR)
            ERR("root_attr (%u) does not point to an ATTR cell (type=%s)",
                h.root_attr, type_name(rac.type));
        else if ((rac.f2 & S_IFMT_BITS) != S_IFDIR_BITS)
            ERR("root_attr's mode 0%04o does not have S_IFDIR file-type bits", rac.f2);
    }

    if (h.root_size > MAX_TABLESIZE_PRIME)
        ERR("root_size (%u) exceeds MAX_TABLESIZE_PRIME (%u)", h.root_size, MAX_TABLESIZE_PRIME);
    if (h.root_real >= 32768u)
        ERR("root_real (%u) must be < 2^15", h.root_real);
    if (h.root_real > h.root_size)
        ERR("root_real (%u) > root_size (%u)", h.root_real, h.root_size);
    if (h.root_size == 0) {
        if (h.root_real != 0)
            ERR("root_size == 0 but root_real (%u) != 0", h.root_real);
        if (h.root_first != 0)
            ERR("root_size == 0 but root_first (%u) != 0", h.root_first);
    } else if ((uint64_t)h.root_first + h.root_size > cell_count) {
        ERR("root bucket slice [%u, %u) exceeds cell_count (%u)",
            h.root_first, h.root_first + h.root_size, cell_count);
    }

    uint32_t root_mode, root_p1;
    if (!decode_hash_ctrl(h.root_p, &root_mode, &root_p1))
        ERR("root_p (0x%02x) decodes to invalid mode=%u or p1_index=%u (must be < %u)",
            h.root_p, root_mode, root_p1, SMALL_PRIMES_COUNT);
    g_any_dual_mode = (root_mode == FEMTOFS_HASH_MODE_DUAL);

    if (h.meta_hash != fnv1_32_buf(g_image + FEMTOFS_HEADER_SIZE, h.meta_size, FNV1_32_INIT))
        ERR("meta_hash mismatch: metadata table fails integrity check");
    else
        INFO("meta_hash OK");

    if (h.image_hash != fnv1_32_buf(g_image + 16, image_size - 16, FNV1_32_INIT))
        ERR("image_hash mismatch: image data fails integrity check");
    else
        INFO("image_hash OK");

    /* ---------------- Allocate working structures ---------------- */

    g_covered = calloc(image_size, 1);
    g_slice_owner = malloc((size_t)cell_count * sizeof(uint32_t));
    g_obj_visited = calloc(cell_count, sizeof(bool));
    g_bucket_targeted = calloc(cell_count, sizeof(bool));
    if (!g_covered || !g_slice_owner || !g_obj_visited || !g_bucket_targeted) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    for (uint32_t i = 0; i < cell_count; i++) g_slice_owner[i] = UINT32_MAX;

    g_h = h;
    g_cell_count = cell_count;
    g_image_size = image_size;

    mark_covered(0, FEMTOFS_HEADER_SIZE + (uint64_t)h.meta_size);

    /* ---------------------------------------------------------------
     * Phase 1: cheap, linear, position-independent checks -- unknown-
     * type rejection and ATTR-cell field validation, both fully
     * position-independent (ATTR cells "may appear anywhere in the
     * metadata table"). Object-shaped rules (FILE/DIR/SYMLINK/HARDLINK/
     * FIFO) cannot be checked linearly: see visit_object()'s comment.
     * --------------------------------------------------------------- */

    for (uint32_t i = 0; i < cell_count; i++) {
        cell_t c = read_cell(i);

        if (!type_is_known(c.type)) {
            ERR("cell %u: unknown %s type 0x%02x", i, is_aux_type(c.type) ? "aux" : "non-aux", c.type);
            continue;
        }
        if (c.type != FEMTOFS_TYPE_ATTR) continue;

        /* reinterpreted fields: b1=reserved0, f2=mode, w0=uid, w1=gid, w2=ext_off */
        if (c.b1 != 0)
            ERR("cell %u (ATTR): reserved0 must be 0, got %u", i, c.b1);
        if (c.w2 != 0)
            ERR("cell %u (ATTR): ext_off must be 0 in version 0x0100, got %u", i, c.w2);
        record_attr_tuple(c.f2, c.w0, c.w1, c.w2, i);
    }
    check_attr_dedup();

    /* ---------------------------------------------------------------
     * Phase 2: tree walk starting at root, per "Validation must start
     * with the root bucket slice and recursively discover filesystem
     * objects through occupied buckets."
     * --------------------------------------------------------------- */

    add_dir(-1, h.root_first, h.root_size, h.root_real);

    while (g_queue_head < g_dir_count) {
        dir_desc_t d = g_dirs[g_queue_head++];

        if (d.tablesize == 0) continue; /* bucket_first/N==0 already checked at discovery */
        if ((uint64_t)d.bucket_first + d.tablesize > cell_count) continue; /* already flagged */

        bool slice_ok = true;
        for (uint32_t idx = d.bucket_first; idx < d.bucket_first + d.tablesize; idx++) {
            if (g_slice_owner[idx] != UINT32_MAX) {
                ERR("cell %u: claimed by more than one directory's bucket slice "
                    "(owner %d and owner %d)",
                    idx, g_dirs[g_slice_owner[idx]].owner_idx, d.owner_idx);
                slice_ok = false;
            } else if (g_obj_visited[idx]) {
                ERR("cell %u: directory (owner %d) bucket slice coincides with an "
                    "already-visited standalone object cell", idx, d.owner_idx);
                slice_ok = false;
            } else {
                g_slice_owner[idx] = g_queue_head - 1;
            }
        }
        if (!slice_ok) continue;

        uint8_t dir_hash_p = (d.owner_idx == -1) ? h.root_p : read_cell((uint32_t)d.owner_idx).b1;
        uint32_t dir_hmode = 0, dir_hp1 = 0;
        bool dir_hash_ok = decode_hash_ctrl(dir_hash_p, &dir_hmode, &dir_hp1);

        uint32_t occupied_count = 0;
        uint32_t max_names = d.n + 1;
        uint32_t *name_offs = malloc((max_names ? max_names : 1) * sizeof(uint32_t));
        uint32_t name_count = 0;

        for (uint32_t rel = 0; rel < d.tablesize; rel++) {
            uint32_t idx = d.bucket_first + rel;
            cell_t c = read_cell(idx);

            if (c.type == FEMTOFS_TYPE_NULL) {
                if (c.b1 != 0) ERR("cell %u: empty bucket hash_p must be 0, got %u", idx, c.b1);
                if (c.f2 != 0) ERR("cell %u: empty bucket attr_index must be 0, got %u", idx, c.f2);
                if (c.w0 != 0) ERR("cell %u: empty bucket data_off must be 0, got %u", idx, c.w0);
                if (c.w1 != 0) ERR("cell %u: empty bucket size must be 0, got %u", idx, c.w1);
                if (c.w2 != d.tablesize)
                    ERR("cell %u: empty bucket realsize (%u) != tablesize (%u)", idx, c.w2, d.tablesize);
                continue;
            }
            if (is_aux_type(c.type)) continue; /* ATTR filler; validated in Phase 1 */

            occupied_count++;
            if (c.b1 != 0)
                ERR("cell %u: occupied bucket has hash_p != 0 (%u)", idx, c.b1);
            if (c.f2 != 0)
                ERR("cell %u: occupied bucket has attr_index != 0 (%u)", idx, c.f2);

            uint32_t realsize = c.w2;
            if (!(realsize == d.tablesize || realsize < d.tablesize))
                ERR("cell %u: occupied bucket realsize (%u) neither sentinel (%u) nor in-range",
                    idx, realsize, d.tablesize);
            else if (realsize < d.tablesize) {
                cell_t nc = read_cell(d.bucket_first + realsize);
                if (is_aux_type(nc.type))
                    ERR("cell %u: chain link points at auxiliary cell %u (forbidden)",
                        idx, d.bucket_first + realsize);
            }

            uint32_t target = c.w0;
            if (target >= cell_count) {
                ERR("cell %u: occupied bucket data_off (target object index %u) >= cell_count (%u)",
                    idx, target, cell_count);
            } else {
                cell_t tc = read_cell(target);
                if (tc.type == FEMTOFS_TYPE_NULL || is_aux_type(tc.type)) {
                    ERR("cell %u: occupied bucket target object %u is null/auxiliary (type=%s), "
                        "must be a real filesystem object", idx, target, type_name(tc.type));
                } else if (g_slice_owner[target] != UINT32_MAX) {
                    ERR("cell %u: occupied bucket target object %u coincides with a directory "
                        "bucket slice", idx, target);
                } else if (g_bucket_targeted[target]) {
                    ERR("cell %u: occupied bucket targets object %u, but that object is already "
                        "targeted by another occupied bucket (each non-root object must be "
                        "targeted by exactly one)", idx, target);
                } else {
                    if (tc.type != c.type)
                        ERR("cell %u: bucket type field (%s) does not exactly equal referenced "
                            "object %u's actual type (%s)", idx, type_name(c.type), target, type_name(tc.type));
                    g_bucket_targeted[target] = true;
                    visit_object(target, d.owner_idx);
                }
            }

            char ctxbuf[64];
            snprintf(ctxbuf, sizeof(ctxbuf), "cell %u (bucket, owner %d)", idx, d.owner_idx);
            long len = scan_filename_length(c.w1, ctxbuf);
            if (len >= 0) {
                uint64_t stored;
                if (check_blob_layout(c.w1, (uint32_t)len, ctxbuf, &stored)) {
                    mark_covered(c.w1, stored);
                    record_interval(c.w1, (uint32_t)stored, false);
                }
                if (name_count < max_names) {
                    for (uint32_t nn = 0; nn < name_count; nn++) {
                        uint32_t oo = name_offs[nn];
                        long olen = 0;
                        while (oo + olen < (long)image_size && g_image[oo + olen] != 0) olen++;
                        if (olen == len && memcmp(g_image + oo, g_image + c.w1, (size_t)len) == 0) {
                            ERR("cell %u: duplicate directory-entry name in directory (owner %d), also at offset %u",
                                idx, d.owner_idx, oo);
                            break;
                        }
                    }
                    name_offs[name_count++] = c.w1;
                }

                if (dir_hash_ok) {
                    uint32_t p1 = SMALL_PRIMES[dir_hp1];
                    uint32_t h1 = femtofs_hash(g_image + c.w1, (uint32_t)len, p1, d.tablesize);
                    chain_walk_t r1 = walk_anchor_chain(d.bucket_first, d.tablesize, h1, rel);
                    if (r1.bad)
                        ERR("cell %u: hash-anchor chain (h1=%u) in directory (owner %d) is corrupt "
                            "(cycle, out-of-slice jump, or link into an empty/auxiliary cell)",
                            idx, h1, d.owner_idx);
                    bool found = r1.found;
                    bool any_bad = r1.bad;
                    if (dir_hmode == FEMTOFS_HASH_MODE_DUAL) {
                        uint32_t h2 = femtofs_hash(g_image + c.w1, (uint32_t)len, h.hash2_base, d.tablesize);
                        if (h2 != h1) {
                            chain_walk_t r2 = walk_anchor_chain(d.bucket_first, d.tablesize, h2, rel);
                            if (r2.bad)
                                ERR("cell %u: hash-anchor chain (h2=%u) in directory (owner %d) is "
                                    "corrupt (cycle, out-of-slice jump, or link into an "
                                    "empty/auxiliary cell)", idx, h2, d.owner_idx);
                            found = found || r2.found;
                            any_bad = any_bad || r2.bad;
                        }
                    }
                    if (!found && !any_bad)
                        ERR("cell %u: entry \"%.*s\" in directory (owner %d) is not reachable from "
                            "its own computed hash anchor(s) -- lookup would return ENOENT for a "
                            "name that exists on disk", idx, (int)len, g_image + c.w1, d.owner_idx);
                }
            }
        }
        free(name_offs);

        if (occupied_count != d.n)
            ERR("directory (owner %d): occupied bucket count (%u) != N (%u)",
                d.owner_idx, occupied_count, d.n);
    }

    if (g_any_dual_mode) {
        if (!(h.hash2_base > 256u && h.hash2_base < 16777216u && is_prime_u32(h.hash2_base)))
            ERR("at least one directory uses DUAL hash mode but hash2_base (%u) is not a prime "
                "in (2^8, 2^24)", h.hash2_base);
    } else if (h.hash2_base != 0) {
        ERR("no directory uses DUAL hash mode, but hash2_base (%u) is not 0", h.hash2_base);
    }

    if (g_object_count >= MAX_OBJECT_COUNT)
        ERR("total discovered non-root filesystem object count (%u) must be < 2^15", g_object_count);

    /* ---------------- header-to-content alignment gap must be zero ---------------- */

    for (uint64_t off = FEMTOFS_HEADER_SIZE + h.meta_size; off < h.public_off; off++) {
        if (g_image[off] != 0) {
            ERR("header-to-content alignment gap contains non-zero byte at offset %" PRIu64, off);
            break;
        }
    }

    /* ---------------- content-interval overlap check ---------------- */

    check_interval_overlaps();

    /* ---------------- hole-zero scan over the content region ---------------- */

    {
        bool reported = false;
        for (uint64_t off = h.public_off; off < image_size; off++) {
            if (!g_covered[off] && g_image[off] != 0) {
                ERR("unaccounted-for non-zero byte at offset %" PRIu64
                    " (not part of any known stored interval; hole/padding must be 0x00)", off);
                reported = true;
                break;
            }
        }
        if (!reported) INFO("hole/padding zero-fill check passed");
    }

    /* ---------------- unreachable cells / NULL-outside-slice zero check ---------------- */

    for (uint32_t i = 0; i < cell_count; i++) {
        if (g_slice_owner[i] != UINT32_MAX) continue; /* bucket-slice cell */
        cell_t c = read_cell(i);
        switch (c.type) {
        case FEMTOFS_TYPE_FILE:
        case FEMTOFS_TYPE_DIR:
        case FEMTOFS_TYPE_SYMLINK:
        case FEMTOFS_TYPE_HARDLINK:
        case FEMTOFS_TYPE_FIFO:
            if (!g_obj_visited[i])
                ERR("cell %u (%s): not reachable from root (invalid: every non-null, "
                    "non-auxiliary cell outside all directory slices must be a discovered "
                    "object)", i, type_name(c.type));
            break;
        case FEMTOFS_TYPE_NULL:
            if (c.b1 != 0 || c.f2 != 0 || c.w0 != 0 || c.w1 != 0 || c.w2 != 0)
                ERR("cell %u (NULL, outside any directory slice): unused cell must be entirely "
                    "zero apart from the type byte", i);
            break;
        default:
            break; /* ATTR handled below */
        }
    }

    {
        bool *attr_ref = calloc(cell_count, sizeof(bool));
        if (!attr_ref) { fprintf(stderr, "out of memory\n"); return 2; }
        if (h.root_attr < cell_count) attr_ref[h.root_attr] = true;
        for (uint32_t i = 0; i < cell_count; i++) {
            if (g_slice_owner[i] != UINT32_MAX || !g_obj_visited[i]) continue;
            cell_t c = read_cell(i);
            if ((c.type == FEMTOFS_TYPE_FILE || c.type == FEMTOFS_TYPE_DIR ||
                 c.type == FEMTOFS_TYPE_SYMLINK || c.type == FEMTOFS_TYPE_FIFO) &&
                c.f2 < cell_count)
                attr_ref[c.f2] = true;
        }
        for (uint32_t i = 0; i < cell_count; i++) {
            cell_t c = read_cell(i);
            if (c.type == FEMTOFS_TYPE_ATTR && !attr_ref[i])
                ERR("cell %u (ATTR): not referenced by root_attr or any discovered "
                    "non-hardlink object (every ATTR cell must be referenced)", i);
        }
        free(attr_ref);
    }

    free(g_dirs);
    free(g_slice_owner);
    free(g_obj_visited);
    free(g_bucket_targeted);
    free(g_covered);
    free(g_intervals);
    free(g_attr_tuples);
    munmap(map, (size_t)g_image_len);

    fprintf(stderr, "\n%ld error(s)\n", g_error_count);
    if (g_error_count > 0) {
        printf("%s: FAILED\n", path);
        return 1;
    }
    printf("%s: OK\n", path);
    return 0;
}
