use std::time::Instant;

pub struct Stats {
    pub start_time: Instant,
    pub records_in_full: u64,
    pub records_in_part: u64,
    pub records_out_full: u64,
    pub records_out_part: u64,
    pub bytes_copied: u64,
}

impl Stats {
    pub fn new() -> Self {
        Self {
            start_time: Instant::now(),
            records_in_full: 0,
            records_in_part: 0,
            records_out_full: 0,
            records_out_part: 0,
            bytes_copied: 0,
        }
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

        let elapsed = self.start_time.elapsed().as_secs_f64().max(0.000001);
        let speed = (self.bytes_copied as f64) / elapsed;

        if json {
            let json_obj = serde_json_lite(self, elapsed, speed, hash_info);
            eprintln!("{}", json_obj);
            return;
        }

        // Standard GNU dd format compatibility
        eprintln!(
            "{}+{} records in",
            self.records_in_full, self.records_in_part
        );
        eprintln!(
            "{}+{} records out",
            self.records_out_full, self.records_out_part
        );

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
            self.bytes_copied,
            human_readable_bytes(self.bytes_copied),
            elapsed,
            speed_val,
            speed_unit
        );

        if let Some((hash_val, algo_name)) = hash_info {
            eprintln!("{}: {}", algo_name, hash_val);
        }
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
    let mut out = format!(
        "{{\"bytes_copied\":{},\"elapsed_sec\":{:.5},\"speed_bps\":{:.0},\"records_in_full\":{},\"records_out_full\":{}",
        stats.bytes_copied, elapsed, speed, stats.records_in_full, stats.records_out_full
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
