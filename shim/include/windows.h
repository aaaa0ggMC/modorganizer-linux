// mo-linux Windows API shim：只覆盖 uibase / game_bethesda 实际用到的符号。
// 语义参见 shim/src/*.cpp；注意 Linux 上 wchar_t 是 4 字节，WCHAR 同为 wchar_t。
#pragma once
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <strings.h>

// ---- 基本类型 --------------------------------------------------------------
using BYTE = unsigned char;
using WORD = unsigned short;
using DWORD = uint32_t;
using UINT = unsigned int;
using INT = int;
using LONG = int32_t;
using ULONG = uint32_t;
using BOOL = int;
using WCHAR = wchar_t;
using CHAR = char;
using LPDWORD = DWORD*;
using LPVOID = void*;
using LPCVOID = const void*;
using PVOID = void*;
using HANDLE = void*;
using HMODULE = void*;
using HWND = void*;
using HKEY = void*;
using HRESULT = int32_t;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using LPCSTR = const char*;
using LPSTR = char*;
using LONG_PTR = intptr_t;
using ULONG_PTR = uintptr_t;
using DWORD_PTR = uintptr_t;
using SIZE_T = size_t;
using LRESULT = LONG_PTR;
using WPARAM = UINT;
using LPARAM = LONG_PTR;
using UINT_PTR = uintptr_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using PHKEY = HKEY*;
using REGSAM = DWORD;
using LSTATUS = LONG;

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define WINAPI
#define CALLBACK
#define APIENTRY
#define INFINITE 0xFFFFFFFFu
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define L_(x) L##x
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define UNREFERENCED_PARAMETER(P) (void)(P)

union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
};
using LARGE_INTEGER = _LARGE_INTEGER;
union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    ULONGLONG QuadPart;
};
using ULARGE_INTEGER = _ULARGE_INTEGER;
struct _FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; };
using FILETIME = _FILETIME;
struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
using SYSTEMTIME = _SYSTEMTIME;
struct GUID { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; };
using KNOWNFOLDERID = GUID;
using REFKNOWNFOLDERID = const GUID&;
struct SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; };
using LPSECURITY_ATTRIBUTES = SECURITY_ATTRIBUTES*;
struct OVERLAPPED;

// ---- 文件属性 / 访问常量 -------------------------------------------------------
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define CREATE_NEW 1
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define PAGE_READONLY 0x02
#define FILE_MAP_READ 0x4

// ---- 错误码（常用） -----------------------------------------------------------
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_PARAMETER 87
#define ERROR_MORE_DATA 234
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_ALREADY_EXISTS 183
#define ERROR_NOT_FOUND 1168
#define ERROR_CANCELLED 1223
#define ERROR_ELEVATION_REQUIRED 740
#define ERROR_BAD_EXE_FORMAT 193

// ---- 注册表 ------------------------------------------------------------------
#define HKEY_CLASSES_ROOT ((HKEY)(uintptr_t)0x80000000)
#define HKEY_CURRENT_USER ((HKEY)(uintptr_t)0x80000001)
#define HKEY_LOCAL_MACHINE ((HKEY)(uintptr_t)0x80000002)
#define KEY_READ 0x20019
#define KEY_QUERY_VALUE 0x1
#define KEY_WOW64_32KEY 0x200
#define KEY_WOW64_64KEY 0x100
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_DWORD 4

// ---- 函数声明（实现见 shim/src） -------------------------------------------------
DWORD GetLastError();
void SetLastError(DWORD);
HANDLE CreateFileW(LPCWSTR, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES, DWORD disposition,
                   DWORD flags, HANDLE templ);
BOOL CloseHandle(HANDLE);
BOOL GetFileSizeEx(HANDLE, LARGE_INTEGER*);
BOOL ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, OVERLAPPED*);
DWORD GetFileAttributesW(LPCWSTR);
BOOL DeleteFileW(LPCWSTR);
BOOL MoveFileExW(LPCWSTR, LPCWSTR, DWORD flags);
#define MOVEFILE_REPLACE_EXISTING 0x1
#define MOVEFILE_COPY_ALLOWED 0x2
DWORD FormatMessageW(DWORD flags, LPCVOID src, DWORD msgId, DWORD langId, LPWSTR buf, DWORD size, void* args);
#define FORMAT_MESSAGE_FROM_SYSTEM 0x1000
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x200
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x100
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
#define LANG_NEUTRAL 0
#define SUBLANG_DEFAULT 1
BOOL IsDebuggerPresent();
void OutputDebugStringW(LPCWSTR);
void OutputDebugStringA(LPCSTR);
HMODULE GetModuleHandleW(LPCWSTR);
DWORD GetModuleFileNameW(HMODULE, LPWSTR, DWORD);
DWORD ExpandEnvironmentStringsW(LPCWSTR, LPWSTR, DWORD);
DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR file);
DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file);
UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT def, LPCWSTR file);
BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file);
BOOL WritePrivateProfileSectionW(LPCWSTR section, LPCWSTR data, LPCWSTR file);
LSTATUS RegOpenKeyExW(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
LSTATUS RegQueryValueExW(HKEY, LPCWSTR, LPDWORD, LPDWORD, BYTE*, LPDWORD);
LSTATUS RegCloseKey(HKEY);


// ---- 补充（uibase 用到） ---------------------------------------------------------
using NTSTATUS = LONG;
BOOL SetFileAttributesW(LPCWSTR, DWORD);
DWORD GetLongPathNameW(LPCWSTR, LPWSTR, DWORD);
DWORD GetShortPathNameW(LPCWSTR, LPWSTR, DWORD);
#define MB_OK 0x0
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT type);
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
HANDLE GetStdHandle(DWORD);
BOOL GetConsoleMode(HANDLE, LPDWORD);
// UNICODE 构建下的 TCHAR 版本别名
#define GetFileAttributes GetFileAttributesW
#define SetFileAttributes SetFileAttributesW
#define WritePrivateProfileString WritePrivateProfileStringW
#define GetPrivateProfileString GetPrivateProfileStringW
#define GetPrivateProfileInt GetPrivateProfileIntW
#define CreateFile CreateFileW
#define DeleteFile DeleteFileW
#define FormatMessage FormatMessageW
#define GetModuleFileName GetModuleFileNameW
#define GetModuleHandle GetModuleHandleW
#define MessageBox MessageBoxW

#define ERROR_NOT_SAME_DEVICE 17
#define ERROR_WRITE_PROTECT 19
#define ERROR_BAD_PATHNAME 161
#define ERROR_BUFFER_OVERFLOW 111
#define FOREGROUND_BLUE 0x1
#define FOREGROUND_GREEN 0x2
#define FOREGROUND_RED 0x4
#define FOREGROUND_INTENSITY 0x8

// ---- COM（仅让 taskprogressmanager 通过编译；CoCreateInstance 恒失败） -------------
using CLSID = GUID;
using IID = GUID;
using REFCLSID = const GUID&;
using REFIID = const GUID&;
#define CLSCTX_INPROC_SERVER 0x1
inline constexpr GUID CLSID_TaskbarList = {0x56FDF344, 0xFD6D, 0x11D0, {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
inline constexpr GUID IID_Placeholder = {};
#define IID_PPV_ARGS(pp) IID_Placeholder, reinterpret_cast<void**>(pp)
HRESULT CoCreateInstance(REFCLSID, void* outer, DWORD ctx, REFIID, void** out);
HRESULT CoInitialize(void*);
void CoUninitialize();

#define ERROR_DISK_FULL 112
#define ERROR_GEN_FAILURE 31
// SHFileOperationW：仅支持 FO_DELETE（实现中 FOF_ALLOWUNDO 对应移入回收站 ~/.local/share/Trash）
#define FO_MOVE 0x1
#define FO_COPY 0x2
#define FO_DELETE 0x3
#define FO_RENAME 0x4
#define FOF_MULTIDESTFILES 0x1
#define FOF_SILENT 0x4
#define FOF_NOCONFIRMATION 0x10
#define FOF_ALLOWUNDO 0x40
#define FOF_NOERRORUI 0x400
using FILEOP_FLAGS = WORD;
struct SHFILEOPSTRUCTW {
    HWND hwnd;
    UINT wFunc;
    LPCWSTR pFrom;  // 以双 NUL 结尾的路径列表
    LPCWSTR pTo;
    FILEOP_FLAGS fFlags;
    BOOL fAnyOperationsAborted;
    LPVOID hNameMappings;
    LPCWSTR lpszProgressTitle;
};
int SHFileOperationW(SHFILEOPSTRUCTW*);
#define SHFileOperation SHFileOperationW
#define SHFILEOPSTRUCT SHFILEOPSTRUCTW

#define FOF_NOCOPYSECURITYATTRIBS 0x800
#define FOF_NOCONFIRMMKDIR 0x200
#define FOF_NO_UI (FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR)
#define ERROR_BAD_FORMAT 11
#define SE_ERR_FNF 2
#define SE_ERR_PNF 3
#define SE_ERR_ACCESSDENIED 5
#define SE_ERR_OOM 8
#define SE_ERR_SHARE 26
#define SE_ERR_ASSOCINCOMPLETE 27
#define SE_ERR_DDETIMEOUT 28
#define SE_ERR_DDEFAIL 29
#define SE_ERR_DDEBUSY 30
#define SE_ERR_NOASSOC 31
#define SW_SHOWNORMAL 1
#define SW_HIDE 0

// ---- 进程/shell（uibase utility.cpp 引用；Linux 上为受限实现，见 shim/src/process.cpp） ----
#define SE_ERR_DLLNOTFOUND 32
#define SEE_MASK_NOCLOSEPROCESS 0x40
#define SEE_MASK_FLAG_NO_UI 0x400
struct SHELLEXECUTEINFOW {
    DWORD cbSize; ULONG fMask; HWND hwnd; LPCWSTR lpVerb; LPCWSTR lpFile; LPCWSTR lpParameters;
    LPCWSTR lpDirectory; int nShow; HMODULE hInstApp; LPVOID lpIDList; LPCWSTR lpClass;
    HKEY hkeyClass; DWORD dwHotKey; HANDLE hIcon; HANDLE hProcess;
};
using SHELLEXECUTEINFO = SHELLEXECUTEINFOW;
BOOL ShellExecuteExW(SHELLEXECUTEINFOW*);
HMODULE ShellExecuteW(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, int);
#define ShellExecuteEx ShellExecuteExW
#define FORMAT_MESSAGE_ARGUMENT_ARRAY 0x2000
#define FORMAT_MESSAGE_FROM_STRING 0x400
#define FORMAT_MESSAGE_FROM_HMODULE 0x800
void* LocalFree(void*);
struct STARTUPINFOW {
    DWORD cb; LPWSTR lpReserved; LPWSTR lpDesktop; LPWSTR lpTitle; DWORD dwX, dwY, dwXSize, dwYSize,
        dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2;
    BYTE* lpReserved2; HANDLE hStdInput, hStdOutput, hStdError;
};
using STARTUPINFO = STARTUPINFOW;
struct PROCESS_INFORMATION { HANDLE hProcess; HANDLE hThread; DWORD dwProcessId; DWORD dwThreadId; };
BOOL CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL inherit,
                    DWORD flags, LPVOID env, LPCWSTR dir, STARTUPINFOW*, PROCESS_INFORMATION*);
#define CreateProcess CreateProcessW
#define MoveFileEx MoveFileExW
int SHCreateDirectory(HWND, LPCWSTR);
#define SHCreateDirectoryEx SHCreateDirectory
#define LOCALE_USER_DEFAULT 0x0400
#define LOCALE_USE_CP_ACP 0x40000000
#define DATE_SHORTDATE 0x1
#define DATE_LONGDATE 0x2
#define TIME_NOSECONDS 0x2
int GetDateFormatEx(LPCWSTR locale, DWORD flags, const SYSTEMTIME*, LPCWSTR fmt, LPWSTR out, int cch, LPCWSTR cal);
int GetTimeFormatEx(LPCWSTR locale, DWORD flags, const SYSTEMTIME*, LPCWSTR fmt, LPWSTR out, int cch);
int GetDateFormatW(DWORD locale, DWORD flags, const SYSTEMTIME*, LPCWSTR fmt, LPWSTR out, int cch);
int GetTimeFormatW(DWORD locale, DWORD flags, const SYSTEMTIME*, LPCWSTR fmt, LPWSTR out, int cch);
BOOL FileTimeToSystemTime(const FILETIME*, SYSTEMTIME*);
BOOL FileTimeToLocalFileTime(const FILETIME*, FILETIME*);
BOOL SystemTimeToFileTime(const SYSTEMTIME*, FILETIME*);

#include "winver.h"

#define TEXT(x) L##x
#define _T(x) L##x
int GetDateFormatA(DWORD locale, DWORD flags, const SYSTEMTIME*, LPCSTR fmt, LPSTR out, int cch);
int GetTimeFormatA(DWORD locale, DWORD flags, const SYSTEMTIME*, LPCSTR fmt, LPSTR out, int cch);

#include <csignal>
#include <cstdio>
inline void DebugBreak() { std::raise(SIGTRAP); }
template <size_t N, class... A>
int sprintf_s(char (&buf)[N], const char* fmt, A... a) { return std::snprintf(buf, N, fmt, a...); }

// ---- 目录枚举 / 文件映射 / PE 头（gamegamebryo::getArch 用） -------------------------
struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14];
};
using WIN32_FIND_DATA = WIN32_FIND_DATAW;
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW*);
BOOL FindNextFileW(HANDLE, WIN32_FIND_DATAW*);
BOOL FindClose(HANDLE);
#define FindFirstFile FindFirstFileW
#define FindNextFile FindNextFileW
#define SEC_IMAGE 0x1000000
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name);
LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHi, DWORD offLo, SIZE_T bytes);
BOOL UnmapViewOfFile(LPCVOID);
#define IMAGE_FILE_MACHINE_I386 0x014c
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_MACHINE_ARM64 0xAA64
struct IMAGE_FILE_HEADER {
    WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
    WORD SizeOfOptionalHeader, Characteristics;
};
struct IMAGE_NT_HEADERS {  // 仅 Signature + FileHeader（足够读取 Machine）
    DWORD Signature; IMAGE_FILE_HEADER FileHeader;
};
using PIMAGE_NT_HEADERS = IMAGE_NT_HEADERS*;
PIMAGE_NT_HEADERS ImageNtHeader(PVOID base);
// ---- 注册表补充 ----------------------------------------------------------------
#define ERROR_UNSUPPORTED_TYPE 1630
#define RRF_RT_REG_SZ 0x2
#define RRF_RT_REG_EXPAND_SZ 0x4
#define RRF_RT_REG_DWORD 0x10
#define RRF_RT_ANY 0xffff
#define RRF_NOEXPAND 0x10000000
LSTATUS RegGetValueW(HKEY, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD size);

using PWSTR = wchar_t*;
using PCWSTR = const wchar_t*;
#define KF_FLAG_DEFAULT_PATH 0x400
#define KF_FLAG_CREATE 0x8000
#define VerQueryValue VerQueryValueW
#define GetFileVersionInfo GetFileVersionInfoW
#define GetFileVersionInfoSize GetFileVersionInfoSizeW
