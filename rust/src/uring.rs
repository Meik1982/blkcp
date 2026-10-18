use std::fs::File;
use std::os::unix::io::AsRawFd;
use anyhow::{anyhow, Result};
use io_uring::{opcode, types, IoUring};

use crate::args::Config;
use crate::stats::Stats;

pub fn execute_uring_transfer(
    in_file: &File,
    out_file: &File,
    cfg: &Config,
    stats: &mut Stats,
) -> Result<()> {
    let queue_depth = 64;
    let mut ring = IoUring::new(queue_depth)
        .map_err(|e| anyhow!("Failed to initialize io_uring: {}", e))?;

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
            ring.submission().push(&read_e).map_err(|_| anyhow!("io_uring submission queue full"))?;
        }
        ring.submit_and_wait(1)?;

        let cqe = ring.completion().next().ok_or_else(|| anyhow!("Missing completion CQE"))?;
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
            ring.submission().push(&write_e).map_err(|_| anyhow!("io_uring submission queue full"))?;
        }
        ring.submit_and_wait(1)?;

        let cqe = ring.completion().next().ok_or_else(|| anyhow!("Missing completion CQE"))?;
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
