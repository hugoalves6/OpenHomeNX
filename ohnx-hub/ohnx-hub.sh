#!/bin/bash
# ohnx-hub.sh — avvio hub su Linux (PC, NAS, Raspberry, handheld).
# Stessa logica del .command Mac: python3 + zeroconf automatico, niente
# sudo (porta 8321). OHN_X_DATA per cambiare cartella dati.
set -u
cd "$(dirname "$0")"
DATA="${OHN_X_DATA:-$HOME/OHNX-Data}"
mkdir -p "$DATA" || exit 1
command -v python3 >/dev/null 2>&1 || { echo "Serve python3."; exit 1; }
python3 -c "import zeroconf" 2>/dev/null || python3 -m pip install --user -q zeroconf || {
    echo "zeroconf mancante e pip fallito: niente publish mDNS (solo IP fisso)."
}
exec python3 hub.py "$DATA"
