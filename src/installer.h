#pragma once

#include "common.h"

namespace installer {

// Установлена ли служба.
bool IsInstalled();
// Запущен ли exe из папки установки.
bool RunningFromInstallDir();

// Режимы --install / --uninstall (нужны права администратора). Возвращают код выхода.
int Install(bool quiet);
int Uninstall(bool quiet);

// Запуск себя с повышением прав (UAC). Возвращает дескриптор процесса (закрыть CloseHandle) или nullptr,
// если пользователь отказался.
HANDLE StartElevated(const wchar_t* args, HWND owner);
// То же с ожиданием; возвращает код выхода или -1.
int RunElevated(const wchar_t* args);

}  // namespace installer
