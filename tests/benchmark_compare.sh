#!/usr/bin/env bash
# Benchmark comparison script: blkcp (optimized) vs System /usr/bin/dd
set -euo pipefail
export LC_ALL=C

LOCAL_BLKCP="./blkcp"
SYSTEM_DD="/usr/bin/dd"

if [[ ! -x "$LOCAL_BLKCP" ]]; then
  echo "Error: Local binary '$LOCAL_BLKCP' not found. Run 'make release' first." >&2
  exit 1
fi

if [[ ! -x "$SYSTEM_DD" ]]; then
  echo "Error: System binary '$SYSTEM_DD' not found." >&2
  exit 1
fi

TMP_DIR=$(mktemp -d -t blkcp_bench_XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

echo "======================================================================"
echo "         Performance Benchmark: blkcp vs. System dd                   "
echo "======================================================================"
echo "Local Binary:  $LOCAL_BLKCP ($(ls -lh "$LOCAL_BLKCP" | awk '{print $5}'))"
echo "System Binary: $SYSTEM_DD ($(ls -lh "$SYSTEM_DD" | awk '{print $5}'))"
echo "Date:          $(date '+%Y-%m-%d %H:%M:%S')"
echo "Host:          $(uname -sr) on $(uname -m)"
echo "======================================================================"
printf "\n"

# Helper to run a command and extract throughput in GB/s (or MB/s normalized to GB/s)
measure_speed() {
  local out
  out=$("$@" 2>&1 || true)
  
  # Extract throughput line (e.g. "... copied, 0.123 s, 18.5 GB/s" or "... MB/s")
  local speed_line
  speed_line=$(echo "$out" | grep -E "copied|kopiert" | tail -n 1)
  
  if [[ -z "$speed_line" ]]; then
    echo "0"
    return
  fi

  # Parse speed value and unit from end of string
  local val unit
  val=$(echo "$speed_line" | awk -F', ' '{print $NF}' | awk '{print $1}' | tr ',' '.')
  unit=$(echo "$speed_line" | awk -F', ' '{print $NF}' | awk '{print $2}')
  
  if [[ -z "$val" || -z "$unit" ]]; then
    echo "0"
    return
  fi

  # Normalize all values to GB/s
  case "$unit" in
    *TB/s*|*TiB/s*)
      awk -v v="$val" 'BEGIN { printf "%.2f", v * 1024 }'
      ;;
    *GB/s*|*GiB/s*)
      awk -v v="$val" 'BEGIN { printf "%.2f", v }'
      ;;
    *MB/s*|*MiB/s*)
      awk -v v="$val" 'BEGIN { printf "%.3f", v / 1024 }'
      ;;
    *kB/s*|*KiB/s*|*KB/s*)
      awk -v v="$val" 'BEGIN { printf "%.4f", v / (1024 * 1024) }'
      ;;
    *)
      awk -v v="$val" 'BEGIN { printf "%.2f", v }'
      ;;
  esac
}

# Run standard benchmark test where both binaries take identical CLI arguments
run_test() {
  local title="$1"
  shift
  local args=("$@")

  printf "%-36s : " "$title"

  local sys_best=0
  local loc_best=0

  for i in {1..3}; do
    local s_speed l_speed
    s_speed=$(measure_speed "$SYSTEM_DD" "${args[@]}")
    l_speed=$(measure_speed "$LOCAL_BLKCP" "${args[@]}")

    sys_best=$(awk -v a="$sys_best" -v b="$s_speed" 'BEGIN { print (a > b ? a : b) }')
    loc_best=$(awk -v a="$loc_best" -v b="$l_speed" 'BEGIN { print (a > b ? a : b) }')
  done

  local delta
  delta=$(awk -v l="$loc_best" -v s="$sys_best" 'BEGIN {
    if (s > 0) {
      d = ((l - s) / s) * 100;
      if (d >= 0) printf "+%.1f%%", d;
      else printf "%.1f%%", d;
    } else {
      print "N/A";
    }
  }')

  printf "System: %6.2f GB/s  |  Local: %6.2f GB/s  |  Delta: %7s\n" "$sys_best" "$loc_best" "$delta"
}

# Run asymmetric benchmark test comparing feature-optimized local run against system baseline
run_custom_test() {
  local title="$1"
  shift
  local sys_args=()
  local loc_args=()

  # Split arguments separated by the marker "VS"
  local seen_vs=false
  for arg in "$@"; do
    if [[ "$arg" == "VS" ]]; then
      seen_vs=true
      continue
    fi
    if ! $seen_vs; then
      sys_args+=("$arg")
    else
      loc_args+=("$arg")
    fi
  done

  printf "%-36s : " "$title"

  local sys_best=0
  local loc_best=0

  for i in {1..3}; do
    local s_speed l_speed
    s_speed=$(measure_speed "$SYSTEM_DD" "${sys_args[@]}")
    l_speed=$(measure_speed "$LOCAL_BLKCP" "${loc_args[@]}")

    sys_best=$(awk -v a="$sys_best" -v b="$s_speed" 'BEGIN { print (a > b ? a : b) }')
    loc_best=$(awk -v a="$loc_best" -v b="$l_speed" 'BEGIN { print (a > b ? a : b) }')
  done

  local delta
  delta=$(awk -v l="$loc_best" -v s="$sys_best" 'BEGIN {
    if (s > 0) {
      d = ((l - s) / s) * 100;
      if (d >= 0) printf "+%.1f%%", d;
      else printf "%.1f%%", d;
    } else {
      print "N/A";
    }
  }')

  printf "System: %6.2f GB/s  |  Local: %6.2f GB/s  |  Delta: %7s\n" "$sys_best" "$loc_best" "$delta"
}

# Prepare test data for file-to-file benchmark
head -c 300M /dev/zero > "$TMP_DIR/bench_in.bin"

echo "--- 1. POSIX Standard Comparison Benchmarks ---"
run_test "1. Stream bs=1M (2GB)"        if=/dev/zero of=/dev/null bs=1M count=2000 status=progress
run_test "2. Stream bs=64k (3.2GB)"     if=/dev/zero of=/dev/null bs=64k count=50000 status=progress
run_test "3. Two-Buf ibs=64k obs=64k"   if=/dev/zero of=/dev/null ibs=64k obs=64k count=50000 status=progress
run_test "4. Small Block bs=4k (400MB)" if=/dev/zero of=/dev/null bs=4k count=100000 status=progress
run_test "5. Sparse conv=sparse (2GB)"  if=/dev/zero of=/dev/null bs=64k count=30000 conv=sparse status=progress
run_test "6. File-to-File (300MB)"      if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_out.bin" bs=64k status=progress
run_test "7. Conversion conv=ucase"     if=/dev/zero of=/dev/null bs=64k count=20000 conv=ucase status=progress

printf "\n"
echo "--- 2. Modern Subsystem & Optimizer Benchmarks ---"
# Test 8: Compare autotuning against unoptimized default (bs=512)
run_custom_test "8. Autotune (bs=auto vs def 512)" \
  if="$TMP_DIR/bench_in.bin" of=/dev/null status=progress \
  VS \
  if="$TMP_DIR/bench_in.bin" of=/dev/null bs=auto status=progress

# Test 9: Async Double-Buffering on File-to-File transfer
run_custom_test "9. Async Double-Buffer (opt=async)" \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_sys_async.bin" bs=64k status=progress \
  VS \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_loc_async.bin" bs=64k opt=async status=progress

# Test 10: In-Kernel Zero-Copy (copy_file_range vs POSIX copy)
run_custom_test "10. Kernel Zero-Copy (conv=reflink)" \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_sys_out.bin" bs=1M status=progress \
  VS \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_loc_out.bin" conv=reflink status=progress

# Test 11: Arbitrary Byte Count targeting
run_custom_test "11. Exact Byte Target (tc=1G)" \
  if=/dev/zero of=/dev/null bs=1M count=1073741824 iflag=count_bytes status=progress \
  VS \
  if=/dev/zero of=/dev/null bs=auto tc=1G status=progress

# Test 12: In-Flight SHA256 Streaming Integrity Checksum
run_custom_test "12. In-Flight SHA256 (conv=sha256)" \
  if=/dev/zero of=/dev/null bs=64k count=30000 status=progress \
  VS \
  if=/dev/zero of=/dev/null bs=64k count=30000 conv=sha256 status=progress

# Test 13: io_uring Asynchronous Kernel Engine (-e uring vs standard sync)
run_custom_test "13. io_uring Engine (-e uring)" \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_sys_uring.bin" bs=1M status=progress \
  VS \
  -i "$TMP_DIR/bench_in.bin" -o "$TMP_DIR/bench_loc_uring.bin" -b 1M -e uring -p

echo "======================================================================"
echo "Benchmark completed successfully."
