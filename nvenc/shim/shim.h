#pragma once
#include <cstdio>
#include <vector>
struct ShimPacket { size_t len; unsigned long long ts; bool idr; };
struct ShimState {
    bool verbose = false; bool paramsSent = false;
    unsigned long long bitrateBps = 30000000; float framerate = 90;
    FILE* out = nullptr; std::vector<ShimPacket> packets;
    // live host: packets and dynamic encoder params are forwarded to the Rust host
    void (*packet_cb)(void* user, const unsigned char* data, int len, unsigned long long ts, bool idr) = nullptr;
    bool (*params_cb)(void* user, unsigned long long* bitrate_bps, float* framerate) = nullptr;
    void* user = nullptr;
};
extern ShimState g_shim;
