#!/usr/bin/env bash
set -euo pipefail

DD_BIN="./dd"
TMP_DIR=$(mktemp -d -t dd_test_XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

echo "=== Running Extended dd Compatibility Test Suite ==="

# Test 1: Basic stdin to stdout
printf "hello world" | $DD_BIN status=none > "$TMP_DIR/out1"
[[ "$(<"$TMP_DIR/out1")" == "hello world" ]]
echo "Test 1 passed: Basic stdin -> stdout"

# Test 2: if and of with bs and count
head -c 2048 /dev/zero > "$TMP_DIR/in2"
$DD_BIN if="$TMP_DIR/in2" of="$TMP_DIR/out2" bs=512 count=2 status=none
[[ $(wc -c < "$TMP_DIR/out2") -eq 1024 ]]
echo "Test 2 passed: if/of with bs=512 count=2"

# Test 3: skip and seek
printf "0123456789ABCDEF" > "$TMP_DIR/in3"
$DD_BIN if="$TMP_DIR/in3" of="$TMP_DIR/out3" ibs=4 obs=4 skip=1 seek=2 count=2 status=none
[[ $(wc -c < "$TMP_DIR/out3") -eq 16 ]]
echo "Test 3 passed: skip and seek block offsets"

# Test 4: conv=ucase
printf "modern dd in c++" | $DD_BIN conv=ucase status=none > "$TMP_DIR/out4"
[[ "$(<"$TMP_DIR/out4")" == "MODERN DD IN C++" ]]
echo "Test 4 passed: conv=ucase"

# Test 5: conv=lcase
printf "UPPER CASE STRING" | $DD_BIN conv=lcase status=none > "$TMP_DIR/out5"
[[ "$(<"$TMP_DIR/out5")" == "upper case string" ]]
echo "Test 5 passed: conv=lcase"

# Test 6: conv=swab (swap adjacent bytes)
printf "123456" | $DD_BIN conv=swab status=none > "$TMP_DIR/out6"
[[ "$(<"$TMP_DIR/out6")" == "214365" ]]
echo "Test 6 passed: conv=swab"

# Test 7: Output records on stderr (status=level check)
STDERR_OUT=$($DD_BIN if="$TMP_DIR/in2" of="$TMP_DIR/out7" bs=1024 count=1 2>&1 >/dev/null)
echo "$STDERR_OUT" | grep -q "1+0 records in"
echo "$STDERR_OUT" | grep -q "1+0 records out"
echo "$STDERR_OUT" | grep -q "1024 bytes"
echo "Test 7 passed: Status report format on stderr"

# Test 8: Large block copying & hash verification
head -c 1048576 /dev/urandom > "$TMP_DIR/random.bin"
ORIG_HASH=$(sha256sum "$TMP_DIR/random.bin" | awk '{print $1}')
$DD_BIN if="$TMP_DIR/random.bin" of="$TMP_DIR/copy.bin" bs=64k status=none
COPY_HASH=$(sha256sum "$TMP_DIR/copy.bin" | awk '{print $1}')
[[ "$ORIG_HASH" == "$COPY_HASH" ]]
echo "Test 8 passed: 1MB block copy bit-exactness"

# Test 9: conv=sync padding with nulls
printf "123" | $DD_BIN ibs=8 conv=sync status=none > "$TMP_DIR/out9"
[[ $(wc -c < "$TMP_DIR/out9") -eq 8 ]]
echo "Test 9 passed: conv=sync padding to ibs"

# Test 10: conv=block and conv=unblock
printf "line1\nline2\n" | $DD_BIN cbs=10 conv=block status=none | $DD_BIN cbs=10 conv=unblock status=none > "$TMP_DIR/out10"
[[ "$(<"$TMP_DIR/out10")" == $'line1\nline2' ]]
echo "Test 10 passed: conv=block and conv=unblock roundtrip"

# Test 11: iflag=count_bytes
head -c 1024 /dev/zero | $DD_BIN iflag=count_bytes count=350 status=none > "$TMP_DIR/out11"
[[ $(wc -c < "$TMP_DIR/out11") -eq 350 ]]
echo "Test 11 passed: iflag=count_bytes"

# Test 12: status=none suppression check
STDERR_NONE=$($DD_BIN if="$TMP_DIR/in2" of="$TMP_DIR/out12" bs=512 count=1 status=none 2>&1 >/dev/null || true)
[[ -z "$STDERR_NONE" ]]
echo "Test 12 passed: status=none total silence on stderr"

# Test 13: conv=autotune dynamic optimization bit-exactness
head -c 2097152 /dev/urandom > "$TMP_DIR/rand_auto.bin"
AUTO_ORIG_HASH=$(sha256sum "$TMP_DIR/rand_auto.bin" | awk '{print $1}')
$DD_BIN if="$TMP_DIR/rand_auto.bin" of="$TMP_DIR/copy_auto.bin" conv=autotune status=none
AUTO_COPY_HASH=$(sha256sum "$TMP_DIR/copy_auto.bin" | awk '{print $1}')
[[ "$AUTO_ORIG_HASH" == "$AUTO_COPY_HASH" ]]
echo "Test 13 passed: conv=autotune bit-exact copy"

# Test 14: bs=auto syntax alias
$DD_BIN if="$TMP_DIR/rand_auto.bin" of="$TMP_DIR/copy_auto2.bin" bs=auto status=none
AUTO_COPY2_HASH=$(sha256sum "$TMP_DIR/copy_auto2.bin" | awk '{print $1}')
[[ "$AUTO_ORIG_HASH" == "$AUTO_COPY2_HASH" ]]
echo "Test 14 passed: bs=auto alias bit-exact copy"

# Test 15: Target Safety Guard against writing to mounted root device
ROOT_BLK=$(df / 2>/dev/null | tail -1 | awk '{print $1}')
if [[ -b "$ROOT_BLK" ]]; then
  SG_OUT=$($DD_BIN if=/dev/zero of="$ROOT_BLK" count=1 2>&1 || true)
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
DD_SHA_OUTPUT=$($DD_BIN if="$TMP_DIR/rand_hash.bin" of="$TMP_DIR/rand_hash_out.bin" conv=sha256 status=none 2>&1)
PARSED_SHA=$(echo "$DD_SHA_OUTPUT" | grep "^sha256:" | awk '{print $2}')
[[ "$EXPECTED_SHA" == "$PARSED_SHA" ]]
# Verify content exactness as well
OUT_SHA=$(sha256sum "$TMP_DIR/rand_hash_out.bin" | awk '{print $1}')
[[ "$EXPECTED_SHA" == "$OUT_SHA" ]]
echo "Test 16 passed: conv=sha256 on-the-fly streaming checksum verification"

# Test 17: Multi-Threaded Async Double-Buffering Pipeline (opt=async)
head -c 8388608 /dev/urandom > "$TMP_DIR/rand_async.bin"
EXPECTED_ASYNC_SHA=$(sha256sum "$TMP_DIR/rand_async.bin" | awk '{print $1}')
DD_ASYNC_OUTPUT=$($DD_BIN if="$TMP_DIR/rand_async.bin" of="$TMP_DIR/rand_async_out.bin" bs=64k opt=async,hash status=none 2>&1)
PARSED_ASYNC_SHA=$(echo "$DD_ASYNC_OUTPUT" | grep "^sha256:" | awk '{print $2}')
[[ "$EXPECTED_ASYNC_SHA" == "$PARSED_ASYNC_SHA" ]]
OUT_ASYNC_SHA=$(sha256sum "$TMP_DIR/rand_async_out.bin" | awk '{print $1}')
[[ "$EXPECTED_ASYNC_SHA" == "$OUT_ASYNC_SHA" ]]
echo "Test 17 passed: Multi-threaded async double-buffering pipeline bit-exactness"

echo "=== All 17 extended tests passed successfully! ==="
