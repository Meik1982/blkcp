use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

pub struct Stats {
    pub start_time: Instant,
    pub records_in_full: AtomicU64,
    pub records_in_part: AtomicU64,
    pub records_out_full: AtomicU64,
    pub records_out_part: AtomicU64,
    pub bytes_copied: AtomicU64,
}

impl Stats {
    pub fn new() -> Self {
        Self {
            start_time: Instant::now(),
            records_in_full: AtomicU64::new(0),
            records_in_part: AtomicU64::new(0),
            records_out_full: AtomicU64::new(0),
            records_out_part: AtomicU64::new(0),
            bytes_copied: AtomicU64::new(0),
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

        let elapsed = self.start_time.elapsed().as_secs_f64().max(0.000001);
        let speed = (bytes as f64) / elapsed;

        eprintln!("{}+{} records in", r_in_f, r_in_p);
        eprintln!("{}+{} records out", r_out_f, r_out_p);

        let (speed_val, speed_unit) = if speed >= 1_000_000_000.0 {
            (speed / 1_000_000_000.0, "GB/s")
        } else if speed >= 1_000_000.0 {
            (speed / 1_000_000.0, "MB/s")
        } else if speed >= 1_000.0 {
            (speed / 1_000.0, "kB/s")
        } else {
            (speed, "B/s")
        };

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
        if quiet {
            if let Some((hash_val, algo_name)) = hash_info {
                eprintln!("{}: {}", algo_name, hash_val);
            }
            return;
        }

        let r_in_f = self.records_in_full.load(Ordering::Relaxed);
        let r_in_p = self.records_in_part.load(Ordering::Relaxed);
        let r_out_f = self.records_out_full.load(Ordering::Relaxed);
        let r_out_p = self.records_out_part.load(Ordering::Relaxed);
        let bytes = self.bytes_copied.load(Ordering::Relaxed);

        let elapsed = self.start_time.elapsed().as_secs_f64().max(0.000001);
        let speed = (bytes as f64) / elapsed;

        if json {
            let json_obj = serde_json_lite(self, elapsed, speed, hash_info);
            eprintln!("{}", json_obj);
            return;
        }

        // Standard GNU dd format compatibility
        eprintln!("{}+{} records in", r_in_f, r_in_p);
        eprintln!("{}+{} records out", r_out_f, r_out_p);

        let (speed_val, speed_unit) = if speed >= 1_000_000_000.0 {
            (speed / 1_000_000_000.0, "GB/s")
        } else if speed >= 1_000_000.0 {
            (speed / 1_000_000.0, "MB/s")
        } else if speed >= 1_000.0 {
            (speed / 1_000.0, "kB/s")
        } else {
            (speed, "B/s")
        };

        eprintln!(
            "{} bytes ({}) copied, {:.5} s, {:.1} {}",
            bytes,
            human_readable_bytes(bytes),
            elapsed,
            speed_val,
            speed_unit
        );

        if let Some((hash_val, algo_name)) = hash_info {
            eprintln!("{}: {}", algo_name, hash_val);
        }
    }
}

impl Default for Stats {
    fn default() -> Self {
        Self::new()
    }
}

fn human_readable_bytes(bytes: u64) -> String {
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
    let r_out_f = stats.records_out_full.load(Ordering::Relaxed);

    let mut out = format!(
        "{{\"bytes_copied\":{},\"elapsed_sec\":{:.5},\"speed_bps\":{:.0},\"records_in_full\":{},\"records_out_full\":{}",
        bytes, elapsed, speed, r_in_f, r_out_f
    );
    if let Some((hash_val, algo_name)) = hash_info {
        out.push_str(&format!(
            ",\"hash_algo\":\"{}\",\"hash\":\"{}\"",
            algo_name, hash_val
        ));
    }
    out.push('}');
    out
}
