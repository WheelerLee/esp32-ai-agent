#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

FONT_FILE="$PROJECT_DIR/fonts/puhui.ttf"
TEXT_FILE="$PROJECT_DIR/fonts/常用字.txt"
SIZE=14
BPP=1
OUTPUT=""
ACTIVE_OUTPUT=""
PYTHON_BIN="${PYTHON:-python}"

usage() {
  cat <<EOF
Usage: tools/generate_lazy_font.sh [options]

Options:
  --fontsize SIZE   Font size in px. Default: 14
  --size SIZE       Same as --fontsize
  --bpp BPP         Bits per pixel. Default: 1
  --bbp BPP         Alias for --bpp
  --font PATH       TTF/OTF font path. Default: fonts/puhui.ttf
  --text PATH       Character text file. Default: fonts/常用字.txt
  --output PATH     Output bin path. Default: fonts/llm_text_<SIZE>_lazy.bin
  --active PATH     Copy output here after generation. Default: font_active/<output name>
  --python PATH     Python executable. Default: python or \$PYTHON
  -h, --help        Show this help

Example:
  tools/generate_lazy_font.sh --fontsize 14 --bbp 1
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --fontsize|--size)
      SIZE="$2"
      shift 2
      ;;
    --bpp|--bbp)
      BPP="$2"
      shift 2
      ;;
    --font)
      FONT_FILE="$2"
      shift 2
      ;;
    --text)
      TEXT_FILE="$2"
      shift 2
      ;;
    --output)
      OUTPUT="$2"
      shift 2
      ;;
    --active)
      ACTIVE_OUTPUT="$2"
      shift 2
      ;;
    --python)
      PYTHON_BIN="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

case "$SIZE" in
  *[!0-9]*|"")
    echo "--fontsize must be a positive integer" >&2
    exit 1
    ;;
esac

case "$BPP" in
  1)
    ;;
  *)
    echo "Only --bpp/--bbp 1 is supported by the current ESP32 lazy font reader" >&2
    exit 1
    ;;
esac

case "$FONT_FILE" in
  /*|?:/*|?:\\*) ;;
  *) FONT_FILE="$PROJECT_DIR/$FONT_FILE" ;;
esac

case "$TEXT_FILE" in
  /*|?:/*|?:\\*) ;;
  *) TEXT_FILE="$PROJECT_DIR/$TEXT_FILE" ;;
esac

if [ -z "$OUTPUT" ]; then
  OUTPUT="$PROJECT_DIR/fonts/llm_text_${SIZE}_lazy.bin"
else
  case "$OUTPUT" in
    /*|?:/*|?:\\*) ;;
    *) OUTPUT="$PROJECT_DIR/$OUTPUT" ;;
  esac
fi

if [ -z "$ACTIVE_OUTPUT" ]; then
  ACTIVE_OUTPUT="$PROJECT_DIR/font_active/$(basename -- "$OUTPUT")"
else
  case "$ACTIVE_OUTPUT" in
    /*|?:/*|?:\\*) ;;
    *) ACTIVE_OUTPUT="$PROJECT_DIR/$ACTIVE_OUTPUT" ;;
  esac
fi

"$PYTHON_BIN" "$PROJECT_DIR/tools/generate_lazy_font.py" \
  --font "$FONT_FILE" \
  --text "$TEXT_FILE" \
  --size "$SIZE" \
  --bpp "$BPP" \
  --output "$OUTPUT"

mkdir -p "$(dirname -- "$ACTIVE_OUTPUT")"
cp "$OUTPUT" "$ACTIVE_OUTPUT"

echo "copied to $ACTIVE_OUTPUT"
