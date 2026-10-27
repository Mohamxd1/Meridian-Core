#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <stdlib.h>
#include <string.h>

#include "meridian.h"

/* Vendor-neutral Windows fallback: sums "3D" engine utilization and dedicated VRAM use.
 * Provides no temperature or power. Instances appear and disappear as processes start,
 * so the wildcard is re-expanded on every sample and new counters are added on demand. */
#define MAX_COUNTERS 2048
static PDH_HQUERY q;
static struct {
    char path[256];
    PDH_HCOUNTER h;
} util[MAX_COUNTERS], mem[MAX_COUNTERS];
static int n_util, n_mem;

static void sync_counters(const char *wildcard, typeof(util) tab, int *n) {
    DWORD len = 0;
    PdhExpandWildCardPathA(NULL, wildcard, NULL, &len, 0);
    if (len == 0) return;
    char *list = malloc(len);
    if (!list) return;
    if (PdhExpandWildCardPathA(NULL, wildcard, list, &len, 0) == ERROR_SUCCESS) {
        for (char *p = list; *p; p += strlen(p) + 1) {
            int known = 0;
            for (int i = 0; i < *n; i++)
                if (!strcmp(tab[i].path, p)) known = 1;
            if (known || *n >= MAX_COUNTERS || strlen(p) >= sizeof tab[0].path) continue;
            if (PdhAddEnglishCounterA(q, p, 0, &tab[*n].h) == ERROR_SUCCESS) {
                strcpy(tab[*n].path, p);
                (*n)++;
            }
        }
    }
    free(list);
}

int collect_gpu_pdh_init(void) {
    if (PdhOpenQuery(NULL, 0, &q) != ERROR_SUCCESS) return -1;
    sync_counters("\\GPU Engine(*)\\Utilization Percentage", util, &n_util);
    sync_counters("\\GPU Adapter Memory(*)\\Dedicated Usage", mem, &n_mem);
    if (n_util == 0 && n_mem == 0) return -1;
    PdhCollectQueryData(q);
    return 0;
}

void collect_gpu_pdh(ring_t *r, double ts) {
    if (!q) return;
    sync_counters("\\GPU Engine(*)\\Utilization Percentage", util, &n_util);
    sync_counters("\\GPU Adapter Memory(*)\\Dedicated Usage", mem, &n_mem);
    if (PdhCollectQueryData(q) != ERROR_SUCCESS) return;
    PDH_FMT_COUNTERVALUE v;
    double sum_util = 0, sum_mem = 0;
    int any_util = 0, any_mem = 0;
    for (int i = 0; i < n_util; i++)
        if (strstr(util[i].path, "engtype_3D") &&
            PdhGetFormattedCounterValue(util[i].h, PDH_FMT_DOUBLE, NULL, &v) == ERROR_SUCCESS) {
            sum_util += v.doubleValue;
            any_util = 1;
        }
    for (int i = 0; i < n_mem; i++)
        if (PdhGetFormattedCounterValue(mem[i].h, PDH_FMT_LARGE, NULL, &v) == ERROR_SUCCESS) {
            sum_mem += (double)v.largeValue;
            any_mem = 1;
        }
    if (any_util) ring_push(r, ts, "gpu.util", "gpu0", sum_util > 100.0 ? 100.0 : sum_util);
    if (any_mem) ring_push(r, ts, "gpu.mem_used_bytes", "gpu0", sum_mem);
}
#else
#include "meridian.h"

int collect_gpu_pdh_init(void) { return -1; } /* Windows only */
void collect_gpu_pdh(ring_t *r, double ts) { (void)r; (void)ts; }
#endif
