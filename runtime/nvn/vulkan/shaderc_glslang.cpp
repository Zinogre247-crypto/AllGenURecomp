// [AllGenU-Android] shaderc de verdad, pero pequeno: glslang -> SPIR-V.
//
// runtime/nvn/vulkan/shaders.c solo usa 10 funciones de la API de shaderc, y
// todas son "compila este GLSL a SPIR-V con estas opciones". Compilar shaderc
// completo (con sus dependencias: glslang, SPIRV-Tools, SPIRV-Headers) son
// 10-15 minutos por intento en CI; aqui se implementa lo mismo apoyandose en
// glslang, que se compila aparte (libglslang.a) en el mismo job.
//
// Lo que NO hace: --target-env distinto de Vulkan 1.0 y optimizacion de SPIR-V
// (shaderc_optimization_level_performance cae al mismo camino que zero: se
// genera SPIR-V sin optimizar, que es valido y solo algo mas grande).
//
// Si algun dia quieres shaderc completo: quita este archivo del target y enlaza
// libshaderc_combined.a (google/shaderc, build con CMake).
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <shaderc/shaderc.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>


// La API de shaderc es C: si no se declara asi, C++ la "manglea" y el enlazador
// no encuentra simbolos como shaderc_compile_options_set_target_env.
extern "C" {

// ------------------------------------------------------------------ tipos
struct shaderc_compiler {
    int inited;  // glslang_initialize_process() hecho
};

struct shaderc_compile_options {
    shaderc_target_env env;
    shaderc_env_version env_version;
    shaderc_optimization_level opt;
};

struct shaderc_compilation_result {
    shaderc_compilation_status status;
    char *messages;   // error_message
    char *bytes;      // SPIR-V
    size_t length;
};

static bool g_process_inited;

// ------------------------------------------------------------------ compiler
shaderc_compiler_t shaderc_compiler_initialize(void) {
    if (!g_process_inited) {
        // (en glslang >= 12 el proceso se inicializa solo; en versiones viejas
        //  hay que llamarlo. __has_include no sirve para funciones: se usa el
        //  inicializador de glslang, que esta siempre).
        glslang::InitializeProcess();
        g_process_inited = 1;
    }
    shaderc_compiler_t c = (shaderc_compiler_t)calloc(1, sizeof *c);
    if (c) c->inited = 1;
    return c;
}

void shaderc_compiler_release(shaderc_compiler_t c) { free(c); }

// ------------------------------------------------------------------ options
shaderc_compile_options_t shaderc_compile_options_initialize(void) {
    shaderc_compile_options_t o = (shaderc_compile_options_t)calloc(1, sizeof *o);
    if (o) {
        o->env = shaderc_target_env_vulkan;
        o->env_version = shaderc_env_version_vulkan_1_0;
        o->opt = shaderc_optimization_level_zero;
    }
    return o;
}

shaderc_compile_options_t shaderc_compile_options_clone(const shaderc_compile_options_t src) {
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    if (o && src) *o = *src;
    return o;
}

void shaderc_compile_options_release(shaderc_compile_options_t o) { free(o); }

void shaderc_compile_options_set_target_env(shaderc_compile_options_t o, shaderc_target_env env,
                                           uint32_t v) {
    if (!o) return;
    o->env = env;
    o->env_version = (shaderc_env_version)v;
}

void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t o, shaderc_optimization_level l) {
    if (o) o->opt = l;
}

// (las opciones que no usamos: no-op, para que el enlazado no se queje si
//  alguna version de shaders.c las llama)
void shaderc_compile_options_add_macro_definition(shaderc_compile_options_t o, const char *n, size_t nl,
                                                 const char *v, size_t vl) {
    (void)o; (void)n; (void)nl; (void)v; (void)vl;
}
void shaderc_compile_options_set_source_language(shaderc_compile_options_t o, shaderc_source_language l) {
    (void)o; (void)l;
}
void shaderc_compile_options_set_generate_debug_info(shaderc_compile_options_t o) { (void)o; }
void shaderc_compile_options_set_suppress_warnings(shaderc_compile_options_t o) { (void)o; }

// ------------------------------------------------------------------ compile
static EShLanguage esh_of(shaderc_shader_kind k) {
    switch (k) {
    case shaderc_vertex_shader: return EShLangVertex;
    case shaderc_fragment_shader: return EShLangFragment;
    case shaderc_compute_shader: return EShLangCompute;
    case shaderc_geometry_shader: return EShLangGeometry;
    case shaderc_tess_control_shader: return EShLangTessControl;
    case shaderc_tess_evaluation_shader: return EShLangTessEvaluation;
    default: return EShLangVertex;
    }
}

static shaderc_compilation_result_t result_new(void) {
    return (shaderc_compilation_result_t)calloc(1, sizeof(struct shaderc_compilation_result));
}

shaderc_compilation_result_t shaderc_compile_into_spv(const shaderc_compiler_t compiler, const char *src,
                                                     size_t src_len, shaderc_shader_kind kind,
                                                     const char *input_name, const char *entry_point,
                                                     const shaderc_compile_options_t options) {
    (void)compiler; (void)options;
    shaderc_compilation_result_t r = result_new();
    if (!r) return NULL;
    EShLanguage lang = esh_of(kind);

    glslang::TShader shader(lang);
    const char *texts[1] = {src};
    const int lens[1] = {(int)src_len};
    shader.setStringsWithLengths(texts, lens, 1);
    shader.setEntryPoint(entry_point ? entry_point : "main");
    shader.setSourceEntryPoint(entry_point ? entry_point : "main");
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const TBuiltInResource *res = GetDefaultResources();
    if (!shader.parse(const_cast<TBuiltInResource *>(res), 100, false, EShMsgDefault)) {
        const char *m = shader.getInfoLog();
        r->status = shaderc_compilation_status_compilation_error;
        r->messages = strdup(m && *m ? m : "glslang: error de compilacion");
        return r;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        const char *m = program.getInfoLog();
        r->status = shaderc_compilation_status_compilation_error;
        r->messages = strdup(m && *m ? m : "glslang: error de enlace");
        return r;
    }

    std::vector<unsigned int> spv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spv);
    r->length = spv.size() * sizeof(unsigned int);
    if (r->length) {
        r->bytes = (char *)malloc(r->length);
        if (r->bytes) memcpy(r->bytes, spv.data(), r->length);
        else r->length = 0;
    }
    r->status = shaderc_compilation_status_success;
    (void)input_name;
    return r;
}

shaderc_compilation_status shaderc_result_get_compilation_status(const shaderc_compilation_result_t r) {
    return r ? r->status : shaderc_compilation_status_internal_error;
}

const char *shaderc_result_get_error_message(const shaderc_compilation_result_t r) {
    return (r && r->messages) ? r->messages : "";
}

size_t shaderc_result_get_length(const shaderc_compilation_result_t r) { return r ? r->length : 0; }

const char *shaderc_result_get_bytes(const shaderc_compilation_result_t r) {
    return (r && r->bytes) ? r->bytes : NULL;
}

void shaderc_result_release(shaderc_compilation_result_t r) {
    if (!r) return;
    free(r->messages);
    free(r->bytes);
    free(r);
}

}  // extern "C"
