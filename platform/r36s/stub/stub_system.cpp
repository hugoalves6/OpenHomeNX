// OpenHomeNX - stub LED + misc system per build Linux/macOS.
#include <string>
#include "path_utils.h"
#include "led.h"
#include "nro_version.h"
#include "autocheck_usb.h"
#include "debug_log.h"

void ledInitWithPath(const char*) {}
void ledExit() {}
void ledBlink() {}
void ledOff() {}

bool readNroDisplayVersion(const std::string&, std::string& out) { out = ""; return false; }
int compareVersionStrings(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    return a < b ? -1 : 1;
}
bool copyFileTo(const std::string& src, const std::string& dst) {
    // Stessa byte-copy di path_utils.h (con controllo errori, che qui mancava).
    return copyFile(src, dst);
}

bool loadAutoCheckUsb(const std::string&) { return false; }
void saveAutoCheckUsb(const std::string&, bool) {}