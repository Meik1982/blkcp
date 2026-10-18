use anyhow::{anyhow, Result};
use io_uring::{opcode, types, IoUring};
use std::fs::File;
use std::os::unix::io::AsRawFd;

use crate::args::Config;
use crate::stats::Stats;

pub fn execute_uring_transfer(
    in_file: &File,
    out_file: &File,
    cfg: &Config,
    stats: &mut Stats,
) -> Result<()> {
    if cfg.threads > 1 {
        return execute_uring_sharded(in_file, out_file, cfg, stats);
    }

    let queue_depth = 64;
    let mut ring =
        IoUring::new(queue_depth).map_err(|e| anyhow!("Failed to initialize io_uring: {}", e))?;

    let block_size = cfg.block_size.max(512);
    let in_fd = types::Fd(in_file.as_raw_fd());
    let out_fd = types::Fd(out_file.as_raw_fd());

    let mut in_offset = cfg.skip_bytes;
    let mut out_offset = cfg.seek_bytes;

    let mut remaining = cfg.limit_bytes;
    if let Some(cnt) = cfg.count {
        let total = cnt.saturating_mul(block_size as u64);
        remaining = Some(remaining.map_or(total, |r| r.min(total)));
    }

    // Double-buffered overlapped loop
    let mut buf = vec![0u8; block_size];

    loop {
        if let Some(0) = remaining {
            break;
        }

        let to_read = if let Some(rem) = remaining {
            (rem as usize).min(block_size)
        } else {
            block_size
        };

        // Submit Read
        let read_e = opcode::Read::new(in_fd, buf.as_mut_ptr(), to_read as u32)
            .offset(in_offset)
            .build()
            .user_data(1);

        unsafe {
            ring.submission()
                .push(&read_e)
                .map_err(|_| anyhow!("io_uring submission queue full"))?;
        }
        ring.submit_and_wait(1)?;

        let cqe = ring
            .completion()
            .next()
            .ok_or_else(|| anyhow!("Missing completion CQE"))?;
        let res = cqe.result();

        if res == 0 {
            break; // EOF
        } else if res < 0 {
            let err = -res;
            if err == libc::EINTR {
                continue;
            }
            return Err(anyhow!("io_uring read error: errno {}", err));
        }

        let bytes_read = res as usize;
        in_offset += bytes_read as u64;

        if bytes_read == block_size {
            stats.records_in_full += 1;
        } else {
            stats.records_in_part += 1;
        }

        // Submit Write
        let write_e = opcode::Write::new(out_fd, buf.as_ptr(), bytes_read as u32)
            .offset(out_offset)
            .build()
            .user_data(2);

        unsafe {
            ring.submission()
                .push(&write_e)
                .map_err(|_| anyhow!("io_uring submission queue full"))?;
        }
        ring.submit_and_wait(1)?;

        let cqe = ring
            .completion()
            .next()
            .ok_or_else(|| anyhow!("Missing completion CQE"))?;
        let res = cqe.result();

        if res < 0 {
            return Err(anyhow!("io_uring write error: errno {}", -res));
        }

        let bytes_written = res as usize;
        out_offset += bytes_written as u64;

        if bytes_written == block_size {
            stats.records_out_full += 1;
        } else {
            stats.records_out_part += 1;
        }

        stats.bytes_copied += bytes_read as u64;

        if let Some(ref mut rem) = remaining {
            *rem = rem.saturating_sub(bytes_read as u64);
        }
    }

    Ok(())
}

pub fn execute_uring_sharded(
    in_file: &File,
    out_file: &File,
    cfg: &Config,
    stats: &mut Stats,
) -> Result<()> {
    use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
    use std::sync::Arc;

    let num_workers = cfg.threads.clamp(2, 64);
    let chunk_size = (64 * 1024 * 1024u64).max(cfg.block_size as u64); // 64 MiB stripes

    let block_size = cfg.block_size.max(512);
    let total_bytes_to_copy = cfg
        .limit_bytes
        .or_else(|| cfg.count.map(|cnt| cnt.saturating_mul(block_size as u64)));

    let in_start_offset = cfg.skip_bytes;
    let out_start_offset = cfg.seek_bytes;

    let next_offset = Arc::new(AtomicU64::new(0));
    let bytes_copied = Arc::new(AtomicU64::new(0));
    let records_full = Arc::new(AtomicU64::new(0));
    let records_part = Arc::new(AtomicU64::new(0));
    let abort_flag = Arc::new(AtomicBool::new(false));

    let in_fd_val = in_file.as_raw_fd();
    let out_fd_val = out_file.as_raw_fd();

    let mut handles = Vec::with_capacity(num_workers);

    for _ in 0..num_workers {
        let next_offset = Arc::clone(&next_offset);
        let bytes_copied = Arc::clone(&bytes_copied);
        let records_full = Arc::clone(&records_full);
        let records_part = Arc::clone(&records_part);
        let abort_flag = Arc::clone(&abort_flag);

        let handle = std::thread::spawn(move || -> Result<()> {
            let mut ring =
                IoUring::new(32).map_err(|e| anyhow!("io_uring worker init failed: {}", e))?;
            let in_fd = types::Fd(in_fd_val);
            let out_fd = types::Fd(out_fd_val);

            let mut buf = vec![0u8; block_size];

            loop {
                if abort_flag.load(Ordering::Relaxed) {
                    break;
                }

                let cur_chunk_base = next_offset.fetch_add(chunk_size, Ordering::SeqCst);
                if let Some(total) = total_bytes_to_copy {
                    if cur_chunk_base >= total {
                        break;
                    }
                }

                let this_chunk_len = if let Some(total) = total_bytes_to_copy {
                    (total - cur_chunk_base).min(chunk_size)
                } else {
                    chunk_size
                };

                let mut chunk_done = 0u64;
                while chunk_done < this_chunk_len {
                    let to_read = ((this_chunk_len - chunk_done) as usize).min(block_size);
                    let read_off = in_start_offset + cur_chunk_base + chunk_done;
                    let write_off = out_start_offset + cur_chunk_base + chunk_done;

                    let read_e = opcode::Read::new(in_fd, buf.as_mut_ptr(), to_read as u32)
                        .offset(read_off)
                        .build()
                        .user_data(1);

                    unsafe {
                        ring.submission()
                            .push(&read_e)
                            .map_err(|_| anyhow!("io_uring SQ full"))?;
                    }
                    ring.submit_and_wait(1)?;

                    let cqe = ring
                        .completion()
                        .next()
                        .ok_or_else(|| anyhow!("Missing CQE"))?;
                    let res = cqe.result();

                    if res == 0 {
                        // EOF
                        abort_flag.store(true, Ordering::Relaxed);
                        return Ok(());
                    } else if res < 0 {
                        let err = -res;
                        if err == libc::EINTR {
                            continue;
                        }
                        abort_flag.store(true, Ordering::Relaxed);
                        return Err(anyhow!("io_uring sharded read error: errno {}", err));
                    }

                    let nread = res as usize;

                    let write_e = opcode::Write::new(out_fd, buf.as_ptr(), nread as u32)
                        .offset(write_off)
                        .build()
                        .user_data(2);

                    unsafe {
                        ring.submission()
                            .push(&write_e)
                            .map_err(|_| anyhow!("io_uring SQ full"))?;
                    }
                    ring.submit_and_wait(1)?;

                    let cqe = ring
                        .completion()
                        .next()
                        .ok_or_else(|| anyhow!("Missing CQE"))?;
                    let wres = cqe.result();

                    if wres < 0 {
                        abort_flag.store(true, Ordering::Relaxed);
                        return Err(anyhow!("io_uring sharded write error: errno {}", -wres));
                    }

                    let nwritten = wres as usize;
                    chunk_done += nwritten as u64;
                    bytes_copied.fetch_add(nwritten as u64, Ordering::Relaxed);

                    if nwritten == block_size {
                        records_full.fetch_add(1, Ordering::Relaxed);
                    } else {
                        records_part.fetch_add(1, Ordering::Relaxed);
                    }
                }
            }

            Ok(())
        });

        handles.push(handle);
    }

    for h in handles {
        h.join().map_err(|_| anyhow!("Worker thread panicked"))??;
    }

    stats.bytes_copied = bytes_copied.load(Ordering::Relaxed);
    stats.records_in_full = records_full.load(Ordering::Relaxed);
    stats.records_in_part = records_part.load(Ordering::Relaxed);
    stats.records_out_full = stats.records_in_full;
    stats.records_out_part = stats.records_in_part;

    Ok(())
}
