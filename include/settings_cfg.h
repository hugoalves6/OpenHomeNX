#pragma once
#include <string>

// Unico file impostazioni (key=value): settings.cfg in basePath_.
// Sostituisce i 10 file sparsi (theme/zoom/gallery/crypto/autocheck_usb/
// launcher_prompt_seen/noled/language/defaultuser/quickmenu).
// Restano fuori apposta: favorites.cfg (cambia spesso), import_paths.cfg,
// update.cfg/.off (override updater), overrideOT.cfg (override galleria),
// gallery_cache.dat (cache), debug.enable (flag leggibile).
//
// init() va chiamata una volta a boot (idempotente): se settings.cfg manca,
// importa i file legacy una volta sola e li cancella. Mai perdita: i default
// sono quelli storici di ogni loader.
namespace Settings {
void init(const std::string& basePath);

int themeIndex();
void setThemeIndex(int v);
int zoomGrow();
void setZoomGrow(int v);
int galleryLayout();
void setGalleryLayout(int v);
int cryptoEngine();
void setCryptoEngine(int v); // 1 = OH, 0 = PK
bool autoCheckUsb();
void setAutoCheckUsb(bool v);
bool launcherSeen();
void setLauncherSeen();
bool noLed();
std::string language();
void setLanguage(const std::string& v);
std::string defaultUser(); // "" = chiedi
void setDefaultUser(const std::string& v);
bool quickMenu();
void setQuickMenu(bool v);
bool radialMenu(); // menu radiale (Classica): on/off, default off finche' non implementato
void setRadialMenu(bool v);
bool tradeAnim(); // animazione schermo diviso allo scambio: on/off, default on
void setTradeAnim(bool v);
std::string dockOrder(); // CSV: "Backpack,Banks,SaveMenu,Trade,Eject"
void setDockOrder(const std::string& v);
bool dockVisible(); // mostra/nascondi dock inferiore
void setDockVisible(bool v);
// Impostazioni -> Sviluppatore -> Ricerca dispositivi: host/credenziali del
// Filebrowser web esposto da ArkOS/JELOS/ROCKNIX (vedi remote_sync.h).
std::string remoteSyncHost(); // "" = non ancora configurato (si chiede l'IP)
void setRemoteSyncHost(const std::string& v);
std::string remoteSyncUser(); // default "ark" (credenziale di default ArkOS)
void setRemoteSyncUser(const std::string& v);
std::string remoteSyncPass(); // default "ark"
void setRemoteSyncPass(const std::string& v);
int remoteSyncProbeMs(); // Sviluppatore -> Timeout probe: 50-800 step 50, default 150
void setRemoteSyncProbeMs(int v);
// Sincronizzazione save (categoria a parte): 0=Off, 1=Server, 2=Dispositivo.
// Mutuamente esclusiva per costruzione: una sola auto-sync alla volta.
int syncMode();
void setSyncMode(int v);
// Istanza mDNS associata (es. "OHN X-Hub-Mac"): pairing stabile oltre il DHCP.
std::string syncServerInstance();
void setSyncServerInstance(const std::string& v);
// Sync dei salvataggi col server all'avvio (thread dedicato, solo casi
// one-sided: conflitti mai toccati in automatico).
bool syncAutoBoot();
void setSyncAutoBoot(bool v);
// Connessione Auto (default) / Manuale: in Auto IP da discovery e
// credenziali sempre ark/ark; in Manuale righe visibili + credenziali tue.
bool syncConnAuto();
void setSyncConnAuto(bool v);
// Credenziali EFFETTIVE per la rete (non per il menu, che mostra le tue).
std::string syncLoginUser();
std::string syncLoginPass();
// Prima config server proposta una sola volta (YES o NO = mai piu').
bool syncFirstAsked();
void setSyncFirstAsked(bool v);
int boxartStyle(); // Sviluppatore -> Stile boxart: 0=Locale, 1=2D, 2=3D
void setBoxartStyle(int v);
bool showRomsWithoutSave(); // Sviluppatore -> Mostra ROM senza save
void setShowRomsWithoutSave(bool v);
bool showFrlgRoms(); // Sviluppatore -> Versioni ROM di FRLG: default OFF (nascoste se nativa)
void setShowFrlgRoms(bool v);
bool frlgAutoSync(); // Sviluppatore -> Auto-sync FRLG: default OFF finche' non e' provato
void setFrlgAutoSync(bool v);
bool confirmExit(); // Sistema -> Conferma uscita con B: off = esci subito
void setConfirmExit(bool v);
} // namespace Settings
