#!/usr/bin/env bash
# Build the dictionary soak test (real Dictionary + DictZip + InflateReader +
# uzlib) under AddressSanitizer. Apple clang is fine here — no libFuzzer needed.
#
#   ./test/soak/build.sh
#   SOAK_SD=/path/to/sd ./test/soak/dictionary_soak <folder>
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-clang}"
CXX="${CXX:-clang++}"
SAN="-fsanitize=address -fno-omit-frame-pointer"
SECT="-ffunction-sections -fdata-sections"

# Dead-strip unused functions at link, as the firmware does. uzlib's
# uzlib_uncompress_chksum (unused by InflateReader) references adler32/crc32
# units this uzlib copy doesn't ship; gc'ing unused sections drops it.
case "$(uname -s)" in
  Darwin) STRIP="-Wl,-dead_strip" ;;
  *)      STRIP="-Wl,--gc-sections" ;;
esac

# uzlib is C; compile it separately so the C++ -std flag doesn't hit it.
"$CC" -c "$ROOT/lib/uzlib/src/tinflate.c" -o "$HERE/tinflate.o" \
  -I"$ROOT/lib/uzlib/src" -O1 -g $SAN $SECT

"$CXX" -std=gnu++2a -g -O1 -fno-exceptions -fno-rtti $SAN \
  -DXML_GE=0 -DEINK_DISPLAY_SINGLE_BUFFER_MODE=1 \
  -I"$HERE" \
  -I"$HERE/stubs" \
  -I"$ROOT/src/util" \
  -I"$ROOT/lib/Memory" \
  -I"$ROOT/lib/InflateReader" \
  -I"$ROOT/lib/uzlib/src" \
  "$ROOT/src/util/Dictionary.cpp" \
  "$ROOT/src/util/DictZip.cpp" \
  "$ROOT/lib/InflateReader/InflateReader.cpp" \
  "$HERE/resolve.cpp" \
  "$HERE/dictionary_soak.cpp" \
  "$HERE/tinflate.o" \
  $SECT $STRIP \
  -o "$HERE/dictionary_soak"

echo "Built $HERE/dictionary_soak"
