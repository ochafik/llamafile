// -*- mode:c;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=c ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2023 Mozilla Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "llama.cpp/ggml/include/ggml-metal.h"
#include "llamafile.h"
#include "log.h"
#include <assert.h>
#include <cosmo.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// [llamafile] Define struct removed from upstream llama.cpp
struct ggml_metal_device_properties {
    char name[256];
    float memory;
    int core_count;
    int metal_version;
    int gpu_family;
    int gpu_family_common;
};

// Core headers from native llama.cpp structure
__static_yoink("llama.cpp/ggml/include/ggml.h");
__static_yoink("llama.cpp/ggml/include/ggml-alloc.h");
__static_yoink("llama.cpp/ggml/include/ggml-backend.h");
__static_yoink("llama.cpp/ggml/include/ggml-metal.h");
__static_yoink("llama.cpp/ggml/include/gguf.h");
__static_yoink("llama.cpp/ggml/src/ggml-impl.h");
__static_yoink("llama.cpp/ggml/src/ggml-backend-impl.h");
__static_yoink("llama.cpp/ggml/src/ggml-common.h");
__static_yoink("llama.cpp/ggml/src/ggml-quants.h");
__static_yoink("llamafile/llamafile.h");
// Metal-specific sources (simplified in new llama.cpp)
__static_yoink("llama.cpp/ggml/src/ggml-metal/ggml-metal.m");
__static_yoink("llama.cpp/ggml/src/ggml-metal/ggml-metal-impl.h");
__static_yoink("llama.cpp/ggml/src/ggml-metal/ggml-metal.metal");

static const struct Source {
    const char *zip;
    const char *name;
} srcs[] = {
    // All files extracted preserving llama.cpp/ path structure for include compatibility
    // Core headers
    {"/zip/llama.cpp/ggml/include/ggml.h", "llama.cpp/ggml/include/ggml.h"},
    {"/zip/llama.cpp/ggml/include/ggml-alloc.h", "llama.cpp/ggml/include/ggml-alloc.h"},
    {"/zip/llama.cpp/ggml/include/ggml-backend.h", "llama.cpp/ggml/include/ggml-backend.h"},
    {"/zip/llama.cpp/ggml/include/ggml-metal.h", "llama.cpp/ggml/include/ggml-metal.h"},
    {"/zip/llama.cpp/ggml/include/gguf.h", "llama.cpp/ggml/include/gguf.h"},
    // Internal headers
    {"/zip/llama.cpp/ggml/src/ggml-impl.h", "llama.cpp/ggml/src/ggml-impl.h"},
    {"/zip/llama.cpp/ggml/src/ggml-backend-impl.h", "llama.cpp/ggml/src/ggml-backend-impl.h"},
    {"/zip/llama.cpp/ggml/src/ggml-common.h", "llama.cpp/ggml/src/ggml-common.h"},
    {"/zip/llama.cpp/ggml/src/ggml-quants.h", "llama.cpp/ggml/src/ggml-quants.h"},
    {"/zip/llamafile/llamafile.h", "llamafile/llamafile.h"},
    // Metal files (simplified in new llama.cpp - single .m file + header + shaders)
    {"/zip/llama.cpp/ggml/src/ggml-metal/ggml-metal-impl.h", "llama.cpp/ggml/src/ggml-metal/ggml-metal-impl.h"},
    {"/zip/llama.cpp/ggml/src/ggml-metal/ggml-metal.metal", "llama.cpp/ggml/src/ggml-metal/ggml-metal.metal"},
    // Metal source (compiled into .dylib)
    {"/zip/llama.cpp/ggml/src/ggml-metal/ggml-metal.m", "llama.cpp/ggml/src/ggml-metal/ggml-metal.m"},
};

static struct Metal {
    bool supported;
    atomic_uint once;
    typeof(ggml_backend_metal_init) *backend_init;
    typeof(ggml_backend_is_metal) *backend_is_metal;
    typeof(ggml_backend_metal_set_abort_callback) *set_abort_callback;
    typeof(ggml_backend_metal_reg) *reg;  // Returns ggml_backend_reg_t
    typeof(ggml_backend_metal_supports_family) *supports_family;
} ggml_metal;

static const char *Dlerror(void) {
    const char *msg;
    msg = cosmo_dlerror();
    if (!msg)
        msg = "null dlopen error";
    return msg;
}

static bool FileExists(const char *path) {
    struct stat st;
    return !stat(path, &st);
}

// Helper to create directory from path (creates parent dirs as needed)
static bool EnsureParentDir(const char *path) {
    char dir[PATH_MAX];
    strlcpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (makedirs(dir, 0755)) {
            perror(dir);
            return false;
        }
    }
    return true;
}

// Check file extension
static bool IsCppFile(const char *name) {
    size_t len = strlen(name);
    return (len > 4 && strcmp(name + len - 4, ".cpp") == 0);
}

static bool IsObjCFile(const char *name) {
    size_t len = strlen(name);
    return (len > 2 && strcmp(name + len - 2, ".m") == 0);
}

static bool IsSourceFile(const char *name) {
    return IsCppFile(name) || IsObjCFile(name);
}

// Compile a single source file to an object file
static bool CompileSourceFile(const char *src, const char *obj, const char *basedir,
                              bool is_cpp) {
    // Build include paths for the various source locations
    char incl_base[PATH_MAX], incl_ggml_include[PATH_MAX], incl_ggml_src[PATH_MAX];
    char incl_metal[PATH_MAX];
    snprintf(incl_base, sizeof(incl_base), "-I%s", basedir);
    snprintf(incl_ggml_include, sizeof(incl_ggml_include), "-I%sllama.cpp/ggml/include", basedir);
    snprintf(incl_ggml_src, sizeof(incl_ggml_src), "-I%sllama.cpp/ggml/src", basedir);
    snprintf(incl_metal, sizeof(incl_metal), "-I%sllama.cpp/ggml/src/ggml-metal", basedir);

    char *args[30];
    int argc = 0;

    args[argc++] = is_cpp ? "clang++" : "clang";
    args[argc++] = incl_base;
    args[argc++] = incl_ggml_include;
    args[argc++] = incl_ggml_src;
    args[argc++] = incl_metal;
    args[argc++] = "-O3";
    args[argc++] = "-fPIC";
    args[argc++] = "-c";
    if (is_cpp) {
        args[argc++] = "-std=c++17";
    }
    args[argc++] = "-DNDEBUG";
    args[argc++] = "-ffixed-x28";  // cosmo's tls register
    args[argc++] = "-DTARGET_OS_OSX";
    args[argc++] = "-DGGML_MULTIPLATFORM";
    args[argc++] = (char *)src;
    args[argc++] = "-o";
    args[argc++] = (char *)obj;
    args[argc] = NULL;

    int pid, ws;
    llamafile_log_command(args);
    errno_t err = posix_spawnp(&pid, args[0], NULL, NULL, args, environ);
    if (err) {
        perror(args[0]);
        return false;
    }
    while (waitpid(pid, &ws, 0) == -1) {
        if (errno != EINTR) {
            perror(args[0]);
            return false;
        }
    }
    return ws == 0;
}

static bool BuildMetal(const char *dso) {
    char basedir[PATH_MAX];
    llamafile_get_app_dir(basedir, PATH_MAX);

    // Create base directory
    if (makedirs(basedir, 0755)) {
        perror(basedir);
        return false;
    }

    // extract source code and track source files
    char src[PATH_MAX];
    char srcfiles[16][PATH_MAX];  // Array to hold source file paths
    int nsrcfiles = 0;
    bool needs_rebuild = false;

    for (int i = 0; i < sizeof(srcs) / sizeof(*srcs); ++i) {
        strlcpy(src, basedir, sizeof(src));
        strlcat(src, srcs[i].name, sizeof(src));

        // Create parent directory for file
        if (!EnsureParentDir(src)) {
            return false;
        }

        switch (llamafile_is_file_newer_than(srcs[i].zip, src)) {
        case -1:
            return false;
        case 0:
            break;
        case 1:
            needs_rebuild = true;
            if (!llamafile_extract(srcs[i].zip, src)) {
                return false;
            }
            break;
        default:
            __builtin_unreachable();
        }

        // Track source files for compilation
        if (IsSourceFile(srcs[i].name) && nsrcfiles < 16) {
            strlcpy(srcfiles[nsrcfiles++], src, PATH_MAX);
        }
    }

    // determine if we need to build (check last source file against dso)
    if (!needs_rebuild && nsrcfiles > 0) {
        switch (llamafile_is_file_newer_than(srcfiles[nsrcfiles - 1], dso)) {
        case -1:
            return false;
        case 0:
            break;
        case 1:
            needs_rebuild = true;
            break;
        default:
            __builtin_unreachable();
        }
    }

    // compile dynamic shared object
    if (needs_rebuild || FLAG_recompile) {
        tinylog("building ggml-metal.dylib with xcode...\n", NULL);
        int fd;
        char tmpdso[PATH_MAX];
        strlcpy(tmpdso, dso, sizeof(tmpdso));
        strlcat(tmpdso, ".XXXXXX", sizeof(tmpdso));
        if ((fd = mkostemp(tmpdso, O_CLOEXEC)) != -1) {
            close(fd);
        } else {
            perror(tmpdso);
            return false;
        }

        // Compile each source file to an object file
        char objfiles[16][PATH_MAX];
        int nobjfiles = 0;

        for (int i = 0; i < nsrcfiles; ++i) {
            // Generate object file path
            snprintf(objfiles[nobjfiles], PATH_MAX, "%s.o", srcfiles[i]);

            bool is_cpp = IsCppFile(srcfiles[i]);
            if (!CompileSourceFile(srcfiles[i], objfiles[nobjfiles], basedir, is_cpp)) {
                tinylog("failed to compile: ", srcfiles[i], "\n", NULL);
                return false;
            }
            nobjfiles++;
        }

        // Link all object files into dylib
        char *args[40];
        int argc = 0;
        args[argc++] = "clang++";
        args[argc++] = "-shared";
        args[argc++] = "-pthread";
        args[argc++] = "-ffixed-x28";
        // Allow undefined symbols - they'll be resolved at runtime from main binary
        args[argc++] = "-undefined";
        args[argc++] = "dynamic_lookup";

        // Add all object files
        for (int i = 0; i < nobjfiles; ++i) {
            args[argc++] = objfiles[i];
        }

        args[argc++] = "-o";
        args[argc++] = tmpdso;
        args[argc++] = "-framework";
        args[argc++] = "Foundation";
        args[argc++] = "-framework";
        args[argc++] = "Metal";
        args[argc++] = "-framework";
        args[argc++] = "MetalKit";
        args[argc] = NULL;

        int pid, ws;
        llamafile_log_command(args);
        errno_t err = posix_spawnp(&pid, "clang++", NULL, NULL, args, environ);
        if (err) {
            perror("clang++");
            if (err == ENOENT) {
                tinylog("PLEASE RUN: xcode-select --install\n", NULL);
            }
            return false;
        }
        while (waitpid(pid, &ws, 0) == -1) {
            if (errno != EINTR) {
                perror("clang++");
                return false;
            }
        }
        if (ws) {
            tinylog("linker returned nonzero exit status\n", NULL);
            return false;
        }
        if (rename(tmpdso, dso)) {
            perror(dso);
            return false;
        }

        // Clean up object files
        for (int i = 0; i < nobjfiles; ++i) {
            unlink(objfiles[i]);
        }
    }

    return true;
}

static bool LinkMetal(const char *dso) {

    // runtime link dynamic shared object
    void *lib;
    lib = cosmo_dlopen(dso, RTLD_LAZY);
    if (!lib) {
        tinylog(Dlerror(), ": failed to load library\n", NULL);
        return false;
    }

    // import functions
    bool ok = true;
    // ok &= !!(ggml_metal.ggml_metal_link = cosmo_dlsym(lib, "ggml_metal_link"));  // Removed: API changed
    ok &= !!(ggml_metal.backend_init = cosmo_dlsym(lib, "ggml_backend_metal_init"));
    // ok &= !!(ggml_metal.backend_buffer_type = cosmo_dlsym(lib, "ggml_backend_metal_buffer_type"));  // Removed: API changed
    // ok &= !!(ggml_metal.backend_buffer_from_ptr = cosmo_dlsym(lib, "ggml_backend_metal_buffer_from_ptr"));  // Removed: API changed
    ok &= !!(ggml_metal.backend_is_metal = cosmo_dlsym(lib, "ggml_backend_is_metal"));
    // ok &= !!(ggml_metal.backend_set_n_cb = cosmo_dlsym(lib, "ggml_backend_metal_set_n_cb"));  // Removed: API changed
    ok &= !!(ggml_metal.set_abort_callback = cosmo_dlsym(lib, "ggml_backend_metal_set_abort_callback"));  // Changed function name
    ok &= !!(ggml_metal.reg = cosmo_dlsym(lib, "ggml_backend_metal_reg"));
    // ok &= !!(ggml_metal.get_device_properties = cosmo_dlsym(lib, "ggml_backend_metal_get_device_properties"));  // Removed: API changed
    // ok &= !!(ggml_metal.get_device_memory_usage = cosmo_dlsym(lib, "ggml_backend_metal_get_device_memory_usage"));  // Removed: API changed
    ok &= !!(ggml_metal.supports_family = cosmo_dlsym(lib, "ggml_backend_metal_supports_family"));
    if (!ok) {
        tinylog(Dlerror(), ": not all symbols could be imported\n", NULL);
        return false;
    }

    // we're good - no more ggml_backend_api() call needed
    return true;
}

static bool ImportMetalImpl(void) {

    // Ensure this is MacOS ARM64.
    if (!IsXnuSilicon()) {
        return false;
    }

    // Check if we're allowed to even try.
    switch (FLAG_gpu) {
    case LLAMAFILE_GPU_AUTO:
    case LLAMAFILE_GPU_APPLE:
        break;
    default:
        return false;
    }

    npassert(FLAGS_READY);

    // Get path of DSO.
    char dso[PATH_MAX];
    llamafile_get_app_dir(dso, PATH_MAX);
    strlcat(dso, "ggml-metal.dylib", sizeof(dso));
    if (FLAG_nocompile) {
        return LinkMetal(dso);
    }

    // Build and link Metal support DSO if possible.
    if (BuildMetal(dso)) {
        return LinkMetal(dso);
    } else {
        return false;
    }
}

static void ImportMetal(void) {
    if (ImportMetalImpl()) {
        ggml_metal.supported = true;
        tinylog("Apple Metal GPU support successfully loaded\n", NULL);
    } else if (FLAG_gpu == LLAMAFILE_GPU_APPLE) {
        tinyprint(2, "fatal error: support for --gpu ", llamafile_describe_gpu(),
                  FLAG_tinyblas ? " --tinyblas" : "",
                  " was explicitly requested, but it wasn't available\n", NULL);
        exit(1);
    }
}

bool llamafile_has_metal(void) {
    cosmo_once(&ggml_metal.once, ImportMetal);
    return ggml_metal.supported;
}

ggml_backend_t ggml_backend_metal_init(void) {
    if (!llamafile_has_metal())
        return 0;
    return ggml_metal.backend_init();
}

// Removed: ggml_backend_metal_buffer_type no longer exists
// Use ggml_backend_get_default_buffer_type(backend) instead
// GGML_CALL ggml_backend_buffer_type_t ggml_backend_metal_buffer_type(void) {
//     if (!llamafile_has_metal())
//         return 0;
//     return ggml_metal.backend_buffer_type();
// }

// Removed: ggml_backend_metal_buffer_from_ptr no longer exists
// Use ggml_backend_cpu_buffer_from_ptr instead
// GGML_CALL ggml_backend_buffer_t ggml_backend_metal_buffer_from_ptr(void *data, size_t size,
//                                                                    size_t max_size) {
//     if (!llamafile_has_metal())
//         return 0;
//     return ggml_metal.backend_buffer_from_ptr(data, size, max_size);
// }

bool ggml_backend_is_metal(ggml_backend_t backend) {
    if (!llamafile_has_metal())
        return 0;
    return ggml_metal.backend_is_metal(backend);
}

// Removed: ggml_backend_metal_set_n_cb no longer exists
// void ggml_backend_metal_set_n_cb(ggml_backend_t backend, int n_cb) {
//     if (!llamafile_has_metal())
//         return;
//     return ggml_metal.backend_set_n_cb(backend, n_cb);
// }

// Changed: ggml_backend_metal_log_set_callback → ggml_backend_metal_set_abort_callback
void ggml_backend_metal_set_abort_callback(ggml_backend_t backend, ggml_abort_callback abort_callback, void *user_data) {
    if (!llamafile_has_metal())
        return;
    return ggml_metal.set_abort_callback(backend, abort_callback, user_data);
}

ggml_backend_reg_t ggml_backend_metal_reg(void) {
    if (!llamafile_has_metal())
        return 0;
    return ggml_metal.reg();
}

// [llamafile] Stub implementation - metal properties are collected differently in macOS
void ggml_backend_metal_get_device_properties(ggml_backend_t backend, struct ggml_metal_device_properties *properties) {
    if (!properties)
        return;
    memset(properties, 0, sizeof(*properties));
    if (!llamafile_has_metal())
        return;
    // Properties are typically filled in by system_profiler on macOS
    // This is a stub that localscore will populate via other means
    strncpy(properties->name, "Apple Metal", sizeof(properties->name) - 1);
}

// Removed: ggml_backend_metal_get_device_memory_usage no longer exists
// void ggml_backend_metal_get_device_memory_usage(ggml_backend_t backend, float *used, float *total) {
//     if (!llamafile_has_metal())
//         return;
//     return ggml_metal.get_device_memory_usage(backend, used, total);
// }

bool ggml_backend_metal_supports_family(ggml_backend_t backend, int family) {
    if (!llamafile_has_metal())
        return 0;
    return ggml_metal.supports_family(backend, family);
}
