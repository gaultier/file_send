#pragma once
#include "lib.c"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

// Win32 compiles and links, and does nothing else yet: every slot of the
// vtable is null, so the first call through it faults. In `main` that is
// `arena_valloc` asking `Platform` for the page size, which happens before
// anything has had a chance to report anything -- so this builds and does not
// run, and is not to be mistaken for a port.
//
// It is here rather than absent because the build is what keeps `lib.c`,
// `torrent.c` and `sha2.c` free of anything Unix. That is a property a compiler
// can check, and one that rots the moment nothing checks it; `matrix.sh` checks
// it on two Windows targets. The implementations come after.
//
// `platform_init` still answers `ErrKindUnsupported` rather than handing
// back a vtable of nulls. Not because anything reaches it first -- nothing does
// -- but because a function that can report has no business faulting instead.

// The CRT's own `errno` table, not `GetLastError`: what an `Error` carries is
// whatever this file put in it, and the wrappers below will be putting `errno`
// there.
__attribute__((warn_unused_result)) static bool
platform_error_describe(u64 os_error, char *dst, usize dst_len) {
  assert(dst);
  assert(dst_len > 0);

  return 0 == strerror_s(dst, dst_len, (i32)os_error);
}

// TODO: `CreateFileW`, `ReadFile`, `WriteFile` and Winsock for these.
__attribute__((warn_unused_result)) static Error
win32_open(Platform *platform, Bytes path, FileOpenOptions options,
           i32 *dst_fd) {
  (void)platform;
  (void)path;
  (void)options;
  (void)dst_fd;

  return (Error){.kind = ErrKindUnsupported};
}

__attribute__((warn_unused_result)) static Error win32_close(Platform *platform,
                                                             i32 fd) {
  (void)platform;
  (void)fd;

  return (Error){.kind = ErrKindUnsupported};
}

__attribute__((warn_unused_result)) static Error
win32_read(Platform *platform, i32 fd, Bytes data, usize *dst_read) {
  (void)platform;
  (void)fd;
  (void)data;
  (void)dst_read;

  return (Error){.kind = ErrKindUnsupported};
}

__attribute__((warn_unused_result)) static Error
win32_write(Platform *platform, i32 fd, Bytes data, usize *dst_written) {
  (void)platform;
  (void)fd;
  (void)data;
  (void)dst_written;

  return (Error){.kind = ErrKindUnsupported};
}

__attribute__((warn_unused_result)) static Error
win32_accept(Platform *platform, i32 listen_socket, i32 *dst_accept_socket,
             Ipv4Addr *dst_accept_addr) {
  (void)platform;
  (void)listen_socket;
  (void)dst_accept_socket;
  (void)dst_accept_addr;

  return (Error){.kind = ErrKindUnsupported};
}

__attribute__((warn_unused_result)) static Error
win32_connect(Platform *platform, i32 fd, Ipv4Addr addr) {
  (void)platform;
  (void)fd;
  (void)addr;

  return (Error){.kind = ErrKindUnsupported};
}

__attribute__((warn_unused_result)) static Error
win32_send_to(Platform *platform, i32 fd, Ipv4Addr addr, Bytes data,
              usize *dst_sent) {
  (void)platform;
  (void)fd;
  (void)addr;
  (void)data;
  (void)dst_sent;

  return (Error){.kind = ErrKindUnsupported};
}

__attribute__((warn_unused_result)) static Error
win32_file_size(Platform *platform, i32 fd, usize *dst_size) {
  (void)platform;
  (void)fd;
  (void)dst_size;

  return (Error){.kind = ErrKindUnsupported};
}

__attribute__((warn_unused_result)) static Error
win32_remove_file(Platform *platform, Bytes path) {
  (void)platform;
  (void)path;

  return (Error){.kind = ErrKindUnsupported};
}

// TODO: `GetSystemInfo` for the page size, `VirtualAlloc` and `VirtualProtect`
// for the arena, `GetCurrentProcessId`, Winsock for the sockets, and
// `CreateFileMapping` plus `MapViewOfFile` for `map_fd`.
__attribute__((warn_unused_result)) static Platform *env_platform_make(void) {
  // A `static` for the same reason as on Unix: there is one per process, and
  // `platform_init` copies it rather than holding on to it.
  static Platform platform = {
      .open = win32_open,
      .close = win32_close,
      .read = win32_read,
      .write = win32_write,
      .accept = win32_accept,
      .connect = win32_connect,
      .send_to = win32_send_to,
      .file_size = win32_file_size,
      .remove_file = win32_remove_file,
  };

  return &platform;
}

// TODO: a completion port. Unlike kqueue and epoll, and like io_uring, the
// kernel makes the syscall here, so this is not a readiness loop: an overlapped
// operation is started at submission and `GetQueuedCompletionStatusEx` collects
// what finished. `unix_platform_perform` has no counterpart -- the whole point
// is that this process does not make the call -- and `PlatformAction` is what
// an `OVERLAPPED` is filled in from.
__attribute__((warn_unused_result)) static Error
platform_init(Arena *arena, PlatformBackend backend, Platform **dst) {
  (void)arena;
  (void)dst;
  (void)backend;

  // Every backend, `Default` included: there is nothing here to hand back yet,
  // and saying so is better than handing back a vtable that faults on first
  // use.
  return (Error){.kind = ErrKindUnsupported};
}
