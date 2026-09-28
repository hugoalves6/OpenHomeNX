@echo off
rem OHNX-Hub.bat — doppio click su Windows per avviare l'hub.
rem Cerca il launcher Python, installa zeroconf se manca, crea
rem %USERPROFILE%\OHNX-Data. Niente admin, niente Docker: porta 8321,
rem la Switch lo trova via mDNS. Ctrl-C per uscire.
setlocal
where py >nul 2>nul
if errorlevel 1 (
    echo Serve Python 3 da https://www.python.org/downloads/
    echo   ^(spunta "Add python.exe to PATH" in installazione^). Poi rilancia.
    pause
    exit /b 1
)
py -c "import zeroconf" 2>nul
if errorlevel 1 (
    echo Installo zeroconf ^(una volta sola^)...
    py -m pip install --user -q zeroconf
    if errorlevel 1 (
        echo pip fallito: prova "py -m pip install --user zeroconf" a mano.
        pause
        exit /b 1
    )
)
set "DATA=%USERPROFILE%\OHNX-Data"
if not exist "%DATA%" mkdir "%DATA%"
py "%~dp0hub.py" "%DATA%"
