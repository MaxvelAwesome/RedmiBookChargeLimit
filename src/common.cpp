#include "common.h"

#include <aclapi.h>
#include <sddl.h>
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

std::wstring SessionObjectName(DWORD session, const wchar_t* name) {
    return L"Session\\" + std::to_wstring(session) + L"\\" + name;
}

namespace {
// Владелец Administrators; SYSTEM и Administrators — полный доступ, Users — чтение (наследуется).
constexpr wchar_t kDataDirSddl[] = L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)";

void EnablePrivilege(const wchar_t* name) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) return;
    Handle tok(raw);
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(tok.get(), FALSE, &tp, sizeof(tp), nullptr, nullptr);
}
}  // namespace

bool SecureDataDir() {
    const std::wstring dir = DataDir();
    if (dir.empty()) return false;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kDataDirSddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    // Каталог мог создать обычный пользователь (и закрыть нам доступ) — берём права резервного
    // восстановления, чтобы сменить владельца и DACL.
    EnablePrivilege(SE_RESTORE_NAME);
    EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);

    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        if (CreateDirectoryW(dir.c_str(), &sa)) { ok = true; break; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;

        // Уже есть. Открываем сам объект (не цель ссылки) и проверяем через дескриптор — без гонки
        // между проверкой и сменой прав.
        Handle h(CreateFileW(dir.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER | DELETE | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!h) break;
        FILE_BASIC_INFO bi{};
        if (!GetFileInformationByHandleEx(h.get(), FileBasicInfo, &bi, sizeof(bi))) break;
        if ((bi.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(bi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            // Подменённый путь: удаляем саму ссылку/файл (цель не трогается) и создаём каталог заново.
            FILE_DISPOSITION_INFO di{TRUE};
            if (!SetFileInformationByHandle(h.get(), FileDispositionInfo, &di, sizeof(di))) break;
            continue;  // h закроется -> объект удалён
        }
        PSID owner = nullptr;
        PACL dacl = nullptr;
        BOOL def = FALSE, present = FALSE;
        GetSecurityDescriptorOwner(sd, &owner, &def);
        GetSecurityDescriptorDacl(sd, &present, &dacl, &def);
        ok = SetSecurityInfo(h.get(), SE_FILE_OBJECT,
                             OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                             owner, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    }
    LocalFree(sd);
    return ok;
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
    // Каталог здесь не создаём: его с правильными правами создаёт SecureDataDir() (служба/установщик).
    std::wstring dir = DataDir();
    if (dir.empty()) return;  // папка не определилась
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
    // Пишем только в обычный файл: не следуем по ссылкам и не пишем в жёсткую ссылку на чужой файл.
    for (int attempt = 0; attempt < 2; ++attempt) {
        Handle f(CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!f) break;
        BY_HANDLE_FILE_INFORMATION fi{};
        if (!GetFileInformationByHandle(f.get(), &fi)) break;
        if ((fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || fi.nNumberOfLinks != 1) {
            f.reset();
            DeleteFileW(path.c_str());  // удаляется сама ссылка, не цель
            continue;
        }
        DWORD written = 0;
        WriteFile(f.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        break;
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
