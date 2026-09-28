// A stand-in for the UCRT's sys/stat.h, which declares its POSIX names unless
// __STDC__ is true.
#pragma once
#include <corecrt.h>
#include <sys/types.h>
#if !__STDC__
struct stat {
  _off_t st_size;
};
#endif
