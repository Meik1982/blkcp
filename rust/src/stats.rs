use crossbeam_utils::CachePadded;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

/// High-concurrency transfer telemetry counters.
///
/// Each atomic counter is wrapped in `CachePadded` (64/128-byte cache-line aligned)
/// to eliminate CPU L1/L2 cache-line bouncing (false sharing) across reader, writer,
/// hasher, and multi-ring `io_uring` shard threads.
pub struct Stats {
    pub start_time: Instant,
    pub records_in_full: CachePadded<AtomicU64>,
    pub records_in_part: CachePadded<AtomicU64>,
    pub records_out_full: CachePadded<AtomicU64>,
    pub records_out_part: CachePadded<AtomicU64>,
    pub bytes_copied: CachePadded<AtomicU64>,
    pub async_capacity: CachePadded<AtomicU64>,
    pub reader_stalls: CachePadded<AtomicU64>,
    pub writer_stalls: CachePadded<AtomicU64>,
}

impl Stats {
    pub fn new() -> Self {
        Self {
            start_time: Instant::now(),
            records_in_full: CachePadded::new(AtomicU64::new(0)),
            records_in_part: CachePadded::new(AtomicU64::new(0)),
            records_out_full: CachePadded::new(AtomicU64::new(0)),
            records_out_part: CachePadded::new(AtomicU64::new(0)),
            bytes_copied: CachePadded::new(AtomicU64::new(0)),
            async_capacity: CachePadded::new(AtomicU64::new(0)),
            reader_stalls: CachePadded::new(AtomicU64::new(0)),
            writer_stalls: CachePadded::new(AtomicU64::new(0)),
        }
    }

    #[inline]
    pub fn inc_in_full(&self) {
        self.records_in_full.fetch_add(1, Ordering::Relaxed);
    }

    #[inline]
    pub fn inc_in_part(&self) {
        self.records_in_part.fetch_add(1, Ordering::Relaxed);
    }

    #[inline]
    pub fn inc_out_full(&self) {
        self.records_out_full.fetch_add(1, Ordering::Relaxed);
    }

    #[inline]
    pub fn inc_out_part(&self) {
        self.records_out_part.fetch_add(1, Ordering::Relaxed);
    }

    #[inline]
    pub fn add_bytes(&self, bytes: u64) {
        self.bytes_copied.fetch_add(bytes, Ordering::Relaxed);
    }

    #[inline]
    pub fn get_bytes(&self) -> u64 {
        self.bytes_copied.load(Ordering::Relaxed)
    }

    pub fn print_instant_telemetry(&self) {
        let r_in_f = self.records_in_full.load(Ordering::Relaxed);
        let r_in_p = self.records_in_part.load(Ordering::Relaxed);
        let r_out_f = self.records_out_full.load(Ordering::Relaxed);
        let r_out_p = self.records_out_part.load(Ordering::Relaxed);
        let bytes = self.bytes_copied.load(Ordering::Relaxed);

        let elapsed = self.start_time.elapsed().as_secs_f64().max(0.000_001);
        let speed = (bytes as f64) / elapsed;

        eprintln!("{}+{} records in", r_in_f, r_in_p);
        eprintln!("{}+{} records out", r_out_f, r_out_p);

        let (speed_val, speed_unit) = format_speed(speed);

        eprintln!(
            "{} bytes ({}) copied, {:.5} s, {:.1} {}",
            bytes,
            human_readable_bytes(bytes),
            elapsed,
            speed_val,
            speed_unit
        );
    }

    pub fn print_summary(
        &self,
        quiet: bool,
        json: bool,
        hash_info: Option<(String, &'static str)>,
    ) {
        let r_in_f = self.records_in_full.load(Ordering::Relaxed);
        let r_in_p = self.records_in_part.load(Ordering::Relaxed);
        let r_out_f = self.records_out_full.load(Ordering::Relaxed);
        let r_out_p = self.records_out_part.load(Ordering::Relaxed);
        let bytes = self.bytes_copied.load(Ordering::Relaxed);

        let elapsed = self.start_time.elapsed().as_secs_f64().max(0.000_001);
        let speed = (bytes as f64) / elapsed;

        if json {
            let json_obj = serde_json_lite(self, elapsed, speed, hash_info);
            eprintln!("{}", json_obj);
            return;
        }

        if quiet {
            if let Some((hash_val, algo_name)) = hash_info {
                if algo_name == "blks" {
                    eprintln!("blks:   {}", hash_val);
                } else {
                    eprintln!("{}: {}", algo_name, hash_val);
                }
            }
            return;
        }

        // Standard GNU dd format compatibility
        eprintln!("{}+{} records in", r_in_f, r_in_p);
        eprintln!("{}+{} records out", r_out_f, r_out_p);

        let (speed_val, speed_unit) = format_speed(speed);

        eprintln!(
            "{} bytes ({}) copied, {:.5} s, {:.1} {}",
            bytes,
            human_readable_bytes(bytes),
            elapsed,
            speed_val,
            speed_unit
        );

        if let Some((hash_val, algo_name)) = hash_info {
            if algo_name == "blks" {
                eprintln!("blks:   {}", hash_val);
            } else {
                eprintln!("{}: {}", algo_name, hash_val);
            }
        }
    }
}

impl Default for Stats {
    fn default() -> Self {
        Self::new()
    }
}

pub fn format_speed(speed: f64) -> (f64, &'static str) {
    if speed >= 1_000_000_000.0 {
        (speed / 1_000_000_000.0, "GB/s")
    } else if speed >= 1_000_000.0 {
        (speed / 1_000_000.0, "MB/s")
    } else if speed >= 1_000.0 {
        (speed / 1_000.0, "kB/s")
    } else {
        (speed, "B/s")
    }
}

pub fn human_readable_bytes(bytes: u64) -> String {
    if bytes >= 1024 * 1024 * 1024 {
        format!("{:.1} GB", bytes as f64 / 1_073_741_824.0)
    } else if bytes >= 1024 * 1024 {
        format!("{:.1} MB", bytes as f64 / 1_048_576.0)
    } else if bytes >= 1024 {
        format!("{:.1} kB", bytes as f64 / 1024.0)
    } else {
        format!("{} B", bytes)
    }
}

fn serde_json_lite(
    stats: &Stats,
    elapsed: f64,
    speed: f64,
    hash_info: Option<(String, &'static str)>,
) -> String {
    let bytes = stats.bytes_copied.load(Ordering::Relaxed);
    let r_in_f = stats.records_in_full.load(Ordering::Relaxed);
    let r_in_p = stats.records_in_part.load(Ordering::Relaxed);
    let r_out_f = stats.records_out_full.load(Ordering::Relaxed);
    let r_out_p = stats.records_out_part.load(Ordering::Relaxed);
    let cap = stats.async_capacity.load(Ordering::Relaxed);
    let r_stalls = stats.reader_stalls.load(Ordering::Relaxed);
    let w_stalls = stats.writer_stalls.load(Ordering::Relaxed);

    let pipeline_json = if cap > 0 {
        format!(
            "{{\"capacity\":{},\"reader_stalls\":{},\"writer_stalls\":{}}}",
            cap, r_stalls, w_stalls
        )
    } else {
        "null".to_string()
    };

    let (checksum_json, sha256_json) = if let Some((ref hash_val, algo_name)) = hash_info {
        (
            format!(
                "{{\"algorithm\":\"{}\",\"digest\":\"{}\"}}",
                algo_name, hash_val
            ),
            format!("\"{}\"", hash_val),
        )
    } else {
        ("null".to_string(), "null".to_string())
    };

    format!(
        "{{\"event\":\"finished\",\"copied_bytes\":{},\"records_in\":{{\"full\":{},\"partial\":{},\"truncated\":0}},\"records_out\":{{\"full\":{},\"partial\":{}}},\"elapsed_s\":{:.4},\"avg_speed_bps\":{:.0},\"pipeline\":{},\"checksum\":{},\"sha256\":{}}}",
        bytes, r_in_f, r_in_p, r_out_f, r_out_p, elapsed, speed, pipeline_json, checksum_json, sha256_json
    )
}
