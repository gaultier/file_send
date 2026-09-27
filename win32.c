#pragma once
#include "lib.c"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

// Win32 compiles and links, and does nothing else yet: every slot of both
// vtables is null, so the first call through either one faults. In `main` that
// is `arena_valloc` asking `Env` for the page size, which happens before
// anything has had a chance to report anything -- so this builds and does not
// run, and is not to be mistaken for a port.
//
// It is here rather than absent because the build is what keeps `lib.c`,
// `torrent.c` and `sha2.c` free of anything Unix. That is a property a compiler
// can check, and one that rots the moment nothing checks it; `matrix.sh` checks
// it on two Windows targets. The implementations come after.
//
// `io_platform_make` still answers `ErrKindUnsupported` rather than handing back
// a vtable of nulls. Not because anything reaches it first -- nothing does --
// but because a function that can report has no business faulting instead.

// The CRT's own `errno` table, not `GetLastError`: what an `Error` carries is
// whatever this file put in it, and the wrappers below will be putting `errno`
// there.
__attribute__((warn_unused_result)) static bool
platform_error_describe(u64 os_error, char *dst, usize dst_len) {
  assert(dst);
  assert(dst_len > 0);

  return 0 == strerror_s(dst, dst_len, (i32)os_error);
}

// TODO: `GetSystemInfo` for the page size, `VirtualAlloc` and `VirtualProtect`
// for the arena, `GetCurrentProcessId`, Winsock for the sockets, and
// `CreateFileMapping` plus `MapViewOfFile` for `map_fd`.
__attribute__((warn_unused_result)) static const Env *env_platform_make(void) {
  // A `static` for the same reason as on Unix: an `IO` holds on to the `Env` it
  // was made with, and there is one per process to hold on to.
  static const Env env = {0};

  return &env;
}

// TODO: a completion port. Unlike kqueue and epoll, and like io_uring, the
// kernel makes the syscall here, so this is not a readiness loop: an overlapped
// operation is started at submission and `GetQueuedCompletionStatusEx` collects
// what finished. `unix_io_perform` has no counterpart -- the whole point is that
// this process does not make the call -- and `IoAction` is what an `OVERLAPPED`
// is filled in from.
__attribute__((warn_unused_result)) static Error
io_platform_make(Arena *arena, const Env *env, IoBackend backend, IO **dst) {
  (void)arena;
  (void)env;
  (void)dst;
  (void)backend;

  // Every backend, `Default` included: there is nothing here to hand back yet,
  // and saying so is better than handing back a vtable that faults on first use.
  return (Error){.kind = ErrKindUnsupported};
}
