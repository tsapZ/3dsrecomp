// The Skylanders 3DS Portal of Power, emulated for ir:USER (runtime/services/misc.cpp). The five Skylanders 3DS games
// (Spyro's Adventure, Giants, Swap Force, Trap Team, SuperChargers Racing) talk to their battery portal over infrared
// once AutoConnection connected it: every packet is the portal light's colour (r g b) and a console portal command
// (R, A, C, S, Q, W), answered like skylanders-portal's SoftwarePortal with one figure on slot 0. Ported from
// tsapZ/azahar (src/core/hle/service/ir/skylanders_portal.cpp, same author).
//
// The figure: a 1024-byte figure file (the same files as the Xbox 360 ports use), read once and never written back
// (what the game writes stays in memory). SKYLANDERS_PORTAL_FIGURE names figure files separated by ';': the first goes
// on at the first connection, and after the game has read one and reconnects, the next. place() and lift() change it
// while the game runs. SKYLANDERS_PORTAL_SCRIPT times toys for scripted runs: "<seconds>:lift" or "<seconds>:place
// <file>", separated by ';', in seconds since the program started (SuperChargers Racing takes a vehicle only when it is
// put on while the game asks for it).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "rt.h"

namespace skylanders_portal {
using Send = std::function<void(const std::vector<u8> &)>;

void on_connect();
void on_disconnect();
// a packet from the game; replies go through send
void on_receive(const u8 *data, u32 size, const Send &send);

bool place(const std::string &path);
void lift();
bool present();
// the figure file's name without its folder and extension ("" when the portal is empty)
std::string figure_name();
}  // namespace skylanders_portal
