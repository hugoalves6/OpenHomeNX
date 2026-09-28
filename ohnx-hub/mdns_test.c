// mdns_test.c — test host del modulo mdns.c contro un publisher reale.
// Compila: gcc -o /tmp/mdns_test mdns_test.c -I../../include
// Richiede l'hub attivo (publish _ohnx-sync._tcp).
#include <stdio.h>
#include "mdns.h"
#include "../../source/mdns.c"

int main(void) {
    MdnsOhnxResult r[MDNS_MAX_RESULTS];
    int n = mdnsQueryOhnx(r, MDNS_MAX_RESULTS, 3000, NULL);
    printf("trovati: %d\n", n);
    for (int i = 0; i < n; i++)
        printf("[%d] inst='%s' host='%s' ip='%s' port=%d api='%s' fb=%d role='%s' uuid='%s' dev='%s' v=%d\n",
               i, r[i].instance, r[i].host, r[i].ip, r[i].port,
               r[i].api, r[i].fb, r[i].role, r[i].uuid, r[i].dev, r[i].version);
    return n > 0 ? 0 : 1;
}
