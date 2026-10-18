use anyhow::{anyhow, Result};
use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::fs::OpenOptionsExt;

use crate::args::Config;
use crate::hash::InFlightHasher;
use crate::stats::Stats;
use crate::swab::swab_buffer;

pub fn execute_transfer(cfg: &Config) -> Result<()> {
    if cfg.dry_run {
        eprintln!("blkcp (dry-run): Target inspection passed. No data written.");
        return Ok(());
    }

    let mut stats = Stats::new();
    let mut hasher = InFlightHasher::new(cfg.hash_algo);

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
    let mut buffer = vec![0u8; block_size];

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
