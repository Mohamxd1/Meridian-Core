#ifndef MERIDIAN_H
#define MERIDIAN_H

#include <stddef.h>
#include <stdint.h>

#define METRIC_LEN 64
#define DEVICE_LEN 128
#define HOST_LEN 256
#define URL_LEN 512
#define KEY_LEN 256

typedef struct {
    double ts;
    char metric[METRIC_LEN];
    char device[DEVICE_LEN];
    double value;
} sample_t;

/* ---------- ring buffer of samples: oldest dropped when full ---------- */
typedef struct {
    sample_t *items;
    size_t cap, head, count;
    size_t dropped;
} ring_t;

int ring_init(ring_t *r, size_t cap);
void ring_free(ring_t *r);
/* Returns 1 if an old sample was dropped to make room, 0 otherwise, -1 if rejected
 * (non-finite value or invalid arguments). */
int ring_push(ring_t *r, double ts, const char *metric, const char *device, double value);
size_t ring_peek(const ring_t *r, sample_t *out, size_t max); /* copies oldest-first */
void ring_pop(ring_t *r, size_t n);

/* ---------- JSON batch builder ---------- */
/* Writes the batch body. Returns the length that the full output needs (like snprintf);
 * the output is truncated and NUL-terminated if that is >= cap. */
size_t json_build(char *buf, size_t cap, const char *host, const char *version,
                  const sample_t *s, size_t n);
size_t json_escape(char *dst, size_t cap, const char *src);
#define JSON_BYTES_PER_SAMPLE 320

/* ---------- CPU math ---------- */
typedef struct {
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_times_t;

/* Parses one "cpu"/"cpuN" line of /proc/stat. Returns 0 on success. */
int parse_cpu_line(const char *line, char *label, size_t label_cap, cpu_times_t *t);
/* Utilization 0-100 from two readings; returns -1 if no time has elapsed. */
double cpu_util_pct(const cpu_times_t *prev, const cpu_times_t *cur);
/* Linux counter delta -> bytes per second. Returns -1 for reset/invalid input. */
double rate_per_s(uint64_t prev, uint64_t cur, double dt);

/* ---------- configuration ---------- */
typedef struct {
    char api_url[URL_LEN];
    char api_key[KEY_LEN];
    char host_name[HOST_LEN];
    int interval_s;
    int batch_s;
    size_t buffer_samples;
    int print_only;
} config_t;

void config_defaults(config_t *c);
/* Applies one KEY=VALUE pair; unknown keys are ignored. Returns 0 if applied. */
int config_set(config_t *c, const char *key, const char *value);
int config_load_file(config_t *c, const char *path);
void config_load_env(config_t *c);
int config_validate(const config_t *c, char *err, size_t err_cap);

/* ---------- HTTP ---------- */
int http_global_init(void);
void http_global_cleanup(void);
/* POSTs a JSON body. Returns 0 on 2xx, -1 otherwise; *status gets the HTTP code (0 if none). */
int http_post(const char *url, const char *api_key, const char *body, size_t len, long *status);
/* Exponential backoff in seconds: 1, 2, 4, ... capped at 30. attempt starts at 1. */
int backoff_seconds(int attempt);

/* ---------- platform helpers ---------- */
double now_unix(void);
void sleep_ms(unsigned ms);

/* ---------- collectors: each appends samples to the ring ---------- */
int collect_cpu_init(void);
void collect_cpu(ring_t *r, double ts);
void collect_mem(ring_t *r, double ts);
void collect_disk(ring_t *r, double ts);
void collect_net(ring_t *r, double ts);
int collect_gpu_nvml_init(void);
int collect_gpu_nvml(ring_t *r, double ts); /* returns number of GPUs sampled */
void collect_gpu_nvml_shutdown(void);
int collect_gpu_pdh_init(void);
void collect_gpu_pdh(ring_t *r, double ts);

#endif
