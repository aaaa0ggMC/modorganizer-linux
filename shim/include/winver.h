#pragma once
#include "windows.h"
struct VS_FIXEDFILEINFO {
    DWORD dwSignature, dwStrucVersion, dwFileVersionMS, dwFileVersionLS, dwProductVersionMS,
        dwProductVersionLS, dwFileFlagsMask, dwFileFlags, dwFileOS, dwFileType, dwFileSubtype,
        dwFileDateMS, dwFileDateLS;
};
// 通过解析 PE 的 VS_VERSION_INFO 资源实现，见 shim/src/version.cpp。
DWORD GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle);
BOOL GetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD len, LPVOID data);
BOOL VerQueryValueW(LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, UINT* len);
