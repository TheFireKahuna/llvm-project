// A stand-in for the Windows SDK's wrl/def.h.
#pragma once
#include <sdkddkver.h>
#if _MSC_VER < 1600
#error WRL requires compiler version 16.00 or greater
#endif
