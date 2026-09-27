#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

// Rilevamento gioco/lingua dall'header ROM (offline, niente rete).
//
// GBA: complement check 0xA0-0xBD + game code 0xAC-0xAF (4o char = regione
// Nintendo standard: E=USA, J=Giappone, P=Europa, D=Germania, F=Francia,
// S=Spagna, I=Italia).
// GB/GBC: checksum header 0x14D + titolo interno 0x134-0x143 (tabella titoli
// per lingua; flag CGB 0x143 distingue gb/gbc).
// NDS: solo kind via CRC16 logo (game non mappato -> niente canonical).
namespace RomInfo {

struct Info {
    bool valid = false;
    std::string kind; // "gba", "gb", "gbc", "nds"
    std::string game; // "emerald", "crystal", ... ("" se ignoto)
    std::string lang; // "en","it","es","fr","de","ja" ("" se ignota)
    std::string region; // "USA", "Italy", ... ("" se ignota)
};

bool detect(const std::string& path, Info& out);

// Nome canonico stile No-Intro SENZA estensione
// (es. "Pokemon - Versione Cristallo (Italy)"). "" se game/lang ignoti.
std::string canonicalName(const Info& info);

// Piano di rinomina: ROM + file associati stesso basename (.sav/.srm/.dsv
// .state/.state.auto/.rtc/.bak) + patch <path> in gamelist.xml.
// Ritorna false se niente da rinominare (giusto nome o non rilevato).
struct RenamePlan {
    std::string dir;
    std::string oldBase, newBase, ext;
    std::vector<std::pair<std::string, std::string>> moves; // {src,dst} full path
};
bool planRename(const std::string& romPath, RenamePlan& out);
bool applyRename(const RenamePlan& plan, std::string& err);

// Elenca le ROM (.gba/.gbc/.gb/.nds, case-insensitive, non ricorsivo)
// nelle cartelle date, ordinato per path. Usato da Rinomina ROM per
// passare a setaccio tutte le ROM, anche senza save associato.
std::vector<std::string> scanRoms(const std::vector<std::string>& dirs);

// Elenca i save (.sav/.srm/.dsv, case-insensitive, non ricorsivo).
// Per gli orfani (senza ROM omonima): vedi planOrphanSave sotto.
std::vector<std::string> scanSaves(const std::vector<std::string>& dirs);

// Piano per un save orfano (nessuna ROM con lo stesso basename nella
// stessa cartella): cerca nella lista l'UNICA ROM dello stesso gioco
// (rilevata da header per le ROM, da keyword per il save) e propone il
// rename del save sul base della ROM (nuovo base se anche la ROM viene
// rinominata in questo giro). Ritorna false se ambiguo (0 o 2+ ROM
// candidate) o gioco non determinabile: mai indovinare, logga e salta.
bool planOrphanSave(const std::string& savePath,
                    const std::vector<std::string>& romsInDir,
                    const std::map<std::string, std::string>& renamedBase,
                    RenamePlan& out);

} // namespace RomInfo
