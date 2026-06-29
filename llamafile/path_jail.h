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

#ifndef LLAMAFILE_PATH_JAIL_H_
#define LLAMAFILE_PATH_JAIL_H_

// llamafile path jail (SECURITY) — canonicalization helper.
//
// The built-in server file tools confine every path they touch to a root
// directory ("--tools-root"). The live confinement lives in the llama.cpp
// server-tools patch (llama.cpp.patches/patches/tools_server_server-tools.cpp.patch,
// function `jail_resolve`). This header is the llamafile-owned, root-parameterized
// extraction of that exact algorithm so it can be unit-tested in isolation
// (tests/path_jail_test.cpp) and, ideally, included by the patch itself to remove
// the duplicated copy.
//
// Behavior: resolve `in` against `root` and canonicalize it (normalizing "."/".."
// and resolving symlinks for the portion of the path that exists). On success
// returns true with `out` set to the absolute canonical path that is guaranteed
// to be `root` itself or strictly below it. On escape or error returns false with
// a human-readable reason in `err`.

#include <filesystem>
#include <string>
#include <system_error>

namespace lf {

inline bool jail_resolve(const std::string & root_in,
                         const std::string & in,
                         std::string & out,
                         std::string & err) {
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::path(root_in), ec);
    if (ec || root.empty()) {
        root = fs::path(root_in);
    }

    fs::path p(in);
    if (p.is_relative()) {
        p = root / p;
    }

    ec.clear();
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec || canon.empty()) {
        canon = p.lexically_normal();
    }

    // containment: canon must be root itself or strictly below it
    const std::string rel = canon.lexically_relative(root).generic_string();
    const bool inside = !rel.empty()
                     && rel != ".."
                     && rel.rfind("../", 0) != 0; // does not start with "../"
    if (!inside) {
        err = "path '" + in + "' escapes the tools root (" + root.string() +
              "). Use a path inside the root, or start the server with --tools-root.";
        return false;
    }

    out = canon.string();
    return true;
}

} // namespace lf

#endif // LLAMAFILE_PATH_JAIL_H_
