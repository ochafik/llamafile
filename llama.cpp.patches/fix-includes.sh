#!/bin/bash
# Fix includes in llama.cpp to work with cosmopolitan mkdeps
# - Headers in same directory: keep as-is (e.g., models/models.h)
# - Headers in parent directories: use relative path (e.g., ../include/llama.h)
# - Headers in subdirectories: use full path from root (e.g., ggml/include/ggml.h)

set -e

LLAMACPP_DIR="llama.cpp"

echo "Fixing includes in $LLAMACPP_DIR..."

# Create temp file to store header locations
declare -A HEADER_DIRS
find "$LLAMACPP_DIR" -name "*.h" -o -name "*.hpp" | while read -r header; do
    basename=$(basename "$header")
    rel_path=${header#$LLAMACPP_DIR/}
    dir=$(dirname "$rel_path")
    HEADER_DIRS[$basename]="$dir"
done

# Process a single source file
process_file() {
    local source_file="$1"
    local tmp_file="${source_file}.fixing"
    
    cp "$source_file" "$tmp_file"
    
    # Get source directory relative to llama.cpp root
    local rel_source_file="${source_file#$LLAMACPP_DIR/}"
    local source_dir
    source_dir=$(dirname "$rel_source_file")
    
    # Find all #include "..." statements and fix them
    # Use a temp file to track what we've already replaced
    local replaced=""
    for header_name in "${!HEADER_DIRS[@]}"; do
        local header_dir="${HEADER_DIRS[$header_name]}"
        
        # Skip if header is in same directory as source
        if [[ "$header_dir" == "$source_dir" ]]; then
            continue
        fi
        
        # Calculate relative path from source to header
        local rel_path=""
        local up_count=0
        
        # Count how many directories to go up
        local IFS='/'
        read -ra src_parts <<< "$source_dir"
        read -ra hdr_parts <<< "$header_dir"
        
        # Find common prefix
        local i=0
        while [[ $i -lt ${#src_parts[@]} && $i -lt ${#hdr_parts[@]} && "${src_parts[$i]:-}" == "${hdr_parts[$i]:-}" ]]; do
            ((i++))
        done
        
        # Go up from source to common point
        for ((j=i; j<${#src_parts[@]}; j++)); do
            rel_path="../$rel_path"
        done
        
        # Go down from common point to header
        for ((j=i; j<${#hdr_parts[@]}; j++)); do
            if [[ -n "${hdr_parts[$j]}" ]]; then
                rel_path="${rel_path}${hdr_parts[$j]}/"
            fi
        done
        
        # Add the filename
        local new_include="${rel_path}${header_name}"
        
        # Replace include (only if not already replaced)
        if [[ "$replaced" != *"$header_name"* ]]; then
            if grep -q "#include \"$header_name\"" "$tmp_file" 2>/dev/null; then
                sed -i.tmp "s|#include \"$header_name\"|#include \"$new_include\"|g" "$tmp_file"
                rm -f "${tmp_file}.tmp"
                replaced="$replaced $header_name"
            fi
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

export LLAMACPP_DIR HEADER_DIRS

# Process all C/C++ source files
echo "Fixing includes in source files..."
find "$LLAMACPP_DIR" \( -name "*.cpp" -o -name "*.c" -o -name "*.cc" \) -type f | while read -r source_file; do
    process_file "$source_file"
done

echo "Done!"
