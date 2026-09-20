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

# Helper to run a command and extract throughput in GB/s
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

# Run asymmetric benchmark test comparing blkcp modern syntax against system dd
run_bench() {
  local title="$1"
  shift
  local sys_args=()
  local loc_args=()

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

echo "--- 1. Baseline & High-Throughput Stream Benchmarks ---"
run_bench "1. Stream bs=1M (2GB)" \
  if=/dev/zero of=/dev/null bs=1M count=2000 status=progress \
  VS \
  -i /dev/zero -o /dev/null -b 1M -c 2000 -p

run_bench "2. Stream bs=64k (3.2GB)" \
  if=/dev/zero of=/dev/null bs=64k count=50000 status=progress \
  VS \
  -i /dev/zero -o /dev/null -b 64K -c 50000 -p

run_bench "3. Small Block bs=4k (400MB)" \
  if=/dev/zero of=/dev/null bs=4k count=100000 status=progress \
  VS \
  -i /dev/zero -o /dev/null -b 4K -c 100000 -p

run_bench "4. Sparse Stream (2GB)" \
  if=/dev/zero of=/dev/null bs=64k count=30000 conv=sparse status=progress \
  VS \
  -i /dev/zero -o /dev/null -b 64K -c 30000 --sparse -p

run_bench "5. File-to-File (300MB)" \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_sys_f2f.bin" bs=64k status=progress \
  VS \
  -i "$TMP_DIR/bench_in.bin" -o "$TMP_DIR/bench_loc_f2f.bin" -b 64K -p

run_bench "6. Positional Syntax File-to-File" \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_sys_pos.bin" bs=64k status=progress \
  VS \
  "$TMP_DIR/bench_in.bin" "$TMP_DIR/bench_loc_pos.bin" -b 64K -p

printf "\n"
echo "--- 2. Modern Subsystem & Optimizer Benchmarks ---"

# Test 7: Autotuning vs unoptimized default
run_bench "7. Autotune (bs=auto vs def 512)" \
  if="$TMP_DIR/bench_in.bin" of=/dev/null status=progress \
  VS \
  -i "$TMP_DIR/bench_in.bin" -o /dev/null -b auto -p

# Test 8: Async Double-Buffering on File-to-File transfer
run_bench "8. Async Double-Buffer (-e async)" \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_sys_async.bin" bs=64k status=progress \
  VS \
  -i "$TMP_DIR/bench_in.bin" -o "$TMP_DIR/bench_loc_async.bin" -b 64K -e async -p

# Test 9: In-Kernel Zero-Copy (copy_file_range vs POSIX copy)
run_bench "9. Kernel Zero-Copy (-e reflink)" \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_sys_out.bin" bs=1M status=progress \
  VS \
  -i "$TMP_DIR/bench_in.bin" -o "$TMP_DIR/bench_loc_out.bin" -e reflink -p

# Test 10: Arbitrary Byte Count targeting
run_bench "10. Exact Byte Limit (-l 1G)" \
  if=/dev/zero of=/dev/null bs=1M count=1073741824 iflag=count_bytes status=progress \
  VS \
  -i /dev/zero -o /dev/null -b 1M -l 1G -p

# Test 11: In-Flight SHA256 Streaming Integrity Checksum
run_bench "11. In-Flight SHA256 (--hash)" \
  if=/dev/zero of=/dev/null bs=64k count=30000 status=progress \
  VS \
  -i /dev/zero -o /dev/null -b 64K -c 30000 --hash -p

# Test 12: io_uring Asynchronous Kernel Engine (-e uring vs standard sync)
run_bench "12. io_uring Engine (-e uring)" \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_sys_uring.bin" bs=1M status=progress \
  VS \
  -i "$TMP_DIR/bench_in.bin" -o "$TMP_DIR/bench_loc_uring.bin" -b 1M -e uring -p

# Test 13: SIMD AVX2-accelerated Byte Swapping (--swab)
run_bench "13. Byte Swap (--swab 2GB)" \
  if=/dev/zero of=/dev/null bs=64k count=30000 conv=swab status=progress \
  VS \
  -i /dev/zero -o /dev/null -b 64K -c 30000 --swab -p

# Test 14: In-Kernel Zero-Copy Splice Engine (-e splice vs standard sync)
run_bench "14. Kernel Splice (-e splice)" \
  if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_sys_splice.bin" bs=1M status=progress \
  VS \
  -i "$TMP_DIR/bench_in.bin" -o "$TMP_DIR/bench_loc_splice.bin" -b 1M -e splice -p

echo "======================================================================"
echo "Benchmark completed successfully."
