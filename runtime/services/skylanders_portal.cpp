// runtime/services/skylanders_portal.h
#include "skylanders_portal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace skylanders_portal {
namespace {

// status reports that say "just put on" before "on" (skylanders-portal's kAddedReports)
constexpr int kAddedReports = 8;

std::mutex mx;
std::array<u8, 1024> figure{};
bool figure_on = false;
u32 generation = 0;  // changes with every place / lift, so the device tells the game

// the device's state for the current connection
bool active = false;      // the game turned the portal on (A 01)
u8 counter = 0;           // S replies count up
u32 seen_generation = 0;  // the figure's generation as last reported
bool lifting = false;     // the next status says the old figure is being lifted
u8 state = 0;             // slot 0's status bits: 0 empty, 1 on, 2 lifted, 3 just put on
int added_reports = 0;    // status reports still to say "just put on"
int reads = 0;            // blocks the game read since the figure went on
u32 packets = 0;          // from the game, this connection
bool list_loaded = false;
std::vector<std::string> list;  // SKYLANDERS_PORTAL_FIGURE, in order
size_t current = 0;

bool place_locked(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        LOG("[portal] cannot open figure file %s\n", path.c_str());
        return false;
    }
    std::array<u8, 1024> d{};
    const size_t n = fread(d.data(), 1, d.size(), f);
    const bool more = fgetc(f) != EOF;
    fclose(f);
    if (n != d.size() || more) {
        LOG("[portal] %s is not a 1024-byte figure file\n", path.c_str());
        return false;
    }
    figure = d;
    figure_on = true;
    generation++;
    LOG("[portal] figure on the portal: %s\n", path.c_str());
    return true;
}

// slot 0's status bits for this report: a figure placed or lifted meanwhile is reported as the old one lifted once,
// then the new one as just put on (as a real portal and skylanders-portal's SoftwarePortal do)
u8 slot_state() {
    if (generation != seen_generation) {
        seen_generation = generation;
        reads = 0;
        if (active) {
            lifting = state != 0;
            state = figure_on ? 3 : 0;
            added_reports = kAddedReports;
        }
    }
    if (!active) return 0;
    if (lifting) {
        lifting = false;
        return 2;
    }
    const u8 now = state;
    if (state == 3 && --added_reports <= 0) state = 1;
    return now;
}

}  // namespace

bool place(const std::string &path) {
    std::lock_guard<std::mutex> lk(mx);
    return place_locked(path);
}

void lift() {
    std::lock_guard<std::mutex> lk(mx);
    figure_on = false;
    generation++;
    LOG("[portal] figure lifted off\n");
}

bool present() {
    std::lock_guard<std::mutex> lk(mx);
    return figure_on;
}

void on_connect() {
    std::lock_guard<std::mutex> lk(mx);
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
        if (!list.empty() && !figure_on) place_locked(list[0]);
    } else if (reads > 0 && current + 1 < list.size()) {
        // the game reconnects after it has read a figure: now the list's next one stands there
        place_locked(list[++current]);
    }
    // each connection starts over: the game activates the portal again, which announces the figure as just put on
    reads = 0;
    packets = 0;
    seen_generation = generation;
    active = false;
    lifting = false;
    state = 0;
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
        // every packet starts with the portal light's colour (r g b), then the command
        if (size <= 3) return;
        const u8 *c = data + 3;
        const u32 n = size - 3;
        switch (c[0]) {
        case 'R':  // ready: a Trap Team portal, as the USB ones answer
            reply = {0x52, 0x02, 0x1b};
            break;
        case 'A': {  // activate (argument != 0) or deactivate
            const u8 arg = n > 1 ? c[1] : 0;
            if (arg && !active && figure_on) {
                state = 3;  // reported as just put on when the portal turns on
                added_reports = kAddedReports;
            } else if (!arg) {
                state = 0;  // the figure stays, it just isn't reported
            }
            active = arg != 0;
            seen_generation = generation;
            lifting = false;
            reply = {0x41, arg, 0xff, 0x77};
            break;
        }
        case 'S': {  // status: two bits a slot, a counter, whether the portal is on
            const u8 slot0 = slot_state();
            reply = {0x53, slot0, 0, 0, 0, counter++, (u8)(active ? 1 : 0)};
            break;
        }
        case 'Q': {  // read a block: 51, 0x10 | slot when there is a figure, the block, 16 bytes
            const u8 slot = n > 1 ? (u8)(c[1] & 0x0f) : 0, block = n > 2 ? c[2] : 0;
            reply = {0x51, slot, block};
            reply.resize(3 + 16);
            if (slot == 0 && figure_on && block < 64) {
                reply[1] |= 0x10;
                std::copy_n(figure.begin() + block * 16, 16, reply.begin() + 3);
                reads++;
            }
            break;
        }
        case 'W': {  // write a block: kept in memory only, the file is never written
            const u8 slot = n > 1 ? (u8)(c[1] & 0x0f) : 0, block = n > 2 ? c[2] : 0;
            reply = {0x57, slot, block};
            if (slot == 0 && figure_on && block < 64 && n >= 3 + 16) {
                reply[1] |= 0x10;
                std::copy_n(c + 3, 16, figure.begin() + block * 16);
                LOG("[portal] the game wrote block %u\n", block);
            }
            break;
        }
        default:  // C (the light) and the rest: no answer
            return;
        }
    }
    send(reply);
}

}  // namespace skylanders_portal
