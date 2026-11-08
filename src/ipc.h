// Обмен UI <-> служба через именованный канал. Фиксированные POD-сообщения.
#pragma once

#include "common.h"
#include <functional>

namespace ipc {

inline constexpr uint32_t kMagic = 0x4C435242;  // 'BRCL'
inline constexpr uint32_t kVersion = 1;

enum Cmd : uint32_t {
    kGetState   = 1,
    kSetEnabled = 2,  // arg: 0/1
    kFullCharge = 3,  // arg: 1 начать, 0 отменить
};

struct Request {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t cmd = kGetState;
    uint32_t arg = 0;
};

enum HwStatus : uint8_t {
    kHwUnknown = 0,
    kHwOk = 1,
    kHwUnsupported = 2,
    kHwError = 3,
};

struct State {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint8_t  enabled = 0;      // лимит 80% включён в настройках
    uint8_t  fullCharge = 0;   // идёт разовая зарядка до 100%
    uint8_t  hw = kHwUnknown;  // результат последнего обращения к прошивке
    uint8_t  hwCode = 0xFF;    // последнее прочитанное значение лимита
    uint8_t  health = 0;       // здоровье батареи по данным EC, %, 0 — неизвестно
    uint8_t  reserved[3] = {};
    uint64_t lastApply = 0;    // FILETIME последнего применения
};

// Клиент: false, если служба недоступна.
bool Call(uint32_t cmd, uint32_t arg, State& out, DWORD timeoutMs = 5000);

// Сервер: блокирующий цикл до сигнала stop.
void Serve(HANDLE stop, const std::function<State(const Request&)>& handler);

}  // namespace ipc
