@echo off
setlocal

cd /d "%~dp0"

set "PATH=C:\msys64\ucrt64\bin;%PATH%"

if not exist "src\app.rc" (
    echo ERREUR : fichier src\app.rc introuvable.
    goto :echec
)

echo === Compilation des ressources Windows ===
windres -i "src\app.rc" -O coff -o "app_res.o"
if errorlevel 1 goto :echec

echo.
echo === Compilation de Selene ===
REM winproc.c : lancement des outils sans fenetre de console. -mwindows : pas de console au demarrage. -lshell32 : ouverture des dossiers.
gcc -std=c11 -O2 -Wall -Wextra src\main.c src\winproc.c app_res.o -o selene.exe -lraylib -lopengl32 -lgdi32 -lwinmm -lshell32 -lm -mwindows
if errorlevel 1 goto :echec

echo.
echo === OK : selene.exe cree ===
if /i not "%1"=="nopause" pause
exit /b 0

:echec
echo.
echo === ECHEC DE LA COMPILATION ===
if /i not "%1"=="nopause" pause
exit /b 1