use anyhow::{anyhow, Result};
use io_uring::{opcode, types, IoUring};
use std::fs::File;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;

use crate::aligned::AlignedBuffer;
use crate::args::{Config, HashAlgo};
use crate::hash::InFlightHasher;
use crate::stats::Stats;

/// Safely enqueues a submission queue entry into the ring.
///
/// Encapsulates the `io_uring` crate's unsafe `push` method.
#[inline]
fn push_sqe(ring: &mut IoUring, entry: &io_uring::squeue::Entry) -> Result<()> {
    // SAFETY: The entry references memory buffers that are pinned, thread-local, and guaranteed
    // to outlive the synchronous completion cycle awaited via `ring.submit_and_wait(1)`.
    unsafe {
        ring.submission()
            .push(entry)
            .map_err(|_| anyhow!("io_uring submission queue full"))
    }
}

pub fn execute_uring_transfer(
    in_file: &File,
    out_file: &File,
    cfg: &Config,
    stats: &Stats,
    cancel_flag: &Arc<AtomicBool>,
) -> Result<Option<(String, &'static str)>> {
    let mut hasher = InFlightHasher::new(cfg.hash_algo);

    if cfg.threads > 1 {
        let block_size = cfg.block_size.max(1);
        let explicit_limit = cfg
            .limit_bytes
            .or_else(|| cfg.count.map(|cnt| cnt.saturating_mul(block_size as u64)));
        let file_rem = in_file
            .metadata()
            .ok()
            .map(|m| m.len().saturating_sub(cfg.skip_bytes))
            .unwrap_or(0);
        let effective_len = explicit_limit.map_or(file_rem, |lim| lim.min(file_rem));

        let can_parallel_blks = cfg.hash_algo == HashAlgo::Blks
            && block_size.is_multiple_of(blks_core::CHUNK_SIZE)
            && effective_len > blks_core::CHUNK_SIZE as u64;

        if cfg.hash_algo != HashAlgo::None && !can_parallel_blks {
            if !cfg.quiet && !cfg.json {
                eprintln!("blkcp: io_uring: in-flight checksum requires sequential stream; falling back to single-ring mode");
            }
            execute_uring_single(in_file, out_file, cfg, stats, cancel_flag, &mut hasher)?;
            return Ok(hasher.finalize_string());
        }
        return execute_uring_sharded(in_file, out_file, cfg, stats, cancel_flag);
    }

    execute_uring_single(in_file, out_file, cfg, stats, cancel_flag, &mut hasher)?;
    Ok(hasher.finalize_string())
}

fn execute_uring_single(
    in_file: &File,
    out_file: &File,
    cfg: &Config,
    stats: &Stats,
    cancel_flag: &Arc<AtomicBool>,
    hasher: &mut InFlightHasher,
) -> Result<()> {
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

        push_sqe(&mut ring, &read_e)?;
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
        hasher.update(&buf[..bytes_read]);

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

            push_sqe(&mut ring, &write_e)?;
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
) -> Result<Option<(String, &'static str)>> {
    let num_workers = cfg.threads.clamp(2, 64);
    let chunk_size = (64 * 1024 * 1024u64).max(cfg.block_size as u64); // 64 MiB stripes
    let compute_blks = cfg.hash_algo == HashAlgo::Blks;

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

    let all_stripes = std::thread::scope(|s| -> Result<Vec<(u64, Vec<[u8; 48]>)>> {
        let mut handles = Vec::with_capacity(num_workers);

        for _ in 0..num_workers {
            let next_offset_ref = &next_offset;
            let abort_flag_ref = &abort_flag;
            let user_cancel = cancel_flag.as_ref();

            let handle = s.spawn(move || -> Result<Vec<(u64, Vec<[u8; 48]>)>> {
                let mut ring =
                    IoUring::new(32).map_err(|e| anyhow!("io_uring worker init failed: {}", e))?;
                let in_fd = types::Fd(in_fd_val);
                let out_fd = types::Fd(out_fd_val);

                // 4096-byte page-aligned buffer per worker shard
                let mut buf = AlignedBuffer::new(block_size, 4096);
                let mut worker_stripes: Vec<(u64, Vec<[u8; 48]>)> = Vec::new();

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

                    let mut stripe_leaves = if compute_blks {
                        Vec::with_capacity(
                            (this_chunk_len as usize).div_ceil(blks_core::CHUNK_SIZE),
                        )
                    } else {
                        Vec::new()
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

                        push_sqe(&mut ring, &read_e)?;
                        ring.submit_and_wait(1)?;

                        let cqe = ring
                            .completion()
                            .next()
                            .ok_or_else(|| anyhow!("Missing CQE"))?;
                        let res = cqe.result();

                        if res == 0 {
                            // EOF
                            abort_flag_ref.store(true, Ordering::Relaxed);
                            if compute_blks && !stripe_leaves.is_empty() {
                                worker_stripes.push((cur_chunk_base, stripe_leaves));
                            }
                            return Ok(worker_stripes);
                        } else if res < 0 {
                            let err = -res;
                            if err == libc::EINTR {
                                continue;
                            }
                            abort_flag_ref.store(true, Ordering::Relaxed);
                            return Err(anyhow!("io_uring sharded read error: errno {}", err));
                        }

                        let nread = res as usize;
                        if compute_blks {
                            for leaf_slice in buf[..nread].chunks(blks_core::CHUNK_SIZE) {
                                stripe_leaves.push(blks_core::tree::hash_chunk(leaf_slice, false));
                            }
                        }

                        let mut written = 0usize;
                        while written < nread {
                            let rem_w = nread - written;
                            let w_ptr = buf[written..].as_ptr();
                            let write_e = opcode::Write::new(out_fd, w_ptr, rem_w as u32)
                                .offset(write_off + written as u64)
                                .build()
                                .user_data(2);

                            push_sqe(&mut ring, &write_e)?;
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

                    if compute_blks && !stripe_leaves.is_empty() {
                        worker_stripes.push((cur_chunk_base, stripe_leaves));
                    }
                }

                Ok(worker_stripes)
            });

            handles.push(handle);
        }

        let mut collected = Vec::new();
        for h in handles {
            let mut ws = h.join().map_err(|_| anyhow!("Worker thread panicked"))??;
            collected.append(&mut ws);
        }
        Ok(collected)
    })?;

    if compute_blks {
        let mut sorted_stripes = all_stripes;
        sorted_stripes.sort_unstable_by_key(|(base, _)| *base);
        let all_leaves: Vec<[u8; 48]> = sorted_stripes
            .into_iter()
            .flat_map(|(_, leaves)| leaves)
            .collect();
        if !all_leaves.is_empty() {
            let root = blks_core::tree::reduce_nodes(all_leaves, true);
            return Ok(Some((blks_core::encode_base64(&root), "blks")));
        }
    }

    Ok(None)
}
