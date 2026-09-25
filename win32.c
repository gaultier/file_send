#pragma once
#include "lib.c"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

// The CRT's own `errno` table, not `GetLastError`: what an `Error` carries is
// whatever this file put in it, and the wrappers below will be putting
// `errno` there.
__attribute__((warn_unused_result)) static bool
platform_error_describe(u64 os_error, char *dst, usize dst_len) {
  assert(dst);
  assert(dst_len > 0);

  return 0 == strerror_s(dst, dst_len, (i32)os_error);
}

__attribute__((warn_unused_result)) static IO io_platform_make(void) {
  return (IO){
      0 // TODO
  };
}
