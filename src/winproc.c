/* Selene - utilitaires Windows (fenêtre console cachée, ouverture de dossiers).
 * Fichier séparé : windows.h entre en conflit avec raylib (Rectangle, CloseWindow...), il ne peut pas être inclus dans main.c. */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

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
