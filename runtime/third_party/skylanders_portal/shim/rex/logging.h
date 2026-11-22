// The REXLOG_* macros skylanders-portal's USB portal uses, on the 3DS runtime's own log (rt.h's LOG): the same
// fmt-style format strings, formatted with std::format. Only the files in runtime/third_party/skylanders_portal
// include this (their include path puts it before any other rex/).
#pragma once

#include <format>
#include <string>

namespace skylanders_portal_log {
void Line(const char* level, const std::string& text);  // runtime/services/skylanders_portal.cpp
}  // namespace skylanders_portal_log

#define REXLOG_TRACE(...) ((void)0)
#define REXLOG_DEBUG(...) ((void)0)
#define REXLOG_INFO(...) ::skylanders_portal_log::Line("info", std::format(__VA_ARGS__))
#define REXLOG_WARN(...) ::skylanders_portal_log::Line("warning", std::format(__VA_ARGS__))
#define REXLOG_ERROR(...) ::skylanders_portal_log::Line("error", std::format(__VA_ARGS__))
