#!/usr/bin/env bash
set -euo pipefail

DD_BIN="./dd"
TMP_DIR=$(mktemp -d -t dd_test_XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

echo "=== Running dd Compatibility Test Suite ==="

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
# in: "0123456789ABCDEF" -> skip 4 bytes: "456789AB"
# out: seek 2 blocks (8 bytes zero-filled) then "456789AB"
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

echo "=== All 8 tests passed successfully! ==="
