#!/usr/bin/env python3
"""Outils de traduction de Selene.

  python tools/check_lang.py             -> régénère lang/_modele.txt (tous les textes du code) et liste
                                            les textes sans traduction dans chaque lang/xx.txt
  python tools/check_lang.py en          -> même chose, pour la seule langue « en »

Format d'un fichier de langue : « texte français <TAB> traduction », une ligne par texte, UTF-8.
Première ligne conseillée :  #name=English   (nom affiché dans Outils > Préférences).
Gardez les mêmes codes % (%d, %.1f, %s, %%) dans le même ordre que le texte français.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "main.c")
LANG = os.path.join(ROOT, "lang")

TECH = re.compile(r'^(data|tools|exports|python|cmd|http|lang)|\.(csv|f32|png|txt|bin|exe|h|tif|tiff)\b|^(raylib|raymath|rlgl|sdefl|sinfl|-W|rb$|wb$|zx|%lf|0123456789)')

def ui_strings():
    s = open(SRC, encoding="utf-8").read()
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    out, seen = [], set()
    for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', s):
        if lit in seen or "\\" in lit:
            continue
        seen.add(lit)
        if not re.search(r"[A-Za-zÀ-ÿ]{2,}", lit) and lit not in ("N", "E", "S", "O"):
            continue
        if TECH.search(lit) or lit in ("Selene", "Selene ", "Version ", "r", "w"):
            continue
        out.append(lit)
    return out

def load(path):
    d = {}
    for line in open(path, encoding="utf-8-sig"):
        line = line.rstrip("\r\n")
        if "\t" not in line:
            continue
        k, v = line.split("\t", 1)
        d[k] = v
    return d

def main():
    keys = ui_strings()
    os.makedirs(LANG, exist_ok=True)
    with open(os.path.join(LANG, "_modele.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("#name=NomDeLaLangue\n")
        for k in keys:
            f.write(k + "\t" + k + "\n")
    print("lang/_modele.txt : %d textes" % len(keys))
    only = sys.argv[1] if len(sys.argv) > 1 else None
    for fn in sorted(os.listdir(LANG)):
        if not fn.endswith(".txt") or fn.startswith("_"):
            continue
        code = fn[:-4]
        if only and code != only:
            continue
        tr = load(os.path.join(LANG, fn))
        missing = [k for k in keys if k not in tr]
        print("%s : %d traduits, %d manquants (restent en français)" % (code, len(tr), len(missing)))
        for k in missing:
            print("   ", k)

if __name__ == "__main__":
    main()
