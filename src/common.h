// Общие константы, RAII-обёртки, лог и реестр.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>

namespace app {

inline constexpr wchar_t kAppName[]        = L"RedmiBook Charge Limit";
inline constexpr wchar_t kAppVersion[]     = L"1.0.0";
inline constexpr wchar_t kPublisher[]      = L"RedmiBookChargeLimit";
inline constexpr wchar_t kServiceName[]    = L"RedmiChargeLimit";
inline constexpr wchar_t kServiceDisplay[] = L"RedmiBook Charge Limit";
inline constexpr wchar_t kExeName[]        = L"RedmiChargeLimit.exe";
inline constexpr wchar_t kRegKey[]         = L"SOFTWARE\\RedmiChargeLimit";
inline constexpr wchar_t kUninstallKey[]   = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RedmiChargeLimit";
inline constexpr wchar_t kPipeName[]       = L"\\\\.\\pipe\\RedmiChargeLimit";
inline constexpr wchar_t kUiMutex[]        = L"Global\\RedmiChargeLimit.UI";
inline constexpr wchar_t kShowEvent[]      = L"Global\\RedmiChargeLimit.Show";
inline constexpr wchar_t kWindowClass[]    = L"RedmiChargeLimit.Window";
inline constexpr wchar_t kShortcutName[]   = L"RedmiBook Charge Limit.lnk";

// Сообщение главному окну от второго экземпляра / установщика.
inline constexpr UINT WM_APP_SHOW  = WM_APP + 1;
inline constexpr UINT WM_APP_QUIT  = WM_APP + 2;

// Уникальный дескриптор Win32.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.release()) {}
    Handle& operator=(Handle&& o) noexcept { if (this != &o) reset(o.release()); return *this; }
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }
    HANDLE release() { HANDLE t = h_; h_ = nullptr; return t; }
    void reset(HANDLE h = nullptr) {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = h;
    }
private:
    HANDLE h_ = nullptr;
};

class RegKey {
public:
    RegKey() = default;
    ~RegKey() { if (k_) RegCloseKey(k_); }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    HKEY* put() { if (k_) { RegCloseKey(k_); k_ = nullptr; } return &k_; }
    HKEY get() const { return k_; }
    explicit operator bool() const { return k_ != nullptr; }
private:
    HKEY k_ = nullptr;
};

// Путь к текущему exe.
std::wstring SelfPath();
std::wstring DirName(const std::wstring& path);
// %ProgramFiles%\RedmiChargeLimit
std::wstring InstallDir();
// %ProgramData%\RedmiChargeLimit
std::wstring DataDir();

// Настройки (HKLM\SOFTWARE\RedmiChargeLimit). Чтение доступно всем, запись — только SYSTEM/admin.
DWORD RegGetDword(const wchar_t* name, DWORD def);
bool  RegSetDword(const wchar_t* name, DWORD value);
ULONGLONG RegGetQword(const wchar_t* name, ULONGLONG def);
bool  RegSetQword(const wchar_t* name, ULONGLONG value);

// Лог службы (кольцевой, ≤ 2×64 КБ). Потокобезопасный.
void LogInit(const wchar_t* tag);
void Log(const wchar_t* fmt, ...);
void LogBytes(const wchar_t* prefix, const uint8_t* in, size_t inLen, const uint8_t* out, size_t outLen);

ULONGLONG NowFileTime();

}  // namespace app
