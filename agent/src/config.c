#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meridian.h"

static void copy(char *dst, size_t cap, const char *src) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void config_defaults(config_t *c) {
    memset(c, 0, sizeof *c);
    copy(c->api_url, URL_LEN, "http://localhost:8000/ingest");
    c->interval_s = 5;
    c->batch_s = 10;
    c->buffer_samples = 5000;
    copy(c->host_name, HOST_LEN, "localhost");
}

int config_set(config_t *c, const char *key, const char *value) {
    if (!strcmp(key, "API_URL")) copy(c->api_url, URL_LEN, value);
    else if (!strcmp(key, "API_KEY")) copy(c->api_key, KEY_LEN, value);
    else if (!strcmp(key, "HOST_NAME")) copy(c->host_name, HOST_LEN, value);
    else if (!strcmp(key, "INTERVAL_SECONDS")) c->interval_s = atoi(value);
    else if (!strcmp(key, "BATCH_SECONDS")) c->batch_s = atoi(value);
    else if (!strcmp(key, "BUFFER_SAMPLES")) c->buffer_samples = (size_t)strtoull(value, NULL, 10);
    else return -1;
    return 0;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

int config_load_file(config_t *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (*s == '#' || *s == '\0') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        config_set(c, trim(s), trim(eq + 1));
    }
    fclose(f);
    return 0;
}

void config_load_env(config_t *c) {
    static const char *keys[] = {"API_URL", "API_KEY", "HOST_NAME", "INTERVAL_SECONDS",
                                 "BATCH_SECONDS", "BUFFER_SAMPLES"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        const char *v = getenv(keys[i]);
        if (v && *v) config_set(c, keys[i], v);
    }
}

int config_validate(const config_t *c, char *err, size_t err_cap) {
    const char *msg = NULL;
    if (c->interval_s < 1 || c->interval_s > 3600) msg = "INTERVAL_SECONDS must be 1-3600";
    else if (c->batch_s < c->interval_s) msg = "BATCH_SECONDS must be >= INTERVAL_SECONDS";
    else if (c->buffer_samples < 100) msg = "BUFFER_SAMPLES must be >= 100";
    else if (!c->host_name[0]) msg = "HOST_NAME must not be empty";
    else if (!c->print_only && !c->api_key[0]) msg = "API_KEY is required";
    else if (!c->print_only && strncmp(c->api_url, "http", 4) != 0) msg = "API_URL must be http(s)";
    if (msg) snprintf(err, err_cap, "%s", msg);
    return msg ? -1 : 0;
}
