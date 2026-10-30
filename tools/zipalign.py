#!/usr/bin/env python3
"""
zipalign.py — reemplazo de `zipalign` para Termux (el binario oficial de
Android SDK es x86_64 y no corre en el telefono).

Reescribe el APK de forma que los datos de cada entrada queden alineados
a 4 bytes (y resources.arsc sin comprimir), igual que hace zipalign -f 4.

Uso:
    python zipalign.py entrada.apk salida.apk
    python zipalign.py -P 16 entrada.apk salida.apk   # paginas de 16 KB (Android 15+)
    python zipalign.py -c -P 16 entrada.apk           # solo verificar

Android 15+ exige que las librerias nativas (lib/*/**.so) esten alineadas a
16384 bytes y SIN comprimir. Las de 4 bytes (resources.arsc, dex) siguen igual.
"""
import sys
import zipfile

ALIGN = 4
# Campo "extra" que usa zipalign (ID 0xd935) para rellenar
PAD_ID = 0xD935


def align_apk(src, dst, align=ALIGN, page=ALIGN):
    zin = zipfile.ZipFile(src, "r")
    with open(dst, "wb") as fout:
        zout = zipfile.ZipFile(fout, "w")

        # Orden recomendado: manifest, resources.arsc, resto
        names = zin.namelist()
        first = [n for n in ("AndroidManifest.xml", "resources.arsc") if n in names]
        rest = [n for n in names if n not in first]
        orden = first + rest

        for name in orden:
            info = zin.getinfo(name)
            data = zin.read(name)

            # las .so necesitan la pagina del dispositivo (16 KB en Android 15+)
            es_so = name.endswith(".so")
            alineacion = page if es_so else align

            zi = zipfile.ZipInfo(name, date_time=info.date_time)
            zi.external_attr = info.external_attr
            zi.internal_attr = info.internal_attr
            zi.create_system = info.create_system

            # v1 (META-INF) ya no debe existir: firmamos despues.
            if name == "resources.arsc" or es_so:
                zi.compress_type = zipfile.ZIP_STORED
            else:
                zi.compress_type = zipfile.ZIP_DEFLATED

            # padding para que el offset de los datos sea multiplo de `align`
            offset = fout.tell()
            header = 30 + len(name.encode("utf-8"))
            pad = (-(offset + header)) % alineacion
            if pad < 4:
                pad += alineacion
            extra = PAD_ID.to_bytes(2, "little") + (pad - 4).to_bytes(2, "little")
            extra += b"\x00" * (pad - 4)
            zi.extra = extra

            zout.writestr(zi, data)

        zout.close()
    zin.close()


def local_data_offset(f, info):
    """Offset real de los datos: el campo `extra` del header LOCAL puede ser
    distinto al del directorio central (zipalign guarda ahi el relleno)."""
    f.seek(info.header_offset)
    hdr = f.read(30)
    if len(hdr) < 30 or hdr[:4] != b"PK\x03\x04":
        return None
    name_len = int.from_bytes(hdr[26:28], "little")
    extra_len = int.from_bytes(hdr[28:30], "little")
    return info.header_offset + 30 + name_len + extra_len


def verify(path, align=ALIGN, page=ALIGN):
    ok = True
    with zipfile.ZipFile(path) as z, open(path, "rb") as f:
        for i in z.infolist():
            off = local_data_offset(f, i)
            if off is None:
                print(f"  MAL  header corrupto: {i.filename}"); ok = False; continue
            es_so = i.filename.endswith(".so")
            # Solo importa la alineacion de lo que Android mapea/lee directo:
            #   .so            -> pagina (16 KB) y sin comprimir
            #   resources.arsc -> 4 bytes, sin comprimir
            # El resto (xml, dex comprimidos) no necesita alineacion.
            if es_so:
                good = (off % page) == 0 and i.compress_type == zipfile.ZIP_STORED
            elif i.filename == "resources.arsc":
                good = (off % align) == 0 and i.compress_type == zipfile.ZIP_STORED
            else:
                good = True
            print(f"  {'OK ' if good else 'MAL'}  {off:>8}  {i.filename}")
            ok = ok and good
    print("Verificacion:", "OK" if ok else "FALLÓ")
    return ok


if __name__ == "__main__":
    a = sys.argv[1:]
    page = ALIGN
    modo_check = False
    limpio = []
    i = 0
    while i < len(a):
        if a[i] == "-c":
            modo_check = True
        elif a[i] == "-P":
            page = int(a[i + 1]) * 1024
            i += 1
        elif a[i] in ("-v", "-f"):
            pass
        elif a[i] in ("4", "4096"):
            pass
        else:
            limpio.append(a[i])
        i += 1

    if modo_check:
        if len(limpio) != 1:
            print(__doc__)
            sys.exit(2)
        sys.exit(0 if verify(limpio[0], ALIGN, page) else 1)
    if len(limpio) != 2:
        print(__doc__)
        sys.exit(2)
    align_apk(limpio[0], limpio[1], ALIGN, page)
    print(f"alineado -> {limpio[1]} (paginas .so: {page} bytes)")
