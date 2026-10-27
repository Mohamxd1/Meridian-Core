#include <stdio.h>
#include <string.h>

#include "meridian.h"

/* Escapes a string for use inside JSON quotes. Returns the full length needed. */
size_t json_escape(char *dst, size_t cap, const char *src) {
    size_t need = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        char tmp[8];
        size_t len;
        switch (*p) {
        case '"': strcpy(tmp, "\\\""); len = 2; break;
        case '\\': strcpy(tmp, "\\\\"); len = 2; break;
        case '\n': strcpy(tmp, "\\n"); len = 2; break;
        case '\r': strcpy(tmp, "\\r"); len = 2; break;
        case '\t': strcpy(tmp, "\\t"); len = 2; break;
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof tmp, "\\u%04x", *p);
                len = 6;
            } else {
                tmp[0] = (char)*p; tmp[1] = '\0';
                len = 1;
            }
        }
        if (dst && need + len < cap) memcpy(dst + need, tmp, len);
        need += len;
    }
    if (dst && cap) dst[need < cap ? need : cap - 1] = '\0';
    return need;
}

typedef struct {
    char *buf;
    size_t cap, len;
} out_t;

static void put(out_t *o, const char *s) {
    size_t n = strlen(s);
    if (o->len + n < o->cap) memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void put_escaped(out_t *o, const char *s) {
    char *dst = (o->len < o->cap) ? o->buf + o->len : NULL;
    size_t room = (o->len < o->cap) ? o->cap - o->len : 0;
    o->len += json_escape(dst, room, s);
}

size_t json_build(char *buf, size_t cap, const char *host, const char *version,
                  const sample_t *s, size_t n) {
    out_t o = {buf, cap, 0};
    char num[64];
    put(&o, "{\"host\":\"");
    put_escaped(&o, host);
    put(&o, "\",\"agent_version\":\"");
    put_escaped(&o, version);
    put(&o, "\",\"samples\":[");
    for (size_t i = 0; i < n; i++) {
        if (i) put(&o, ",");   /* separators between items only: no trailing comma */
        snprintf(num, sizeof num, "{\"ts\":%.3f,\"metric\":\"", s[i].ts);
        put(&o, num);
        put_escaped(&o, s[i].metric);
        put(&o, "\",\"device\":\"");
        put_escaped(&o, s[i].device);
        snprintf(num, sizeof num, "\",\"value\":%.10g}", s[i].value);
        put(&o, num);
    }
    put(&o, "]}");
    if (cap) buf[o.len < cap ? o.len : cap - 1] = '\0';
    return o.len;
}
