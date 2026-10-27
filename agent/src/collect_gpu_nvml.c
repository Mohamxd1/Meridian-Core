#ifdef _WIN32
#include <windows.h>
typedef HMODULE lib_t;
static lib_t open_lib(void) {
    lib_t h = LoadLibraryA("nvml.dll");
    if (!h) h = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    return h;
}
#define SYM(h, n) ((void *)GetProcAddress((h), (n)))
#define CLOSE_LIB(h) FreeLibrary(h)
#else
#include <dlfcn.h>
typedef void *lib_t;
static lib_t open_lib(void) { return dlopen("libnvidia-ml.so.1", RTLD_NOW); }
#define SYM(h, n) dlsym((h), (n))
#define CLOSE_LIB(h) dlclose(h)
#endif

#include <stdio.h>

#include "meridian.h"

/* Hand-written typedefs mirror nvml.h; the library itself is always resolved at runtime so
 * the same binary runs on machines without an NVIDIA driver. NVML returns 0 on success. */
typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;
typedef struct { unsigned long long total, free, used; } nvmlMemory_t;
typedef int (*fn_void_t)(void);
typedef int (*fn_count_t)(unsigned int *);
typedef int (*fn_handle_t)(unsigned int, void **);
typedef int (*fn_util_t)(void *, nvmlUtilization_t *);
typedef int (*fn_mem_t)(void *, nvmlMemory_t *);
typedef int (*fn_temp_t)(void *, int, unsigned int *);
typedef int (*fn_power_t)(void *, unsigned int *);

static struct {
    lib_t lib;
    fn_void_t init, shutdown;
    fn_count_t count;
    fn_handle_t handle;
    fn_util_t util;
    fn_mem_t mem;
    fn_temp_t temp;
    fn_power_t power;
    unsigned int n;
    int up;
} N;

int collect_gpu_nvml_init(void) {
    N.lib = open_lib();
    if (!N.lib) return -1;
    N.init = (fn_void_t)SYM(N.lib, "nvmlInit_v2");
    N.shutdown = (fn_void_t)SYM(N.lib, "nvmlShutdown");
    N.count = (fn_count_t)SYM(N.lib, "nvmlDeviceGetCount_v2");
    N.handle = (fn_handle_t)SYM(N.lib, "nvmlDeviceGetHandleByIndex_v2");
    N.util = (fn_util_t)SYM(N.lib, "nvmlDeviceGetUtilizationRates");
    N.mem = (fn_mem_t)SYM(N.lib, "nvmlDeviceGetMemoryInfo");
    N.temp = (fn_temp_t)SYM(N.lib, "nvmlDeviceGetTemperature");
    N.power = (fn_power_t)SYM(N.lib, "nvmlDeviceGetPowerUsage");
    if (!N.init || !N.shutdown || !N.count || !N.handle || N.init() != 0) {
        CLOSE_LIB(N.lib);
        N.lib = NULL;
        return -1;
    }
    if (N.count(&N.n) != 0 || N.n == 0) {
        N.shutdown();
        CLOSE_LIB(N.lib);
        N.lib = NULL;
        return -1;
    }
    N.up = 1;
    return (int)N.n;
}

int collect_gpu_nvml(ring_t *r, double ts) {
    if (!N.up) return 0;
    int sampled = 0;
    for (unsigned int i = 0; i < N.n; i++) {
        void *h;
        char dev[16];
        if (N.handle(i, &h) != 0) continue;
        snprintf(dev, sizeof dev, "gpu%u", i);
        /* Each metric is independent: NOT_SUPPORTED on one must not drop the others. */
        nvmlUtilization_t u;
        if (N.util && N.util(h, &u) == 0) {
            ring_push(r, ts, "gpu.util", dev, (double)u.gpu);
            ring_push(r, ts, "gpu.mem_util", dev, (double)u.memory);
        }
        nvmlMemory_t m;
        if (N.mem && N.mem(h, &m) == 0) {
            ring_push(r, ts, "gpu.mem_used_bytes", dev, (double)m.used);
            ring_push(r, ts, "gpu.mem_total_bytes", dev, (double)m.total);
        }
        unsigned int t;
        if (N.temp && N.temp(h, 0 /* NVML_TEMPERATURE_GPU */, &t) == 0)
            ring_push(r, ts, "gpu.temp_c", dev, (double)t);
        unsigned int mw;
        if (N.power && N.power(h, &mw) == 0)
            ring_push(r, ts, "gpu.power_w", dev, mw / 1000.0); /* NVML reports milliwatts */
        sampled++;
    }
    return sampled;
}

void collect_gpu_nvml_shutdown(void) {
    if (N.up) N.shutdown();
    if (N.lib) CLOSE_LIB(N.lib);
    N.up = 0;
    N.lib = NULL;
}
