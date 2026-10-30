#!/usr/bin/env python3
"""Normaliza el dump (venga como zip, tar o carpeta suelta) a gamedata/exefs/.

Se ejecuta DENTRO del workflow despues de clonar el repo privado. Acepta las
formas en que alguien puede haber subido el exefs desde el telefono:

  * exefs/main, exefs/main.npdm, ...            (carpeta en la raiz)
  * dump/exefs/main, ...                        (dentro de una carpeta)
  * exefs.zip  /  dump.zip  /  loquesea.zip     (zip con o sin carpeta dentro)
  * dump.tar / dump.tar.gz / dump.tar.zst       (tar con o sin carpeta dentro)
  * main, main.npdm, rtld, sdk, subsdk0         (archivos sueltos en la raiz)

Uso:  python3 preparar-dump.py <carpeta_del_clon> <carpeta_salida>
"""
import os
import shutil
import sys
import tarfile
import zipfile

FIRMAS = ("main", "main.npdm", "rtld", "sdk", "subsdk0")


def es_exefs(d):
    if not os.path.isdir(d):
        return False
    entradas = set(os.listdir(d))
    return "main" in entradas and (entradas & set(FIRMAS))


def buscar_exefs(raiz, profundidad=4):
    """Devuelve la primera carpeta que parezca un exefs/, hasta N niveles."""
    if es_exefs(raiz):
        return raiz
    nivel = [raiz]
    for _ in range(profundidad):
        siguiente = []
        for d in nivel:
            try:
                hijos = [os.path.join(d, x) for x in os.listdir(d)]
            except OSError:
                continue
            for h in hijos:
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
    clon, salida = sys.argv[1], sys.argv[2]
    exefs = buscar_exefs(clon)
    if exefs:
        print(f"    exefs encontrado en: {os.path.relpath(exefs, clon)}")
    else:
        # ultimo recurso: archivos sueltos en la raiz
        sueltos = [f for f in FIRMAS if os.path.isfile(os.path.join(clon, f))]
        if len(sueltos) >= 2:
            tmp = os.path.join(salida, "exefs")
            os.makedirs(tmp, exist_ok=True)
            for f in sueltos:
                shutil.copy2(os.path.join(clon, f), tmp)
            print(f"    archivos sueltos en la raiz: {', '.join(sueltos)}")
            exefs = tmp
        else:
            # comprimidos: extraer y volver a buscar
            for nombre in sorted(os.listdir(clon)):
                p = os.path.join(clon, nombre)
                if not os.path.isfile(p):
                    continue
                if not (nombre.lower().endswith((".zip", ".tar", ".tar.gz", ".tgz", ".tar.zst", ".zst"))):
                    continue
                print(f"    extrayendo {nombre}...")
                tmp = os.path.join("/tmp", "_extraido")
                shutil.rmtree(tmp, ignore_errors=True)
                if extraer_archivo(p, tmp):
                    exefs = buscar_exefs(tmp)
                    if exefs:
                        print(f"    exefs dentro de {nombre}")
                        break
            if not exefs:
                print("ERROR: no encontre exefs/ (ni un zip/tar con el dentro) en el repo del dump")
                print("       contenidos vistos:", sorted(os.listdir(clon))[:20])
                sys.exit(1)

    destino = os.path.join(salida, "exefs")
    shutil.rmtree(destino, ignore_errors=True)
    os.makedirs(os.path.dirname(destino), exist_ok=True)
    shutil.copytree(exefs, destino)
    print("    exefs -> gamedata/exefs :", sorted(os.listdir(destino)))


if __name__ == "__main__":
    main()
