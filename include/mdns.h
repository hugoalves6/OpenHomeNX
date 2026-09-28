#pragma once
// mdns.h — query mDNS minimale per _ohnx-sync._tcp (solo client/query).
//
// Disegno: interroga in multicast (224.0.0.251:5353) ma chiede risposte in
// UNICAST (bit QU, RFC 6762 §5.4): non serve IP_ADD_MEMBERSHIP né bind sulla
// 5353, solo un socket UDP con timeout — il minimo che deve funzionare anche
// sui socket libnx. Niente publish qui (lo fanno hub/iOS/R36S).
// C portabile (BSD sockets), nessuna dipendenza. Compila sia per Switch
// (source/) che per host (test in devtools/).

#if !defined(__cplusplus) && !defined(__bool_true_false_are_defined)
#include <stdbool.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_MAX_RESULTS 8

typedef struct {
    char instance[128]; // es. "OHN X-Hub-Mac"
    char host[128];     // target SRV ("....local.")
    char ip[64];        // record A (puo' restare vuoto)
    int port;           // record SRV
    char api[32];       // da TXT (default "/ohnx")
    int fb;             // da TXT: 1 se lo shim filebrowser e' attivo
    char role[16];      // da TXT: "hub" | "device" | "" (sconosciuto)
    char uuid[40];      // da TXT: id persistente server ("" se assente)
    char dev[64];       // da TXT
    int version;        // da TXT ("v")
} MdnsOhnxResult;

// Cerca istanze _ohnx-sync._tcp.local per timeoutMs (budget totale).
// Ritorna il numero di risultati scritti in out (0 = nessuno), -1 su errore
// socket. Bloccante: chiamare da worker, mai dal thread UI. cancel
// (opzionale) = flag cooperativo controllato nel loop di ricezione.
int mdnsQueryOhnx(MdnsOhnxResult* out, int maxOut, int timeoutMs,
                  const bool* cancel);

#ifdef __cplusplus
}
#endif
