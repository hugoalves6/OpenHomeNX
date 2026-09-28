#!/bin/bash
# OHNX-Hub.command — doppio click nel Finder per avviare l'hub.
# Fa tutto da solo: cerca python3, installa zeroconf se manca, crea
# ~/OHNX-Data. Niente password, niente Docker: l'hub gira sulla porta 8321
# e la Switch lo trova via mDNS. Log visibili nel Terminale. Ctrl-C per uscire.
cd "$(dirname "$0")"
DATA="$HOME/OHNX-Data"
mkdir -p "$DATA" || exit 1
if ! command -v python3 >/dev/null 2>&1; then
    echo "Serve Python 3 (https://www.python.org/downloads/). Poi rilancia."
    read -p "Premi Invio per chiudere. " _dummy
    exit 1
fi
if ! python3 -c "import zeroconf" 2>/dev/null; then
    echo "Installo zeroconf (una volta sola)..."
    python3 -m pip install --user -q zeroconf || {
        echo "pip fallito: prova 'python3 -m pip install --user zeroconf' a mano."
        read -p "Premi Invio per chiudere. " _dummy
        exit 1
    }
fi
exec python3 hub.py "$DATA"
