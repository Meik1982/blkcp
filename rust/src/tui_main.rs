use blkcp::args::{parse_size, Config, Engine, HashAlgo};
use blkcp::engine::execute_transfer_with_control;
use blkcp::stats::{format_speed, human_readable_bytes, Stats};
use blkcp::tui_device::{
    format_bytes_human, list_directory_entries, scan_block_devices_ext, BlockDevice, EndpointKind,
    FileEntry,
};

use crossterm::{
    event::{self, Event, KeyCode, KeyModifiers},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{
    backend::CrosstermBackend,
    layout::{Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Cell, Clear, Gauge, Paragraph, Row, Table},
    Terminal,
};
use std::io;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FocusPane {
    Source,
    Target,
    Modules,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SyncMode {
    None,
    Fdatasync,
    Fsync,
}

impl SyncMode {
    fn as_str(self) -> &'static str {
        match self {
            SyncMode::None => "none",
            SyncMode::Fdatasync => "fdatasync",
            SyncMode::Fsync => "fsync",
        }
    }

    fn next(self) -> Self {
        match self {
            SyncMode::None => SyncMode::Fdatasync,
            SyncMode::Fdatasync => SyncMode::Fsync,
            SyncMode::Fsync => SyncMode::None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum ModalDialog {
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

struct ActiveJob {
    stats: Arc<Stats>,
    cancel_flag: Arc<AtomicBool>,
    expected_bytes: Option<u64>,
    handle: JoinHandle<anyhow::Result<Option<(String, &'static str)>>>,
}

struct TuiState {
    devices: Vec<BlockDevice>,
    include_partitions: bool,
    source_idx: usize,
    target_idx: usize,
    focus: FocusPane,
    module_idx: usize,

    // Core Module Parameters
    block_size_str: &'static str,
    engine: Engine,
    hash_algo: HashAlgo,
    threads: usize,
    queue_depth_opt: Option<usize>,

    // Combination Flags
    direct_io: bool,
    nocache: bool,
    sparse: bool,
    swab: bool,
    sync_pad: bool,
    notrunc: bool,
    noerror: bool,
    dry_run: bool,
    force_override: bool,
    sync_mode: SyncMode,

    // Offsets & Limits
    limit_str: String,
    skip_str: String,
    seek_str: String,

    // UI & Execution State
    modal: ModalDialog,
    active_job: Option<ActiveJob>,
    last_hash_result: Option<(String, &'static str)>,
    last_summary_line: Option<String>,
    running: bool,
    status_msg: String,
}

impl TuiState {
    fn new() -> Self {
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
                "Bereit. Nutze [Tab] für Fokus, [1-5] für Presets, [F] für Datei-Browser, [Enter] für Start."
                    .to_string(),
        }
    }

    fn refresh_devices(&mut self) {
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

    fn add_or_select_custom_path(&mut self, path: PathBuf, for_target: bool) {
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

    fn apply_preset(&mut self, preset: u8) {
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
            _ => {}
        }
    }

    fn cycle_block_size(&mut self) {
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

    fn cycle_engine(&mut self) {
        self.engine = match self.engine {
            Engine::Auto => Engine::Uring,
            Engine::Uring => Engine::Async,
            Engine::Async => Engine::Reflink,
            Engine::Reflink => Engine::Splice,
            Engine::Splice => Engine::Sync,
            Engine::Sync => Engine::Auto,
        };
    }

    fn cycle_hash(&mut self) {
        self.hash_algo = match self.hash_algo {
            HashAlgo::None => HashAlgo::Blake3,
            HashAlgo::Blake3 => HashAlgo::Blks,
            HashAlgo::Blks => HashAlgo::Sha256,
            HashAlgo::Sha256 => HashAlgo::None,
        };
    }

    fn cycle_threads(&mut self) {
        self.threads = match self.threads {
            1 => 2,
            2 => 4,
            4 => 8,
            8 => 16,
            16 => 32,
            _ => 1,
        };
    }

    fn cycle_queue_depth(&mut self) {
        self.queue_depth_opt = match self.queue_depth_opt {
            None => Some(16),
            Some(16) => Some(32),
            Some(32) => Some(64),
            Some(64) => Some(128),
            _ => None,
        };
    }

    fn toggle_selected_module(&mut self) {
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

    fn build_config(&self) -> anyhow::Result<Config> {
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

    fn build_cli_command(&self) -> String {
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

    fn analyze_combination(&self) -> (String, Color) {
        let mut notes = Vec::new();
        let mut severity = Color::Green;

        let needs_userspace =
            self.hash_algo != HashAlgo::None || self.swab || self.sparse || self.sync_pad;

        if self.engine == Engine::Reflink && needs_userspace {
            notes.push(
                "Reflink + Userspace-Transformation (Hash/Swab/Sparse) inkompatibel -> nutzt Buffered-Streaming"
                    .to_string(),
            );
            severity = Color::Yellow;
        } else if self.engine == Engine::Uring && needs_userspace {
            notes.push(
                "io_uring + Hash/Swab/Sparse gewählt -> transparenter Fallback auf Userspace-Pipeline für bitgenaue Prüfsumme"
                    .to_string(),
            );
            severity = Color::Yellow;
        } else if self.engine == Engine::Uring && self.threads > 1 {
            notes.push(format!(
                "Multi-Ring io_uring aktiv ({} parallele Kernel-Ringe à 64 MiB Stripes)",
                self.threads
            ));
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

        if let Some(src) = self.devices.get(self.source_idx) {
            if src.kind == EndpointKind::VirtualDevice
                && (self.limit_str.trim().is_empty() || self.limit_str.trim() == "0")
            {
                notes.push(format!(
                    "ACHTUNG: Endlos-Stream ({}) ohne Byte-Limit (-l)! Setze [L] Limit oder stoppe manuell mit [X].",
                    src.path.display()
                ));
                severity = Color::Yellow;
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

    fn start_transfer(&mut self) {
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

    fn poll_active_job(&mut self) {
        let is_finished = self
            .active_job
            .as_ref()
            .map(|j| j.handle.is_finished())
            .unwrap_or(false);

        if is_finished {
            if let Some(job) = self.active_job.take() {
                let bytes = job.stats.get_bytes();
                let elapsed = job.stats.start_time.elapsed().as_secs_f64().max(0.000001);
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

fn main() -> Result<(), Box<dyn std::error::Error>> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let mut state = TuiState::new();

    while state.running {
        state.poll_active_job();

        terminal.draw(|f| {
            render_ui(f, &state);
        })?;

        if event::poll(Duration::from_millis(50))? {
            if let Event::Key(key) = event::read()? {
                handle_key_event(&mut state, key.code, key.modifiers);
            }
        }
    }

    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;
    terminal.show_cursor()?;
    Ok(())
}

fn handle_key_event(state: &mut TuiState, code: KeyCode, _modifiers: KeyModifiers) {
    // 1. If a transfer job is actively running, allow immediate cancellation via X or Esc
    if state.active_job.is_some() {
        if matches!(code, KeyCode::Char('x') | KeyCode::Char('X') | KeyCode::Esc) {
            if let Some(ref job) = state.active_job {
                job.cancel_flag.store(true, Ordering::Relaxed);
                state.status_msg = "Abbruchsignal gesendet...".to_string();
            }
        }
        return;
    }

    // 2. Modal Dialog Handling
    match &mut state.modal {
        ModalDialog::None => {}
        ModalDialog::SafetyConfirm => {
            match code {
                KeyCode::Char('y') | KeyCode::Char('Y') | KeyCode::Enter => {
                    state.modal = ModalDialog::None;
                    state.start_transfer();
                }
                KeyCode::Char('n') | KeyCode::Char('N') | KeyCode::Esc => {
                    state.modal = ModalDialog::None;
                    state.status_msg = "Schreibvorgang vom Benutzer abgebrochen.".to_string();
                }
                _ => {}
            }
            return;
        }
        ModalDialog::PathInput { for_target, buffer } => {
            match code {
                KeyCode::Esc => {
                    state.modal = ModalDialog::None;
                }
                KeyCode::Enter => {
                    let trimmed = buffer.trim().to_string();
                    let is_tgt = *for_target;
                    state.modal = ModalDialog::None;
                    if !trimmed.is_empty() {
                        state.add_or_select_custom_path(PathBuf::from(trimmed), is_tgt);
                    }
                }
                KeyCode::Backspace => {
                    buffer.pop();
                }
                KeyCode::Char(c) => {
                    buffer.push(c);
                }
                _ => {}
            }
            return;
        }
        ModalDialog::LimitInput {
            field,
            limit_buf,
            skip_buf,
            seek_buf,
        } => {
            match code {
                KeyCode::Esc => {
                    state.modal = ModalDialog::None;
                }
                KeyCode::Tab | KeyCode::Down => {
                    *field = (*field + 1) % 3;
                }
                KeyCode::BackTab | KeyCode::Up => {
                    *field = (*field + 2) % 3;
                }
                KeyCode::Enter => {
                    state.limit_str = limit_buf.trim().to_string();
                    state.skip_str = skip_buf.trim().to_string();
                    state.seek_str = seek_buf.trim().to_string();
                    state.modal = ModalDialog::None;
                    state.status_msg = format!(
                        "Limits aktualisiert: Limit='{}', Skip='{}', Seek='{}'",
                        state.limit_str, state.skip_str, state.seek_str
                    );
                }
                KeyCode::Backspace => match *field {
                    0 => {
                        limit_buf.pop();
                    }
                    1 => {
                        skip_buf.pop();
                    }
                    _ => {
                        seek_buf.pop();
                    }
                },
                KeyCode::Char(c) => match *field {
                    0 => limit_buf.push(c),
                    1 => skip_buf.push(c),
                    _ => seek_buf.push(c),
                },
                _ => {}
            }
            return;
        }
        ModalDialog::FileBrowser {
            for_target,
            current_dir,
            entries,
            selected_idx,
        } => {
            match code {
                KeyCode::Esc | KeyCode::Char('q') => {
                    state.modal = ModalDialog::None;
                }
                KeyCode::Up | KeyCode::Char('k') => {
                    if *selected_idx > 0 {
                        *selected_idx -= 1;
                    }
                }
                KeyCode::Down | KeyCode::Char('j') => {
                    if *selected_idx + 1 < entries.len() {
                        *selected_idx += 1;
                    }
                }
                KeyCode::Enter => {
                    if let Some(entry) = entries.get(*selected_idx).cloned() {
                        if entry.is_dir {
                            *current_dir = entry.path;
                            *entries = list_directory_entries(current_dir);
                            *selected_idx = 0;
                        } else {
                            let is_tgt = *for_target;
                            let chosen = entry.path;
                            state.modal = ModalDialog::None;
                            state.add_or_select_custom_path(chosen, is_tgt);
                        }
                    }
                }
                _ => {}
            }
            return;
        }
    }

    // 3. Normal Navigation & Module Shortcuts
    match code {
        KeyCode::Char('q') | KeyCode::Esc => state.running = false,
        KeyCode::Tab => {
            state.focus = match state.focus {
                FocusPane::Source => FocusPane::Target,
                FocusPane::Target => FocusPane::Modules,
                FocusPane::Modules => FocusPane::Source,
            };
        }
        KeyCode::BackTab => {
            state.focus = match state.focus {
                FocusPane::Source => FocusPane::Modules,
                FocusPane::Target => FocusPane::Source,
                FocusPane::Modules => FocusPane::Target,
            };
        }
        KeyCode::Up | KeyCode::Char('k') => match state.focus {
            FocusPane::Source if state.source_idx > 0 => state.source_idx -= 1,
            FocusPane::Target if state.target_idx > 0 => state.target_idx -= 1,
            FocusPane::Modules if state.module_idx > 0 => state.module_idx -= 1,
            _ => {}
        },
        KeyCode::Down => match state.focus {
            FocusPane::Source if state.source_idx + 1 < state.devices.len() => {
                state.source_idx += 1
            }
            FocusPane::Target if state.target_idx + 1 < state.devices.len() => {
                state.target_idx += 1
            }
            FocusPane::Modules if state.module_idx + 1 < 16 => state.module_idx += 1,
            _ => {}
        },
        KeyCode::Char(' ') => {
            if state.focus == FocusPane::Modules {
                state.toggle_selected_module();
            }
        }
        // Global Hotkeys for all modules & combinations
        KeyCode::Char('b') | KeyCode::Char('B') => state.cycle_block_size(),
        KeyCode::Char('e') | KeyCode::Char('E') => state.cycle_engine(),
        KeyCode::Char('h') | KeyCode::Char('H') => state.cycle_hash(),
        KeyCode::Char('j') | KeyCode::Char('J') => state.cycle_threads(),
        KeyCode::Char('u') | KeyCode::Char('U') => state.cycle_queue_depth(),
        KeyCode::Char('d') | KeyCode::Char('D') => state.direct_io = !state.direct_io,
        KeyCode::Char('c') | KeyCode::Char('C') => state.nocache = !state.nocache,
        KeyCode::Char('a') | KeyCode::Char('A') => state.sparse = !state.sparse,
        KeyCode::Char('w') | KeyCode::Char('W') => state.swab = !state.swab,
        KeyCode::Char('y') | KeyCode::Char('Y') => state.sync_pad = !state.sync_pad,
        KeyCode::Char('t') | KeyCode::Char('T') => state.notrunc = !state.notrunc,
        KeyCode::Char('g') | KeyCode::Char('G') => state.sync_mode = state.sync_mode.next(),
        KeyCode::Char('m') | KeyCode::Char('M') => state.noerror = !state.noerror,
        KeyCode::Char('n') | KeyCode::Char('N') => state.dry_run = !state.dry_run,
        KeyCode::Char('p') | KeyCode::Char('P') => {
            state.include_partitions = !state.include_partitions;
            state.refresh_devices();
        }
        KeyCode::Char('r') | KeyCode::Char('R') => state.refresh_devices(),
        KeyCode::Char('l') | KeyCode::Char('L') => {
            state.modal = ModalDialog::LimitInput {
                field: 0,
                limit_buf: state.limit_str.clone(),
                skip_buf: state.skip_str.clone(),
                seek_buf: state.seek_str.clone(),
            };
        }
        KeyCode::Char('i') | KeyCode::Char('I') => {
            let cur = state
                .devices
                .get(state.source_idx)
                .map(|d| d.path.to_string_lossy().to_string())
                .unwrap_or_default();
            state.modal = ModalDialog::PathInput {
                for_target: false,
                buffer: cur,
            };
        }
        KeyCode::Char('o') | KeyCode::Char('O') => {
            let cur = state
                .devices
                .get(state.target_idx)
                .map(|d| d.path.to_string_lossy().to_string())
                .unwrap_or_default();
            state.modal = ModalDialog::PathInput {
                for_target: true,
                buffer: cur,
            };
        }
        KeyCode::Char('f') | KeyCode::Char('F') => {
            let for_target = state.focus == FocusPane::Target;
            let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
            let entries = list_directory_entries(&cwd);
            state.modal = ModalDialog::FileBrowser {
                for_target,
                current_dir: cwd,
                entries,
                selected_idx: 0,
            };
        }
        // Workflow Combination Presets 1..5
        KeyCode::Char('1') => state.apply_preset(1),
        KeyCode::Char('2') => state.apply_preset(2),
        KeyCode::Char('3') => state.apply_preset(3),
        KeyCode::Char('4') => state.apply_preset(4),
        KeyCode::Char('5') => state.apply_preset(5),
        KeyCode::Enter => {
            if state.dry_run {
                state.start_transfer();
                return;
            }
            if let Some(target) = state.devices.get(state.target_idx) {
                if target.is_system_root && !state.force_override {
                    state.status_msg =
                        "BLOCKIERT: Ziel ist ein gemountetes System-Laufwerk! (Sicherheits-Sperre aktiv)"
                            .to_string();
                } else if target.path == std::path::Path::new("/dev/null") {
                    // Writing to /dev/null is non-destructive -> start immediately
                    state.start_transfer();
                } else {
                    state.modal = ModalDialog::SafetyConfirm;
                }
            }
        }
        _ => {}
    }
}

fn render_ui(f: &mut ratatui::Frame, state: &TuiState) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3), // 0: Header & Presets
            Constraint::Min(9),    // 1: Source & Target Endpoints
            Constraint::Length(9), // 2: 16-Module Matrix
            Constraint::Length(5), // 3: Live Progress / Combination Advisor & CLI
            Constraint::Length(3), // 4: Footer Telemetry & Controls
        ])
        .split(f.area());

    // 0. Header
    let header = Paragraph::new(Line::from(vec![
        Span::styled(
            " blkcp-tui v2.0 ",
            Style::default()
                .fg(Color::Black)
                .bg(Color::Yellow)
                .add_modifier(Modifier::BOLD),
        ),
        Span::raw(" Presets: "),
        Span::styled("[1] NVMe-Uring ", Style::default().fg(Color::Cyan)),
        Span::styled("[2] Forensik+BLKS ", Style::default().fg(Color::Green)),
        Span::styled("[3] Sparse+BLAKE3 ", Style::default().fg(Color::Magenta)),
        Span::styled("[4] CoW-Reflink ", Style::default().fg(Color::Blue)),
        Span::styled("[5] 2G-Speedtest ", Style::default().fg(Color::Yellow)),
    ]))
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" Modulare Block-Replikations & Krypto-Pipeline "),
    );
    f.render_widget(header, chunks[0]);

    // 1. Source & Target Tables
    let center_chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(50), Constraint::Percentage(50)])
        .split(chunks[1]);

    let source_rows: Vec<Row> = state
        .devices
        .iter()
        .enumerate()
        .map(|(idx, d)| {
            let is_sel = idx == state.source_idx;
            let prefix = if is_sel { "▶ " } else { "  " };
            let mut style = Style::default();
            if is_sel {
                style = style.fg(Color::Yellow).add_modifier(Modifier::BOLD);
            } else if d.kind == EndpointKind::VirtualDevice {
                style = style.fg(Color::Cyan);
            } else if d.kind == EndpointKind::RegularFile {
                style = style.fg(Color::Green);
            }
            Row::new(vec![
                Cell::from(format!("{}{}", prefix, d.path.display())),
                Cell::from(d.size_human()),
                Cell::from(d.model.clone()),
            ])
            .style(style)
        })
        .collect();

    let source_table = Table::new(
        source_rows,
        [
            Constraint::Length(18),
            Constraint::Length(12),
            Constraint::Min(16),
        ],
    )
    .header(
        Row::new(vec!["Quelle (-i)", "Größe", "Typ / Modell"]).style(
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        ),
    )
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" [Quelle] ([I] Pfad, [F] Browser, [P] Partitionen) ")
            .border_style(if state.focus == FocusPane::Source {
                Style::default().fg(Color::Yellow)
            } else {
                Style::default()
            }),
    );
    f.render_widget(source_table, center_chunks[0]);

    let target_rows: Vec<Row> = state
        .devices
        .iter()
        .enumerate()
        .map(|(idx, d)| {
            let is_sel = idx == state.target_idx;
            let prefix = if is_sel { "▶ " } else { "  " };
            let mut style = Style::default();
            if is_sel {
                style = style.fg(Color::Yellow).add_modifier(Modifier::BOLD);
            }
            if d.is_system_root {
                style = style.fg(Color::Red).add_modifier(Modifier::BOLD);
            } else if !d.mount_points.is_empty() {
                style = style.fg(Color::Magenta);
            } else if d.kind == EndpointKind::VirtualDevice {
                style = style.fg(Color::Cyan);
            } else if d.kind == EndpointKind::RegularFile {
                style = style.fg(Color::Green);
            }

            let status_str = if d.is_system_root {
                "SYSTEM / ROOT (SPERRE)".to_string()
            } else if !d.mount_points.is_empty() {
                format!("Mounted: {}", d.mount_points.join(","))
            } else {
                d.model.clone()
            };

            Row::new(vec![
                Cell::from(format!("{}{}", prefix, d.path.display())),
                Cell::from(d.size_human()),
                Cell::from(status_str),
            ])
            .style(style)
        })
        .collect();

    let target_table = Table::new(
        target_rows,
        [
            Constraint::Length(18),
            Constraint::Length(12),
            Constraint::Min(16),
        ],
    )
    .header(
        Row::new(vec!["Ziel (-o)", "Größe", "Safety / Status"]).style(
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        ),
    )
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" [Ziel] ([O] Pfad, [F] Browser, Rot = Geschützt) ")
            .border_style(if state.focus == FocusPane::Target {
                Style::default().fg(Color::Yellow)
            } else {
                Style::default()
            }),
    );
    f.render_widget(target_table, center_chunks[1]);

    // 2. 16-Module Combination Matrix
    render_module_matrix(f, state, chunks[2]);

    // 3. Live Progress OR Combination Advisor + CLI Command
    if let Some(ref job) = state.active_job {
        let bytes = job.stats.get_bytes();
        let elapsed = job.stats.start_time.elapsed().as_secs_f64().max(0.000001);
        let (spd, unit) = format_speed(bytes as f64 / elapsed);
        let r_in = job.stats.records_in_full.load(Ordering::Relaxed);
        let r_out = job.stats.records_out_full.load(Ordering::Relaxed);

        let ratio = if let Some(exp) = job.expected_bytes {
            if exp > 0 {
                (bytes as f64 / exp as f64).clamp(0.0, 1.0)
            } else {
                0.5
            }
        } else {
            0.5
        };

        let label = format!(
            "{} kopiert | {:.1} {} | {:.1} s | Blöcke In: {} Out: {} | [X/Esc] Abbruch",
            human_readable_bytes(bytes),
            spd,
            unit,
            elapsed,
            r_in,
            r_out
        );

        let gauge = Gauge::default()
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" LIVE TRANSFER TELEMETRIE ")
                    .border_style(Style::default().fg(Color::Green)),
            )
            .gauge_style(
                Style::default()
                    .fg(Color::Green)
                    .bg(Color::Black)
                    .add_modifier(Modifier::BOLD),
            )
            .ratio(ratio)
            .label(label);
        f.render_widget(gauge, chunks[3]);
    } else {
        let (advice, adv_color) = state.analyze_combination();
        let cli_cmd = state.build_cli_command();

        let third_line = if let Some((ref digest, algo)) = state.last_hash_result {
            Line::from(vec![
                Span::styled(
                    format!(" Letzter {}-Digest: ", algo.to_uppercase()),
                    Style::default()
                        .fg(Color::Green)
                        .add_modifier(Modifier::BOLD),
                ),
                Span::styled(digest.clone(), Style::default().fg(Color::White)),
            ])
        } else if let Some(ref sum) = state.last_summary_line {
            Line::from(Span::styled(
                format!(" {}", sum),
                Style::default().fg(Color::LightGreen),
            ))
        } else {
            Line::from(vec![
                Span::styled(
                    " Äquivalenter CLI-Befehl: ",
                    Style::default().fg(Color::Cyan),
                ),
                Span::styled(cli_cmd, Style::default().fg(Color::White)),
            ])
        };

        let advisor_lines = vec![
            Line::from(vec![
                Span::styled(
                    " Pipeline-Analyse: ",
                    Style::default()
                        .fg(Color::Yellow)
                        .add_modifier(Modifier::BOLD),
                ),
                Span::styled(advice, Style::default().fg(adv_color)),
            ]),
            Line::from(vec![
                Span::styled(" CLI: ", Style::default().fg(Color::Cyan)),
                Span::raw(state.build_cli_command()),
            ]),
            third_line,
        ];

        let advisor = Paragraph::new(advisor_lines).block(
            Block::default()
                .borders(Borders::ALL)
                .title(" Kombinations-Advisor, CLI-Generator & Prüfsumme "),
        );
        f.render_widget(advisor, chunks[3]);
    }

    // 4. Footer
    let footer_text = format!(
        " {} | [Tab]: Bereich | [Space]: Modul ändern | [Enter]: Start | [Q]: Beenden",
        state.status_msg
    );
    let footer = Paragraph::new(footer_text)
        .style(Style::default().fg(Color::LightCyan))
        .block(Block::default().borders(Borders::ALL));
    f.render_widget(footer, chunks[4]);

    // 5. Render Active Modal Dialog
    render_modal(f, state);
}

fn render_module_matrix(f: &mut ratatui::Frame, state: &TuiState, area: Rect) {
    let eng_name = match state.engine {
        Engine::Auto => "auto",
        Engine::Uring => "uring (Kernel Ring)",
        Engine::Async => "async (3-Stage Ring)",
        Engine::Reflink => "reflink (Zero-Copy)",
        Engine::Splice => "splice (Pipe)",
        Engine::Sync => "sync (Standard)",
    };
    let hash_name = match state.hash_algo {
        HashAlgo::None => "none",
        HashAlgo::Blake3 => "blake3 (SIMD)",
        HashAlgo::Blks => "blks (BLKS-384 PQ)",
        HashAlgo::Sha256 => "sha256 (FIPS)",
    };
    let qd_name = state
        .queue_depth_opt
        .map(|q| q.to_string())
        .unwrap_or_else(|| "auto".to_string());

    let items: [(usize, &str, String, bool); 16] = [
        (0, "[B] Blockgröße", state.block_size_str.to_string(), true),
        (1, "[E] I/O-Engine", eng_name.to_string(), true),
        (
            2,
            "[H] Krypto-Hash",
            hash_name.to_string(),
            state.hash_algo != HashAlgo::None,
        ),
        (
            3,
            "[J] Shards/Threads",
            format!("{}", state.threads),
            state.threads > 1,
        ),
        (
            4,
            "[U] Queue-Depth",
            qd_name,
            state.queue_depth_opt.is_some(),
        ),
        (
            5,
            "[D] Direct I/O",
            if state.direct_io { "AN" } else { "aus" }.to_string(),
            state.direct_io,
        ),
        (
            6,
            "[C] No-Cache",
            if state.nocache { "AN" } else { "aus" }.to_string(),
            state.nocache,
        ),
        (
            7,
            "[A] Sparse-Holes",
            if state.sparse { "AN" } else { "aus" }.to_string(),
            state.sparse,
        ),
        (
            8,
            "[W] AVX2 Swab",
            if state.swab { "AN" } else { "aus" }.to_string(),
            state.swab,
        ),
        (
            9,
            "[Y] Sync-Pad",
            if state.sync_pad { "AN" } else { "aus" }.to_string(),
            state.sync_pad,
        ),
        (
            10,
            "[T] No-Truncate",
            if state.notrunc { "AN" } else { "aus" }.to_string(),
            state.notrunc,
        ),
        (
            11,
            "[G] Flush-Sync",
            state.sync_mode.as_str().to_string(),
            state.sync_mode != SyncMode::None,
        ),
        (
            12,
            "[M] Ignore-Err",
            if state.noerror { "AN" } else { "aus" }.to_string(),
            state.noerror,
        ),
        (
            13,
            "[N] Dry-Run",
            if state.dry_run { "AN" } else { "aus" }.to_string(),
            state.dry_run,
        ),
        (
            14,
            "[L] Limit/Offsets",
            format!(
                "L:{} S:{}/{}",
                if state.limit_str.is_empty() {
                    "∞"
                } else {
                    &state.limit_str
                },
                state.skip_str,
                state.seek_str
            ),
            !state.limit_str.is_empty() && state.limit_str != "0",
        ),
        (
            15,
            "[Force] Guard-Bypass",
            if state.force_override { "AN!" } else { "aus" }.to_string(),
            state.force_override,
        ),
    ];

    let mut rows = Vec::new();
    for r in 0..4 {
        let mut cells = Vec::new();
        for c in 0..4 {
            let idx = r + c * 4;
            let (i, label, val, active) = &items[idx];
            let is_sel = state.focus == FocusPane::Modules && state.module_idx == *i;
            let marker = if is_sel { "▶" } else { " " };
            let color = if is_sel {
                Color::Yellow
            } else if *i == 15 && *active {
                Color::Red
            } else if *active {
                Color::Green
            } else {
                Color::DarkGray
            };
            cells.push(
                Cell::from(format!("{}{}: {}", marker, label, val)).style(
                    Style::default()
                        .fg(color)
                        .add_modifier(if is_sel || *active {
                            Modifier::BOLD
                        } else {
                            Modifier::empty()
                        }),
                ),
            );
        }
        rows.push(Row::new(cells));
    }

    let table = Table::new(
        rows,
        [
            Constraint::Percentage(25),
            Constraint::Percentage(25),
            Constraint::Percentage(25),
            Constraint::Percentage(25),
        ],
    )
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" [Modul-Matrix & Flags] (Direkt-Taste oder Pfeiltasten + Space) ")
            .border_style(if state.focus == FocusPane::Modules {
                Style::default().fg(Color::Yellow)
            } else {
                Style::default()
            }),
    );
    f.render_widget(table, area);
}

fn render_modal(f: &mut ratatui::Frame, state: &TuiState) {
    match &state.modal {
        ModalDialog::None => {}
        ModalDialog::SafetyConfirm => {
            let area = centered_rect(68, 36, f.area());
            f.render_widget(Clear, area);

            let src_dev = &state.devices[state.source_idx];
            let target_dev = &state.devices[state.target_idx];
            let modal_lines = vec![
                Line::from(Span::styled(
                    "ACHTUNG: SCHREIBZUGRIFF AUF ZIEL-ENDPUNKT!",
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                )),
                Line::from(""),
                Line::from(format!(
                    "Quelle: {} ({})",
                    src_dev.path.display(),
                    src_dev.size_human()
                )),
                Line::from(format!(
                    "Ziel:   {} ({}, {})",
                    target_dev.path.display(),
                    target_dev.size_human(),
                    target_dev.model
                )),
                Line::from(format!("Befehl: {}", state.build_cli_command())),
                Line::from(""),
                Line::from(vec![
                    Span::raw("Ausführung starten? "),
                    Span::styled(
                        "[Y / Enter] JA, STARTEN",
                        Style::default()
                            .fg(Color::Green)
                            .add_modifier(Modifier::BOLD),
                    ),
                    Span::raw("  |  "),
                    Span::styled(
                        "[N / Esc] ABBRECHEN",
                        Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                    ),
                ]),
            ];

            let modal = Paragraph::new(modal_lines).block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" TARGET SAFETY GUARD - BESTÄTIGUNG ")
                    .border_style(Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)),
            );
            f.render_widget(modal, area);
        }
        ModalDialog::PathInput { for_target, buffer } => {
            let area = centered_rect(65, 25, f.area());
            f.render_widget(Clear, area);
            let title = if *for_target {
                " Ziel-Dateipfad / Blockgerät eingeben (-o) "
            } else {
                " Quell-Dateipfad / Image eingeben (-i) "
            };
            let lines = vec![
                Line::from("Gib einen absoluten oder relativen Dateipfad ein:"),
                Line::from(""),
                Line::from(Span::styled(
                    format!("> {}_", buffer),
                    Style::default()
                        .fg(Color::Yellow)
                        .add_modifier(Modifier::BOLD),
                )),
                Line::from(""),
                Line::from("[Enter]: Übernehmen  |  [Esc]: Abbrechen"),
            ];
            let modal = Paragraph::new(lines).block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(title)
                    .border_style(Style::default().fg(Color::Yellow)),
            );
            f.render_widget(modal, area);
        }
        ModalDialog::LimitInput {
            field,
            limit_buf,
            skip_buf,
            seek_buf,
        } => {
            let area = centered_rect(65, 35, f.area());
            f.render_widget(Clear, area);
            let f_style = |idx: usize| {
                if *field == idx {
                    Style::default()
                        .fg(Color::Yellow)
                        .add_modifier(Modifier::BOLD)
                } else {
                    Style::default().fg(Color::White)
                }
            };
            let lines = vec![
                Line::from("Größenangaben unterstützen Suffixe wie K, M, G, MiB, GiB, b (512B):"),
                Line::from(""),
                Line::from(Span::styled(
                    format!(
                        "{} Byte-Limit (-l, leer/0 = unbegrenzt): {}",
                        if *field == 0 { "▶" } else { " " },
                        limit_buf
                    ),
                    f_style(0),
                )),
                Line::from(Span::styled(
                    format!(
                        "{} Input Skip-Offset (--skip):          {}",
                        if *field == 1 { "▶" } else { " " },
                        skip_buf
                    ),
                    f_style(1),
                )),
                Line::from(Span::styled(
                    format!(
                        "{} Output Seek-Offset (--seek):         {}",
                        if *field == 2 { "▶" } else { " " },
                        seek_buf
                    ),
                    f_style(2),
                )),
                Line::from(""),
                Line::from("[Tab / Pfeile]: Feld wechseln | [Enter]: Speichern | [Esc]: Abbrechen"),
            ];
            let modal = Paragraph::new(lines).block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" Byte-Limit & Offsets konfigurieren ")
                    .border_style(Style::default().fg(Color::Cyan)),
            );
            f.render_widget(modal, area);
        }
        ModalDialog::FileBrowser {
            for_target,
            current_dir,
            entries,
            selected_idx,
        } => {
            let area = centered_rect(75, 65, f.area());
            f.render_widget(Clear, area);
            let title = format!(
                " Datei-Browser ({}) - {} ",
                if *for_target { "Ziel -o" } else { "Quelle -i" },
                current_dir.display()
            );

            let start = selected_idx.saturating_sub(10);
            let rows: Vec<Row> = entries
                .iter()
                .enumerate()
                .skip(start)
                .take(15)
                .map(|(idx, e)| {
                    let is_sel = idx == *selected_idx;
                    let style = if is_sel {
                        Style::default()
                            .fg(Color::Yellow)
                            .add_modifier(Modifier::BOLD)
                    } else if e.is_dir {
                        Style::default().fg(Color::Cyan)
                    } else {
                        Style::default().fg(Color::White)
                    };
                    Row::new(vec![
                        Cell::from(format!("{}{}", if is_sel { "▶ " } else { "  " }, e.name)),
                        Cell::from(if e.is_dir {
                            "<DIR>".to_string()
                        } else {
                            format_bytes_human(e.size_bytes)
                        }),
                    ])
                    .style(style)
                })
                .collect();

            let table = Table::new(rows, [Constraint::Min(40), Constraint::Length(16)])
                .header(
                    Row::new(vec![
                        "Name ([Enter] Öffnen/Wählen, [Esc] Schließen)",
                        "Größe",
                    ])
                    .style(Style::default().fg(Color::Green)),
                )
                .block(
                    Block::default()
                        .borders(Borders::ALL)
                        .title(title)
                        .border_style(Style::default().fg(Color::Yellow)),
                );
            f.render_widget(table, area);
        }
    }
}

fn centered_rect(percent_x: u16, percent_y: u16, r: Rect) -> Rect {
    let popup_layout = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Percentage((100 - percent_y) / 2),
            Constraint::Percentage(percent_y),
            Constraint::Percentage((100 - percent_y) / 2),
        ])
        .split(r);

    Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage((100 - percent_x) / 2),
            Constraint::Percentage(percent_x),
            Constraint::Percentage((100 - percent_x) / 2),
        ])
        .split(popup_layout[1])[1]
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
}
