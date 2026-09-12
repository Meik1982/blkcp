/**
 * @file tui_device.h
 * @brief Block device discovery and inspection for dd-tui
 */

#ifndef TUI_DEVICE_H
#define TUI_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_DEVICES 64
#define DEV_NAME_LEN 256
#define DEV_MODEL_LEN 128
#define DEV_PATH_LEN 512

/**
 * @brief Information about a discovered block storage device
 */
typedef struct tui_device {
    char name[DEV_NAME_LEN];          /**< Device node name (e.g. sda, nvme0n1) */
    char path[DEV_PATH_LEN];          /**< Full /dev path (e.g. /dev/sda) */
    char model[DEV_MODEL_LEN];        /**< Model / Vendor string */
    uint64_t size_bytes;              /**< Capacity in bytes */
    bool is_removable;                /**< True if USB or hotplug removable */
    bool is_system_root;              /**< True if hosting /, /boot, or /home */
    char mountpoint[DEV_PATH_LEN];    /**< Primary mountpoint if mounted */
} tui_device_t;

/**
 * @brief Scans /sys/block and /proc/mounts for available block devices
 * @param devices Output array of discovered devices
 * @param max_dev Maximum items to store
 * @return Number of discovered devices
 */
size_t tui_scan_devices(tui_device_t *devices, size_t max_dev);

/**
 * @brief Formats byte capacity to human-readable string (e.g., "31.8 GB")
 * @param bytes Size in bytes
 * @param buf Output buffer
 * @param buf_len Length of output buffer
 */
void tui_format_size(uint64_t bytes, char *buf, size_t buf_len);

#endif /* TUI_DEVICE_H */
