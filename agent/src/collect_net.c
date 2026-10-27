#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <stdio.h>
#include <string.h>

#include "meridian.h"

#define MAX_IF 64
static struct {
    NET_LUID luid;
    ULONG64 rx, tx;
    int used;
} prev[MAX_IF];
static double prev_ts;

void collect_net(ring_t *r, double ts) {
    PMIB_IF_TABLE2 t = NULL;
    if (GetIfTable2(&t) != NO_ERROR) return;
    double dt = ts - prev_ts;
    for (ULONG i = 0; i < t->NumEntries; i++) {
        MIB_IF_ROW2 *row = &t->Table[i];
        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK || row->OperStatus != IfOperStatusUp) continue;
        int slot = -1, freeslot = -1;
        for (int k = 0; k < MAX_IF; k++) {
            if (prev[k].used && prev[k].luid.Value == row->InterfaceLuid.Value) slot = k;
            if (!prev[k].used && freeslot < 0) freeslot = k;
        }
        if (slot < 0) { /* first sighting: remember it and skip; a rate needs two samples */
            if (freeslot >= 0) {
                prev[freeslot].used = 1;
                prev[freeslot].luid = row->InterfaceLuid;
                prev[freeslot].rx = row->InOctets;
                prev[freeslot].tx = row->OutOctets;
            }
            continue;
        }
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, row->Alias, -1, name, sizeof name, NULL, NULL);
        double rx = rate_per_s(prev[slot].rx, row->InOctets, dt);
        double tx = rate_per_s(prev[slot].tx, row->OutOctets, dt);
        if (prev_ts > 0 && rx >= 0) ring_push(r, ts, "net.rx_bytes_per_s", name, rx);
        if (prev_ts > 0 && tx >= 0) ring_push(r, ts, "net.tx_bytes_per_s", name, tx);
        prev[slot].rx = row->InOctets;
        prev[slot].tx = row->OutOctets;
    }
    FreeMibTable(t);
    prev_ts = ts;
}
#else
#include <stdio.h>
#include <string.h>

#include "meridian.h"

#define MAX_IF 64
static struct {
    char name[64];
    uint64_t rx, tx;
} prev[MAX_IF];
static int nprev;
static double prev_ts;

void collect_net(ring_t *r, double ts) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[512];
    double dt = ts - prev_ts;
    int count = 0;
    static struct { char name[64]; uint64_t rx, tx; } cur[MAX_IF];
    while (fgets(line, sizeof line, f) && count < MAX_IF) {
        char *colon = strchr(line, ':');
        if (!colon) continue; /* header lines */
        *colon = '\0';
        char *name = line;
        while (*name == ' ') name++;
        if (!strcmp(name, "lo")) continue;
        unsigned long long rx, tx, skip;
        if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &rx, &skip, &skip,
                   &skip, &skip, &skip, &skip, &skip, &tx) != 9)
            continue;
        strncpy(cur[count].name, name, 63);
        cur[count].name[63] = '\0';
        cur[count].rx = rx;
        cur[count].tx = tx;
        count++;
    }
    fclose(f);
    for (int i = 0; i < count; i++) {
        for (int k = 0; k < nprev && prev_ts > 0; k++) {
            if (strcmp(prev[k].name, cur[i].name)) continue;
            double rrx = rate_per_s(prev[k].rx, cur[i].rx, dt);
            double rtx = rate_per_s(prev[k].tx, cur[i].tx, dt);
            if (rrx >= 0) ring_push(r, ts, "net.rx_bytes_per_s", cur[i].name, rrx);
            if (rtx >= 0) ring_push(r, ts, "net.tx_bytes_per_s", cur[i].name, rtx);
        }
    }
    nprev = count;
    for (int i = 0; i < count; i++) {
        strcpy(prev[i].name, cur[i].name);
        prev[i].rx = cur[i].rx;
        prev[i].tx = cur[i].tx;
    }
    prev_ts = ts;
}
#endif
