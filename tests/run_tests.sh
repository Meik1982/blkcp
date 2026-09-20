#!/usr/bin/env bash
set -euo pipefail

BLKCP_BIN="./blkcp"
if [[ ! -x "$BLKCP_BIN" ]]; then
  echo "Error: Binary '$BLKCP_BIN' not found. Run 'make release' first." >&2
  exit 1
fi
TMP_DIR=$(mktemp -d -t blkcp_test_XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

echo "=== Running Extended blkcp Test Suite ==="

# Test 1: Basic stdin to stdout
printf "hello world" | $BLKCP_BIN status=none > "$TMP_DIR/out1"
[[ "$(<"$TMP_DIR/out1")" == "hello world" ]]
echo "Test 1 passed: Basic stdin -> stdout"

# Test 2: if and of with bs and count
head -c 2048 /dev/zero > "$TMP_DIR/in2"
$BLKCP_BIN if="$TMP_DIR/in2" of="$TMP_DIR/out2" bs=512 count=2 status=none
[[ $(wc -c < "$TMP_DIR/out2") -eq 1024 ]]
echo "Test 2 passed: if/of with bs=512 count=2"

# Test 3: skip and seek
printf "0123456789ABCDEF" > "$TMP_DIR/in3"
$BLKCP_BIN if="$TMP_DIR/in3" of="$TMP_DIR/out3" ibs=4 obs=4 skip=1 seek=2 count=2 status=none
[[ $(wc -c < "$TMP_DIR/out3") -eq 16 ]]
echo "Test 3 passed: skip and seek block offsets"

# Test 4: conv=ucase
printf "modern dd in c++" | $BLKCP_BIN conv=ucase status=none > "$TMP_DIR/out4"
[[ "$(<"$TMP_DIR/out4")" == "MODERN DD IN C++" ]]
echo "Test 4 passed: conv=ucase"

# Test 5: conv=lcase
printf "UPPER CASE STRING" | $BLKCP_BIN conv=lcase status=none > "$TMP_DIR/out5"
[[ "$(<"$TMP_DIR/out5")" == "upper case string" ]]
echo "Test 5 passed: conv=lcase"

# Test 6: conv=swab (swap adjacent bytes)
printf "123456" | $BLKCP_BIN conv=swab status=none > "$TMP_DIR/out6"
[[ "$(<"$TMP_DIR/out6")" == "214365" ]]
echo "Test 6 passed: conv=swab"

# Test 7: Output records on stderr (status=level check)
STDERR_OUT=$($BLKCP_BIN if="$TMP_DIR/in2" of="$TMP_DIR/out7" bs=1024 count=1 2>&1 >/dev/null)
echo "$STDERR_OUT" | grep -q "1+0 records in"
echo "$STDERR_OUT" | grep -q "1+0 records out"
echo "$STDERR_OUT" | grep -q "1024 bytes"
echo "Test 7 passed: Status report format on stderr"

# Test 8: Large block copying & hash verification
head -c 1048576 /dev/urandom > "$TMP_DIR/random.bin"
ORIG_HASH=$(sha256sum "$TMP_DIR/random.bin" | awk '{print $1}')
$BLKCP_BIN if="$TMP_DIR/random.bin" of="$TMP_DIR/copy.bin" bs=64k status=none
COPY_HASH=$(sha256sum "$TMP_DIR/copy.bin" | awk '{print $1}')
[[ "$ORIG_HASH" == "$COPY_HASH" ]]
echo "Test 8 passed: 1MB block copy bit-exactness"

# Test 9: conv=sync padding with nulls
printf "123" | $BLKCP_BIN ibs=8 conv=sync status=none > "$TMP_DIR/out9"
[[ $(wc -c < "$TMP_DIR/out9") -eq 8 ]]
echo "Test 9 passed: conv=sync padding to ibs"

# Test 10: conv=block and conv=unblock
printf "line1\nline2\n" | $BLKCP_BIN cbs=10 conv=block status=none | $BLKCP_BIN cbs=10 conv=unblock status=none > "$TMP_DIR/out10"
[[ "$(<"$TMP_DIR/out10")" == $'line1\nline2' ]]
echo "Test 10 passed: conv=block and conv=unblock roundtrip"

# Test 11: iflag=count_bytes
head -c 1024 /dev/zero | $BLKCP_BIN iflag=count_bytes count=350 status=none > "$TMP_DIR/out11"
[[ $(wc -c < "$TMP_DIR/out11") -eq 350 ]]
echo "Test 11 passed: iflag=count_bytes"

# Test 12: status=none suppression check
STDERR_NONE=$($BLKCP_BIN if="$TMP_DIR/in2" of="$TMP_DIR/out12" bs=512 count=1 status=none 2>&1 >/dev/null || true)
[[ -z "$STDERR_NONE" ]]
echo "Test 12 passed: status=none total silence on stderr"

# Test 13: conv=autotune dynamic optimization bit-exactness
head -c 2097152 /dev/urandom > "$TMP_DIR/rand_auto.bin"
AUTO_ORIG_HASH=$(sha256sum "$TMP_DIR/rand_auto.bin" | awk '{print $1}')
$BLKCP_BIN if="$TMP_DIR/rand_auto.bin" of="$TMP_DIR/copy_auto.bin" conv=autotune status=none
AUTO_COPY_HASH=$(sha256sum "$TMP_DIR/copy_auto.bin" | awk '{print $1}')
[[ "$AUTO_ORIG_HASH" == "$AUTO_COPY_HASH" ]]
echo "Test 13 passed: conv=autotune bit-exact copy"

# Test 14: bs=auto syntax alias
$BLKCP_BIN if="$TMP_DIR/rand_auto.bin" of="$TMP_DIR/copy_auto2.bin" bs=auto status=none
AUTO_COPY2_HASH=$(sha256sum "$TMP_DIR/copy_auto2.bin" | awk '{print $1}')
[[ "$AUTO_ORIG_HASH" == "$AUTO_COPY2_HASH" ]]
echo "Test 14 passed: bs=auto alias bit-exact copy"

# Test 15: Target Safety Guard against writing to mounted root device
ROOT_BLK=$(df / 2>/dev/null | tail -1 | awk '{print $1}')
if [[ -b "$ROOT_BLK" ]]; then
  SG_OUT=$($BLKCP_BIN if=/dev/zero of="$ROOT_BLK" count=1 2>&1 || true)
  if echo "$SG_OUT" | grep -q "SAFETY GUARD"; then
    echo "Test 15 passed: Target Safety Guard successfully blocked write to mounted root device"
  else
    echo "Test 15 failed: Safety guard did not trigger" >&2
    exit 1
  fi
else
  echo "Test 15 skipped: Root device not a block device in current environment"
fi

# Test 16: On-the-fly streaming SHA-256 computation (conv=sha256)
head -c 4194304 /dev/urandom > "$TMP_DIR/rand_hash.bin"
EXPECTED_SHA=$(sha256sum "$TMP_DIR/rand_hash.bin" | awk '{print $1}')
DD_SHA_OUTPUT=$($BLKCP_BIN if="$TMP_DIR/rand_hash.bin" of="$TMP_DIR/rand_hash_out.bin" conv=sha256 status=none 2>&1)
PARSED_SHA=$(echo "$DD_SHA_OUTPUT" | grep "^sha256:" | awk '{print $2}')
[[ "$EXPECTED_SHA" == "$PARSED_SHA" ]]
# Verify content exactness as well
OUT_SHA=$(sha256sum "$TMP_DIR/rand_hash_out.bin" | awk '{print $1}')
[[ "$EXPECTED_SHA" == "$OUT_SHA" ]]
echo "Test 16 passed: conv=sha256 on-the-fly streaming checksum verification"

# Test 17: Multi-Threaded Async Double-Buffering Pipeline (opt=async)
head -c 8388608 /dev/urandom > "$TMP_DIR/rand_async.bin"
EXPECTED_ASYNC_SHA=$(sha256sum "$TMP_DIR/rand_async.bin" | awk '{print $1}')
DD_ASYNC_OUTPUT=$($BLKCP_BIN if="$TMP_DIR/rand_async.bin" of="$TMP_DIR/rand_async_out.bin" bs=64k opt=async,hash status=none 2>&1)
PARSED_ASYNC_SHA=$(echo "$DD_ASYNC_OUTPUT" | grep "^sha256:" | awk '{print $2}')
[[ "$EXPECTED_ASYNC_SHA" == "$PARSED_ASYNC_SHA" ]]
OUT_ASYNC_SHA=$(sha256sum "$TMP_DIR/rand_async_out.bin" | awk '{print $1}')
[[ "$EXPECTED_ASYNC_SHA" == "$OUT_ASYNC_SHA" ]]
echo "Test 17 passed: Multi-threaded async double-buffering pipeline bit-exactness"

# Test 18: Autotune staging on small transfers (< 64 KB) and exact byte counts
head -c 12345 /dev/urandom > "$TMP_DIR/rand_small.bin"
EXPECTED_SMALL_SHA=$(sha256sum "$TMP_DIR/rand_small.bin" | awk '{print $1}')
$BLKCP_BIN if="$TMP_DIR/rand_small.bin" of="$TMP_DIR/rand_small_out.bin" bs=auto count=12345 iflag=count_bytes status=none
OUT_SMALL_SHA=$(sha256sum "$TMP_DIR/rand_small_out.bin" | awk '{print $1}')
[[ "$EXPECTED_SMALL_SHA" == "$OUT_SMALL_SHA" ]]
ACTUAL_SIZE=$(stat -c %s "$TMP_DIR/rand_small_out.bin")
[[ "$ACTUAL_SIZE" -eq 12345 ]]
echo "Test 18 passed: Autotune staging on small transfers (< 64 KB) with exact byte counting"

# Test 19: Kernel Zero-Copy / Reflink copy (conv=reflink / opt=reflink)
head -c 16777216 /dev/urandom > "$TMP_DIR/rand_reflink.bin"
EXPECTED_REFLINK_SHA=$(sha256sum "$TMP_DIR/rand_reflink.bin" | awk '{print $1}')
$BLKCP_BIN if="$TMP_DIR/rand_reflink.bin" of="$TMP_DIR/rand_reflink_out.bin" conv=reflink status=none
OUT_REFLINK_SHA=$(sha256sum "$TMP_DIR/rand_reflink_out.bin" | awk '{print $1}')
[[ "$EXPECTED_REFLINK_SHA" == "$OUT_REFLINK_SHA" ]]
REFLINK_SIZE=$(stat -c %s "$TMP_DIR/rand_reflink_out.bin")
[[ "$REFLINK_SIZE" -eq 16777216 ]]

# Also verify opt=reflink alias with count and bs
$BLKCP_BIN if="$TMP_DIR/rand_reflink.bin" of="$TMP_DIR/rand_reflink_chunk.bin" opt=reflink bs=1M count=4 status=none
CHUNK_SIZE=$(stat -c %s "$TMP_DIR/rand_reflink_chunk.bin")
[[ "$CHUNK_SIZE" -eq 4194304 ]]
cmp -n 4194304 "$TMP_DIR/rand_reflink.bin" "$TMP_DIR/rand_reflink_chunk.bin"
echo "Test 19 passed: Kernel Zero-Copy copy_file_range/reflink bit-exactness and count chunking"

# Test 20: Arbitrary exact byte count targeting (bytes=N, tocopy=N, tc=N) across sync, auto, async
head -c 5000000 /dev/urandom > "$TMP_DIR/rand_tc.bin"
$BLKCP_BIN if="$TMP_DIR/rand_tc.bin" of="$TMP_DIR/out_tc1.bin" bs=1048576 tc=4529848 status=none
[[ $(stat -c %s "$TMP_DIR/out_tc1.bin") -eq 4529848 ]]
cmp -n 4529848 "$TMP_DIR/rand_tc.bin" "$TMP_DIR/out_tc1.bin"

$BLKCP_BIN if="$TMP_DIR/rand_tc.bin" of="$TMP_DIR/out_tc2.bin" bs=auto tocopy=4529848 status=none
[[ $(stat -c %s "$TMP_DIR/out_tc2.bin") -eq 4529848 ]]
cmp -n 4529848 "$TMP_DIR/rand_tc.bin" "$TMP_DIR/out_tc2.bin"

$BLKCP_BIN if="$TMP_DIR/rand_tc.bin" of="$TMP_DIR/out_tc3.bin" opt=async bytes=4529848 status=none
[[ $(stat -c %s "$TMP_DIR/out_tc3.bin") -eq 4529848 ]]
cmp -n 4529848 "$TMP_DIR/rand_tc.bin" "$TMP_DIR/out_tc3.bin"
echo "Test 20 passed: Arbitrary exact byte limit targeting (bytes=N, tocopy=N, tc=N) with bs=auto, large bs and async"

# Test 21: Modern CLI syntax with io_uring engine (-i, -o, -b, -e uring, --hash)
head -c 8388608 /dev/urandom > "$TMP_DIR/rand_uring.bin"
EXPECTED_URING_SHA=$(sha256sum "$TMP_DIR/rand_uring.bin" | awk '{print $1}')
$BLKCP_BIN -i "$TMP_DIR/rand_uring.bin" -o "$TMP_DIR/rand_uring_out.bin" -b 64K -e uring -q
OUT_URING_SHA=$(sha256sum "$TMP_DIR/rand_uring_out.bin" | awk '{print $1}')
[[ "$EXPECTED_URING_SHA" == "$OUT_URING_SHA" ]]
[[ $(stat -c %s "$TMP_DIR/rand_uring_out.bin") -eq 8388608 ]]
echo "Test 21 passed: Modern CLI syntax with io_uring engine (-i, -o, -b, -e uring, -q)"

# Test 22: Modern CLI exact byte limit (-l / --limit) with io_uring
$BLKCP_BIN -i "$TMP_DIR/rand_uring.bin" -o "$TMP_DIR/rand_uring_limit.bin" -b 128K -e uring -l 3141592 -q
[[ $(stat -c %s "$TMP_DIR/rand_uring_limit.bin") -eq 3141592 ]]
cmp -n 3141592 "$TMP_DIR/rand_uring.bin" "$TMP_DIR/rand_uring_limit.bin"
echo "Test 22 passed: Modern CLI exact byte limit (-l 3141592) with io_uring engine"

# Test 23: Modern CLI Autotuning and Hash flags (--autotune, --hash)
$BLKCP_BIN -i "$TMP_DIR/rand_uring.bin" -o "$TMP_DIR/rand_auto_hash.bin" --autotune -e async --hash -q > "$TMP_DIR/hash_out.txt"
[[ $(stat -c %s "$TMP_DIR/rand_auto_hash.bin") -eq 8388608 ]]
cmp "$TMP_DIR/rand_uring.bin" "$TMP_DIR/rand_auto_hash.bin"
echo "Test 23 passed: Modern CLI flags (--autotune, --hash, -e async)"

# Test 24: Modern CLI positional arguments (blkcp INPUT OUTPUT -e uring)
$BLKCP_BIN "$TMP_DIR/rand_uring.bin" "$TMP_DIR/rand_pos_out.bin" -e uring -b 256K -q
[[ $(stat -c %s "$TMP_DIR/rand_pos_out.bin") -eq 8388608 ]]
cmp "$TMP_DIR/rand_uring.bin" "$TMP_DIR/rand_pos_out.bin"
echo "Test 24 passed: Modern CLI positional arguments (blkcp INPUT OUTPUT -e uring)"

# Test 25: io_uring with input skip and output seek offsets
head -c 2097152 /dev/urandom > "$TMP_DIR/rand_offset.bin"
$BLKCP_BIN -i "$TMP_DIR/rand_offset.bin" -o "$TMP_DIR/rand_offset_out.bin" -b 64K -e uring --skip=65536 --seek=131072 -l 524288 -q
[[ $(stat -c %s "$TMP_DIR/rand_offset_out.bin") -eq 655360 ]]
/usr/bin/dd if="$TMP_DIR/rand_offset.bin" of="$TMP_DIR/expected_slice.bin" bs=65536 skip=1 count=8 status=none
/usr/bin/dd if="$TMP_DIR/rand_offset_out.bin" of="$TMP_DIR/actual_slice.bin" bs=65536 skip=2 count=8 status=none
cmp "$TMP_DIR/expected_slice.bin" "$TMP_DIR/actual_slice.bin"
echo "Test 25 passed: io_uring engine with --skip and --seek offsets"

# Test 26: io_uring streaming from pipe stdin
$BLKCP_BIN -o "$TMP_DIR/rand_pipe_out.bin" -b 64K -e uring -q < "$TMP_DIR/rand_offset.bin"
[[ $(stat -c %s "$TMP_DIR/rand_pipe_out.bin") -eq 2097152 ]]
cmp "$TMP_DIR/rand_offset.bin" "$TMP_DIR/rand_pipe_out.bin"
echo "Test 26 passed: io_uring streaming from stdin pipe"

# Test 27: SIMD AVX2-accelerated conv=swab roundtrip bit-exactness on 4MB stream
head -c 4194304 /dev/urandom > "$TMP_DIR/rand_swab_orig.bin"
$BLKCP_BIN -i "$TMP_DIR/rand_swab_orig.bin" -o "$TMP_DIR/rand_swab_pass1.bin" -b 64K conv=swab -q
$BLKCP_BIN -i "$TMP_DIR/rand_swab_pass1.bin" -o "$TMP_DIR/rand_swab_pass2.bin" -b 64K conv=swab -q
[[ $(stat -c %s "$TMP_DIR/rand_swab_pass2.bin") -eq 4194304 ]]
cmp "$TMP_DIR/rand_swab_orig.bin" "$TMP_DIR/rand_swab_pass2.bin"
echo "Test 27 passed: SIMD AVX2-accelerated conv=swab 4MB roundtrip bit-exactness"

# Test 28: Direct I/O (--direct) with unaligned byte size across sync and io_uring
head -c 123456 /dev/urandom > "$TMP_DIR/rand_direct_in.bin"
$BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -o "$TMP_DIR/rand_direct_sync.bin" --direct -b 64K -q
[[ $(stat -c %s "$TMP_DIR/rand_direct_sync.bin") -eq 123456 ]]
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_direct_sync.bin"

$BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -o "$TMP_DIR/rand_direct_uring.bin" --direct -e uring -b 64K -q
[[ $(stat -c %s "$TMP_DIR/rand_direct_uring.bin") -eq 123456 ]]
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_direct_uring.bin"
echo "Test 28 passed: Direct I/O (--direct) unaligned tail handling and bit-exactness"

# Test 29: Machine-readable NDJSON telemetry (--json and status=json)
JSON_OUT=$($BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -o "$TMP_DIR/rand_json_out.bin" --json --hash 2>&1)
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_json_out.bin"
python3 -c "
import json, sys
data = json.loads('''$JSON_OUT''')
assert data['event'] == 'finished'
assert data['copied_bytes'] == 123456
assert data['records_in']['full'] >= 0
assert 'avg_speed_bps' in data
assert len(data['sha256']) == 64
"
echo "Test 29 passed: Machine-readable NDJSON telemetry (--json and status=json)"

echo "=== All 29 extended tests passed successfully! ==="
