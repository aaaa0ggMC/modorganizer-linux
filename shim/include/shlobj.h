#pragma once
#include "windows.h"
#include "knownfolders.h"
#define CSIDL_PERSONAL 0x0005
#define CSIDL_LOCAL_APPDATA 0x001c
#define CSIDL_APPDATA 0x001a
#define CSIDL_COMMON_APPDATA 0x0023
// 返回值用 CoTaskMemAlloc 分配（此处为 malloc），调用方用 CoTaskMemFree 释放。
HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, wchar_t** out);
void CoTaskMemFree(void*);
