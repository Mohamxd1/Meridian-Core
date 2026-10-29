#ifdef _WIN32
#include <windows.h>

#include "meridian.h"

void collect_mem(ring_t *r, double ts) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return;
    ring_push(r, ts, "mem.used_pct", "", (double)m.dwMemoryLoad);
    ring_push(r, ts, "mem.used_bytes", "", (double)(m.ullTotalPhys - m.ullAvailPhys));
}
#else
#include <stdio.h>
#include <string.h>

#include "meridian.h"

void collect_mem(ring_t *r, double ts) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    unsigned long long total_kb = 0, avail_kb = 0;
    int have_total = 0, have_avail = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %llu kB", &total_kb) == 1) have_total = 1;
        else if (sscanf(line, "MemAvailable: %llu kB", &avail_kb) == 1) have_avail = 1;
    }
    fclose(f);
    if (!have_total || !have_avail || total_kb == 0 || avail_kb > total_kb) return;
    double used = (double)(total_kb - avail_kb) * 1024.0;
    ring_push(r, ts, "mem.used_pct", "", 100.0 * (double)(total_kb - avail_kb) / (double)total_kb);
    ring_push(r, ts, "mem.used_bytes", "", used);
}
#endif
