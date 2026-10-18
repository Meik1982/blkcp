use std::fs::{self, File};
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EndpointKind {
    PhysicalDisk,
    Partition,
    VirtualDevice,
    RegularFile,
}

#[derive(Debug, Clone)]
pub struct BlockDevice {
    pub name: String,
    pub path: PathBuf,
    pub size_bytes: u64,
    pub model: String,
    pub kind: EndpointKind,
    pub is_removable: bool,
    pub is_read_only: bool,
    pub mount_points: Vec<String>,
    pub is_system_root: bool,
}

impl BlockDevice {
    pub fn size_human(&self) -> String {
        format_bytes_human(self.size_bytes)
    }

    pub fn from_custom_path(path: PathBuf) -> Self {
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_else(|| path.to_string_lossy().to_string());

        let (size_bytes, exists) = match fs::metadata(&path) {
            Ok(m) => (m.len(), true),
            Err(_) => (0, false),
        };

        let model = if exists {
            "Image / Datei (Existiert)".to_string()
        } else {
            "Neue Zieldatei (Wird erstellt)".to_string()
        };

        let mut is_root = false;
        let mut mounts = Vec::new();
        if let Some(violation) = crate::safety::detect_mounted_violation(&path) {
            is_root = true;
            mounts.push(violation);
        }

        Self {
            name,
            path,
            size_bytes,
            model,
            kind: EndpointKind::RegularFile,
            is_removable: false,
            is_read_only: false,
            mount_points: mounts,
            is_system_root: is_root,
        }
    }
}

pub fn format_bytes_human(b: u64) -> String {
    if b == 0 {
        return "Stream / 0 B".to_string();
    }
    if b >= 1024 * 1024 * 1024 * 1024 {
        format!("{:.2} TiB", b as f64 / 1_099_511_627_776.0)
    } else if b >= 1024 * 1024 * 1024 {
        format!("{:.2} GiB", b as f64 / 1_073_741_824.0)
    } else if b >= 1024 * 1024 {
        format!("{:.2} MiB", b as f64 / 1_048_576.0)
    } else if b >= 1024 {
        format!("{:.1} KiB", b as f64 / 1024.0)
    } else {
        format!("{} B", b)
    }
}

pub fn scan_block_devices_ext(include_partitions: bool) -> Vec<BlockDevice> {
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

            if name.starts_with("ram") || name.starts_with("zram") {
                continue;
            }
            if !include_partitions && name.starts_with("loop") {
                continue;
            }

            let is_partition = entry.path().join("partition").exists();
            if is_partition && !include_partitions {
                continue;
            }

            let path = PathBuf::from(format!("/dev/{}", name));

            let size_bytes = fs::read_to_string(entry.path().join("size"))
                .ok()
                .and_then(|s| s.trim().parse::<u64>().ok())
                .unwrap_or(0)
                * 512;

            if size_bytes == 0 {
                continue;
            }

            let model = fs::read_to_string(entry.path().join("device/model"))
                .or_else(|_| fs::read_to_string(entry.path().join("device/name")))
                .unwrap_or_else(|_| {
                    if is_partition {
                        "Partition".to_string()
                    } else {
                        "Block Storage".to_string()
                    }
                })
                .trim()
                .to_string();

            let is_removable = fs::read_to_string(entry.path().join("removable"))
                .ok()
                .and_then(|s| s.trim().parse::<u32>().ok())
                .map(|v| v == 1)
                .unwrap_or(false);

            let is_read_only = fs::read_to_string(entry.path().join("ro"))
                .ok()
                .and_then(|s| s.trim().parse::<u32>().ok())
                .map(|v| v == 1)
                .unwrap_or(false);

            let mut dev_mounts = Vec::new();
            let mut is_root = false;

            let path_str = path.to_string_lossy();
            for (mdev, mps) in &mounts_map {
                let matches = if is_partition {
                    mdev == &*path_str
                } else {
                    mdev == &*path_str || mdev.starts_with(&*path_str)
                };
                if matches {
                    for mp in mps {
                        if !dev_mounts.contains(mp) {
                            dev_mounts.push(mp.clone());
                        }
                        if mp == "/" || mp == "/boot" || mp == "/home" {
                            is_root = true;
                        }
                    }
                }
            }

            if !is_root && crate::safety::detect_mounted_violation(&path).is_some() {
                is_root = true;
            }

            devices.push(BlockDevice {
                name,
                path,
                size_bytes,
                model,
                kind: if is_partition {
                    EndpointKind::Partition
                } else {
                    EndpointKind::PhysicalDisk
                },
                is_removable,
                is_read_only,
                mount_points: dev_mounts,
                is_system_root: is_root,
            });
        }
    }

    devices.sort_by(|a, b| a.name.cmp(&b.name));

    // Append virtual stream endpoints useful for benchmarking, wiping, and testing
    devices.push(BlockDevice {
        name: "zero".to_string(),
        path: PathBuf::from("/dev/zero"),
        size_bytes: 0,
        model: "Virtueller Null-Stream (Wipe / Benchmark)".to_string(),
        kind: EndpointKind::VirtualDevice,
        is_removable: false,
        is_read_only: false,
        mount_points: Vec::new(),
        is_system_root: false,
    });
    devices.push(BlockDevice {
        name: "urandom".to_string(),
        path: PathBuf::from("/dev/urandom"),
        size_bytes: 0,
        model: "CSPRNG Zufalls-Stream (Shred / Test)".to_string(),
        kind: EndpointKind::VirtualDevice,
        is_removable: false,
        is_read_only: true,
        mount_points: Vec::new(),
        is_system_root: false,
    });
    devices.push(BlockDevice {
        name: "null".to_string(),
        path: PathBuf::from("/dev/null"),
        size_bytes: 0,
        model: "Bit-Senke (Read/Hash Benchmark)".to_string(),
        kind: EndpointKind::VirtualDevice,
        is_removable: false,
        is_read_only: false,
        mount_points: Vec::new(),
        is_system_root: false,
    });

    devices
}

pub fn scan_block_devices() -> Vec<BlockDevice> {
    scan_block_devices_ext(false)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileEntry {
    pub name: String,
    pub path: PathBuf,
    pub is_dir: bool,
    pub size_bytes: u64,
}

pub fn list_directory_entries(dir: &Path) -> Vec<FileEntry> {
    let mut entries = Vec::new();
    if let Some(parent) = dir.parent() {
        entries.push(FileEntry {
            name: ".. (Übergeordnetes Verzeichnis)".to_string(),
            path: parent.to_path_buf(),
            is_dir: true,
            size_bytes: 0,
        });
    }

    if let Ok(read_dir) = fs::read_dir(dir) {
        let mut dirs = Vec::new();
        let mut files = Vec::new();
        for entry in read_dir.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().to_string();
            if name.starts_with('.') {
                continue;
            }
            if let Ok(meta) = entry.metadata() {
                if meta.is_dir() {
                    dirs.push(FileEntry {
                        name: format!("📁 {}/", name),
                        path,
                        is_dir: true,
                        size_bytes: 0,
                    });
                } else {
                    files.push(FileEntry {
                        name,
                        path,
                        is_dir: false,
                        size_bytes: meta.len(),
                    });
                }
            }
        }
        dirs.sort_by(|a, b| a.name.cmp(&b.name));
        files.sort_by(|a, b| a.name.cmp(&b.name));
        entries.extend(dirs);
        entries.extend(files);
    }

    entries
}
