# zfgfs

**Zero Fsck Giving. Zero Frills. Near-Zero-Copy.**

`zfgfs` is a read-only filesystem image format designed for one job: get bytes
from storage into userspace fast, predictably, and without dragging around the
complexity of a writable filesystem pretending to be an appliance image.

If you want a compact image that mounts cleanly, looks up paths in O(1)-style
hashed directories, offers clean/leaking/dirty `mmap(2)` mount modes, and
never needs a repair utility, this is the point.

## Why It Exists

Most filesystems are built for mutation first and deployment second. `zfgfs`
goes the other way.

It is built for:

- read-only system images
- RAM-backed mounts with `md(4)`-style workflows
- small, memory-friendly metadata
- correct `uid`/`gid`/mode semantics for real system trees
- deterministic lookup behavior
- aggressive sharing of packed content bytes
- mode-selectable `mmap(2)` behavior:
  - `clean` (default): no neighboring-byte exposure
  - `leaking`/`dirty`: only public-pool neighboring bytes may be exposed

That makes it a strong fit for base system payloads, installer media, rescue
environments, embedded FreeBSD deployments, immutable appliances, VM images,
and container-like distribution formats where correctness and startup latency
matter more than write support.

## What Makes It Different

`zfgfs` is intentionally small and sharp:

- A whole image is four regions: fixed 128-byte header, fixed-size metadata
  table, public content pool, private content pool.
- Every on-disk record uses fixed-width little-endian fields. No ABI roulette.
- Directories are hashed at build time, so lookups are cheap and predictable.
- Object metadata preserves full Unix mode bits plus numeric `uid`/`gid`.
- Regular-file payload and filename dedup classes can be promoted to the public
  pool when any visible reference is public-eligible.
- Symlink targets and metadata-extension payloads stay private-pool by policy.
- `clean` mode copies at most one tail page per mapped file object.
- `leaking` mode removes that copy for public-pool files.
- `dirty` further allows shifted zero-copy for public-eligible files
  smaller than one page.
- There is no journal, no write path, no recovery dance, and therefore no
  `fsck` story to apologize for.

This is not a general-purpose filesystem. That is the advantage.

## Why FreeBSD Should Care

FreeBSD already has the right instincts for clean kernel interfaces and
practical system engineering. `zfgfs` fits that culture:

- The on-disk ABI is simple enough to audit.
- The mount semantics are narrow enough to implement well.
- The read path is optimized for what the base system actually does a lot:
  open, lookup, read, and `mmap`.
- The format is honest about scope. It preserves the metadata that actually
  matters for a bootable tree: ownership and full mode bits, including
  setuid/setgid/sticky.
- It does not spend core ABI space on BSD file flags whose practical value is
  marginal on a read-only image.
- It leaves a forward hook for future ACL and extended-attribute support
  without bloating the common case.
- Security policy is explicit and auditable: private-pool bytes are never
  exposed by neighbor spill in any mode; only public-pool bytes may spill in
  `leaking`/`dirty`.

For maintainers, this means less policy hidden in edge cases. For users, it
means faster cold starts, less metadata overhead, and a filesystem that behaves
like a deployable artifact instead of a tiny database.

## Design Snapshot

- Target platforms: FreeBSD 14+, `amd64` and `arm64`
- Mode: read-only
- Image size limit: `< 2^32` bytes
- Metadata-table cell limit: `< 2^16`
- Theoretical guaranteed visible-object limit: `< 2^15`
- In practical trees, attr deduplication and ordinary hash occupancy should
  allow counts much closer to `< 2^16`
- Per-directory entry limit: `< 2^15`
- Root directory metadata stored directly in the header
- Full Unix metadata carried through deduplicated attribute cells:
  `uid`, `gid`, and complete `st_mode`
- Directory entries, object metadata, and deduplicated attribute records share
  the same 16-byte metadata table
- Hardlinks resolve by canonical object index
- Two content pools separate spill-safe public bytes from private bytes
- Regular-file payload classes are public/private by eligibility and dedup
  promotion
- Filename string classes are public/private by directory visibility and dedup
  promotion
- Symlink targets remain private-pool content
- Per-pool packing keeps sub-page objects page-contained and page-aligns large
  objects automatically
- `mmap` mount modes:
  - `clean` (default): page-aligned + zero-fill outside file range
  - `leaking`: page-aligned, public-pool spill allowed
  - `dirty`: `leaking` plus shifted zero-copy for public-eligible sub-page
    files
- One reserved extension pointer in each attribute record leaves room for
  future ACL/xattr payloads in private-pool content

The result is a format that is easy to generate, cheap to mount, and pleasant
to reason about under a debugger.

## What You Do Not Get

You do not get in-place writes, journaling, or a kitchen-sink metadata model.
Version 1 intentionally skips BSD file flags and does not yet define ACL or
extended-attribute payloads, even though the format leaves space for them
later. If you need a mutable filesystem, use one. `zfgfs` is for shipping
known-good trees efficiently and mounting them with minimal drama.

## Repository Status

This repository is specification-first. The core format is documented in
[specification.md](/Users/blackye/zfgfs/specification.md), and the simulator in
[programs/zerofsSim.cpp](/Users/blackye/zfgfs/programs/zerofsSim.cpp) explores
hash selection, visibility split behavior, and packing overhead.

## Pitch In One Sentence

`zfgfs` is the filesystem for people who want immutable images to mount fast,
map safely, stay small, and never waste a boot on repair theater.
