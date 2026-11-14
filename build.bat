@echo off
REM Recompiler Selene (necessite le compilateur gcc fourni avec l'installateur raylib : https://www.raylib.com/)
gcc -O2 -std=c99 -o selene.exe src\main.c -I C:\raylib\raylib\src -L C:\raylib\raylib\src -lraylib -lopengl32 -lgdi32 -lwinmm -static -mwindows
if %errorlevel% neq 0 (echo ECHEC de la compilation) else (echo OK : selene.exe genere)
pause
