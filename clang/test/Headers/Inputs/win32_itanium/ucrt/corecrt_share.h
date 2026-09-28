// A stand-in for the UCRT's corecrt_share.h, which io.h and wchar.h include,
// and which defines the sharing modes' POSIX names unless __STDC__ is true.
#pragma once
#include <corecrt.h>
#define _SH_DENYNO 0x40
#if !__STDC__
#define SH_DENYNO _SH_DENYNO
#endif
