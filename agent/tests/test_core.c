#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meridian.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define NEAR(a, b) CHECK(fabs((a) - (b)) < 1e-9)

static void test_cpu_math(void) {
    cpu_times_t a, b;
    char label[16];
    CHECK(parse_cpu_line("cpu  100 0 50 800 50 0 0 0 0 0", label, sizeof label, &a) == 0);
    CHECK(strcmp(label, "cpu") == 0 && a.user == 100 && a.idle == 800 && a.iowait == 50);
    CHECK(parse_cpu_line("cpu3 1 2 3 4", label, sizeof label, &b) == 0 && strcmp(label, "cpu3") == 0);
    CHECK(parse_cpu_line("intr 12345", label, sizeof label, &b) != 0);
    CHECK(parse_cpu_line("cpu 1 2", label, sizeof label, &b) != 0);

    /* busy +30 (user 20, system 10), idle +60 (idle 50, iowait 10) -> 30/90 */
    cpu_times_t p = {100, 0, 50, 800, 50, 0, 0, 0};
    cpu_times_t c = {120, 0, 60, 850, 60, 0, 0, 0};
    NEAR(cpu_util_pct(&p, &c), 100.0 * 30.0 / 90.0);
    CHECK(cpu_util_pct(&p, &p) < 0);   /* no time elapsed */
    CHECK(cpu_util_pct(&c, &p) < 0);   /* counter went backwards */
    cpu_times_t full = {200, 0, 50, 800, 50, 0, 0, 0};
    NEAR(cpu_util_pct(&p, &full), 100.0 * 100.0 / 100.0);
    cpu_times_t steal = {100, 0, 50, 800, 50, 0, 0, 10}; /* steal counts as busy */
    NEAR(cpu_util_pct(&p, &steal), 100.0);

    NEAR(rate_per_s(1000, 3000, 2.0), 1000.0);
    CHECK(rate_per_s(3000, 1000, 2.0) < 0);
    CHECK(rate_per_s(1, 2, 0.0) < 0);
}

static void test_ring(void) {
    ring_t r;
    CHECK(ring_init(&r, 3) == 0);
    CHECK(ring_push(&r, 1, "a.b", "", 1) == 0);
    CHECK(ring_push(&r, 2, "a.b", "", 2) == 0);
    CHECK(ring_push(&r, 3, "a.b", "", 3) == 0);
    CHECK(ring_push(&r, 4, "a.b", "", 4) == 1);  /* overflow drops the oldest */
    CHECK(ring_push(&r, 5, "a.b", "", 5) == 1);
    CHECK(r.count == 3 && r.dropped == 2);
    sample_t out[8];
    CHECK(ring_peek(&r, out, 8) == 3);
    NEAR(out[0].value, 3); NEAR(out[1].value, 4); NEAR(out[2].value, 5);
    ring_pop(&r, 2);
    CHECK(r.count == 1 && ring_peek(&r, out, 8) == 1);
    NEAR(out[0].value, 5);
    ring_push(&r, 6, "a.b", "", 6);  /* wraparound still ordered */
    CHECK(ring_peek(&r, out, 8) == 2);
    NEAR(out[0].value, 5); NEAR(out[1].value, 6);
    CHECK(ring_push(&r, 7, "a.b", "", NAN) == -1 && ring_push(&r, 7, "a.b", "", INFINITY) == -1);
    CHECK(r.count == 2);
    ring_pop(&r, 99);
    CHECK(r.count == 0);
    ring_free(&r);
}

static void test_json(void) {
    char buf[1024];
    sample_t s[2];
    memset(s, 0, sizeof s);
    s[0].ts = 1761573600.0; strcpy(s[0].metric, "cpu.util_total"); strcpy(s[0].device, "total"); s[0].value = 23.5;
    s[1].ts = 1761573600.5; strcpy(s[1].metric, "gpu.util"); strcpy(s[1].device, "gp\"u\\0\n"); s[1].value = 71;
    size_t n = json_build(buf, sizeof buf, "my\"host", "0.1.0", s, 2);
    CHECK(n == strlen(buf));
    CHECK(strstr(buf, "\"host\":\"my\\\"host\"") != NULL);
    CHECK(strstr(buf, "\"ts\":1761573600.000,\"metric\":\"cpu.util_total\",\"device\":\"total\",\"value\":23.5}") != NULL);
    CHECK(strstr(buf, "\"device\":\"gp\\\"u\\\\0\\n\"") != NULL);   /* quotes, backslash, newline escaped */
    CHECK(strstr(buf, ",]") == NULL && strstr(buf, "},{") != NULL);  /* no trailing comma */
    CHECK(buf[n - 1] == '}' && buf[n - 2] == ']');

    n = json_build(buf, sizeof buf, "h", "v", s, 0);
    CHECK(strstr(buf, "\"samples\":[]}") != NULL);

    char small[40];                                  /* truncation reports the needed size */
    size_t need = json_build(small, sizeof small, "h", "v", s, 2);
    CHECK(need > sizeof small && strlen(small) < sizeof small);
    s[0].value = 0.1 + 0.2;
    json_build(buf, sizeof buf, "h", "v", s, 1);
    CHECK(strstr(buf, "0.3") != NULL);
    char esc[16];
    json_escape(esc, sizeof esc, "\x01");
    CHECK(strcmp(esc, "\\u0001") == 0);
}

static void test_config(void) {
    config_t c;
    char err[64];
    config_defaults(&c);
    CHECK(config_validate(&c, err, sizeof err) != 0);   /* API_KEY missing */
    CHECK(config_set(&c, "API_KEY", "k") == 0 && config_set(&c, "NOPE", "x") != 0);
    CHECK(config_validate(&c, err, sizeof err) == 0);
    config_set(&c, "INTERVAL_SECONDS", "20");
    CHECK(config_validate(&c, err, sizeof err) != 0);   /* batch < interval */
    config_set(&c, "BATCH_SECONDS", "30");
    CHECK(config_validate(&c, err, sizeof err) == 0);
    config_set(&c, "INTERVAL_SECONDS", "0");
    CHECK(config_validate(&c, err, sizeof err) != 0);

    FILE *f = fopen("conf_test.tmp", "w");
    fputs("# comment\nAPI_URL = http://x:1/ingest \n\nHOST_NAME=box\nbogus line\n", f);
    fclose(f);
    config_defaults(&c);
    CHECK(config_load_file(&c, "conf_test.tmp") == 0);
    CHECK(!strcmp(c.api_url, "http://x:1/ingest") && !strcmp(c.host_name, "box"));
    remove("conf_test.tmp");
    CHECK(config_load_file(&c, "does-not-exist") != 0);
}

static void test_backoff(void) {
    int want[] = {1, 2, 4, 8, 16, 30, 30, 30};
    for (int i = 0; i < 8; i++) CHECK(backoff_seconds(i + 1) == want[i]);
    CHECK(backoff_seconds(0) == 1);
}

static void test_linux_collectors_smoke(void) {
#ifndef _WIN32
    ring_t r;
    ring_init(&r, 4096);
    collect_cpu_init();
    collect_cpu(&r, 1.0);
    CHECK(r.count == 0);            /* first CPU reading is discarded */
    sleep_ms(200);
    collect_cpu(&r, 1.2);
    CHECK(r.count >= 1);
    sample_t s[4096];
    size_t n = ring_peek(&r, s, 4096);
    for (size_t i = 0; i < n; i++) CHECK(s[i].value >= 0.0 && s[i].value <= 100.0);
    ring_pop(&r, r.count);
    collect_mem(&r, 2.0);
    CHECK(r.count == 2);
    collect_disk(&r, 2.0);
    ring_free(&r);
#endif
}

int main(void) {
    test_cpu_math();
    test_ring();
    test_json();
    test_config();
    test_backoff();
    test_linux_collectors_smoke();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all agent unit tests passed\n");
    return 0;
}
