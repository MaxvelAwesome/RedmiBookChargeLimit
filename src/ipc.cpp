#include "ipc.h"

#include <sddl.h>

namespace ipc {
using app::Handle;

namespace {

// PID процесса нашей службы (0 — не запущена). Статус службы могут читать обычные пользователи.
DWORD ServicePid() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    DWORD pid = 0;
    if (SC_HANDLE svc = OpenServiceW(scm, app::kServiceName, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD need = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&ssp), sizeof(ssp), &need) &&
            ssp.dwCurrentState == SERVICE_RUNNING)
            pid = ssp.dwProcessId;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return pid;
}

DWORD Remaining(ULONGLONG deadline) {
    ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
}

// Overlapped-операция с ограничением по времени.
bool CompleteIo(HANDLE h, OVERLAPPED& ov, BOOL started, ULONGLONG deadline, DWORD& transferred) {
    if (!started && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(ov.hEvent, Remaining(deadline)) != WAIT_OBJECT_0) {
        CancelIoEx(h, &ov);
        GetOverlappedResult(h, &ov, &transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(h, &ov, &transferred, FALSE) != FALSE;
}

}  // namespace

bool Call(uint32_t cmd, uint32_t arg, State& out, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    // SECURITY_IDENTIFICATION: даже если канал подменён, его владелец не сможет действовать от имени клиента.
    Handle pipe;
    for (;;) {
        pipe.reset(CreateFileW(app::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (pipe) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        DWORD left = Remaining(deadline);
        if (left == 0 || !WaitNamedPipeW(app::kPipeName, left)) return false;
    }

    // Отвечать должна именно наша служба, а не процесс, занявший имя канала, пока она остановлена.
    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(), &serverPid) || serverPid == 0 || serverPid != ServicePid()) return false;

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) return false;

    Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ev) return false;
    Request req;
    req.cmd = cmd;
    req.arg = arg;
    OVERLAPPED ov{};
    ov.hEvent = ev.get();
    DWORD n = 0;
    if (!CompleteIo(pipe.get(), ov, WriteFile(pipe.get(), &req, sizeof(req), &n, &ov), deadline, n) || n != sizeof(req))
        return false;

    State st;
    ResetEvent(ev.get());
    ov = {};
    ov.hEvent = ev.get();
    n = 0;
    if (!CompleteIo(pipe.get(), ov, ReadFile(pipe.get(), &st, sizeof(st), &n, &ov), deadline, n)) return false;
    if (n != sizeof(st) || st.magic != kMagic || st.version != kVersion) return false;
    out = st;
    return true;
}

namespace {

// Ждёт завершения overlapped-операции или сигнала stop.
bool WaitIo(HANDLE pipe, OVERLAPPED& ov, HANDLE stop, DWORD timeoutMs, DWORD& transferred) {
    HANDLE hs[2] = {stop, ov.hEvent};
    DWORD r = WaitForMultipleObjects(2, hs, FALSE, timeoutMs);
    if (r != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &transferred, FALSE) != FALSE;
}

}  // namespace

void Serve(HANDLE stop, const std::function<State(const Request&)>& handler) {
    // SYSTEM и администраторы — полный доступ; интерактивные пользователи — чтение/запись.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)",
                                                              SDDL_REVISION_1, &sd, nullptr)) {
        app::Log(L"IPC: SDDL ошибка %lu", GetLastError());
        return;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));

    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
        Handle pipe(CreateNamedPipeW(app::kPipeName, flags,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     1, sizeof(State), sizeof(Request), 0, &sa));
        if (!pipe) {
            app::Log(L"IPC: CreateNamedPipe ошибка %lu", GetLastError());
            if (WaitForSingleObject(stop, 5000) != WAIT_TIMEOUT) break;
            continue;
        }

        OVERLAPPED ov{};
        ov.hEvent = ev.get();
        ResetEvent(ev.get());
        DWORD n = 0;
        BOOL ok = ConnectNamedPipe(pipe.get(), &ov);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (!WaitIo(pipe.get(), ov, stop, INFINITE, n)) continue;
        } else if (err != ERROR_SUCCESS && err != ERROR_PIPE_CONNECTED) {
            continue;
        }

        Request req{};
        ResetEvent(ev.get());
        ok = ReadFile(pipe.get(), &req, sizeof(req), &n, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) { DisconnectNamedPipe(pipe.get()); continue; }
        if (!WaitIo(pipe.get(), ov, stop, 3000, n) || n != sizeof(req) ||
            req.magic != kMagic || req.version != kVersion) {
            DisconnectNamedPipe(pipe.get());
            continue;
        }

        State st = handler(req);
        ResetEvent(ev.get());
        ok = WriteFile(pipe.get(), &st, sizeof(st), &n, &ov);
        if (ok || GetLastError() == ERROR_IO_PENDING) WaitIo(pipe.get(), ov, stop, 3000, n);
        FlushFileBuffers(pipe.get());
        DisconnectNamedPipe(pipe.get());
    }
    LocalFree(sd);
}

}  // namespace ipc
