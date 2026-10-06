#pragma once

#include "lib.c"

#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// `strerror_r` and not `strerror`: `strerror` hands back a buffer shared by
// the whole process, which is one caller's to hold on to and no other's. This
// is the XSI spelling, the one `_POSIX_C_SOURCE` selects, which answers with 0
// or an error number rather than with a `char *`.
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
unix_valloc(const Env *env, usize bytes_count, u8 **res) {
  (void)env;

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
unix_get_page_size(const Env *env) {
  (void)env;

  const i64 res = sysconf(_SC_PAGE_SIZE);
  assert(-1 != res && "unreachable");

  return (usize)res;
}

__attribute__((warn_unused_result)) static Error
unix_vprotect_none(const Env *env, void *ptr, usize size) {
  (void)env;

  if (-1 == mprotect(ptr, size, PROT_NONE)) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

// Every descriptor that reaches `IO` is non-blocking. That is not a tuning
// choice: `IO` waits for readiness and then retries the syscall, which only
// works if the retry can answer `EAGAIN`. One blocking descriptor would park
// the single thread that runs the loop on the first peer to go quiet, and with
// it every other connection.
__attribute__((warn_unused_result)) static Error unix_set_nonblocking(i32 fd) {
  const i32 flags = fcntl(fd, F_GETFL, 0);
  if (-1 == flags) {
    return unix_error_from_errno(errno);
  }

  if (-1 == fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_socket(const Env *env, SocketDomain domain, SocketType type, i32 *fd) {
  (void)env;

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

  {
    const Error err = unix_set_nonblocking(ret);
    if (ErrKindNone != err.kind) {
      (void)close(ret);
      return err;
    }
  }

  *fd = ret;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_listen(const Env *env, i32 fd, i32 backlog) {
  (void)env;

  const i32 ret = listen(fd, backlog);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_open(Bytes path, FileOpenOptions options, i32 *fd) {
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
unix_tcp_bind_ipv4(const Env *env, i32 listen_socket, Ipv4Addr *addr) {

  (void)env;
  assert(addr);

  struct sockaddr_in sock_addr_in = {
      .sin_family = AF_INET,
      .sin_port = htons(addr->port),
      .sin_addr.s_addr = htonl(addr->ip),
  };

  i32 ret = 0;
  do {
    ret = bind(listen_socket, (struct sockaddr *)&sock_addr_in,
               sizeof(sock_addr_in));
  } while (-1 == ret && EINTR == errno);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  socklen_t sock_size = sizeof(sock_addr_in);
  ret =
      getsockname(listen_socket, (struct sockaddr *)&sock_addr_in, &sock_size);
  if (-1 == ret) {
    return unix_error_from_errno(ret);
  }
  addr->ip = sock_addr_in.sin_addr.s_addr;
  addr->port = sock_addr_in.sin_port;
  assert(addr->port);

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_accept(i32 listen_socket, i32 *dst_accept_socket,
            Ipv4Addr *dst_accept_addr) {

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

  {
    const Error err = unix_set_nonblocking(ret);
    if (ErrKindNone != err.kind) {
      (void)close(ret);
      return err;
    }
  }

  *dst_accept_socket = ret;
  dst_accept_addr->port = ntohs(sock_addr_in.sin_port);
  dst_accept_addr->ip = ntohl(sock_addr_in.sin_addr.s_addr);

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_thread_create(const Env *env, ThreadCallback cb, void *data) {
  (void)env;

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

__attribute__((warn_unused_result)) static Error unix_close(i32 fd) {
  const i32 ret = close(fd);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_close_socket(const Env *env, i32 fd) {
  (void)env;

  if (-1 == close(fd)) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_enable_socket_reuse(const Env *env, i32 fd) {
  (void)env;

  int val = 1;
  const int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error unix_read(i32 fd, Bytes data,
                                                           usize *dst_read) {
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
unix_write(i32 fd, Bytes data, usize *dst_written) {
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
unix_udp_multicast_open_ipv4(const Env *env, u32 ipv4, i32 *dst_fd) {
  (void)env;

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

  {
    const Error err = unix_set_nonblocking(fd);
    if (ErrKindNone != err.kind) {
      (void)close(fd);
      return err;
    }
  }

  *dst_fd = fd;

  return (Error){.kind = ErrKindNone};
}

// A connect on a non-blocking socket almost never finishes in the call that
// starts it: it answers `EINPROGRESS`, the loop waits for the socket to become
// writable, and calls this again. That second call is what reports the outcome
// -- `EISCONN` for a connection that came up, the real reason otherwise -- so
// there is no handshake state to keep anywhere.
__attribute__((warn_unused_result)) static Error unix_connect(i32 fd,
                                                              Ipv4Addr addr) {
  const struct sockaddr_in sock_addr_in = {
      .sin_family = AF_INET,
      .sin_port = htons(addr.port),
      .sin_addr.s_addr = htonl(addr.ip),
  };

  i32 ret = 0;
  do {
    ret = connect(fd, (const struct sockaddr *)&sock_addr_in,
                  sizeof(sock_addr_in));
  } while (-1 == ret && EINTR == errno);

  if (0 == ret) {
    return (Error){.kind = ErrKindNone};
  }

  switch (errno) {
  // Already up: this was the call after the socket became writable.
  case EISCONN:
    return (Error){.kind = ErrKindNone};

  // Still going. Not a failure, and the answer that tells the loop to wait.
  case EINPROGRESS:
  case EALREADY:
    return (Error){.kind = ErrKindAgain, .data = (u64)errno};

  default:
    return unix_error_from_errno(errno);
  }
}

__attribute__((warn_unused_result)) static Error
unix_udp_send_to_ipv4(i32 fd, Ipv4Addr addr, Bytes msg, usize *dst_sent) {
  assert(dst_sent);

  const struct sockaddr_in sock_addr_in = {
      .sin_family = AF_INET,
      .sin_port = htons(addr.port),
      .sin_addr.s_addr = htonl(addr.ip),
  };

  isize ret = 0;
  do {
    ret = sendto(fd, msg.data, msg.len, 0,
                 (const struct sockaddr *)&sock_addr_in, sizeof(sock_addr_in));
  } while (-1 == ret && EINTR == errno);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  *dst_sent = (usize)ret;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
unix_file_size(i32 fd, usize *dst_size) {
  assert(dst_size);

  struct stat st = {0};
  const isize ret = fstat(fd, &st);

  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  *dst_size = (usize)st.st_size;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error unix_remove_file(Bytes path) {
  // Same bound and the same reason as `unix_open`: the name has to reach the
  // kernel as a NUL terminated string, and `Bytes` carries no terminator.
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
unix_get_process_id(const Env *env) {
  (void)env;

  // `getpid` cannot fail, and a pid is never negative.
  const i64 res = (i64)getpid();
  assert(res >= 0);

  return (usize)res;
}

__attribute__((warn_unused_result)) static Error
unix_stdout_silence(const Env *env, i32 *dst_saved) {
  (void)env;
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
unix_stdout_restore(const Env *env, i32 saved) {
  (void)env;

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

// The mapping half of `io_map_file`. What is left of `unix_map_file`: opening
// the file and asking how big it is are operations the loop submits now, and
// this is the part it has no business in, because `mmap` only records what a
// later page fault will do. Nothing waits, so it belongs to `Env`.
__attribute__((warn_unused_result)) static Error
unix_map_fd(const Env *env, i32 fd, usize size, FileOpenOptions opts,
            Bytes *dst) {
  (void)env;
  assert(dst);

  i32 unix_opts = 0;
  if (opts & FileOpenOptionsReadOnly) {
    unix_opts = PROT_READ;
  } else if (opts & FileOpenOptionsWriteOnly) {
    unix_opts = PROT_WRITE;
  }

  void *const data = mmap(NULL, size, unix_opts, MAP_PRIVATE, fd, 0);
  if ((void *)-1 == data) {
    return unix_error_from_errno(errno);
  }

  dst->data = data;
  dst->len = size;

  return (Error){.kind = ErrKindNone};
}

// Shared by every Unix backend built on readiness -- kqueue on Darwin, epoll on
// Linux -- because once the multiplexer has said a descriptor is ready, what is
// left is the same syscall either way. Only the saying differs, and that is
// what stays in the platform file.
//
// A future io_uring backend uses neither of these: there the kernel makes the
// syscall, so it builds a submission entry out of the same `IoAction` rather
// than coming through here.

// Can this operation answer `ErrKindAgain`, and so be worth waiting on? An
// operation on a file answers or fails, and no multiplexer has anything to say
// about it.
__attribute__((warn_unused_result)) static bool
unix_io_action_can_wait(IoActionKind kind) {
  switch (kind) {
  case IoActionKindRead:
  case IoActionKindWrite:
  case IoActionKindAccept:
  case IoActionKindConnect:
  case IoActionKindSendTo:
    return true;

  case IoActionKindNone:
  case IoActionKindOpen:
  case IoActionKindClose:
  case IoActionKindFileSize:
  case IoActionKindRemoveFile:
    return false;
  }

  assert(0 && "unreachable");
  return false;
}

// Try the operation once. Whatever it answers is the answer, `ErrKindAgain`
// included: it is the backend above that turns that one into a wait.
__attribute__((warn_unused_result)) static Error
unix_io_perform(IoCompletion *completion, i32 fd, usize *dst_res) {
  assert(completion);
  assert(dst_res);

  Error err = {.kind = ErrKindNone};
  usize res = 0;

  switch (completion->action.kind) {
  // The one operation with no descriptor to act on: it produces one, which is
  // what it reports.
  case IoActionKindOpen: {
    assert(-1 == fd);
    i32 opened = -1;
    err = unix_open(completion->action.v.open.path,
                    completion->action.v.open.options, &opened);
    if (ErrKindNone == err.kind) {
      assert(opened >= 0);
      res = (usize)(u32)opened;
    }
  } break;

  case IoActionKindClose:
    err = unix_close(fd);
    break;

  case IoActionKindRead:
    err = unix_read(fd, completion->action.v.read.data, &res);
    break;

  case IoActionKindWrite:
    err = unix_write(fd, completion->action.v.write.data, &res);
    break;

  // `fd` is the listener; what it reports is the connection.
  case IoActionKindAccept: {
    i32 accepted = -1;
    err = unix_accept(fd, &accepted, &completion->action.v.accept.addr);
    if (ErrKindNone == err.kind) {
      assert(accepted >= 0);
      res = (usize)(u32)accepted;
    }
  } break;

  case IoActionKindConnect:
    err = unix_connect(fd, completion->action.v.connect.addr);
    break;

  case IoActionKindSendTo:
    err = unix_udp_send_to_ipv4(fd, completion->action.v.send_to.addr,
                                completion->action.v.send_to.data, &res);
    break;

  case IoActionKindFileSize:
    err = unix_file_size(fd, &res);
    break;

  // Works on a name, so it has no descriptor either.
  case IoActionKindRemoveFile:
    assert(-1 == fd);
    err = unix_remove_file(completion->action.v.remove_file.path);
    break;

  // A completion reported without an action is this code having got a slot
  // wrong, not anything the operating system could have said.
  case IoActionKindNone:
    assert(0 && "unreachable");
    break;
  }

  *dst_res = res;

  return err;
}

// The clock behind every deadline, shared by both readiness backends: the
// kernel makes no distinction here, so neither does this.
//
// `CLOCK_MONOTONIC` and not `CLOCK_REALTIME`: a deadline has to survive the
// clock being set, and a realtime clock stepping back an hour would hold every
// connection open for an hour. Darwin's is `mach_absolute_time` with the
// timebase already applied, so there is no `mach_timebase_info` to do by hand.
__attribute__((warn_unused_result)) static u64 unix_monotonic_ns(IO *io) {
  (void)io;

  struct timespec ts = {0};
  // Nothing to report: the clock exists and the pointer is this stack frame's,
  // which are the only two things the call can object to.
  assert(0 == clock_gettime(CLOCK_MONOTONIC, &ts));

  return (u64)ts.tv_sec * Second + (u64)ts.tv_nsec;
}

__attribute__((warn_unused_result)) static const Env *env_platform_make(void) {
  // A `static` rather than a value handed back: an `IO` holds on to the `Env`
  // it was made with, and there is one per process to hold on to. The Unix
  // implementation is stateless, so `ctx` stays null; a test's fake is what it
  // is for.
  static const Env env = {
      .get_page_size = unix_get_page_size,
      .valloc = unix_valloc,
      .vprotect_none = unix_vprotect_none,
      .get_process_id = unix_get_process_id,

      .socket = unix_socket,
      .listen = unix_listen,
      .tcp_bind_ipv4 = unix_tcp_bind_ipv4,
      .enable_socket_reuse = unix_enable_socket_reuse,
      .udp_multicast_open_ipv4 = unix_udp_multicast_open_ipv4,
      .close_socket = unix_close_socket,
      .map_fd = unix_map_fd,

      .thread_create = unix_thread_create,

      .stdout_silence = unix_stdout_silence,
      .stdout_restore = unix_stdout_restore,
  };

  return &env;
}
