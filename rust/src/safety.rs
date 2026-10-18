use anyhow::{anyhow, Result};
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::Path;

pub fn check_target_safety(output_path: &Path, force: bool) -> Result<()> {
    if force {
        return Ok(());
    }

    let canonical = match output_path.canonicalize() {
        Ok(p) => p,
        Err(_) => output_path.to_path_buf(),
    };

    let path_str = canonical.to_string_lossy();

    // Only block devices need mount protection
    if !path_str.starts_with("/dev/") {
        return Ok(());
    }

    // Special pseudo devices that are always safe to overwrite/discard
    if path_str == "/dev/null" || path_str == "/dev/zero" {
        return Ok(());
    }

    // Inspect /proc/mounts
    let mounts_file = match File::open("/proc/mounts") {
        Ok(f) => f,
        Err(_) => return Ok(()), // /proc/mounts unavailable (e.g. non-Linux or chroot)
    };

    let reader = BufReader::new(mounts_file);
    for line in reader.lines().map_while(Result::ok) {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() < 2 {
            continue;
        }

        let mounted_dev = parts[0];
        let mount_point = parts[1];

        // Direct device match
        if mounted_dev == path_str {
            return Err(anyhow!(
                "Target Safety Guard: Target device '{}' is mounted on '{}'! Refusing destructive write without --force / -f.",
                path_str,
                mount_point
            ));
        }

        // Partition match on parent disk (e.g. target is /dev/sda and /dev/sda1 is mounted)
        if mounted_dev.starts_with(path_str.as_ref()) {
            let suffix = &mounted_dev[path_str.len()..];
            if suffix.chars().all(|c| c.is_ascii_digit() || c == 'p') {
                return Err(anyhow!(
                    "Target Safety Guard: Partition '{}' of target device '{}' is mounted on '{}'! Refusing destructive write without --force / -f.",
                    mounted_dev,
                    path_str,
                    mount_point
                ));
            }
        }

        // Device-Mapper & Holder match (e.g. dm-0 holds nvme0n1p3 and is mounted on /)
        let dev_name = path_str.trim_start_matches("/dev/");
        let holders_path = format!("/sys/class/block/{}/holders", dev_name);
        if let Ok(entries) = std::fs::read_dir(holders_path) {
            for entry in entries.flatten() {
                let holder_name = entry.file_name().to_string_lossy().to_string();
                let holder_dev1 = format!("/dev/{}", holder_name);
                let holder_dev2 = format!("/dev/mapper/{}", holder_name);
                if mounted_dev == holder_dev1 || mounted_dev.starts_with(&holder_dev2) {
                    return Err(anyhow!(
                        "Target Safety Guard: Active device holder '{}' (LVM/LUKS) of '{}' is mounted on '{}'! Refusing destructive write without --force / -f.",
                        holder_name,
                        path_str,
                        mount_point
                    ));
                }
            }
        }
    }

    Ok(())
}
