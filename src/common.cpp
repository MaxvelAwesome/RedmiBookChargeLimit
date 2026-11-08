#include "common.h"

#include <shlobj.h>
#include <cstdarg>
#include <cwchar>

namespace app {

std::wstring SelfPath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); return buf; }
        buf.resize(buf.size() * 2);
    }
}

std::wstring DirName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring InstallDir() {
    std::wstring b = KnownFolder(FOLDERID_ProgramFiles);
    return b.empty() ? b : b + L"\\RedmiChargeLimit";
}
std::wstring DataDir() {
    std::wstring b = KnownFolder(FOLDERID_ProgramData);
    return b.empty() ? b : b + L"\\RedmiChargeLimit";
}

DWORD RegGetDword(const wchar_t* name, DWORD def) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v;
    return def;
}

bool RegSetDword(const wchar_t* name, DWORD value) {
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, kRegKey, name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

ULONGLONG RegGetQword(const wchar_t* name, ULONGLONG def) {
    ULONGLONG v = 0; DWORD sz = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kRegKey, name, RRF_RT_REG_QWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v;
    return def;
}

bool RegSetQword(const wchar_t* name, ULONGLONG value) {
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, kRegKey, name, REG_QWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

ULONGLONG NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// ---------------- лог ----------------
namespace {
SRWLOCK g_logLock = SRWLOCK_INIT;
wchar_t g_logTag[16] = L"app";
constexpr LONGLONG kMaxLog = 64 * 1024;

void WriteLine(const wchar_t* text) {
    std::wstring dir = DataDir();
    if (dir.empty()) return;  // папка не определилась
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\log.txt";

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t head[64];
    swprintf_s(head, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, g_logTag);
    std::wstring line = head;
    line += text;
    line += L"\r\n";

    int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::string utf8(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), need, nullptr, nullptr);

    AcquireSRWLockExclusive(&g_logLock);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        LONGLONG size = (static_cast<LONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (size > kMaxLog) {
            std::wstring old = dir + L"\\log.old.txt";
            MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
    Handle f(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (f) {
        DWORD written = 0;
        WriteFile(f.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}
}  // namespace

void LogInit(const wchar_t* tag) { wcsncpy_s(g_logTag, tag, _TRUNCATE); }

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    WriteLine(buf);
#ifdef _DEBUG
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
#endif
}

void LogBytes(const wchar_t* prefix, const uint8_t* in, size_t inLen, const uint8_t* out, size_t outLen) {
    std::wstring s = prefix;
    wchar_t hx[4];
    s += L" IN";
    for (size_t i = 0; i < inLen && i < 8; ++i) { swprintf_s(hx, L" %02X", in[i]); s += hx; }
    s += L" | OUT";
    for (size_t i = 0; i < outLen && i < 8; ++i) { swprintf_s(hx, L" %02X", out[i]); s += hx; }
    WriteLine(s.c_str());
}

}  // namespace app
