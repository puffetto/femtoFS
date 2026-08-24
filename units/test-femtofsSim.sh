#!/bin/sh
# File: units/test-femtofsSim.sh
# Created by Andrea "Nemesi" Cocito on 24/08/2026
# Check specification-sensitive simulator page and image accounting.

set -eu

simulator=$1
work_root=$2
case_dir=$(mktemp -d "$work_root/femtofsSim-test.XXXXXX")
trap 'rm -rf "$case_dir"' EXIT HUP INT TERM

cat > "$case_dir/list" <<'EOF'
1 1 drwxr-xr-x 2 root wheel 0 Aug 24 2026 ./
2 10 -r--r--r-- 1 root wheel 5000 Aug 24 2026 ./large
EOF

"$simulator" "$case_dir/list" 4096 --vm-page-size=4096 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment > "$case_dir/report"

grep -Eq '^  specification baseline: +0x0100$' "$case_dir/report"
grep -Eq '^  encoded page-size code: +4$' "$case_dir/report"
grep -Eq '^  public_off: +4096$' "$case_dir/report"
grep -Eq '^  private_off: +12288$' "$case_dir/report"
grep -Eq '^  image_size: +12.00 KiB \(12288 bytes\)$' "$case_dir/report"
grep -Eq '^Structural format-limit result: representable$' "$case_dir/report"

"$simulator" "$case_dir/list" 4096 --vm-page-size=16384 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment > "$case_dir/large-vm-report"
grep -Eq '^  effective mmap mode forced: +clean$' "$case_dir/large-vm-report"
grep -Eq '^  leaking guaranteed-aligned: +0$' "$case_dir/large-vm-report"

"$simulator" "$case_dir/list" 256 --vm-page-size=4096 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment > "$case_dir/future-page-report"
grep -Eq '^  encoded page-size code: +0$' "$case_dir/future-page-report"
grep -Fq 'future-format experiment, not a valid 0x0100 image model' \
    "$case_dir/future-page-report"

sed -n '1p' "$case_dir/list" > "$case_dir/root-only-list"
"$simulator" "$case_dir/root-only-list" 2147483648 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment > "$case_dir/max-page-report"
grep -Eq '^  encoded page-size code: +23$' "$case_dir/max-page-report"
grep -Eq '^  image_size: +2.00 GiB \(2147483648 bytes\)$' "$case_dir/max-page-report"
grep -Eq '^Structural format-limit result: representable$' "$case_dir/max-page-report"

if "$simulator" "$case_dir/list" 3000 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment \
    > "$case_dir/invalid-report" 2> "$case_dir/invalid-error"; then
    echo "invalid image page size was accepted" >&2
    exit 1
fi
grep -Fq 'image PAGE_SIZE must be a power of two from 256 through 2^31' \
    "$case_dir/invalid-error"

# END File: units/test-femtofsSim.sh
