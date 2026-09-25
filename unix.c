#pragma once

#include "lib.c"

#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// `strerror_r` and not `strerror`: a thread is spawned per client, and
// `strerror` hands back a buffer shared by the whole process. This is the XSI
// spelling, the one `_POSIX_C_SOURCE` selects, which answers with 0 or an
// error number rather than with a `char *`.
__attribute__((warn_unused_result)) static bool
platform_error_describe(u64 os_error, char *dst, usize dst_len) {
  assert(dst);
  assert(dst_len > 0);

  return 0 == strerror_r((i32)os_error, dst, dst_len);
}

// Map an `errno` onto our own errors. Shared by every syscall wrapper: an
// `errno` means the same thing whichever call produced it, so the mapping
// lives in one place rather than being repeated per wrapper.
//
// Anything not listed is this code calling the kernel wrong, which is what
// `ErrInvalidData` covers: a bad descriptor, a misaligned address, a length
// of zero, an unsupported protection, a socket option that does not apply.
__attribute__((warn_unused_result)) static Error unix_error_from_errno(i32 e) {
  ErrorKind kind = ErrKindInvalidData;

  switch (e) {
  case EACCES:
  case EPERM:
    kind = ErrOSKindPermission;
    break;

  case ENOMEM:
  case ENOBUFS:
    kind = ErrKindOOM;
    break;

  case EOVERFLOW:
    kind = ErrKindRange;
    break;

  case EADDRINUSE:
    kind = ErrKindAddrInUse;
    break;

  // `EAGAIN` and `EWOULDBLOCK` are permitted to be the same value, and on
  // this platform they are, so the second label would be a duplicate case.
  case EAGAIN:
#if EAGAIN != EWOULDBLOCK
  case EWOULDBLOCK:
#endif
    kind = ErrKindAgain;
    break;

  case EINTR:
    kind = ErrKindInterrupted;
    break;

  case ECONNABORTED:
  case ECONNRESET:
  case EPIPE:
    kind = ErrKindConnReset;
    break;

  case EMFILE: // Per process limit.
  case ENFILE: // System wide limit.
    kind = ErrKindTooManyFiles;
    break;

  case EHOSTUNREACH:
    kind = ErrKindHostUnreachable;
    break;

  default:
    kind = ErrKindInvalidData;
    break;
  }

  // The `errno` value rides along with the kind so a caller can render the
  // system's own description. `errno` is never 0 on a real failure, so a
  // `data` of 0 reads as "no OS detail", which is also what the `e == 0`
  // case produces.
  return (Error){.kind = kind, .data = (u64)e};
}

// On success `*res` is the mapping; on failure it is left alone and the
// `errno` `mmap` set is mapped onto an `Error`.
__attribute__((warn_unused_result)) static Error
unix_valloc(const IO *io, usize bytes_count, u8 **res) {
  (void)io;

  assert(bytes_count > 0);
  assert(res);

  void *const alloc = mmap(NULL, bytes_count, PROT_READ | PROT_WRITE,
                           MAP_ANON | MAP_PRIVATE, -1, 0);

  if ((void *)-1 == alloc) {
    return unix_error_from_errno(errno);
  }

  *res = alloc;
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static usize
unix_get_page_size(const IO *io) {
  (void)io;

  const i64 res = sysconf(_SC_PAGE_SIZE);
  assert(-1 != res && "unreachable");

  return (usize)res;
}

__attribute__((warn_unused_result)) static Error
unix_vprotect_none(const IO *io, void *ptr, usize size) {
  (void)io;

  if (-1 == mprotect(ptr, size, PROT_NONE)) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_socket(const IO *io, SocketDomain domain, SocketType type, i32 *fd) {
  (void)io;

  assert(fd);

  i32 unix_domain = 0;
  switch (domain) {
  case SocketDomainIpv4:
    unix_domain = AF_INET;
    break;
  default:
    assert(0 && "todo");
  }

  i32 unix_type = 0;
  switch (type) {
  case SocketTypeUdp:
    unix_type = SOCK_DGRAM;
    break;
  case SocketTypeTcp:
    unix_type = SOCK_STREAM;
    break;
  default:
    assert(0 && "todo");
  }

  const i32 ret = socket(unix_domain, unix_type, 0);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  *fd = ret;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_listen(const IO *io, i32 fd, i32 backlog) {
  (void)io;

  const i32 ret = listen(fd, backlog);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_open(const IO *io, Slice_u8 path, FileOpenOptions options, i32 *fd) {
  (void)io;

  assert(fd);

  i32 unix_options = 0;
  if (FileOpenOptionsReadOnly & options) {
    unix_options |= O_RDONLY;
  } else if (FileOpenOptionsWriteOnly & options) {
    unix_options |= O_WRONLY;
  }
  if (FileOpenOptionsCreate & options) {
    unix_options |= O_CREAT;
  }
  if (FileOpenOptionsTruncate & options) {
    unix_options |= O_TRUNC;
  }

  // Only consulted when `O_CREAT` actually creates the file, and the process
  // umask reduces it from there, so the usual outcome is `0644`. `0666` and
  // not `0777`: nothing this opens is meant to be executable.
  const mode_t unix_mode = 0666;

  // FILE_PATH_MAX
  char unix_path[4096] = {0};
  const usize unix_path_max_len = sizeof(unix_path) - 1;

  if (!path.data || 0 == path.len) {
    return (Error){.kind = ErrKindInvalidData};
  }

  if (path.len > unix_path_max_len) {
    return (Error){.kind = ErrKindRange, .data = unix_path_max_len};
  }

  memcpy(unix_path, path.data, path.len);

  i32 ret = 0;
  do {
    ret = open(unix_path, unix_options, unix_mode);
  } while (-1 == ret && EINTR == errno);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  *fd = ret;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_tcp_bind_ipv4(const IO *io, i32 listen_socket, Ipv4Addr addr) {

  (void)io;

  struct sockaddr_in sock_addr_in = {
      .sin_family = AF_INET,
      .sin_port = htons(addr.port),
      .sin_addr.s_addr = htonl(addr.ip),
  };

  i32 ret = 0;
  do {
    ret = bind(listen_socket, (struct sockaddr *)&sock_addr_in,
               sizeof(sock_addr_in));
  } while (-1 == ret && EINTR == errno);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_accept(const IO *io, i32 listen_socket, i32 *dst_accept_socket,
            Ipv4Addr *dst_accept_addr) {

  (void)io;

  assert(dst_accept_socket);
  assert(dst_accept_addr);

  struct sockaddr_in sock_addr_in = {0};
  socklen_t sock_addr_in_len = sizeof(sock_addr_in);

  i32 ret = 0;
  do {
    ret = accept(listen_socket, (struct sockaddr *)&sock_addr_in,
                 &sock_addr_in_len);
  } while (-1 == ret && EINTR == errno);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  *dst_accept_socket = ret;
  dst_accept_addr->port = ntohs(sock_addr_in.sin_port);
  dst_accept_addr->ip = ntohl(sock_addr_in.sin_addr.s_addr);

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_thread_create(const IO *io, ThreadCallback cb, void *data) {
  (void)io;

  assert(cb);

  // Nothing ever joins these threads, so the implementation has to reclaim
  // the stack and the thread structure itself once the callback returns.
  pthread_attr_t attr = {0};
  const i32 ret_init = pthread_attr_init(&attr);
  if (0 != ret_init) {
    return unix_error_from_errno(ret_init);
  }
  // Only fails on an invalid detach state, and this one is a constant.
  assert(0 == pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));

  pthread_t thread = {0};
  // Unlike every other wrapper here, the `pthread_*` calls return the error
  // number directly and leave `errno` untouched.
  const i32 ret = pthread_create(&thread, &attr, cb, data);

  assert(0 == pthread_attr_destroy(&attr));

  if (0 != ret) {
    return unix_error_from_errno(ret);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error unix_close(const IO *io,
                                                            i32 fd) {
  (void)io;

  const i32 ret = close(fd);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_enable_socket_reuse(const IO *io, i32 fd) {
  (void)io;

  int val = 1;
  const int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_read(const IO *io, i32 fd, Slice_u8 data, usize *dst_read) {
  (void)io;

  assert(dst_read);

  isize ret = 0;
  do {
    ret = read(fd, data.data, data.len);
  } while (-1 == ret && EINTR == errno);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  assert(ret >= 0);
  *dst_read = (usize)ret;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_write(const IO *io, i32 fd, Slice_u8 data, usize *dst_written) {
  (void)io;

  assert(dst_written);

  isize ret = 0;
  do {
    ret = write(fd, data.data, data.len);
  } while (-1 == ret && EINTR == errno);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  assert(ret >= 0);
  *dst_written = (usize)ret;

  return (Error){.kind = ErrKindNone};
}
__attribute__((warn_unused_result)) static Error
unix_udp_multicast_open_ipv4(const IO *io, u32 ipv4, i32 *dst_fd) {
  (void)io;

  assert(dst_fd);

  const i32 fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (-1 == fd) {
    return unix_error_from_errno(errno);
  }

  const u8 ttl = 1;
  if (-1 == setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl))) {
    const Error err = unix_error_from_errno(errno);
    (void)close(fd);
    return err;
  }

  const struct in_addr iface = {.s_addr = htonl(ipv4)};
  if (-1 ==
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface))) {
    const Error err = unix_error_from_errno(errno);
    (void)close(fd);
    return err;
  }

  *dst_fd = fd;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_udp_send_to_ipv4(const IO *io, i32 fd, Ipv4Addr addr, const u8 *buf,
                      usize len, usize *dst_sent) {
  (void)io;
  assert(buf);
  assert(dst_sent);

  const struct sockaddr_in sock_addr_in = {
      .sin_family = AF_INET,
      .sin_port = htons(addr.port),
      .sin_addr.s_addr = htonl(addr.ip),
  };

  isize ret = 0;
  do {
    ret = sendto(fd, buf, len, 0, (const struct sockaddr *)&sock_addr_in,
                 sizeof(sock_addr_in));
  } while (-1 == ret && EINTR == errno);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  *dst_sent = (usize)ret;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_file_size(const IO *io, i32 fd, usize *dst_size) {
  (void)io;
  assert(dst_size);

  struct stat st = {0};
  const isize ret = fstat(fd, &st);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  *dst_size = (usize)st.st_size;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_remove_file(const IO *io, Slice_u8 path) {
  (void)io;

  // Same bound and the same reason as `unix_open`: the name has to reach the
  // kernel as a NUL terminated string, and the slice carries no terminator.
  char unix_path[4096] = {0};
  const usize unix_path_max_len = sizeof(unix_path) - 1;

  if (!path.data || 0 == path.len) {
    return (Error){.kind = ErrKindInvalidData};
  }

  if (path.len > unix_path_max_len) {
    return (Error){.kind = ErrKindRange, .data = unix_path_max_len};
  }

  memcpy(unix_path, path.data, path.len);

  if (-1 == unlink(unix_path)) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static usize
unix_get_process_id(const IO *io) {
  (void)io;

  // `getpid` cannot fail, and a pid is never negative.
  const i64 res = (i64)getpid();
  assert(res >= 0);

  return (usize)res;
}

__attribute__((warn_unused_result)) static Error
unix_stdout_silence(const IO *io, i32 *dst_saved) {
  (void)io;
  assert(dst_saved);

  // Anything already buffered belongs on the real stdout, so it has to go out
  // before the descriptor underneath it is replaced.
  (void)fflush(stdout);

  const i32 saved = dup(STDOUT_FILENO);
  if (-1 == saved) {
    return unix_error_from_errno(errno);
  }

  const i32 devnull = open("/dev/null", O_WRONLY);
  if (-1 == devnull) {
    const Error err = unix_error_from_errno(errno);
    (void)close(saved);
    return err;
  }

  if (-1 == dup2(devnull, STDOUT_FILENO)) {
    const Error err = unix_error_from_errno(errno);
    (void)close(devnull);
    (void)close(saved);
    return err;
  }

  // `dup2` gave `STDOUT_FILENO` its own reference to the same description.
  (void)close(devnull);

  *dst_saved = saved;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_stdout_restore(const IO *io, i32 saved) {
  (void)io;

  // Whatever the silenced stretch wrote went to `/dev/null` and is of no
  // interest, but the stream still has to be emptied against the descriptor
  // it was written for.
  (void)fflush(stdout);

  if (-1 == dup2(saved, STDOUT_FILENO)) {
    const Error err = unix_error_from_errno(errno);
    (void)close(saved);
    return err;
  }

  if (-1 == close(saved)) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

// Composites: one operation to the program, several to Unix. They go
// through `io` so another platform can implement the same operation out
// of entirely different primitives, and so the primitives can be faked
// underneath them in a test.
__attribute__((warn_unused_result)) static Error
unix_map_file(const IO *io, Slice_u8 path, FileOpenOptions opts,
              Slice_u8 *dst) {
  (void)io;
  assert(dst);

  Error err = {0};

  i32 fd = 0;
  err = io->open(io, path, FileOpenOptionsReadOnly, &fd);
  if (ErrKindNone != err.kind) {
    return err;
  }

  usize file_size = 0;
  err = io->file_size(io, fd, &file_size);
  if (ErrKindNone != err.kind) {
    (void)io->close(io, fd);
    return err;
  }

  i32 unix_opts = 0;
  if (opts & FileOpenOptionsReadOnly) {
    unix_opts = PROT_READ;
  } else if (opts & FileOpenOptionsWriteOnly) {
    unix_opts = PROT_WRITE;
  }

  void *const data = mmap(NULL, file_size, unix_opts, MAP_PRIVATE, fd, 0);
  // Read `errno` before `close` gets a chance to overwrite it.
  const Error err_mmap =
      ((void *)-1 == data) ? unix_error_from_errno(errno) : (Error){0};

  // The mapping holds its own reference to the file, so the descriptor has
  // done its job either way.
  (void)io->close(io, fd);

  if (ErrKindNone != err_mmap.kind) {
    return err_mmap;
  }

  dst->data = data;
  dst->len = file_size;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_write_all_to_file(const IO *io, Slice_u8 path, Slice_u8 data) {
  (void)io;

  // TODO: Should we still 'touch' the file?
  if (!data.data || data.len == 0) {
    return (Error){.kind = ErrKindNone};
  }

  Error err = {0};

  i32 fd = 0;
  err = io->open(io, path,
                 FileOpenOptionsWriteOnly | FileOpenOptionsCreate |
                     FileOpenOptionsTruncate,
                 &fd);
  if (ErrKindNone != err.kind) {
    return err;
  }

  Slice_u8 remaining = data;

  for (; remaining.len > 0;) {
    usize written = 0;
    err = io->write(io, fd, remaining, &written);

    // A signal before any progress is not a failure: reissue the call.
    if (ErrKindInterrupted == err.kind) {
      continue;
    }

    if (ErrKindNone != err.kind) {
      goto end;
    }

    // A short write is ordinary; a write of nothing would loop forever.
    if (0 == written) {
      err = (Error){.kind = ErrKindConnReset};
      goto end;
    }

    slice_u8_advance(&remaining, written);
  }

end:
  (void)io->close(io, fd);

  return err;
}

__attribute__((warn_unused_result)) static IO io_platform_make(void) {
  return (IO){
      .socket = unix_socket,
      .listen = unix_listen,
      .open = unix_open,
      .tcp_bind_ipv4 = unix_tcp_bind_ipv4,
      .accept = unix_accept,
      .thread_create = unix_thread_create,
      .close = unix_close,
      .enable_socket_reuse = unix_enable_socket_reuse,
      .udp_multicast_open_ipv4 = unix_udp_multicast_open_ipv4,
      .udp_send_to_ipv4 = unix_udp_send_to_ipv4,
      .read = unix_read,
      .write = unix_write,
      .file_size = unix_file_size,
      .map_file = unix_map_file,
      .write_all_to_file = unix_write_all_to_file,
      .remove_file = unix_remove_file,
      .get_page_size = unix_get_page_size,
      .valloc = unix_valloc,
      .vprotect_none = unix_vprotect_none,
      .get_process_id = unix_get_process_id,
      .stdout_silence = unix_stdout_silence,
      .stdout_restore = unix_stdout_restore,
      // The Unix implementation is stateless; every slot ignores its `ctx`.
      .ctx = NULL,
  };
}
