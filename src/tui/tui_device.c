/**
 * @file tui_device.c
 * @brief Block device discovery implementation for dd-tui
 */

#include <config.h>
#include "tui_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <ctype.h>

static inline bool
is_partition_of_device(char const *dev_name, char const *disk_name)
{
    size_t dlen = strlen(disk_name);
    if (dlen == 0 || strncmp(dev_name, disk_name, dlen) != 0)
        return false;

    char next = dev_name[dlen];
    if (next == '\0')
        return true;

    char last_disk_char = disk_name[dlen - 1];
    if (isdigit((unsigned char)last_disk_char)) {
        if (next == 'p' && isdigit((unsigned char)dev_name[dlen + 1]))
            return true;
        return false;
    } else {
        if (isdigit((unsigned char)next))
            return true;
        return false;
    }
}

static void
trim_string(char *s)
{
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
}

void
tui_format_size(uint64_t bytes, char *buf, size_t buf_len)
{
    if (bytes == 0) {
        snprintf(buf, buf_len, "0 B");
        return;
    }
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int idx = 0;
    double d = (double)bytes;
    while (d >= 1000.0 && idx < 5) {
        d /= 1000.0;
        idx++;
    }
    if (idx == 0)
        snprintf(buf, buf_len, "%llu B", (unsigned long long)bytes);
    else
        snprintf(buf, buf_len, "%.1f %s", d, units[idx]);
}

static void
check_mount_status(tui_device_t *dev)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return;

    struct stat dev_st;
    if (stat(dev->path, &dev_st) != 0 || !S_ISBLK(dev_st.st_mode)) {
        fclose(fp);
        return;
    }

    char line[1024];
    char m_dev[512];
    char m_point[512];

    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, "%511s %511s", m_dev, m_point) != 2)
            continue;

        struct stat m_st;
        if (stat(m_dev, &m_st) == 0 && S_ISBLK(m_st.st_mode)) {
            bool matches = false;

            /* Direct device match */
            if (m_st.st_rdev == dev_st.st_rdev) {
                matches = true;
            }
            /* Partition on disk match: shares major number and name prefix */
            else if (major(m_st.st_rdev) == major(dev_st.st_rdev)) {
                char const *tgt_base = strrchr(dev->path, '/');
                char const *dev_base = strrchr(m_dev, '/');
                if (tgt_base && dev_base) {
                    tgt_base++;
                    dev_base++;
                    if (is_partition_of_device(dev_base, tgt_base))
                        matches = true;
                }
            }

            if (matches) {
                if (dev->mountpoint[0] == '\0') {
                    snprintf(dev->mountpoint, sizeof dev->mountpoint, "%.255s", m_point);
                }
                if (strcmp(m_point, "/") == 0
                    || strcmp(m_point, "/boot") == 0
                    || strcmp(m_point, "/boot/efi") == 0
                    || strcmp(m_point, "/home") == 0) {
                    dev->is_system_root = true;
                }
            }
        }
    }
    fclose(fp);
}

size_t
tui_scan_devices(tui_device_t *devices, size_t max_dev)
{
    size_t count = 0;
    DIR *dir = opendir("/sys/block");
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_dev) {
        /* Skip . and .. */
        if (ent->d_name[0] == '.')
            continue;

        /* Skip virtual loop/ram/dm unless partitions */
        if (strncmp(ent->d_name, "loop", 4) == 0
            || strncmp(ent->d_name, "ram", 3) == 0)
            continue;

        tui_device_t *dev = &devices[count];
        memset(dev, 0, sizeof *dev);
        snprintf(dev->name, sizeof dev->name, "%s", ent->d_name);
        snprintf(dev->path, sizeof dev->path, "/dev/%s", ent->d_name);

        /* Read capacity in 512-byte sectors */
        char syspath[512];
        snprintf(syspath, sizeof syspath, "/sys/block/%s/size", ent->d_name);
        FILE *fp = fopen(syspath, "r");
        if (fp) {
            unsigned long long sectors = 0;
            if (fscanf(fp, "%llu", &sectors) == 1) {
                dev->size_bytes = sectors * 512ULL;
            }
            fclose(fp);
        }

        /* Skip empty 0-byte devices */
        if (dev->size_bytes == 0)
            continue;

        /* Check removable / hotplug flag */
        snprintf(syspath, sizeof syspath, "/sys/block/%s/removable", ent->d_name);
        fp = fopen(syspath, "r");
        if (fp) {
            int rem = 0;
            if (fscanf(fp, "%d", &rem) == 1 && rem == 1) {
                dev->is_removable = true;
            }
            fclose(fp);
        }

        /* Read vendor & model */
        char vendor[64] = "";
        char model[64] = "";
        snprintf(syspath, sizeof syspath, "/sys/block/%s/device/vendor", ent->d_name);
        fp = fopen(syspath, "r");
        if (fp) {
            if (fgets(vendor, sizeof vendor, fp)) trim_string(vendor);
            fclose(fp);
        }

        snprintf(syspath, sizeof syspath, "/sys/block/%s/device/model", ent->d_name);
        fp = fopen(syspath, "r");
        if (fp) {
            if (fgets(model, sizeof model, fp)) trim_string(model);
            fclose(fp);
        }

        if (model[0] == '\0') {
            /* Fallback for NVMe: /sys/block/nvmeXn1/device/model */
            snprintf(syspath, sizeof syspath, "/sys/block/%s/../model", ent->d_name);
            fp = fopen(syspath, "r");
            if (fp) {
                if (fgets(model, sizeof model, fp)) trim_string(model);
                fclose(fp);
            }
        }

        if (vendor[0] != '\0' && model[0] != '\0')
            snprintf(dev->model, sizeof dev->model, "%s %s", vendor, model);
        else if (model[0] != '\0')
            snprintf(dev->model, sizeof dev->model, "%s", model);
        else
            snprintf(dev->model, sizeof dev->model, "Disk Device (%.100s)", dev->name);

        /* Cross-check mounts */
        check_mount_status(dev);

        count++;
    }
    closedir(dir);
    return count;
}
