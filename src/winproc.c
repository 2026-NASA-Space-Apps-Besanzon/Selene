/* Selene - utilitaires Windows (fenêtre console cachée, ouverture de dossiers).
 * Fichier séparé : windows.h entre en conflit avec raylib (Rectangle, CloseWindow...), il ne peut pas être inclus dans main.c. */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

/* Lance « cmd.exe /C <commande> » SANS fenêtre, attend la fin et renvoie le code de sortie (-1 si le lancement échoue). */
int sln_run_hidden(const char *cmdline) {
    char buf[2400];
    snprintf(buf, sizeof buf, "cmd.exe /C \"%s\"", cmdline);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

/* Ouvre un dossier ou un fichier avec le programme par défaut de Windows (sans passer par une console). */
void sln_open(const char *path) {
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
}

/* Détache la console si le programme a été compilé sans -mwindows (évite la fenêtre noire au démarrage). */
void sln_hide_console(void) {
    FreeConsole();
}

/* Parcourt un dossier (et ses sous-dossiers si rec != 0) et appelle cb(chemin, taille en octets) pour chaque fichier .tif / .tiff (et chaque « ldem* »).
 * N'utilise PAS stat() : celui de MinGW échoue au-delà de 2 Go, et raylib ne « voit » alors plus les gros MNT (LDEM128 : plusieurs Go).
 * Les dossiers data/input et data/sites (résultats préparés) sont ignorés. */
void sln_scan_tifs(const char *dir, int rec, void (*cb)(const char *path, unsigned long long size)) {
    char pat[MAX_PATH], sub[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (rec && _stricmp(fd.cFileName, "input") != 0 && _stricmp(fd.cFileName, "sites") != 0) sln_scan_tifs(sub, rec, cb);
        } else {
            const char *dot = strrchr(fd.cFileName, '.');
            int is_tif = dot && (_stricmp(dot, ".tif") == 0 || _stricmp(dot, ".tiff") == 0);
            if (is_tif || _strnicmp(fd.cFileName, "ldem", 4) == 0)         /* les « ldem* » d'extension inattendue sont signalés aussi */
                cb(sub, ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}