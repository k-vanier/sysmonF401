#!/bin/sh
# Compile la prévisualisation de l'affichage sur PC et écrit les captures dans $1.
# Il faut un compilateur C : CC="zig cc", gcc ou clang.
#   sh tools/preview/build.sh /chemin/sortie
set -e
here=$(cd "$(dirname "$0")" && pwd)
fw="$here/../../firmware/sysmonf401/Core"
out=${1:-.}
${CC:-cc} -O1 -Wall -Wextra -Wno-unused-parameter \
  -I "$here" -I "$fw/Inc" \
  "$here/preview.c" "$fw/Src/sysmon.c" "$fw/Src/gfx.c" "$fw/Src/assets.c" \
  -lm -o "$out/preview"
"$out/preview" "$out"
