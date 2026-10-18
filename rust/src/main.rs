mod args;
mod engine;
mod hash;
mod safety;
mod stats;
mod swab;
mod uring;

use anyhow::Result;

fn main() -> Result<()> {
    let cfg = match args::parse_args() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("blkcp: {}", e);
            std::process::exit(1);
        }
    };

    // Target Safety Guard: Check mount status of output target (HOL-Guard compatible)
    if let Some(ref out_path) = cfg.output {
        if let Err(e) = safety::check_target_safety(out_path, cfg.force) {
            eprintln!("blkcp: {}", e);
            std::process::exit(1);
        }
    }

    if let Err(e) = engine::execute_transfer(&cfg) {
        eprintln!("blkcp: error: {}", e);
        std::process::exit(1);
    }

    Ok(())
}
