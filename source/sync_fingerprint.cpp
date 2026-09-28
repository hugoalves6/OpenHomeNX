// sync_fingerprint.cpp — vedi sync_fingerprint.h. Solo Gen3 (FRLG/RSE):
// trainer block in sezione 0 (layout condiviso SaveBlock2: OT 8B @0x00,
// TID u16LE @0x0A, SID @0x0C, playtime @0x0E -- stessi offset usati da
// playTimeSeconds(), verificati su pret/), dex via Pokedex::getDexStatus.
#include "sync_fingerprint.h"

#include <cstring>

#include "debug_log.h"
#include "pokedex.h"
#include "pokemon.h" // decodeGen3String pubblica
#include "save_file.h"
#include "update_net.h" // sha256HexFile

// u16string OT -> utf8 (stesso pattern dei loader in save_file.cpp).
static std::string fpOtToUtf8(const std::u16string& ot) {
    std::string out;
    out.reserve(ot.size());
    for (char16_t c : ot) {
        if (c == 0) break;
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

SaveFingerprint syncFingerprint(SaveFile& save) {
    SaveFingerprint fp;
    GameType g = save.gameType();
    if (isImportedFile(g) || isFRLG(g)) {
        // Gen3: blocco trainer in sezione 0 (OT 8B @0x00, TID @0x0A,
        // SID @0x0C). Diretto, non dsOtName_/dsTid_ (il loader GBA lascia
        // dsTid a 0 -- vedi loadGBA).
        uint8_t* sec0 = save.findGbaSectorData(0);
        if (!sec0) return fp;
        fp.ot = decodeGen3String(sec0, 0, 8, isFRLG_JA(g));
        if (fp.ot.empty()) return fp;
        uint16_t tid = 0, sid = 0;
        std::memcpy(&tid, sec0 + 0x0A, 2);
        std::memcpy(&sid, sec0 + 0x0C, 2);
        fp.id32 = (uint32_t)tid | ((uint32_t)sid << 16);
    } else if (isGen1File(g) || isGen2File(g)) {
        // Gen1/2: OT/TID dal trainer block (loadGB/loadGBC li mettono in
        // dsOtName_/dsTid_). Playtime dal formato (0x2CED / 0x2052 BE).
        fp.ot = save.dsOtName();
        if (fp.ot.empty()) return fp;
        fp.id32 = save.dsTid(); // niente SID in Gen1/2
    } else {
        // Tutte le altre (LGPE/BDSP/SwSh/LA/SV/ZA/Gen4-7): via
        // getTrainerInfo(). Dove non c'e' reader (Gen4/5/6/7) torna
        // invalid -> Unknown onesto, mai dati inventati.
        TrainerInfo ti = save.getTrainerInfo();
        if (!ti.valid) return fp;
        fp.ot = fpOtToUtf8(ti.otName);
        if (fp.ot.empty()) return fp;
        fp.id32 = ti.id32;
    }
    fp.playSeconds = save.playTimeSeconds();
    Pokedex::DexStatus dex = Pokedex::getDexStatus(save);
    if (dex.supported) {
        fp.dexCaught = dex.caught;
        fp.dexTotal = dex.total;
    }
    fp.valid = true;
    return fp;
}

SaveFingerprint syncFingerprintFile(const std::string& path, GameType type) {
    SaveFingerprint fp;
    if (path.empty()) return fp;
    SaveFile save;
    save.setGameType(type);
    if (!save.load(path)) {
        DebugLog::line("sync fp: load fallito %s", path.c_str());
        return fp;
    }
    return syncFingerprint(save);
}

FpVerdict syncCompareFp(const SaveFingerprint& local, const SaveFingerprint& remote) {
    if (!local.valid || !remote.valid) return FpVerdict::Unknown;
    if (local.id32 != remote.id32 || local.ot != remote.ot)
        return FpVerdict::DifferentPlayer;
    // Stesso giocatore: prima il tempo di gioco, poi il dex. mtime mai.
    if (local.playSeconds >= 0 && remote.playSeconds >= 0) {
        if (local.playSeconds != remote.playSeconds)
            return local.playSeconds > remote.playSeconds ? FpVerdict::LocalNewer
                                                          : FpVerdict::RemoteNewer;
    }
    if (local.dexCaught >= 0 && remote.dexCaught >= 0) {
        if (local.dexCaught != remote.dexCaught)
            return local.dexCaught > remote.dexCaught ? FpVerdict::LocalNewer
                                                      : FpVerdict::RemoteNewer;
    }
    // Stesso giocatore ma nessun segnale con cui ordinare (es. Gen1/2 con
    // stesso dex): Equal, si salta in silenzio. MAI mtime, MAI blocco: il
    // blocco resta solo per giocatori diversi o file illeggibili.
    return FpVerdict::Equal;
}

bool syncSameSha(const std::string& localPath, const std::string& remoteSha) {
    if (remoteSha.empty() || localPath.empty()) return false;
    if (localPath.rfind("save:/", 0) == 0) return false;
    std::string localSha = sha256HexFile(localPath);
    if (localSha.empty()) return false;
    return localSha == remoteSha;
}

const char* syncFpVerdictName(FpVerdict v) {
    switch (v) {
        case FpVerdict::DifferentPlayer: return "DifferentPlayer";
        case FpVerdict::LocalNewer: return "LocalNewer";
        case FpVerdict::RemoteNewer: return "RemoteNewer";
        case FpVerdict::Equal: return "Equal";
        default: return "Unknown";
    }
}
