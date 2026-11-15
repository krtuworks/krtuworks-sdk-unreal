#pragma once

// The core's public API is called across module boundaries (a game's module, KrtuworksSteam), so it is exported
// from the Krtuworks module: UBT defines KRTUWORKS_API as DLLEXPORT for it and DLLIMPORT for every module that
// depends on it. The core's own files include no engine header, so those two come from the engine's platform header
// here. Outside Unreal (the core's own tests) the core is plain C++ and nothing is exported.
#ifdef KRTUWORKS_API
#ifndef DLLEXPORT
#include "HAL/Platform.h"
#endif
#define KRTU_API KRTUWORKS_API
#else
#define KRTU_API
#endif
