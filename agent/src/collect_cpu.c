#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <stdio.h>

#include "meridian.h"

#define MAX_CORES 256
static PDH_HQUERY q;
static PDH_HCOUNTER total_ctr, core_ctr[MAX_CORES];
static DWORD ncores;

int collect_cpu_init(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ncores = si.dwNumberOfProcessors > MAX_CORES ? MAX_CORES : si.dwNumberOfProcessors;
    if (PdhOpenQuery(NULL, 0, &q) != ERROR_SUCCESS) return -1;
    if (PdhAddEnglishCounterA(q, "\\Processor(_Total)\\% Processor Time", 0, &total_ctr) !=
        ERROR_SUCCESS)
        return -1;
    for (DWORD i = 0; i < ncores; i++) {
        char path[96];
        snprintf(path, sizeof path, "\\Processor(%lu)\\%% Processor Time", (unsigned long)i);
        if (PdhAddEnglishCounterA(q, path, 0, &core_ctr[i]) != ERROR_SUCCESS) core_ctr[i] = NULL;
    }
    PdhCollectQueryData(q); /* priming sample: the first value is not valid yet */
    return 0;
}

void collect_cpu(ring_t *r, double ts) {
    PDH_FMT_COUNTERVALUE v;
    if (!q || PdhCollectQueryData(q) != ERROR_SUCCESS) return;
    if (PdhGetFormattedCounterValue(total_ctr, PDH_FMT_DOUBLE, NULL, &v) == ERROR_SUCCESS)
        ring_push(r, ts, "cpu.util_total", "total", v.doubleValue);
    for (DWORD i = 0; i < ncores; i++) {
        char dev[16];
        if (!core_ctr[i]) continue;
        if (PdhGetFormattedCounterValue(core_ctr[i], PDH_FMT_DOUBLE, NULL, &v) != ERROR_SUCCESS)
            continue;
        snprintf(dev, sizeof dev, "%lu", (unsigned long)i);
        ring_push(r, ts, "cpu.util_core", dev, v.doubleValue);
    }
}
#else
#include <stdio.h>
#include <string.h>

#include "meridian.h"

#define MAX_CPUS 257 /* aggregate line + 256 cores */
static cpu_times_t prev[MAX_CPUS];
static int have_prev[MAX_CPUS];

int collect_cpu_init(void) {
    memset(have_prev, 0, sizeof have_prev);
    return 0;
}

void collect_cpu(ring_t *r, double ts) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    char line[512], label[32];
    int idx = 0;
    while (fgets(line, sizeof line, f) && idx < MAX_CPUS) {
        cpu_times_t cur;
        if (parse_cpu_line(line, label, sizeof label, &cur) != 0) break;
        if (have_prev[idx]) { /* first reading is discarded: needs two samples */
            double pct = cpu_util_pct(&prev[idx], &cur);
            if (pct >= 0.0) {
                if (!strcmp(label, "cpu")) ring_push(r, ts, "cpu.util_total", "total", pct);
                else ring_push(r, ts, "cpu.util_core", label + 3, pct);
            }
        }
        prev[idx] = cur;
        have_prev[idx] = 1;
        idx++;
    }
    fclose(f);
}
#endif
