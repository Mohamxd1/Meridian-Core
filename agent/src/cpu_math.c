#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "meridian.h"

int parse_cpu_line(const char *line, char *label, size_t label_cap, cpu_times_t *t) {
    if (strncmp(line, "cpu", 3) != 0) return -1;
    const char *p = line;
    size_t n = 0;
    while (*p && !isspace((unsigned char)*p)) {
        if (n + 1 < label_cap) label[n++] = *p;
        p++;
    }
    label[n] = '\0';
    unsigned long long v[8] = {0};
    int got = sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
                     &v[4], &v[5], &v[6], &v[7]);
    if (got < 4) return -1; /* user nice system idle are mandatory; older kernels omit the rest */
    t->user = v[0]; t->nice = v[1]; t->system = v[2]; t->idle = v[3];
    t->iowait = v[4]; t->irq = v[5]; t->softirq = v[6]; t->steal = v[7];
    return 0;
}

double cpu_util_pct(const cpu_times_t *prev, const cpu_times_t *cur) {
    uint64_t busy_p = prev->user + prev->nice + prev->system + prev->irq + prev->softirq + prev->steal;
    uint64_t busy_c = cur->user + cur->nice + cur->system + cur->irq + cur->softirq + cur->steal;
    uint64_t idle_p = prev->idle + prev->iowait, idle_c = cur->idle + cur->iowait;
    if (busy_c < busy_p || idle_c < idle_p) return -1.0; /* counter reset */
    double busy = (double)(busy_c - busy_p);
    double total = busy + (double)(idle_c - idle_p);
    if (total <= 0.0) return -1.0;
    return 100.0 * busy / total;
}

double rate_per_s(uint64_t prev, uint64_t cur, double dt) {
    if (dt <= 0.0 || cur < prev) return -1.0;
    return (double)(cur - prev) / dt;
}
