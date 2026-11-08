#include "installer.h"

#include "mifs.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <cwchar>
#include <thread>
#include <vector>

namespace installer {
using app::Handle;
using app::Log;
using Microsoft::WRL::ComPtr;

namespace {

class ScHandle {
public:
    explicit ScHandle(SC_HANDLE h = nullptr) : h_(h) {}
    ~ScHandle() { if (h_) CloseServiceHandle(h_); }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    SC_HANDLE get() const { return h_; }
    void reset(SC_HANDLE h) { if (h_) CloseServiceHandle(h_); h_ = h; }
    explicit operator bool() const { return h_ != nullptr; }
private:
    SC_HANDLE h_;
};

bool IsElevated() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    Handle tok(t);
    TOKEN_ELEVATION e{};
    DWORD sz = 0;
    return GetTokenInformation(tok.get(), TokenElevation, &e, sizeof(e), &sz) && e.TokenIsElevated;
}

std::wstring InstalledExe() { return app::InstallDir() + L"\\" + app::kExeName; }

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void Message(bool quiet, const wchar_t* text, UINT icon) {
    if (!quiet) MessageBoxW(nullptr, text, app::kAppName, MB_OK | icon | MB_SETFOREGROUND);
}

// Запуск консольной утилиты без окна с ожиданием.
DWORD RunHidden(const std::wstring& cmdline, DWORD timeoutMs) {
    std::wstring cmd = cmdline;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return static_cast<DWORD>(-1);
    Handle p(pi.hProcess), t(pi.hThread);
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(p.get(), timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(p.get(), &code);
    return code;
}

std::wstring SystemExe(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir, n) + L"\\" + name;
}

// Завершает процессы, запущенные из указанного exe (кроме текущего).
void KillByPath(const std::wstring& exePath) {
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap.get(), &pe); ok; ok = Process32NextW(snap.get(), &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        if (CompareStringOrdinal(pe.szExeFile, -1, app::kExeName, -1, TRUE) != CSTR_EQUAL) continue;
        Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID));
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD len = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(h.get(), 0, path, &len) && SamePath(path, exePath)) {
            TerminateProcess(h.get(), 0);
            WaitForSingleObject(h.get(), 5000);
        }
    }
}

// Корректно закрывает окна (трей) установленной копии и ждёт выхода процессов.
// Копию, запущенную из другой папки (например, из «Загрузок» для установки), не трогаем.
void CloseRunningUi() {
    std::vector<DWORD> pids;
    const std::wstring installed = InstalledExe();
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, app::kWindowClass, nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (!pid || pid == GetCurrentProcessId()) continue;
        Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        wchar_t path[MAX_PATH * 2];
        DWORD len = ARRAYSIZE(path);
        if (h && QueryFullProcessImageNameW(h.get(), 0, path, &len) && SamePath(path, installed)) {
            PostMessageW(w, app::WM_APP_QUIT, 0, 0);
            pids.push_back(pid);
        }
    }
    for (DWORD pid : pids) {
        Handle h(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid));
        if (h && WaitForSingleObject(h.get(), 5000) == WAIT_TIMEOUT) TerminateProcess(h.get(), 0);
    }
    // На случай процессов без окна.
    KillByPath(InstalledExe());
}

bool StopService(SC_HANDLE svc) {
    SERVICE_STATUS st{};
    if (!QueryServiceStatus(svc, &st)) return false;
    if (st.dwCurrentState == SERVICE_STOPPED) return true;
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 150; ++i) {
        if (!QueryServiceStatus(svc, &st)) return false;
        if (st.dwCurrentState == SERVICE_STOPPED) return true;
        Sleep(100);
    }
    return false;
}

void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                RemoveDirectoryW(p.c_str());  // не идём по ссылкам
                DeleteFileW(p.c_str());
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTree(p);
            } else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(p.c_str())) MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (!RemoveDirectoryW(dir.c_str())) MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

// Удаление прошлой версии (.NET, 163 МБ): задача RedmiChargeLimitTray и её файлы в ProgramData.
void RemoveLegacy() {
    RunHidden(L"\"" + SystemExe(L"schtasks.exe") + L"\" /End /TN RedmiChargeLimitTray", 10000);
    RunHidden(L"\"" + SystemExe(L"schtasks.exe") + L"\" /Delete /TN RedmiChargeLimitTray /F", 10000);
    std::wstring data = app::DataDir();
    if (data.empty()) return;
    std::wstring legacyExe = data + L"\\app\\" + app::kExeName;
    KillByPath(legacyExe);
    if (GetFileAttributesW((data + L"\\app").c_str()) != INVALID_FILE_ATTRIBUTES) DeleteTree(data + L"\\app");
    DeleteFileW((data + L"\\config.json").c_str());
    DeleteFileW((data + L"\\charge-limit.log").c_str());
}

std::wstring ShortcutPath() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &p)) && p)
        r = std::wstring(p) + L"\\" + app::kShortcutName;
    CoTaskMemFree(p);
    return r;
}

bool CreateShortcut(const std::wstring& target) {
    std::wstring lnk = ShortcutPath();
    if (lnk.empty()) return false;
    bool ok = false;
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
        {
            ComPtr<IShellLinkW> link;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
                link->SetPath(target.c_str());
                link->SetWorkingDirectory(app::DirName(target).c_str());
                link->SetDescription(L"Лимит заряда батареи RedmiBook");
                link->SetIconLocation(target.c_str(), 0);
                ComPtr<IPersistFile> pf;
                if (SUCCEEDED(link.As(&pf))) ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
            }
        }
        CoUninitialize();
    }).join();
    return ok;
}

void WriteUninstallEntry(const std::wstring& exe) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, app::kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    auto setStr = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    };
    auto setDw = [&](const wchar_t* name, DWORD v) {
        RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    };
    setStr(L"DisplayName", app::kAppName);
    setStr(L"DisplayVersion", app::kAppVersion);
    setStr(L"Publisher", app::kPublisher);
    setStr(L"DisplayIcon", exe + L",0");
    setStr(L"InstallLocation", app::DirName(exe));
    setStr(L"UninstallString", L"\"" + exe + L"\" --uninstall");
    setStr(L"QuietUninstallString", L"\"" + exe + L"\" --uninstall --quiet");
    setDw(L"NoModify", 1);
    setDw(L"NoRepair", 1);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fad)) setDw(L"EstimatedSize", (fad.nFileSizeLow + 1023) / 1024);
    RegCloseKey(k);
}

// Выключает лимит напрямую через прошивку (служба уже остановлена).
bool DisableLimitInFirmware() {
    bool ok = false;
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        mifs::InitProcessComSecurity();
        {
            mifs::Client cli;
            if (cli.Connect()) ok = cli.SetLimitCode(mifs::kCodeOff) == mifs::Status::Ok;
        }
        CoUninitialize();
    }).join();
    return ok;
}

}  // namespace

bool IsInstalled() {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) return false;
    ScHandle svc(OpenServiceW(scm.get(), app::kServiceName, SERVICE_QUERY_STATUS));
    return static_cast<bool>(svc);
}

bool RunningFromInstallDir() { return SamePath(app::SelfPath(), InstalledExe()); }

HANDLE StartElevated(const wchar_t* args, HWND owner) {
    std::wstring exe = app::SelfPath();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return nullptr;
    return sei.hProcess;
}

int RunElevated(const wchar_t* args) {
    Handle p(StartElevated(args, nullptr));
    if (!p) return -1;
    WaitForSingleObject(p.get(), INFINITE);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(p.get(), &code);
    return static_cast<int>(code);
}

int Install(bool quiet) {
    if (!IsElevated()) return RunElevated(quiet ? L"--install --quiet" : L"--install");
    app::LogInit(L"setup");
    Log(L"Установка %s", app::kAppVersion);

    std::wstring self = app::SelfPath();
    std::wstring target = InstalledExe();

    RemoveLegacy();

    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    if (!scm) { Message(quiet, L"Нет доступа к диспетчеру служб.", MB_ICONERROR); return 2; }
    {
        ScHandle old(OpenServiceW(scm.get(), app::kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
        if (old) StopService(old.get());
    }
    CloseRunningUi();

    if (!SamePath(self, target)) {
        CreateDirectoryW(app::InstallDir().c_str(), nullptr);
        bool copied = false;
        for (int i = 0; i < 10 && !copied; ++i) {
            copied = CopyFileW(self.c_str(), target.c_str(), FALSE) != FALSE;
            if (!copied) Sleep(500);
        }
        if (!copied) {
            Log(L"Не удалось скопировать exe: %lu", GetLastError());
            Message(quiet, L"Не удалось скопировать файл приложения в Program Files.", MB_ICONERROR);
            return 3;
        }
    }

    std::wstring bin = L"\"" + target + L"\" --service";
    ScHandle svc(OpenServiceW(scm.get(), app::kServiceName, SERVICE_ALL_ACCESS));
    if (svc) {
        ChangeServiceConfigW(svc.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(),
                             nullptr, nullptr, nullptr, nullptr, nullptr, app::kServiceDisplay);
    } else {
        svc.reset(CreateServiceW(scm.get(), app::kServiceName, app::kServiceDisplay, SERVICE_ALL_ACCESS,
                                 SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(),
                                 nullptr, nullptr, nullptr, nullptr, nullptr));
    }
    if (!svc) {
        Log(L"CreateService: %lu", GetLastError());
        Message(quiet, L"Не удалось зарегистрировать службу.", MB_ICONERROR);
        return 4;
    }

    wchar_t desc[] = L"Держит лимит заряда батареи RedmiBook на 80% (после загрузки, сна и смены питания).";
    SERVICE_DESCRIPTIONW sd{desc};
    ChangeServiceConfig2W(svc.get(), SERVICE_CONFIG_DESCRIPTION, &sd);
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 24 * 3600;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    SERVICE_PRESHUTDOWN_INFO pre{10000};
    ChangeServiceConfig2W(svc.get(), SERVICE_CONFIG_PRESHUTDOWN_INFO, &pre);
    SERVICE_SID_INFO sid{SERVICE_SID_TYPE_UNRESTRICTED};
    ChangeServiceConfig2W(svc.get(), SERVICE_CONFIG_SERVICE_SID_INFO, &sid);

    // Настройки по умолчанию: лимит включён.
    if (app::RegGetDword(L"Enabled", 2) == 2) app::RegSetDword(L"Enabled", 1);
    app::RegSetDword(L"FullCharge", 0);
    CreateDirectoryW(app::DataDir().c_str(), nullptr);

    CreateShortcut(target);
    WriteUninstallEntry(target);

    if (!StartServiceW(svc.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        Log(L"StartService: %lu", GetLastError());
        Message(quiet, L"Служба установлена, но не запустилась. Подробности в журнале.", MB_ICONWARNING);
        return 5;
    }
    Log(L"Установка завершена");
    // Служба сама откроет иконку в трее в текущей сессии.
    return 0;
}

int Uninstall(bool quiet) {
    if (!IsElevated()) return RunElevated(quiet ? L"--uninstall --quiet" : L"--uninstall");
    app::LogInit(L"setup");

    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    if (scm) {
        ScHandle svc(OpenServiceW(scm.get(), app::kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
        if (svc) {
            StopService(svc.get());
            DeleteService(svc.get());
        }
    }
    CloseRunningUi();

    // Батарея не должна «застрять» на 80%: возвращаем обычную зарядку до 100%.
    bool fwOk = DisableLimitInFirmware();

    std::wstring lnk = ShortcutPath();
    if (!lnk.empty()) DeleteFileW(lnk.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, app::kUninstallKey);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, app::kUninstallKey);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, app::kRegKey);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, app::kRegKey);

    RemoveLegacy();
    std::wstring data = app::DataDir();
    if (!data.empty()) DeleteTree(data);

    std::wstring self = app::SelfPath();
    std::wstring target = InstalledExe();
    if (!SamePath(self, target)) {
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) DeleteTree(app::InstallDir());
    }

    Message(quiet, fwOk ? L"Приложение удалено. Лимит заряда выключен — батарея снова заряжается до 100%."
                        : L"Приложение удалено. Не удалось подтвердить отключение лимита в прошивке — "
                          L"после перезагрузки или переподключения зарядки ноутбук будет заряжаться как обычно.",
            MB_ICONINFORMATION);

    if (SamePath(self, target)) {
        // Самоудаление после выхода процесса.
        std::wstring cmd = L"\"" + SystemExe(L"cmd.exe") + L"\" /c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + target +
                           L"\" & rmdir /q \"" + app::InstallDir() + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            MoveFileExW(app::InstallDir().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    return 0;
}

}  // namespace installer
