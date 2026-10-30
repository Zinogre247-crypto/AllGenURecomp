#!/usr/bin/env python3
"""Comprueba que el 'main' es el de Monster Hunter Generations Ultimate v1.4.0.

Uso: python3 tools/verificar-dump.py gamedata/exefs/main
"""
import sys

BUILD_ID = "fb08f1d20fd1204f"   # v1.4.0


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "gamedata/exefs/main"
    try:
        with open(path, "rb") as f:
            d = f.read(0x48)
    except OSError as e:
        sys.exit(f"ERROR: no puedo abrir {path}: {e}")
    if len(d) < 0x48:
        sys.exit(f"ERROR: {path} es demasiado pequeno ({len(d)} bytes)")
    if d[:4] != b"NSO0":
        sys.exit(f"ERROR: {path} no es un NSO (empieza por {d[:4]!r})")
    bid = d[0x40:0x48].hex()
    print("build id:", bid)
    if bid != BUILD_ID:
        sys.exit("ERROR: NO es la version 1.4.0 (build id distinto). "
                 "Reextrae el juego con la actualizacion 1.4.0 aplicada.")
    print("OK: Monster Hunter Generations Ultimate v1.4.0")


if __name__ == "__main__":
    main()
