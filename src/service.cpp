// Служба Windows: единственный компонент, который обращается к прошивке.
//  - применяет лимит при старте, после сна/гибернации, смены питания и перед выключением;
//  - следит, чтобы батарея не заряжалась выше лимита (если EC «забыл» настройку — переприменяет);
//  - ведёт разовый режим «зарядить до 100%» и сама возвращает лимит 80%;
//  - слушает кнопку Xiaomi и открывает окно приложения в сессии пользователя.
#include "service.h"

#include "battery.h"
#include "ipc.h"
#include "mifs.h"

#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <cstdio>
#include <thread>

namespace service {
using app::Handle;
using app::Log;

namespace {

constexpr DWORD kDebounceMs        = 1500;
constexpr DWORD kSecondApplyMs     = 15000;            // повтор после пробуждения
constexpr DWORD kWatchdogMs        = 2 * 60 * 1000;    // проверка состояния
constexpr DWORD kFullChargePollMs  = 30 * 1000;
constexpr ULONGLONG kFullChargeMax = 12ULL * 3600 * 10000000;  // 12 ч в FILETIME
constexpr ULONGLONG kForceGap      = 10ULL * 60 * 10000000;    // не чаще раза в 10 мин при «перезаряде»
constexpr int kOverchargePercent   = 82;

SERVICE_STATUS_HANDLE g_ssh = nullptr;
SERVICE_STATUS g_status{};
Handle g_stop;      // остановка службы
Handle g_kick;      // применить с задержкой (debounce)
Handle g_kickNow;   // применить немедленно
Handle g_showEvent; // сигнал UI «показать окно»

SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
ipc::State g_state;          // защищено g_lock
uint64_t g_reqSeq = 0;       // номер запрошенного применения
uint64_t g_doneSeq = 0;      // номер выполненного применения
BYTE g_lastAc = 0xFF;        // последний известный источник питания (только поток обработчика SCM)

void SetStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    static DWORD checkpoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_POWEREVENT | SERVICE_ACCEPT_SESSIONCHANGE
        : 0;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_ssh, &g_status);
}

// Запрос немедленного применения и ожидание результата (из потоков, отличных от рабочего).
void ApplyAndWait(DWORD timeoutMs) {
    AcquireSRWLockExclusive(&g_lock);
    uint64_t seq = ++g_reqSeq;
    SetEvent(g_kickNow.get());
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (g_doneSeq < seq) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        SleepConditionVariableSRW(&g_cv, &g_lock, static_cast<DWORD>(deadline - now), 0);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

ipc::State Snapshot() {
    AcquireSRWLockShared(&g_lock);
    ipc::State s = g_state;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

// ---------- запуск UI в сессии пользователя ----------
bool UiRunning() {
    Handle m(OpenMutexW(SYNCHRONIZE, FALSE, app::kUiMutex));
    return static_cast<bool>(m);
}

bool LaunchUi(DWORD session, const wchar_t* args) {
    if (session == 0xFFFFFFFF || session == 0) return false;
    HANDLE raw = nullptr;
    if (!WTSQueryUserToken(session, &raw)) return false;  // никто не вошёл
    Handle userToken(raw);
    HANDLE primary = nullptr;
    if (!DuplicateTokenEx(userToken.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &primary))
        return false;
    Handle token(primary);

    void* env = nullptr;
    if (!CreateEnvironmentBlock(&env, token.get(), FALSE)) env = nullptr;

    std::wstring exe = app::SelfPath();
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::wstring dir = app::DirName(exe);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    wchar_t desktop[] = L"winsta0\\default";
    si.lpDesktop = desktop;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessAsUserW(token.get(), exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                   CREATE_UNICODE_ENVIRONMENT | NORMAL_PRIORITY_CLASS, env, dir.c_str(), &si, &pi);
    if (env) DestroyEnvironmentBlock(env);
    if (!ok) { Log(L"UI: не удалось запустить в сессии %lu, ошибка %lu", session, GetLastError()); return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log(L"UI: запущен в сессии %lu (%s)", session, args);
    return true;
}

void LaunchTrayAsync(DWORD session) {
    std::thread([session] {
        // Даём Explorer'у подняться после входа.
        for (int i = 0; i < 20 && !UiRunning(); ++i) {
            if (WaitForSingleObject(g_stop.get(), i == 0 ? 3000 : 1000) != WAIT_TIMEOUT) return;
            if (UiRunning()) return;
            if (LaunchUi(session, L"--tray")) return;
        }
    }).detach();
}

void OnMiButton() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 600) return;
    last = now;
    Log(L"Нажата кнопка Xiaomi.");
    if (UiRunning()) {
        SetEvent(g_showEvent.get());
    } else {
        LaunchUi(WTSGetActiveConsoleSessionId(), L"--show");
    }
}

// ---------- рабочий поток: всё общение с прошивкой ----------
void WorkerThread() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { Log(L"COM init failed"); return; }
    {
        mifs::Client cli;
        bool supportedChecked = false;
        bool unsupported = false;
        ULONGLONG lastForce = 0;
        ULONGLONG secondApplyAt = 0;
        bool force = true;  // первое применение при старте

        while (true) {
            // --- подключение (WMI может быть ещё не готов на ранней загрузке) ---
            if (!cli.Connected() && !unsupported) {
                if (!cli.Connect()) {
                    AcquireSRWLockExclusive(&g_lock);
                    g_state.hw = ipc::kHwError;
                    ReleaseSRWLockExclusive(&g_lock);
                    if (WaitForSingleObject(g_stop.get(), 5000) != WAIT_TIMEOUT) break;
                    continue;
                }
                if (!supportedChecked) {
                    uint8_t code = 0;
                    mifs::Status s = cli.Get(mifs::kSubLimit, code);
                    if (s == mifs::Status::Unsupported) {
                        unsupported = true;
                        Log(L"Прошивка не поддерживает лимит заряда (0xE0). Запись отключена.");
                        AcquireSRWLockExclusive(&g_lock);
                        g_state.hw = ipc::kHwUnsupported;
                        ReleaseSRWLockExclusive(&g_lock);
                    }
                    supportedChecked = true;
                }
            }

            // --- состояние ---
            DWORD enabled = app::RegGetDword(L"Enabled", 1);
            DWORD fullCharge = app::RegGetDword(L"FullCharge", 0);
            if (fullCharge && !enabled) { fullCharge = 0; app::RegSetDword(L"FullCharge", 0); }

            battery::Info bi;
            battery::Query(bi);

            if (fullCharge) {
                ULONGLONG start = app::RegGetQword(L"FullChargeStart", 0);
                ULONGLONG now = app::NowFileTime();
                const wchar_t* reason = nullptr;
                if (!bi.acOnline) reason = L"зарядка отключена";
                else if (bi.percent >= 100 || (bi.fullMwh > 0 && bi.capacityMwh >= bi.fullMwh)) reason = L"батарея заряжена до 100%";
                else if (start && now > start && now - start > kFullChargeMax) reason = L"истекло 12 часов";
                if (reason) {
                    Log(L"Разовая зарядка завершена: %s. Возврат лимита 80%%.", reason);
                    fullCharge = 0;
                    app::RegSetDword(L"FullCharge", 0);
                    force = true;
                }
            }

            const uint8_t want = (enabled && !fullCharge) ? mifs::kCode80 : mifs::kCodeOff;

            // Батарея заряжается заметно выше лимита — EC потерял настройку.
            if (!force && want == mifs::kCode80 && bi.acOnline && bi.charging && bi.percent >= kOverchargePercent) {
                ULONGLONG now = app::NowFileTime();
                if (now - lastForce > kForceGap) {
                    Log(L"Батарея заряжается выше лимита (%d%%) — переприменяю.", bi.percent);
                    force = true;
                }
            }

            uint64_t seqAtStart;
            AcquireSRWLockShared(&g_lock);
            seqAtStart = g_reqSeq;
            ReleaseSRWLockShared(&g_lock);

            uint8_t hwCode = 0xFF;
            uint8_t hw = ipc::kHwUnknown;
            if (unsupported) {
                hw = ipc::kHwUnsupported;
            } else if (cli.Connected()) {
                mifs::Status s = mifs::Status::Ok;
                if (!force) {
                    s = cli.Get(mifs::kSubLimit, hwCode);
                    if (s == mifs::Status::Ok && hwCode != want) force = true;
                }
                if (force) {
                    s = cli.SetLimitCode(want);
                    lastForce = app::NowFileTime();
                    if (s == mifs::Status::Ok) {
                        hwCode = want;
                        Log(want == mifs::kCode80 ? L"Лимит 80%% применён." : L"Лимит выключен (зарядка до 100%%).");
                    } else {
                        Log(L"Не удалось применить лимит (status=%d).", static_cast<int>(s));
                        cli.Disconnect();  // переподключимся на следующей итерации
                    }
                }
                hw = s == mifs::Status::Ok ? ipc::kHwOk : s == mifs::Status::Unsupported ? ipc::kHwUnsupported : ipc::kHwError;
            } else {
                hw = ipc::kHwError;
            }

            uint8_t health = 0;
            if (cli.Connected() && !unsupported) {
                uint8_t h = 0;
                if (cli.Get(mifs::kSubHealth, h) == mifs::Status::Ok && h > 0 && h <= 100) health = h;
            }

            AcquireSRWLockExclusive(&g_lock);
            g_state.enabled = enabled ? 1 : 0;
            g_state.fullCharge = fullCharge ? 1 : 0;
            g_state.hw = hw;
            g_state.hwCode = hwCode;
            if (health) g_state.health = health;
            if (force) g_state.lastApply = app::NowFileTime();
            if (g_doneSeq < seqAtStart) g_doneSeq = seqAtStart;
            ReleaseSRWLockExclusive(&g_lock);
            WakeAllConditionVariable(&g_cv);
            force = false;

            // --- ожидание следующего события ---
            DWORD timeout = fullCharge ? kFullChargePollMs : kWatchdogMs;
            ULONGLONG nowTick = GetTickCount64();
            if (secondApplyAt) {
                DWORD left = secondApplyAt > nowTick ? static_cast<DWORD>(secondApplyAt - nowTick) : 0;
                if (left < timeout) timeout = left;
            }

            HANDLE hs[3] = {g_stop.get(), g_kickNow.get(), g_kick.get()};
            DWORD r = WaitForMultipleObjects(3, hs, FALSE, timeout);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 1) {
                force = true;
            } else if (r == WAIT_OBJECT_0 + 2) {
                // Событие питания: ждём, пока всё «устаканится», затем применяем и повторяем через 15 с.
                secondApplyAt = GetTickCount64() + kSecondApplyMs;
                DWORD r2;
                do {
                    r2 = WaitForMultipleObjects(3, hs, FALSE, kDebounceMs);
                } while (r2 == WAIT_OBJECT_0 + 2);  // ещё событие — продлеваем паузу
                if (r2 == WAIT_OBJECT_0) break;
                force = true;
            } else if (r == WAIT_TIMEOUT) {
                if (secondApplyAt && GetTickCount64() >= secondApplyAt) {
                    secondApplyAt = 0;
                    force = true;
                }
            }
        }
    }
    CoUninitialize();
}

ipc::State HandleRequest(const ipc::Request& req) {
    switch (req.cmd) {
    case ipc::kSetEnabled: {
        bool on = req.arg != 0;
        app::RegSetDword(L"Enabled", on ? 1 : 0);
        if (!on) app::RegSetDword(L"FullCharge", 0);
        Log(on ? L"Пользователь включил лимит." : L"Пользователь выключил лимит.");
        ApplyAndWait(6000);
        break;
    }
    case ipc::kFullCharge: {
        if (req.arg) {
            battery::Info bi;
            battery::Query(bi);
            if (!bi.acOnline || !app::RegGetDword(L"Enabled", 1)) break;  // без зарядки или при выключенном лимите не нужно
            app::RegSetQword(L"FullChargeStart", app::NowFileTime());
            app::RegSetDword(L"FullCharge", 1);
            Log(L"Пользователь запустил разовую зарядку до 100%%.");
        } else {
            app::RegSetDword(L"FullCharge", 0);
            Log(L"Пользователь отменил разовую зарядку.");
        }
        ApplyAndWait(6000);
        break;
    }
    default:
        break;
    }
    return Snapshot();
}

DWORD WINAPI CtrlHandler(DWORD ctrl, DWORD type, LPVOID data, LPVOID) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
        SetStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        SetEvent(g_stop.get());
        return NO_ERROR;
    case SERVICE_CONTROL_PRESHUTDOWN:
        // Перед выключением/перезагрузкой ещё раз фиксируем лимит в EC.
        SetStatus(SERVICE_STOP_PENDING, NO_ERROR, 8000);
        ApplyAndWait(5000);
        SetEvent(g_stop.get());
        return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:
        switch (type) {
        case PBT_APMSUSPEND:
            ApplyAndWait(1500);
            break;
        case PBT_APMRESUMEAUTOMATIC:
        case PBT_APMRESUMESUSPEND:
            SetEvent(g_kick.get());
            break;
        case PBT_APMPOWERSTATUSCHANGE: {
            // Событие приходит и при каждом изменении процента — реагируем только на смену источника питания.
            SYSTEM_POWER_STATUS sps{};
            if (GetSystemPowerStatus(&sps) && sps.ACLineStatus != g_lastAc) {
                g_lastAc = sps.ACLineStatus;
                SetEvent(g_kick.get());
            }
            break;
        }
        default:
            break;
        }
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (type == WTS_SESSION_LOGON && data) {
            auto* n = static_cast<WTSSESSION_NOTIFICATION*>(data);
            LaunchTrayAsync(n->dwSessionId);
        }
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    app::LogInit(L"svc");
    g_ssh = RegisterServiceCtrlHandlerExW(app::kServiceName, CtrlHandler, nullptr);
    if (!g_ssh) return;
    SetStatus(SERVICE_START_PENDING, NO_ERROR, 5000);

    g_stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    g_kick.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    g_kickNow.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));

    // Событие «показать окно»: UI (обычный пользователь) может только ждать его.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100000;;;IU)",
                                                             SDDL_REVISION_1, &sd, nullptr)) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        g_showEvent.reset(CreateEventW(&sa, FALSE, FALSE, app::kShowEvent));
        LocalFree(sd);
    }

    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) g_lastAc = sps.ACLineStatus;
    g_state.enabled = app::RegGetDword(L"Enabled", 1) ? 1 : 0;
    g_state.fullCharge = app::RegGetDword(L"FullCharge", 0) ? 1 : 0;
    Log(L"Служба запущена. Лимит: %s.", g_state.enabled ? L"80%" : L"выкл");

    std::thread worker(WorkerThread);
    std::thread pipe([] { ipc::Serve(g_stop.get(), HandleRequest); });
    std::thread buttons([] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        while (WaitForSingleObject(g_stop.get(), 0) == WAIT_TIMEOUT) {
            mifs::ListenEvents(L"HID_EVENT20", g_stop.get(), [](const uint8_t* d, size_t n) {
                if (n >= 2 && d[0] == mifs::kEventHotkey && d[1] == mifs::kKeyMiDown) OnMiButton();
                else if (n >= 3 && d[1] != 0x26) Log(L"HID-событие %02X %02X %02X", d[0], d[1], d[2]);
            });
            if (WaitForSingleObject(g_stop.get(), 10000) != WAIT_TIMEOUT) break;
        }
        CoUninitialize();
    });

    SetStatus(SERVICE_RUNNING);
    LaunchTrayAsync(WTSGetActiveConsoleSessionId());

    WaitForSingleObject(g_stop.get(), INFINITE);
    worker.join();
    pipe.join();
    buttons.join();
    Log(L"Служба остановлена.");
    SetStatus(SERVICE_STOPPED);
}

}  // namespace

int Run() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    mifs::InitProcessComSecurity();
    wchar_t name[] = L"RedmiChargeLimit";
    SERVICE_TABLE_ENTRYW table[] = {{name, ServiceMain}, {nullptr, nullptr}};
    BOOL ok = StartServiceCtrlDispatcherW(table);
    CoUninitialize();
    return ok ? 0 : 1;
}

// ---------- диагностика ----------
int RunDiag() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    SetConsoleOutputCP(CP_UTF8);
    app::LogInit(L"diag");

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    mifs::InitProcessComSecurity();
    {
        mifs::Client cli;
        if (!cli.Connect()) {
            std::printf("MICommonInterface недоступен (нужен запуск от администратора).\n");
        } else {
            uint8_t code = 0, health = 0;
            mifs::Status s = cli.Get(mifs::kSubLimit, code);
            std::printf("Лимит: status=%d code=0x%02X (%s)\n", static_cast<int>(s), code,
                        code == 0 ? "выкл" : code == 1 ? "80%" : "другое");
            s = cli.Get(mifs::kSubHealth, health);
            std::printf("Здоровье EC: status=%d %u%%\n", static_cast<int>(s), health);
        }
    }
    battery::Info bi;
    battery::Query(bi);
    std::printf("Батарея: %d%%, AC=%d, charging=%d, rate=%ld мВт, V=%ld мВ, cap=%ld/%ld/%ld мВт·ч, cycles=%ld\n",
                bi.percent, bi.acOnline, bi.charging, bi.rateMw, bi.voltageMv, bi.capacityMwh, bi.fullMwh,
                bi.designMwh, bi.cycles);

    std::printf("Слушаю HID_EVENT20..23 30 секунд — нажмите кнопку Xiaomi.\n");
    std::fflush(stdout);
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const wchar_t* classes[] = {L"HID_EVENT20", L"HID_EVENT21", L"HID_EVENT22", L"HID_EVENT23"};
    std::thread ts[4];
    for (int i = 0; i < 4; ++i) {
        ts[i] = std::thread([&, i] {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
            mifs::ListenEvents(classes[i], stop.get(), [&, i](const uint8_t* d, size_t n) {
                std::printf("%ls:", classes[i]);
                for (size_t k = 0; k < n && k < 8; ++k) std::printf(" %02X", d[k]);
                std::printf("\n");
                std::fflush(stdout);
                Log(L"%s: %02X %02X %02X %02X", classes[i], d[0], n > 1 ? d[1] : 0, n > 2 ? d[2] : 0, n > 3 ? d[3] : 0);
            });
            CoUninitialize();
        });
    }
    Sleep(30000);
    SetEvent(stop.get());
    for (auto& t : ts) t.join();
    CoUninitialize();
    std::printf("Готово.\n");
    return 0;
}

}  // namespace service
