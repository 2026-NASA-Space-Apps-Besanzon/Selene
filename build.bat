@echo off
cd /d "%~dp0"
"C:\msys64\ucrt64\bin\gcc.exe" src\main.c -o selene.exe -O2 -Wall -lraylib -lopengl32 -lgdi32 -lwinmm
if errorlevel 1 (
  echo.
  echo === ECHEC DE LA COMPILATION ===
) else (
  echo.
  echo === OK : selene.exe cree ===
)
pause