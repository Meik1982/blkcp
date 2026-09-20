#!/usr/bin/env bash
set -euo pipefail

BLKCP_BIN="./blkcp"
if [[ ! -x "$BLKCP_BIN" ]]; then
  echo "Error: Binary '$BLKCP_BIN' not found. Run 'make' first." >&2
  exit 1
fi
TMP_DIR=$(mktemp -d -t blkcp_test_XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

echo "=== Running Modern blkcp Test Suite ==="

# Test 1: Basic stdin to stdout
printf "hello world" | $BLKCP_BIN -q > "$TMP_DIR/out1"
[[ "$(<"$TMP_DIR/out1")" == "hello world" ]]
echo "Test 1 passed: Basic stdin -> stdout"

# Test 2: -i and -o with -b and -c
head -c 2048 /dev/zero > "$TMP_DIR/in2"
$BLKCP_BIN -i "$TMP_DIR/in2" -o "$TMP_DIR/out2" -b 512 -c 2 -q
[[ $(wc -c < "$TMP_DIR/out2") -eq 1024 ]]
echo "Test 2 passed: -i and -o with -b 512 -c 2"

# Test 3: --skip and --seek offsets
printf "0123456789ABCDEF" > "$TMP_DIR/in3"
$BLKCP_BIN -i "$TMP_DIR/in3" -o "$TMP_DIR/out3" -b 4 --skip 4 --seek 8 -c 2 -q
[[ $(wc -c < "$TMP_DIR/out3") -eq 16 ]]
echo "Test 3 passed: --skip and --seek byte offsets"

# Test 4: Filenames containing '=' characters (unambiguous positional operands)
printf "payload with equals sign" > "$TMP_DIR/file=with=equals.bin"
$BLKCP_BIN "$TMP_DIR/file=with=equals.bin" "$TMP_DIR/out=with=equals.bin" -q
[[ "$(<"$TMP_DIR/out=with=equals.bin")" == "payload with equals sign" ]]
echo "Test 4 passed: Positional arguments with '=' characters in filenames"

# Test 5: Exact byte limit (-l / --limit)
head -c 4096 /dev/urandom > "$TMP_DIR/in5"
$BLKCP_BIN -i "$TMP_DIR/in5" -o "$TMP_DIR/out5" -b 512 -l 1234 -q
[[ $(wc -c < "$TMP_DIR/out5") -eq 1234 ]]
cmp -n 1234 "$TMP_DIR/in5" "$TMP_DIR/out5"
echo "Test 5 passed: Exact byte limit (-l 1234)"

# Test 6: --swab (AVX2-accelerated byte-pair swap)
printf "123456" | $BLKCP_BIN --swab -q > "$TMP_DIR/out6"
[[ "$(<"$TMP_DIR/out6")" == "214365" ]]
echo "Test 6 passed: --swab byte-pair swapping"

# Test 7: Output telemetry on stderr (default verbosity)
STDERR_OUT=$($BLKCP_BIN -i "$TMP_DIR/in2" -o "$TMP_DIR/out7" -b 1024 -c 1 2>&1 >/dev/null)
echo "$STDERR_OUT" | grep -q "1+0 records in"
echo "$STDERR_OUT" | grep -q "1+0 records out"
echo "$STDERR_OUT" | grep -q "1024 bytes"
echo "Test 7 passed: Status report format on stderr"

# Test 8: Large block copying & hash verification
head -c 1048576 /dev/urandom > "$TMP_DIR/random.bin"
ORIG_HASH=$(sha256sum "$TMP_DIR/random.bin" | awk '{print $1}')
$BLKCP_BIN -i "$TMP_DIR/random.bin" -o "$TMP_DIR/copy.bin" -b 64K -q
COPY_HASH=$(sha256sum "$TMP_DIR/copy.bin" | awk '{print $1}')
[[ "$ORIG_HASH" == "$COPY_HASH" ]]
echo "Test 8 passed: 1MB block copy bit-exactness"

# Test 9: --sync padding with nulls
printf "123" | $BLKCP_BIN -b 8 --sync -q > "$TMP_DIR/out9"
[[ $(wc -c < "$TMP_DIR/out9") -eq 8 ]]
echo "Test 9 passed: --sync padding to block size"

# Test 10: Positional arguments (blkcp INPUT OUTPUT)
$BLKCP_BIN "$TMP_DIR/random.bin" "$TMP_DIR/pos_copy.bin" -b 128K -q
cmp "$TMP_DIR/random.bin" "$TMP_DIR/pos_copy.bin"
echo "Test 10 passed: Pure positional arguments (blkcp INPUT OUTPUT)"

# Test 11: Exact byte count limit (-l)
head -c 1024 /dev/zero | $BLKCP_BIN -l 350 -q > "$TMP_DIR/out11"
[[ $(wc -c < "$TMP_DIR/out11") -eq 350 ]]
echo "Test 11 passed: Exact byte count limit (-l 350)"

# Test 12: -q / --quiet total silence check
STDERR_NONE=$($BLKCP_BIN -i "$TMP_DIR/in2" -o "$TMP_DIR/out12" -b 512 -c 1 -q 2>&1 >/dev/null || true)
[[ -z "$STDERR_NONE" ]]
echo "Test 12 passed: -q / --quiet total silence on stderr"

# Test 13: --autotune dynamic optimization bit-exactness
head -c 2097152 /dev/urandom > "$TMP_DIR/rand_auto.bin"
AUTO_ORIG_HASH=$(sha256sum "$TMP_DIR/rand_auto.bin" | awk '{print $1}')
$BLKCP_BIN -i "$TMP_DIR/rand_auto.bin" -o "$TMP_DIR/copy_auto.bin" --autotune -q
AUTO_COPY_HASH=$(sha256sum "$TMP_DIR/copy_auto.bin" | awk '{print $1}')
[[ "$AUTO_ORIG_HASH" == "$AUTO_COPY_HASH" ]]
echo "Test 13 passed: --autotune bit-exact copy"

# Test 14: -b auto alias
$BLKCP_BIN -i "$TMP_DIR/rand_auto.bin" -o "$TMP_DIR/copy_auto2.bin" -b auto -q
AUTO_COPY2_HASH=$(sha256sum "$TMP_DIR/copy_auto2.bin" | awk '{print $1}')
[[ "$AUTO_ORIG_HASH" == "$AUTO_COPY2_HASH" ]]
echo "Test 14 passed: -b auto alias bit-exact copy"

# Test 15: Target Safety Guard against writing to mounted root device
ROOT_BLK=$(df / 2>/dev/null | tail -1 | awk '{print $1}')
if [[ -b "$ROOT_BLK" ]]; then
  SG_OUT=$($BLKCP_BIN -i /dev/zero -o "$ROOT_BLK" -c 1 2>&1 || true)
  if echo "$SG_OUT" | grep -q "SAFETY GUARD"; then
    echo "Test 15 passed: Target Safety Guard successfully blocked write to mounted root device"
  else
    echo "Test 15 failed: Safety guard did not trigger" >&2
    exit 1
  fi
else
  echo "Test 15 skipped: Root device not a block device in current environment"
fi

# Test 16: On-the-fly streaming SHA-256 computation (--hash / --sha256)
head -c 4194304 /dev/urandom > "$TMP_DIR/rand_hash.bin"
EXPECTED_SHA=$(sha256sum "$TMP_DIR/rand_hash.bin" | awk '{print $1}')
DD_SHA_OUTPUT=$($BLKCP_BIN -i "$TMP_DIR/rand_hash.bin" -o "$TMP_DIR/rand_hash_out.bin" --hash -q 2>&1)
PARSED_SHA=$(echo "$DD_SHA_OUTPUT" | grep "^sha256:" | awk '{print $2}')
[[ "$EXPECTED_SHA" == "$PARSED_SHA" ]]
OUT_SHA=$(sha256sum "$TMP_DIR/rand_hash_out.bin" | awk '{print $1}')
[[ "$EXPECTED_SHA" == "$OUT_SHA" ]]
echo "Test 16 passed: --hash on-the-fly streaming checksum verification"

# Test 17: Multi-Threaded Async Double-Buffering Pipeline (-e async)
head -c 8388608 /dev/urandom > "$TMP_DIR/rand_async.bin"
EXPECTED_ASYNC_SHA=$(sha256sum "$TMP_DIR/rand_async.bin" | awk '{print $1}')
DD_ASYNC_OUTPUT=$($BLKCP_BIN -i "$TMP_DIR/rand_async.bin" -o "$TMP_DIR/rand_async_out.bin" -b 64K -e async --hash -q 2>&1)
PARSED_ASYNC_SHA=$(echo "$DD_ASYNC_OUTPUT" | grep "^sha256:" | awk '{print $2}')
[[ "$EXPECTED_ASYNC_SHA" == "$PARSED_ASYNC_SHA" ]]
OUT_ASYNC_SHA=$(sha256sum "$TMP_DIR/rand_async_out.bin" | awk '{print $1}')
[[ "$EXPECTED_ASYNC_SHA" == "$OUT_ASYNC_SHA" ]]
echo "Test 17 passed: Multi-threaded async double-buffering pipeline bit-exactness"

# Test 18: Autotune staging on small transfers (< 64 KB) and exact byte counts
head -c 12345 /dev/urandom > "$TMP_DIR/rand_small.bin"
EXPECTED_SMALL_SHA=$(sha256sum "$TMP_DIR/rand_small.bin" | awk '{print $1}')
$BLKCP_BIN -i "$TMP_DIR/rand_small.bin" -o "$TMP_DIR/rand_small_out.bin" -b auto -l 12345 -q
OUT_SMALL_SHA=$(sha256sum "$TMP_DIR/rand_small_out.bin" | awk '{print $1}')
[[ "$EXPECTED_SMALL_SHA" == "$OUT_SMALL_SHA" ]]
ACTUAL_SIZE=$(stat -c %s "$TMP_DIR/rand_small_out.bin")
[[ "$ACTUAL_SIZE" -eq 12345 ]]
echo "Test 18 passed: Autotune staging on small transfers (< 64 KB) with exact byte counting"

# Test 19: Kernel Zero-Copy Reflink copy (-e reflink)
head -c 16777216 /dev/urandom > "$TMP_DIR/rand_reflink.bin"
EXPECTED_REFLINK_SHA=$(sha256sum "$TMP_DIR/rand_reflink.bin" | awk '{print $1}')
$BLKCP_BIN -i "$TMP_DIR/rand_reflink.bin" -o "$TMP_DIR/rand_reflink_out.bin" -e reflink -q
OUT_REFLINK_SHA=$(sha256sum "$TMP_DIR/rand_reflink_out.bin" | awk '{print $1}')
[[ "$EXPECTED_REFLINK_SHA" == "$OUT_REFLINK_SHA" ]]
REFLINK_SIZE=$(stat -c %s "$TMP_DIR/rand_reflink_out.bin")
[[ "$REFLINK_SIZE" -eq 16777216 ]]

# Also verify -e reflink with limit
$BLKCP_BIN -i "$TMP_DIR/rand_reflink.bin" -o "$TMP_DIR/rand_reflink_chunk.bin" -e reflink -l 4194304 -q
CHUNK_SIZE=$(stat -c %s "$TMP_DIR/rand_reflink_chunk.bin")
[[ "$CHUNK_SIZE" -eq 4194304 ]]
cmp -n 4194304 "$TMP_DIR/rand_reflink.bin" "$TMP_DIR/rand_reflink_chunk.bin"
echo "Test 19 passed: Kernel Zero-Copy copy_file_range/reflink bit-exactness and limit chunking"

# Test 20: Arbitrary exact byte count targeting (-l / --limit) across sync, auto, async
head -c 5000000 /dev/urandom > "$TMP_DIR/rand_tc.bin"
$BLKCP_BIN -i "$TMP_DIR/rand_tc.bin" -o "$TMP_DIR/out_tc1.bin" -b 1048576 -l 4529848 -q
[[ $(stat -c %s "$TMP_DIR/out_tc1.bin") -eq 4529848 ]]
cmp -n 4529848 "$TMP_DIR/rand_tc.bin" "$TMP_DIR/out_tc1.bin"

$BLKCP_BIN -i "$TMP_DIR/rand_tc.bin" -o "$TMP_DIR/out_tc2.bin" -b auto -l 4529848 -q
[[ $(stat -c %s "$TMP_DIR/out_tc2.bin") -eq 4529848 ]]
cmp -n 4529848 "$TMP_DIR/rand_tc.bin" "$TMP_DIR/out_tc2.bin"

$BLKCP_BIN -i "$TMP_DIR/rand_tc.bin" -o "$TMP_DIR/out_tc3.bin" -e async -l 4529848 -q
[[ $(stat -c %s "$TMP_DIR/out_tc3.bin") -eq 4529848 ]]
cmp -n 4529848 "$TMP_DIR/rand_tc.bin" "$TMP_DIR/out_tc3.bin"
echo "Test 20 passed: Arbitrary exact byte limit targeting (-l) with bs=auto, large bs and async"

# Test 21: Modern CLI syntax with io_uring engine (-i, -o, -b, -e uring, -q)
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
head -c 589824 "$TMP_DIR/rand_offset.bin" | tail -c 524288 > "$TMP_DIR/expected_slice.bin"
tail -c +131073 "$TMP_DIR/rand_offset_out.bin" | head -c 524288 > "$TMP_DIR/actual_slice.bin"
cmp "$TMP_DIR/expected_slice.bin" "$TMP_DIR/actual_slice.bin"
echo "Test 25 passed: io_uring engine with --skip and --seek offsets"

# Test 26: io_uring streaming from pipe stdin
$BLKCP_BIN -o "$TMP_DIR/rand_pipe_out.bin" -b 64K -e uring -q < "$TMP_DIR/rand_offset.bin"
[[ $(stat -c %s "$TMP_DIR/rand_pipe_out.bin") -eq 2097152 ]]
cmp "$TMP_DIR/rand_offset.bin" "$TMP_DIR/rand_pipe_out.bin"
echo "Test 26 passed: io_uring streaming from stdin pipe"

# Test 27: SIMD AVX2-accelerated --swab roundtrip bit-exactness on 4MB stream
head -c 4194304 /dev/urandom > "$TMP_DIR/rand_swab_orig.bin"
$BLKCP_BIN -i "$TMP_DIR/rand_swab_orig.bin" -o "$TMP_DIR/rand_swab_pass1.bin" -b 64K --swab -q
$BLKCP_BIN -i "$TMP_DIR/rand_swab_pass1.bin" -o "$TMP_DIR/rand_swab_pass2.bin" -b 64K --swab -q
[[ $(stat -c %s "$TMP_DIR/rand_swab_pass2.bin") -eq 4194304 ]]
cmp "$TMP_DIR/rand_swab_orig.bin" "$TMP_DIR/rand_swab_pass2.bin"
echo "Test 27 passed: SIMD AVX2-accelerated --swab 4MB roundtrip bit-exactness"

# Test 28: Direct I/O (--direct) with unaligned byte size across sync and io_uring
head -c 123456 /dev/urandom > "$TMP_DIR/rand_direct_in.bin"
$BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -o "$TMP_DIR/rand_direct_sync.bin" --direct -b 64K -q
[[ $(stat -c %s "$TMP_DIR/rand_direct_sync.bin") -eq 123456 ]]
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_direct_sync.bin"

$BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -o "$TMP_DIR/rand_direct_uring.bin" --direct -e uring -b 64K -q
[[ $(stat -c %s "$TMP_DIR/rand_direct_uring.bin") -eq 123456 ]]
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_direct_uring.bin"
echo "Test 28 passed: Direct I/O (--direct) unaligned tail handling and bit-exactness"

# Test 29: Machine-readable NDJSON telemetry (--json)
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
echo "Test 29 passed: Machine-readable NDJSON telemetry (--json)"

# Test 30: Dynamic Ringbuffer Scaling and custom queue depth in io_async (--queue-depth)
JSON_ASYNC=$($BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -o "$TMP_DIR/rand_async_out.bin" -e async --queue-depth=16 -b 4K --json --hash 2>&1)
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_async_out.bin"
python3 -c "
import json, sys
data = json.loads('''$JSON_ASYNC''')
assert data['event'] == 'finished'
assert data['copied_bytes'] == 123456
assert data['pipeline']['capacity'] == 16
assert 'reader_stalls' in data['pipeline']
assert 'writer_stalls' in data['pipeline']
"

# Test 30b: Auto dynamic scaling for async ringbuffer
JSON_AUTO_ASYNC=$($BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -o "$TMP_DIR/rand_auto_async_out.bin" -e async -b 4K --json 2>&1)
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_auto_async_out.bin"
python3 -c "
import json, sys
data = json.loads('''$JSON_AUTO_ASYNC''')
assert data['pipeline']['capacity'] == 128
"
echo "Test 30 passed: Dynamic Ringbuffer Scaling and custom queue depth in io_async (--queue-depth)"

# Test 31: Kernel-Level Zero-Copy Splice Engine (splice(2))
# 31a: Pipe-to-File
cat "$TMP_DIR/rand_direct_in.bin" | $BLKCP_BIN -o "$TMP_DIR/rand_splice_p2f.bin" -e splice -q
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_splice_p2f.bin"

# 31b: File-to-Pipe
$BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -e splice -q | cat > "$TMP_DIR/rand_splice_f2p.bin"
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_splice_f2p.bin"

# 31c: Double-Splice File-to-File
$BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -o "$TMP_DIR/rand_splice_f2f.bin" -e splice -q
cmp "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_splice_f2f.bin"

# 31d: Splice with exact byte limit (-l 54321)
$BLKCP_BIN -i "$TMP_DIR/rand_direct_in.bin" -o "$TMP_DIR/rand_splice_limit.bin" -e splice -l 54321 -q
[[ $(stat -c %s "$TMP_DIR/rand_splice_limit.bin") -eq 54321 ]]
cmp -n 54321 "$TMP_DIR/rand_direct_in.bin" "$TMP_DIR/rand_splice_limit.bin"
echo "Test 31 passed: Kernel-Level Zero-Copy Splice Engine (splice(2)) for pipes and files"

echo "=== All 31 modern blkcp tests passed successfully! ==="
