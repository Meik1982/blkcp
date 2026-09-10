#!/usr/bin/env bash
# Benchmark comparison script: Local optimized dd vs System /usr/bin/dd
set -euo pipefail
export LC_ALL=C

LOCAL_DD="./dd"
SYSTEM_DD="/usr/bin/dd"

if [[ ! -x "$LOCAL_DD" ]]; then
  echo "Error: Local binary '$LOCAL_DD' not found. Run 'make all' first." >&2
  exit 1
fi

if [[ ! -x "$SYSTEM_DD" ]]; then
  echo "Error: System binary '$SYSTEM_DD' not found." >&2
  exit 1
fi

TMP_DIR=$(mktemp -d -t dd_bench_XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

echo "======================================================================"
echo "         Performance Benchmark: Local dd vs. System dd                "
echo "======================================================================"
echo "Local Binary:  $LOCAL_DD ($(ls -lh "$LOCAL_DD" | awk '{print $5}'))"
echo "System Binary: $SYSTEM_DD ($(ls -lh "$SYSTEM_DD" | awk '{print $5}'))"
echo "Date:          $(date '+%Y-%m-%d %H:%M:%S')"
echo "Host:          $(uname -sr) on $(uname -m)"
echo "======================================================================"
printf "\n"

# Helper to run a command and extract throughput in GB/s (or MB/s normalized to GB/s)
measure_speed() {
  local binary="$1"
  shift
  local out
  out=$("$binary" "$@" 2>&1 || true)
  
  # Extract throughput line (e.g. "... copied, 0.123 s, 18.5 GB/s" or "... MB/s")
  local speed_line
  speed_line=$(echo "$out" | grep -E "copied" | tail -n 1)
  
  # Parse speed value and unit
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

# Run benchmark test with 3 iterations, picking the best run
run_test() {
  local title="$1"
  shift
  local args=("$@")

  printf "%-32s : " "$title"

  local sys_best=0
  local loc_best=0

  for i in {1..3}; do
    local s_speed l_speed
    s_speed=$(measure_speed "$SYSTEM_DD" "${args[@]}")
    l_speed=$(measure_speed "$LOCAL_DD" "${args[@]}")

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

run_test "1. Stream bs=1M (2GB)"        if=/dev/zero of=/dev/null bs=1M count=2000 status=progress
run_test "2. Stream bs=64k (3.2GB)"     if=/dev/zero of=/dev/null bs=64k count=50000 status=progress
run_test "3. Two-Buf ibs=64k obs=64k"   if=/dev/zero of=/dev/null ibs=64k obs=64k count=50000 status=progress
run_test "4. Small Block bs=4k (400MB)" if=/dev/zero of=/dev/null bs=4k count=100000 status=progress
run_test "5. Sparse conv=sparse (2GB)"  if=/dev/zero of=/dev/null bs=64k count=30000 conv=sparse status=progress
run_test "6. File-to-File (300MB)"      if="$TMP_DIR/bench_in.bin" of="$TMP_DIR/bench_out.bin" bs=64k status=progress
run_test "7. Conversion conv=ucase"     if=/dev/zero of=/dev/null bs=64k count=20000 conv=ucase status=progress

echo "======================================================================"
echo "Benchmark completed successfully."
