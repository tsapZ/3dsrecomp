#!/bin/sh
# Make a Windows package of one game for players (MSYS2 CLANG64 shell):
#   tools/bundle_windows.sh <name> <title id> [App Name]
# <build>/<App Name>/ holds the executable (<name>.exe), every DLL it loads from MSYS2's clang64 (found with ldd),
# each DLL package's licence (pacman -Qo, share/licenses), this project's own per-game files (games/<title id>/
# pipelines.bin, profile.txt: the shader pipeline list and the game's profile, made here, not the game's) and a
# README. No game files: the player's own dump goes into resources/game/ (exheader.bin, code.bin, romfs.bin,
# info.json, as tools/extract.py writes them); system_archives/ (Nintendo's) are never copied. A zip of the folder is
# made next to it (<build>/<App Name>.zip).
set -e
B=${BUILD:-build}
N=$1; TID=$2; APP=${3:-$1}
[ -n "$N" ] && [ -n "$TID" ] && [ -f "$B/$N.exe" ] || { echo "usage: BUILD=<build folder> $0 <name> <title id> [App Name]"; exit 1; }
OUT="$B/$APP"
rm -rf "$OUT" "$B/$APP.zip"
mkdir -p "$OUT/licenses" "$OUT/resources/game"
cp "$B/$N.exe" "$OUT/"
# without its debug info: a third of the size, and that info names the build folder in every lambda's type
llvm-strip "$OUT/$N.exe"
# the DLLs from clang64 (the system's own stay out), and the packages they come from
PKGS=""
for dll in $(ldd "$B/$N.exe" | sed -n 's/.*=> \(\/clang64\/[^ ]*\.dll\) .*/\1/p' | sort -u); do
  p=$(cygpath -u "$dll")
  cp "$p" "$OUT/"
  PKGS="$PKGS $(pacman -Qoq "$p" 2>/dev/null || true)"
done
for pkg in $(echo $PKGS | tr ' ' '\n' | sort -u); do
  name=${pkg#mingw-w64-clang-x86_64-}
  if [ -d "/clang64/share/licenses/$name" ]; then
    mkdir -p "$OUT/licenses/$name"
    cp -r "/clang64/share/licenses/$name/." "$OUT/licenses/$name/"
  elif [ "$name" = libidn2 ] && [ -f /clang64/share/licenses/libunistring/LICENSE.LIB ]; then
    # MSYS2 installs no licence for libidn2; the library is LGPL-3.0-or-later or GPL-2.0-or-later (its README),
    # taken under the LGPL-3.0, whose text (and the GPL-3.0 it builds on) libunistring ships
    mkdir -p "$OUT/licenses/libidn2"
    cp /clang64/share/licenses/libunistring/LICENSE.LIB "$OUT/licenses/libidn2/LGPL-3.0.txt"
    cp /clang64/share/licenses/libunistring/LICENSE "$OUT/licenses/libidn2/GPL-3.0.txt"
    printf '%s\n' "libidn2 (https://gitlab.com/libidn/libidn2): the library is under the GNU LGPL 3.0 or later, or the" \
      "GNU GPL 2.0 or later; used here under the LGPL 3.0 (LGPL-3.0.txt, with GPL-3.0.txt). Source: the URL above," \
      "built by MSYS2 (package mingw-w64-clang-x86_64-libidn2)." > "$OUT/licenses/libidn2/NOTICE.txt"
  else
    echo "$pkg: no licence folder in /clang64/share/licenses (add it by hand)" >> "$OUT/licenses/MISSING.txt"
  fi
done
cp LICENSE "$OUT/licenses/3dsrecomp-EUPL-1.2.txt"
for f in pipelines.bin profile.txt; do [ -f "games/$TID/$f" ] && cp -L "games/$TID/$f" "$OUT/resources/game/"; done
cat > "$OUT/README.txt" <<README
$APP for Windows, recompiled with tsapZ/3dsrecomp (https://github.com/tsapZ/3dsrecomp, EUPL-1.2).

No game files come with it: they come from your own dump of the game (.3ds, .cci, .cxi or .cia), once:

    $N.exe --setup "<your dump>"

That writes them into resources\\game. It has to be the edition this was made from (title $TID); a dump of
another one is refused. An encrypted dump needs your own console's keys: boot9.bin (and for a .cia aes_keys.txt, for
some titles seeddb.bin) next to the dump, or pass --boot9 / --keys / --seeddb. Then start $N.exe.
Saves go to %APPDATA%\\3dsrecomp, or to the folder given with --save-dir=<folder>.
Figures: F5 puts a figure file on the portal, F6 lifts it off. The licences of the libraries are in licenses\\.
Not affiliated with Activision or Nintendo.
README
# nothing that ships may name the build machine: the source folder (both spellings) or the user's name
SRC_W=$(pwd -W 2>/dev/null || pwd)
WHO=${USERNAME:-$USER}
LEAK=""
for f in "$OUT"/*.exe "$OUT"/*.dll; do
  grep -a -q -i -F -e "$(pwd)" -e "$SRC_W" ${WHO:+-e "$WHO"} "$f" && LEAK="$LEAK $(basename "$f")"
done
if [ -n "$LEAK" ]; then
  echo "not packaged, build machine paths in:$LEAK"
  rm -rf "$OUT"
  exit 1
fi
( cd "$B" && zip -qr "$APP.zip" "$APP" ) 2>/dev/null || powershell.exe -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$OUT")' -DestinationPath '$(cygpath -w "$B/$APP.zip")'"
echo "built $OUT ($(ls "$OUT"/*.dll | wc -l) DLLs, $(du -sh "$OUT" | cut -f1))"
[ -f "$OUT/licenses/MISSING.txt" ] && cat "$OUT/licenses/MISSING.txt"
exit 0
