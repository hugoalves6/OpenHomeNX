#include "remote_sync.h"
#include "mdns.h"
#include "path_utils.h"
#include "sync_fingerprint.h"
#include "debug_log.h"
#include "settings_cfg.h"
#include "update_net.h"
#include "string_utils.h"

#define JSON_NOEXCEPTION
#include "json.hpp"

#include <curl/curl.h>
#include <switch.h>
#include <netinet/in.h>
#include <SDL2/SDL.h>

#include <atomic>
#include <cctype>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace {

size_t writeToStringRS(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string encodePathSegments(const std::string& path) {
    // Codifica ogni segmento tra '/' con curl_easy_escape (spazi -> %20, ecc.)
    // Mantiene '/' come separatore. Necessario per Filebrowser con nomi con spazi.
    CURL* curl = curl_easy_init();
    if (!curl) return path;
    std::string out;
    size_t start = 0;
    while (true) {
        size_t slash = path.find('/', start);
        std::string seg = (slash == std::string::npos) ? path.substr(start) : path.substr(start, slash - start);
        if (!seg.empty()) {
            char* esc = curl_easy_escape(curl, seg.c_str(), (int)seg.size());
            if (esc) { out += esc; curl_free(esc); }
            else out += seg;
        }
        if (slash == std::string::npos) break;
        out += '/';
        start = slash + 1;
        if (start >= path.size()) break; // trailing slash
    }
    curl_easy_cleanup(curl);
    return out;
}

// Come sopra ma scrive su un FILE* aperto dal chiamante (download su disco
// invece che in memoria -- i save DS arrivano a 512KB, inutile tenerli
// interi in una std::string quando vanno comunque scritti su file).
size_t writeToFileRS(char* ptr, size_t size, size_t nmemb, void* userdata) {
    FILE* fp = static_cast<FILE*>(userdata);
    return std::fwrite(ptr, size, nmemb, fp);
}

// Token cache: JWT di Filebrowser valido 2h (exp - iat = 7200s). Evita di rifare
// login ad ogni sync se siamo ancora connessi allo stesso host e il token non è scaduto.
static std::string g_cachedHost;
static std::string g_cachedToken;
static long long g_cachedExp = 0; // unix timestamp da exp del JWT

// Ultimo contatto di rete riuscito con un device remoto (login/download/
// upload, aggiornato da remoteSyncWatchdogCheck() stesso quando il ping
// va a buon fine): serve solo a remoteSyncWatchdogCheck() per non pingare
// mai un device con cui si e' appena parlato per altri motivi (scambio
// attivo in corso -- vedi il commento sopra la sua implementazione).
static double g_lastContactAt = 0.0;

long long jwtExpFromToken(const std::string& token) {
    size_t dot1 = token.find('.');
    if (dot1 == std::string::npos) return 0;
    size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return 0;
    std::string payloadB64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    // base64url -> base64
    for (char& ch : payloadB64) { if (ch == '-') ch = '+'; else if (ch == '_') ch = '/'; }
    while (payloadB64.size() % 4) payloadB64 += '=';
    // decodifica manuale base64 (evita dipendenze extra, usiamo solo exp)
    auto b64val = [](char ch) -> int {
        if ('A' <= ch && ch <= 'Z') return ch - 'A';
        if ('a' <= ch && ch <= 'z') return ch - 'a' + 26;
        if ('0' <= ch && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    std::string json;
    for (size_t i = 0; i + 3 < payloadB64.size(); i += 4) {
        int v0 = b64val(payloadB64[i]), v1 = b64val(payloadB64[i+1]), v2 = b64val(payloadB64[i+2]), v3 = b64val(payloadB64[i+3]);
        if (v0 < 0 || v1 < 0) break;
        json.push_back(char((v0 << 2) | (v1 >> 4)));
        if (v2 >= 0) json.push_back(char(((v1 & 0xF) << 4) | (v2 >> 2)));
        if (v3 >= 0) json.push_back(char(((v2 & 0x3) << 6) | v3));
    }
    auto p = json.find("\"exp\"");
    if (p == std::string::npos) return 0;
    p = json.find(':', p);
    if (p == std::string::npos) return 0;
    size_t s = p + 1; while (s < json.size() && (json[s] == ' ' || json[s] == '\t')) s++;
    long long exp = 0;
    while (s < json.size() && std::isdigit((unsigned char)json[s])) { exp = exp * 10 + (json[s] - '0'); s++; }
    return exp;
}

bool isCachedTokenValid(const std::string& host) {
    if (g_cachedHost != host || g_cachedToken.empty() || g_cachedExp == 0) return false;
    // margine 5 minuti prima della scadenza
    long long now = (long long)time(nullptr);
    return g_cachedExp > now + 300;
}

// Timeout brevi apposta: e' LAN locale (stesso router), non GitHub -- se il
// device non risponde in fretta meglio fallire subito che bloccare l'app
// per il timeout lungo pensato per l'updater su internet.
void applyLanOpts(CURL* c) {
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    // http:// in LAN, nessun certificato da verificare.
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
}

// Tempo di parete in secondi, stesso helper (stesso motivo: clock() su
// newlib/Switch e' tempo CPU, non avanza mentre si e' bloccati su I/O) di
// wallSeconds() in update_net.cpp -- non condiviso da li' per non creare
// una dipendenza incrociata fra i due file per una riga di codice.
double wallSecondsRS() {
    return (double)armTicksToNs(armGetSystemTick()) / 1.0e9;
}

// Cerca fra le sottocartelle di 'entries' quella il cui nome corrisponde
// (case-insensitive) a uno degli 'aliases' (gia' passati in minuscolo).
// Restituisce il nome ESATTO (maiuscole comprese) cosi' come sta sul
// device -- serve per costruire il path vero da passare a
// remoteSyncListPath, che e' case-sensitive lato server (Linux): il
// Filebrowser di ArkOS/JELOS/ROCKNIX puo' chiamare le cartelle "roms" o
// "Roms" secondo la build/tema, e un confronto esatto si romperebbe in
// silenzio proprio dove serve di piu'. Stringa vuota se nessuna
// sottocartella corrisponde.
std::string findDirCaseInsensitive(const std::vector<RemoteEntry>& entries,
                                    const std::vector<std::string>& aliases) {
    for (const auto& e : entries) {
        if (!e.isDir) continue;
        std::string lower = toLowerRS(e.name);
        for (const auto& alias : aliases) {
            if (lower == alias) return e.name;
        }
    }
    return std::string();
}

// Giorni dal 1970-01-01 per una data (proletticamente) gregoriana. Algoritmo
// standard "days_from_civil" (Howard Hinnant, dominio pubblico): non serve
// timegm/mktime, che dipenderebbero dal fuso locale del sistema (non
// affidabile su homebrew) -- i timestamp del Filebrowser sono gia' UTC o con
// offset esplicito, gestito a mano in parseIso8601ToUnix qui sotto.
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

// "YYYY-MM-DDTHH:MM:SS[.frazione][Z|+HH:MM|-HH:MM]" -> unix time UTC.
// 0 se il formato non e' quello atteso (mai un crash su un campo mancante).
long long parseIso8601ToUnix(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 6)
        return 0;
    long long secs = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400LL
                    + h * 3600LL + mi * 60LL + se;
    size_t tzPos = s.find_first_of("Z+-", 19); // dopo "YYYY-MM-DDTHH:MM:SS" (19 char)
    if (tzPos != std::string::npos && s[tzPos] != 'Z') {
        int sign = (s[tzPos] == '-') ? -1 : 1;
        int tzH = 0, tzM = 0;
        std::sscanf(s.c_str() + tzPos + 1, "%d:%d", &tzH, &tzM);
        secs -= sign * (tzH * 3600LL + tzM * 60LL); // riporta a UTC
    }
    return secs;
}

// Host "ip" o "ip:porta": l'hub puo' stare su una porta custom (niente
// sudo, niente conflitti con la 80) e la Switch gli parla il dialetto
// filebrowser su qualunque porta (l'hub lo serve ovunque). Tutto il resto
// del flusso usa la stringa cosi' com'e' ("http://"+host+...), solo qui e
// in login si risolve la porta effettiva.
static int splitHostPort(const std::string& host, std::string& outIp) {
    size_t colon = host.rfind(':');
    if (colon == std::string::npos) {
        outIp = host;
        return 80;
    }
    outIp = host.substr(0, colon);
    int p = std::atoi(host.c_str() + colon + 1);
    return (p > 0 && p < 65536) ? p : 80;
}

// Connessione TCP "vuota" (solo connect, nessuna richiesta HTTP) con timeout
// breve: serve solo a capire se qualcosa risponde sulla porta, prima di
// spendere una vera richiesta di login. Approccio ripristinato dopo il
// tentativo con fingerprint di contenuto (GET + "window.FileBrowser" nel
// body): su hardware reale quel tentativo si e' rivelato molto piu' lento
// (fino a un minuto+ per uno scan completo, anche con concorrenza limitata)
// e non ha mai trovato il device -- il motivo piu' probabile e' che ogni
// indirizzo mai visto sulla subnet richiede prima una risoluzione ARP, che
// sul Switch sembra bloccare piu' a lungo di quanto imposti CURLOPT_*
// TIMEOUT (il connect() vero e proprio parte solo dopo). Questo approccio
// (connessione TCP nuda, timeout breve, sequenziale) e' quello confermato
// funzionante e rapido su hardware vero.
bool probeTcpOpen(const std::string& host, int port, long timeoutMs) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    std::string url;
    if (host.find(':') == std::string::npos)
        url = "http://" + host + ":" + std::to_string(port);
    else
        url = "http://" + host; // porta gia' dentro ("ip:porta")
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return rc == CURLE_OK;
}

// Un solo GET su http://<host>/ per verificare il fingerprint di contenuto
// (vedi il commento sopra la scansione LAN in remote_sync.h): chiamata solo
// su host che hanno gia' superato probeTcpOpen, quindi con l'ARP gia'
// risolto -- timeout brevi, stesso ordine di grandezza di probeTcpOpen,
// senza bisogno del margine piu' ampio usato per parlare con un host gia'
// noto da tempo (vedi applyLanOpts).
bool fetchFingerprintBody(const std::string& host, std::string& outBody) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    outBody.clear();
    std::string url = "http://" + host + "/";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeToStringRS);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &outBody);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 500L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 800L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return rc == CURLE_OK;
}

} // namespace

// Ruolo ultimo contatto (vedi remoteSyncNoteRole in header): solo l'ultimo
// host conta, la UI lo legge subito dopo il login/scan riuscito.
static std::string g_roleHost;
static std::string g_role;

void remoteSyncNoteRole(const std::string& host, const std::string& role) {
    g_roleHost = host;
    g_role = role;
}

std::string remoteSyncRole(const std::string& host) {
    if (host.empty() || host != g_roleHost) return "";
    return g_role;
}

std::string remoteSyncRoleForIp(const std::string& host) {
    if (host.empty()) return "";
    std::string ip;
    splitHostPort(host, ip); // "ip:porta" -> ip
    MdnsOhnxResult found[MDNS_MAX_RESULTS];
    int n = mdnsQueryOhnx(found, MDNS_MAX_RESULTS, 1500, nullptr);
    for (int i = 0; i < n; i++) {
        if (ip == found[i].ip) {
            std::string role = found[i].role;
            if (role.empty() && found[i].fb) role = "hub"; // publisher _ohnx-sync con fb
            DebugLog::line("remote sync: ruolo mDNS %s -> '%s'", ip.c_str(), role.c_str());
            return role;
        }
    }
    return "";
}

bool remoteSyncLogin(const std::string& host, const std::string& user,
                      const std::string& pass, std::string& outToken, std::string& err) {
    outToken.clear();
    CURL* c = curl_easy_init();
    if (!c) { err = "curl_easy_init fallito"; return false; }

    nlohmann::json body;
    body["username"] = user;
    body["password"] = pass;
    body["recaptcha"] = ""; // filebrowser lo richiede anche se non usato
    std::string bodyStr = body.dump();

    std::string respBody;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    std::string url = "http://" + host + "/api/login";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)bodyStr.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeToStringRS);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &respBody);
    applyLanOpts(c);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        err = std::string("rete: ") + curl_easy_strerror(rc);
        return false;
    }
    if (http != 200) {
        err = "HTTP " + std::to_string(http);
        return false;
    }

    std::string trimmed = respBody;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' '))
        trimmed.pop_back();

    // Il token puo' arrivare come stringa JSON pura ("eyJ..."), come oggetto
    // {"token":"eyJ..."} (alcune fork/versioni), o come testo grezzo senza
    // virgolette (filebrowser classico: risponde text/plain col JWT).
    auto j = nlohmann::json::parse(trimmed, nullptr, false);
    if (!j.is_discarded()) {
        if (j.is_string()) outToken = j.get<std::string>();
        else if (j.is_object() && j.contains("token") && j["token"].is_string())
            outToken = j["token"].get<std::string>();
    }
    if (outToken.empty()) {
        outToken = trimmed;
        if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
            outToken = trimmed.substr(1, trimmed.size() - 2);
    }
    if (outToken.empty()) { err = "risposta di login vuota"; return false; }
    // Cache per riuso: evita login ad ogni sync se siamo ancora connessi allo stesso host
    g_cachedHost = host;
    g_cachedToken = outToken;
    g_cachedExp = jwtExpFromToken(outToken);
    g_lastContactAt = wallSecondsRS();
    DebugLog::line("remote sync: token cached for %s exp=%lld (valid %lld sec)", host.c_str(), g_cachedExp, g_cachedExp - (long long)time(nullptr));
    return true;
}

bool remoteSyncGetCachedToken(const std::string& host, std::string& outToken) {
    if (isCachedTokenValid(host)) { outToken = g_cachedToken; return true; }
    return false;
}

bool remoteSyncScanLan(const std::string& user, const std::string& pass,
                        std::string& outHost, std::string& outToken, std::string& err,
                        const RemoteSyncScanProgressFn& onProgress,
                        const bool* cancel) {
    outHost.clear();
    outToken.clear();
    if (!updateNetEnsureNifm()) { err = "nifm non disponibile"; return false; }

    struct in_addr addr;
    std::memset(&addr, 0, sizeof(addr));
    {
        // Stesso lock di updateNetEnsureNifm()/updateNetLinkStr() (vedi il
        // commento su updateNetNifmMutex() in update_net.h): nifm:u e' una
        // sessione IPC condivisa fra piu' thread ora -- mai una chiamata
        // nifm* diretta senza questo lock, altrimenti due thread possono
        // finire a dialogare in contemporanea sulla stessa sessione (hang,
        // non solo un dato letto storto).
        std::lock_guard<std::mutex> lock(updateNetNifmMutex());
        if (R_FAILED(nifmGetCurrentIpAddress((u32*)&addr.s_addr)) || addr.s_addr == 0) {
            err = "IP locale non disponibile (WiFi collegato?)";
            return false;
        }
    }
    // "network byte order" vuol dire che il primo ottetto e' il primo byte
    // in memoria, qualunque sia l'endianness della CPU: leggere per byte
    // (invece che con shift aritmetici su addr.s_addr come intero) da' il
    // risultato giusto su qualsiasi architettura, Switch (little-endian)
    // inclusa -- niente da indovinare sull'ordine dei byte.
    const uint8_t* oct = reinterpret_cast<const uint8_t*>(&addr.s_addr);
    char prefix[24];
    std::snprintf(prefix, sizeof(prefix), "%u.%u.%u.", oct[0], oct[1], oct[2]);
    int self = oct[3];

    // Trovare un host che accetta la connessione TCP non basta a sapere che
    // sia il device giusto: il login da solo non basta neanche lui (bug
    // reale confermato su hardware -- un router sulla stessa rete ha
    // "accettato" il login pur non essendo affatto il Filebrowser). Per
    // ogni host che risponde sulla porta 80 si verifica quindi PRIMA il
    // fingerprint di contenuto (fetchFingerprintBody, "window.FileBrowser"
    // nel body) e solo se combacia si prova il login vero -- continuando
    // finche' non se ne trova uno che soddisfa entrambi, o finche' la
    // sottorete non e' esaurita. Sequenziale, non concorrente: il
    // fingerprint qui costa poco perche' si fa solo sui pochi host che
    // hanno gia' superato probeTcpOpen (ARP gia' risolto), non su tutti i
    // 254 in parallelo come nel tentativo abbandonato in precedenza.
    bool foundOpenPort = false;

    // Scorciatoia: se un host era gia' salvato su questa stessa subnet
    // (stesso prefisso IP), provalo per primo fuori dal loop -- stesso
    // identico controllo a due passi (fingerprint, poi login) di ogni altro
    // host nel loop sotto, solo anticipato perche' e' il candidato piu'
    // probabile. Se fallisce (device davvero spostato d'indirizzo), si
    // ricade nel loop normale che lo esclude via "h == preferred" per non
    // riprovarlo due volte.
    int preferred = -1;
    int preferredPort = 80;
    {
        std::string savedHost = Settings::remoteSyncHost();
        size_t prefixLen = std::strlen(prefix);
        if (savedHost.size() > prefixLen && savedHost.compare(0, prefixLen, prefix) == 0) {
            int val = std::atoi(savedHost.c_str() + prefixLen);
            if (val >= 1 && val <= 254 && val != self) {
                preferred = val;
                // Host salvato "ip:porta" (hub nativo): riusa la porta.
                std::string rip;
                preferredPort = splitHostPort(savedHost, rip);
            }
        }
    }
    if (preferred != -1) {
        std::string host = std::string(prefix) + std::to_string(preferred);
        if (preferredPort != 80) {
            char hp[80];
            std::snprintf(hp, sizeof(hp), "%s:%d", host.c_str(), preferredPort);
            host = hp;
        }
        double probeT0 = wallSecondsRS();
        bool open = probeTcpOpen(host, preferredPort, Settings::remoteSyncProbeMs());
        if (open) {
            // Solo diagnostica (vedi il commento sopra "int preferred"):
            // quanto ci ha messo DAVVERO a rispondere, in ms, cosi' non
            // dobbiamo indovinare quanto margine c'e' sotto il timeout attuale.
            DebugLog::line("remote sync: scorciatoia host noto %s risponde in %.0fms",
                           host.c_str(), (wallSecondsRS() - probeT0) * 1000.0);
            foundOpenPort = true;
            std::string body;
            if (fetchFingerprintBody(host, body) && body.find("window.FileBrowser") != std::string::npos) {
                std::string loginErr, token;
                if (remoteSyncLogin(host, user, pass, token, loginErr)) {
                    outHost = host;
                    outToken = token;
                    return true;
                }
            }
        } else {
            DebugLog::line("remote sync: scorciatoia host noto %s non risponde entro %dms", host.c_str(), Settings::remoteSyncProbeMs());
        }
    }

    // Via veloce mDNS: se qualcuno pubblica _ohnx-sync._tcp (hub, app iOS,
    // R36S futuro), prova il login diretto senza spazzolare la /24. fb=1
    // -> shim su porta 80 (legacy); altrimenti porta pubblicata (nativo,
    // es. 8321: niente sudo, niente conflitti con la 80). L'hub serve il
    // dialetto filebrowser su OGNI porta, quindi host "ip:porta" passa
    // invariato in tutto il flusso. Fallback: sweep come prima.
    {
        if (onProgress) onProgress("Ricerca dispositivi in corso\n  mDNS…  [B per annullare]");
        MdnsOhnxResult mdns[MDNS_MAX_RESULTS];
        int nMdns = mdnsQueryOhnx(mdns, MDNS_MAX_RESULTS, 1500, cancel);
        DebugLog::line("remote sync: mDNS trovati %d", nMdns);
        for (int i = 0; i < nMdns; i++) {
            if (cancel && *cancel) { err = "annullato"; return false; }
            if (mdns[i].ip[0] == '\0' || mdns[i].port <= 0) continue;
            // role=device (peer iOS/futuri senza shim filebrowser): la via
            // veloce resta per chi serve il dialetto; i peer vanno di sweep.
            if (!mdns[i].fb && mdns[i].port == 80) continue;
            std::string host = mdns[i].ip;
            if (mdns[i].port != 80) {
                char hp[80];
                std::snprintf(hp, sizeof(hp), "%s:%d", mdns[i].ip, mdns[i].port);
                host = hp;
            }
            DebugLog::line("remote sync: mDNS '%s' -> %s (fb=%d)", mdns[i].instance,
                           host.c_str(), mdns[i].fb);
            if (!probeTcpOpen(host, mdns[i].port, Settings::remoteSyncProbeMs())) continue;
            foundOpenPort = true;
            std::string body;
            if (!fetchFingerprintBody(host, body) || body.find("window.FileBrowser") == std::string::npos) continue;
            std::string loginErr, token;
            if (remoteSyncLogin(host, user, pass, token, loginErr)) {
                // Publisher _ohnx-sync: e' il nostro hub anche senza role
                // (legacy); i role=device non arrivano qui (niente fb/porta).
                std::string role = mdns[i].role;
                if (role.empty()) role = "hub";
                remoteSyncNoteRole(host, role);
                outHost = host;
                outToken = token;
                return true;
            }
        }
    }

    double lastProgressEmit = 0.0;
    for (int h = 1; h <= 254; h++) {
        if (h == self || h == preferred) continue;
        // Annullamento: via flag cooperativo su worker (la coda eventi SDL
        // non e' thread-safe: solo il main puo' drenarla), via poll B nel
        // percorso sincrono originale (cancel == nullptr, main thread).
        if (cancel && *cancel) { err = "annullato"; return false; }
        if (!cancel) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                    err = "annullato dall'utente";
                    return false;
                }
                if (ev.type == SDL_QUIT) { err = "annullato"; return false; }
            }
        }
        if (onProgress) {
            // Throttle a 0.1s (stesso valore di dlXferInfoUI in
            // update_net.cpp): a questa velocita' di scan (fino a 254 host,
            // molti dei quali rispondono "connection refused" quasi
            // all'istante) chiamare onProgress -> showWorking() ad ogni
            // host aggiungerebbe un SDL_RenderPresent (bloccante su vsync)
            // per ognuno, rischiando di rendere la scansione PIU' lenta di
            // prima solo per il ridisegno, non per la rete.
            double now = wallSecondsRS();
            if (lastProgressEmit == 0.0 || now - lastProgressEmit >= 0.1) {
                lastProgressEmit = now;
                char line[96];
                int pct = (h * 100) / 254;
                std::snprintf(line, sizeof(line),
                              "Ricerca dispositivi in corso\n  %d%%  (host %d/254)  [B per annullare]", pct, h);
                onProgress(line);
            }
        }
        std::string host = std::string(prefix) + std::to_string(h);
        double probeT0 = wallSecondsRS();
        bool open = probeTcpOpen(host, 80, Settings::remoteSyncProbeMs());
        if (!open) continue;
        // Solo diagnostica, stesso motivo della scorciatoia sopra: tempo di
        // risposta reale in ms per un host che ha davvero la porta aperta,
        // per sapere quanto margine c'e' sotto il timeout attuale senza
        // indovinare.
        DebugLog::line("remote sync: %s risponde in %.0fms", host.c_str(), (wallSecondsRS() - probeT0) * 1000.0);
        foundOpenPort = true;
        std::string body;
        if (!fetchFingerprintBody(host, body) || body.find("window.FileBrowser") == std::string::npos) {
            DebugLog::line("remote sync: %s risponde sulla 80 ma non e' un Filebrowser (fingerprint assente)",
                           host.c_str());
            continue; // porta aperta ma non e' il Filebrowser (es. un router): mai provare il login qui
        }
        std::string loginErr, token;
        if (remoteSyncLogin(host, user, pass, token, loginErr)) {
            remoteSyncNoteRole(host, "device"); // sweep: legacy filebrowser
            outHost = host;
            outToken = token;
            return true;
        }
    }
    err = foundOpenPort
        ? "trovati device sulla rete locale ma nessuno e' un Filebrowser che accetta queste credenziali"
        : "nessun device trovato sulla rete locale";
    return false;
}

bool remoteSyncListPath(const std::string& host, const std::string& token,
                         const std::string& path, std::vector<RemoteEntry>& outEntries,
                         std::string& err) {
    outEntries.clear();
    CURL* c = curl_easy_init();
    if (!c) { err = "curl_easy_init fallito"; return false; }

    std::string p = path;
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    if (!p.empty() && p.back() != '/') p += '/';

    std::string respBody;
    struct curl_slist* hdrs = nullptr;
    std::string authHdr = "X-Auth: " + token;
    hdrs = curl_slist_append(hdrs, authHdr.c_str());

    std::string url = "http://" + host + "/api/resources/" + encodePathSegments(p);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeToStringRS);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &respBody);
    applyLanOpts(c);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) { err = std::string("rete: ") + curl_easy_strerror(rc); return false; }
    if (http == 404) { err = "cartella non trovata"; return false; }
    if (http != 200) { err = "HTTP " + std::to_string(http); return false; }

    auto j = nlohmann::json::parse(respBody, nullptr, false);
    if (j.is_discarded()) { err = "risposta non JSON"; return false; }

    auto readItem = [](const nlohmann::json& it, RemoteEntry& e) {
        e.name = it.value("name", std::string());
        e.isDir = it.value("isDir", false);
        e.size = it.value("size", (long long)0);
        e.sha = it.value("sha", std::string());
        std::string mod = it.value("modified", std::string());
        e.modifiedUnix = mod.empty() ? 0 : parseIso8601ToUnix(mod);
    };

    if (j.is_object() && j.contains("items") && j["items"].is_array()) {
        for (auto& it : j["items"]) {
            RemoteEntry e;
            readItem(it, e);
            outEntries.push_back(e);
        }
    } else if (j.is_array()) {
        for (auto& it : j) {
            RemoteEntry e;
            readItem(it, e);
            outEntries.push_back(e);
        }
    } else if (j.is_object()) {
        // Non e' una cartella (nessun "items"): file singolo, un solo entry.
        RemoteEntry e;
        readItem(j, e);
        outEntries.push_back(e);
    }
    return true;
}

bool remoteSyncListRoot(const std::string& host, const std::string& token,
                         int& outItemCount, std::string& err) {
    outItemCount = 0;
    std::vector<RemoteEntry> entries;
    if (!remoteSyncListPath(host, token, "", entries, err)) return false;
    outItemCount = (int)entries.size();
    return true;
}

const std::vector<RemoteSystemPaths>& remoteSyncKnownSystems() {
    static const std::vector<RemoteSystemPaths> table = {
        // GBA/GBC/GB: ArkOS tiene salvataggi e savestate nella stessa
        // cartella della ROM, di norma senza sottocartelle dedicate
        // (confermato dalla documentazione ArkOS). Il nome qui sotto e'
        // cercato case-insensitive dentro "roms" a runtime (vedi
        // remoteSyncBuildCandidates), non e' piu' un path fisso.
        {"gba", {""}},
        {"gbc", {""}},
        {"gb",  {""}},
        // NDS: non ancora confermato se ArkOS tenga i salvataggi accanto
        // alla ROM (come GBA) o in una sottocartella "backup/" (convenzione
        // di DraStic, l'emulatore NDS piu' comune su questi handheld, che
        // userebbe .dsv anziche' .sav) -- si provano entrambe finche' non
        // e' verificato su un R36S vero.
        {"nds", {"", "backup"}},
    };
    return table;
}

bool remoteSyncGuessGameFromName(const std::string& fileNameIn, GameType& outGuess) {
    // Solo dal nome del file, senza scaricare nulla: euristica di primo
    // filtro (falsi positivi possibili) -- l'identificazione precisa (via
    // header rom/save) resta un passo successivo, vedi remote_sync.h.
    std::string n = " ";
    for (char c : fileNameIn) {
        char cc = (c == '_' || c == '-' || c == '.') ? ' ' : c;
        n += (char)std::tolower((unsigned char)cc);
    }
    n += " ";

    auto has = [&](const char* kw) { return n.find(kw) != std::string::npos; };

    // Le varianti piu' specifiche vanno controllate prima di quelle che le
    // conterrebbero come sottostringa (es. "black2" prima di "black").
    if (has("heartgold") || has("heart gold") || has(" hg ")) { outGuess = GameType::HEARTGOLD; return true; }
    if (has("soulsilver") || has("soul silver") || has(" ss ")) { outGuess = GameType::SOULSILVER; return true; }
    if (has("firered") || has("fire red") || has("rosso fuoco") || has("frlg")) { outGuess = GameType::FR; return true; }
    if (has("leafgreen") || has("leaf green") || has("verde foglia")) { outGuess = GameType::LG; return true; }
    if (has("sapphire") || has("zaffiro")) { outGuess = GameType::SAPPHIRE; return true; }
    if (has("ruby") || has("rubino")) { outGuess = GameType::RUBY; return true; }
    if (has("emerald") || has("smeraldo")) { outGuess = GameType::EMERALD; return true; }
    if (has("platinum") || has("platino")) { outGuess = GameType::PLATINUM; return true; }
    if (has("diamond") || has("diamante")) { outGuess = GameType::DIAMOND; return true; }
    if (has("pearl") || has("perla")) { outGuess = GameType::PEARL; return true; }
    if (has("crystal") || has("cristallo")) { outGuess = GameType::CRYSTAL; return true; }
    if (has("gold") || has(" oro ")) { outGuess = GameType::GOLD; return true; }
    if (has("silver") || has("argento")) { outGuess = GameType::SILVER; return true; }
    if (has("black2") || has("black 2") || has("nero 2") || has("nera 2") || has("b2w2") || has("bw2")) { outGuess = GameType::BLACK2; return true; }
    if (has("white2") || has("white 2") || has("bianco 2") || has("bianca 2")) { outGuess = GameType::WHITE2; return true; }
    if (has("black") || has("nero") || has("nera")) { outGuess = GameType::BLACK; return true; }
    if (has("white") || has("bianco") || has("bianca")) { outGuess = GameType::WHITE; return true; }
    if (has("yellow") || has("giallo") || has("gialla")) { outGuess = GameType::YELLOW; return true; }
    if (has("red") || has("rosso") || has("rossa")) { outGuess = GameType::RED; return true; }
    if (has("blue") || has("blu")) { outGuess = GameType::BLUE; return true; }
    return false;
}

bool remoteSyncIsSaveFileName(const std::string& fileName) {
    std::string low = fileName;
    for (char& ch : low) ch = (char)std::tolower((unsigned char)ch);
    auto ends = [&](const char* suf) {
        size_t sl = std::strlen(suf);
        return low.size() >= sl && low.compare(low.size() - sl, sl, suf) == 0;
    };
    // .sav (tutti i sistemi), .srm (convenzione RetroArch), .dsv (DraStic,
    // l'emulatore NDS piu' comune su questi handheld).
    return ends(".sav") || ends(".srm") || ends(".dsv");
}

bool remoteSyncIsRomFileName(const std::string& fileName) {
    std::string low = fileName;
    for (char& ch : low) ch = (char)std::tolower((unsigned char)ch);
    auto ends = [&](const char* suf) {
        size_t sl = std::strlen(suf);
        return low.size() >= sl && low.compare(low.size() - sl, sl, suf) == 0;
    };
    // Solo vere ROM, non state/save/metadata (evita .state, .state.auto, .png, .xml ecc.)
    return ends(".gba") || ends(".gbc") || ends(".gb") || ends(".nds") || ends(".sfc") || ends(".smc");
}

// Stesso gruppo-save ai fini del match locale/remoto (estratto dal loop
// sotto per riuso nel secondo passaggio "solo locali"): FRLG condivide il
// save fra tutte le lingue dello stesso gioco (FR o LG).
bool sameSaveGroup(GameType a, GameType b) {
    if (a == b) return true;
    if (isFRLG(a) && isFRLG(b)) {
        auto isFR = [](GameType g) {
            return g == GameType::FR || g == GameType::FR_ES || g == GameType::FR_DE ||
                   g == GameType::FR_IT || g == GameType::FR_FR || g == GameType::FR_JA;
        };
        return isFR(a) == isFR(b);
    }
    return false;
}

// Cartella sistema remota dove INVIARE un save di un gioco che esiste solo
// in locale (l'hub crea le cartelle mancanti; il filebrowser classico no,
// ma l'hub e' il target primario). "" = non si sa dove metterlo, salta.
std::string sysFolderForUpload(GameType g) {
    if (isGen1File(g)) return "gb";
    if (isGen2File(g)) return "gbc";
    if (isFRLG(g)) return "gba";
    if (g == GameType::RUBY || g == GameType::SAPPHIRE || g == GameType::EMERALD) return "gba";
    if (isGen45File(g)) return "nds";
    return "";
}

std::vector<SyncCandidate> remoteSyncBuildCandidates(
    const std::string& host, const std::string& token,
    const std::vector<ImportedGame>& localGames, std::string& err) {
    std::vector<SyncCandidate> out;
    err.clear();

    // Scoperta dinamica di radice e cartella "roms" una sola volta,
    // condivisa da tutti i sistemi sotto (vedi il commento sopra la
    // dichiarazione in remote_sync.h): elenca sempre entrambe nei log,
    // cosi' un "nessuna cartella trovata" per un sistema ha subito i nomi
    // veri visti sul device, da confrontare con remoteSyncKnownSystems().
    std::vector<RemoteEntry> rootEntries;
    std::string rootErr;
    if (!remoteSyncListPath(host, token, "", rootEntries, rootErr)) {
        err = "radice del Filebrowser non raggiungibile: " + rootErr;
        DebugLog::line("remote sync: radice non raggiungibile: %s", rootErr.c_str());
        return out;
    }
    DebugLog::line("remote sync: radice: %zu elementi", rootEntries.size());
    for (const auto& e : rootEntries)
        DebugLog::line("remote sync:   /%s%s", e.name.c_str(), e.isDir ? "/" : "");

    std::string romsName = findDirCaseInsensitive(rootEntries, {"roms"});
    std::string romsPrefix;
    std::vector<RemoteEntry> romsEntries;
    if (!romsName.empty()) {
        romsPrefix = romsName + "/";
        std::string romsErr;
        if (remoteSyncListPath(host, token, romsPrefix, romsEntries, romsErr)) {
            DebugLog::line("remote sync: %s: %zu elementi", romsPrefix.c_str(), romsEntries.size());
            for (const auto& e : romsEntries)
                DebugLog::line("remote sync:   %s%s%s", romsPrefix.c_str(), e.name.c_str(), e.isDir ? "/" : "");
        } else {
            DebugLog::line("remote sync: %s non risponde (%s), provo la radice come cartella roms",
                           romsPrefix.c_str(), romsErr.c_str());
            romsPrefix.clear();
            romsEntries = rootEntries;
        }
    } else {
        DebugLog::line("remote sync: nessuna cartella \"roms\" in radice, provo la radice stessa come cartella roms");
        romsEntries = rootEntries;
    }

    for (const auto& sysPaths : remoteSyncKnownSystems()) {
        std::string sysName = findDirCaseInsensitive(romsEntries, {sysPaths.systemId});
        std::vector<RemoteEntry> entries;
        std::string dirUsed;
        if (!sysName.empty()) {
            for (const auto& subPath : sysPaths.subPaths) {
                std::string candidateDir = romsPrefix + sysName + "/" + (subPath.empty() ? "" : subPath + "/");
                std::string listErr;
                if (remoteSyncListPath(host, token, candidateDir, entries, listErr)) {
                    dirUsed = candidateDir;
                    DebugLog::line("remote sync: %s (%s): %zu elementi",
                                   candidateDir.c_str(), sysPaths.systemId, entries.size());
                    break; // prima sottocartella candidata che risponde
                }
                DebugLog::line("remote sync: %s (%s) non risponde: %s",
                               candidateDir.c_str(), sysPaths.systemId, listErr.c_str());
                // Fallback anti-doppio "roms/roms": se Filebrowser ha root già su /roms,
                // "roms/gba/" → 404 ma "gba/" funziona. Proviamo senza prefisso.
                if (!romsPrefix.empty()) {
                    std::string altDir = sysName + "/" + (subPath.empty() ? "" : subPath + "/");
                    if (remoteSyncListPath(host, token, altDir, entries, listErr)) {
                        dirUsed = altDir;
                        DebugLog::line("remote sync: %s (%s fallback senza roms): %zu elementi",
                                       altDir.c_str(), sysPaths.systemId, entries.size());
                        break;
                    }
                    DebugLog::line("remote sync: %s (%s fallback) non risponde: %s",
                                   altDir.c_str(), sysPaths.systemId, listErr.c_str());
                }
            }
        } else if (!romsPrefix.empty()) {
            // Nessun sysName in romsEntries ma magari è direttamente in root
            // (es. Filebrowser con root=/roms dove roms/gba esiste come gba in root ma
            // la ricerca iniziale ha trovato "roms" per errore). Prova root.
            for (const auto& subPath : sysPaths.subPaths) {
                std::string altDir = std::string(sysPaths.systemId) + "/" + (subPath.empty() ? "" : subPath + "/");
                std::string listErr;
                if (remoteSyncListPath(host, token, altDir, entries, listErr)) {
                    dirUsed = altDir;
                    DebugLog::line("remote sync: %s (%s fallback root): %zu elementi",
                                   altDir.c_str(), sysPaths.systemId, entries.size());
                    break;
                }
            }
        }
        if (dirUsed.empty()) {
            // Diagnostica per "nessun candidato" apparentemente senza
            // motivo: gli elenchi di radice e "roms" sopra mostrano gia' i
            // nomi veri delle cartelle sul device -- se quello del sistema
            // non compare fra questi log, il nome vero non e' fra gli
            // alias provati (vedi remoteSyncKnownSystems in
            // remote_sync.h), va aggiunto li'.
            DebugLog::line("remote sync: nessuna cartella trovata per %s sotto %s",
                           sysPaths.systemId, romsPrefix.empty() ? "radice" : romsPrefix.c_str());
            continue; // nessuna cartella nota per questo sistema ha risposto
        }

        // Prima passata sui file della cartella: per ogni GameType
        // riconosciuto dal nome, tiene il primo save e la prima ROM trovati.
        struct Found {
            bool hasSave = false;
            std::string savePath;
            long long saveModified = 0;
            std::string saveSha;
            bool hasRom = false;
            std::string romBaseName;
        };
        std::unordered_map<int, Found> byType;

        for (const auto& e : entries) {
            if (e.isDir)
                continue;
            GameType g;
            if (!remoteSyncGuessGameFromName(e.name, g)) {
                // File visto ma scartato: nessuna parola chiave nota nel nome
                // (vedi remoteSyncGuessGameFromName) -- se e' proprio il save
                // che l'utente si aspetta di trovare, il problema e' qui, non
                // nella cartella o nell'estensione.
                DebugLog::line("remote sync: %s%s ignorato (nome non riconosciuto)",
                               dirUsed.c_str(), e.name.c_str());
                continue;
            }
            Found& f = byType[static_cast<int>(g)];
            if (remoteSyncIsSaveFileName(e.name)) {
                if (!f.hasSave) {
                    f.hasSave = true;
                    f.savePath = dirUsed + e.name;
                    f.saveModified = e.modifiedUnix;
                    f.saveSha = e.sha;
                }
            } else if (!f.hasRom && remoteSyncIsRomFileName(e.name)) {
                f.hasRom = true;
                std::string base = e.name;
                size_t dot = base.find_last_of('.');
                if (dot != std::string::npos)
                    base = base.substr(0, dot);
                f.romBaseName = base;
            }
        }

        for (const auto& kv : byType) {
            const Found& f = kv.second;
            SyncCandidate c;
            c.type = static_cast<GameType>(kv.first);
            c.remoteDir = dirUsed;
            c.hasRemoteSave = f.hasSave;
            c.remoteSavePath = f.savePath;
            c.remoteSaveModifiedUnix = f.saveModified;
            c.remoteSaveSha = f.saveSha;
            c.hasRemoteRom = f.hasRom;
            c.remoteRomBaseName = f.romBaseName;
            // Nativi (marker save:/, o gia' risolti in tmp dall'autoboot che
            // consuma il marker sul main thread prima di arrivare qui - vedi
            // ui.cpp) prima dei file: su Switch il giocato e' il nativo, lo
            // sdmc e' la copia di trasferimento. Prima il match prendeva il
            // primo in lista e il nativo restava oscurato per sempre (FRLG:
            // fp sul .sav mentre si giocava il nativo). Due passaggi, stesso
            // gruppo. sourceTag=="Switch" (impostato da
            // appendNativeSwitchSaveCandidates) e' il secondo segnale perche'
            // l'autoboot risolve il marker "save:/..." in un path reale PRIMA
            // di chiamare questa funzione: il prefisso da solo non basta piu'
            // a riconoscere il nativo una volta risolto (bug reale,
            // 2026-09-28: l'autoboot vedeva il nativo ma lo trattava come un
            // file qualunque, vinceva sempre il primo della lista = la ROM).
            for (int pass = 0; pass < 2 && !c.hasLocal; pass++) {
                for (const auto& lg : localGames) {
                    bool isMarker = lg.filePath.rfind("save:/", 0) == 0 || lg.sourceTag == "Switch";
                    if ((pass == 0) != isMarker) continue;
                    if (sameSaveGroup(lg.type, c.type)) {
                        c.hasLocal = true;
                        c.localPath = lg.filePath;
                        c.localIsNativeSwitch = isMarker;
                        // Salva anche il tipo effettivo locale per distinzione SW/ROM nel picker
                        c.type = lg.type; // usa il tipo locale per coerenza (es. FR_IT invece di FR generico)
                        break;
                    }
                }
            }
            if (!c.hasLocal && !c.hasRemoteSave)
                continue; // solo la ROM, nessun save da nessuna parte: niente da proporre
            out.push_back(c);
        }
    }

    // Secondo passaggio: giochi SOLO locali (nessuna cartella remota per il
    // loro sistema, es. hub vuoto appena creato). Senza questo, verso un
    // server vergine non nascerebbe mai nessun candidato e "Invia" non
    // esisterebbe -- il primo upload sarebbe impossibile.
    for (const auto& lg : localGames) {
        bool covered = false;
        for (const auto& c : out) {
            if (sameSaveGroup(lg.type, c.type)) { covered = true; break; }
        }
        if (covered) continue;
        std::string sys = sysFolderForUpload(lg.type);
        if (sys.empty()) continue;
        SyncCandidate c;
        c.type = lg.type;
        c.hasLocal = true;
        c.localPath = lg.filePath;
        c.remoteDir = romsPrefix + sys + "/";
        DebugLog::line("remote sync: candidato solo-locale %d -> %s",
                       static_cast<int>(lg.type), c.remoteDir.c_str());
        out.push_back(c);
    }
    DebugLog::line("remote sync: %zu candidati costruiti su %s", out.size(), host.c_str());
    return out;
}

// GET grezzo di un file remoto in un path locale -- ESATTAMENTE la stessa
// logica che remoteSyncDownload() aveva prima di questo commit, estratta
// cosi' com'era per essere richiamabile due volte (download reale +
// riscarico di verifica) senza duplicare la parte curl. Nessun controllo
// di checksum qui: lo fa il chiamante.
static bool downloadRawToFile(const std::string& host, const std::string& token,
                               const std::string& remotePath, const std::string& outLocalPath,
                               std::string& err) {
    std::string p = remotePath;
    while (!p.empty() && p.front() == '/')
        p.erase(p.begin());

    FILE* fp = std::fopen(outLocalPath.c_str(), "wb");
    if (!fp) { err = "impossibile creare il file locale"; return false; }

    CURL* c = curl_easy_init();
    if (!c) { std::fclose(fp); err = "curl_easy_init fallito"; return false; }

    struct curl_slist* hdrs = nullptr;
    std::string authHdr = "X-Auth: " + token;
    hdrs = curl_slist_append(hdrs, authHdr.c_str());

    std::string url = "http://" + host + "/api/raw/" + encodePathSegments(p);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeToFileRS);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, fp);
    applyLanOpts(c);
    // Payload (save/ROM fino a MB): su uplink lenti (dongle R36S) 8s non
    // bastano nemmeno per 128KB. Login/liste restano a 8s.
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    std::fclose(fp);

    if (rc != CURLE_OK) {
        std::remove(outLocalPath.c_str());
        err = std::string("rete: ") + curl_easy_strerror(rc);
        return false;
    }
    if (http != 200) {
        std::remove(outLocalPath.c_str());
        err = "HTTP " + std::to_string(http);
        return false;
    }
    return true;
}

bool remoteSyncDownload(const std::string& host, const std::string& token,
                        const std::string& remotePath, const std::string& outLocalPath,
                        std::string& err) {
    if (!downloadRawToFile(host, token, remotePath, outLocalPath, err))
        return false;

    // Checksum sempre attivo (richiesto esplicitamente, vedi il commento
    // esteso sopra downloadRawToFile): nessun endpoint di checksum lato
    // server su Filebrowser (verificato prima di scrivere questo codice),
    // quindi la verifica reale e' un secondo GET indipendente dello stesso
    // file e il confronto degli hash locali -- se non coincidono, il
    // trasferimento e' stato tagliato/corrotto in rete.
    std::string hash1 = sha256HexFile(outLocalPath);
    if (hash1.empty()) {
        std::remove(outLocalPath.c_str());
        err = "impossibile calcolare checksum del file ricevuto";
        return false;
    }
    std::string verifyPath = outLocalPath + ".vrfy";
    std::string verifyErr;
    bool verifyOk = downloadRawToFile(host, token, remotePath, verifyPath, verifyErr);
    std::string hash2 = verifyOk ? sha256HexFile(verifyPath) : std::string();
    std::remove(verifyPath.c_str());
    if (!verifyOk || hash2.empty() || hash1 != hash2) {
        std::remove(outLocalPath.c_str());
        err = "verifica checksum fallita dopo la ricezione (doppio controllo discorde)";
        return false;
    }
    g_lastContactAt = wallSecondsRS();
    return true;
}

bool remoteSyncUpload(const std::string& host, const std::string& token,
                       const std::string& localPath, const std::string& remotePath,
                       std::string& err) {
    std::ifstream f(localPath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) { err = "file locale non leggibile"; return false; }
    std::streamsize size = f.tellg();
    f.seekg(0);
    DebugLog::line("remote sync: upload leggo %s (%lld byte)", localPath.c_str(), (long long)size);
    if (size <= 0 || size > 256 * 1024 * 1024) { err = "dimensione file non valida"; return false; }
    std::vector<char> buf(static_cast<size_t>(size));
    if (!f.read(buf.data(), size)) { err = "lettura file locale fallita"; return false; }
    DebugLog::line("remote sync: upload letto, calcolo sha...");

    // Checksum sempre attivo (richiesto esplicitamente): calcolato PRIMA
    // dell'invio sul file locale, confrontato dopo l'upload con lo stesso
    // file riscaricato dal remoto (vedi sotto) -- verifica end-to-end reale,
    // non solo lo stato HTTP 200/201.
    std::string localHash = sha256HexFile(localPath);
    if (localHash.empty()) { err = "impossibile calcolare checksum locale"; return false; }
    DebugLog::line("remote sync: upload sha ok, POST...");

    std::string p = remotePath;
    while (!p.empty() && p.front() == '/')
        p.erase(p.begin());

    CURL* c = curl_easy_init();
    if (!c) { err = "curl_easy_init fallito"; return false; }

    struct curl_slist* hdrs = nullptr;
    std::string authHdr = "X-Auth: " + token;
    hdrs = curl_slist_append(hdrs, authHdr.c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");

    std::string respBody;
    // ?override=true: senza, Filebrowser rifiuta di sovrascrivere un file
    // gia' esistente (serve sia per "Invia" su un save che c'e' gia', sia
    // per la creazione ex-novo, dove non fa differenza).
    std::string url = "http://" + host + "/api/resources/" + encodePathSegments(p) + "?override=true";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, buf.empty() ? "" : buf.data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)buf.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeToStringRS);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &respBody);
    applyLanOpts(c);
    // Come download: l'upload di 128KB su uplink lenti supera gli 8s.
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    DebugLog::line("remote sync: upload POST finito rc=%d http=%ld", (int)rc, http);
    if (rc != CURLE_OK) { err = std::string("rete: ") + curl_easy_strerror(rc); return false; }
    if (http != 200 && http != 201) { err = "HTTP " + std::to_string(http); return false; }

    // Verifica: riscarica lo stesso file appena inviato (GET indipendente,
    // stessa funzione grezza usata da remoteSyncDownload, non la versione
    // con doppio controllo -- eviterebbe un terzo GET inutile) e confronta
    // l'hash con quello calcolato sul locale prima dell'invio.
    DebugLog::line("remote sync: upload verifico riscaricando...");
    std::string verifyPath = localPath + ".vrfy";
    std::string verifyErr;
    bool verifyOk = downloadRawToFile(host, token, remotePath, verifyPath, verifyErr);
    DebugLog::line("remote sync: upload verifica download %s", verifyOk ? "ok" : verifyErr.c_str());
    std::string remoteHash = verifyOk ? sha256HexFile(verifyPath) : std::string();
    std::remove(verifyPath.c_str());
    if (!verifyOk || remoteHash.empty() || remoteHash != localHash) {
        err = "verifica checksum fallita dopo l'invio";
        return false;
    }

    g_lastContactAt = wallSecondsRS();
    return true;
}
// Controllo periodico "e' ancora vivo?" per il device gia' trovato (dal
// worker in background o dallo scan manuale): senza questo, spegnere/
// riaccendere il device remoto (o solo spostarlo di rete) lo lascia
// "trovato" per sempre agli occhi dell'app, perche' remoteSyncWorkerStart()
// tenta la scoperta una sola volta per boot (vedi il commento sopra la sua
// implementazione). Da richiamare una volta per frame dal loop principale
// (ui.cpp) quando UI::remoteDeviceAvailable_ e' true: quasi sempre non
// bloccante (un solo confronto fra double), tranne quando scatta davvero il
// controllo -- al massimo una volta ogni WATCHDOG_INTERVAL_SEC, e MAI se e'
// arrivato un contatto vero (login/upload/download riusciti, vedi
// g_lastContactAt) piu' di recente: chi sta scambiando attivamente non
// subisce mai questo controllo in mezzo a un'operazione, esattamente come
// richiesto ("se sto attivamente scambiando non ha senso"). Quando scatta
// e' l'identico controllo in due passi dello scan (probeTcpOpen + fingerprint)
// contro l'host gia' noto: un singolo hitch di frame ogni un paio di minuti
// al massimo, mai piu' spesso. true = nessun problema rilevato (o troppo
// presto per un altro controllo); false = il device non risponde piu' -- il
// chiamante deve azzerare remoteDeviceAvailable_.
bool remoteSyncWatchdogCheck(const std::string& host) {
    constexpr double WATCHDOG_INTERVAL_SEC = 120.0;
    static double lastCheckAt = 0.0;
    double now = wallSecondsRS();
    if (lastCheckAt == 0.0) lastCheckAt = now; // primo giro: arma solo il timer
    if (now - g_lastContactAt < WATCHDOG_INTERVAL_SEC) return true; // contatto recente, niente ping
    if (now - lastCheckAt < WATCHDOG_INTERVAL_SEC) return true; // troppo presto dall'ultimo ping
    lastCheckAt = now;
    std::string body;
    bool alive = probeTcpOpen(host, 80, 250) &&
                 fetchFingerprintBody(host, body) &&
                 body.find("window.FileBrowser") != std::string::npos;
    if (alive) {
        g_lastContactAt = now;
        DebugLog::line("remote sync: watchdog ok, %s ancora vivo", host.c_str());
    } else {
        DebugLog::line("remote sync: watchdog - %s non risponde piu'", host.c_str());
    }
    return alive;
}

// ============================================================================
// Scoperta automatica in background (thread separato, un tentativo per
// boot) -- stesso pattern di autoupdate.cpp: gli input (host/user/pass) sono
// preparati sul main thread PRIMA di avviare il thread, poi la comunicazione
// e' solo via atomici (mai std::string o Settings condivisi mentre il
// worker e' vivo). Il worker non chiama mai DebugLog::line() dal proprio
// thread, per lo stesso motivo per cui non lo fa il worker di autoupdate.cpp
// (non e' garantito sicuro se il main thread lo usasse nello stesso istante):
// l'esito va loggato dal chiamante sul main thread dopo remoteSyncWorkerPoll.
//
// Limite noto non risolto in questo giro: se l'utente apre a mano il flusso
// DevSync/RemoteBox (che fa il suo stesso login+scansione) proprio mentre
// questo worker e' ancora in corso, i due possono sovrapporsi e superare
// insieme il limite di sessioni bsd condivise con curl (vedi il commento in
// cima a questo file) -- finestra stretta (pochi secondi dopo il boot) e
// autolimitante (le richieste in eccesso falliscono e vengono ritentate),
// non e' stato aggiunto un mutex dedicato solo per questo.
namespace {

constexpr size_t kWorkerStackSize = 64 * 1024; // stessa taglia del worker autoupdate
Thread s_worker;
std::atomic<bool> s_workerStarted{false};
std::atomic<bool> s_workerRunning{false}; // thread vivo: solo allora join ha senso
std::atomic<int> s_workerState{0}; // 0 idle/consumato, 1 in corso, 2 trovato, 3 non trovato
std::atomic<bool> s_workerStop{false}; // uscita in corso: il join non deve aspettare i timeout
char s_workerHost[64] = {0};
char s_workerToken[512] = {0}; // JWT tipicamente ~150-300 char, margine ampio
std::string s_workerSavedHost, s_workerUser, s_workerPass;

void remoteSyncWorkerMain(void*) {
    // Aspetta il link vero prima di provare, stesso idioma del worker
    // autoupdate: socketInitializeDefault riesce molto prima che il WiFi si
    // associ davvero, e un tentativo troppo presto fallirebbe in silenzio.
    // Timeout ~46s, poi si prova comunque (login/scansione falliranno da
    // soli se la rete non c'e' per davvero).
    for (int i = 0; i < 23 && !s_workerStop.load(std::memory_order_acquire); i++) {
        if (std::strcmp(updateNetLinkStr(), "OFF") != 0)
            break;
        svcSleepThread(2000000000ULL);
    }
    if (s_workerStop.load(std::memory_order_acquire)) return; // uscita in corso: niente login a meta' teardown

    std::string host = s_workerSavedHost;
    std::string token, err;
    // SOLO un login diretto sull'host gia' salvato -- MAI la scansione
    // completa della subnet (remoteSyncScanLan, fino a ~100s con curl-multi
    // su 253 host) come fallback automatico. Quella va benissimo per
    // un'azione manuale che l'utente ha esplicitamente richiesto e sa di
    // dover aspettare, ma qui girerebbe in un thread in background a OGNI
    // singolo boot in cui l'host salvato non risponde, competendo per tutto
    // quel tempo con QUALSIASI altra richiesta di rete dell'app (check
    // aggiornamenti, download della build, ecc.) sulle stesse ~3 sessioni
    // bsd condivise -- causa esatta dell'"aggiornamento lentissimo"
    // segnalato subito dopo l'introduzione di questo worker. Se l'host e'
    // cambiato IP, resta il flusso manuale (RemoteBox/DevSync) a ritrovarlo
    // con la scansione vera, in un momento che l'utente ha scelto lui.
    bool ok = remoteSyncLogin(host, s_workerUser, s_workerPass, token, err);

    if (ok) {
        std::snprintf(s_workerHost, sizeof(s_workerHost), "%s", host.c_str());
        std::snprintf(s_workerToken, sizeof(s_workerToken), "%s", token.c_str());
        s_workerState.store(2, std::memory_order_release);
    } else {
        s_workerState.store(3, std::memory_order_release);
    }
}

} // namespace

void remoteSyncWorkerStart() {
    bool expected = false;
    if (!s_workerStarted.compare_exchange_strong(expected, true))
        return; // un solo tentativo per boot

    std::string host = Settings::remoteSyncHost();
    if (host.empty())
        return; // remote sync mai configurato a mano: niente da ritrovare in automatico

    s_workerSavedHost = host;
    s_workerUser = Settings::syncLoginUser();
    s_workerPass = Settings::syncLoginPass();
    s_workerState.store(1, std::memory_order_release);

    Result rcCreate = threadCreate(&s_worker, remoteSyncWorkerMain, nullptr, nullptr,
                                   kWorkerStackSize, 0x2C, -2);
    Result rcStart = R_SUCCEEDED(rcCreate) ? threadStart(&s_worker) : rcCreate;
    if (R_SUCCEEDED(rcCreate) && R_SUCCEEDED(rcStart))
        s_workerRunning.store(true, std::memory_order_release);
    else
        s_workerState.store(3, std::memory_order_release); // mai appeso su start fallita
}

RemoteSyncWorkerResult remoteSyncWorkerPoll(std::string& outHost, std::string& outToken) {
    int state = s_workerState.load(std::memory_order_acquire);
    if (state == 2) {
        s_workerState.store(0, std::memory_order_release); // consumato una sola volta
        outHost = s_workerHost;
        outToken = s_workerToken;
        return RemoteSyncWorkerResult::Found;
    }
    if (state == 3) {
        s_workerState.store(0, std::memory_order_release); // consumato una sola volta
        return RemoteSyncWorkerResult::NotFound;
    }
    return RemoteSyncWorkerResult::Pending;
}

// ---- Auto-sync boot: solo upload one-sided verso il server ----
namespace {
Thread s_autoThread;
std::atomic<bool> s_autoStarted{false};
std::atomic<bool> s_autoRunning{false};
std::atomic<int> s_autoState{0}; // 0 idle, 1 in corso, 2 presenza ok, 3 niente/fallito
char s_autoHost[64] = {0};
char s_autoToken[512] = {0};
std::atomic<int> s_autoFailKind{0}; // vedi remoteSyncAutoBootFailPoll
std::string s_autoFailMsg;
std::string s_autoInstance, s_autoUser, s_autoPass;
std::vector<ImportedGame> s_autoLocal;
std::string s_autoFileDir, s_autoImportDir, s_autoTmpDir;
std::atomic<int> s_autoUp{0};
std::atomic<int> s_autoDown{0};
std::atomic<bool> s_autoProblem{false};
std::string s_autoProblemMsg;
// Scritte da questo worker, lette dal main SOLO dopo il join (s_autoState==2):
// stesso pattern di s_autoUp/s_autoProblemMsg sopra, nessun mutex necessario.
std::vector<NativeSyncWrite> s_autoPendingNative;

bool autoFileReadable(const std::string& p) {
    if (p.empty()) return false;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

void remoteSyncAutoBootMain(void*) {
    for (int i = 0; i < 23; i++) {
        if (std::strcmp(updateNetLinkStr(), "OFF") != 0)
            break;
        svcSleepThread(2000000000ULL);
    }
    // Istanza associata -> IP corrente (stabile oltre il DHCP).
    // Doppio giro: la prima query multicast a freddo va persa spesso
    // (join/ARP, visto su HW: kind=1 a hub acceso). Costa 2.5s solo se il
    // primo giro e' vuoto.
    MdnsOhnxResult found[MDNS_MAX_RESULTS];
    int n = mdnsQueryOhnx(found, MDNS_MAX_RESULTS, 2500, nullptr);
    std::string ip, role;
    int port = 80;
    for (int attempt = 0; attempt < 2 && ip.empty(); attempt++) {
        if (attempt > 0) {
            DebugLog::line("remote sync: autoboot, retry mDNS");
            n = mdnsQueryOhnx(found, MDNS_MAX_RESULTS, 2500, nullptr);
        }
        for (int i = 0; i < n; i++) {
            if (found[i].instance == s_autoInstance) {
                ip = found[i].ip;
                role = found[i].role;
                port = found[i].port;
                break;
            }
        }
    }
    if (ip.empty() || port <= 0) {
        DebugLog::line("remote sync: autoboot, istanza '%s' non trovata via mDNS",
                       s_autoInstance.c_str());
        s_autoFailKind.store(1, std::memory_order_release);
        s_autoFailMsg = s_autoInstance;
        s_autoState.store(3, std::memory_order_release);
        return;
    }
    // Host "ip:porta" se custom: tutto il flusso sotto passa la stringa
    // invariata (l'hub serve il dialetto su ogni porta).
    std::string host = rsHostPort(ip, port);
    std::string token, err;
    if (!remoteSyncLogin(host, s_autoUser, s_autoPass, token, err)) {
        DebugLog::line("remote sync: autoboot, login fallito su %s: %s", host.c_str(), err.c_str());
        // 401 = credenziali (filebrowser), "rete:" = irraggiungibile.
        if (err.find("401") != std::string::npos) s_autoFailKind.store(3, std::memory_order_release);
        else s_autoFailKind.store(2, std::memory_order_release);
        s_autoFailMsg = err;
        s_autoState.store(3, std::memory_order_release);
        return;
    }
    if (role.empty()) role = "hub"; // istanza associata = nostro hub
    remoteSyncNoteRole(host, role);
    // Presenza (state 2) pubblicata SOLO a transfer finiti: se la UI
    // consuma al login legge problem=false e la warn non scatta mai (bug
    // reale: race login vs transfer). La presenza ritarda di secondi, ok.
    std::snprintf(s_autoHost, sizeof(s_autoHost), "%s", host.c_str());
    std::snprintf(s_autoToken, sizeof(s_autoToken), "%s", token.c_str());
    DebugLog::line("remote sync: autoboot, login ok su %s (%s)", host.c_str(), role.c_str());
    if (role == "device") {
        // Legacy associato per sbaglio? Mai upload automatici verso un
        // device: solo presenza, i trasferimenti restano manuali.
        DebugLog::line("remote sync: autoboot, ruolo device: solo presenza");
        s_autoState.store(2, std::memory_order_release);
        return;
    }
    // Two-way automatico sui fingerprint (stesso giocatore): upload dove
    // il locale e' avanti o manca, download dove e' avanti o manca il
    // remoto, skip se uguali. Giocatori diversi / illeggibili / transfer
    // falliti -> problem (warn in barra, mai decisioni cieche).
    auto baseOf = [](const std::string& p) -> std::string {
        size_t slash = p.find_last_of('/');
        std::string f = (slash == std::string::npos) ? p : p.substr(slash + 1);
        size_t dot = f.find_last_of('.');
        return (dot == std::string::npos) ? f : f.substr(0, dot);
    };
    auto remoteExt = [&](GameType t) -> std::string {
        if (role != "device") return ""; // hub: tiene l'estensione locale
        if (isGen4File(t) || isGen5File(t)) return ".dsv";
        return ".srm";
    };
    std::string cerr;
    std::vector<SyncCandidate> cands = remoteSyncBuildCandidates(host, token, s_autoLocal, cerr);
    int up = 0, down = 0;
    bool problem = false;
    std::string problemMsg;
    std::string tmpBase = s_autoTmpDir + "auto_";
    for (const auto& c : cands) {
        const char* tag = gameInfo(c.type).gameTag;
        if (!c.hasLocal && !c.hasRemoteSave) continue;
        std::string localPath = c.localPath;
        if (c.hasLocal && !autoFileReadable(localPath)) {
            DebugLog::line("remote sync: autoboot, %s locale non leggibile, salto", tag);
            continue;
        }
        if (c.hasLocal && !c.hasRemoteSave) {
            std::string ext = remoteExt(c.type);
            std::string remotePath = c.remoteDir + baseOf(localPath) +
                                     (ext.empty() ? std::string("") : ext);
            // ext vuota = tiene la sua: riusa il nome file completo.
            if (ext.empty()) {
                size_t sl = localPath.find_last_of('/');
                remotePath = c.remoteDir + (sl == std::string::npos ? localPath
                                                                    : localPath.substr(sl + 1));
            }
            std::string uerr;
            if (remoteSyncUpload(host, token, localPath, remotePath, uerr)) {
                up++;
                DebugLog::line("remote sync: autoboot, inviato %s", remotePath.c_str());
            } else {
                problem = true;
                problemMsg = std::string(tag) + ": invio fallito";
                DebugLog::line("remote sync: autoboot, invio fallito %s: %s",
                               remotePath.c_str(), uerr.c_str());
            }
        } else if (!c.hasLocal && c.hasRemoteSave) {
            // Base dal remoteSavePath vero (gameTag collide FR/LG). Dir:
            // romsDir + sistema (R36S) o piatto (Switch); ext locale.
            std::string rbase;
            {
                size_t slash = c.remoteSavePath.find_last_of('/');
                std::string f = (slash == std::string::npos) ? c.remoteSavePath
                                                             : c.remoteSavePath.substr(slash + 1);
                size_t dot = f.find_last_of('.');
                rbase = (dot == std::string::npos) ? f : f.substr(0, dot);
            }
            if (rbase.empty())
                rbase = c.remoteRomBaseName.empty() ? std::string(tag) : c.remoteRomBaseName;
            for (char& ch : rbase) if (ch == '/' || ch == '\\') ch = '_';
            bool fileBacked = isFRLG(c.type) || isImportedFile(c.type) || isGen1File(c.type) ||
                              isGen2File(c.type) || isGen4File(c.type) || isGen5File(c.type);
            std::string dd = fileBacked ? romsDir() : s_autoImportDir;
            if (fileBacked) {
#ifdef OH_LINUX
                std::string sys = sysFolderForUpload(c.type);
                if (!sys.empty()) {
                    if (!dd.empty() && dd.back() != '/') dd += "/";
                    dd += sys + "/";
                }
#endif
            }
            if (!dd.empty() && dd.back() != '/') dd += "/";
            std::string dest = dd + rbase + localSaveExt(c.type);
            std::string derr;
            if (remoteSyncDownload(host, token, c.remoteSavePath, dest, derr)) {
                down++;
                DebugLog::line("remote sync: autoboot, ricevuto %s", dest.c_str());
            } else {
                problem = true;
                problemMsg = std::string(tag) + ": ricezione fallita";
                DebugLog::line("remote sync: autoboot, ricezione fallita %s: %s",
                               dest.c_str(), derr.c_str());
            }
        } else {
            if (syncSameSha(localPath, c.remoteSaveSha)) {
                DebugLog::line("remote sync: autoboot, %s sha identico, skip", tag);
                continue;
            }
            std::string rtmp = tmpBase + tag + ".tmp";
            std::string derr;
            if (!remoteSyncDownload(host, token, c.remoteSavePath, rtmp, derr)) {
                problem = true;
                problemMsg = std::string(tag) + ": controllo fallito";
                DebugLog::line("remote sync: autoboot, download controllo fallito %s: %s",
                               tag, derr.c_str());
                continue;
            }
            SaveFingerprint lf = syncFingerprintFile(localPath, c.type);
            SaveFingerprint rf = syncFingerprintFile(rtmp, c.type);
            FpVerdict v = syncCompareFp(lf, rf);
            DebugLog::line("remote sync: autoboot, %s OT=%s/%s PT=%ld/%ld dex=%d/%d => %s",
                           tag, lf.ot.c_str(), rf.ot.c_str(), lf.playSeconds, rf.playSeconds,
                           lf.dexCaught, rf.dexCaught, syncFpVerdictName(v));
            if (v == FpVerdict::Equal) {
                std::remove(rtmp.c_str());
                continue;
            } else if (v == FpVerdict::LocalNewer) {
                std::string uerr;
                if (remoteSyncUpload(host, token, localPath, c.remoteSavePath, uerr)) {
                    up++;
                    DebugLog::line("remote sync: autoboot, aggiornato %s", tag);
                } else {
                    problem = true;
                    problemMsg = std::string(tag) + ": invio fallito";
                    DebugLog::line("remote sync: autoboot, invio fallito %s: %s",
                                   tag, uerr.c_str());
                }
            } else if (v == FpVerdict::RemoteNewer) {
                // Nativo: localPath qui e' il tmp di risoluzione (vedi ui.cpp),
                // MAI il vero save -- scriverci sopra si perde al prossimo
                // avvio (bug reale, 2026-09-28: "aggiornato locale" loggato,
                // ma la galleria restava alla versione vecchia). mountSave/
                // commitSave non sono sicure da questo worker: si scarica in
                // un tmp dedicato e si lascia la scrittura vera al main via
                // remoteSyncAutoBootTakePendingNative().
                std::string derr2;
                if (c.localIsNativeSwitch) {
                    std::string nativeTmp = tmpBase + tag + "_native_recv.tmp";
                    if (remoteSyncDownload(host, token, c.remoteSavePath, nativeTmp, derr2)) {
                        down++;
                        s_autoPendingNative.push_back({c.type, nativeTmp});
                        DebugLog::line("remote sync: autoboot, ricevuto %s per scrittura nativa differita", tag);
                    } else {
                        problem = true;
                        problemMsg = std::string(tag) + ": ricezione fallita";
                        DebugLog::line("remote sync: autoboot, ricezione fallita %s: %s",
                                       tag, derr2.c_str());
                    }
                    std::remove(rtmp.c_str());
                    continue;
                }
                if (remoteSyncDownload(host, token, c.remoteSavePath, localPath, derr2)) {
                    down++;
                    DebugLog::line("remote sync: autoboot, aggiornato locale %s", tag);
                } else {
                    problem = true;
                    problemMsg = std::string(tag) + ": ricezione fallita";
                    DebugLog::line("remote sync: autoboot, ricezione fallita %s: %s",
                                   tag, derr2.c_str());
                }
            } else {
                problem = true;
                problemMsg = std::string(tag) + ": giocatori diversi, risolvi a mano";
                DebugLog::line("remote sync: autoboot, %s in conflitto, salto", tag);
            }
            std::remove(rtmp.c_str());
        }
    }
    s_autoUp.store(up, std::memory_order_release);
    s_autoDown.store(down, std::memory_order_release);
    s_autoProblem.store(problem, std::memory_order_release);
    s_autoProblemMsg = problemMsg;
    DebugLog::line("remote sync: autoboot, su %s: inviati=%d ricevuti=%d problemi=%d",
                   host.c_str(), up, down, problem ? 1 : 0);
    s_autoState.store(2, std::memory_order_release); // DOPO i conti: vedi sopra
}

} // namespace

void remoteSyncAutoBootStart(const std::vector<ImportedGame>& localGames,
                             const std::string& fileDir, const std::string& importDir,
                             const std::string& tmpDir) {
    bool expected = false;
    if (!s_autoStarted.compare_exchange_strong(expected, true))
        return; // un solo tentativo per boot
    // Modello B: niente riga modo — associato ⟺ server. Legacy (device)
    // mai in automatico: solo presenza, vedi guardia sul ruolo sotto.
    if (!Settings::syncAutoBoot())
        return;
    if (Settings::syncServerInstance().empty())
        return;
    s_autoInstance = Settings::syncServerInstance();
    s_autoUser = Settings::syncLoginUser();
    s_autoPass = Settings::syncLoginPass();
    s_autoLocal = localGames;
    s_autoFileDir = fileDir;
    s_autoImportDir = importDir;
    s_autoTmpDir = tmpDir;
    s_autoUp.store(0, std::memory_order_release);
    s_autoDown.store(0, std::memory_order_release);
    s_autoProblem.store(false, std::memory_order_release);
    s_autoProblemMsg.clear();
    s_autoState.store(1, std::memory_order_release);
    Result rcCreate = threadCreate(&s_autoThread, remoteSyncAutoBootMain, nullptr, nullptr,
                                   64 * 1024, 0x2C, -2); // come kWorkerStackSize sopra
    Result rcStart = R_SUCCEEDED(rcCreate) ? threadStart(&s_autoThread) : rcCreate;
    if (R_SUCCEEDED(rcCreate) && R_SUCCEEDED(rcStart))
        s_autoRunning.store(true, std::memory_order_release);
    else
        s_autoState.store(3, std::memory_order_release);
}

bool remoteSyncAutoBootPoll(std::string& outHost, std::string& outToken,
                            int& outUp, int& outDown,
                            bool& outProblem, std::string& outProblemMsg) {
    int state = s_autoState.load(std::memory_order_acquire);
    if (state == 2) {
        s_autoState.store(0, std::memory_order_release); // consumato una sola volta
        outHost = s_autoHost;
        outToken = s_autoToken;
        outUp = s_autoUp.load(std::memory_order_acquire);
        outDown = s_autoDown.load(std::memory_order_acquire);
        outProblem = s_autoProblem.load(std::memory_order_acquire);
        outProblemMsg = s_autoProblemMsg;
        return true;
    }
    return false; // 3 consumato solo da FailPoll (serve kind+msg), pending = false
}

bool remoteSyncAutoBootFailPoll(int& outKind, std::string& outDetail) {
    int state = s_autoState.load(std::memory_order_acquire);
    if (state != 3) return false;
    s_autoState.store(0, std::memory_order_release); // consumato una sola volta
    outKind = s_autoFailKind.load(std::memory_order_acquire);
    outDetail = s_autoFailMsg;
    return true;
}

bool remoteSyncAutoBootBusy() {
    return s_autoStarted.load(std::memory_order_acquire) &&
           s_autoState.load(std::memory_order_acquire) == 1;
}

bool remoteSyncAutoBootTakePendingNative(std::vector<NativeSyncWrite>& out) {
    if (s_autoPendingNative.empty()) return false;
    out = std::move(s_autoPendingNative);
    s_autoPendingNative.clear();
    return true;
}

void remoteSyncAutoBootJoin() {
    if (!s_autoStarted.load(std::memory_order_acquire))
        return;
    if (!s_autoRunning.load(std::memory_order_acquire))
        return;
    threadWaitForExit(&s_autoThread);
    threadClose(&s_autoThread);
}

void remoteSyncWorkerJoin() {
    if (!s_workerStarted.load(std::memory_order_acquire))
        return;
    if (!s_workerRunning.load(std::memory_order_acquire))
        return; // start fallita: nessun thread da aspettare
    s_workerStop.store(true, std::memory_order_release);
    threadWaitForExit(&s_worker);
    threadClose(&s_worker);
}
