// A stand-in for the UCRT's sys/types.h, which defines its POSIX names unless
// __STDC__ is true.
#pragma once
typedef long _off_t;
#if !__STDC__
typedef _off_t off_t;
#endif
