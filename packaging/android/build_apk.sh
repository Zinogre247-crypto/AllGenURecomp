#!/usr/bin/env bash
# =====================================================================
#  Empaqueta el APK de AllGenURecomp (arm64-v8a) a partir de:
#    - src/                       glue Java (SDLActivity + AllGenUActivity)
#    - res/                       iconos + strings
#    - libs/arm64-v8a/*.so        libmain.so (runtime+juego), libSDL3.so, ...
#
#  Corre en GitHub Actions (x86_64 con SDK/NDK) o en un PC Linux.
#  Tambien corre en Termux si le das aapt2/d8/apksigner del repo (ver GUIA.md),
#  pero compilar los .so necesita NDK -> eso es el paso "native".
# =====================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_DIR="$SCRIPT_DIR"
ROOT="$(cd "$PKG_DIR/../.." && pwd)"

ANDROID_SDK_ROOT="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[ -n "$ANDROID_SDK_ROOT" ] || { echo "Falta ANDROID_HOME"; exit 1; }

BUILD_TOOLS="${BUILD_TOOLS:-$(find "$ANDROID_SDK_ROOT/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n1)}"
ANDROID_JAR="${ANDROID_JAR:-$(find "$ANDROID_SDK_ROOT/platforms" -name android.jar | sort -V | tail -n1)}"
NDK_STRIP="${NDK_STRIP:-$(find "${ANDROID_NDK_HOME:-$ANDROID_SDK_ROOT/ndk}" -name llvm-strip 2>/dev/null | head -n1 || true)}"

ABI="arm64-v8a"
OUT="$PKG_DIR/build"
APK="$OUT/AllGenURecomp-1.2-android-$ABI.apk"
KEYSTORE="${KEYSTORE:-$OUT/allgenu.keystore}"
KS_PASS="${KS_PASS:-allgenukey}"
KS_ALIAS="${KS_ALIAS:-allgenu}"

echo "== build-tools: $BUILD_TOOLS"
echo "== android.jar: $ANDROID_JAR"
echo "== strip      : ${NDK_STRIP:-<no encontrado, no se hace strip>}"

rm -rf "$OUT"; mkdir -p "$OUT/classes" "$OUT/inter"

# ---------- 1. Java -> .class ----------
echo "=== [1/7] javac"
find "$PKG_DIR/src" -name '*.java' > "$OUT/sources.txt"
# SDLActivity usa APIs modernas: --release 11 como en OceanTri
javac --release 11 -encoding UTF-8 -nowarn -cp "$ANDROID_JAR" -d "$OUT/classes" @"$OUT/sources.txt"

# ---------- 2. .class -> classes.dex ----------
echo "=== [2/7] d8"
find "$OUT/classes" -name '*.class' > "$OUT/classes.txt"
"$BUILD_TOOLS/d8" --release --min-api 29 --lib "$ANDROID_JAR" \
    --output "$OUT" $(cat "$OUT/classes.txt" | tr '\n' ' ')
# (d8 deja OUT/classes.dex)

# OJO: targetSdkVersion 29 a proposito.
#  * Con 30+ Android aplica "scoped storage" y la app YA NO puede leer
#    /sdcard/Download, donde el jugador pone su dump.
#  * Con 29 (y los permisos del manifiesto) si puede: se pide el permiso al
#    arrancar y el dump se lee de Download/ sin tocar nada mas.

# ---------- 3. recursos ----------
echo "=== [3/7] aapt2 compile"
"$BUILD_TOOLS/aapt2" compile --dir "$PKG_DIR/res" -o "$OUT/res.zip"

# ---------- 4. aapt2 link ----------
echo "=== [4/7] aapt2 link"
"$BUILD_TOOLS/aapt2" link \
    -o "$OUT/inter/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$PKG_DIR/AndroidManifest.xml" \
    --min-sdk-version 29 \
    --target-sdk-version 29 \
    --auto-add-overlay \
    "$OUT/res.zip"

# ---------- 5. dex + .so dentro del APK ----------
echo "=== [5/7] empaquetar dex y librerias nativas"
STAGE="$OUT/apkroot"
mkdir -p "$STAGE/lib/$ABI"

cp "$OUT/classes.dex" "$STAGE/classes.dex"

shopt -s nullglob
LIBS=("$PKG_DIR/libs/$ABI/"*.so)
[ ${#LIBS[@]} -gt 0 ] || { echo "No hay .so en $PKG_DIR/libs/$ABI (falta compilar libmain.so)"; exit 1; }
for so in "${LIBS[@]}"; do
    cp "$so" "$STAGE/lib/$ABI/"
    if [ -n "${NDK_STRIP:-}" ] && [ -x "$NDK_STRIP" ]; then
        "$NDK_STRIP" --strip-unneeded "$STAGE/lib/$ABI/$(basename "$so")" || true
    fi
done
echo "    libs: $(ls "$STAGE/lib/$ABI" | tr '\n' ' ')"

# OJO: las .so DEBEN ir sin comprimir (-0), si no zipalign no puede alinearlas
# a 16 KB y Android 15+ rechaza la instalacion.
( cd "$STAGE" && zip -q -X -0 "$OUT/inter/base.apk" lib/$ABI/*.so && zip -q -X "$OUT/inter/base.apk" classes.dex )

# ---------- 6. zipalign 16 KB (Android 15+) ----------
echo "=== [6/7] zipalign -P 16"
if [ -x "$BUILD_TOOLS/zipalign" ]; then
    "$BUILD_TOOLS/zipalign" -P 16 -f 4 "$OUT/inter/base.apk" "$OUT/inter/aligned.apk"
else
    # Termux no trae zipalign en build-tools: usar el emulador en Python
    python3 "$ROOT/tools/zipalign.py" -P 16 "$OUT/inter/base.apk" "$OUT/inter/aligned.apk"
fi
# Comprobacion: .so en offset multiplo de 16384 y sin comprimir
python3 "$ROOT/tools/zipalign.py" -c -P 16 "$OUT/inter/aligned.apk" || {
    echo "ALINEACION INCORRECTA"; exit 1; }

# ---------- 7. firmar ----------
echo "=== [7/7] apksigner"
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair -keystore "$KEYSTORE" -alias "$KS_ALIAS" \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass "$KS_PASS" -keypass "$KS_PASS" \
        -dname "CN=AllGenURecomp Android, O=KrisZtS, C=VE" >/dev/null
    echo "    keystore nuevo: $KEYSTORE (pass $KS_PASS)"
fi
"$BUILD_TOOLS/apksigner" sign \
    --ks "$KEYSTORE" --ks-key-alias "$KS_ALIAS" \
    --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
    --min-sdk-version 24 \
    --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true \
    --out "$APK" "$OUT/inter/aligned.apk"

"$BUILD_TOOLS/apksigner" verify --verbose --print-certs "$APK" | head -n 12
echo
echo "APK -> $APK ($(du -h "$APK" | cut -f1))"
