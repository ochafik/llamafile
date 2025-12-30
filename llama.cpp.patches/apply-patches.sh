#!/bin/bash
# Apply llamafile patches to llama.cpp submodule
# NATIVE STRUCTURE VERSION - No flattening!

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLAMA_DIR="$SCRIPT_DIR/../llama.cpp"
PATCHES_DIR="$SCRIPT_DIR/patches"
LLAMAFILE_FILES_DIR="$SCRIPT_DIR/llamafile-files"

cd "$LLAMA_DIR"

# Check if status is dirty, if so, exit
if [ -n "$(git status --porcelain)" ]; then
    echo "Git status is dirty. Please commit or stash your changes before applying patches."
    exit 1
fi

echo "Applying patches to llama.cpp submodule..."

echo "Copying all files in llamafile-files to root directory..."
cp -r "$LLAMAFILE_FILES_DIR"/* .

# NOTE: renames.sh is SKIPPED - we use llama.cpp's native structure now

echo "Removing unnecessary files and directories..."
rm -rf examples
rm -rf models
rm -rf gguf-py
rm -rf tests
rm -rf spm-headers
rm -rf scripts
rm -rf .clang-tidy
rm -rf .devops/
rm -rf .dockerignore
rm -rf .ecrc
rm -rf .editorconfig
rm -rf .flake8
rm -rf .github/
rm -rf .gitignore
rm -rf .gitmodules
rm -rf .pre-commit-config.yaml
rm -rf AUTHORS
rm -rf CMakeLists.txt
rm -rf CMakePresets.json
rm -rf CONTRIBUTING.md
rm -rf Makefile
rm -rf Package.swift
rm -rf README.md
rm -rf SECURITY.md
rm -rf ci/
rm -rf cmake/
rm -rf convert_hf_to_gguf.py
rm -rf convert_hf_to_gguf_update.py
rm -rf convert_llama_ggml_to_gguf.py
rm -rf convert_lora_to_gguf.py
rm -rf docs/
rm -rf flake.lock
rm -rf flake.nix
rm -rf grammars/
rm -rf media/
rm -rf mypy.ini
rm -rf pocs/
rm -rf poetry.lock
rm -rf prompts/
rm -rf pyproject.toml
rm -rf pyrightconfig.json
rm -rf requirements.txt
rm -rf requirements/

cd ..
echo "Applying modifications to upstream files..."
for patch_file in "$PATCHES_DIR"/*.patch; do
    if [ -f "$patch_file" ]; then
        patch_basename=$(basename "$patch_file")
        # Skip llava patches - these files don't exist in new llama.cpp (renamed to mtmd)
        if [[ "$patch_basename" == llava_* ]]; then
            echo "Skipping $patch_basename (obsolete - mtmd API used instead)"
            continue
        fi
        # Skip main_main.cpp.patch - main.cpp moved to tools/cli/ in new structure
        if [[ "$patch_basename" == main_main.cpp.patch ]]; then
            echo "Skipping $patch_basename (obsolete - file moved to tools/cli/)"
            continue
        fi
        # Skip patches for files that no longer exist in their old locations
        if [[ "$patch_basename" == base64.h.patch || "$patch_basename" == common.cpp.patch || "$patch_basename" == common.h.patch || "$patch_basename" == console.cpp.patch ]]; then
            echo "Skipping $patch_basename (file structure changed in new llama.cpp)"
            continue
        fi
        # Skip ggml.h.patch - ggml.h moved to ggml/include/ggml.h in new structure
        if [[ "$patch_basename" == ggml.h.patch ]]; then
            echo "Skipping $patch_basename (obsolete - ggml.h moved to ggml/include/ggml.h)"
            continue
        fi
        echo "Applying $patch_basename..."
        if ! patch -p0 < "$patch_file" 2>/dev/null; then
            echo "  Note: $patch_basename did not apply cleanly, skipping..."
        fi
    fi
done

echo ""
echo "Patches applied successfully!"
echo "Note: These changes are not committed to the submodule."
echo "To reset the submodule to its clean state, run:"
echo "  cd llama.cpp && git reset --hard && git clean -fdx"
