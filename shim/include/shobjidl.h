#pragma once
#include "windows.h"
// 仅供 taskprogressmanager（任务栏进度）引用；Linux 上无此功能。
enum TBPFLAG { TBPF_NOPROGRESS = 0, TBPF_INDETERMINATE = 1, TBPF_NORMAL = 2, TBPF_ERROR = 4, TBPF_PAUSED = 8 };
struct ITaskbarList3 {
    HRESULT SetProgressState(HWND, TBPFLAG) { return S_OK; }
    HRESULT SetProgressValue(HWND, ULONGLONG, ULONGLONG) { return S_OK; }
    ULONG Release() { return 0; }
};
