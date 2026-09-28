// mdns.c — query mDNS minimale, vedi mdns.h. Solo query PTR + parse
// PTR/SRV/TXT/A con compressione nomi. Niente malloc (buffer fissi).
#include "mdns.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifdef __SWITCH__
#include <switch.h> // armGetSystemTick per il budget (vedi mdnsNowMs)
#endif

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define MDNS_SERVICE "_ohnx-sync._tcp.local"

// Millisecondi monotonici (solo per il budget totale).
static long mdnsNowMs(void) {
#ifdef __SWITCH__
    extern uint64_t armGetSystemTick(void);
    extern uint64_t armGetSystemTickFreq(void);
    return (long)(armGetSystemTick() * 1000ULL / armGetSystemTickFreq());
#else
#include <sys/time.h>
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)(tv.tv_sec * 1000L + tv.tv_usec / 1000L);
#endif
}

// Decodifica un nome DNS (con puntatori 0xC0..) in out (sempre NUL-terminato).
// Ritorna l'offset DOPO il nome nel pacchetto, o -1 su errore.
static int mdnsDecodeName(const uint8_t* p, int len, int off,
                          char* out, int outLen) {
    int outPos = 0, jumped = 0, jumpBack = -1, steps = 0;
    while (off < len && steps++ < 32) {
        uint8_t b = p[off];
        if ((b & 0xC0) == 0xC0) { // puntatore
            if (off + 1 >= len) return -1;
            if (!jumped) jumpBack = off + 2;
            off = ((b & 0x3F) << 8) | p[off + 1];
            jumped = 1;
            continue;
        }
        if (b == 0) {
            off++;
            break;
        }
        if (off + 1 + b > (unsigned)len) return -1;
        if (outPos > 0 && outPos < outLen - 1) out[outPos++] = '.';
        for (int i = 0; i < b && outPos < outLen - 1; i++)
            out[outPos++] = (char)p[off + 1 + i];
        off += 1 + b;
    }
    if (outLen > 0) out[outPos < outLen ? outPos : outLen - 1] = '\0';
    return jumped ? jumpBack : off;
}

// Salta un nome (senza decodificarlo), ritorna l'offset dopo, o -1.
static int mdnsSkipName(const uint8_t* p, int len, int off) {
    int steps = 0;
    while (off < len && steps++ < 32) {
        uint8_t b = p[off];
        if ((b & 0xC0) == 0xC0) return off + 2;
        if (b == 0) return off + 1;
        off += 1 + b;
    }
    return -1;
}

static void mdnsParseTxt(MdnsOhnxResult* r, const uint8_t* d, int n) {
    int i = 0;
    while (i < n) {
        int l = d[i++];
        if (l <= 0 || i + l > n) break;
        const char* s = (const char*)(d + i);
        if (!strncmp(s, "v=", 2)) r->version = atoi(s + 2);
        else if (!strncmp(s, "api=", 4)) {
            strncpy(r->api, s + 4, (l - 4) < (int)sizeof(r->api) - 1 ? (l - 4) : (int)sizeof(r->api) - 1);
        } else if (!strncmp(s, "fb=", 3)) r->fb = atoi(s + 3);
        else if (!strncmp(s, "role=", 5)) {
            strncpy(r->role, s + 5, (l - 5) < (int)sizeof(r->role) - 1 ? (l - 5) : (int)sizeof(r->role) - 1);
        } else if (!strncmp(s, "uuid=", 5)) {
            strncpy(r->uuid, s + 5, (l - 5) < (int)sizeof(r->uuid) - 1 ? (l - 5) : (int)sizeof(r->uuid) - 1);
        }
        else if (!strncmp(s, "dev=", 4)) {
            strncpy(r->dev, s + 4, (l - 4) < (int)sizeof(r->dev) - 1 ? (l - 4) : (int)sizeof(r->dev) - 1);
        }
        i += l;
    }
}

int mdnsQueryOhnx(MdnsOhnxResult* out, int maxOut, int timeoutMs,
                  const bool* cancel) {
    if (!out || maxOut <= 0 || timeoutMs <= 0) return 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    // Query PTR _ohnx-sync._tcp.local, classe IN + bit QU (unicast reply).
    uint8_t q[256];
    int qp = 0;
    q[qp++] = 0; q[qp++] = 0; // ID 0 (mDNS)
    q[qp++] = 0; q[qp++] = 0; // flags: query standard
    q[qp++] = 0; q[qp++] = 1; // QDCOUNT
    q[qp++] = 0; q[qp++] = 0; // ANCOUNT
    q[qp++] = 0; q[qp++] = 0; // NSCOUNT
    q[qp++] = 0; q[qp++] = 0; // ARCOUNT
    const char* svc = MDNS_SERVICE;
    const char* dot = svc;
    while (*dot) { // label per label
        const char* next = strchr(dot, '.');
        int l = next ? (int)(next - dot) : (int)strlen(dot);
        if (l > 63 || qp + 1 + l + 4 > (int)sizeof(q)) { goto fail; }
        q[qp++] = (uint8_t)l;
        memcpy(q + qp, dot, l);
        qp += l;
        dot = next ? next + 1 : dot + l;
    }
    q[qp++] = 0;
    q[qp++] = 0; q[qp++] = 12;    // QTYPE PTR
    q[qp++] = 0x80; q[qp++] = 1;  // QCLASS IN + QU

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(MDNS_PORT);
    dst.sin_addr.s_addr = inet_addr(MDNS_GROUP);
    if (sendto(fd, (const char*)q, qp, 0,
               (struct sockaddr*)&dst, sizeof(dst)) < 0) goto fail;

    int found = 0;
    long deadline = mdnsNowMs() + timeoutMs;
    uint8_t buf[1500];
    while (found < maxOut) {
        if (cancel && *cancel) break;
        long left = deadline - mdnsNowMs();
        if (left <= 0) break;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (left > 250 ? 250 : left) * 1000L;
        if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
        int n = (int)recv(fd, (char*)buf, sizeof(buf), 0);
        if (n < 12) continue;
        int qd = (buf[4] << 8) | buf[5];
        // mDNS mette quasi tutto negli ADDITIONAL (SRV/TXT/A): vanno
        // parsati come le risposte, non ignorati.
        int total = ((buf[6] << 8) | buf[7]) + ((buf[8] << 8) | buf[9]) +
                    ((buf[10] << 8) | buf[11]);
        int off = 12, i;
        for (i = 0; i < qd; i++) { // salta le domande
            off = mdnsSkipName(buf, n, off);
            if (off < 0 || off + 4 > n) break;
            off += 4;
        }
        if (i < qd) continue;
        MdnsOhnxResult cur;
        memset(&cur, 0, sizeof(cur));
        strncpy(cur.api, "/ohnx", sizeof(cur.api) - 1);
        int haveInst = 0;
        for (i = 0; i < total; i++) {
            char name[160];
            int no = mdnsDecodeName(buf, n, off, name, sizeof(name));
            if (no < 0 || no + 10 > n) break;
            int type = (buf[no] << 8) | buf[no + 1];
            int rdlen = (buf[no + 8] << 8) | buf[no + 9];
            int rd = no + 10;
            if (rd + rdlen > n) break;
            if (type == 12) { // PTR -> istanza
                char inst[160];
                if (mdnsDecodeName(buf, n, rd, inst, sizeof(inst)) >= 0) {
                    // Tieni solo "Istanza._ohnx-sync._tcp.local"
                    char* cut = strstr(inst, "._ohnx-sync.");
                    if (cut) *cut = '\0';
                    strncpy(cur.instance, inst, sizeof(cur.instance) - 1);
                    haveInst = 1;
                }
            } else if (type == 33 && rdlen >= 6) { // SRV
                char tgt[160];
                cur.port = (buf[rd + 4] << 8) | buf[rd + 5];
                if (mdnsDecodeName(buf, n, rd + 6, tgt, sizeof(tgt)) >= 0)
                    strncpy(cur.host, tgt, sizeof(cur.host) - 1);
            } else if (type == 16) { // TXT
                mdnsParseTxt(&cur, buf + rd, rdlen);
            } else if (type == 1 && rdlen == 4) { // A
                snprintf(cur.ip, sizeof(cur.ip), "%u.%u.%u.%u",
                         buf[rd], buf[rd + 1], buf[rd + 2], buf[rd + 3]);
            }
            off = rd + rdlen;
        }
        if (haveInst && cur.port > 0) {
            int dup = 0, k;
            for (k = 0; k < found; k++)
                if (!strcmp(out[k].instance, cur.instance)) { dup = 1; break; }
            if (!dup) out[found++] = cur;
        }
    }
#ifndef _WIN32
    close(fd);
#else
    closesocket(fd);
#endif
    return found;
fail:
#ifndef _WIN32
    close(fd);
#else
    closesocket(fd);
#endif
    return -1;
}

