#ifdef _WIN32
#include <windows.h>

#include "meridian.h"

void collect_disk(ring_t *r, double ts) {
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        char root[4] = {(char)('A' + i), ':', '\\', '\0'};
        if (GetDriveTypeA(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER avail, total, free_b;
        if (!GetDiskFreeSpaceExA(root, &avail, &total, &free_b) || total.QuadPart == 0) continue;
        double used = (double)(total.QuadPart - free_b.QuadPart);
        char dev[3] = {root[0], ':', '\0'};
        ring_push(r, ts, "disk.used_pct", dev, 100.0 * used / (double)total.QuadPart);
    }
}
#else
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#include "meridian.h"

static int real_fs(const char *t) {
    static const char *ok[] = {"ext2", "ext3", "ext4", "xfs", "btrfs", "zfs", "f2fs",
                               "vfat", "exfat", "ntfs", "ntfs3", "fuseblk", "reiserfs", "jfs"};
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
        if (!strcmp(t, ok[i])) return 1;
    return 0;
}

void collect_disk(ring_t *r, double ts) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char line[1024], dev[256], mnt[512], fst[64];
    char seen[32][256];
    int nseen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%255s %511s %63s", dev, mnt, fst) != 3 || !real_fs(fst)) continue;
        int dup = 0;
        for (int i = 0; i < nseen; i++)
            if (!strcmp(seen[i], dev)) dup = 1; /* bind mounts of one device: report once */
        if (dup || nseen >= 32) continue;
        strncpy(seen[nseen], dev, 255);
        seen[nseen++][255] = '\0';
        struct statvfs s;
        if (statvfs(mnt, &s) != 0 || s.f_blocks == 0) continue;
        /* same definition as df: used / (used + available to unprivileged users) */
        double used = (double)(s.f_blocks - s.f_bfree);
        double denom = used + (double)s.f_bavail;
        if (denom <= 0) continue;
        ring_push(r, ts, "disk.used_pct", mnt, 100.0 * used / denom);
    }
    fclose(f);
}
#endif
