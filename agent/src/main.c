#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meridian.h"

#define AGENT_VERSION "0.1.0"
#define POST_CHUNK 1000

static volatile sig_atomic_t stop_flag;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static void collect_all(ring_t *r, int have_nvml, int have_pdh) {
    double ts = now_unix();
    collect_cpu(r, ts);
    collect_mem(r, ts);
    collect_disk(r, ts);
    collect_net(r, ts);
    if (have_nvml) collect_gpu_nvml(r, ts);
    else if (have_pdh) collect_gpu_pdh(r, ts);
}

/* Sends buffered samples oldest-first in chunks. Samples leave the ring only after the
 * server acknowledged them. Returns 0 if the ring was fully drained. */
static int flush(ring_t *r, const config_t *c) {
    static sample_t chunk[POST_CHUNK];
    char *body = malloc((size_t)POST_CHUNK * JSON_BYTES_PER_SAMPLE + 1024);
    if (!body) return -1;
    int rc = 0;
    while (r->count > 0) {
        size_t n = ring_peek(r, chunk, POST_CHUNK);
        size_t cap = n * JSON_BYTES_PER_SAMPLE + 1024;
        size_t len = json_build(body, cap, c->host_name, AGENT_VERSION, chunk, n);
        long status;
        if (len >= cap || http_post(c->api_url, c->api_key, body, len, &status) != 0) {
            fprintf(stderr, "meridian-agent: POST failed (http %ld); %zu samples buffered\n", status,
                    r->count);
            rc = -1;
            break;
        }
        ring_pop(r, n);
    }
    free(body);
    return rc;
}

static void print_readout(const ring_t *r) {
    static sample_t tmp[4096];
    size_t n = ring_peek(r, tmp, 4096);
    for (size_t i = 0; i < n; i++)
        printf("%-22s %-14s %14.3f\n", tmp[i].metric, tmp[i].device, tmp[i].value);
    printf("--\n");
    fflush(stdout);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-c agent.conf] [--print]\n"
            "  --print   local readout every 2 s (phase 1); sends nothing\n"
            "Settings come from the file, then environment: API_URL API_KEY HOST_NAME\n"
            "INTERVAL_SECONDS BATCH_SECONDS BUFFER_SAMPLES\n",
            argv0);
}

int main(int argc, char **argv) {
    config_t cfg;
    config_defaults(&cfg);
    const char *conf_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) conf_path = argv[++i];
        else if (!strcmp(argv[i], "--print")) cfg.print_only = 1;
        else { usage(argv[0]); return 2; }
    }
    if (conf_path && config_load_file(&cfg, conf_path) != 0) {
        fprintf(stderr, "meridian-agent: cannot read %s\n", conf_path);
        return 2;
    }
    config_load_env(&cfg);
    if (cfg.print_only) cfg.interval_s = 2, cfg.batch_s = 2;
    char err[128];
    if (config_validate(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "meridian-agent: %s\n", err);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    ring_t ring;
    if (ring_init(&ring, cfg.buffer_samples) != 0 || http_global_init() != 0) return 1;

    collect_cpu_init();
    int have_nvml = collect_gpu_nvml_init() > 0;
    int have_pdh = !have_nvml && collect_gpu_pdh_init() == 0;
    fprintf(stderr, "meridian-agent %s host=%s gpu=%s\n", AGENT_VERSION, cfg.host_name,
            have_nvml ? "nvml" : have_pdh ? "pdh" : "none");

    double next_collect = now_unix(), next_flush = next_collect + cfg.batch_s;
    int failures = 0;
    double retry_at = 0;
    while (!stop_flag) {
        double now = now_unix();
        if (now >= next_collect) {
            collect_all(&ring, have_nvml, have_pdh);
            next_collect += cfg.interval_s;
            if (next_collect < now) next_collect = now + cfg.interval_s; /* resumed from sleep */
        }
        if (now >= next_flush) {
            if (cfg.print_only) {
                print_readout(&ring);
                ring_pop(&ring, ring.count);
                next_flush = now + cfg.batch_s;
            } else if (now >= retry_at) {
                if (flush(&ring, &cfg) == 0) {
                    failures = 0;
                    next_flush = now + cfg.batch_s;
                } else {
                    retry_at = now + backoff_seconds(++failures);
                }
            }
        }
        sleep_ms(100);
    }
    if (!cfg.print_only && ring.count > 0) flush(&ring, &cfg); /* best effort on shutdown */
    if (ring.dropped) fprintf(stderr, "meridian-agent: %zu samples dropped (buffer full)\n", ring.dropped);
    collect_gpu_nvml_shutdown();
    ring_free(&ring);
    http_global_cleanup();
    return 0;
}
