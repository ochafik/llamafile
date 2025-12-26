// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2024 Mozilla Foundation
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

#pragma once

// Forward declare the C struct from llamafile/zim/zim.h
struct zim_archive;

namespace lf {
namespace server {

// Initialize ZIM archive from file path
// Returns true on success
bool zim_init(const char* path);

// Close ZIM archive and free resources
void zim_shutdown();

// Check if a ZIM archive is loaded
bool zim_is_loaded();

// Get the global ZIM archive pointer (for tool calling)
// Returns pointer to C zim_archive struct
::zim_archive* zim_get_archive();

} // namespace server
} // namespace lf
