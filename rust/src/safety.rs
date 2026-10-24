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

fn check_device_relation(
    active_dev: &str,
    target_path_str: &str,
    all_holders: &HashSet<String>,
    usage_desc: &str,
) -> Option<String> {
    // 1. Direct device match
    if active_dev == target_path_str {
        return Some(format!(
            "Target device '{}' is {}",
            target_path_str, usage_desc
        ));
    }

    // 2. Partition match on parent disk (e.g. target is /dev/sda and /dev/sda1 is active)
    if let Some(suffix) = active_dev.strip_prefix(target_path_str) {
        if !suffix.is_empty() && suffix.chars().all(|c| c.is_ascii_digit() || c == 'p') {
            return Some(format!(
                "Partition '{}' of target device '{}' is {}",
                active_dev, target_path_str, usage_desc
            ));
        }
    }

    // 3. Transitive Device-Mapper / LUKS / LVM / mdadm holder match
    for holder_name in all_holders {
        let holder_dev1 = format!("/dev/{}", holder_name);
        let holder_dev2 = format!("/dev/mapper/{}", holder_name);
        if active_dev == holder_dev1 || active_dev.starts_with(&holder_dev2) {
            return Some(format!(
                "Active device holder '{}' (LVM/LUKS) of '{}' is {}",
                holder_name, target_path_str, usage_desc
            ));
        }
    }

    None
}

/// Evaluates whether the output target is a mounted system/block device or active swap.
/// Returns `Some(detail)` if the target or one of its partitions/holders is mounted or active swap.
pub fn detect_mounted_violation(output_path: &Path) -> Option<String> {
    let canonical = match output_path.canonicalize() {
        Ok(p) => p,
        Err(_) => output_path.to_path_buf(),
    };

    let path_str = canonical.to_string_lossy();

    // Only block devices in /dev/ need mount/swap protection
    if !path_str.starts_with("/dev/") {
        return None;
    }

    // Special pseudo devices that are always safe to overwrite/discard
    if path_str == "/dev/null" || path_str == "/dev/zero" {
        return None;
    }

    let dev_name = path_str.trim_start_matches("/dev/");
    let all_holders = collect_all_holders(dev_name);

    // 1. Check /proc/mounts
    if let Ok(mounts_file) = File::open("/proc/mounts") {
        let reader = BufReader::new(mounts_file);
        for line in reader.lines().map_while(Result::ok) {
            let parts: Vec<&str> = line.split_whitespace().collect();
            if parts.len() < 2 {
                continue;
            }

            let raw_mounted_dev = parts[0];
            let mount_point = parts[1];
            let usage_desc = format!("mounted on '{}'", mount_point);

            let canon_mounted = if raw_mounted_dev.starts_with("/dev/") {
                Path::new(raw_mounted_dev)
                    .canonicalize()
                    .ok()
                    .map(|p| p.to_string_lossy().to_string())
            } else {
                None
            };
            let candidates: [&str; 2] = [
                raw_mounted_dev,
                canon_mounted.as_deref().unwrap_or(raw_mounted_dev),
            ];

            for cand in candidates {
                if let Some(violation) =
                    check_device_relation(cand, &path_str, &all_holders, &usage_desc)
                {
                    return Some(violation);
                }
            }
        }
    }

    // 2. Check /proc/swaps for active system swap partitions/devices
    if let Ok(swaps_file) = File::open("/proc/swaps") {
        let reader = BufReader::new(swaps_file);
        for (idx, line) in reader.lines().map_while(Result::ok).enumerate() {
            if idx == 0 {
                continue; // Skip header line
            }
            let parts: Vec<&str> = line.split_whitespace().collect();
            if parts.is_empty() {
                continue;
            }

            let raw_swap_dev = parts[0];
            let usage_desc = "active system swap";

            let canon_swap = if raw_swap_dev.starts_with("/dev/") {
                Path::new(raw_swap_dev)
                    .canonicalize()
                    .ok()
                    .map(|p| p.to_string_lossy().to_string())
            } else {
                None
            };
            let candidates: [&str; 2] =
                [raw_swap_dev, canon_swap.as_deref().unwrap_or(raw_swap_dev)];

            for cand in candidates {
                if let Some(violation) =
                    check_device_relation(cand, &path_str, &all_holders, usage_desc)
                {
                    return Some(violation);
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

    #[test]
    fn test_device_relation_matching() {
        let mut holders = HashSet::new();
        holders.insert("dm-0".to_string());

        // Direct match
        assert!(
            check_device_relation("/dev/nvme0n1", "/dev/nvme0n1", &holders, "mounted on '/'")
                .is_some()
        );

        // Partition match on parent disk
        assert!(check_device_relation(
            "/dev/nvme0n1p2",
            "/dev/nvme0n1",
            &holders,
            "active system swap"
        )
        .is_some());

        // Transitive LUKS/LVM holder match
        assert!(
            check_device_relation("/dev/dm-0", "/dev/nvme0n1", &holders, "mounted on '/'")
                .is_some()
        );

        // Unrelated disk must not match
        assert!(
            check_device_relation("/dev/sdb1", "/dev/nvme0n1", &holders, "mounted on '/mnt'")
                .is_none()
        );
    }
}
