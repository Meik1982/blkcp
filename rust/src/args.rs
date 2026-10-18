use anyhow::{anyhow, Result};
use std::path::PathBuf;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HashAlgo {
    None,
    Sha256,
    Blake3,
    Blks,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Engine {
    Auto,
    Sync,
    Async,
    Reflink,
    Uring,
    Splice,
}

#[derive(Debug, Clone)]
pub struct Config {
    pub input: Option<PathBuf>,
    pub output: Option<PathBuf>,
    pub block_size: usize,
    pub count: Option<u64>,
    pub limit_bytes: Option<u64>,
    pub skip_bytes: u64,
    pub seek_bytes: u64,
    pub engine: Engine,
    pub threads: usize,
    pub progress: bool,
    pub json: bool,
    pub quiet: bool,
    pub force: bool,
    pub dry_run: bool,
    pub autotune: bool,
    pub direct: bool,
    pub nocache: bool,
    pub sparse: bool,
    pub sync: bool,
    pub swab: bool,
    pub fdatasync: bool,
    pub fsync: bool,
    pub notrunc: bool,
    pub noerror: bool,
    pub hash_algo: HashAlgo,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            input: None,
            output: None,
            block_size: 512,
            count: None,
            limit_bytes: None,
            skip_bytes: 0,
            seek_bytes: 0,
            engine: Engine::Auto,
            threads: 1,
            progress: false,
            json: false,
            quiet: false,
            force: false,
            dry_run: false,
            autotune: false,
            direct: false,
            nocache: false,
            sparse: false,
            sync: false,
            swab: false,
            fdatasync: false,
            fsync: false,
            notrunc: false,
            noerror: false,
            hash_algo: HashAlgo::None,
        }
    }
}

pub fn parse_size(s: &str) -> Result<u64> {
    let s = s.trim();
    if s.is_empty() {
        return Err(anyhow!("Empty size string"));
    }

    if s.eq_ignore_ascii_case("auto") {
        return Ok(0); // 0 means autotune
    }

    let mut num_end = 0;
    for c in s.chars() {
        if c.is_ascii_digit() {
            num_end += 1;
        } else {
            break;
        }
    }

    if num_end == 0 {
        return Err(anyhow!("Invalid number: {}", s));
    }

    let num: u64 = s[..num_end].parse()?;
    let suffix = s[num_end..].trim();

    let multiplier: u64 = match suffix.to_lowercase().as_str() {
        "" => 1,
        "c" => 1,
        "b" => 512, // POSIX & GNU dd standard: 1 block = 512 bytes
        "k" | "kib" => 1024,
        "kb" => 1000,
        "m" | "mib" => 1024 * 1024,
        "mb" => 1000 * 1000,
        "g" | "gib" => 1024 * 1024 * 1024,
        "gb" => 1000 * 1000 * 1000,
        "t" | "tib" => 1024 * 1024 * 1024 * 1024,
        "tb" => 1000 * 1000 * 1000 * 1000,
        _ => return Err(anyhow!("Unknown size suffix '{}' in '{}'", suffix, s)),
    };

    Ok(num * multiplier)
}

pub fn parse_args() -> Result<Config> {
    let args: Vec<String> = std::env::args().collect();
    let mut cfg = Config::default();
    let mut positional = Vec::new();

    let mut i = 1;
    while i < args.len() {
        let arg = &args[i];

        if arg == "-h" || arg == "--help" {
            print_help();
            std::process::exit(0);
        } else if arg == "-v" || arg == "--version" {
            println!("blkcp 2.0.0-alpha.1 (Rust Core)");
            println!("Copyright (C) 2026 Meik Augenblick");
            std::process::exit(0);
        } else if arg == "-i" || arg == "--input" {
            i += 1;
            if i >= args.len() {
                return Err(anyhow!("Option -i requires an argument"));
            }
            cfg.input = Some(PathBuf::from(&args[i]));
        } else if let Some(val) = arg.strip_prefix("--input=") {
            cfg.input = Some(PathBuf::from(val));
        } else if arg == "-o" || arg == "--output" {
            i += 1;
            if i >= args.len() {
                return Err(anyhow!("Option -o requires an argument"));
            }
            cfg.output = Some(PathBuf::from(&args[i]));
        } else if let Some(val) = arg.strip_prefix("--output=") {
            cfg.output = Some(PathBuf::from(val));
        } else if arg == "-b" || arg == "--block-size" || arg == "--bs" {
            i += 1;
            if i >= args.len() {
                return Err(anyhow!("Option -b requires an argument"));
            }
            let sz = parse_size(&args[i])?;
            if sz == 0 {
                cfg.autotune = true;
                cfg.block_size = 64 * 1024;
            } else {
                cfg.block_size = sz as usize;
            }
        } else if let Some(val) = arg
            .strip_prefix("--block-size=")
            .or_else(|| arg.strip_prefix("--bs="))
        {
            let sz = parse_size(val)?;
            if sz == 0 {
                cfg.autotune = true;
                cfg.block_size = 64 * 1024;
            } else {
                cfg.block_size = sz as usize;
            }
        } else if arg == "-c" || arg == "--count" {
            i += 1;
            if i >= args.len() {
                return Err(anyhow!("Option -c requires an argument"));
            }
            cfg.count = Some(parse_size(&args[i])?);
        } else if let Some(val) = arg.strip_prefix("--count=") {
            cfg.count = Some(parse_size(val)?);
        } else if arg == "-l"
            || arg == "--limit"
            || arg == "-s"
            || arg == "--size"
            || arg == "--bytes"
        {
            i += 1;
            if i >= args.len() {
                return Err(anyhow!("Option -l requires an argument"));
            }
            cfg.limit_bytes = Some(parse_size(&args[i])?);
        } else if let Some(val) = arg
            .strip_prefix("--limit=")
            .or_else(|| arg.strip_prefix("--size="))
            .or_else(|| arg.strip_prefix("--bytes="))
        {
            cfg.limit_bytes = Some(parse_size(val)?);
        } else if arg == "--skip" {
            i += 1;
            if i >= args.len() {
                return Err(anyhow!("Option --skip requires an argument"));
            }
            cfg.skip_bytes = parse_size(&args[i])?;
        } else if let Some(val) = arg.strip_prefix("--skip=") {
            cfg.skip_bytes = parse_size(val)?;
        } else if arg == "--seek" {
            i += 1;
            if i >= args.len() {
                return Err(anyhow!("Option --seek requires an argument"));
            }
            cfg.seek_bytes = parse_size(&args[i])?;
        } else if let Some(val) = arg.strip_prefix("--seek=") {
            cfg.seek_bytes = parse_size(val)?;
        } else if arg == "-e" || arg == "--engine" {
            i += 1;
            if i >= args.len() {
                return Err(anyhow!("Option -e requires an argument"));
            }
            cfg.engine = match args[i].to_lowercase().as_str() {
                "sync" => Engine::Sync,
                "async" => Engine::Async,
                "reflink" => Engine::Reflink,
                "uring" => Engine::Uring,
                "splice" => Engine::Splice,
                "auto" => Engine::Auto,
                _ => return Err(anyhow!("Unknown engine: {}", args[i])),
            };
        } else if let Some(val) = arg.strip_prefix("--engine=") {
            cfg.engine = match val.to_lowercase().as_str() {
                "sync" => Engine::Sync,
                "async" => Engine::Async,
                "reflink" => Engine::Reflink,
                "uring" => Engine::Uring,
                "splice" => Engine::Splice,
                "auto" => Engine::Auto,
                _ => return Err(anyhow!("Unknown engine: {}", val)),
            };
        } else if arg == "-j" || arg == "--threads" || arg == "--shards" {
            i += 1;
            if i >= args.len() {
                return Err(anyhow!("Option -j requires an argument"));
            }
            cfg.threads = args[i].parse()?;
        } else if let Some(val) = arg
            .strip_prefix("--threads=")
            .or_else(|| arg.strip_prefix("--shards="))
        {
            cfg.threads = val.parse()?;
        } else if arg == "-p" || arg == "--progress" {
            cfg.progress = true;
        } else if arg == "-q" || arg == "--quiet" {
            cfg.quiet = true;
        } else if arg == "-f" || arg == "--force" {
            cfg.force = true;
        } else if arg == "-n" || arg == "--dry-run" {
            cfg.dry_run = true;
        } else if arg == "--autotune" {
            cfg.autotune = true;
        } else if arg == "--direct" {
            cfg.direct = true;
        } else if arg == "--nocache" {
            cfg.nocache = true;
        } else if arg == "--sparse" {
            cfg.sparse = true;
        } else if arg == "--sync" {
            cfg.sync = true;
        } else if arg == "--swab" {
            cfg.swab = true;
        } else if arg == "--fdatasync" {
            cfg.fdatasync = true;
        } else if arg == "--fsync" {
            cfg.fsync = true;
        } else if arg == "--notrunc" {
            cfg.notrunc = true;
        } else if arg == "--noerror" {
            cfg.noerror = true;
        } else if arg == "--json" {
            cfg.json = true;
        } else if arg == "--sha256" {
            cfg.hash_algo = HashAlgo::Sha256;
        } else if arg == "--blake3" {
            cfg.hash_algo = HashAlgo::Blake3;
        } else if arg == "--blks" {
            cfg.hash_algo = HashAlgo::Blks;
        } else if arg == "--hash" {
            cfg.hash_algo = HashAlgo::Sha256;
        } else if let Some(val) = arg.strip_prefix("--hash=") {
            cfg.hash_algo = match val.to_lowercase().as_str() {
                "sha256" => HashAlgo::Sha256,
                "blake3" => HashAlgo::Blake3,
                "blks" => HashAlgo::Blks,
                _ => return Err(anyhow!("Unknown hash algorithm: {}", val)),
            };
        } else if !arg.starts_with('-') {
            positional.push(arg.clone());
        } else {
            return Err(anyhow!("Unknown option: {}", arg));
        }

        i += 1;
    }

    // Resolve positional operands: blkcp [INPUT] [OUTPUT]
    if cfg.input.is_none() && !positional.is_empty() {
        cfg.input = Some(PathBuf::from(positional.remove(0)));
    }
    if cfg.output.is_none() && !positional.is_empty() {
        cfg.output = Some(PathBuf::from(positional.remove(0)));
    }
    if !positional.is_empty() {
        return Err(anyhow!("Extra operand: {}", positional[0]));
    }

    Ok(cfg)
}

fn print_help() {
    println!("Usage: blkcp [OPTIONS] [INPUT] [OUTPUT]");
    println!("   or: blkcp -i INPUT -o OUTPUT [OPTIONS]\n");
    println!(
        "Next-Gen Block Copy Tool (blkcp) for fast, safe and modern stream/disk replication.\n"
    );
    println!("Core Options:");
    println!("  -i, --input=FILE         Input file or block device (default: stdin)");
    println!("  -o, --output=FILE        Output file or block device (default: stdout)");
    println!("  -b, --block-size=SIZE    Block size for transfer (e.g. 64K, 4M, 1G, 1MiB)");
    println!("  -e, --engine=NAME        Execution backend: 'sync', 'async', 'reflink', 'uring', 'splice', 'auto'");
    println!("  -l, --limit=BYTES        Limit copy to exactly BYTES bytes");
    println!("  -c, --count=N            Copy only N input blocks");
    println!("  -j, --threads=N          Parallel worker threads / io_uring shards (default: 1)");
    println!("  -p, --progress           Show periodic real-time transfer telemetry");
    println!("  -q, --quiet              Suppress all output except fatal error messages");
    println!(
        "  -f, --force              Override Target Safety Guard (e.g. write to mounted disks)"
    );
    println!("  -n, --dry-run            Simulate transfer without writing");
    println!("      --hash[=ALGO]        Compute on-the-fly checksum (sha256, blake3, blks)");
    println!("      --sha256, --blake3, --blks Direct shortcuts for SHA-256, BLAKE3 and BLKS-384");
    println!("      --direct             Use direct I/O (O_DIRECT) bypassing page cache");
    println!("      --swab               Swap adjacent byte pairs (AVX2-accelerated)");
    println!("      --sparse             Punch holes for blocks of zeros");
    println!("      --sync               Pad short reads with zero bytes");
}
