// runtime/services/skylanders_portal.h
#include "skylanders_portal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>

#include "settings.h"
#if RECOMP_USB_PORTAL
#include "portal/usb/usb_portal.h"  // runtime/third_party/skylanders_portal (Windows builds)
#endif

#if RECOMP_USB_PORTAL
// skylanders-portal's REXLOG_* lines (runtime/third_party/skylanders_portal/shim/rex/logging.h) in this log
namespace skylanders_portal_log {
void Line(const char *level, const std::string &text) { LOG("[portal usb] %s: %s\n", level, text.c_str()); }
}  // namespace skylanders_portal_log
#endif

namespace skylanders_portal {
namespace {

#if RECOMP_USB_PORTAL
// The real portal (the menu's "Real portal (USB)", g_settings.real_portal; SKYLANDERS_PORTAL=usb for a run): the
// game's infrared packets go to a USB Portal of Power as the same commands (the 3DS portal speaks the console portal's
// protocol, after the light's colour), its answers come back. skylanders-portal's UsbPortal (HID, or WinUSB when a
// player replaced the driver) as the Xbox 360 ports use it; what the game writes goes onto the real figure, as on a
// console. Opened at the first packet with the setting on, again after the setting changed.
std::unique_ptr<skylanders::portal::UsbPortal> usb;  // (under mx)
bool usb_tried = false;

bool want_usb() {
    static const char *env = getenv("SKYLANDERS_PORTAL");
    if (env) return std::string(env) == "usb";
    return g_settings.real_portal;
}

skylanders::portal::UsbPortal *usb_locked() {
    if (!want_usb()) {
        usb.reset();
        usb_tried = false;
        return nullptr;
    }
    if (!usb && !usb_tried) {
        usb_tried = true;
        usb = std::make_unique<skylanders::portal::UsbPortal>();
        if (!usb->IsOpen()) {
            LOG("[portal] no real portal found (USB): the game sees an empty portal\n");
            usb.reset();
        } else {
            LOG("[portal] real portal (USB) opened\n");
        }
    }
    return usb.get();
}

// (lock held) one packet of the game on the real portal: the answer as the infrared side sends it (the console
// reply's length, as the emulated portal below), or nothing when the command has none or no answer came.
std::vector<u8> relay_locked(skylanders::portal::UsbPortal &p, const u8 *c, u32 n) {
    using skylanders::portal::Report;
    const u8 cmd = c[0];
    auto trimmed = [cmd](const Report &r) {
        size_t len = 0;
        switch (cmd) {
        case 'R': len = 3; break;
        case 'A': len = 4; break;
        case 'S': len = 7; break;   // S, four bytes of slot bits, the counter, on/off
        case 'Q': len = 19; break;  // Q, slot, block, 16 bytes
        case 'W': len = 3; break;
        }
        return std::vector<u8>(r.begin(), r.begin() + len);
    };
    if (cmd == 'S') {  // the portal reports its status by itself: the newest report
        for (int i = 0; i < 16; i++) {
            const Report r = p.Read();
            if (r[0] == 'S') return trimmed(r);
        }
        return {};
    }
    Report out{};
    std::copy_n(c, std::min<u32>(n, (u32)out.size()), out.begin());
    p.Write(out);
    if (cmd == 'J') return {0x4a};  // a light fade: older portals don't answer it, the game only wants the echo
    if (cmd != 'R' && cmd != 'A' && cmd != 'Q' && cmd != 'W') return {};  // C, L and the rest: no answer
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) {
        const Report r = p.Read();
        if (r[0] != cmd) continue;
        if ((cmd == 'Q' || cmd == 'W') && n >= 3 && ((r[1] & 0x0f) != (c[1] & 0x0f) || r[2] != c[2])) continue;
        return trimmed(r);
    }
    LOG("[portal] the real portal didn't answer %c in 300 ms\n", cmd);
    return {};
}
#endif

// status reports that say "just put on" before "on" (skylanders-portal's kAddedReports)
constexpr int kAddedReports = 8;

// Two slots: one figure, or a Swapper's two halves (Swap Force: its top and its bottom are two toys the portal reads
// side by side; Core of Light #72: "Swappers need both top and bottom")
constexpr int kSlots = 2;

struct Slot {
    std::array<u8, 1024> figure{};
    bool on = false;
    std::string file;         // (UTF-8)
    u32 generation = 0;       // changes with every place / lift, so the device tells the game
    // the device's state for the current connection
    u32 seen_generation = 0;  // the figure's generation as last reported
    bool lifting = false;     // the next status says the old figure is being lifted
    u8 state = 0;             // status bits: 0 empty, 1 on, 2 lifted, 3 just put on
    int added_reports = 0;    // status reports still to say "just put on"
};

std::mutex mx;
Slot slots[kSlots];
bool connected_once = false;  // the game connected the portal at least once (in_use(): the menu's Portal page)
bool active = false;      // the game turned the portal on (A 01)
u8 counter = 0;           // S replies count up
int reads = 0;            // blocks the game read since a figure went on

bool any_on() {
    for (const Slot &s : slots)
        if (s.on) return true;
    return false;
}

// (lock held) every figure off the portal
void lift_all_locked() {
    for (Slot &s : slots) {
        if (!s.on) continue;
        s.on = false;
        s.generation++;
    }
}

// A Swapper half by its toy id (block 1, bytes 0-1, little-endian, as skylanders-portal reads it): Swap Force's
// bottoms are 1000-1999, its tops 2000-2999. 1 bottom, 2 top, 0 anything else.
int swapper_half(const std::array<u8, 1024> &f) {
    const unsigned id = f[16] | (f[17] << 8);
    return id >= 1000 && id < 2000 ? 1 : id >= 2000 && id < 3000 ? 2 : 0;
}
u32 packets = 0;          // from the game, this connection
bool list_loaded = false;
std::vector<std::string> list;  // SKYLANDERS_PORTAL_FIGURE, in order
size_t current = 0;
// SKYLANDERS_PORTAL_SCRIPT, for scripted runs: "<seconds>:lift" or "<seconds>:place <file>", separated by ';', in
// seconds since the program started, each done with the first packet or connection after its time (SuperChargers
// Racing takes a vehicle only when it is put on while the game asks for it)
bool script_loaded = false;
std::vector<std::pair<double, std::string>> script;
size_t script_next = 0;
const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

bool place_locked(const std::string &path) {
    // (a UTF-8 path: the file dialog's; std::filesystem opens it on Windows as well)
    std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!in) {
        LOG("[portal] cannot open figure file %s\n", path.c_str());
        return false;
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::array<u8, 1024> data{};
    if (bytes.size() != data.size()) {
        LOG("[portal] %s is not a 1024-byte figure file\n", path.c_str());
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), data.begin());
    // A Swapper half next to the other half stays with it: they make one Swapper. Anything else replaces what is on
    // the portal (one figure at a time, as the menu and F5 put them on).
    const int half = swapper_half(data);
    int target = 0;
    bool pairing = false;
    for (int i = 0; half && i < kSlots; i++) {
        if (slots[i].on && swapper_half(slots[i].figure) == 3 - half) {
            target = (i + 1) % kSlots;
            pairing = true;
        }
    }
    if (!pairing) lift_all_locked();
    Slot &s = slots[target];
    s.figure = data;
    s.on = true;
    s.file = path;
    s.generation++;
    LOG("[portal] figure on the portal (slot %d): %s\n", target, path.c_str());
    return true;
}

// (lock held) one item of SKYLANDERS_PORTAL_FIGURE: a file, or a Swapper's two halves as "<top>|<bottom>"
void place_item_locked(const std::string &item) {
    size_t start = 0;
    while (start <= item.size()) {
        size_t end = item.find('|', start);
        if (end == std::string::npos) end = item.size();
        if (end > start) place_locked(item.substr(start, end - start));
        start = end + 1;
    }
}

// a slot's status bits for this report: a figure placed or lifted meanwhile is reported as the old one lifted once,
// then the new one as just put on (as a real portal and skylanders-portal's SoftwarePortal do)
u8 slot_state(Slot &s) {
    if (s.generation != s.seen_generation) {
        s.seen_generation = s.generation;
        reads = 0;
        if (active) {
            s.lifting = s.state != 0;
            s.state = s.on ? 3 : 0;
            s.added_reports = kAddedReports;
        }
    }
    if (!active) return 0;
    if (s.lifting) {
        s.lifting = false;
        return 2;
    }
    const u8 now = s.state;
    if (s.state == 3 && --s.added_reports <= 0) s.state = 1;
    return now;
}

// (lock held) the script's actions that are due
void run_script_locked() {
    if (!script_loaded) {
        script_loaded = true;
        const char *env = getenv("SKYLANDERS_PORTAL_SCRIPT");
        const std::string text = env ? env : "";
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find(';', start);
            if (end == std::string::npos) end = text.size();
            const std::string item = text.substr(start, end - start);
            const size_t colon = item.find(':');
            if (colon != std::string::npos && colon > 0) {
                std::string action = item.substr(colon + 1);
                action.erase(0, action.find_first_not_of(' '));
                script.emplace_back(atof(item.substr(0, colon).c_str()), action);
            }
            start = end + 1;
        }
        std::stable_sort(script.begin(), script.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    }
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    while (script_next < script.size() && script[script_next].first <= now) {
        const std::string &action = script[script_next].second;
        LOG("[portal] script at %.1f s: %s\n", now, action.c_str());
        if (action == "lift") {
            lift_all_locked();
        } else if (action.rfind("place ", 0) == 0) {
            place_locked(action.substr(6));
        } else {
            LOG("[portal] unknown script action '%s'\n", action.c_str());
        }
        script_next++;
    }
}

}  // namespace

bool place(const std::string &path) {
    std::lock_guard<std::mutex> lk(mx);
    return place_locked(path);
}

void lift() {
    std::lock_guard<std::mutex> lk(mx);
    lift_all_locked();
    LOG("[portal] figure lifted off\n");
}

bool present() {
    std::lock_guard<std::mutex> lk(mx);
    return any_on();
}

bool in_use() {
    std::lock_guard<std::mutex> lk(mx);
    return connected_once;
}

bool needs_other_half() {
    std::lock_guard<std::mutex> lk(mx);
    int on = 0, half = 0;
    for (const Slot &s : slots) {
        if (!s.on) continue;
        on++;
        half = swapper_half(s.figure);
    }
    return on == 1 && half != 0;
}

std::string figure_name() {
    std::lock_guard<std::mutex> lk(mx);
    std::string out;  // "Blast Zone Top + Blast Zone Bottom" for a Swapper's two halves
    for (const Slot &s : slots) {
        if (!s.on) continue;
        const auto p = std::filesystem::path(std::u8string(s.file.begin(), s.file.end())).stem().u8string();
        out += (out.empty() ? "" : " + ") + std::string(p.begin(), p.end());
    }
    return out;
}

bool real_portal_supported() {
#if RECOMP_USB_PORTAL
    return true;
#else
    return false;
#endif
}

void set_real_portal(bool on) {
    std::lock_guard<std::mutex> lk(mx);
    g_settings.real_portal = on;
#if RECOMP_USB_PORTAL
    usb.reset();  // opened (or looked for) again at the game's next packet
    usb_tried = false;
#endif
}

std::string real_portal_state() {
    std::lock_guard<std::mutex> lk(mx);
#if RECOMP_USB_PORTAL
    if (!want_usb()) return "Off";
    if (usb) return usb->SeemsDisconnected() ? "Unplugged?" : "Connected";
    return usb_tried ? "Not found" : "Waiting for the game";
#else
    return "Off";
#endif
}

void on_connect() {
    std::lock_guard<std::mutex> lk(mx);
    connected_once = true;
    run_script_locked();
    if (!list_loaded) {
        list_loaded = true;
        const char *env = getenv("SKYLANDERS_PORTAL_FIGURE");
        std::string text = env ? env : "";
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find(';', start);
            if (end == std::string::npos) end = text.size();
            if (end > start) list.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        if (!list.empty() && !any_on()) place_item_locked(list[0]);
    } else if (reads > 0 && current + 1 < list.size()) {
        // the game reconnects after it has read a figure: now the list's next one stands there
        place_item_locked(list[++current]);
    }
    // each connection starts over: the game activates the portal again, which announces the figure as just put on
    reads = 0;
    packets = 0;
    for (Slot &s : slots) {
        s.seen_generation = s.generation;
        s.lifting = false;
        s.state = 0;
    }
    active = false;
    LOG("[portal] connected\n");
}

void on_disconnect() {
    std::lock_guard<std::mutex> lk(mx);
    LOG("[portal] disconnected after %u packets from the game\n", packets);
}

void on_receive(const u8 *data, u32 size, const Send &send) {
    std::vector<u8> reply;
    {
        std::lock_guard<std::mutex> lk(mx);
        packets++;
        run_script_locked();
        // every packet starts with the portal light's colour (r g b), then the command
        if (size <= 3) return;
        const u8 *c = data + 3;
        const u32 n = size - 3;
#if RECOMP_USB_PORTAL
        if (auto *p = usb_locked()) {
            reply = relay_locked(*p, c, n);
            if (reply.empty()) return;
            goto answer;
        }
#endif
        switch (c[0]) {
        case 'J':  // a light fade on one side (Trap Team portals): acknowledged with a bare 4A, as skylanders-portal's
                   // SoftwarePortal does (SuperChargers Racing sends one pair every 3 s while it waits for a Skylander)
            reply = {0x4a};
            break;
        case 'R':  // ready: a Trap Team portal, as the USB ones answer
            reply = {0x52, 0x02, 0x1b};
            break;
        case 'A': {  // activate (argument != 0) or deactivate
            const u8 arg = n > 1 ? c[1] : 0;
            for (Slot &s : slots) {
                if (arg && !active && s.on) {
                    s.state = 3;  // reported as just put on when the portal turns on
                    s.added_reports = kAddedReports;
                } else if (!arg) {
                    s.state = 0;  // the figure stays, it just isn't reported
                }
                s.seen_generation = s.generation;
                s.lifting = false;
            }
            active = arg != 0;
            reply = {0x41, arg, 0xff, 0x77};
            break;
        }
        case 'S': {  // status: two bits a slot (slot 0 in the low bits), a counter, whether the portal is on
            u8 bits = 0;
            for (int i = 0; i < kSlots; i++) bits |= (u8)(slot_state(slots[i]) << (2 * i));
            reply = {0x53, bits, 0, 0, 0, counter++, (u8)(active ? 1 : 0)};
            break;
        }
        case 'Q': {  // read a block: 51, 0x10 | slot when there is a figure, the block, 16 bytes
            const u8 slot = n > 1 ? (u8)(c[1] & 0x0f) : 0, block = n > 2 ? c[2] : 0;
            reply = {0x51, slot, block};
            reply.resize(3 + 16);
            if (slot < kSlots && slots[slot].on && block < 64) {
                reply[1] |= 0x10;
                std::copy_n(slots[slot].figure.begin() + block * 16, 16, reply.begin() + 3);
                reads++;
            }
            break;
        }
        case 'W': {  // write a block: kept in memory only, the file is never written
            const u8 slot = n > 1 ? (u8)(c[1] & 0x0f) : 0, block = n > 2 ? c[2] : 0;
            reply = {0x57, slot, block};
            if (slot < kSlots && slots[slot].on && block < 64 && n >= 3 + 16) {
                reply[1] |= 0x10;
                std::copy_n(c + 3, 16, slots[slot].figure.begin() + block * 16);
                LOG("[portal] the game wrote slot %u block %u\n", slot, block);
            }
            break;
        }
        default:  // C (the light) and the rest: no answer
            if (getenv("SKYLANDERS_PORTAL_LOG")) {
                char in[16] = {};
                for (u32 i = 1; i < n && i < 5; i++) snprintf(in + (i - 1) * 3, 4, "%02x ", c[i]);
                LOG("[portal] %c (%02x) %s(%u bytes, no answer)\n", c[0] >= 0x20 && c[0] < 0x7f ? c[0] : '?', c[0], in,
                    size);
            }
            return;
        }
#if RECOMP_USB_PORTAL
    answer:
#endif
        // SKYLANDERS_PORTAL_LOG: what the game asks and gets, every packet (the light and other commands without an
        // answer above), to see what a game does with a figure
        static const bool trace = getenv("SKYLANDERS_PORTAL_LOG") != nullptr;
        if (trace) {  // (all of them: a game sends a few a second)
            char in[16] = {}, out[16] = {};
            for (u32 i = 1; i < n && i < 5; i++) snprintf(in + (i - 1) * 3, 4, "%02x ", c[i]);
            for (size_t i = 1; i < reply.size() && i < 5; i++) snprintf(out + (i - 1) * 3, 4, "%02x ", reply[i]);
            LOG("[portal] %c %s-> %s(%s)\n", c[0], in, out, any_on() ? "figure on" : "empty");
        }
    }
    send(reply);
}

}  // namespace skylanders_portal
