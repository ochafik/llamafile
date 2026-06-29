// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2026 Mozilla.ai
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

//
// Unit test for the path-jail canonicalization helper (llamafile/path_jail.h),
// the llamafile-owned extraction of the server-tools `jail_resolve` security
// check. Builds a temp directory tree (an in-root file, a sub-dir, and a symlink
// pointing OUTSIDE the root) and asserts containment decisions:
//
//   * in-root relative + absolute paths           -> ALLOW
//   * "." and the root itself                      -> ALLOW
//   * ".." / nested "../.." escapes                -> REJECT
//   * an absolute path outside the root            -> REJECT
//   * a symlink whose target is outside the root   -> REJECT (symlinks resolved)
//
// Hermetic: the tree is created under a fresh mkdtemp and removed at the end.

#include "path_jail.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char *msg) {
    g_checks++;
    if (!cond) {
        g_fails++;
        fprintf(stderr, "FAIL: %s\n", msg);
    }
}

// Assert `in` is ALLOWED under `root` (and that out lands inside root).
static void allow(const std::string &root, const std::string &in, const char *msg) {
    std::string out, err;
    bool ok = lf::jail_resolve(root, in, out, err);
    if (!ok) fprintf(stderr, "  (unexpected reject: %s)\n", err.c_str());
    check(ok, msg);
}

// Assert `in` is REJECTED under `root`.
static void reject(const std::string &root, const std::string &in, const char *msg) {
    std::string out, err;
    bool ok = lf::jail_resolve(root, in, out, err);
    if (ok) fprintf(stderr, "  (unexpected allow -> %s)\n", out.c_str());
    check(!ok, msg);
}

int main(void) {
    printf("=== path-jail tests ===\n");

    char tmpl[] = "/tmp/pathjail_XXXXXX";
    char *base = mkdtemp(tmpl);
    if (!base) { perror("mkdtemp"); return 1; }

    fs::path root = fs::path(base) / "root";
    fs::path outside = fs::path(base) / "outside";
    std::error_code ec;
    fs::create_directories(root / "sub", ec);
    fs::create_directories(outside, ec);

    // an in-root file
    {
        FILE *f = fopen((root / "inside.txt").c_str(), "w");
        if (f) { fputs("hi", f); fclose(f); }
    }
    // an out-of-root file + a symlink inside root that points to it
    {
        FILE *f = fopen((outside / "secret.txt").c_str(), "w");
        if (f) { fputs("secret", f); fclose(f); }
    }
    fs::create_symlink(outside / "secret.txt", root / "escape_link", ec);

    const std::string r = root.string();

    // ---- ALLOW ----
    allow(r, "inside.txt",            "relative in-root file allowed");
    allow(r, "sub",                   "relative in-root subdir allowed");
    allow(r, "sub/../inside.txt",     "in-root path with normalized '..' allowed");
    allow(r, ".",                     "'.' (root itself) allowed");
    allow(r, (root / "inside.txt").string(), "absolute in-root path allowed");
    allow(r, "newfile.txt",           "not-yet-existing in-root path allowed (weakly canonical)");

    // ---- REJECT ----
    reject(r, "..",                   "'..' escapes root");
    reject(r, "../outside/secret.txt","'../outside/...' escapes root");
    reject(r, "../../etc/passwd",     "nested '../..' escapes root");
    reject(r, outside.string(),       "absolute path outside root rejected");
    reject(r, "/etc/passwd",          "absolute system path rejected");
    reject(r, "escape_link",          "symlink whose target is outside root rejected");

    // cleanup
    fs::remove_all(base, ec);

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails) {
        printf("PATH-JAIL TESTS FAILED\n");
        return 1;
    }
    printf("ALL PATH-JAIL TESTS PASSED\n");
    return 0;
}
