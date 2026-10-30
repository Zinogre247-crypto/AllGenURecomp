#!/usr/bin/env python3
"""Normaliza el dump (zip, tar o carpeta suelta) a <salida>/exefs/.

Se ejecuta en el workflow despues de clonar el repo privado. Acepta:

  * exefs/main, exefs/main.npdm, ...            (carpeta en la raiz)
  * dump/exefs/main, ...                        (dentro de una carpeta)
  * exefs.zip / dump.zip / loquesea.zip         (zip con o sin carpeta dentro)
  * dump.tar / dump.tar.gz / dump.tar.zst       (tar con o sin carpeta dentro)
  * main, main.npdm, rtld, sdk, subsdk0         (archivos sueltos en la raiz)

IMPORTANTE: un archivo llamado "main" tambien existe dentro de .git (es la rama),
por eso aqui solo cuenta si es un NSO de verdad (empieza por "NSO0" y pesa mas de
1 MB). Sin esa comprobacion el script copiaba la rama en vez del juego.

Uso:  python3 preparar-dump.py <carpeta_del_clon> <carpeta_salida>
"""
import os
import shutil
import sys
import tarfile
import zipfile

OTROS = ("main.npdm", "rtld", "sdk", "subsdk0")
MIN_NSO = 1 << 20  # 1 MB: un "main" de juego nunca es mas pequeno
IGNORAR = {".git", ".github", "node_modules", "__pycache__"}


def es_nso(path):
    """True si path es un NSO real (magia NSO0 y tamano razonable)."""
    try:
        if os.path.getsize(path) < MIN_NSO:
            return False
        with open(path, "rb") as f:
            return f.read(4) == b"NSO0"
    except OSError:
        return False


def es_exefs(d):
    if not os.path.isdir(d):
        return False
    m = os.path.join(d, "main")
    if not es_nso(m):
        return False
    entradas = set(os.listdir(d))
    return bool(entradas & set(OTROS)) or True  # main valido ya basta


def buscar_exefs(raiz, profundidad=4):
    """Primera carpeta con un main NSO de verdad, hasta N niveles. Nunca entra en .git."""
    if es_exefs(raiz):
        return raiz
    nivel = [raiz]
    for _ in range(profundidad):
        siguiente = []
        for d in nivel:
            try:
                hijos = sorted(os.listdir(d))
            except OSError:
                continue
            for x in hijos:
                if x in IGNORAR or x.startswith("."):
                    continue
                h = os.path.join(d, x)
                if os.path.isdir(h):
                    if es_exefs(h):
                        return h
                    siguiente.append(h)
        nivel = siguiente
    return None


def extraer_archivo(path, destino):
    os.makedirs(destino, exist_ok=True)
    try:
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as z:
                z.extractall(destino)
            return True
        if tarfile.is_tarfile(path):
            with tarfile.open(path) as t:
                t.extractall(destino)
            return True
    except Exception as e:  # noqa: BLE001
        print(f"    no pude extraer {path}: {e}")
    return False


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: preparar-dump.py <carpeta_del_clon> <carpeta_salida>")
    clon, salida = sys.argv[1], sys.argv[2]

    exefs = buscar_exefs(clon)
    if exefs:
        print(f"    exefs encontrado en: {os.path.relpath(exefs, clon)}")
    else:
        # 1) archivos sueltos en la raiz
        sueltos = [f for f in ("main",) + OTROS if os.path.isfile(os.path.join(clon, f)) and es_nso(os.path.join(clon, f))]
        if sueltos:
            tmp = os.path.join(salida, "exefs")
            shutil.rmtree(tmp, ignore_errors=True)
            os.makedirs(tmp, exist_ok=True)
            for f in os.listdir(clon):
                p = os.path.join(clon, f)
                if os.path.isfile(p) and f in ("main",) + OTROS:
                    shutil.copy2(p, tmp)
            print(f"    archivos sueltos en la raiz: {', '.join(sorted(os.listdir(tmp)))}")
            exefs = tmp
        else:
            # 2) comprimidos: extraer y volver a buscar
            for nombre in sorted(os.listdir(clon)):
                p = os.path.join(clon, nombre)
                if not os.path.isfile(p):
                    continue
                if not nombre.lower().endswith((".zip", ".tar", ".tar.gz", ".tgz", ".tar.zst", ".zst", ".7z")):
                    continue
                print(f"    extrayendo {nombre} ({os.path.getsize(p) / 1048576:.1f} MB)...")
                tmp = "/tmp/_extraido"
                shutil.rmtree(tmp, ignore_errors=True)
                if extraer_archivo(p, tmp):
                    exefs = buscar_exefs(tmp)
                    if exefs:
                        print(f"    exefs dentro de {nombre}")
                        break
                    print(f"    {nombre} no trae un exefs valido")
                shutil.rmtree(tmp, ignore_errors=True)
            if not exefs:
                print("ERROR: no encontre un exefs/ valido (main debe empezar por NSO0 y pesar > 1 MB)")
                print("       contenidos del repo:", sorted(os.listdir(clon))[:20])
                sys.exit(1)

    destino = os.path.join(salida, "exefs")
    shutil.rmtree(destino, ignore_errors=True)
    os.makedirs(os.path.dirname(destino) or ".", exist_ok=True)
    if exefs == destino:
        pass
    else:
        shutil.copytree(exefs, destino)
    print("    exefs -> gamedata/exefs :", sorted(os.listdir(destino)))
    print(f"    main: {os.path.getsize(os.path.join(destino, 'main')) / 1048576:.1f} MB")


if __name__ == "__main__":
    main()
