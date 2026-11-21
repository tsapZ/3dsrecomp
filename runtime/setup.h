#pragma once
// The game files from the player's own dump, without Python: tools/extract.py and tools/ctrcrypto.py in C++, for the
// Windows packages (tools/bundle_windows.sh), which ship no game files. Writes what the runtime reads (exheader.bin,
// code.bin, romfs.bin) plus exefs/*.bin and a short info.json, byte for byte as extract.py does; the unpacked romfs/
// (extract.py's, for inspection only) is left out.
//
//   <game>.exe --setup <dump .3ds/.cci/.cxi/.app/.cia> [--out <folder>] [--boot9 boot9.bin] [--keys aes_keys.txt]
//              [--seeddb seeddb.bin | --seed <32 hex digits>]
//
// <folder> defaults to resources/game next to the executable. Key files not given are looked for next to the dump,
// in the current folder and in Azahar's or Citra's sysdata folder. Returns the process exit code.
namespace setup {
int run(int argc, char **argv);
}
