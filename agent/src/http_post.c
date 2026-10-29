#include <stdio.h>
#include <curl/curl.h>

#include "meridian.h"

int http_global_init(void) { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1; }
void http_global_cleanup(void) { curl_global_cleanup(); }

int backoff_seconds(int attempt) {
    if (attempt < 1) attempt = 1;
    if (attempt > 6) return 30;
    int s = 1 << (attempt - 1);
    return s > 30 ? 30 : s;
}

static size_t discard(void *p, size_t sz, size_t n, void *u) {
    (void)p; (void)u;
    return sz * n;
}

int http_post(const char *url, const char *api_key, const char *body, size_t len, long *status) {
    *status = 0;
    CURL *h = curl_easy_init();
    if (!h) return -1;
    char key_header[KEY_LEN + 16];
    snprintf(key_header, sizeof key_header, "X-API-Key: %s", api_key);
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, key_header);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, discard);
    CURLcode rc = curl_easy_perform(h);
    if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return (rc == CURLE_OK && *status >= 200 && *status < 300) ? 0 : -1;
}
