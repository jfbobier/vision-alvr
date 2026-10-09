#pragma once
#include "alvr_server/bindings.h"
#include <windows.h>
#include <string>

// same as upstream Utils.h (the rest of that header is OpenVR-only)
inline std::wstring GetErrorStr(HRESULT hr) {
    wchar_t* s = NULL;
    std::wstring ret;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, hr,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&s, 0, NULL);
    if (s) { ret = s; LocalFree(s); }
    while (!ret.empty() && (ret.back() == L'\n' || ret.back() == L'\r')) ret.pop_back();
    return ret;
}
