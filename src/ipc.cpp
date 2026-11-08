#include "ipc.h"

#include <sddl.h>

namespace ipc {
using app::Handle;

bool Call(uint32_t cmd, uint32_t arg, State& out, DWORD timeoutMs) {
    Request req;
    req.cmd = cmd;
    req.arg = arg;
    State st;
    DWORD read = 0;
    if (!CallNamedPipeW(app::kPipeName, &req, sizeof(req), &st, sizeof(st), &read, timeoutMs)) return false;
    if (read != sizeof(st) || st.magic != kMagic || st.version != kVersion) return false;
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
