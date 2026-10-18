use anyhow::{anyhow, Result};
use std::collections::HashSet;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::Path;

/// Collects all direct and transitive block holders (e.g., dm-0 LUKS -> dm-1 LVM)
/// for a given block device name (e.g., `nvme0n1` or `nvme0n1p3`), including holders
/// attached to any child partitions of `dev_name`.
fn collect_all_holders(dev_name: &str) -> HashSet<String> {
    let mut holders = HashSet::new();
    let mut queue = Vec::new();

    // 1. Direct holders of the device itself
    queue.push(dev_name.to_string());

    // 2. Child partitions in /sys/class/block/<dev_name>/<dev_name>*
    let sys_dev_dir = format!("/sys/class/block/{}", dev_name);
    if let Ok(entries) = std::fs::read_dir(&sys_dev_dir) {
        for entry in entries.flatten() {
            let sub_name = entry.file_name().to_string_lossy().to_string();
            if sub_name.starts_with(dev_name) && entry.path().join("partition").exists() {
                queue.push(sub_name);
            }
        }
    }

    // 3. Breadth-first traversal of /sys/class/block/<item>/holders (handles LUKS -> LVM chains)
    let mut visited = HashSet::new();
    while let Some(curr) = queue.pop() {
        if !visited.insert(curr.clone()) {
            continue;
        }
        let holders_dir = format!("/sys/class/block/{}/holders", curr);
        if let Ok(entries) = std::fs::read_dir(&holders_dir) {
            for entry in entries.flatten() {
                let h_name = entry.file_name().to_string_lossy().to_string();
                if holders.insert(h_name.clone()) {
                    queue.push(h_name);
                }
            }
        }
    }

    holders
}

/// Evaluates whether the output target is a mounted system/block device.
/// Returns `Some(detail)` if the target or one of its partitions/holders is mounted.
pub fn detect_mounted_violation(output_path: &Path) -> Option<String> {
    let canonical = match output_path.canonicalize() {
        Ok(p) => p,
        Err(_) => output_path.to_path_buf(),
    };

    let path_str = canonical.to_string_lossy();

    // Only block devices in /dev/ need mount protection
    if !path_str.starts_with("/dev/") {
        return None;
    }

    // Special pseudo devices that are always safe to overwrite/discard
    if path_str == "/dev/null" || path_str == "/dev/zero" {
        return None;
    }

    let dev_name = path_str.trim_start_matches("/dev/");
    let all_holders = collect_all_holders(dev_name);

    let mounts_file = match File::open("/proc/mounts") {
        Ok(f) => f,
        Err(_) => return None,
    };

    let reader = BufReader::new(mounts_file);
    for line in reader.lines().map_while(Result::ok) {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() < 2 {
            continue;
        }

        let raw_mounted_dev = parts[0];
        let mount_point = parts[1];

        // Resolve symlinks such as /dev/mapper/root -> /dev/dm-0
        let canon_mounted = if raw_mounted_dev.starts_with("/dev/") {
            Path::new(raw_mounted_dev)
                .canonicalize()
                .ok()
                .map(|p| p.to_string_lossy().to_string())
        } else {
            None
        };
        let mounted_candidates: [&str; 2] = [
            raw_mounted_dev,
            canon_mounted.as_deref().unwrap_or(raw_mounted_dev),
        ];

        for mounted_dev in mounted_candidates {
            // 1. Direct device match
            if mounted_dev == path_str {
                return Some(format!(
                    "Target device '{}' is mounted on '{}'",
                    path_str, mount_point
                ));
            }

            // 2. Partition match on parent disk (e.g. target is /dev/sda and /dev/sda1 is mounted)
            if mounted_dev.starts_with(path_str.as_ref()) {
                let suffix = &mounted_dev[path_str.len()..];
                if !suffix.is_empty() && suffix.chars().all(|c| c.is_ascii_digit() || c == 'p') {
                    return Some(format!(
                        "Partition '{}' of target device '{}' is mounted on '{}'",
                        mounted_dev, path_str, mount_point
                    ));
                }
            }

            // 3. Transitive Device-Mapper / LUKS / LVM / mdadm holder match
            for holder_name in &all_holders {
                let holder_dev1 = format!("/dev/{}", holder_name);
                let holder_dev2 = format!("/dev/mapper/{}", holder_name);
                if mounted_dev == holder_dev1 || mounted_dev.starts_with(&holder_dev2) {
                    return Some(format!(
                        "Active device holder '{}' (LVM/LUKS) of '{}' is mounted on '{}'",
                        holder_name, path_str, mount_point
                    ));
                }
            }
        }
    }

    None
}

pub fn check_target_safety(output_path: &Path, force: bool) -> Result<()> {
    if force {
        return Ok(());
    }

    if let Some(detail) = detect_mounted_violation(output_path) {
        return Err(anyhow!(
            "TARGET SAFETY GUARD: {}! Refusing destructive write without --force / -f.",
            detail
        ));
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_pseudo_devices_are_always_safe() {
        assert!(detect_mounted_violation(Path::new("/dev/null")).is_none());
        assert!(detect_mounted_violation(Path::new("/dev/zero")).is_none());
        assert!(check_target_safety(Path::new("/dev/null"), false).is_ok());
    }
}
