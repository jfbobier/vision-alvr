// Windows helpers to create-or-open the named IPC objects. Used by OVRShim and by the host's C++ library.
// The objects get a permissive DACL and a Low mandatory label so a non-elevated (medium integrity) OpenXR app can
// share them with an elevated host (SSH sessions are elevated; the OpenXR loader ignores XR_RUNTIME_JSON when elevated).
#pragma once
#include "ipc.h"
#include <windows.h>
#include <sddl.h>
#include <string>

namespace visionalvr_ipc {

struct Ipc {
    HANDLE map = nullptr;
    IpcState* state = nullptr;
    HANDLE vsync = nullptr;
    HANDLE frame = nullptr;
    bool created = false; // this process created the mapping (zero-initialized by the OS)
};

inline std::wstring SessionPrefix() {
    wchar_t buf[128];
    DWORD n = GetEnvironmentVariableW(L"OVRSHIM_SESSION", buf, 128);
    return (n > 0 && n < 128) ? std::wstring(buf) : std::wstring(L"VisionALVR");
}

inline void CloseIpc(Ipc& ipc) {
    if (ipc.state) UnmapViewOfFile(ipc.state);
    if (ipc.map) CloseHandle(ipc.map);
    if (ipc.vsync) CloseHandle(ipc.vsync);
    if (ipc.frame) CloseHandle(ipc.frame);
    ipc = {};
}

// Namespace the objects ended up in ("Global\\" or "Local\\"), for logs.
inline const wchar_t*& IpcNamespace() {
    static const wchar_t* ns = L"";
    return ns;
}

inline bool OpenIpc(Ipc& ipc) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;

    // Global\ is needed when host and app run in different Windows sessions (harness: elevated SSH host); creating there needs
    // SeCreateGlobalPrivilege (elevated), opening does not. An elevated process can therefore create a private Global\ object
    // that a non-elevated peer never sees (it uses Local\): so first look for objects that already EXIST in either
    // namespace, and only create one (Global\ if allowed, else Local\) when the other side has not started yet.
    const wchar_t* order[2] = {L"Global\\", L"Local\\"};
    int existing = -1;
    for (int k = 0; k < 2 && existing < 0; k++) {
        if (HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (std::wstring(order[k]) + SessionPrefix() + L"_State").c_str())) {
            CloseHandle(h);
            existing = k;
        }
    }
    for (int k = 0; k < 2; k++) {
        if (existing >= 0 && k != existing) {
            continue;
        }
        const wchar_t* ns = order[k];
        const std::wstring p = std::wstring(ns) + SessionPrefix();
        ipc.map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(IpcState), (p + L"_State").c_str());
        if (!ipc.map) {
            continue;
        }
        ipc.created = GetLastError() != ERROR_ALREADY_EXISTS;
        IpcNamespace() = ns;
        ipc.state = (IpcState*)MapViewOfFile(ipc.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IpcState));
        if (ipc.state && (ipc.state->magic != kMagic || ipc.state->version != kVersion)) {
            if (ipc.created || ipc.state->magic == 0) {
                ipc.state->magic = kMagic;
                ipc.state->version = kVersion;
            } else {
                UnmapViewOfFile(ipc.state);
                ipc.state = nullptr; // incompatible version: refuse
            }
        }
        ipc.vsync = CreateEventW(&sa, FALSE, FALSE, (p + L"_Vsync").c_str());
        ipc.frame = CreateEventW(&sa, FALSE, FALSE, (p + L"_Frame").c_str());
        if (ipc.state && ipc.vsync && ipc.frame) {
            break;
        }
        CloseIpc(ipc);
    }
    if (sd) LocalFree(sd);
    return ipc.state && ipc.vsync && ipc.frame;
}

inline double QpcSeconds() {
    static const double inv = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1.0 / (double)f.QuadPart; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * inv; // identical to ovrnull::utils::QpcToOvrTime
}

} // namespace visionalvr_ipc
