@echo off
setlocal

cd /d "%~dp0"

rem Utiliser les outils MSYS2 UCRT64, y compris pour le preprocesseur.
set "PATH=C:\msys64\ucrt64\bin;%PATH%"

if not exist "src\app.rc" (
    echo ERREUR : fichier src\app.rc introuvable.
    goto :echec
)

echo === Compilation des ressources Windows ===

"C:\msys64\ucrt64\bin\windres.exe" -i "src\app.rc" -O coff -o "app_res.o"

if errorlevel 1 goto :echec

echo.
echo === Compilation de Selene ===

"C:\msys64\ucrt64\bin\gcc.exe" ^
    -std=c11 ^
    -O2 ^
    -Wall ^
    -Wextra ^
    src\main.c ^
    app_res.o ^
    -o selene.exe ^
    -lraylib ^
    -lopengl32 ^
    -lgdi32 ^
    -lwinmm ^
    -lm

if errorlevel 1 goto :echec

echo.
echo === OK : selene.exe cree ===
pause
exit /b 0

:echec
echo.
echo === ECHEC DE LA COMPILATION ===
echo Lire le message d'erreur affiche au-dessus.
pause
exit /b 1