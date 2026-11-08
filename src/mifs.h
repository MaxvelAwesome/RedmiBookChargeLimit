// Доступ к штатному WMI-интерфейсу прошивки Xiaomi (root\wmi MICommonInterface.MiInterface).
// Это тот же канал, которым пользуется Xiaomi PC Manager: никаких драйверов и прямого доступа к EC.
// Требует прав администратора / SYSTEM. Объект Client не потокобезопасен: используйте из одного потока,
// в котором инициализирован COM (MTA).
#pragma once

#include "common.h"

#include <wbemidl.h>
#include <wrl/client.h>
#include <functional>

namespace mifs {

using Microsoft::WRL::ComPtr;

enum class Status { Ok, Unsupported, Error };

// Коды лимита заряда (cmd 0x10, sub 0x02). Пишем ТОЛЬКО эти два значения.
inline constexpr uint8_t kCodeOff = 0x00;  // лимит выключен, зарядка до 100%
inline constexpr uint8_t kCode80  = 0x01;  // лимит 80%

inline constexpr uint8_t kCmdCharge     = 0x10;
inline constexpr uint8_t kSubLimit      = 0x02;
inline constexpr uint8_t kSubHealth     = 0x01;

// Кнопка Xiaomi: HID_EVENT20, EventDetail = {01, code, value}.
inline constexpr uint8_t kEventHotkey   = 0x01;
inline constexpr uint8_t kKeyMiDown     = 0x25;

// CoInitializeSecurity для процесса (один раз, сразу после CoInitializeEx).
bool InitProcessComSecurity();

class Client {
public:
    // Подключение к root\wmi и поиск экземпляра MICommonInterface.
    bool Connect();
    bool Connected() const { return svc_ != nullptr; }
    void Disconnect();

    // GET cmd 0x10 / sub. Возвращает OUT[6].
    Status Get(uint8_t sub, uint8_t& value);
    // Безопасная установка лимита: только 0x00 или 0x01, с проверкой чтением.
    Status SetLimitCode(uint8_t code);

private:
    Status RawCall(uint8_t op, uint8_t sub, uint8_t arg, uint8_t out[32]);

    ComPtr<IWbemServices> svc_;
    ComPtr<IWbemClassObject> inSig_;
    BSTR path_ = nullptr;
public:
    Client() = default;
    ~Client() { Disconnect(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
};

// Блокирующий цикл чтения WMI-событий класса eventClass до сигнала stop.
// cb вызывается в этом же потоке. Возвращает false при ошибке подключения.
bool ListenEvents(const wchar_t* eventClass, HANDLE stop,
                  const std::function<void(const uint8_t* data, size_t len)>& cb);

}  // namespace mifs
