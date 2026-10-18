#!/bin/sh
# Builds ./build/maas on Linux or macOS with the system C/C++ compiler.
# Usage: ./build.sh            (optimised)
#        DEBUG=1 ./build.sh    (no optimisation, faster to compile)
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
CXX="${CXX:-c++}"
OPT="-O2"
[ -n "$DEBUG" ] && OPT="-O0 -g"
mkdir -p build/obj
FLAGS="-std=c++17 $OPT -Wall -Ithird_party -pthread"

# SQLite and the QR library rarely change: build them once.
[ -f build/obj/sqlite3.o ] || "$CC" -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_MAX_COLUMN=4096 \
  -c third_party/sqlite3.c -o build/obj/sqlite3.o
[ -f build/obj/qrcodegen.o ] || "$CXX" -std=c++17 -O2 -Ithird_party -c third_party/qrcodegen.cpp -o build/obj/qrcodegen.o

OBJECTS=""
for source in src/*.cpp; do
  name=$(basename "$source" .cpp)
  [ "$name" = "platform_win" ] && continue
  object="build/obj/$name.o"
  # Rebuild when the source or any header is newer than the object.
  if [ ! -f "$object" ] || [ "$source" -nt "$object" ] || [ -n "$(find src -name '*.hpp' -newer "$object" | head -1)" ]; then
    echo "  compiling $name"
    "$CXX" $FLAGS -c "$source" -o "$object" &
  fi
  OBJECTS="$OBJECTS $object"
done
wait
LIBS="-pthread"
[ "$(uname -s)" = "Linux" ] && LIBS="$LIBS -ldl"
"$CXX" $OBJECTS build/obj/sqlite3.o build/obj/qrcodegen.o -o build/maas $LIBS
echo "built build/maas"
