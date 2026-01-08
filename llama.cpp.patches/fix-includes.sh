#!/bin/bash
# Fix includes in llama.cpp to use full paths from llama.cpp root
# This makes includes compatible with cosmopolitan mkdeps which expects exact path matches

set -e

LLAMACPP_DIR="llama.cpp"

echo "Building header map for $LLAMACPP_DIR..."

# Create a temp file to store header mappings (basename -> full_path)
HEADER_MAP=$(mktemp)
find "$LLAMACPP_DIR" -name "*.h" -o -name "*.hpp" | while read -r header; do
    basename=$(basename "$header")
    rel_path=${header#$LLAMACPP_DIR/}
    echo "$basename|llama.cpp/$rel_path" >> "$HEADER_MAP"
done

# Process a single source file
process_file() {
    local source_file="$1"
    local tmp_file="${source_file}.fixing"

    cp "$source_file" "$tmp_file"

    # Get all unique includes with bare filenames or relative paths
    # Match both: #include "filename.h" and #include "../../file.h"
    grep -oE '#include "[^"]*"' "$tmp_file" 2>/dev/null | sort -u | while read -r include_line; do
        # Extract the header filename from the include
        header_name=$(echo "$include_line" | sed 's/#include "\(.*\)"/\1/')

        # Skip if it's already a full path from llama.cpp root
        if [[ "$header_name" == llama.cpp/* ]]; then
            continue
        fi

        # Get just the basename for lookup
        basename_only=$(basename "$header_name")

        # Look up the full path for this header
        full_path=$(grep "^${basename_only}|" "$HEADER_MAP" 2>/dev/null | cut -d'|' -f2 | head -1)

        if [[ -n "$full_path" ]]; then
            # Replace the include with the full path from repository root
            perl -pi -e "s|#include \"${header_name}\"|#include \"${full_path}\"|g" "$tmp_file"
        fi
    done

    # Check if changed
    if ! diff -q "$source_file" "$tmp_file" > /dev/null 2>&1; then
        mv "$tmp_file" "$source_file"
        echo "  Fixed: $source_file"
    else
        rm "$tmp_file"
    fi
}

export LLAMACPP_DIR HEADER_MAP

# Process all C/C++ source and header files
echo "Fixing includes in source and header files..."
find "$LLAMACPP_DIR" \( -name "*.cpp" -o -name "*.c" -o -name "*.cc" -o -name "*.h" -o -name "*.hpp" \) -type f | while read -r source_file; do
    process_file "$source_file"
done

# Clean up
rm -f "$HEADER_MAP"

echo "Done! All includes now use full paths from repository root."
