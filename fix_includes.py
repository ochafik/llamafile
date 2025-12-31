#!/usr/bin/env python3
"""Replace relative include paths with full paths in llama.cpp directory."""
import os
import re
import subprocess
from pathlib import Path

# Project root
LLAMAFILE_ROOT = Path("/Users/ochafik/github/llamafile-upgrade")
LLAMACPP_DIR = LLAMAFILE_ROOT / "llama.cpp"

# Mapping of include prefixes to their full paths
INCLUDE_MAPPINGS = {
    "llama.h": "llama.cpp/include/llama.h",
    "llama-grammar.h": "llama.cpp/include/llama-grammar.h",
    "llama-cpp.h": "llama.cpp/include/llama-cpp.h",
    "ggml.h": "llama.cpp/ggml/include/ggml.h",
    "ggml-alloc.h": "llama.cpp/ggml/include/ggml-alloc.h",
    "ggml-backend.h": "llama.cpp/ggml/include/ggml-backend.h",
    "ggml-opt.h": "llama.cpp/ggml/include/ggml-opt.h",
    "ggml-cpu.h": "llama.cpp/ggml/include/ggml-cpu.h",
    "gguf.h": "llama.cpp/ggml/include/gguf.h",
    "ggml-common.h": "llama.cpp/ggml/src/ggml-common.h",
    "ggml-impl.h": "llama.cpp/ggml/src/ggml-impl.h",
    "ggml-quants.h": "llama.cpp/ggml/src/ggml-quants.h",
    "ggml-cpu-impl.h": "llama.cpp/ggml/src/ggml-cpu/ggml-cpu-impl.h",
    "simd-mappings.h": "llama.cpp/ggml/src/ggml-cpu/simd-mappings.h",
    "kernels.h": "llama.cpp/ggml/src/ggml-cpu/kernels.h",
    "common.h": "llama.cpp/common/common.h",
    "log.h": "llama.cpp/common/log.h",
    "sampling.h": "llama.cpp/common/sampling.h",
    "tokenizer.h": "llama.cpp/common/tokenizer.h",
    "console.h": "llama.cpp/common/console.h",
    "arg.h": "llama.cpp/common/arg.h",
    "json.hpp": "llama.cpp/vendor/nlohmann/json.hpp",
    "json_fwd.hpp": "llama.cpp/vendor/nlohmann/json_fwd.hpp",
}

def find_file(name, search_dirs):
    """Find a file in the given search directories."""
    for search_dir in search_dirs:
        path = search_dir / name
        if path.exists() and path.is_file():
            return path
    return None

def get_full_path(include_name, current_file):
    """Get the full path for an include statement."""
    # Check if it's a system include (<...>)
    if include_name.startswith('<') and include_name.endswith('>'):
        return None  # Skip system includes
    
    # Remove quotes
    name = include_name.strip('"\'')
    
    # Check direct mappings first
    if name in INCLUDE_MAPPINGS:
        return INCLUDE_MAPPINGS[name]
    
    # Try to find the file relative to llama.cpp
    search_dirs = [
        LLAMACPP_DIR / "include",
        LLAMACPP_DIR / "src",
        LLAMACPP_DIR / "common",
        LLAMACPP_DIR / "ggml" / "include",
        LLAMACPP_DIR / "ggml" / "src",
        LLAMACPP_DIR / "ggml" / "src" / "ggml-cpu",
        LLAMACPP_DIR / "vendor",
        LLAMACPP_DIR / "tools" / "mtmd",
    ]
    
    found = find_file(name, search_dirs)
    if found:
        # Return relative to project root
        return str(found.relative_to(LLAMAFILE_ROOT))
    
    # Try with common subdirectories
    for subdir in ["src", "include", "common", "ggml/include", "ggml/src", "vendor"]:
        found = find_file(name, [LLAMACPP_DIR / subdir])
        if found:
            return str(found.relative_to(LLAMAFILE_ROOT))
    
    return None

def fix_includes_in_file(filepath):
    """Fix all includes in a single file."""
    filepath = Path(filepath)
    if not filepath.is_file():
        return False
    content = filepath.read_text()
    original_content = content
    
    # Match both #include "..." and #include <...>
    def replace_include(match):
        include_type_start = match.group(1)  # " or <
        include_name = match.group(2)  # the filename
        include_type_end = match.group(3)  # " or >
        
        full_path = get_full_path(include_name, filepath)
        if full_path:
            return f'#include {include_type_start}{full_path}{include_type_end}'
        return match.group(0)
    
    # Replace includes
    content = re.sub(r'#include\s+([<"])([^>"]+)([>"])', replace_include, content)
    
    if content != original_content:
        filepath.write_text(content)
        return True
    return False

def main():
    """Process all source files in llama.cpp."""
    count = 0
    for ext in ['*.c', '*.cpp', '*.h', '*.hpp']:
        for filepath in LLAMACPP_DIR.rglob(ext):
            if fix_includes_in_file(filepath):
                print(f"Fixed: {filepath.relative_to(LLAMAFILE_ROOT)}")
                count += 1
    print(f"\nTotal files modified: {count}")

if __name__ == "__main__":
    main()
