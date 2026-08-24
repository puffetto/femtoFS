Went through the whole spec carefully — cross-checking the struct layouts, the union aliasing, and every normative rule against each other. Here's what I found, worst first.

## Confirmed bugs

**1. `SMALL_PRIMES_COUNT` is wrong.** The array literally has **53** entries, not 54:

```
3,5,7,11,13,17,19,23,29,31,37,41,43,47,          14
53,59,61,67,71,73,79,83,89,97,101,103,107,       13
109,113,127,131,137,139,149,151,157,163,167,     11
173,179,181,191,193,197,199,211,223,227,229,     11
233,239,241,251                                   4
```
14+13+11+11+4 = 53. But `#define SMALL_PRIMES_COUNT 54`. Since validation requires `p1_index < SMALL_PRIMES_COUNT`, an encoder or reader that trusts the constant will accept `p1_index == 53` as valid and then index one past the end of the array — real OOB read in any straightforward C implementation. You need to either drop the count to 53 or add the missing prime (and figure out what it should have been — maybe `257`, continuing the sequence, though that's `> 2^8` and starts colliding conceptually with the `hash2_base` range `2^8 < hash2_base < 2^24`, so I'd just fix the count rather than extend the table).

**2. `root_size` bound is inconsistent with ordinary directories.** Validation rules say:
> `root_size < 2^15`

But for every other directory, `tablesize` (the same quantity — `bucket_count`) is documented as "bounded by `<= 65521`" (`MAX_TABLESIZE_PRIME`), and the hash-parameter search algorithm explicitly grows `tablesize` up to `65521` via `next_prime`. There's nothing in the "Scope" or "Format Overview" sections suggesting root is supposed to be special-cased to a stricter bound. If root legitimately needs a table bigger than 32767 slots (a plausible root directory in a real tree, e.g. thousands of packages), the current validation rule would reject an otherwise-conformant image built by the reference algorithm. Worth deciding explicitly: either root really is capped tighter than other dirs (and that should be called out as intentional, with a reason), or the `2^15` should be `65521` to match the general case.

## Design gaps worth resolving before implementation

**3. `PAGE_SIZE` is never pinned down.** The whole zero-copy story (`public_off`/`private_off` page-alignment, packing "contents `>= PAGE_SIZE`", the three mmap modes) is built on an assumption of one page size, but the format targets both amd64 and arm64, and FreeBSD/arm64 doesn't universally guarantee 4K pages (support exists for larger page sizes on some arm64 targets). If a `dirty`/`leaking`-mode image is built assuming 4K and then mounted on a kernel/hardware combo running with a larger page size, `public_off`/`private_off` and packed content may no longer be page-aligned relative to the *runtime* page size, silently breaking the zero-copy contract or (worse) the neighbor-spill safety guarantees in `dirty`/`leaking`. I'd suggest either: (a) record the builder's assumed page size in the header and have the reader fall back to `clean` mode whenever it disagrees with `getpagesize()`, or (b) state explicitly that femtoFS only supports 4K-page kernels and have the reader refuse to mount otherwise. Right now it's silently undefined.

**4. Free-hole/padding byte contents are unspecified.** The packing algorithm produces gaps (`< PAGE_SIZE`) between packed contents for page alignment, and `image_hash`/`meta_hash` cover the *entire* byte range including those gaps. Nothing says what a builder must write into hole bytes. Two consequences: (a) two builders packing an identical source tree could produce different `image_hash` values depending on what garbage is left in the holes, undermining "same image" fingerprinting/reproducible builds; (b) if a builder reuses a scratch buffer without zeroing it, hole bytes could leak whatever was previously in that memory into the shipped image (a low-severity but real info-leak pattern, same class of bug that's bitten tar/ISO builders before). I'd add a "hole bytes MUST be zero" rule.

## Minor / clarity nits

- The bucket role's `size` field (aliased from `role.raw.size`) actually holds a **filename byte offset**, not a size — the doc does explain this, but the compact-notation aliasing (`data_off`/`size`/`realsize` reused across DIR/FILE/bucket roles with totally different meanings per role) is a sharp edge for anyone implementing off memory rather than re-reading the union each time. A implementer who greps for `size` and assumes "byte length" in bucket-handling code has an easy path to a real bug. Worth a one-line callout right where bucket fields are introduced.
- The header comment `// format version, currently 0x00000100` is written like a 32-bit literal next to a `uint16_t` field. It's numerically fine (`0x0100` = 256 fits in 16 bits), but the 8-digit style will make readers do a double-take — worth trimming to `0x0100`.
- Worth stating explicitly (it's implied but never said outright) that **NULL/empty bucket cells inside a directory's hash slice are legitimate placement targets for `ATTR` cells**, since the AUX-skip rule in lookup makes an `ATTR` cell behave identically to an empty bucket. That's a nice bit of space reuse, but it's currently something a reader has to *derive* from two separate rules rather than being told — I'd make it an explicit note in the attribute-placement policy so nobody "fixes" it by mistake later.

Everything else — the offset/index terminology split, the public/private classification, hardlink canonicalization (good, it structurally forbids hardlink chains so canonical-index resolution never recurses), the `.`/`..` handling, and the struct-size static asserts — checks out internally consistent.
