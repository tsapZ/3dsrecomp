// The game files from the player's own dump (setup.h): tools/extract.py and tools/ctrcrypto.py in C++. AES-CBC/CTR
// and SHA-256 come from OpenSSL's libcrypto. Keys only ever come from the player's own files.
#include "setup.h"
#include "platform.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace setup {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using Block = std::array<uint8_t, 16>;
constexpr uint64_t MU = 0x200;  // the media unit

[[noreturn]] void fail(const std::string &why) {
    std::fprintf(stderr, "setup: %s\n", why.c_str());
    std::fflush(stderr);
    std::exit(1);
}

uint16_t u16(const uint8_t *b) { return uint16_t(b[0] | b[1] << 8); }
uint32_t u32(const uint8_t *b) { return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24; }
uint64_t u64(const uint8_t *b) { return u32(b) | uint64_t(u32(b + 4)) << 32; }
uint32_t be32(const uint8_t *b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
uint16_t be16(const uint8_t *b) { return uint16_t(b[0] << 8 | b[1]); }
uint64_t be64(const uint8_t *b) { return uint64_t(be32(b)) << 32 | be32(b + 4); }

Bytes read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) fail("cannot open " + p.string());
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

void write_file(const fs::path &p, const uint8_t *data, size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char *>(data), std::streamsize(n))) fail("cannot write " + p.string());
}

// ---- AES (decryption only, as ctrcrypto.py) and SHA-256
void aes(bool ctr, const Block &key, const Block &iv, const uint8_t *in, uint8_t *out, size_t n) {
    if (!n) return;
    std::unique_ptr<EVP_CIPHER_CTX, void (*)(EVP_CIPHER_CTX *)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int moved = 0;
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), ctr ? EVP_aes_128_ctr() : EVP_aes_128_cbc(), nullptr, key.data(), iv.data()) != 1)
        fail("AES init");
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    if (EVP_DecryptUpdate(ctx.get(), out, &moved, in, int(n)) != 1 || size_t(moved) != n) fail("AES");
}

std::array<uint8_t, 32> sha256(const uint8_t *data, size_t n) {
    std::array<uint8_t, 32> out{};
    unsigned len = 0;
    if (EVP_Digest(data, n, out.data(), &len, EVP_sha256(), nullptr) != 1) fail("SHA-256");
    return out;
}

// 128-bit numbers big-endian in a Block: the key scrambler and the CTR counters
struct U128 {
    uint64_t hi = 0, lo = 0;
};
U128 from_block(const Block &b) { return {be64(b.data()), be64(b.data() + 8)}; }
Block to_block(U128 v) {
    Block b{};
    for (int i = 0; i < 8; i++) b[i] = uint8_t(v.hi >> (56 - 8 * i)), b[8 + i] = uint8_t(v.lo >> (56 - 8 * i));
    return b;
}
U128 rol(U128 v, int r) {
    r %= 128;
    if (r >= 64) std::swap(v.hi, v.lo), r -= 64;
    if (!r) return v;
    return {v.hi << r | v.lo >> (64 - r), v.lo << r | v.hi >> (64 - r)};
}
U128 add(U128 a, U128 b) {
    U128 s{a.hi + b.hi, a.lo + b.lo};
    if (s.lo < a.lo) s.hi++;
    return s;
}
// the AES engine's key scrambler: (((KeyX <<< 2) ^ KeyY) + C) <<< 87
Block normal_key(const Block &key_x, const Block &key_y) {
    const U128 c{0x1FF9E9AAC5FE0408ull, 0x024591DC5D52768Aull};
    U128 x = rol(from_block(key_x), 2), y = from_block(key_y);
    return to_block(rol(add({x.hi ^ y.hi, x.lo ^ y.lo}, c), 87));
}

Block block_at(const uint8_t *p) {
    Block b{};
    std::memcpy(b.data(), p, 16);
    return b;
}

// ---- the player's key files
struct Keys {
    std::map<int, Block> x, common;
    std::map<uint64_t, Block> seeds;
    std::optional<Block> seed;

    const Block &key_x(int slot, const std::string &what) const {
        auto it = x.find(slot);
        if (it == x.end()) {
            char s[96];
            std::snprintf(s, sizeof s, (slot == 0x2C || slot == 0x3D) ? "--boot9 boot9.bin" : "--keys aes_keys.txt (slot0x%02XKeyX)", slot);
            char m[200];
            std::snprintf(m, sizeof m, "%s needs KeyX 0x%02X: pass %s", what.c_str(), slot, s);
            fail(m);
        }
        return it->second;
    }

    Block title_seed(uint64_t program_id, const uint8_t *check) const {
        std::optional<Block> s = seed;
        if (!s) {
            auto it = seeds.find(program_id);
            if (it != seeds.end()) s = it->second;
        }
        char id[17];
        std::snprintf(id, sizeof id, "%016llX", (unsigned long long)program_id);
        if (!s) fail(std::string("NCCH ") + id + " uses seed crypto: pass --seeddb seeddb.bin (or --seed) with its seed");
        uint8_t buf[24];
        std::memcpy(buf, s->data(), 16);
        for (int i = 0; i < 8; i++) buf[16 + i] = uint8_t(program_id >> (8 * i));
        if (std::memcmp(sha256(buf, 24).data(), check, 4) != 0)
            fail(std::string("the seed given for ") + id + " does not match the one its NCCH was made with");
        return *s;
    }
};

Block hex16(const std::string &s, const std::string &what) {
    if (s.size() != 32) fail(what + ": expected 16 bytes (32 hex digits)");
    Block b{};
    for (int i = 0; i < 16; i++) b[i] = uint8_t(std::stoul(s.substr(2 * i, 2), nullptr, 16));
    return b;
}

Keys load_keys(const std::string &boot9, const std::string &keyfile, const std::string &seeddb, const std::string &seed) {
    Keys k;
    if (!seed.empty()) k.seed = hex16(seed, "--seed");
    if (!seeddb.empty()) {
        Bytes db = read_file(seeddb);
        const uint32_t count = db.size() >= 4 ? u32(db.data()) : 0;
        if (db.size() < 0x10 + size_t(count) * 0x20) fail(seeddb + ": not a seeddb.bin");
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *e = db.data() + 0x10 + 0x20 * i;
            k.seeds[u64(e)] = block_at(e + 8);
        }
    }
    if (!boot9.empty()) {
        Bytes b9 = read_file(boot9);
        if (b9.size() != 0x10000 && b9.size() != 0x8000) fail(boot9 + ": not a boot9.bin (expected 64 KiB, or its 32 KiB protected half)");
        const size_t o = b9.size() == 0x8000 ? 0x59D0 : 0xD9D0;  // the KeyX table the boot ROM loads
        k.x[0x2C] = block_at(b9.data() + o);
        k.x[0x3D] = block_at(b9.data() + o + 0x50);
    }
    if (!keyfile.empty()) {
        std::ifstream f(keyfile);
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
            const std::string name = line.substr(0, eq), value = line.substr(eq + 1);
            if (value.empty()) continue;
            if (name.rfind("common", 0) == 0 && name.size() > 6 && std::all_of(name.begin() + 6, name.end(), ::isdigit))
                k.common[std::stoi(name.substr(6))] = hex16(value, name);
            else if (name.rfind("slot0x", 0) == 0 && name.size() >= 12 && name.substr(name.size() - 4) == "KeyX")
                k.x[int(std::stoul(name.substr(6, 2), nullptr, 16))] = hex16(value, name);
        }
    }
    return k;
}

// boot9.bin, aes_keys.txt and seeddb.bin next to the dump, in the current folder, or in an emulator's sysdata folder
std::map<std::string, std::string> find_key_files(const fs::path &dump_dir) {
    std::vector<fs::path> where = {dump_dir, fs::current_path()};
    if (const char *appdata = std::getenv("APPDATA"))
        for (const char *app : {"Azahar", "Citra", "Lime3DS"}) where.push_back(fs::path(appdata) / app / "sysdata");
    std::map<std::string, std::string> found;
    for (const auto &d : where)
        for (const char *name : {"boot9.bin", "aes_keys.txt", "seeddb.bin"}) {
            std::error_code ec;
            if (!found.count(name) && fs::is_regular_file(d / name, ec)) found[name] = (d / name).string();
        }
    return found;
}

// ---- readers: a view with pread(pos, n), as ctrcrypto.Reader
struct Reader {
    uint64_t size = 0;
    virtual ~Reader() = default;
    virtual Bytes pread(uint64_t pos, uint64_t n) = 0;
};

struct FileReader : Reader {
    FILE *f;
    explicit FileReader(const fs::path &p) {
#ifdef _WIN32
        f = _wfopen(p.wstring().c_str(), L"rb");
#else
        f = std::fopen(p.string().c_str(), "rb");
#endif
        if (!f) fail("cannot open " + p.string());
#ifdef _WIN32
        _fseeki64(f, 0, SEEK_END);
        size = uint64_t(_ftelli64(f));
#else
        fseeko(f, 0, SEEK_END);
        size = uint64_t(ftello(f));
#endif
    }
    ~FileReader() override { std::fclose(f); }
    Bytes pread(uint64_t pos, uint64_t n) override {
        if (pos >= size) return {};
        n = std::min(n, size - pos);
        Bytes out(n);
#ifdef _WIN32
        _fseeki64(f, int64_t(pos), SEEK_SET);
#else
        fseeko(f, off_t(pos), SEEK_SET);
#endif
        out.resize(std::fread(out.data(), 1, n, f));
        return out;
    }
};

struct SliceReader : Reader {
    Reader &src;
    uint64_t off;
    SliceReader(Reader &s, uint64_t o, uint64_t n) : src(s), off(o) { size = n; }
    Bytes pread(uint64_t pos, uint64_t n) override {
        if (pos >= size) return {};
        return src.pread(off + pos, std::min(n, size - pos));
    }
};

// a CIA content encrypted with the title key (AES-CBC, IV = the content index)
struct CbcReader : Reader {
    std::unique_ptr<Reader> src;
    Block key, iv{};
    CbcReader(std::unique_ptr<Reader> s, const Block &k, uint16_t index) : src(std::move(s)), key(k) {
        size = src->size;
        iv[0] = uint8_t(index >> 8), iv[1] = uint8_t(index);
    }
    Bytes pread(uint64_t pos, uint64_t n) override {
        n = pos < size ? std::min(n, size - pos) : 0;
        const uint64_t a = pos & ~15ull, b = std::min(size, (pos + n + 15) & ~15ull);
        if (a >= b) return {};
        Bytes ct;
        if (a) {
            ct = src->pread(a - 16, b - a + 16);
        } else {
            ct.assign(iv.begin(), iv.end());
            Bytes rest = src->pread(0, b);
            ct.insert(ct.end(), rest.begin(), rest.end());
        }
        if (ct.size() < 16) return {};
        Bytes plain(ct.size() - 16);
        aes(false, key, block_at(ct.data()), ct.data() + 16, plain.data(), plain.size() & ~size_t(15));
        const uint64_t from = pos - a;
        if (from >= plain.size()) return {};
        return Bytes(plain.begin() + from, plain.begin() + std::min<uint64_t>(plain.size(), from + n));
    }
};

uint32_t sig_size(const uint8_t *blob) {
    switch (be32(blob)) {
    case 0x10000: case 0x10003: return 0x240;
    case 0x10001: case 0x10004: return 0x140;
    case 0x10002: case 0x10005: return 0x80;
    }
    fail("CIA: unknown signature type");
}

// the CIA's main content (index 0, the program's NCCH), with the CIA layer removed
std::unique_ptr<Reader> open_cia(Reader &f, const Keys &keys) {
    Bytes hdr = f.pread(0, 0x20);
    if (hdr.size() < 0x20) fail("CIA: short header");
    const uint32_t hdr_size = u32(&hdr[0]), cert_size = u32(&hdr[8]), tik_size = u32(&hdr[12]), tmd_size = u32(&hdr[16]);
    auto al = [](uint64_t n) { return (n + 63) & ~63ull; };
    const uint64_t tik_off = al(hdr_size) + al(cert_size), tmd_off = tik_off + al(tik_size), content_off = tmd_off + al(tmd_size);
    Bytes tik = f.pread(tik_off, tik_size), tmd = f.pread(tmd_off, tmd_size);
    const uint32_t ts = sig_size(tik.data()), ms = sig_size(tmd.data());
    const uint8_t *t = tik.data() + ts, *m = tmd.data() + ms;
    const uint16_t count = be16(m + 0x9E);
    if (!count) fail("CIA: no contents");
    const uint8_t *chunk = m + 0xC4 + 64 * 36;  // the first chunk record: the program
    const uint16_t index = be16(chunk + 4), ctype = be16(chunk + 6);
    const uint64_t size = be64(chunk + 8);
    if (index != 0) fail("CIA: the first content is not the program (index " + std::to_string(index) + ")");
    auto src = std::make_unique<SliceReader>(f, content_off, size);
    if (!(ctype & 1)) return src;
    const int common = t[0xB1];
    auto it = keys.common.find(common);
    if (it == keys.common.end())
        fail("the CIA's ticket uses common key " + std::to_string(common) + ": pass --keys aes_keys.txt (common" + std::to_string(common) + "=<KeyY>)");
    const Block kek = normal_key(keys.key_x(0x3D, "the CIA's title key"), it->second);
    Block iv{};
    std::memcpy(iv.data(), t + 0x9C, 8);  // the title id, then zeros
    Block title_key{};
    aes(false, kek, iv, t + 0x7F, title_key.data(), 16);
    return std::make_unique<CbcReader>(std::move(src), title_key, index);
}

// an encrypted NCCH with its keyslot layer removed; its header says NoCrypto
struct NcchReader : Reader {
    std::unique_ptr<Reader> owned;
    Reader &src;
    struct Region {
        uint64_t start, stop;
        Block key;
        U128 ctr;
        uint64_t base;
    };
    std::vector<Region> regions;
    Bytes header;

    NcchReader(Reader &s, std::unique_ptr<Reader> own, const Keys &keys) : owned(std::move(own)), src(s) {
        size = src.size;
        Bytes nh = src.pread(0, 0x200);
        if (nh.size() < 0x200) fail("NCCH: short header");
        const uint8_t *flags = &nh[0x188];
        Block key0{}, key1{};
        if (!(flags[7] & 1)) {
            Block key_y = block_at(nh.data());  // the first 16 bytes of the header's signature
            key0 = normal_key(keys.key_x(0x2C, "NCCH"), key_y);
            int slot;
            switch (flags[3]) {
            case 0x00: slot = 0x2C; break;
            case 0x01: slot = 0x25; break;
            case 0x0A: slot = 0x18; break;
            case 0x0B: slot = 0x1B; break;
            default: fail("NCCH crypto method " + std::to_string(flags[3]) + " is unknown");
            }
            if (flags[7] & 0x20) {  // seed crypto: the secondary key's KeyY also hashes in the title's seed
                const Block seed = keys.title_seed(u64(&nh[0x118]), &nh[0x114]);
                uint8_t buf[32];
                std::memcpy(buf, key_y.data(), 16);
                std::memcpy(buf + 16, seed.data(), 16);
                key_y = block_at(sha256(buf, 32).data());
            }
            key1 = normal_key(keys.key_x(slot, "NCCH crypto method " + std::to_string(flags[3])), key_y);
        }
        const uint16_t version = u16(&nh[0x112]);
        const uint64_t exefs_off = u32(&nh[0x1A0]) * MU, exefs_size = u32(&nh[0x1A4]) * MU;
        const uint64_t romfs_off = u32(&nh[0x1B0]) * MU, romfs_size = u32(&nh[0x1B4]) * MU;
        auto ctr = [&](uint8_t kind, uint64_t off) {
            Block c{};
            if (version == 1) {  // partition id, then the region's byte offset
                std::memcpy(c.data(), &nh[0x108], 8);
                for (int i = 0; i < 4; i++) c[12 + i] = uint8_t(off >> (24 - 8 * i));
            } else {
                for (int i = 0; i < 8; i++) c[i] = nh[0x10F - i];
                c[8] = kind;
            }
            return from_block(c);
        };
        if (u32(&nh[0x180])) regions.push_back({0x200, 0xA00, key0, ctr(1, 0x200), 0x200});
        if (exefs_size) {
            const U128 base = ctr(2, exefs_off);
            regions.push_back({exefs_off, exefs_off + exefs_size, key0, base, exefs_off});
            const Bytes exefs_hdr = pread(exefs_off, 0x200);
            const bool same = key0 == key1;
            for (int i = 0; i < 10 && exefs_hdr.size() >= 0x200; i++) {
                const char *name = reinterpret_cast<const char *>(&exefs_hdr[i * 16]);
                const std::string nm(name, strnlen(name, 8));
                const uint32_t off = u32(&exefs_hdr[i * 16 + 8]), sz = u32(&exefs_hdr[i * 16 + 12]);
                if (!nm.empty() && nm != "icon" && nm != "banner" && !same) {
                    const uint64_t start = exefs_off + 0x200 + off;
                    regions.push_back({start, start + ((sz + 15) & ~15u), key1, base, exefs_off});
                }
            }
        }
        if (romfs_size) regions.push_back({romfs_off, romfs_off + romfs_size, key1, ctr(3, romfs_off), romfs_off});
        nh[0x18B] = 0;                                            // crypto method
        nh[0x18F] = uint8_t((nh[0x18F] & ~0x21) | 4);             // NoCrypto, no fixed key or seed
        header = nh;
    }

    Bytes pread(uint64_t pos, uint64_t n) override {
        n = pos < size ? std::min(n, size - pos) : 0;
        Bytes out = src.pread(pos, n);
        const uint64_t end = pos + out.size();
        for (const Region &r : regions) {
            const uint64_t a = std::max(pos, r.start), b = std::min(end, r.stop);
            if (a >= b) continue;
            const uint64_t a16 = r.base + ((a - r.base) & ~15ull);
            const uint64_t blocks = (a16 - r.base) / 16;
            const Block iv = to_block(add(r.ctr, {0, blocks}));
            Bytes ct = src.pread(a16, b - a16);
            Bytes pt(ct.size());
            aes(true, r.key, iv, ct.data(), pt.data(), ct.size());
            std::memcpy(out.data() + (a - pos), pt.data() + (a - a16), size_t(b - a));
        }
        if (pos < 0x200 && !header.empty()) {
            const uint64_t stop = std::min<uint64_t>(end, 0x200);
            std::memcpy(out.data(), header.data() + pos, size_t(stop - pos));
        }
        return out;
    }
};

// the ExeFS .code backward LZ77 ("BLZ")
Bytes lzss_decompress(const Bytes &comp) {
    if (comp.size() < 8) fail(".code: too short to be compressed");
    const uint32_t top_bottom = u32(&comp[comp.size() - 8]), extra = u32(&comp[comp.size() - 4]);
    const size_t out_size = comp.size() + extra;
    Bytes out(out_size);
    std::memcpy(out.data(), comp.data(), comp.size());
    int64_t index = int64_t(comp.size()) - ((top_bottom >> 24) & 0xFF);
    const int64_t stop = int64_t(comp.size()) - (top_bottom & 0xFFFFFF);
    int64_t o = int64_t(out_size);
    while (index > stop) {
        index--;
        uint8_t control = comp[size_t(index)];
        for (int k = 0; k < 8; k++) {
            if (index <= stop || o <= 0) break;
            if (control & 0x80) {
                index -= 2;
                uint32_t seg = comp[size_t(index)] | comp[size_t(index) + 1] << 8;
                const uint32_t n = ((seg >> 12) & 15) + 3;
                seg = (seg & 0xFFF) + 2;
                for (uint32_t i = 0; i < n; i++) {
                    const uint8_t b = out[size_t(o + seg)];
                    o--;
                    out[size_t(o)] = b;
                }
            } else {
                index--;
                o--;
                out[size_t(o)] = comp[size_t(index)];
            }
            control = uint8_t(control << 1);
        }
    }
    return out;
}

std::string hex64(uint64_t v) {
    char s[17];
    std::snprintf(s, sizeof s, "%016llx", (unsigned long long)v);
    return s;
}

std::string json_text(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) {
        const char c = char(p[i]);
        if (c == '"' || c == '\\') s += '\\';
        s += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    return s;
}

}  // namespace

int run(int argc, char **argv) {
    std::string dump, out, boot9, keyfile, seeddb, seed;
    for (int i = 0; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) fail(a + " needs a value");
            return argv[++i];
        };
        if (a == "--out") out = next();
        else if (a == "--boot9") boot9 = next();
        else if (a == "--keys") keyfile = next();
        else if (a == "--seeddb") seeddb = next();
        else if (a == "--seed") seed = next();
        else if (dump.empty()) dump = a;
        else fail("unknown argument " + a);
    }
    if (dump.empty()) fail("usage: --setup <dump .3ds/.cci/.cxi/.app/.cia> [--out <folder>] [--boot9 <file>] [--keys <file>] [--seeddb <file> | --seed <hex>]");
    if (out.empty()) {
        const std::string exe = platform::exe_path();
        out = ((exe.empty() ? fs::current_path() : fs::path(exe).parent_path()) / "resources" / "game").string();
    }
    const auto found = find_key_files(fs::absolute(dump).parent_path());
    auto pick = [&](std::string &v, const char *name) {
        if (v.empty() && found.count(name)) {
            v = found.at(name);
            std::fprintf(stderr, "using %s\n", v.c_str());
        }
    };
    pick(boot9, "boot9.bin");
    pick(keyfile, "aes_keys.txt");
    if (seed.empty()) pick(seeddb, "seeddb.bin");
    const Keys keys = load_keys(boot9, keyfile, seeddb, seed);

    FileReader file(dump);
    Bytes head = file.pread(0, 0x200);
    if (head.size() >= 4 && std::memcmp(head.data(), "3DSX", 4) == 0) fail("a .3dsx is homebrew: use tools/extract.py");
    if (head.size() < 0x200) fail("not an NCCH/NCSD/CIA image");
    std::unique_ptr<Reader> cia;
    Reader *src = &file;
    uint64_t ncch_off = 0;
    if (u32(head.data()) == 0x2020) {  // a CIA's header size
        cia = open_cia(file, keys);
        src = cia.get();
    } else if (std::memcmp(&head[0x100], "NCSD", 4) == 0) {
        ncch_off = u32(&head[0x120]) * MU;  // partition 0: the executable content
    }
    Bytes nh = src->pread(ncch_off, 0x200);
    if (nh.size() < 0x200 || std::memcmp(&nh[0x100], "NCCH", 4) != 0) fail("not an NCCH/NCSD/CIA image");
    std::unique_ptr<Reader> slice, plain;
    Reader *ncch = src;
    if (!(nh[0x188 + 7] & 4)) {
        if (ncch_off || src == &file) {
            slice = std::make_unique<SliceReader>(*src, ncch_off, uint64_t(u32(&nh[0x104])) * MU);
            plain = std::make_unique<NcchReader>(*slice, nullptr, keys);
        } else {
            plain = std::make_unique<NcchReader>(*src, nullptr, keys);
        }
        ncch = plain.get();
        ncch_off = 0;
        nh = ncch->pread(0, 0x200);
    }
    const uint32_t exh_size = u32(&nh[0x180]);
    const uint64_t exefs_off = u32(&nh[0x1A0]) * MU, exefs_size = u32(&nh[0x1A4]) * MU;
    const uint64_t romfs_off = u32(&nh[0x1B0]) * MU, romfs_size = u32(&nh[0x1B4]) * MU;

    const Bytes exheader = ncch->pread(ncch_off + 0x200, 0x800);
    if (exheader.size() != 0x800) fail("short exheader");
#ifdef RECOMP_TITLE_ID
    // this executable runs one title's recompiled code: another game's dump, or another region's (other code), can't
    if (hex64(u64(&exheader[0x200])) != RECOMP_TITLE_ID)
        fail("this dump is " + json_text(&exheader[0], 8) + " (" + hex64(u64(&exheader[0x200])) +
             "); this program was made for the title " + RECOMP_TITLE_ID + ", a dump of that one is needed");
#endif
    std::error_code ec;
    fs::create_directories(fs::path(out) / "exefs", ec);  // (only once the dump is the right one)
    if (ec) fail("cannot make " + out);
    write_file(fs::path(out) / "exheader.bin", exheader.data(), exheader.size());

    const Bytes exefs = ncch->pread(ncch_off + exefs_off, exefs_size);
    if (exefs.size() < 0x200) fail("short ExeFS");
    Bytes code;
    for (int i = 0; i < 10; i++) {
        const char *nm = reinterpret_cast<const char *>(&exefs[i * 16]);
        const std::string name(nm, strnlen(nm, 8));
        if (name.empty()) continue;
        const uint32_t off = u32(&exefs[i * 16 + 8]), sz = u32(&exefs[i * 16 + 12]);
        if (0x200ull + off + sz > exefs.size()) fail("ExeFS file " + name + " runs past its end");
        const Bytes data(exefs.begin() + 0x200 + off, exefs.begin() + 0x200 + off + sz);
        const std::string file_name = (name[0] == '.' ? name.substr(1) : name) + ".bin";
        write_file(fs::path(out) / "exefs" / file_name, data.data(), data.size());
        if (name == ".code") code = data;
    }
    if (code.empty()) fail("no .code in the ExeFS: is this a game's program partition?");
    if (exheader[0xD] & 1) code = lzss_decompress(code);
    write_file(fs::path(out) / "code.bin", code.data(), code.size());

    uint64_t l3_size = 0, l3_off = 0;
    if (romfs_size) {
        const Bytes ivfc = ncch->pread(ncch_off + romfs_off, 0x60);
        if (ivfc.size() < 0x60 || std::memcmp(ivfc.data(), "IVFC", 4) != 0)
            fail("bad IVFC (an encrypted dump whose keys didn't fit?)");
        const uint32_t master_hash = u32(&ivfc[8]);
        const uint64_t l3_bs = 1ull << u32(&ivfc[0x4C]);  // level 3 block size
        l3_size = u64(&ivfc[0x44]);
        l3_off = (0x60 + master_hash + l3_bs - 1) & ~(l3_bs - 1);
        // streamed in 16 MiB pieces: a RomFS is up to a few GB
        std::ofstream f(fs::path(out) / "romfs.bin", std::ios::binary | std::ios::trunc);
        if (!f) fail("cannot write romfs.bin");
        for (uint64_t done = 0; done < l3_size;) {
            const Bytes part = ncch->pread(ncch_off + romfs_off + l3_off + done, std::min<uint64_t>(16 << 20, l3_size - done));
            if (part.empty()) fail("the dump ends inside the RomFS");
            f.write(reinterpret_cast<const char *>(part.data()), std::streamsize(part.size()));
            done += part.size();
        }
        if (!f) fail("cannot write romfs.bin");
    }

    // a short info.json for the tools (the runtime reads the three files above)
    std::string j = "{\n";
    j += "  \"title\": \"" + json_text(&exheader[0], 8) + "\",\n";
    j += "  \"program_id\": \"" + hex64(u64(&exheader[0x200])) + "\",\n";
    j += "  \"ncch_program_id\": \"" + hex64(u64(&nh[0x118])) + "\",\n";
    j += "  \"product_code\": \"" + json_text(&nh[0x150], 16) + "\",\n";
    j += "  \"compressed\": " + std::string(exheader[0xD] & 1 ? "true" : "false") + ",\n";
    j += "  \"exheader_size\": " + std::to_string(exh_size) + ",\n";
    j += "  \"code_size\": " + std::to_string(code.size()) + ",\n";
    j += "  \"romfs_level3_offset\": " + std::to_string(l3_off) + ",\n";
    j += "  \"romfs_size\": " + std::to_string(l3_size) + "\n}\n";
    write_file(fs::path(out) / "info.json", reinterpret_cast<const uint8_t *>(j.data()), j.size());
    std::printf("setup: %s (%s) into %s: code %zu bytes, RomFS %llu bytes\n", json_text(&exheader[0], 8).c_str(),
                hex64(u64(&exheader[0x200])).c_str(), out.c_str(), code.size(), (unsigned long long)l3_size);
    return 0;
}

}  // namespace setup
