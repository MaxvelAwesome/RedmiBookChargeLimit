#pragma once

#include "common.h"

namespace service {

// Точка входа режима --service (вызывается SCM).
int Run();

// Консольная диагностика (--diag, нужны права администратора).
int RunDiag();

}  // namespace service
