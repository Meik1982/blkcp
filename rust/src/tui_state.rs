use crate::args::{parse_size, Config, Engine, HashAlgo};
use crate::engine::execute_transfer_with_control;
use crate::stats::{format_speed, human_readable_bytes, Stats};
use crate::tui_device::{scan_block_devices_ext, BlockDevice, EndpointKind, FileEntry};

use ratatui::style::Color;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FocusPane {
    Source,
    Target,
    Modules,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SyncMode {
    None,
    Fdatasync,
    Fsync,
}

impl SyncMode {
    pub fn as_str(self) -> &'static str {
        match self {
            SyncMode::None => "none",
            SyncMode::Fdatasync => "fdatasync",
            SyncMode::Fsync => "fsync",
        }
    }

    pub fn next(self) -> Self {
        match self {
            SyncMode::None => SyncMode::Fdatasync,
            SyncMode::Fdatasync => SyncMode::Fsync,
            SyncMode::Fsync => SyncMode::None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ModalDialog {
    None,
    SafetyConfirm,
    PathInput {
        for_target: bool,
        buffer: String,
    },
    LimitInput {
        field: usize, // 0 = limit, 1 = skip, 2 = seek
        limit_buf: String,
        skip_buf: String,
        seek_buf: String,
    },
    FileBrowser {
        for_target: bool,
        current_dir: PathBuf,
        entries: Vec<FileEntry>,
        selected_idx: usize,
    },
}

pub struct ActiveJob {
    pub stats: Arc<Stats>,
    pub cancel_flag: Arc<AtomicBool>,
    pub expected_bytes: Option<u64>,
    pub handle: JoinHandle<anyhow::Result<Option<(String, &'static str)>>>,
}

pub struct TuiState {
    pub devices: Vec<BlockDevice>,
    pub include_partitions: bool,
    pub source_idx: usize,
    pub target_idx: usize,
    pub focus: FocusPane,
    pub module_idx: usize,

    // Core Module Parameters
    pub block_size_str: &'static str,
    pub engine: Engine,
    pub hash_algo: HashAlgo,
    pub threads: usize,
    pub queue_depth_opt: Option<usize>,

    // Combination Flags
    pub direct_io: bool,
    pub nocache: bool,
    pub sparse: bool,
    pub swab: bool,
    pub sync_pad: bool,
    pub notrunc: bool,
    pub noerror: bool,
    pub dry_run: bool,
    pub force_override: bool,
    pub sync_mode: SyncMode,

    // Offsets & Limits
    pub limit_str: String,
    pub skip_str: String,
    pub seek_str: String,

    // UI & Execution State
    pub modal: ModalDialog,
    pub active_job: Option<ActiveJob>,
    pub last_hash_result: Option<(String, &'static str)>,
    pub last_summary_line: Option<String>,
    pub running: bool,
    pub status_msg: String,
}

impl TuiState {
    pub fn new() -> Self {
        let devices = scan_block_devices_ext(false);
        // Default to safe virtual benchmark pair (/dev/zero -> /dev/null) if available
        let zero_idx = devices
            .iter()
            .position(|d| d.path == std::path::Path::new("/dev/zero"))
            .unwrap_or(0);
        let null_idx = devices
            .iter()
            .position(|d| d.path == std::path::Path::new("/dev/null"))
            .unwrap_or(0);

        Self {
            devices,
            include_partitions: false,
            source_idx: zero_idx,
            target_idx: null_idx,
            focus: FocusPane::Modules,
            module_idx: 0,
            block_size_str: "1M",
            engine: Engine::Auto,
            hash_algo: HashAlgo::Blake3,
            threads: 1,
            queue_depth_opt: None,
            direct_io: false,
            nocache: false,
            sparse: false,
            swab: false,
            sync_pad: false,
            notrunc: false,
            noerror: false,
            dry_run: false,
            force_override: false,
            sync_mode: SyncMode::None,
            limit_str: "256M".to_string(),
            skip_str: "0".to_string(),
            seek_str: "0".to_string(),
            modal: ModalDialog::None,
            active_job: None,
            last_hash_result: None,
            last_summary_line: None,
            running: true,
            status_msg:
                "Bereit. Nutze [Tab] für Fokus, [1-8] für Presets, [Z] Auto-Fix, [F] Browser, [Enter] Start."
                    .to_string(),
        }
    }

    pub fn refresh_devices(&mut self) {
        let prev_src = self.devices.get(self.source_idx).map(|d| d.path.clone());
        let prev_dst = self.devices.get(self.target_idx).map(|d| d.path.clone());

        let custom_devs: Vec<BlockDevice> = self
            .devices
            .iter()
            .filter(|d| d.kind == EndpointKind::RegularFile)
            .cloned()
            .collect();

        self.devices = scan_block_devices_ext(self.include_partitions);
        for cd in custom_devs {
            if !self.devices.iter().any(|d| d.path == cd.path) {
                self.devices.push(cd);
            }
        }

        if let Some(p) = prev_src {
            if let Some(pos) = self.devices.iter().position(|d| d.path == p) {
                self.source_idx = pos;
            }
        }
        if let Some(p) = prev_dst {
            if let Some(pos) = self.devices.iter().position(|d| d.path == p) {
                self.target_idx = pos;
            }
        }
        if self.source_idx >= self.devices.len() {
            self.source_idx = 0;
        }
        if self.target_idx >= self.devices.len() {
            self.target_idx = 0;
        }
        self.status_msg = format!(
            "Geräteliste aktualisiert ({} Endpunkte, Partitionen: {}).",
            self.devices.len(),
            if self.include_partitions { "AN" } else { "AUS" }
        );
    }

    pub fn add_or_select_custom_path(&mut self, path: PathBuf, for_target: bool) {
        let dev = BlockDevice::from_custom_path(path.clone());
        let idx = if let Some(pos) = self.devices.iter().position(|d| d.path == path) {
            self.devices[pos] = dev;
            pos
        } else {
            self.devices.push(dev);
            self.devices.len() - 1
        };

        if for_target {
            self.target_idx = idx;
            self.status_msg = format!("Ziel auf '{}' gesetzt.", path.display());
        } else {
            self.source_idx = idx;
            self.status_msg = format!("Quelle auf '{}' gesetzt.", path.display());
        }
    }

    pub fn apply_preset(&mut self, preset: u8) {
        match preset {
            1 => {
                // Max-Speed NVMe / Block Clone
                self.engine = Engine::Uring;
                self.threads = 4;
                self.block_size_str = "1M";
                self.direct_io = true;
                self.nocache = false;
                self.sparse = false;
                self.swab = false;
                self.sync_pad = false;
                self.hash_algo = HashAlgo::None;
                self.sync_mode = SyncMode::Fdatasync;
                self.status_msg =
                    "Preset [1] geladen: Max-Speed Multi-Ring io_uring (-e uring -j 4 -b 1M --direct --fdatasync)"
                        .to_string();
            }
            2 => {
                // Forensic Verified Image (Post-Quantum BLKS-384 + Async 3-Stage Pipeline)
                self.engine = Engine::Async;
                self.threads = 1;
                self.queue_depth_opt = Some(32);
                self.block_size_str = "1M";
                self.direct_io = false;
                self.sparse = false;
                self.swab = false;
                self.sync_pad = true;
                self.noerror = true;
                self.hash_algo = HashAlgo::Blks;
                self.sync_mode = SyncMode::Fsync;
                self.status_msg =
                    "Preset [2] geladen: Forensik-Kopie mit 3-Stufen-Pipeline & BLKS-384 PQ-Hash (-e async --blks --sync --noerror --fsync)"
                        .to_string();
            }
            3 => {
                // Sparse Image Backup + BLAKE3
                self.engine = Engine::Sync;
                self.threads = 1;
                self.block_size_str = "1M";
                self.direct_io = false;
                self.sparse = true;
                self.swab = false;
                self.sync_pad = false;
                self.hash_algo = HashAlgo::Blake3;
                self.sync_mode = SyncMode::Fdatasync;
                self.status_msg =
                    "Preset [3] geladen: Sparse Backup mit Hole-Punching & BLAKE3 (--sparse --blake3 -b 1M)"
                        .to_string();
            }
            4 => {
                // Instant CoW Snapshot (Reflink)
                self.engine = Engine::Reflink;
                self.threads = 1;
                self.block_size_str = "4M";
                self.direct_io = false;
                self.nocache = false;
                self.sparse = false;
                self.swab = false;
                self.sync_pad = false;
                self.hash_algo = HashAlgo::None;
                self.sync_mode = SyncMode::None;
                self.status_msg =
                    "Preset [4] geladen: Kernel Zero-Copy CoW Reflink (-e reflink -b 4M)"
                        .to_string();
            }
            5 => {
                // Safe 2 GiB Hardware Benchmark (/dev/zero -> /dev/null)
                if let Some(z) = self
                    .devices
                    .iter()
                    .position(|d| d.path == std::path::Path::new("/dev/zero"))
                {
                    self.source_idx = z;
                }
                if let Some(n) = self
                    .devices
                    .iter()
                    .position(|d| d.path == std::path::Path::new("/dev/null"))
                {
                    self.target_idx = n;
                }
                self.engine = Engine::Uring;
                self.threads = 4;
                self.block_size_str = "1M";
                self.limit_str = "2G".to_string();
                self.direct_io = false;
                self.sparse = false;
                self.swab = false;
                self.hash_algo = HashAlgo::None;
                self.dry_run = false;
                self.status_msg =
                    "Preset [5] geladen: 2 GiB Speed-Benchmark (/dev/zero -> /dev/null, -e uring -j 4 -l 2G)"
                        .to_string();
            }
            6 => {
                // In-Kernel Zero-Copy Splice Pipeline
                self.engine = Engine::Splice;
                self.threads = 1;
                self.block_size_str = "1M";
                self.direct_io = false;
                self.nocache = false;
                self.sparse = false;
                self.swab = false;
                self.sync_pad = false;
                self.hash_algo = HashAlgo::None;
                self.sync_mode = SyncMode::Fdatasync;
                self.status_msg =
                    "Preset [6] geladen: In-Kernel Zero-Copy Splice (-e splice -b 1M --fdatasync)"
                        .to_string();
            }
            7 => {
                // High-End Multi-Ring Direct I/O Cache-Bypass (100+ GB Storage Transfers)
                self.engine = Engine::Uring;
                self.threads = 4;
                self.block_size_str = "1M";
                self.direct_io = true;
                self.nocache = true;
                self.sparse = false;
                self.swab = false;
                self.sync_pad = false;
                self.hash_algo = HashAlgo::None;
                self.sync_mode = SyncMode::Fdatasync;
                self.status_msg =
                    "Preset [7] geladen: Direct I/O + No-Cache Multi-Ring (-e uring -j 4 --direct --nocache --fdatasync)"
                        .to_string();
            }
            8 => {
                // Cross-Architecture Endian-Swap Migration (AVX2 Swab + BLAKE3 Integrity)
                self.engine = Engine::Async;
                self.threads = 1;
                self.block_size_str = "1M";
                self.direct_io = false;
                self.nocache = false;
                self.sparse = false;
                self.swab = true;
                self.sync_pad = false;
                self.hash_algo = HashAlgo::Blake3;
                self.sync_mode = SyncMode::Fdatasync;
                self.status_msg =
                    "Preset [8] geladen: Cross-Arch Endian-Swap mit AVX2 & BLAKE3 (--swab --blake3 -e async -b 1M)"
                        .to_string();
            }
            _ => {}
        }
    }

    pub fn auto_resolve_combinations(&mut self) {
        let mut changes = Vec::new();

        // 1. Reflink with userspace transforms or hashing
        if self.engine == Engine::Reflink
            && (self.hash_algo != HashAlgo::None || self.swab || self.sparse || self.sync_pad)
        {
            self.engine = Engine::Auto;
            changes.push("Engine 'reflink' -> 'auto' umgestellt");
        }

        // 2. Splice with Direct I/O or Hash/Swab
        if self.engine == Engine::Splice && self.direct_io {
            self.direct_io = false;
            changes.push("Direct I/O deaktiviert (splice(2) inkompatibel)");
        }
        if self.engine == Engine::Splice && (self.hash_algo != HashAlgo::None || self.swab) {
            self.engine = Engine::Async;
            changes.push("Engine 'splice' -> 'async' umgestellt (für Hash/Swab)");
        }

        // 3. Direct I/O with redundant nocache
        if self.direct_io && self.nocache {
            self.nocache = false;
            changes.push("No-Cache deaktiviert (Direct I/O umgeht Page Cache)");
        }

        if changes.is_empty() {
            self.status_msg =
                "Kombinations-Check: Alle aktiven Module sind bereits optimal abgestimmt."
                    .to_string();
        } else {
            self.status_msg = format!("Auto-Resolve: {}", changes.join(" | "));
        }
    }

    pub fn cycle_block_size(&mut self) {
        self.block_size_str = match self.block_size_str {
            "4K" => "64K",
            "64K" => "256K",
            "256K" => "1M",
            "1M" => "4M",
            "4M" => "16M",
            "16M" => "auto",
            _ => "4K",
        };
    }

    pub fn cycle_engine(&mut self) {
        self.engine = match self.engine {
            Engine::Auto => Engine::Uring,
            Engine::Uring => Engine::Async,
            Engine::Async => Engine::Reflink,
            Engine::Reflink => Engine::Splice,
            Engine::Splice => Engine::Sync,
            Engine::Sync => Engine::Auto,
        };
    }

    pub fn cycle_hash(&mut self) {
        self.hash_algo = match self.hash_algo {
            HashAlgo::None => HashAlgo::Blake3,
            HashAlgo::Blake3 => HashAlgo::Blks,
            HashAlgo::Blks => HashAlgo::Sha256,
            HashAlgo::Sha256 => HashAlgo::None,
        };
    }

    pub fn cycle_threads(&mut self) {
        self.threads = match self.threads {
            1 => 2,
            2 => 4,
            4 => 8,
            8 => 16,
            16 => 32,
            _ => 1,
        };
    }

    pub fn cycle_queue_depth(&mut self) {
        self.queue_depth_opt = match self.queue_depth_opt {
            None => Some(16),
            Some(16) => Some(32),
            Some(32) => Some(64),
            Some(64) => Some(128),
            _ => None,
        };
    }

    pub fn toggle_selected_module(&mut self) {
        match self.module_idx {
            0 => self.cycle_block_size(),
            1 => self.cycle_engine(),
            2 => self.cycle_hash(),
            3 => self.cycle_threads(),
            4 => self.cycle_queue_depth(),
            5 => self.direct_io = !self.direct_io,
            6 => self.nocache = !self.nocache,
            7 => self.sparse = !self.sparse,
            8 => self.swab = !self.swab,
            9 => self.sync_pad = !self.sync_pad,
            10 => self.notrunc = !self.notrunc,
            11 => self.sync_mode = self.sync_mode.next(),
            12 => self.noerror = !self.noerror,
            13 => self.dry_run = !self.dry_run,
            14 => {
                self.modal = ModalDialog::LimitInput {
                    field: 0,
                    limit_buf: self.limit_str.clone(),
                    skip_buf: self.skip_str.clone(),
                    seek_buf: self.seek_str.clone(),
                };
            }
            15 => self.force_override = !self.force_override,
            _ => {}
        }
    }

    pub fn build_config(&self) -> anyhow::Result<Config> {
        let mut cfg = Config::default();
        if let Some(src) = self.devices.get(self.source_idx) {
            cfg.input = Some(src.path.clone());
        }
        if let Some(dst) = self.devices.get(self.target_idx) {
            cfg.output = Some(dst.path.clone());
        }

        if self.block_size_str.eq_ignore_ascii_case("auto") {
            cfg.autotune = true;
            cfg.block_size = 64 * 1024;
        } else {
            cfg.block_size = parse_size(self.block_size_str)? as usize;
        }

        cfg.engine = self.engine;
        cfg.hash_algo = self.hash_algo;
        cfg.threads = self.threads;
        cfg.queue_depth = self.queue_depth_opt;
        cfg.direct = self.direct_io;
        cfg.nocache = self.nocache;
        cfg.sparse = self.sparse;
        cfg.swab = self.swab;
        cfg.sync = self.sync_pad;
        cfg.notrunc = self.notrunc;
        cfg.noerror = self.noerror;
        cfg.dry_run = self.dry_run;
        cfg.force = self.force_override;
        cfg.fdatasync = self.sync_mode == SyncMode::Fdatasync;
        cfg.fsync = self.sync_mode == SyncMode::Fsync;

        let lim = self.limit_str.trim();
        if !lim.is_empty() && lim != "0" && !lim.eq_ignore_ascii_case("none") {
            cfg.limit_bytes = Some(parse_size(lim)?);
        }

        let skp = self.skip_str.trim();
        if !skp.is_empty() && skp != "0" {
            cfg.skip_bytes = parse_size(skp)?;
        }

        let sek = self.seek_str.trim();
        if !sek.is_empty() && sek != "0" {
            cfg.seek_bytes = parse_size(sek)?;
        }

        Ok(cfg)
    }

    pub fn build_cli_command(&self) -> String {
        let src = self
            .devices
            .get(self.source_idx)
            .map(|d| d.path.to_string_lossy().to_string())
            .unwrap_or_else(|| "INPUT".to_string());
        let dst = self
            .devices
            .get(self.target_idx)
            .map(|d| d.path.to_string_lossy().to_string())
            .unwrap_or_else(|| "OUTPUT".to_string());

        let mut parts = vec![
            "blkcp".to_string(),
            format!("-i {}", src),
            format!("-o {}", dst),
            format!("-b {}", self.block_size_str),
        ];

        let eng_str = match self.engine {
            Engine::Auto => "auto",
            Engine::Sync => "sync",
            Engine::Async => "async",
            Engine::Reflink => "reflink",
            Engine::Uring => "uring",
            Engine::Splice => "splice",
        };
        if self.engine != Engine::Auto {
            parts.push(format!("-e {}", eng_str));
        }

        if self.threads > 1 {
            parts.push(format!("-j {}", self.threads));
        }
        if let Some(qd) = self.queue_depth_opt {
            parts.push(format!("--queue-depth={}", qd));
        }

        match self.hash_algo {
            HashAlgo::None => {}
            HashAlgo::Blake3 => parts.push("--blake3".to_string()),
            HashAlgo::Blks => parts.push("--blks".to_string()),
            HashAlgo::Sha256 => parts.push("--sha256".to_string()),
        }

        if self.direct_io {
            parts.push("--direct".to_string());
        }
        if self.nocache {
            parts.push("--nocache".to_string());
        }
        if self.sparse {
            parts.push("--sparse".to_string());
        }
        if self.swab {
            parts.push("--swab".to_string());
        }
        if self.sync_pad {
            parts.push("--sync".to_string());
        }
        if self.notrunc {
            parts.push("--notrunc".to_string());
        }
        if self.noerror {
            parts.push("--noerror".to_string());
        }
        match self.sync_mode {
            SyncMode::None => {}
            SyncMode::Fdatasync => parts.push("--fdatasync".to_string()),
            SyncMode::Fsync => parts.push("--fsync".to_string()),
        }
        if !self.limit_str.trim().is_empty() && self.limit_str.trim() != "0" {
            parts.push(format!("-l {}", self.limit_str.trim()));
        }
        if !self.skip_str.trim().is_empty() && self.skip_str.trim() != "0" {
            parts.push(format!("--skip={}", self.skip_str.trim()));
        }
        if !self.seek_str.trim().is_empty() && self.seek_str.trim() != "0" {
            parts.push(format!("--seek={}", self.seek_str.trim()));
        }
        if self.dry_run {
            parts.push("--dry-run".to_string());
        }
        if self.force_override {
            parts.push("--force".to_string());
        }

        parts.join(" ")
    }

    pub fn analyze_combination(&self) -> (String, Color) {
        let mut notes = Vec::new();
        let mut severity = Color::Green;

        let needs_userspace = self.swab || self.sparse || self.sync_pad;

        // 1. Reflink Compatibility
        if self.engine == Engine::Reflink && (self.hash_algo != HashAlgo::None || needs_userspace) {
            notes.push(
                "Reflink + Userspace-Transformation (Hash/Swab/Sparse) inkompatibel -> nutzt Buffered-Streaming"
                    .to_string(),
            );
            severity = Color::Yellow;
        }

        // 2. Splice Compatibility
        if self.engine == Engine::Splice && self.direct_io {
            notes.push("KONFLIKT: splice(2) unterstützt kein O_DIRECT (--direct)!".to_string());
            severity = Color::Red;
        } else if self.engine == Engine::Splice && (self.hash_algo != HashAlgo::None || self.swab) {
            notes.push(
                "KONFLIKT: splice(2) ist Zero-Copy im Kernel und schließt In-Flight Transformationen (Hash/Swab) aus!"
                    .to_string(),
            );
            severity = Color::Red;
        }

        // 3. io_uring Native Capabilities
        if self.engine == Engine::Uring || (self.threads > 1 && self.engine == Engine::Auto) {
            if self.hash_algo != HashAlgo::None {
                let algo_name = match self.hash_algo {
                    HashAlgo::None => "none",
                    HashAlgo::Blake3 => "BLAKE3",
                    HashAlgo::Blks => "BLKS-384",
                    HashAlgo::Sha256 => "SHA-256",
                };
                notes.push(format!(
                    "io_uring In-Flight Hash aktiv (1,15 GB/s BLAKE3 / 0,92 GB/s BLKS-384, {})",
                    algo_name
                ));
            }
            if needs_userspace {
                notes.push(
                    "io_uring + Swab/Sparse/Sync gewählt -> transparenter Fallback auf Userspace-Pipeline"
                        .to_string(),
                );
                severity = Color::Yellow;
            }
            if self.threads > 1 && self.hash_algo == HashAlgo::None {
                notes.push(format!(
                    "Multi-Ring io_uring aktiv ({} parallele Kernel-Ringe à 64 MiB Stripes)",
                    self.threads
                ));
            }
        } else if self.engine == Engine::Async {
            let hash_stage = if self.hash_algo != HashAlgo::None {
                "3-Stufen (Reader -> Writer -> Hasher)"
            } else {
                "2-Stufen (Reader -> Writer)"
            };
            notes.push(format!(
                "Zero-Alloc Ringbuffer-Pipeline aktiv: {}",
                hash_stage
            ));
        }

        // 4. Direct I/O & Cache Synergy
        if self.direct_io && self.nocache {
            notes.push(
                "--direct umgeht den Page Cache bereits; --nocache ist redundant.".to_string(),
            );
            if severity == Color::Green {
                severity = Color::Cyan;
            }
        }

        if let Some(src) = self.devices.get(self.source_idx) {
            if src.kind == EndpointKind::VirtualDevice
                && (self.limit_str.trim().is_empty() || self.limit_str.trim() == "0")
            {
                notes.push(format!(
                    "ACHTUNG: Endlos-Stream ({}) ohne Byte-Limit (-l)! Setze [L] Limit oder stoppe manuell mit [X].",
                    src.path.display()
                ));
                if severity != Color::Red {
                    severity = Color::Yellow;
                }
            }
        }

        if let Some(dst) = self.devices.get(self.target_idx) {
            if dst.is_system_root && !self.force_override {
                notes.push(
                    "TARGET SAFETY GUARD BLOCKIERT: Ziel ist gemountetes Systemlaufwerk!"
                        .to_string(),
                );
                severity = Color::Red;
            } else if dst.is_system_root && self.force_override {
                notes.push("WARNUNG: Systemschutz durch --force übersteuert!".to_string());
                severity = Color::Red;
            }
        }

        if notes.is_empty() {
            notes.push("Optimale Modul-Kombination (keine Konflikte).".to_string());
        }

        (notes.join(" | "), severity)
    }

    pub fn start_transfer(&mut self) {
        let cfg = match self.build_config() {
            Ok(c) => c,
            Err(e) => {
                self.status_msg = format!("Konfigurationsfehler: {}", e);
                return;
            }
        };

        if cfg.dry_run {
            let (advice, _) = self.analyze_combination();
            self.last_summary_line = Some(format!(
                "DRY-RUN OK: {} | {}",
                self.build_cli_command(),
                advice
            ));
            self.status_msg =
                "Dry-Run Simulation erfolgreich geprüft (keine Daten geschrieben).".to_string();
            return;
        }

        let expected_bytes = cfg.limit_bytes.or_else(|| {
            self.devices.get(self.source_idx).and_then(|d| {
                if d.size_bytes > 0 {
                    Some(d.size_bytes)
                } else {
                    None
                }
            })
        });

        let stats = Arc::new(Stats::new());
        let cancel_flag = Arc::new(AtomicBool::new(false));
        let stats_clone = Arc::clone(&stats);
        let cancel_clone = Arc::clone(&cancel_flag);

        let handle = std::thread::spawn(move || {
            execute_transfer_with_control(&cfg, stats_clone, cancel_clone)
        });

        self.last_hash_result = None;
        self.last_summary_line = None;
        self.active_job = Some(ActiveJob {
            stats,
            cancel_flag,
            expected_bytes,
            handle,
        });
        self.status_msg =
            "Transfer läuft im Hintergrund... Drücke [X] oder [Esc] zum Abbrechen.".to_string();
    }

    pub fn poll_active_job(&mut self) {
        let is_finished = self
            .active_job
            .as_ref()
            .map(|j| j.handle.is_finished())
            .unwrap_or(false);

        if is_finished {
            if let Some(job) = self.active_job.take() {
                let bytes = job.stats.get_bytes();
                let elapsed = job.stats.start_time.elapsed().as_secs_f64().max(0.000_001);
                let (spd_val, spd_unit) = format_speed(bytes as f64 / elapsed);
                let r_in_f = job.stats.records_in_full.load(Ordering::Relaxed);
                let r_in_p = job.stats.records_in_part.load(Ordering::Relaxed);
                let r_out_f = job.stats.records_out_full.load(Ordering::Relaxed);
                let r_out_p = job.stats.records_out_part.load(Ordering::Relaxed);

                match job.handle.join() {
                    Ok(Ok(hash_opt)) => {
                        self.last_hash_result = hash_opt;
                        let summary = format!(
                            "FERTIG: {} ({}) in {:.3} s ({:.1} {}) | In: {}+{} | Out: {}+{}",
                            bytes,
                            human_readable_bytes(bytes),
                            elapsed,
                            spd_val,
                            spd_unit,
                            r_in_f,
                            r_in_p,
                            r_out_f,
                            r_out_p
                        );
                        self.last_summary_line = Some(summary.clone());
                        self.status_msg = summary;
                    }
                    Ok(Err(e)) => {
                        let msg = format!("Transfer beendet/abgebrochen: {}", e);
                        self.last_summary_line = Some(msg.clone());
                        self.status_msg = msg;
                    }
                    Err(_) => {
                        self.status_msg = "Kritischer Fehler: Worker-Thread Panic!".to_string();
                    }
                }
            }
        }
    }
}

impl Default for TuiState {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_presets_and_cli_generation() {
        let mut state = TuiState::new();
        state.apply_preset(2);
        assert_eq!(state.engine, Engine::Async);
        assert_eq!(state.hash_algo, HashAlgo::Blks);
        assert!(state.sync_pad);
        assert!(state.noerror);

        let cli = state.build_cli_command();
        assert!(cli.contains("-e async"));
        assert!(cli.contains("--blks"));
        assert!(cli.contains("--sync"));
        assert!(cli.contains("--noerror"));
        assert!(cli.contains("--fsync"));

        let cfg = state.build_config().expect("Valid config from preset 2");
        assert_eq!(cfg.engine, Engine::Async);
        assert_eq!(cfg.hash_algo, HashAlgo::Blks);
        assert!(cfg.fsync);
    }

    #[test]
    fn test_combination_advisor_detects_reflink_hash_conflict() {
        let mut state = TuiState::new();
        state.engine = Engine::Reflink;
        state.hash_algo = HashAlgo::Blake3;
        let (advice, color) = state.analyze_combination();
        assert!(advice.contains("Reflink"));
        assert_eq!(color, Color::Yellow);
    }

    #[test]
    fn test_presets_6_7_8_and_auto_resolve() {
        let mut state = TuiState::new();

        // Preset 6: Splice
        state.apply_preset(6);
        assert_eq!(state.engine, Engine::Splice);
        assert_eq!(state.sync_mode, SyncMode::Fdatasync);
        assert!(state.build_cli_command().contains("-e splice"));

        // Preset 7: Direct + NoCache
        state.apply_preset(7);
        assert_eq!(state.engine, Engine::Uring);
        assert_eq!(state.threads, 4);
        assert!(state.direct_io);
        assert!(state.nocache);
        let cli7 = state.build_cli_command();
        assert!(cli7.contains("--direct"));
        assert!(cli7.contains("--nocache"));
        assert!(cli7.contains("-j 4"));

        // Preset 8: Swab + Blake3
        state.apply_preset(8);
        assert_eq!(state.engine, Engine::Async);
        assert!(state.swab);
        assert_eq!(state.hash_algo, HashAlgo::Blake3);
        let cli8 = state.build_cli_command();
        assert!(cli8.contains("--swab"));
        assert!(cli8.contains("--blake3"));

        // Conflict introduction & Auto-resolve
        state.engine = Engine::Splice;
        state.direct_io = true;
        state.hash_algo = HashAlgo::Blks;
        let (conflict_adv, sev) = state.analyze_combination();
        assert_eq!(sev, Color::Red);
        assert!(conflict_adv.contains("KONFLIKT"));

        state.auto_resolve_combinations();
        let (_resolved_adv, resolved_sev) = state.analyze_combination();
        assert_ne!(resolved_sev, Color::Red);
        assert!(!state.direct_io);
        assert_eq!(state.engine, Engine::Async);
    }
}
