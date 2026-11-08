// RedmiBook Charge Limit — один exe:
//   (без аргументов) / --show   окно + трей
//   --tray                       только трей (автозапуск службой при входе)
//   --service                    служба Windows (запускается SCM)
//   --install / --uninstall      установка и полное удаление (UAC), --quiet без сообщений
//   --diag                       диагностика прошивки и кнопки Xiaomi (от администратора)
#include "common.h"
#include "installer.h"
#include "service.h"
#include "ui.h"

#include <shellapi.h>

namespace {
bool HasArg(int argc, wchar_t** argv, const wchar_t* a) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], a) == 0) return true;
    return false;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // Безопасная загрузка DLL только из System32.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = 0;
    if (!argv) return 1;

    bool quiet = HasArg(argc, argv, L"--quiet");
    if (HasArg(argc, argv, L"--service")) rc = service::Run();
    else if (HasArg(argc, argv, L"--diag")) rc = service::RunDiag();
    else if (HasArg(argc, argv, L"--install")) rc = installer::Install(quiet);
    else if (HasArg(argc, argv, L"--uninstall")) rc = installer::Uninstall(quiet);
    else rc = ui::Run(inst, HasArg(argc, argv, L"--tray"));

    LocalFree(argv);
    return rc;
}
