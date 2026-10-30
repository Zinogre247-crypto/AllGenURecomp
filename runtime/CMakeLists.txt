# =====================================================================
#  Build nativo de AllGenURecomp para Android arm64-v8a.
#  Alternativa al Makefile (que es de Windows/Linux de escritorio).
#
#  Compila UN solo libmain.so con:
#    - runtime/            (28.8k lineas de C/C++)
#    - generated/          (el C del juego, salida de `make gen`)
#    - runtime/platform/android.c + android_jni.c
#
#  Uso (desde la raiz del repo, en GitHub Actions o PC Linux):
#    cmake -S runtime -B build-android -G Ninja \
#          -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
#          -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
#          -DCMAKE_BUILD_TYPE=Release
#    cmake --build build-android --target main -j
# =====================================================================
cmake_minimum_required(VERSION 3.22)
project(allgenu_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------- opciones
# ARMv8.2-A: equivalente arm64 de -march=x86-64-v3 (el Makefile usa v3 en x86).
set(GENARCH "-march=armv8.2-a+fp16+dotprod" CACHE STRING "flags para el codigo generado")
# El C generado se compila a -O1 en el Makefile (devuelve basura en ramas sin
# return a -O2+): se mantiene igual.
set(GENOPT "-O1" CACHE STRING "optimizacion del codigo generado")
# Sin shaderc en Android: usar shaderc del sistema si existe, o SPIR-V precompilado.
option(USE_SYSTEM_SHADERC "enlazar libshaderc_shared del NDK/SDK" OFF)
option(BUILD_GENERATED "compilar generated/ (necesita `make gen` antes)" ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(GEN "${ROOT}/generated")

# ---------------------------------------------------------------- fuentes runtime
file(GLOB RT_C   "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
                 "${CMAKE_CURRENT_SOURCE_DIR}/hle/*.c"
                 "${CMAKE_CURRENT_SOURCE_DIR}/services/*.c"
                 "${CMAKE_CURRENT_SOURCE_DIR}/net/*.c")
set(RT_C ${RT_C}
    "${CMAKE_CURRENT_SOURCE_DIR}/nvn/objects.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/nvn/cmdbuf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/nvn/nvn_args.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/nvn/tegra.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/nvn/formats.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/nvn/backend_null.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/nvn/astc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/nvn/maxwell/translate.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/android.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/android_jni.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/android_touch.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/android_touch_ui.cpp")
file(GLOB RT_VK "${CMAKE_CURRENT_SOURCE_DIR}/nvn/vulkan/*.c")
list(APPEND RT_C ${RT_VK})

# main.c se compila con -Dmain=allgenu_main (ver android_jni.c)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    PROPERTIES COMPILE_DEFINITIONS "main=allgenu_main")

file(GLOB RT_CPP "${CMAKE_CURRENT_SOURCE_DIR}/overlay.cpp"
                 "${CMAKE_CURRENT_SOURCE_DIR}/kzmui.cpp"
                 "${CMAKE_CURRENT_SOURCE_DIR}/forge_imgui.c")

# Dear ImGui (C API de cimgui) — igual que el Makefile
set(IMGUI "${ROOT}/third_party/cimgui")
file(GLOB IMGUI_SRC "${IMGUI}/cimgui.cpp"
    "${IMGUI}/imgui/imgui.cpp" "${IMGUI}/imgui/imgui_draw.cpp"
    "${IMGUI}/imgui/imgui_tables.cpp" "${IMGUI}/imgui/imgui_widgets.cpp"
    "${IMGUI}/imgui/imgui_demo.cpp"
    "${IMGUI}/imgui/backends/imgui_impl_vulkan.cpp")

# ---------------------------------------------------------------- codigo del juego
if(BUILD_GENERATED)
    file(GLOB GEN_C "${GEN}/main/*.c" "${GEN}/sdk/*.c")
    if(NOT GEN_C)
        message(WARNING "generated/ vacio: ejecuta `make gen GAME=<tu dump>` antes. Se compila solo el runtime.")
        set(BUILD_GENERATED OFF)
    endif()
endif()

# ---------------------------------------------------------------- target
add_library(main SHARED
    ${RT_C} ${RT_CPP} ${IMGUI_SRC}
    $<$<BOOL:${BUILD_GENERATED}>:${GEN_C}>
    "${ROOT}/tools/gamelib_entry.c"
)

# Aviso: los .c generados son miles y -O1 con -fno-strict-aliasing es lo que usa
# el Makefile. Sin estas flags el C generado puede romperse.
set_source_files_properties(${GEN_C} PROPERTIES COMPILE_OPTIONS
    "${GENOPT};-fno-strict-aliasing;-ffp-contract=off;-fno-math-errno;-Wno-return-type;-Wno-unused-label;-Wno-parentheses-equality;-Wno-unused-value;-fvisibility=hidden;${GENARCH}")

# SDL3: el NDK no lo trae. Se pasa la carpeta include (del .aar de SDL) por
# -DSDL3_INCLUDE_DIR=... y se enlaza contra el libSDL3.so prebuilt.
set(SDL3_INCLUDE_DIR "" CACHE PATH "include de SDL3 (prefab/modules/SDL3-Headers/include)")
set(SDL3_LIB_DIR "" CACHE PATH "carpeta que contiene libSDL3.so")
find_library(SDL3_LIB NAMES SDL3 SDL3_shared libSDL3
    PATHS "${SDL3_LIB_DIR}"
          "${CMAKE_SOURCE_DIR}/packaging/android/libs/${ANDROID_ABI}"
          "${CMAKE_SOURCE_DIR}/../packaging/android/libs/${ANDROID_ABI}"
    NO_DEFAULT_PATH)
if(NOT SDL3_LIB)
    message(FATAL_ERROR "No encuentro libSDL3.so. Pasa -DSDL3_LIB=/ruta/libSDL3.so "
                        "(o -DSDL3_LIB_DIR=carpeta). Contenido esperado en packaging/android/libs/${ANDROID_ABI}")
endif()
message(STATUS "SDL3: ${SDL3_LIB}")

# El codigo generado de subsdk0 incluye "subsdk0/runtime/hle/hle.h": se crea un
# prefijo que apunta a runtime/ para que resuelva.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/geninc/subsdk0")
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/geninc/subsdk0/runtime")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/geninc/subsdk0/runtime")
endif()

target_include_directories(main PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/geninc"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${IMGUI}" "${IMGUI}/imgui" "${IMGUI}/imgui/backends")
# Terceros como SYSTEM: sus warnings no ensucian el log (el log limpio es lo que
# permite ver el error de verdad cuando algo falla).
target_include_directories(main SYSTEM PRIVATE
    ${SDL3_INCLUDE_DIR}
    ${FFMPEG_INCLUDE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/vma"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cimgui"
    ${GLSLANG_INCLUDE_DIR} ${SHADERC_INCLUDE_DIR} ${SPIRV_HEADERS_DIR})

target_compile_definitions(main PRIVATE
    __ANDROID__=1
    _FILE_OFFSET_BITS=64
    MHGU_ANDROID=1)

target_compile_options(main PRIVATE
    -fPIC -O3 -ffunction-sections -fdata-sections
    -Wno-unused-parameter -Wno-sign-compare
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-nullability-completeness>)

target_link_options(main PRIVATE
    -Wl,--gc-sections
    -Wl,-z,max-page-size=16384     # Android 15+: paginas de 16 KB
    -Wl,--build-id=sha1)

# Orden de busqueda: primero las libs del NDK (SDL3, vulkan) y luego las del sistema
target_link_libraries(main PRIVATE
    ${SDL3_LIB}
    vulkan
    android
    log
    dl
    m
    OpenSLES)

if(USE_SYSTEM_SHADERC)
    target_link_libraries(main PRIVATE shaderc_shared)
endif()

# ---------------------------------------------------------------- cine (FFmpeg)
# movie.c usa libavformat/libavcodec. Compilar FFmpeg para arm64 en CI son
# 30-60 min extra, asi que por defecto se usan STUBS: el juego arranca pero las
# cinematicas no se reproducen (movie:: devuelve error y sigue).
#   -DBUILD_FFMPEG_STUBS=OFF -DFFMPEG_INCLUDE_DIR=... -DFFMPEG_LIB_DIR=...
# para tener video de verdad.
# glslang: compila GLSL -> SPIR-V (lo usa shaderc_glslang.cpp, que suple a shaderc).
set(GLSLANG_INCLUDE_DIR "" CACHE PATH "carpeta que contiene glslang/Public/ShaderLang.h")
set(SHADERC_INCLUDE_DIR "" CACHE PATH "carpeta que contiene shaderc/shaderc.h")
set(SPIRV_HEADERS_DIR "" CACHE PATH "carpeta que contiene spirv/unified1")
set(GLSLANG_LIB_DIR "" CACHE PATH "carpeta con libglslang.a ...")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nvn/vulkan/shaderc_glslang.cpp")
    target_sources(main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/nvn/vulkan/shaderc_glslang.cpp")
    target_include_directories(main PRIVATE "${GLSLANG_INCLUDE_DIR}" "${SHADERC_INCLUDE_DIR}" "${SPIRV_HEADERS_DIR}")
    if(GLSLANG_LIB_DIR)
        target_link_directories(main PRIVATE "${GLSLANG_LIB_DIR}" "${GLSLANG_LIB_DIR}/glslang"
                                "${GLSLANG_LIB_DIR}/glslang/OSDependent/Unix" "${GLSLANG_LIB_DIR}/SPIRV")
        # (--start-group: estas libs se referencian entre si en cualquier orden)
        target_link_libraries(main PRIVATE
            -Wl,--start-group glslang SPIRV MachineIndependent OSDependent GenericCodeGen
            glslang-default-resource-limits -Wl,--end-group)
        message(STATUS "shaderc: glslang en ${GLSLANG_LIB_DIR}")
    else()
        message(FATAL_ERROR "Falta -DGLSLANG_LIB_DIR=... (libglslang.a compilada para Android)")
    endif()
endif()

# VulkanMemoryAllocator: header-only en third_party/vma; su implementacion va en
# un .c aparte (vma_impl.c) para que no se dupliquen simbolos.
foreach(_vma_impl nvn/vulkan/vma_impl.cpp nvn/vulkan/vma_impl.c)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_vma_impl}")
        target_sources(main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${_vma_impl}")
        message(STATUS "VMA: ${_vma_impl}")
    endif()
endforeach()

option(BUILD_FFMPEG_STUBS "usar stubs de FFmpeg (sin cinematicas)" ON)

if(BUILD_FFMPEG_STUBS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hle/movie_stubs.c")
        message(FATAL_ERROR "Falta runtime/hle/movie_stubs.c (lo copia apply-patch.sh). "
                            "Si quieres video real: -DBUILD_FFMPEG_STUBS=OFF con FFmpeg compilado.")
    endif()
    target_sources(main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/hle/movie_stubs.c")
    if(FFMPEG_INCLUDE_DIR)
        target_include_directories(main PRIVATE "${FFMPEG_INCLUDE_DIR}")
    endif()
else()
    target_include_directories(main PRIVATE "${FFMPEG_INCLUDE_DIR}")
    target_link_directories(main PRIVATE "${FFMPEG_LIB_DIR}")
    target_link_libraries(main PRIVATE avformat avcodec avutil)
endif()

set_target_properties(main PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "main"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/arm64-v8a")
