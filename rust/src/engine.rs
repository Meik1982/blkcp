use anyhow::{anyhow, Result};
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use crate::aligned::AlignedBuffer;
use crate::args::{Config, Engine, HashAlgo};
use crate::hash::InFlightHasher;
use crate::stats::Stats;
use crate::swab::swab_buffer;

#[cfg(unix)]
fn install_signal_handler(stats: Arc<Stats>) {
    use signal_hook::consts::signal::SIGUSR1;
    use signal_hook::iterator::Signals;

    std::thread::spawn(move || {
        if let Ok(mut signals) = Signals::new([SIGUSR1]) {
            for _ in signals.forever() {
                stats.print_instant_telemetry();
            }
        }
    });
}

#[cfg(not(unix))]
fn install_signal_handler(_stats: Arc<Stats>) {}

#[cfg(target_os = "linux")]
fn evict_page_cache(file: &File) {
    use nix::fcntl::{posix_fadvise, PosixFadviseAdvice};
    let _ = posix_fadvise(
        file.as_raw_fd(),
        0,
        0,
        PosixFadviseAdvice::POSIX_FADV_DONTNEED,
    );
}

#[cfg(not(target_os = "linux"))]
fn evict_page_cache(_file: &File) {}

fn is_regular_file_or_new(path: &Path) -> bool {
    if let Ok(metadata) = std::fs::metadata(path) {
        #[cfg(unix)]
        {
            use std::os::unix::fs::FileTypeExt;
            let file_type = metadata.file_type();
            file_type.is_file() && !file_type.is_block_device() && !file_type.is_char_device()
        }
        #[cfg(not(unix))]
        {
            metadata.is_file()
        }
    } else {
        true
    }
}

fn open_input_file(path: &Path, direct: bool) -> Result<File> {
    if direct {
        let mut opts = OpenOptions::new();
        opts.read(true).custom_flags(libc::O_DIRECT);
        if let Ok(f) = opts.open(path) {
            return Ok(f);
        }
    }
    OpenOptions::new()
        .read(true)
        .open(path)
        .map_err(|e| anyhow!("Failed to open input '{}': {}", path.display(), e))
}

fn open_output_file(path: &Path, notrunc: bool, direct: bool) -> Result<File> {
    let should_trunc = !notrunc && is_regular_file_or_new(path);
    if direct {
        let mut opts = OpenOptions::new();
        opts.write(true)
            .create(true)
            .truncate(should_trunc)
            .custom_flags(libc::O_DIRECT);
        if let Ok(f) = opts.open(path) {
            return Ok(f);
        }
    }
    OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(should_trunc)
        .open(path)
        .map_err(|e| anyhow!("Failed to open output '{}': {}", path.display(), e))
}

fn can_use_reflink(cfg: &Config) -> bool {
    if cfg.hash_algo != HashAlgo::None
        || cfg.swab
        || cfg.sparse
        || cfg.sync
        || cfg.direct
        || cfg.nocache
    {
        return false;
    }

    match cfg.engine {
        Engine::Reflink => true,
        Engine::Auto => !cfg.autotune,
        _ => false,
    }
}

#[cfg(target_os = "linux")]
fn try_reflink_copy(
    in_file: &mut File,
    out_file: &mut File,
    cfg: &Config,
    stats: &Stats,
    cancel_flag: &AtomicBool,
) -> Result<bool> {
    use nix::sys::stat::{fstat, SFlag};

    let st_in = match fstat(in_file.as_raw_fd()) {
        Ok(st) => st,
        Err(_) => return Ok(false),
    };
    let st_out = match fstat(out_file.as_raw_fd()) {
        Ok(st) => st,
        Err(_) => return Ok(false),
    };

    let in_mode = SFlag::from_bits_truncate(st_in.st_mode);
    let out_mode = SFlag::from_bits_truncate(st_out.st_mode);

    if !in_mode.contains(SFlag::S_IFREG) || !out_mode.contains(SFlag::S_IFREG) {
        if cfg.engine == Engine::Reflink {
            return Err(anyhow!(
                "copy_file_range requires regular files for input and output"
            ));
        }
        return Ok(false);
    }

    let bs = cfg.block_size.max(1);
    let chunk_size = (64 * 1024 * 1024).max(bs); // 64 MiB
    let mut remaining = cfg.limit_bytes;
    if let Some(cnt) = cfg.count {
        let total_from_count = cnt.saturating_mul(bs as u64);
        remaining = Some(remaining.map_or(total_from_count, |r| r.min(total_from_count)));
    }

    loop {
        if cancel_flag.load(Ordering::Relaxed) {
            return Err(anyhow!("Transfer cancelled by user"));
        }
        if let Some(0) = remaining {
            break;
        }

        let to_copy = if let Some(rem) = remaining {
            (rem as usize).min(chunk_size)
        } else {
            chunk_size
        };

        match nix::fcntl::copy_file_range(&mut *in_file, None, &mut *out_file, None, to_copy) {
            Ok(0) => break, // EOF
            Ok(n) => {
                stats.add_bytes(n as u64);
                stats
                    .records_in_full
                    .fetch_add((n / bs) as u64, Ordering::Relaxed);
                if !n.is_multiple_of(bs) {
                    stats.inc_in_part();
                }
                stats
                    .records_out_full
                    .fetch_add((n / bs) as u64, Ordering::Relaxed);
                if !n.is_multiple_of(bs) {
                    stats.inc_out_part();
                }
                if let Some(ref mut rem) = remaining {
                    *rem = rem.saturating_sub(n as u64);
                }
            }
            Err(nix_err) => {
                let raw_err = nix_err as i32;
                if raw_err == libc::EINTR {
                    continue;
                }
                if stats.get_bytes() == 0
                    && (raw_err == libc::EXDEV
                        || raw_err == libc::EINVAL
                        || raw_err == libc::ENOSYS
                        || raw_err == libc::EOPNOTSUPP)
                {
                    if cfg.engine == Engine::Reflink {
                        return Err(anyhow!(
                            "copy_file_range not supported for these files: {}",
                            nix_err
                        ));
                    }
                    return Ok(false); // fallback to standard copy
                }
                return Err(anyhow!("copy_file_range error: {}", nix_err));
            }
        }
    }

    Ok(true)
}

#[cfg(not(target_os = "linux"))]
fn try_reflink_copy(
    _in_file: &mut File,
    _out_file: &mut File,
    _cfg: &Config,
    _stats: &Stats,
    _cancel_flag: &AtomicBool,
) -> Result<bool> {
    Ok(false) // Portable fallback
}

#[cfg(target_os = "linux")]
fn try_splice_copy<FdIn: std::os::fd::AsFd, FdOut: std::os::fd::AsFd>(
    in_fd: FdIn,
    out_fd: FdOut,
    cfg: &Config,
    stats: &Stats,
    cancel_flag: &AtomicBool,
) -> Result<bool> {
    let (pipe_r, pipe_w) = match nix::unistd::pipe() {
        Ok(p) => p,
        Err(_) => return Ok(false),
    };

    let bs = cfg.block_size.max(1);
    let chunk_size = bs.clamp(4096, 1024 * 1024);
    let mut remaining = cfg.limit_bytes;
    if let Some(cnt) = cfg.count {
        let total = cnt.saturating_mul(bs as u64);
        remaining = Some(remaining.map_or(total, |r| r.min(total)));
    }

    let flags = nix::fcntl::SpliceFFlags::SPLICE_F_MOVE | nix::fcntl::SpliceFFlags::SPLICE_F_MORE;

    loop {
        if cancel_flag.load(Ordering::Relaxed) {
            return Err(anyhow!("Transfer cancelled by user"));
        }
        if let Some(0) = remaining {
            break;
        }

        let to_splice = if let Some(rem) = remaining {
            (rem as usize).min(chunk_size)
        } else {
            chunk_size
        };

        let n_in = match nix::fcntl::splice(&in_fd, None, &pipe_w, None, to_splice, flags) {
            Ok(0) => break, // EOF
            Ok(n) => n,
            Err(nix_err) => {
                let raw = nix_err as i32;
                if raw == libc::EINTR {
                    continue;
                }
                if stats.get_bytes() == 0
                    && (raw == libc::EINVAL || raw == libc::ENOSYS || raw == libc::EBADF)
                {
                    return Ok(false); // Graceful fallback if descriptor does not support splice
                }
                return Err(anyhow!("splice input error: {}", nix_err));
            }
        };

        let mut in_pipe = n_in;
        while in_pipe > 0 {
            let n_out = match nix::fcntl::splice(&pipe_r, None, &out_fd, None, in_pipe, flags) {
                Ok(n) => n,
                Err(nix_err) => {
                    let raw = nix_err as i32;
                    if raw == libc::EINTR {
                        continue;
                    }
                    return Err(anyhow!("splice output error: {}", nix_err));
                }
            };
            in_pipe -= n_out;
        }

        let copied = n_in;
        stats.add_bytes(copied as u64);
        stats
            .records_in_full
            .fetch_add((copied / bs) as u64, Ordering::Relaxed);
        if !copied.is_multiple_of(bs) {
            stats.inc_in_part();
        }
        stats
            .records_out_full
            .fetch_add((copied / bs) as u64, Ordering::Relaxed);
        if !copied.is_multiple_of(bs) {
            stats.inc_out_part();
        }

        if let Some(ref mut rem) = remaining {
            *rem = rem.saturating_sub(copied as u64);
        }
    }

    Ok(true)
}

fn execute_dry_run(cfg: &Config) -> Result<()> {
    let (in_path_str, in_type, in_exists, in_size) = if let Some(ref p) = cfg.input {
        let p_str = p.to_string_lossy().to_string();
        if let Ok(meta) = std::fs::metadata(p) {
            #[cfg(unix)]
            let t = {
                use std::os::unix::fs::FileTypeExt;
                let ft = meta.file_type();
                if ft.is_block_device() {
                    "block_device"
                } else if ft.is_char_device() {
                    "character_device"
                } else if ft.is_fifo() {
                    "fifo_pipe"
                } else if ft.is_dir() {
                    "directory"
                } else {
                    "regular_file"
                }
            };
            #[cfg(not(unix))]
            let t = if meta.is_dir() {
                "directory"
            } else {
                "regular_file"
            };
            (p_str, t, true, meta.len() as i64)
        } else {
            (p_str, "missing", false, -1i64)
        }
    } else {
        ("stdin".to_string(), "fifo_pipe", true, -1i64)
    };

    let (out_path_str, out_type, out_exists, out_size) = if let Some(ref p) = cfg.output {
        let p_str = p.to_string_lossy().to_string();
        if let Ok(meta) = std::fs::metadata(p) {
            #[cfg(unix)]
            let t = {
                use std::os::unix::fs::FileTypeExt;
                let ft = meta.file_type();
                if ft.is_block_device() {
                    "block_device"
                } else if ft.is_char_device() {
                    "character_device"
                } else if ft.is_fifo() {
                    "fifo_pipe"
                } else if ft.is_dir() {
                    "directory"
                } else {
                    "regular_file"
                }
            };
            #[cfg(not(unix))]
            let t = if meta.is_dir() {
                "directory"
            } else {
                "regular_file"
            };
            (p_str, t, true, meta.len() as i64)
        } else {
            (p_str, "new_file", false, -1i64)
        }
    } else {
        ("stdout".to_string(), "fifo_pipe", true, -1i64)
    };

    let (guard_status, guard_detail) = if let Some(ref p) = cfg.output {
        if let Some(detail) = crate::safety::detect_mounted_violation(p) {
            if cfg.force {
                ("overridden", detail)
            } else {
                ("blocked", detail)
            }
        } else {
            ("ok", String::new())
        }
    } else {
        ("ok", String::new())
    };

    let engine_name = match cfg.engine {
        Engine::Uring => "io_uring",
        Engine::Reflink => "reflink",
        Engine::Splice => "splice",
        Engine::Async => "async",
        Engine::Sync => "sync",
        Engine::Auto => {
            if can_use_reflink(cfg)
                && in_type == "regular_file"
                && (out_type == "regular_file" || out_type == "new_file")
            {
                "reflink"
            } else if in_type == "block_device" || out_type == "block_device" || cfg.direct {
                "io_uring"
            } else {
                "sync"
            }
        }
    };

    let effective_bs = cfg.block_size.max(1);
    let limit_val: i64 = cfg.limit_bytes.map(|v| v as i64).unwrap_or(-1);

    if cfg.json {
        println!(
            "{{\"event\":\"dry_run\",\"dry_run\":true,\"input\":{{\"path\":\"{}\",\"type\":\"{}\",\"exists\":{},\"size_bytes\":{}}},\"output\":{{\"path\":\"{}\",\"type\":\"{}\",\"exists\":{},\"size_bytes\":{},\"safety_guard\":\"{}\",\"safety_guard_detail\":\"{}\"}},\"plan\":{{\"engine\":\"{}\",\"blocksize\":{},\"autotune\":{},\"direct_io\":{},\"nocache\":{},\"sha256\":{},\"limit_bytes\":{},\"skip_bytes\":{},\"seek_bytes\":{}}}}}",
            in_path_str,
            in_type,
            in_exists,
            in_size,
            out_path_str,
            out_type,
            out_exists,
            out_size,
            guard_status,
            guard_detail,
            engine_name,
            effective_bs,
            cfg.autotune,
            cfg.direct,
            cfg.nocache,
            cfg.hash_algo != HashAlgo::None,
            limit_val,
            cfg.skip_bytes,
            cfg.seek_bytes
        );
    } else {
        println!("=== blkcp Dry-Run Execution Plan ===");
        println!(
            "Input:        {} ({}, {} bytes)",
            in_path_str, in_type, in_size
        );
        println!(
            "Output:       {} ({}, {} bytes)",
            out_path_str, out_type, out_size
        );
        println!("Safety Guard: {} {}", guard_status, guard_detail);
        println!("Engine:       {}", engine_name);
        println!("Block Size:   {} bytes", effective_bs);
        println!(
            "Note: Dry-run simulation mode (-n / --dry-run) active. No data was transferred or modified."
        );
    }

    Ok(())
}

pub fn execute_transfer(cfg: &Config) -> Result<()> {
    if cfg.dry_run {
        return execute_dry_run(cfg);
    }

    let stats = Arc::new(Stats::new());
    install_signal_handler(Arc::clone(&stats));
    let cancel_flag = Arc::new(AtomicBool::new(false));

    let hash_info = execute_transfer_with_control(cfg, Arc::clone(&stats), cancel_flag)?;
    stats.print_summary(cfg.quiet, cfg.json, hash_info);
    Ok(())
}

pub fn execute_transfer_with_control(
    cfg: &Config,
    stats: Arc<Stats>,
    cancel_flag: Arc<AtomicBool>,
) -> Result<Option<(String, &'static str)>> {
    // 1. Fast-path: Kernel Zero-Copy Reflink
    if can_use_reflink(cfg) && cfg.threads <= 1 {
        if let (Some(ref in_path), Some(ref out_path)) = (&cfg.input, &cfg.output) {
            if let (Ok(mut in_f), Ok(mut out_f)) = (
                open_input_file(in_path, false),
                open_output_file(out_path, cfg.notrunc, false),
            ) {
                if cfg.skip_bytes > 0 {
                    let _ = in_f.seek(SeekFrom::Start(cfg.skip_bytes));
                }
                if cfg.seek_bytes > 0 {
                    let _ = out_f.seek(SeekFrom::Start(cfg.seek_bytes));
                }

                match try_reflink_copy(&mut in_f, &mut out_f, cfg, &stats, &cancel_flag) {
                    Ok(true) => {
                        if cfg.fdatasync || cfg.fsync {
                            let _ = out_f.sync_all();
                        }
                        return Ok(None);
                    }
                    Ok(false) => {
                        // Fallback to standard transfer
                    }
                    Err(e) => return Err(e),
                }
            }
        }
    }

    // 2. Kernel Zero-Copy Pipe Splicing (-e splice)
    #[cfg(target_os = "linux")]
    if cfg.engine == Engine::Splice
        && cfg.hash_algo == HashAlgo::None
        && !cfg.swab
        && !cfg.sparse
        && !cfg.sync
    {
        let in_file_holder = if let Some(ref in_path) = cfg.input {
            let mut f = open_input_file(in_path, cfg.direct)?;
            if cfg.skip_bytes > 0 {
                let _ = f.seek(SeekFrom::Start(cfg.skip_bytes));
            }
            Some(f)
        } else {
            None
        };

        let out_file_holder = if let Some(ref out_path) = cfg.output {
            let mut f = open_output_file(out_path, cfg.notrunc, cfg.direct)?;
            if cfg.seek_bytes > 0 {
                let _ = f.seek(SeekFrom::Start(cfg.seek_bytes));
            }
            Some(f)
        } else {
            None
        };

        let s_in = std::io::stdin();
        let s_out = std::io::stdout();
        use std::os::fd::AsFd;
        let in_fd = in_file_holder
            .as_ref()
            .map(|f| f.as_fd())
            .unwrap_or_else(|| s_in.as_fd());
        let out_fd = out_file_holder
            .as_ref()
            .map(|f| f.as_fd())
            .unwrap_or_else(|| s_out.as_fd());

        if let Ok(true) = try_splice_copy(in_fd, out_fd, cfg, &stats, &cancel_flag) {
            if let Some(ref f) = in_file_holder {
                if cfg.nocache {
                    evict_page_cache(f);
                }
            }
            if let Some(ref f) = out_file_holder {
                if cfg.fdatasync {
                    let _ = f.sync_data();
                } else if cfg.fsync {
                    let _ = f.sync_all();
                }
                if cfg.nocache {
                    evict_page_cache(f);
                }
            }
            return Ok(None);
        }
    }

    let mut hasher = InFlightHasher::new(cfg.hash_algo);

    // 3. Asynchronous 3-Stage Ringbuffer Pipeline (-e async)
    if cfg.engine == Engine::Async {
        let mut in_file_opt: Option<File> = None;
        let input_reader: Box<dyn Read + Send> = if let Some(ref in_path) = cfg.input {
            let mut f = open_input_file(in_path, cfg.direct)?;
            if cfg.skip_bytes > 0 {
                f.seek(SeekFrom::Start(cfg.skip_bytes)).map_err(|e| {
                    anyhow!("Failed to seek input to offset {}: {}", cfg.skip_bytes, e)
                })?;
            }
            in_file_opt = f.try_clone().ok();
            Box::new(f)
        } else {
            Box::new(std::io::stdin())
        };

        let mut out_raw_file: Option<File> = if let Some(ref out_path) = cfg.output {
            let mut f = open_output_file(out_path, cfg.notrunc, cfg.direct)?;
            if cfg.seek_bytes > 0 {
                f.seek(SeekFrom::Start(cfg.seek_bytes)).map_err(|e| {
                    anyhow!("Failed to seek output to offset {}: {}", cfg.seek_bytes, e)
                })?;
            }
            Some(f)
        } else {
            None
        };

        execute_async_transfer(
            input_reader,
            &mut out_raw_file,
            cfg,
            &stats,
            &mut hasher,
            &cancel_flag,
        )?;

        if let Some(ref f) = in_file_opt {
            if cfg.nocache {
                evict_page_cache(f);
            }
        }
        if let Some(ref f) = out_raw_file {
            if cfg.fdatasync {
                let _ = f.sync_data();
            } else if cfg.fsync {
                let _ = f.sync_all();
            }
            if cfg.nocache {
                evict_page_cache(f);
            }
        }

        return Ok(hasher.finalize_string());
    }

    // 4. io_uring native execution (Linux only, when input & output are seekable files/devices and no in-flight transform is active)
    #[cfg(target_os = "linux")]
    if (cfg.engine == Engine::Uring || (cfg.threads > 1 && cfg.engine == Engine::Auto))
        && !cfg.swab
        && !cfg.sparse
        && !cfg.sync
    {
        if let (Some(ref in_path), Some(ref out_path)) = (&cfg.input, &cfg.output) {
            if is_regular_file_or_new(in_path) || in_path.to_string_lossy().starts_with("/dev/") {
                let in_f = open_input_file(in_path, cfg.direct)?;
                let out_f = open_output_file(out_path, cfg.notrunc, cfg.direct)?;

                if let Ok(hash_res) =
                    crate::uring::execute_uring_transfer(&in_f, &out_f, cfg, &stats, &cancel_flag)
                {
                    if cfg.nocache {
                        evict_page_cache(&in_f);
                        evict_page_cache(&out_f);
                    }
                    if cfg.fdatasync {
                        let _ = out_f.sync_data();
                    } else if cfg.fsync {
                        let _ = out_f.sync_all();
                    }
                    return Ok(hash_res);
                }
            }
        }
    }

    // 5. Synchronous / Streaming / Sparse / Hashed Transfer Loop
    let mut in_file_opt: Option<File> = None;
    let mut input_file: Box<dyn Read> = if let Some(ref in_path) = cfg.input {
        let mut f = open_input_file(in_path, cfg.direct)?;
        if cfg.skip_bytes > 0 {
            f.seek(SeekFrom::Start(cfg.skip_bytes))
                .map_err(|e| anyhow!("Failed to seek input to offset {}: {}", cfg.skip_bytes, e))?;
        }
        in_file_opt = f.try_clone().ok();
        Box::new(f)
    } else {
        Box::new(std::io::stdin())
    };

    let mut out_raw_file: Option<File> = if let Some(ref out_path) = cfg.output {
        let mut f = open_output_file(out_path, cfg.notrunc, cfg.direct)?;
        if cfg.seek_bytes > 0 {
            f.seek(SeekFrom::Start(cfg.seek_bytes)).map_err(|e| {
                anyhow!("Failed to seek output to offset {}: {}", cfg.seek_bytes, e)
            })?;
        }
        Some(f)
    } else {
        None
    };

    let mut stdout_handle = std::io::stdout();

    let block_size = cfg.block_size.max(1);
    let mut buffer = AlignedBuffer::new(block_size, 4096);

    let mut remaining_bytes = cfg.limit_bytes;
    let mut remaining_blocks = cfg.count;
    let mut last_was_sparse_hole = false;

    loop {
        if cancel_flag.load(Ordering::Relaxed) {
            return Err(anyhow!("Transfer cancelled by user"));
        }
        if let Some(0) = remaining_bytes {
            break;
        }
        if let Some(0) = remaining_blocks {
            break;
        }

        let to_read = if let Some(rem) = remaining_bytes {
            (rem as usize).min(block_size)
        } else {
            block_size
        };

        let bytes_read = match input_file.read(&mut buffer[..to_read]) {
            Ok(0) => break, // EOF
            Ok(n) => n,
            Err(ref e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => {
                if cfg.noerror {
                    eprintln!("blkcp: read error: {}, continuing...", e);
                    continue;
                } else {
                    return Err(anyhow!("Read error: {}", e));
                }
            }
        };

        if bytes_read == block_size {
            stats.inc_in_full();
        } else {
            stats.inc_in_part();
        }

        // Swab byte-swap if requested
        if cfg.swab {
            swab_buffer(&mut buffer[..bytes_read]);
        }

        // In-Flight Hashing
        hasher.update(&buffer[..bytes_read]);

        // Padding if --sync requested
        let write_len = if cfg.sync && bytes_read < block_size {
            buffer[bytes_read..block_size].fill(0);
            block_size
        } else {
            bytes_read
        };

        // Write to output (with sparse hole-punching support if --sparse is active)
        if let Some(ref mut f) = out_raw_file {
            if cfg.sparse && buffer[..write_len].iter().all(|&b| b == 0) {
                if f.seek(SeekFrom::Current(write_len as i64)).is_ok() {
                    last_was_sparse_hole = true;
                } else {
                    f.write_all(&buffer[..write_len])
                        .map_err(|e| anyhow!("Write error: {}", e))?;
                    last_was_sparse_hole = false;
                }
            } else {
                if let Err(e) = f.write_all(&buffer[..write_len]) {
                    // Handle O_DIRECT unaligned tail fallback by reopening without O_DIRECT
                    if let (true, Some(ref out_path)) = (cfg.direct, &cfg.output) {
                        let cur_pos = f.stream_position().unwrap_or(0);
                        let mut fallback_f = OpenOptions::new().write(true).open(out_path)?;
                        fallback_f.seek(SeekFrom::Start(cur_pos))?;
                        fallback_f.write_all(&buffer[..write_len])?;
                        *f = fallback_f;
                    } else {
                        return Err(anyhow!("Write error: {}", e));
                    }
                }
                last_was_sparse_hole = false;
            }
        } else {
            stdout_handle
                .write_all(&buffer[..write_len])
                .map_err(|e| anyhow!("Write error: {}", e))?;
        }

        if write_len == block_size {
            stats.inc_out_full();
        } else {
            stats.inc_out_part();
        }

        stats.add_bytes(bytes_read as u64);

        if let Some(ref mut rem) = remaining_bytes {
            *rem = rem.saturating_sub(bytes_read as u64);
        }
        if let Some(ref mut rem) = remaining_blocks {
            *rem = rem.saturating_sub(1);
        }
    }

    if let Some(ref f) = in_file_opt {
        if cfg.nocache {
            evict_page_cache(f);
        }
    }

    if let Some(ref mut f) = out_raw_file {
        if last_was_sparse_hole {
            if let Ok(pos) = f.stream_position() {
                let _ = f.set_len(pos);
            }
        }
        if cfg.fdatasync {
            let _ = f.sync_data();
        } else if cfg.fsync {
            let _ = f.sync_all();
        }
        if cfg.nocache {
            evict_page_cache(f);
        }
    } else if cfg.fdatasync || cfg.fsync {
        let _ = stdout_handle.flush();
    }

    Ok(hasher.finalize_string())
}

fn execute_async_transfer(
    mut input: Box<dyn Read + Send>,
    out_raw_file: &mut Option<File>,
    cfg: &Config,
    stats: &Stats,
    hasher: &mut InFlightHasher,
    cancel_flag: &Arc<AtomicBool>,
) -> Result<()> {
    use std::sync::mpsc::sync_channel;

    let block_size = cfg.block_size.max(1);
    let queue_depth = cfg
        .queue_depth
        .unwrap_or_else(|| (512 * 1024 / block_size).clamp(16, 128));
    stats
        .async_capacity
        .store(queue_depth as u64, Ordering::Relaxed);

    let (data_tx, data_rx) = sync_channel::<(AlignedBuffer, usize)>(queue_depth);
    let (recycle_tx, recycle_rx) = sync_channel::<AlignedBuffer>(queue_depth);
    let (hash_tx, hash_rx) = sync_channel::<(AlignedBuffer, usize)>(queue_depth);

    // Pre-allocate exactly queue_depth 4096B-aligned buffers (zero allocations during transfer loop)
    for _ in 0..queue_depth {
        let _ = recycle_tx.send(AlignedBuffer::new(block_size, 4096));
    }

    let limit_bytes = cfg.limit_bytes;
    let count = cfg.count;
    let noerror = cfg.noerror;
    let reader_recycle_tx = recycle_tx.clone();
    let hasher_recycle_tx = recycle_tx.clone();
    let reader_cancel = Arc::clone(cancel_flag);

    // Stage 1: Reader Thread (Producer)
    let reader_handle = std::thread::spawn(move || -> Result<u64> {
        let mut total_read = 0u64;
        let mut rem_bytes = limit_bytes;
        let mut rem_blocks = count;

        loop {
            if reader_cancel.load(Ordering::Relaxed) {
                break;
            }
            if let Some(0) = rem_bytes {
                break;
            }
            if let Some(0) = rem_blocks {
                break;
            }

            let to_read = if let Some(rem) = rem_bytes {
                (rem as usize).min(block_size)
            } else {
                block_size
            };

            // Acquire recycled 4096B-aligned buffer with zero heap allocations
            let mut buf = match recycle_rx.recv() {
                Ok(b) => b,
                Err(_) => break, // Consumer terminated
            };

            let n = match input.read(&mut buf[..to_read]) {
                Ok(0) => break,
                Ok(bytes) => bytes,
                Err(ref e) if e.kind() == std::io::ErrorKind::Interrupted => {
                    let _ = reader_recycle_tx.send(buf);
                    continue;
                }
                Err(e) => {
                    if noerror {
                        eprintln!("blkcp: read error: {}, continuing...", e);
                        let _ = reader_recycle_tx.send(buf);
                        continue;
                    } else {
                        return Err(anyhow!("Read error in async pipeline: {}", e));
                    }
                }
            };

            total_read += n as u64;
            if let Some(ref mut rem) = rem_bytes {
                *rem = rem.saturating_sub(n as u64);
            }
            if let Some(ref mut rem) = rem_blocks {
                *rem = rem.saturating_sub(1);
            }

            if data_tx.send((buf, n)).is_err() {
                break;
            }
        }
        Ok(total_read)
    });

    // Stage 3: Dedicated Parallel Hasher Thread
    let has_hash = cfg.hash_algo != HashAlgo::None;
    let mut thread_hasher = InFlightHasher::new(cfg.hash_algo);

    let hash_handle = std::thread::spawn(move || -> InFlightHasher {
        while let Ok((buf, n)) = hash_rx.recv() {
            thread_hasher.update(&buf[..n]);
            let _ = hasher_recycle_tx.send(buf);
        }
        thread_hasher
    });

    let mut stdout_handle = std::io::stdout();
    let mut last_was_sparse_hole = false;

    // Stage 2: Writer Thread (Consumer & Dispatcher)
    while let Ok((mut buf, n)) = data_rx.recv() {
        if cancel_flag.load(Ordering::Relaxed) {
            break;
        }
        if n == block_size {
            stats.inc_in_full();
        } else {
            stats.inc_in_part();
        }

        if cfg.swab {
            swab_buffer(&mut buf[..n]);
        }

        let write_len = if cfg.sync && n < block_size {
            buf[n..block_size].fill(0);
            block_size
        } else {
            n
        };

        if let Some(ref mut f) = out_raw_file {
            if cfg.sparse && buf[..write_len].iter().all(|&b| b == 0) {
                if f.seek(SeekFrom::Current(write_len as i64)).is_ok() {
                    last_was_sparse_hole = true;
                } else {
                    f.write_all(&buf[..write_len])
                        .map_err(|e| anyhow!("Write error in async pipeline: {}", e))?;
                    last_was_sparse_hole = false;
                }
            } else {
                if let Err(e) = f.write_all(&buf[..write_len]) {
                    if let (true, Some(ref out_path)) = (cfg.direct, &cfg.output) {
                        let cur_pos = f.stream_position().unwrap_or(0);
                        let mut fallback_f = OpenOptions::new().write(true).open(out_path)?;
                        fallback_f.seek(SeekFrom::Start(cur_pos))?;
                        fallback_f.write_all(&buf[..write_len])?;
                        *f = fallback_f;
                    } else {
                        return Err(anyhow!("Write error in async pipeline: {}", e));
                    }
                }
                last_was_sparse_hole = false;
            }
        } else {
            stdout_handle
                .write_all(&buf[..write_len])
                .map_err(|e| anyhow!("Write error in async pipeline: {}", e))?;
        }

        if write_len == block_size {
            stats.inc_out_full();
        } else {
            stats.inc_out_part();
        }

        stats.add_bytes(n as u64);

        if has_hash {
            let _ = hash_tx.send((buf, n));
        } else {
            let _ = recycle_tx.send(buf);
        }
    }

    if let Some(ref mut f) = out_raw_file {
        if last_was_sparse_hole {
            if let Ok(pos) = f.stream_position() {
                let _ = f.set_len(pos);
            }
        }
    }

    drop(hash_tx);
    *hasher = hash_handle
        .join()
        .map_err(|_| anyhow!("Hasher thread panicked"))?;

    let _ = reader_handle
        .join()
        .map_err(|_| anyhow!("Reader thread panicked"))??;
    Ok(())
}
