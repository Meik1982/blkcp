use anyhow::{anyhow, Result};
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;

use crate::args::{Config, Engine, HashAlgo};
use crate::hash::InFlightHasher;
use crate::stats::Stats;
use crate::swab::swab_buffer;

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
    stats: &mut Stats,
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

    let chunk_size = (64 * 1024 * 1024).max(cfg.block_size); // 64 MiB chunks
    let mut remaining = cfg.limit_bytes;
    if let Some(cnt) = cfg.count {
        let total_from_count = cnt.saturating_mul(cfg.block_size as u64);
        remaining = Some(remaining.map_or(total_from_count, |r| r.min(total_from_count)));
    }

    loop {
        if let Some(0) = remaining {
            break;
        }

        let to_copy = if let Some(rem) = remaining {
            (rem as usize).min(chunk_size)
        } else {
            chunk_size
        };

        let ret = unsafe {
            libc::copy_file_range(
                in_file.as_raw_fd(),
                std::ptr::null_mut(),
                out_file.as_raw_fd(),
                std::ptr::null_mut(),
                to_copy,
                0,
            )
        };

        if ret == 0 {
            break; // EOF
        } else if ret > 0 {
            let n = ret as usize;
            stats.bytes_copied += n as u64;
            stats.records_in_full += (n / cfg.block_size) as u64;
            if !n.is_multiple_of(cfg.block_size) {
                stats.records_in_part += 1;
            }
            stats.records_out_full += (n / cfg.block_size) as u64;
            if !n.is_multiple_of(cfg.block_size) {
                stats.records_out_part += 1;
            }
            if let Some(ref mut rem) = remaining {
                *rem = rem.saturating_sub(n as u64);
            }
        } else {
            let err = std::io::Error::last_os_error();
            let raw_err = err.raw_os_error().unwrap_or(0);
            if raw_err == libc::EINTR {
                continue;
            }
            if stats.bytes_copied == 0
                && (raw_err == libc::EXDEV
                    || raw_err == libc::EINVAL
                    || raw_err == libc::ENOSYS
                    || raw_err == libc::EOPNOTSUPP)
            {
                if cfg.engine == Engine::Reflink {
                    return Err(anyhow!(
                        "copy_file_range not supported for these files: {}",
                        err
                    ));
                }
                return Ok(false); // fallback to standard copy
            }
            return Err(anyhow!("copy_file_range error: {}", err));
        }
    }

    Ok(true)
}

#[cfg(not(target_os = "linux"))]
fn try_reflink_copy(
    _in_file: &mut File,
    _out_file: &mut File,
    _cfg: &Config,
    _stats: &mut Stats,
) -> Result<bool> {
    Ok(false) // Portable fallback
}

pub fn execute_transfer(cfg: &Config) -> Result<()> {
    if cfg.dry_run {
        eprintln!("blkcp (dry-run): Target inspection passed. No data written.");
        return Ok(());
    }

    let mut stats = Stats::new();

    // Fast-path: Kernel Zero-Copy Reflink
    if can_use_reflink(cfg) {
        if let (Some(ref in_path), Some(ref out_path)) = (&cfg.input, &cfg.output) {
            let in_file_res = OpenOptions::new().read(true).open(in_path);
            let mut out_opts = OpenOptions::new();
            out_opts.write(true).create(true);
            if !cfg.notrunc {
                out_opts.truncate(true);
            }
            let out_file_res = out_opts.open(out_path);

            if let (Ok(mut in_f), Ok(mut out_f)) = (in_file_res, out_file_res) {
                if cfg.skip_bytes > 0 {
                    let _ = in_f.seek(SeekFrom::Start(cfg.skip_bytes));
                }
                if cfg.seek_bytes > 0 {
                    let _ = out_f.seek(SeekFrom::Start(cfg.seek_bytes));
                }

                match try_reflink_copy(&mut in_f, &mut out_f, cfg, &mut stats) {
                    Ok(true) => {
                        if cfg.fdatasync || cfg.fsync {
                            let _ = out_f.flush();
                        }
                        stats.print_summary(cfg.quiet, cfg.json, None);
                        return Ok(());
                    }
                    Ok(false) => {
                        // Fallback to standard transfer
                    }
                    Err(e) => return Err(e),
                }
            }
        }
    }

    let mut hasher = InFlightHasher::new(cfg.hash_algo);

    // Asynchronous double-buffering pipeline execution
    if cfg.engine == Engine::Async {
        let input_reader: Box<dyn Read + Send> = if let Some(ref in_path) = cfg.input {
            let mut opts = OpenOptions::new();
            opts.read(true);
            if cfg.direct {
                opts.custom_flags(libc::O_DIRECT);
            }
            let mut f = opts
                .open(in_path)
                .map_err(|e| anyhow!("Failed to open input '{}': {}", in_path.display(), e))?;

            if cfg.skip_bytes > 0 {
                f.seek(SeekFrom::Start(cfg.skip_bytes)).map_err(|e| {
                    anyhow!("Failed to seek input to offset {}: {}", cfg.skip_bytes, e)
                })?;
            }
            Box::new(f)
        } else {
            Box::new(std::io::stdin())
        };

        let mut output_writer: Box<dyn Write> = if let Some(ref out_path) = cfg.output {
            let mut opts = OpenOptions::new();
            opts.write(true).create(true);
            if !cfg.notrunc {
                opts.truncate(true);
            }
            if cfg.direct {
                opts.custom_flags(libc::O_DIRECT);
            }
            let mut f = opts
                .open(out_path)
                .map_err(|e| anyhow!("Failed to open output '{}': {}", out_path.display(), e))?;

            if cfg.seek_bytes > 0 {
                f.seek(SeekFrom::Start(cfg.seek_bytes)).map_err(|e| {
                    anyhow!("Failed to seek output to offset {}: {}", cfg.seek_bytes, e)
                })?;
            }
            Box::new(f)
        } else {
            Box::new(std::io::stdout())
        };

        execute_async_transfer(
            input_reader,
            &mut output_writer,
            cfg,
            &mut stats,
            &mut hasher,
        )?;

        if cfg.fdatasync || cfg.fsync {
            let _ = output_writer.flush();
        }

        let hash_info = hasher.finalize_string();
        stats.print_summary(cfg.quiet, cfg.json, hash_info);
        return Ok(());
    }

    // io_uring native execution (Linux only)
    #[cfg(target_os = "linux")]
    if cfg.engine == Engine::Uring && cfg.hash_algo == HashAlgo::None && !cfg.swab {
        if let (Some(ref in_path), Some(ref out_path)) = (&cfg.input, &cfg.output) {
            let mut in_opts = OpenOptions::new();
            in_opts.read(true);
            if cfg.direct {
                in_opts.custom_flags(libc::O_DIRECT);
            }
            let in_f = in_opts.open(in_path)?;

            let mut out_opts = OpenOptions::new();
            out_opts.write(true).create(true);
            if !cfg.notrunc {
                out_opts.truncate(true);
            }
            if cfg.direct {
                out_opts.custom_flags(libc::O_DIRECT);
            }
            let out_f = out_opts.open(out_path)?;

            if let Ok(()) = crate::uring::execute_uring_transfer(&in_f, &out_f, cfg, &mut stats) {
                if cfg.fdatasync || cfg.fsync {
                    let _ = nix::unistd::fsync(out_f.as_raw_fd());
                }
                stats.print_summary(cfg.quiet, cfg.json, None);
                return Ok(());
            }
        }
    }

    // Open Input File/Device
    let mut input_file: Box<dyn Read> = if let Some(ref in_path) = cfg.input {
        let mut opts = OpenOptions::new();
        opts.read(true);
        if cfg.direct {
            opts.custom_flags(libc::O_DIRECT);
        }
        let mut f = opts
            .open(in_path)
            .map_err(|e| anyhow!("Failed to open input '{}': {}", in_path.display(), e))?;

        if cfg.skip_bytes > 0 {
            f.seek(SeekFrom::Start(cfg.skip_bytes))
                .map_err(|e| anyhow!("Failed to seek input to offset {}: {}", cfg.skip_bytes, e))?;
        }
        Box::new(f)
    } else {
        Box::new(std::io::stdin())
    };

    // Open Output File/Device
    let mut output_file: Box<dyn Write> = if let Some(ref out_path) = cfg.output {
        let mut opts = OpenOptions::new();
        opts.write(true).create(true);
        if !cfg.notrunc {
            opts.truncate(true);
        }
        if cfg.direct {
            opts.custom_flags(libc::O_DIRECT);
        }
        let mut f = opts
            .open(out_path)
            .map_err(|e| anyhow!("Failed to open output '{}': {}", out_path.display(), e))?;

        if cfg.seek_bytes > 0 {
            f.seek(SeekFrom::Start(cfg.seek_bytes)).map_err(|e| {
                anyhow!("Failed to seek output to offset {}: {}", cfg.seek_bytes, e)
            })?;
        }
        Box::new(f)
    } else {
        Box::new(std::io::stdout())
    };

    // Main Transfer Loop
    let block_size = cfg.block_size.max(512);
    let mut buffer = crate::aligned::AlignedBuffer::new(block_size, 4096);

    let mut remaining_bytes = cfg.limit_bytes;
    let mut remaining_blocks = cfg.count;

    loop {
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
            stats.records_in_full += 1;
        } else {
            stats.records_in_part += 1;
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

        // Write to output
        output_file
            .write_all(&buffer[..write_len])
            .map_err(|e| anyhow!("Write error: {}", e))?;

        if write_len == block_size {
            stats.records_out_full += 1;
        } else {
            stats.records_out_part += 1;
        }

        stats.bytes_copied += bytes_read as u64;

        if let Some(ref mut rem) = remaining_bytes {
            *rem = rem.saturating_sub(bytes_read as u64);
        }
        if let Some(ref mut rem) = remaining_blocks {
            *rem = rem.saturating_sub(1);
        }
    }

    if cfg.fdatasync || cfg.fsync {
        let _ = output_file.flush();
    }

    let hash_info = hasher.finalize_string();
    stats.print_summary(cfg.quiet, cfg.json, hash_info);

    Ok(())
}

fn execute_async_transfer(
    mut input: Box<dyn Read + Send>,
    output: &mut Box<dyn Write>,
    cfg: &Config,
    stats: &mut Stats,
    hasher: &mut InFlightHasher,
) -> Result<()> {
    use std::sync::mpsc::sync_channel;

    let queue_depth = 16;
    let block_size = cfg.block_size.max(512);

    let (data_tx, data_rx) = sync_channel::<(Vec<u8>, usize)>(queue_depth);
    let (recycle_tx, recycle_rx) = sync_channel::<Vec<u8>>(queue_depth);
    let (hash_tx, hash_rx) = sync_channel::<(Vec<u8>, usize)>(queue_depth);

    // Pre-allocate exactly queue_depth buffers (zero allocations during transfer loop)
    for _ in 0..queue_depth {
        let _ = recycle_tx.send(vec![0u8; block_size]);
    }

    let limit_bytes = cfg.limit_bytes;
    let count = cfg.count;
    let noerror = cfg.noerror;
    let reader_recycle_tx = recycle_tx.clone();
    let hasher_recycle_tx = recycle_tx.clone();

    // Stage 1: Reader Thread (Producer)
    let reader_handle = std::thread::spawn(move || -> Result<u64> {
        let mut total_read = 0u64;
        let mut rem_bytes = limit_bytes;
        let mut rem_blocks = count;

        loop {
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

            // Acquire recycled buffer with zero heap allocations
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

    // Stage 2: Writer Thread (Consumer & Dispatcher)
    while let Ok((mut buf, n)) = data_rx.recv() {
        if n == block_size {
            stats.records_in_full += 1;
        } else {
            stats.records_in_part += 1;
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

        output
            .write_all(&buf[..write_len])
            .map_err(|e| anyhow!("Write error in async pipeline: {}", e))?;

        if write_len == block_size {
            stats.records_out_full += 1;
        } else {
            stats.records_out_part += 1;
        }

        stats.bytes_copied += n as u64;

        if has_hash {
            let _ = hash_tx.send((buf, n));
        } else {
            let _ = recycle_tx.send(buf);
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
