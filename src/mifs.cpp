#include "mifs.h"

#include <oleauto.h>
#include <cstring>

namespace mifs {

namespace {

// Минимальная RAII-обёртка для BSTR.
class Bstr {
public:
    explicit Bstr(const wchar_t* s) : b_(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(b_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return b_; }
private:
    BSTR b_;
};

class Variant {
public:
    Variant() { VariantInit(&v); }
    ~Variant() { VariantClear(&v); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    VARIANT v;
};

bool ConnectWmi(ComPtr<IWbemServices>& svc) {
    ComPtr<IWbemLocator> loc;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&loc));
    if (FAILED(hr)) { app::Log(L"WMI: WbemLocator hr=0x%08X", hr); return false; }
    Bstr ns(L"ROOT\\WMI");
    hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    if (FAILED(hr)) { app::Log(L"WMI: ConnectServer hr=0x%08X", hr); return false; }
    hr = CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) { app::Log(L"WMI: CoSetProxyBlanket hr=0x%08X", hr); svc.Reset(); return false; }
    return true;
}

}  // namespace

bool InitProcessComSecurity() {
    HRESULT hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    return SUCCEEDED(hr) || hr == RPC_E_TOO_LATE;
}

void Client::Disconnect() {
    inSig_.Reset();
    svc_.Reset();
    if (path_) { SysFreeString(path_); path_ = nullptr; }
}

bool Client::Connect() {
    Disconnect();
    if (!ConnectWmi(svc_)) return false;

    // Экземпляр (обычно ACPI\PNP0C14\MIFS_0). Берём готовый __RELPATH — он уже корректно экранирован.
    ComPtr<IEnumWbemClassObject> en;
    Bstr wql(L"WQL"), q(L"SELECT * FROM MICommonInterface");
    HRESULT hr = svc_->ExecQuery(wql, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    if (FAILED(hr)) { app::Log(L"MIFS: ExecQuery hr=0x%08X", hr); Disconnect(); return false; }
    CoSetProxyBlanket(en.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    ComPtr<IWbemClassObject> inst;
    ULONG n = 0;
    hr = en->Next(5000, 1, &inst, &n);
    if (FAILED(hr) || n == 0) { app::Log(L"MIFS: экземпляр не найден hr=0x%08X", hr); Disconnect(); return false; }
    {
        Variant rel;
        if (FAILED(inst->Get(L"__RELPATH", 0, &rel.v, nullptr, nullptr)) || rel.v.vt != VT_BSTR) {
            Disconnect();
            return false;
        }
        path_ = SysAllocString(rel.v.bstrVal);
    }

    ComPtr<IWbemClassObject> cls;
    Bstr clsName(L"MICommonInterface");
    hr = svc_->GetObject(clsName, 0, nullptr, &cls, nullptr);
    if (FAILED(hr)) { app::Log(L"MIFS: GetObject hr=0x%08X", hr); Disconnect(); return false; }
    hr = cls->GetMethod(L"MiInterface", 0, &inSig_, nullptr);
    if (FAILED(hr) || !inSig_) { app::Log(L"MIFS: GetMethod hr=0x%08X", hr); Disconnect(); return false; }
    return true;
}

Status Client::RawCall(uint8_t op, uint8_t sub, uint8_t arg, uint8_t out[32]) {
    std::memset(out, 0, 32);
    if (!svc_ || !inSig_ || !path_) return Status::Error;

    uint8_t in[32] = {};
    in[1] = op;
    in[3] = kCmdCharge;
    in[4] = sub;
    in[6] = arg;

    ComPtr<IWbemClassObject> params;
    if (FAILED(inSig_->SpawnInstance(0, &params))) return Status::Error;
    {
        Variant v;
        SAFEARRAY* sa = SafeArrayCreateVector(VT_UI1, 0, sizeof(in));
        if (!sa) return Status::Error;
        void* data = nullptr;
        if (FAILED(SafeArrayAccessData(sa, &data))) { SafeArrayDestroy(sa); return Status::Error; }
        std::memcpy(data, in, sizeof(in));
        SafeArrayUnaccessData(sa);
        v.v.vt = VT_ARRAY | VT_UI1;
        v.v.parray = sa;  // освобождается в ~Variant
        if (FAILED(params->Put(L"InData", 0, &v.v, 0))) return Status::Error;
    }

    ComPtr<IWbemClassObject> result;
    Bstr method(L"MiInterface");
    HRESULT hr = svc_->ExecMethod(path_, method, 0, nullptr, params.Get(), &result, nullptr);
    if (FAILED(hr) || !result) {
        app::Log(L"MIFS: ExecMethod hr=0x%08X", hr);
        return Status::Error;
    }

    size_t outLen = 0;
    {
        Variant v;
        if (FAILED(result->Get(L"OutData", 0, &v.v, nullptr, nullptr)) || v.v.vt != (VT_ARRAY | VT_UI1) || !v.v.parray)
            return Status::Error;
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(v.v.parray, 1, &lo);
        SafeArrayGetUBound(v.v.parray, 1, &hi);
        outLen = hi >= lo ? static_cast<size_t>(hi - lo + 1) : 0;
        if (outLen > 32) outLen = 32;
        void* data = nullptr;
        if (outLen == 0 || FAILED(SafeArrayAccessData(v.v.parray, &data))) return Status::Error;
        std::memcpy(out, data, outLen);
        SafeArrayUnaccessData(v.v.parray);
    }
    // В журнал — только записи и неуспешные ответы (периодические чтения не засоряют лог).
    if (op == 0xFB || outLen < 8 || out[1] != 0x80) app::LogBytes(op == 0xFB ? L"SET" : L"GET", in, sizeof(in), out, outLen);

    if (outLen < 8) return Status::Error;
    if (out[1] == 0xE0) return Status::Unsupported;
    if (out[1] != 0x80) return Status::Error;
    return Status::Ok;
}

Status Client::Get(uint8_t sub, uint8_t& value) {
    uint8_t out[32];
    Status s = RawCall(0xFA, sub, 0x00, out);
    if (s != Status::Ok) return s;
    if (out[3] != kCmdCharge || out[4] != sub) return Status::Error;  // чужой ответ
    value = out[6];
    return Status::Ok;
}

Status Client::SetLimitCode(uint8_t code) {
    if (code != kCodeOff && code != kCode80) return Status::Error;  // whitelist

    Status last = Status::Error;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        uint8_t out[32];
        // Как у штатного ПО: сначала сброс, затем нужный код.
        last = RawCall(0xFB, kSubLimit, kCodeOff, out);
        if (last == Status::Unsupported) return last;
        if (last == Status::Ok && code != kCodeOff) {
            Sleep(100);
            last = RawCall(0xFB, kSubLimit, code, out);
            if (last == Status::Unsupported) return last;
        }
        if (last == Status::Ok) {
            Sleep(100);
            uint8_t now = 0xFF;
            last = Get(kSubLimit, now);
            if (last == Status::Ok && now == code) return Status::Ok;
            app::Log(L"MIFS: проверка не прошла (ожидалось %02X, прочитано %02X), попытка %d", code, now, attempt);
            last = Status::Error;
        }
        Sleep(300 * attempt);
    }
    return last;
}

bool ListenEvents(const wchar_t* eventClass, HANDLE stop,
                  const std::function<void(const uint8_t* data, size_t len)>& cb) {
    ComPtr<IWbemServices> svc;
    if (!ConnectWmi(svc)) return false;

    std::wstring query = L"SELECT * FROM ";
    query += eventClass;
    ComPtr<IEnumWbemClassObject> en;
    Bstr wql(L"WQL"), q(query.c_str());
    HRESULT hr = svc->ExecNotificationQuery(wql, q, WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY, nullptr, &en);
    if (FAILED(hr)) { app::Log(L"WMI: подписка на %s hr=0x%08X", eventClass, hr); return false; }
    CoSetProxyBlanket(en.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        ComPtr<IWbemClassObject> ev;
        ULONG n = 0;
        hr = en->Next(1500, 1, &ev, &n);
        if (hr == WBEM_S_TIMEDOUT) continue;
        if (FAILED(hr)) { app::Log(L"WMI: события %s hr=0x%08X", eventClass, hr); return false; }
        if (n == 0 || !ev) continue;

        Variant v;
        if (SUCCEEDED(ev->Get(L"EventDetail", 0, &v.v, nullptr, nullptr)) && v.v.vt == (VT_ARRAY | VT_UI1) && v.v.parray) {
            LONG lo = 0, hi = -1;
            SafeArrayGetLBound(v.v.parray, 1, &lo);
            SafeArrayGetUBound(v.v.parray, 1, &hi);
            size_t len = hi >= lo ? static_cast<size_t>(hi - lo + 1) : 0;
            uint8_t buf[32] = {};
            if (len > sizeof(buf)) len = sizeof(buf);
            void* data = nullptr;
            if (len && SUCCEEDED(SafeArrayAccessData(v.v.parray, &data))) {
                std::memcpy(buf, data, len);
                SafeArrayUnaccessData(v.v.parray);
                cb(buf, len);
            }
        }
    }
    return true;
}

}  // namespace mifs
