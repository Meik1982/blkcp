use anyhow::{anyhow, Result};
use io_uring::{opcode, types, IoUring};
use std::fs::File;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;

use crate::aligned::AlignedBuffer;
use crate::args::Config;
use crate::stats::Stats;

pub fn execute_uring_transfer(
    in_file: &File,
    out_file: &File,
    cfg: &Config,
    stats: &Stats,
    cancel_flag: &Arc<AtomicBool>,
) -> Result<()> {
    if cfg.threads > 1 {
        return execute_uring_sharded(in_file, out_file, cfg, stats, cancel_flag);
    }

    let queue_depth = cfg.queue_depth.unwrap_or(64).clamp(8, 256) as u32;
    let mut ring =
        IoUring::new(queue_depth).map_err(|e| anyhow!("Failed to initialize io_uring: {}", e))?;

    let block_size = cfg.block_size.max(1);
    let in_fd = types::Fd(in_file.as_raw_fd());
    let out_fd = types::Fd(out_file.as_raw_fd());

    let mut in_offset = cfg.skip_bytes;
    let mut out_offset = cfg.seek_bytes;

    let mut remaining = cfg.limit_bytes;
    if let Some(cnt) = cfg.count {
        let total = cnt.saturating_mul(block_size as u64);
        remaining = Some(remaining.map_or(total, |r| r.min(total)));
    }

    // 4096-byte page-aligned buffer for hardware DMA / O_DIRECT compatibility
    let mut buf = AlignedBuffer::new(block_size, 4096);

    loop {
        if cancel_flag.load(Ordering::Relaxed) {
            return Err(anyhow!("Transfer cancelled by user"));
        }
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

        // SAFETY: `buf` is a 4096-byte aligned buffer of `block_size` bytes. `ring.submit_and_wait(1)`
        // synchronously awaits kernel completion before `buf` is reused or modified.
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
            stats.inc_in_full();
        } else {
            stats.inc_in_part();
        }

        // Submit Write (with full short-write loop)
        let mut written_in_block = 0usize;
        while written_in_block < bytes_read {
            let rem_write = bytes_read - written_in_block;
            let write_ptr = buf[written_in_block..].as_ptr();
            let write_e = opcode::Write::new(out_fd, write_ptr, rem_write as u32)
                .offset(out_offset + written_in_block as u64)
                .build()
                .user_data(2);

            // SAFETY: `write_ptr` points to `rem_write` valid bytes inside `buf`. Synchronous
            // `submit_and_wait(1)` ensures the kernel completes writing before the next iteration.
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
            let wres = cqe.result();

            if wres < 0 {
                let err = -wres;
                if err == libc::EINTR {
                    continue;
                }
                return Err(anyhow!("io_uring write error: errno {}", err));
            }
            written_in_block += wres as usize;
        }

        out_offset += bytes_read as u64;

        if bytes_read == block_size {
            stats.inc_out_full();
        } else {
            stats.inc_out_part();
        }

        stats.add_bytes(bytes_read as u64);

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
    stats: &Stats,
    cancel_flag: &Arc<AtomicBool>,
) -> Result<()> {
    let num_workers = cfg.threads.clamp(2, 64);
    let chunk_size = (64 * 1024 * 1024u64).max(cfg.block_size as u64); // 64 MiB stripes

    let block_size = cfg.block_size.max(1);
    let in_start_offset = cfg.skip_bytes;
    let out_start_offset = cfg.seek_bytes;

    let explicit_limit = cfg
        .limit_bytes
        .or_else(|| cfg.count.map(|cnt| cnt.saturating_mul(block_size as u64)));

    let total_bytes_to_copy = explicit_limit.or_else(|| {
        in_file
            .metadata()
            .ok()
            .map(|m| m.len().saturating_sub(in_start_offset))
            .filter(|&len| len > 0)
    });

    let next_offset = AtomicU64::new(0);
    let abort_flag = AtomicBool::new(false);

    let in_fd_val = in_file.as_raw_fd();
    let out_fd_val = out_file.as_raw_fd();

    std::thread::scope(|s| -> Result<()> {
        let mut handles = Vec::with_capacity(num_workers);

        for _ in 0..num_workers {
            let next_offset_ref = &next_offset;
            let abort_flag_ref = &abort_flag;
            let user_cancel = cancel_flag.as_ref();

            let handle = s.spawn(move || -> Result<()> {
                let mut ring =
                    IoUring::new(32).map_err(|e| anyhow!("io_uring worker init failed: {}", e))?;
                let in_fd = types::Fd(in_fd_val);
                let out_fd = types::Fd(out_fd_val);

                // 4096-byte page-aligned buffer per worker shard
                let mut buf = AlignedBuffer::new(block_size, 4096);

                loop {
                    if abort_flag_ref.load(Ordering::Relaxed) || user_cancel.load(Ordering::Relaxed)
                    {
                        break;
                    }

                    let cur_chunk_base = next_offset_ref.fetch_add(chunk_size, Ordering::SeqCst);
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
                        if user_cancel.load(Ordering::Relaxed) {
                            return Err(anyhow!("Transfer cancelled by user"));
                        }

                        let to_read = ((this_chunk_len - chunk_done) as usize).min(block_size);
                        let read_off = in_start_offset + cur_chunk_base + chunk_done;
                        let write_off = out_start_offset + cur_chunk_base + chunk_done;

                        let read_e = opcode::Read::new(in_fd, buf.as_mut_ptr(), to_read as u32)
                            .offset(read_off)
                            .build()
                            .user_data(1);

                        // SAFETY: `buf` is thread-local, 4096-byte aligned, and points to `block_size` bytes.
                        // `ring.submit_and_wait(1)` blocks until the kernel read finishes.
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
                            abort_flag_ref.store(true, Ordering::Relaxed);
                            return Ok(());
                        } else if res < 0 {
                            let err = -res;
                            if err == libc::EINTR {
                                continue;
                            }
                            abort_flag_ref.store(true, Ordering::Relaxed);
                            return Err(anyhow!("io_uring sharded read error: errno {}", err));
                        }

                        let nread = res as usize;

                        let mut written = 0usize;
                        while written < nread {
                            let rem_w = nread - written;
                            let w_ptr = buf[written..].as_ptr();
                            let write_e = opcode::Write::new(out_fd, w_ptr, rem_w as u32)
                                .offset(write_off + written as u64)
                                .build()
                                .user_data(2);

                            // SAFETY: `w_ptr` points to `rem_w` valid bytes inside `buf`. Synchronous waiting
                            // guarantees no concurrent mutation of `buf` while the kernel writes.
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
                                let err = -wres;
                                if err == libc::EINTR {
                                    continue;
                                }
                                abort_flag_ref.store(true, Ordering::Relaxed);
                                return Err(anyhow!("io_uring sharded write error: errno {}", err));
                            }
                            written += wres as usize;
                        }

                        chunk_done += nread as u64;
                        stats.add_bytes(nread as u64);

                        if nread == block_size {
                            stats.inc_in_full();
                            stats.inc_out_full();
                        } else {
                            stats.inc_in_part();
                            stats.inc_out_part();
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
        Ok(())
    })?;

    Ok(())
}
