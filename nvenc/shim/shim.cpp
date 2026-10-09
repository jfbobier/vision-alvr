// Replacements for Logger.cpp / Settings.cpp / Rust FFI used by the unmodified upstream encoder files.
#include "alvr_server/Settings.h"
#include "alvr_server/Logger.h"
#include "alvr_server/bindings.h"
#include "shim.h"
#include <cstdarg>
#include <cstdio>

Settings::Settings() {}

Exception MakeException(const char* format, ...) {
    va_list a; va_start(a, format); Exception e = FormatExceptionV(format, a); va_end(a); return e;
}
static void vlog(const char* lvl, const char* f, va_list a) {
    char buf[1024]; vsnprintf(buf, sizeof buf, f, a);
    if (g_shim.verbose || lvl[0] == 'E' || lvl[0] == 'W') fprintf(stderr, "[%s] %s", lvl, buf);
}
void Error(const char* f, ...) { va_list a; va_start(a, f); vlog("ERROR", f, a); va_end(a); }
void Warn(const char* f, ...)  { va_list a; va_start(a, f); vlog("WARN", f, a); va_end(a); }
void Info(const char* f, ...)  { va_list a; va_start(a, f); vlog("INFO", f, a); va_end(a); }
void Debug(const char* f, ...) { va_list a; va_start(a, f); vlog("DEBUG", f, a); va_end(a); }

ShimState g_shim;

static FfiDynamicEncoderParams dyn() {
    FfiDynamicEncoderParams p{};
    if (g_shim.params_cb) {
        unsigned long long br = 0; float fr = 0;
        if (g_shim.params_cb(g_shim.user, &br, &fr)) { p.updated = 1; p.bitrate_bps = br; p.framerate = fr; }
        return p;
    }
    if (!g_shim.paramsSent) { g_shim.paramsSent = true; p.updated = 1; p.bitrate_bps = g_shim.bitrateBps; p.framerate = g_shim.framerate; }
    return p;
}
FfiDynamicEncoderParams (*GetDynamicEncoderParams)() = dyn;

void ParseFrameNals(int, unsigned char* buf, int len, unsigned long long ts, bool isIdr) {
    if (g_shim.packet_cb) { g_shim.packet_cb(g_shim.user, buf, len, ts, isIdr); return; }
    g_shim.packets.push_back({ (size_t)len, ts, isIdr });
    if (g_shim.out) fwrite(buf, 1, len, g_shim.out);
}
