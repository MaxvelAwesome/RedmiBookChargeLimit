#pragma once

#include "common.h"

namespace ui {

// Окно + иконка в трее. startHidden — запуск только в трей (автозагрузка).
int Run(HINSTANCE inst, bool startHidden);

}  // namespace ui
