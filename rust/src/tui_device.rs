use std::fs::{self, File};
use std::io::{BufRead, BufReader};
use std::path::PathBuf;

#[allow(dead_code)]
#[derive(Debug, Clone)]
pub struct BlockDevice {
    pub name: String,
    pub path: PathBuf,
    pub size_bytes: u64,
    pub model: String,
    pub is_removable: bool,
    pub is_read_only: bool,
    pub mount_points: Vec<String>,
    pub is_system_root: bool,
}

impl BlockDevice {
    pub fn size_human(&self) -> String {
        let b = self.size_bytes;
        if b >= 1024 * 1024 * 1024 * 1024 {
            format!("{:.1} TB", b as f64 / 1_099_511_627_776.0)
        } else if b >= 1024 * 1024 * 1024 {
            format!("{:.1} GB", b as f64 / 1_073_741_824.0)
        } else if b >= 1024 * 1024 {
            format!("{:.1} MB", b as f64 / 1_048_576.0)
        } else {
            format!("{} B", b)
        }
    }
}

pub fn scan_block_devices() -> Vec<BlockDevice> {
    let mut mounts_map: std::collections::HashMap<String, Vec<String>> =
        std::collections::HashMap::new();

    if let Ok(file) = File::open("/proc/mounts") {
        let reader = BufReader::new(file);
        for line in reader.lines().map_while(Result::ok) {
            let parts: Vec<&str> = line.split_whitespace().collect();
            if parts.len() >= 2 {
                let dev = parts[0].to_string();
                let mp = parts[1].to_string();
                mounts_map.entry(dev).or_default().push(mp);
            }
        }
    }

    let mut devices = Vec::new();
    let sys_block = PathBuf::from("/sys/class/block");

    if let Ok(entries) = fs::read_dir(sys_block) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();

            // Filter out loop, ram, and dm devices by default
            if name.starts_with("loop") || name.starts_with("ram") || name.starts_with("zram") {
                continue;
            }

            // Only top-level disks (ignore individual partitions in main list, or list as children)
            // Top level disks don't have a 'partition' file in their sysfs dir
            let is_partition = entry.path().join("partition").exists();
            if is_partition {
                continue;
            }

            let path = PathBuf::from(format!("/dev/{}", name));

            // Read size (in 512-byte sectors)
            let size_bytes = fs::read_to_string(entry.path().join("size"))
                .ok()
                .and_then(|s| s.trim().parse::<u64>().ok())
                .unwrap_or(0)
                * 512;

            if size_bytes == 0 {
                continue;
            }

            // Read model
            let model = fs::read_to_string(entry.path().join("device/model"))
                .or_else(|_| fs::read_to_string(entry.path().join("device/name")))
                .unwrap_or_else(|_| "Generic Storage".to_string())
                .trim()
                .to_string();

            // Removable flag
            let is_removable = fs::read_to_string(entry.path().join("removable"))
                .ok()
                .and_then(|s| s.trim().parse::<u32>().ok())
                .map(|v| v == 1)
                .unwrap_or(false);

            // Read-only flag
            let is_read_only = fs::read_to_string(entry.path().join("ro"))
                .ok()
                .and_then(|s| s.trim().parse::<u32>().ok())
                .map(|v| v == 1)
                .unwrap_or(false);

            // Collect all mount points of disk and its partitions
            let mut dev_mounts = Vec::new();
            let mut is_root = false;

            let path_str = path.to_string_lossy();
            for (mdev, mps) in &mounts_map {
                if mdev == &*path_str || mdev.starts_with(&*path_str) {
                    for mp in mps {
                        dev_mounts.push(mp.clone());
                        if mp == "/" || mp == "/boot" || mp == "/home" {
                            is_root = true;
                        }
                    }
                }
            }

            devices.push(BlockDevice {
                name,
                path,
                size_bytes,
                model,
                is_removable,
                is_read_only,
                mount_points: dev_mounts,
                is_system_root: is_root,
            });
        }
    }

    devices.sort_by(|a, b| a.name.cmp(&b.name));
    devices
}
