#pragma once
// sync_fingerprint.h — impronta semantica di un save per il sync server.
//
// MAI mtime: basta aprire/chiudere per cambiarlo. Il confronto e' sui dati
// dentro il save, in quest'ordine (decisione utente): stesso giocatore?
// (OT+TID/SID) -> tempo di gioco embedded -> dex. Gen3 diretto (FRLG/RSE),
// Gen1/2 via dsOtName/dsTid + dex (niente playtime nel formato). Stesso
// giocatore senza segnali -> Equal (skip). Altre gen: valid=false.
#include <cstdint>
#include <string>

#include "game_type.h"

class SaveFile;

struct SaveFingerprint {
    bool valid = false;
    std::string ot;       // nome allenatore (utf8)
    uint32_t id32 = 0;    // TID | SID<<16
    long playSeconds = -1; // -1 = non letto
    int dexCaught = -1;   // -1 = non letto
    int dexTotal = 0;
};

// Save gia' caricato (locale). Ritorna invalid se gen non coperta.
SaveFingerprint syncFingerprint(SaveFile& save);

// File su disco (usato per il remoto scaricato in tmp): imposta il tipo,
// carica + impronta. Il tipo serve: SaveFile::load() smista sul gameType_
// membro e senza setGameType() finisce nel loader sbagliato (crash 2168).
SaveFingerprint syncFingerprintFile(const std::string& path, GameType type);

enum class FpVerdict {
    Unknown,          // dati insufficienti -> manuale
    DifferentPlayer,  // OT o ID diversi -> sempre esplicito
    LocalNewer,       // stesso giocatore, locale avanti (playtime, poi dex)
    RemoteNewer,      // stesso giocatore, remoto avanti
    Equal,            // tutto uguale -> niente da fare
};

FpVerdict syncCompareFp(const SaveFingerprint& local, const SaveFingerprint& remote);
const char* syncFpVerdictName(FpVerdict v);

// Fast-path Equal senza download: vero se lo sha remoto (dall'hub) coincide
// con quello del file locale. Marker save:/ e sha assenti -> false (si
// scarica come prima). Lettura locale veloce, zero rete.
bool syncSameSha(const std::string& localPath, const std::string& remoteSha);
