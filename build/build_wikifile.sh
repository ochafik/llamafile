#!/bin/sh
# Build a wikifile (llamafile with Wikipedia ZIM support)
#
# Usage: build_wikifile.sh <name> [--gguf <path>] [--zim <path>]
#
# Arguments:
#   name          Name for the output wikifile (e.g., "my-wiki")
#   --gguf PATH   Path to GGUF model file (optional, will be embedded)
#   --zim PATH    Path to ZIM file (optional, will be embedded)
#
# Output:
#   dist/<name>.llamafile

set -e

PROG=${0##*/}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

usage() {
  local exit_code=${1:-0}
  echo "Usage: $PROG <name> [--gguf <path>] [--zim <path>]"
  echo ""
  echo "Build a wikifile (llamafile with Wikipedia ZIM support)"
  echo ""
  echo "Arguments:"
  echo "  name          Name for the output wikifile (e.g., 'my-wiki')"
  echo "  --gguf PATH   Path to GGUF model file (optional, will be embedded)"
  echo "  --zim PATH    Path to ZIM file (optional, will be embedded)"
  echo ""
  echo "Output:"
  echo "  dist/<name>.llamafile"
  echo ""
  echo "Examples:"
  echo "  $PROG mywiki --gguf model.gguf --zim wikipedia.zim"
  echo "  $PROG mywiki --zim wikipedia.zim  # use external model"
  echo "  $PROG mywiki                       # just build the server"
  exit "$exit_code"
}

abort() {
  echo "$PROG: error: $1" >&2
  exit 1
}

# Parse arguments
NAME=""
GGUF_PATH=""
ZIM_PATH=""

while [ $# -gt 0 ]; do
  case "$1" in
    --help|-h)
      usage
      ;;
    --gguf)
      shift
      [ $# -gt 0 ] || abort "--gguf requires a path argument"
      GGUF_PATH="$1"
      shift
      ;;
    --zim)
      shift
      [ $# -gt 0 ] || abort "--zim requires a path argument"
      ZIM_PATH="$1"
      shift
      ;;
    -*)
      abort "unknown option: $1"
      ;;
    *)
      if [ -z "$NAME" ]; then
        NAME="$1"
      else
        abort "unexpected argument: $1"
      fi
      shift
      ;;
  esac
done

[ -n "$NAME" ] || usage 1

# Validate file paths
if [ -n "$GGUF_PATH" ] && [ ! -f "$GGUF_PATH" ]; then
  abort "GGUF file not found: $GGUF_PATH"
fi
if [ -n "$ZIM_PATH" ] && [ ! -f "$ZIM_PATH" ]; then
  abort "ZIM file not found: $ZIM_PATH"
fi

# Build the llamafile server
echo "Building llamafile server..."
cd "$ROOT_DIR"

# Use cosmocc make if available (required on macOS with old system make)
MAKE=make
if [ -x "$ROOT_DIR/.cosmocc/3.9.7/bin/make" ]; then
  MAKE="$ROOT_DIR/.cosmocc/3.9.7/bin/make"
fi

$MAKE -j "o//llamafile/server/main" || abort "build failed"

# Find zipalign
ZIPALIGN="$ROOT_DIR/o//llamafile/zipalign"
if [ ! -x "$ZIPALIGN" ]; then
  echo "Building zipalign..."
  $MAKE -j "o//llamafile/zipalign" || abort "zipalign build failed"
fi

# Create dist directory
mkdir -p "$ROOT_DIR/dist"

OUTPUT="$ROOT_DIR/dist/${NAME}.llamafile"
echo "Creating $OUTPUT..."

# Start with the llamafile server binary
cp -f "$ROOT_DIR/o//llamafile/server/main" "$OUTPUT" || abort "failed to copy server binary"

# Build .args file for embedded arguments (must be named .args for zipalign)
ARGS_FILE="$ROOT_DIR/dist/.args"
trap "rm -f '$ARGS_FILE'" EXIT

# Start fresh
: > "$ARGS_FILE"

# Add arguments based on what files are embedded
if [ -n "$GGUF_PATH" ]; then
  GGUF_BASENAME=$(basename "$GGUF_PATH")
  printf '%s\n' "-m" >> "$ARGS_FILE"
  printf '%s\n' "$GGUF_BASENAME" >> "$ARGS_FILE"
fi

if [ -n "$ZIM_PATH" ]; then
  ZIM_BASENAME=$(basename "$ZIM_PATH")
  printf '%s\n' "--zim" >> "$ARGS_FILE"
  printf '%s\n' "$ZIM_BASENAME" >> "$ARGS_FILE"
  printf '%s\n' "--zim-tools" >> "$ARGS_FILE"
fi

# Always end with ... to allow additional CLI args
printf '%s\n' "..." >> "$ARGS_FILE"

# Append .args to the llamafile (zipalign uses basename as name in archive)
"$ZIPALIGN" -j0 "$OUTPUT" "$ARGS_FILE" || abort "failed to add .args"

# Append GGUF file if provided
if [ -n "$GGUF_PATH" ]; then
  echo "Embedding GGUF file: $GGUF_PATH"
  "$ZIPALIGN" -j0 "$OUTPUT" "$GGUF_PATH" || abort "failed to embed GGUF"
fi

# Append ZIM file if provided
if [ -n "$ZIM_PATH" ]; then
  echo "Embedding ZIM file: $ZIM_PATH"
  "$ZIPALIGN" -j0 "$OUTPUT" "$ZIM_PATH" || abort "failed to embed ZIM"
fi

# Make executable
chmod +x "$OUTPUT"

echo ""
echo "Success! Created: $OUTPUT"
echo ""
echo "Run with: $OUTPUT"
if [ -z "$GGUF_PATH" ]; then
  echo "Note: No GGUF embedded. You'll need to specify -m <model.gguf> when running."
fi
if [ -z "$ZIM_PATH" ]; then
  echo "Note: No ZIM embedded. You can specify --zim <file.zim> when running."
fi
