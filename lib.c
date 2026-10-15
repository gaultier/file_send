#pragma once

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef assert
#undef assert
#endif

#define min(a, b) (((a) < (b)) ? (a) : (b))

#define assert(e)                                                              \
  (__builtin_expect(!(e), 0) ? (fprintf(stderr, "%s:%s:%d:%s\n", __func__,     \
                                        __FILE_NAME__, __LINE__, #e),          \
                                fflush(stderr), __builtin_trap())              \
                             : (void)0)

#if (defined(__APPLE__) && defined(__MACH__))
#define PLATFORM_DARWIN
#endif

#if defined(__linux__)
#define PLATFORM_LINUX
#endif

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#define PLATFORM_UNIX 1
#elif defined(_WIN32)
#define PLATFORM_WIN32 1
#else
#error "unknown platform"
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef size_t usize;
typedef ssize_t isize;

#define KiB 1024
#define MiB 1024 * KiB
#define GiB 1024 * MiB

// Nanoseconds, which is the unit every duration in here is counted in: a `u64`
// of them is nearly six hundred years, so no wait and no deadline has to think
// about overflow. Named so that `2 * Minute` reads as what it is, where
// `120 * 1000 * 1000 * 1000` reads as a digit count.
//
// `static const` and not `#define`, like `KiB` above, so they carry a type and
// cannot be pasted into something that changes their meaning. The cost is that
// they are not constant expressions: an array length or a `_Static_assert`
// wanting one of these has to spell the number out.
static const u64 Microsecond = 1000;
static const u64 Millisecond = 1000 * 1000;
static const u64 Second = 1000 * 1000 * 1000;
static const u64 Minute = 60ULL * 1000 * 1000 * 1000;

typedef enum {
  ErrKindNone,
  ErrKindOOM,
  ErrKindInvalidData,
  ErrOSKindPermission,
  ErrKindRange,
  // The port is already bound.
  ErrKindAddrInUse,
  // Nothing to do right now; the same call may succeed later. This is the
  // expected, unexceptional answer from a non-blocking socket.
  ErrKindAgain,
  // A signal arrived before the call could make progress. Nothing failed:
  // the call is meant to be reissued immediately.
  ErrKindInterrupted,
  // The peer went away, at any of the points it can: before the connection
  // was accepted, during a read, or on a write to a closed socket.
  ErrKindConnReset,
  // The process or the system is out of file descriptors.
  ErrKindTooManyFiles,
  // No route to the destination. On macOS this is also how a denied Local
  // Network privacy grant surfaces, so it is not always a routing problem.
  ErrKindHostUnreachable,
  // The thing asked for is not something this build can do: an
  // `PlatformBackend` the platform does not have, say. Not a failure at runtime
  // so much as a question that has no answer here.
  ErrKindUnsupported,
} ErrorKind;

typedef struct {
  ErrorKind kind;
  // When the error came from the operating system, the `errno` that produced
  // `kind`; 0 otherwise. A real failure never leaves `errno` at 0, so 0 reads
  // unambiguously as "no OS detail to report".
  u64 data;
} Error;

// The human readable name of `kind`, for diagnostics only.
__attribute__((warn_unused_result)) static const char *
error_kind_to_cstr(ErrorKind kind) {
  switch (kind) {
  case ErrKindNone:
    return "none";
  case ErrKindOOM:
    return "out of memory";
  case ErrKindInvalidData:
    return "invalid data";
  case ErrOSKindPermission:
    return "permission denied";
  case ErrKindRange:
    return "out of range";
  case ErrKindAddrInUse:
    return "address already in use";
  case ErrKindAgain:
    return "would block";
  case ErrKindInterrupted:
    return "interrupted";
  case ErrKindConnReset:
    return "connection reset";
  case ErrKindTooManyFiles:
    return "too many open files";
  case ErrKindHostUnreachable:
    return "host unreachable";
  case ErrKindUnsupported:
    return "unsupported";
  }

  assert(0 && "unreachable");
}

// Writes the operating system's own description of `os_error` into `dst`, and
// answers whether there was one to write. Declared here and defined by the
// platform file, which the unity build reaches only much further down: what it
// takes to render a system error is per system, but wanting to render one is
// not.
__attribute__((warn_unused_result)) static bool
platform_error_describe(u64 os_error, char *dst, usize dst_len);

__attribute__((warn_unused_result)) static bool char_is_digit_ascii(u8 c) {
  return '0' <= c && c <= '9';
}

// Convert `magnitude`, optionally negated, to an isize.
// Returns `ErrRange` if the value does not fit.
__attribute__((warn_unused_result)) static Error
isize_from_usize(usize magnitude, bool negative, isize *res) {
  assert(res);

  // The overflow builtins compute in infinite precision and report whether the
  // result fits the *destination* type, so both of these are checked
  // conversions: `0 - magnitude` is a checked negation, `magnitude + 0` a
  // checked cast. This handles the asymmetric boundary (`|ISIZE_MIN|` is a
  // valid magnitude but not a valid positive value) with no special case.
  const bool overflow = negative ? __builtin_sub_overflow(0, magnitude, res)
                                 : __builtin_add_overflow(magnitude, 0, res);
  return (Error){.kind = overflow ? ErrKindRange : ErrKindNone};
}

// The byte that separates path components on this platform. Passed to the
// `path_*` helpers rather than baked into them: they are pure byte
// functions, and a hardcoded `/` would make them Unix wrappers in everything
// but name.
static const u8 PATH_SEPARATOR_UNIX = '/';

// ---------- Arena ----------

typedef struct {
  // Start of the arena allocation.
  // Increases with each allocation.
  u8 *start;
  // Size of the full arena.
  // Only used to detect the OOM case.
  u8 *end;
} Arena;

__attribute__((warn_unused_result)) static void *
arena_alloc(Arena *arena, usize align, usize elem_size, usize elem_count) {
  assert(arena != NULL);
  assert(arena->start != NULL);
  assert(arena->end != NULL);
  assert(arena->start <= arena->end);
  assert((align == 1) || (align == 2) || (align == 4) || (align == 8));
  assert(elem_size > 0);
  assert(elem_count > 0);

  usize start = (usize)arena->start;

  // Round `start` up to the next multiple of `align`.
  const usize pad = (align - (start % align)) % align;
  assert(!__builtin_add_overflow(start, pad, &start));

  usize alloc_size = 0;
  assert(!__builtin_mul_overflow(elem_size, elem_count, &alloc_size));

  usize end = 0;
  assert(!__builtin_add_overflow(start, alloc_size, &end));

  // OOM? The arena is left untouched in that case.
  if (end > (usize)arena->end) {
    return NULL;
  }

  arena->start = (u8 *)end;
  assert(arena->start <= arena->end);
  assert((start % align) == 0);

  return (u8 *)start;
}

__attribute__((warn_unused_result)) static Arena
arena_from_mem(u8 *mem, usize bytes_count) {
  assert(mem);
  assert(bytes_count);

  usize end = 0;
  assert(!__builtin_add_overflow((usize)mem, bytes_count, &end));

  const Arena res = {.start = mem, .end = (u8 *)end};
  assert(res.start <= res.end);
  return res;
}

// ---------- Bytes ----------

typedef struct {
  usize len;
  u8 *data;
} Bytes;

__attribute__((warn_unused_result)) static bool bytes_is_empty(Bytes s) {
  return NULL == s.data || 0 == s.len;
}

__attribute__((warn_unused_result)) static Bytes
bytes_from_cstr(const char *s) {
  return (Bytes){.data = (u8 *)s, .len = strlen(s)};
}

__attribute__((warn_unused_result)) static bool bytes_contains_byte(Bytes s,
                                                                    u8 byte) {
  if (bytes_is_empty(s)) {
    return false;
  }

  assert(s.data);

  return NULL != memchr(s.data, byte, s.len);
}

__attribute__((warn_unused_result)) static bool bytes_eq(Bytes a, Bytes b) {
  if (a.len != b.len) {
    return false;
  }
  if (0 == a.len) {
    assert(0 == b.len);
    return true;
  }

  assert(a.data);
  assert(a.len > 0);
  assert(b.data > 0);
  assert(b.len > 0);
  assert(a.len == b.len);

  return 0 == memcmp(a.data, b.data, a.len);
}

__attribute__((warn_unused_result)) static bool
bytes_eq_cstr(Bytes s, const char *cstr) {
  if (!cstr) {
    return bytes_is_empty(s);
  }
  return bytes_eq(s, bytes_from_cstr(cstr));
}

// Peek at the first byte of `bytes`, leaving it in place.
// Returns `ErrInvalidData`, and does not touch `*res`, if there is no first
// byte.
__attribute__((warn_unused_result)) static Error bytes_first(Bytes bytes,
                                                             u8 *res) {
  assert(res);

  if (!bytes.data) {
    return (Error){.kind = ErrKindInvalidData};
  }

  if (bytes.len == 0) {
    return (Error){.kind = ErrKindInvalidData};
  }

  *res = bytes.data[0];
  return (Error){.kind = ErrKindNone};
}

// Advance past `count` bytes. The caller must have already established that
// they are available, which is why this cannot fail: every parser here has to
// know the length before it advances anyway, either to `bytes_take` the
// bytes or to pick which error a short input deserves, so a checking variant
// would only ever re-check what the caller just established.
static void bytes_advance(Bytes *bytes, usize count) {
  assert(bytes);
  assert(bytes->data);
  assert(count <= bytes->len);

  bytes->len -= count;
  bytes->data += count;
}

// The caller must have already established that `count` bytes are available:
// silently returning a short `Bytes` would turn a malformed length into a
// successful parse of truncated data.
__attribute__((warn_unused_result)) static Bytes bytes_take(Bytes input,
                                                            usize count) {
  assert(count <= input.len);

  return (Bytes){.data = input.data, .len = count};
}

__attribute__((unused)) __attribute__((warn_unused_result)) static Bytes
bytes_skip(Bytes input, usize count) {
  if (count > input.len) {
    return (Bytes){0};
  }

  return (Bytes){.data = input.data + count, .len = input.len - count};
}

// [from, to)
__attribute__((warn_unused_result)) static Bytes
bytes_subslice(Bytes input, usize from, usize to) {
  assert(from <= to);
  assert(to <= input.len);

  if (from == to) {
    // Adding to NULL, even 0, is UB in C.
    return (Bytes){.data = input.data ? input.data + from : NULL, .len = 0};
  }
  assert(input.data);

  return (Bytes){.data = input.data + from, .len = to - from};
}

__attribute__((warn_unused_result)) static bool
bytes_starts_with(Bytes s, Bytes prefix) {
  if (s.len < prefix.len) {
    return false;
  }

  return bytes_eq(prefix, bytes_take(s, prefix.len));

  return true;
}

__attribute__((warn_unused_result)) static Bytes bytes_make(u8 *data,
                                                            usize len) {
  if (0 != len) {
    assert(data);
  }

  return (Bytes){.data = data, .len = len};
}

__attribute__((warn_unused_result)) static Error bytes_expect_u8(Bytes *bytes,
                                                                 u8 expected) {
  assert(bytes);
  assert(bytes->data);

  u8 actual = 0;
  if (ErrKindNone != bytes_first(*bytes, &actual).kind) {
    return (Error){.kind = ErrKindInvalidData};
  }
  if (actual != expected) {
    return (Error){.kind = ErrKindInvalidData};
  }

  bytes_advance(bytes, 1);
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static bool bytes_consume_u8(Bytes *bytes,
                                                                 u8 *dst) {
  assert(bytes);
  if (bytes->len < sizeof(*dst)) {
    return false;
  }

  if (dst) {
    *dst = bytes->data[0];
  }

  bytes_advance(bytes, sizeof(*dst));

  return true;
}

__attribute__((warn_unused_result)) static bool
bytes_consume_u32_be(Bytes *bytes, u32 *dst) {
  assert(bytes);
  assert(dst);

  if (bytes->len < sizeof(*dst)) {
    return false;
  }

  *dst = (u32)bytes->data[0] << 24 | (u32)bytes->data[1] << 16 |
         (u32)bytes->data[2] << 8 | (u32)bytes->data[3];

  bytes_advance(bytes, sizeof(*dst));

  return true;
}

// The other half of `bytes_consume_u32_be`: four bytes, the most
// significant first. Shifts and not `htonl`, so what lands in `dst` is the
// same whatever order the host keeps its own integers in, and so that nothing
// here depends on a socket header.
static void u8_write_u32_be(u8 dst[static 4], const u32 value) {
  assert(dst);

  dst[0] = (u8)(value >> 24);
  dst[1] = (u8)(value >> 16);
  dst[2] = (u8)(value >> 8);
  dst[3] = (u8)(value >> 0);
}

// The extension of the last component of `path`, dot included, or an empty
// `Bytes` when there is none. The result borrows from `path`: nothing is
// copied, and it is always a suffix of the input.
//
// The extension is what follows the last `.` of the *last* component, so a
// dot inside a directory name is not one: `/x/y.z/f` has no extension. A `.`
// that starts the last component marks a hidden file rather than an empty
// stem, so `.bashrc` has none while `.config.json` has `.json`.
//
// The dot belongs to the result so that an empty extension (`a.`, which
// yields `.`) stays distinguishable from no extension at all (`a`, which
// yields nothing). This is Go's `filepath.Ext`; note that `path_with_ext`
// takes its replacement *without* the dot, the way Rust's
// `Path::set_extension` does.
__attribute__((warn_unused_result)) static Bytes path_get_ext(Bytes path,
                                                              u8 separator) {
  if (!path.data || 0 == path.len) {
    return (Bytes){0};
  }

  // Start of the last component. Everything before it is directories, whose
  // dots must not be mistaken for an extension separator.
  usize base = 0;
  for (usize i = path.len; i > 0; i--) {
    if (separator == path.data[i - 1]) {
      base = i;
      break;
    }
  }

  // The scan stops at `base + 1` rather than `base` so a leading dot stays
  // part of the name, per the hidden file rule above.
  for (usize i = path.len; i > base + 1; i--) {
    if ('.' == path.data[i - 1]) {
      return bytes_make(path.data + i - 1, path.len - (i - 1));
    }
  }

  return (Bytes){0};
}

// Replace the path's extension with `ext`, or append one when the path has
// none. `ext` is given without the leading dot.
//
// What counts as the existing extension is `path_get_ext`'s business; see
// there for the directory and hidden file rules.
//
// `.` and `..` fall out of those rules as ordinary names and come back as
// `..<ext>`; callers pass real file paths, so the case is pinned by the tests
// rather than special cased.
__attribute__((warn_unused_result)) static Error
path_with_ext(Bytes path, Bytes ext, u8 separator, Bytes *dst, Arena *arena) {
  assert(dst);
  assert(arena);
  assert(ext.data);
  assert(ext.len > 0);

  if (!path.data || 0 == path.len) {
    return (Error){.kind = ErrKindInvalidData};
  }

  // A trailing separator leaves no name to put an extension on.
  if (separator == path.data[path.len - 1]) {
    return (Error){.kind = ErrKindInvalidData};
  }

  // How much of `path` is kept, the dot itself excluded. The extension always
  // starts at index one or later, so at least one byte of name survives.
  const Bytes old_ext = path_get_ext(path, separator);
  assert(old_ext.len < path.len);
  const usize stem_len = path.len - old_ext.len;
  assert(stem_len > 0);

  usize dst_len = 0;
  assert(!__builtin_add_overflow(stem_len, ext.len, &dst_len));
  assert(!__builtin_add_overflow(dst_len, 1 /* The dot. */, &dst_len));

  u8 *const data = arena_alloc(arena, __alignof__(u8), sizeof(u8), dst_len);
  if (!data) {
    return (Error){.kind = ErrKindOOM};
  }

  memcpy(data, path.data, stem_len);
  data[stem_len] = '.';
  memcpy(data + stem_len + 1, ext.data, ext.len);

  *dst = bytes_make(data, dst_len);

  return (Error){.kind = ErrKindNone};
}

// Matches https://pkg.go.dev/path/filepath#Base.
//
// The result may be "/", "." or "..": it is the last path element, not a
// validated file name, so callers that need one must check it themselves.
__attribute__((warn_unused_result)) static Bytes
path_last_component(Bytes path, u8 separator) {
  //  If the path is empty, Base returns ".".
  if (bytes_is_empty(path)) {
    return (Bytes){.data = (u8 *)".", .len = 1};
  }

  //  Trailing path separators are removed before extracting the last element.
  while (!bytes_is_empty(path)) {
    if (separator == path.data[path.len - 1]) {
      path.len -= 1;
    } else {
      break;
    }
  }

  //  If the path consists entirely of separators, Base returns a single
  //  separator. Only `len` was trimmed above, so the first byte of the
  //  caller's path is still there to borrow it from.
  if (bytes_is_empty(path)) {
    assert(path.data);
    return (Bytes){.data = path.data, .len = 1};
  }

  // Counts down over one-past-the-byte so the whole walk stays in `usize`:
  // `i` is the start of the component when `path.data[i - 1]` is the
  // separator.
  for (usize i = path.len; i > 0; i--) {
    const u8 c = path.data[i - 1];
    if (separator == c) {
      const Bytes res = {.data = path.data + i, .len = path.len - i};
      // The trailing separators are gone, so there is at least one byte left
      // after the last one.
      assert(!bytes_is_empty(res));
      assert(!bytes_contains_byte(res, separator));

      return res;
    }
  }

  assert(!bytes_contains_byte(path, separator));
  return path;
}

// ---------- Platform ----------

typedef enum {
  SocketDomainIpv4,
  // TODO: More.
} SocketDomain;

typedef enum { SocketTypeUdp, SocketTypeTcp } SocketType;

typedef enum {
  FileOpenOptionsReadOnly = 1,
  FileOpenOptionsWriteOnly = 2,
  FileOpenOptionsCreate = 4,
  FileOpenOptionsTruncate = 8,
} FileOpenOptions;

typedef struct {
  u32 ip;
  u16 port;
} Ipv4Addr;

typedef void *(*ThreadCallback)(void *data);

typedef struct Platform Platform;

__attribute__((warn_unused_result)) static Platform *env_platform_make(void);

typedef enum {
  PlatformActionKindNone,
  PlatformActionKindOpen,
  PlatformActionKindClose,
  PlatformActionKindRead,
  PlatformActionKindWrite,
  PlatformActionKindAccept,
  PlatformActionKindConnect,
  PlatformActionKindSendTo,
  PlatformActionKindFileSize,
  PlatformActionKindRemoveFile,
} PlatformActionKind;

typedef struct {
  PlatformActionKind kind;
  union {
    struct {
      Bytes path;
      FileOpenOptions options;
    } open;
    struct {
      Bytes data;
    } read;
    struct {
      Bytes data;
    } write;
    struct {
      // Written on success: who connected.
      Ipv4Addr addr;
    } accept;
    struct {
      Ipv4Addr addr;
    } connect;
    struct {
      Ipv4Addr addr;
      Bytes data;
    } send_to;
    struct {
      Bytes path;
    } remove_file;
  } v;
} PlatformAction;

typedef struct PlatformCompletion PlatformCompletion;

typedef void (*PlatformCallback)(PlatformCompletion *completion, Error err,
                                 usize res);

struct PlatformCompletion {
  // The submitter's, and never the implementation's: the two have different
  // lifetimes and owners. A slot leaves it alone, so it can be set once when
  // the completion is created and left for every operation after that.
  void *ctx;
  PlatformCallback cb;
  PlatformAction action;
};

struct Platform {
  usize (*get_page_size)(Platform *platform);
  Error (*valloc)(Platform *platform, usize bytes_count, u8 **res);
  Error (*vprotect_none)(Platform *platform, void *ptr, usize size);
  usize (*get_process_id)(Platform *platform);

  Error (*socket)(Platform *platform, SocketDomain domain, SocketType type,
                  i32 *fd);
  Error (*listen)(Platform *platform, i32 fd, i32 backlog);
  Error (*tcp_bind_ipv4)(Platform *platform, i32 listen_socket, Ipv4Addr *addr);
  Error (*enable_socket_reuse)(Platform *platform, i32 fd);
  Error (*udp_multicast_open_ipv4)(Platform *platform, u32 ipv4, i32 *dst_fd);

  Error (*close_socket)(Platform *platform, i32 fd);

  Error (*map_fd)(Platform *platform, i32 fd, usize size, FileOpenOptions opts,
                  Bytes *dst);

  Error (*thread_create)(Platform *platform, ThreadCallback cb, void *data);

  Error (*stdout_silence)(Platform *platform, i32 *dst_saved);
  Error (*stdout_restore)(Platform *platform, i32 saved);

  void *ctx;

  Error (*run_for_ns)(Platform *platform, usize ns);

  u64 (*monotonic_ns)(Platform *platform);

  Error (*open)(Platform *platform, PlatformCompletion *completion, Bytes path,
                FileOpenOptions options, PlatformCallback cb);
  Error (*close)(Platform *platform, PlatformCompletion *completion, i32 fd,
                 PlatformCallback cb);
  Error (*read)(Platform *platform, PlatformCompletion *completion, i32 fd,
                Bytes data, PlatformCallback cb);
  Error (*write)(Platform *platform, PlatformCompletion *completion, i32 fd,
                 Bytes data, PlatformCallback cb);
  // On success the callback's `res` is the accepted socket, and the peer is in
  // `completion->action.v.accept.addr`.
  Error (*accept)(Platform *platform, PlatformCompletion *completion,
                  i32 listen_socket, PlatformCallback cb);
  Error (*connect)(Platform *platform, PlatformCompletion *completion, i32 fd,
                   Ipv4Addr addr, PlatformCallback cb);
  Error (*send_to)(Platform *platform, PlatformCompletion *completion, i32 fd,
                   Ipv4Addr addr, Bytes data, PlatformCallback cb);
  Error (*file_size)(Platform *platform, PlatformCompletion *completion, i32 fd,
                     PlatformCallback cb);
  Error (*remove_file)(Platform *platform, PlatformCompletion *completion,
                       Bytes path, PlatformCallback cb);
};

typedef enum {
  PlatformBackendDefault,
  PlatformBackendKqueue,
  // Linux.
  PlatformBackendEpoll,
  PlatformBackendIoUring,
  // Win32.
  PlatformBackendIocp,
} PlatformBackend;

__attribute__((warn_unused_result)) static const char *
platform_backend_to_cstr(PlatformBackend backend) {
  switch (backend) {
  case PlatformBackendDefault:
    return "default";
  case PlatformBackendKqueue:
    return "kqueue";
  case PlatformBackendEpoll:
    return "epoll";
  case PlatformBackendIoUring:
    return "io_uring";
  case PlatformBackendIocp:
    return "iocp";
  }

  assert(0 && "unreachable");
  return "?";
}

__attribute__((warn_unused_result)) static Error
platform_make(Arena *arena, PlatformBackend backend, Platform **dst);

// ---------- Platform: waiting for one thing ----------

__attribute__((warn_unused_result)) static Error
platform_run_until(Platform *platform, const bool *done, usize tick_ns) {
  assert(platform);
  assert(done);
  assert(tick_ns > 0);

  while (!*done) {
    const Error err = platform->run_for_ns(platform, tick_ns);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  return (Error){.kind = ErrKindNone};
}

#define IO_TICK_NS (100 * Millisecond)

typedef struct {
  PlatformCompletion completion;
  Error err;
  usize res;
  bool done;
} PlatformOnce;

static void platform_once_on_done(PlatformCompletion *completion, Error err,
                                  usize res) {
  assert(completion);

  PlatformOnce *const once = completion->ctx;
  assert(once);
  assert(!once->done);

  once->err = err;
  once->res = res;
  once->done = true;
}

static void platform_once_init(PlatformOnce *once) {
  assert(once);

  *once = (PlatformOnce){0};
  once->completion.ctx = once;
}

__attribute__((warn_unused_result)) static Error
platform_once_wait(Platform *platform, PlatformOnce *once) {
  assert(platform);
  assert(once);

  const Error err = platform_run_until(platform, &once->done, IO_TICK_NS);
  if (ErrKindNone != err.kind) {
    return err;
  }

  return once->err;
}

// ---------- Platform: mapping a whole file ----------

typedef struct {
  Platform *platform;
  PlatformCompletion completion;
  FileOpenOptions opts;
  Bytes *dst;
  i32 fd;
  Error err;
  bool done;
} PlatformMapFile;

static void platform_map_file_on_close(PlatformCompletion *completion,
                                       Error err, usize res) {
  assert(completion);
  (void)res;

  PlatformMapFile *const ctx = completion->ctx;
  assert(ctx);

  if (ErrKindNone == ctx->err.kind) {
    ctx->err = err;
  }
  ctx->done = true;
}

static void platform_map_file_close(PlatformMapFile *ctx) {
  assert(ctx);
  assert(ctx->platform);
  assert(ctx->fd >= 0);

  const Error err = ctx->platform->close(ctx->platform, &ctx->completion,
                                         ctx->fd, platform_map_file_on_close);
  if (ErrKindNone != err.kind) {
    // The close was never submitted, so nothing else is going to finish this.
    // The descriptor is leaked, which is the lesser of the two problems.
    if (ErrKindNone == ctx->err.kind) {
      ctx->err = err;
    }
    ctx->done = true;
  }
}

static void platform_map_file_on_file_size(PlatformCompletion *completion,
                                           Error err, usize res) {
  assert(completion);

  PlatformMapFile *const ctx = completion->ctx;
  assert(ctx);
  assert(ctx->platform);

  if (ErrKindNone != err.kind) {
    ctx->err = err;
    platform_map_file_close(ctx);
    return;
  }

  Platform *const platform = ctx->platform;
  assert(platform);

  ctx->err = platform->map_fd(platform, ctx->fd, res, ctx->opts, ctx->dst);
  platform_map_file_close(ctx);
}

static void platform_map_file_on_open(PlatformCompletion *completion, Error err,
                                      usize res) {
  assert(completion);

  PlatformMapFile *const ctx = completion->ctx;
  assert(ctx);
  assert(ctx->platform);

  if (ErrKindNone != err.kind) {
    // There is no descriptor, so there is nothing to hand back.
    ctx->err = err;
    ctx->done = true;
    return;
  }

  ctx->fd = (i32)res;
  assert(ctx->fd >= 0);

  const Error err_size = ctx->platform->file_size(
      ctx->platform, &ctx->completion, ctx->fd, platform_map_file_on_file_size);
  if (ErrKindNone != err_size.kind) {
    ctx->err = err_size;
    platform_map_file_close(ctx);
  }
}

__attribute__((warn_unused_result)) static Error
platform_map_file(Platform *platform, PlatformMapFile *ctx, Bytes path,
                  FileOpenOptions opts, Bytes *dst) {
  assert(platform);
  assert(ctx);
  assert(dst);

  *ctx = (PlatformMapFile){
      .platform = platform, .opts = opts, .dst = dst, .fd = -1};
  ctx->completion.ctx = ctx;

  return platform->open(platform, &ctx->completion, path,
                        FileOpenOptionsReadOnly, platform_map_file_on_open);
}

// ---------- Platform: writing a whole file ----------

typedef struct {
  Platform *platform;
  PlatformCompletion completion;
  Bytes remaining;
  i32 fd;
  Error err;
  bool done;
} PlatformWriteAllToFile;

static void platform_write_all_to_file_on_close(PlatformCompletion *completion,
                                                Error err, usize res) {
  assert(completion);
  (void)res;

  PlatformWriteAllToFile *const ctx = completion->ctx;
  assert(ctx);

  if (ErrKindNone == ctx->err.kind) {
    ctx->err = err;
  }
  ctx->done = true;
}

static void platform_write_all_to_file_close(PlatformWriteAllToFile *ctx) {
  assert(ctx);
  assert(ctx->platform);
  assert(ctx->fd >= 0);

  const Error err =
      ctx->platform->close(ctx->platform, &ctx->completion, ctx->fd,
                           platform_write_all_to_file_on_close);
  if (ErrKindNone != err.kind) {
    if (ErrKindNone == ctx->err.kind) {
      ctx->err = err;
    }
    ctx->done = true;
  }
}

static void platform_write_all_to_file_on_write(PlatformCompletion *completion,
                                                Error err, usize res);

// Submit the next slice of the write, or hand the descriptor back if there is
// none left.
static void platform_write_all_to_file_step(PlatformWriteAllToFile *ctx) {
  assert(ctx);
  assert(ctx->platform);

  if (0 == ctx->remaining.len) {
    platform_write_all_to_file_close(ctx);
    return;
  }

  const Error err =
      ctx->platform->write(ctx->platform, &ctx->completion, ctx->fd,
                           ctx->remaining, platform_write_all_to_file_on_write);
  if (ErrKindNone != err.kind) {
    ctx->err = err;
    platform_write_all_to_file_close(ctx);
  }
}

static void platform_write_all_to_file_on_write(PlatformCompletion *completion,
                                                Error err, usize res) {
  assert(completion);

  PlatformWriteAllToFile *const ctx = completion->ctx;
  assert(ctx);

  // A signal before any progress is not a failure: reissue the same slice.
  if (ErrKindInterrupted == err.kind) {
    platform_write_all_to_file_step(ctx);
    return;
  }

  if (ErrKindNone != err.kind) {
    ctx->err = err;
    platform_write_all_to_file_close(ctx);
    return;
  }

  // A short write is ordinary; a write of nothing would go round for ever.
  if (0 == res) {
    ctx->err = (Error){.kind = ErrKindConnReset};
    platform_write_all_to_file_close(ctx);
    return;
  }

  assert(res <= ctx->remaining.len);
  bytes_advance(&ctx->remaining, res);

  platform_write_all_to_file_step(ctx);
}

static void platform_write_all_to_file_on_open(PlatformCompletion *completion,
                                               Error err, usize res) {
  assert(completion);

  PlatformWriteAllToFile *const ctx = completion->ctx;
  assert(ctx);

  if (ErrKindNone != err.kind) {
    ctx->err = err;
    ctx->done = true;
    return;
  }

  ctx->fd = (i32)res;
  assert(ctx->fd >= 0);

  platform_write_all_to_file_step(ctx);
}

// Replace the contents of `path` with `data`. `*ctx` has to outlive the
// operation, and `ctx->err` is the answer once `ctx->done`.
__attribute__((warn_unused_result)) static Error
platform_write_all_to_file(Platform *platform, PlatformWriteAllToFile *ctx,
                           Bytes path, Bytes data) {
  assert(platform);
  assert(ctx);

  *ctx = (PlatformWriteAllToFile){
      .platform = platform, .remaining = data, .fd = -1};
  ctx->completion.ctx = ctx;

  // TODO: Should we still 'touch' the file?
  if (!data.data || 0 == data.len) {
    ctx->done = true;
    return (Error){.kind = ErrKindNone};
  }

  return platform->open(platform, &ctx->completion, path,
                        FileOpenOptionsWriteOnly | FileOpenOptionsCreate |
                            FileOpenOptionsTruncate,
                        platform_write_all_to_file_on_open);
}

// ---------- Platform: the file composites, waited out ----------

__attribute__((warn_unused_result)) static Error
platform_map_file_blocking(Platform *platform, Bytes path, FileOpenOptions opts,
                           Bytes *dst) {
  assert(platform);
  assert(dst);

  PlatformMapFile ctx = {0};
  const Error err_submit = platform_map_file(platform, &ctx, path, opts, dst);
  if (ErrKindNone != err_submit.kind) {
    return err_submit;
  }

  const Error err_run = platform_run_until(platform, &ctx.done, IO_TICK_NS);
  if (ErrKindNone != err_run.kind) {
    return err_run;
  }

  return ctx.err;
}

__attribute__((warn_unused_result)) static Error
platform_write_all_to_file_blocking(Platform *platform, Bytes path,
                                    Bytes data) {
  assert(platform);

  PlatformWriteAllToFile ctx = {0};
  const Error err_submit =
      platform_write_all_to_file(platform, &ctx, path, data);
  if (ErrKindNone != err_submit.kind) {
    return err_submit;
  }

  const Error err_run = platform_run_until(platform, &ctx.done, IO_TICK_NS);
  if (ErrKindNone != err_run.kind) {
    return err_run;
  }

  return ctx.err;
}

// ---------- Log ----------

typedef enum {
  LogLevelDebug = 1,
  LogLevelInfo = 2,
  LogLevelError = 4,
  LogLevelAll = LogLevelDebug | LogLevelInfo | LogLevelError,
} LogLevel;

typedef struct {
  u32 level_mask;
  u8 prefix[32];
} Logger;

__attribute__((warn_unused_result)) static Logger logger_make(u32 level_mask,
                                                              Bytes prefix) {
  Logger logger = {.level_mask = level_mask};
  memcpy(&logger.prefix, prefix.data,
         prefix.len < sizeof(logger.prefix) ? prefix.len
                                            : sizeof(logger.prefix));

  return logger;
}

static void log(const Logger *logger, LogLevel level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void log(const Logger *logger, LogLevel level, const char *fmt, ...) {
  assert(logger);
  assert(fmt);

  if (0 == (level & logger->level_mask)) {
    return;
  }

  printf("%.*s", (i32)sizeof(logger->prefix), logger->prefix);

  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);

  printf("\n");
}

static void log_err(const Logger *logger, const char *context, Error err) {
  assert(context);

  if (0 == err.data) {
    log(logger, LogLevelError, "%s: %s", context, error_kind_to_cstr(err.kind));
    return;
  }

  char os_msg[256] = {0};

  if (!platform_error_describe(err.data, os_msg, sizeof(os_msg))) {
    // The description did not fit or the number is not one the system knows;
    // the number itself is still worth printing.
    log(logger, LogLevelError, "%s: %s (errno %" PRIu64 ")", context,
        error_kind_to_cstr(err.kind), err.data);
    return;
  }

  log(logger, LogLevelError, "%s: %s (errno %" PRIu64 ": %s)", context,
      error_kind_to_cstr(err.kind), err.data, os_msg);
}

// ---------- Platform: serving TCP ----------

typedef void (*AcceptCallback)(Platform *platform, void *cb_ctx,
                               Ipv4Addr accept_addr, i32 accept_socket);

typedef struct {
  Platform *platform;
  PlatformCompletion completion;
  void *cb_ctx;
  AcceptCallback on_accept;
  i32 listen_socket;
  // The address actually bound: with port 0, the port the OS picked.
  Ipv4Addr addr;
  Error err;
  bool done;
} PlatformServer;

static void platform_server_on_accept(PlatformCompletion *completion, Error err,
                                      usize res);

// Wait for the next connection. The listener holds exactly one accept at a
// time: a second would be a second completion, and one connection arriving at
// a time is all a single thread can do anything with.
static void platform_server_arm(PlatformServer *server);

static void platform_server_on_close(PlatformCompletion *completion, Error err,
                                     usize res) {
  assert(completion);
  (void)err;
  (void)res;

  PlatformServer *const server = completion->ctx;
  assert(server);

  server->done = true;
}

static void platform_server_shutdown(PlatformServer *server, Error err) {
  assert(server);
  assert(server->platform);
  assert(server->listen_socket >= 0);

  if (ErrKindNone == server->err.kind) {
    server->err = err;
  }

  const Error err_close =
      server->platform->close(server->platform, &server->completion,
                              server->listen_socket, platform_server_on_close);
  if (ErrKindNone != err_close.kind) {
    server->done = true;
  }
}

static void platform_server_arm(PlatformServer *server) {
  assert(server);
  assert(server->platform);

  const Error err = server->platform->accept(
      server->platform, &server->completion, server->listen_socket,
      platform_server_on_accept);
  if (ErrKindNone != err.kind) {
    platform_server_shutdown(server, err);
  }
}

static void platform_server_on_accept(PlatformCompletion *completion, Error err,
                                      usize res) {
  assert(completion);

  PlatformServer *const server = completion->ctx;
  assert(server);
  assert(server->platform);
  assert(server->on_accept);

  // The peer is allowed to vanish between the handshake and the `accept`.
  // That is one dead connection, not a dead server.
  if (ErrKindConnReset == err.kind) {
    platform_server_arm(server);
    return;
  }

  if (ErrKindNone != err.kind) {
    // TODO: `ErrKindTooManyFiles` is transient and deserves a backoff instead
    // of tearing the listener down, which needs a timer in `Platform`.
    platform_server_shutdown(server, err);
    return;
  }

  const i32 accept_socket = (i32)res;
  assert(accept_socket >= 0);

  server->on_accept(server->platform, server->cb_ctx,
                    completion->action.v.accept.addr, accept_socket);

  platform_server_arm(server);
}

__attribute__((warn_unused_result)) static Error
platform_listen_and_serve_tcp_ipv4(Platform *platform, PlatformServer *server,
                                   void *cb_ctx, Ipv4Addr listen_addr,
                                   const Logger *logger,
                                   AcceptCallback on_accept) {
  assert(platform);
  assert(server);
  assert(logger);
  assert(on_accept);

  *server = (PlatformServer){.platform = platform,
                             .cb_ctx = cb_ctx,
                             .on_accept = on_accept,
                             .listen_socket = -1};
  server->completion.ctx = server;

  {
    const Error err = platform->socket(platform, SocketDomainIpv4,
                                       SocketTypeTcp, &server->listen_socket);
    if (ErrKindNone != err.kind) {
      // No socket, so nothing to hand back and nothing to wait for.
      server->err = err;
      server->done = true;
      return err;
    }
  }
  assert(server->listen_socket >= 0);

  {
    const Error err =
        platform->enable_socket_reuse(platform, server->listen_socket);
    if (ErrKindNone != err.kind) {
      platform_server_shutdown(server, err);
      return err;
    }
  }

  {
    // A port left behind by a previous run is an ordinary answer, not a bug in
    // this process, so it travels back as an `Error`.
    const Error err =
        platform->tcp_bind_ipv4(platform, server->listen_socket, &listen_addr);
    if (ErrKindNone != err.kind) {
      platform_server_shutdown(server, err);
      return err;
    }
  }

  assert(0 != listen_addr.port);
  server->addr = listen_addr;
  log(logger, LogLevelInfo, "listening on port %hu", listen_addr.port);

  {
    const Error err = platform->listen(platform, server->listen_socket, 1024);
    if (ErrKindNone != err.kind) {
      platform_server_shutdown(server, err);
      return err;
    }
  }

  platform_server_arm(server);

  // Arming is the last thing that can fail before the loop takes over, and it
  // fails by taking the listener down, so it is reported like every step
  // before it rather than only through `server->err`.
  if (ErrKindNone != server->err.kind) {
    return server->err;
  }

  return (Error){.kind = ErrKindNone};
}

// ---------- Misc ----------
__attribute__((warn_unused_result)) static bool is_power_of_two(usize value) {
  return (value != 0) && ((value & (value - 1)) == 0);
}

__attribute__((warn_unused_result)) static usize
usize_round_up_multiple_of(usize n, usize multiple) {
  assert(multiple != 0);
  assert(is_power_of_two(multiple));

  usize res = 0;
  assert(!__builtin_add_overflow(n, multiple - 1, &res));
  res &= ~(multiple - 1);

  assert(0 == (res & (multiple - 1)));
  assert(res >= n);
  assert(res - n < multiple);
  return res;
}

__attribute__((warn_unused_result)) static usize next_power_of_two(usize val) {
  if (0 == val) {
    return 1;
  }

  val -= 1;
  val |= val >> 1;
  val |= val >> 2;
  val |= val >> 4;
  val |= val >> 8;
  val |= val >> 16;
  val |= val >> 32;
  val += 1;

  assert(0 != val);
  assert(is_power_of_two(val));

  return val;
}

__attribute__((warn_unused_result)) static usize ceil_usize(usize numerator,
                                                            usize denominator) {
  assert(denominator);

  return numerator / denominator + (numerator % denominator != 0);
}

__attribute__((warn_unused_result)) static Error
arena_valloc(Platform *platform, usize bytes_count, Arena *res) {
  assert(platform);
  assert(res);

  const usize page_size = platform->get_page_size(platform);
  assert(page_size > 0);

  const usize usable_bytes = usize_round_up_multiple_of(bytes_count, page_size);
  usize os_alloc_size = 0;
  // Guard page.
  assert(!__builtin_add_overflow(usable_bytes, page_size, &os_alloc_size));

  u8 *arena_memory = NULL;
  {
    const Error err = platform->valloc(platform, os_alloc_size, &arena_memory);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(arena_memory);

  assert(
      ErrKindNone ==
      platform->vprotect_none(platform, arena_memory + usable_bytes, page_size)
          .kind);

  // Right-align the arena against the guard page so that *any* write past
  // `arena.end` faults immediately, then round the start down to the
  // strictest alignment `arena_alloc` hands out. Rounding down can only make
  // the arena slightly larger than requested, never smaller.
  const usize max_align = 8;
  usize start = (usize)arena_memory + usable_bytes - bytes_count;
  start -= start % max_align;
  assert(start >= (usize)arena_memory);
  assert(0 == start % max_align);

  *res =
      arena_from_mem((u8 *)start, (usize)arena_memory + usable_bytes - start);
  return (Error){.kind = ErrKindNone};
}

// Parse a run of ASCII digits from the front of `*data`, consuming them.
//
// Rejects a run with no digits at all, one that is not terminated by a
// non-digit, one with a leading zero, and one that overflows a `usize`.
// `*data` is only advanced, and `*res` only written, when the parse succeeds.
__attribute__((warn_unused_result)) static Error ascii_num_parse(Bytes *data,
                                                                 usize *res) {
  assert(data);
  assert(res);

  if (!data->data) {
    return (Error){.kind = ErrKindInvalidData};
  }

  Bytes remaining = *data;
  usize num = 0;
  bool has_leading_zero = false;

  const usize MAX_LEN = 30;

  for (usize consumed = 0; consumed < MAX_LEN; consumed++) {
    u8 current = 0;

    // Unterminated.
    if (ErrKindNone != bytes_first(remaining, &current).kind) {
      return (Error){.kind = ErrKindInvalidData};
    }

    // End.
    if (!char_is_digit_ascii(current)) {
      // No digit at all e.g. `e` or `:spam`: not a number.
      if (0 == consumed) {
        return (Error){.kind = ErrKindInvalidData};
      }

      assert(consumed < MAX_LEN);
      *data = remaining;
      *res = num;
      return (Error){.kind = ErrKindNone};
    }

    if (current == '0' && consumed == 0) {
      has_leading_zero = true;
    }

    // Leading zeroes forbidden except `i0e`.
    if (consumed > 0 && has_leading_zero) {
      return (Error){.kind = ErrKindInvalidData};
    }

    // A run of digits too long for a `usize` is well formed but out of
    // range, which is a different complaint from malformed input.
    const usize digit = current - '0';
    if (__builtin_mul_overflow(num, 10, &num)) {
      return (Error){.kind = ErrKindRange};
    }
    if (__builtin_add_overflow(num, digit, &num)) {
      return (Error){.kind = ErrKindRange};
    }

    bytes_advance(&remaining, 1);
  }

  assert(0 && "unreachable");
}

__attribute__((warn_unused_result)) static usize usize_digits_base_10(usize n) {
  usize digits = 1;
  while (n >= 10) {
    n /= 10;
    digits += 1;
  }

  return digits;
}

// `-ISIZE_MIN` is not representable as an `isize`, so the magnitude is taken
// in `usize`, where it always is. Conversion of a negative value to an
// unsigned type is modular, so subtracting it from zero yields exactly the
// magnitude, `ISIZE_MIN` included.
__attribute__((warn_unused_result)) static usize isize_magnitude(isize n) {
  return n < 0 ? (usize)0 - (usize)n : (usize)n;
}

// Decimal width including the sign, the mirror of `usize_digits_base_10`.
__attribute__((warn_unused_result)) static usize isize_digits_base_10(isize n) {
  return (n < 0 ? 1 : 0) + usize_digits_base_10(isize_magnitude(n));
}

// The digits are written at the *front* of `dst`, so a caller can encode
// straight into its own output buffer instead of copying out of a scratch
// one. Base 10 yields the least significant digit first, hence the up front
// width.
__attribute__((warn_unused_result)) static usize
encode_usize_base_10(usize n, Bytes dst) {
  assert(dst.data);

  const usize digits = usize_digits_base_10(n);
  assert(dst.len >= digits);

  u8 *end = dst.data + digits;

  do {
    assert(end > dst.data);

    const usize digit = n % 10;
    *(--end) = (u8)(digit + '0');

    n /= 10;
  } while (n > 0);

  // The width matched the digits actually written, at the front of `dst`.
  assert(end == dst.data);

  return digits;
}

__attribute__((warn_unused_result)) static usize
encode_isize_base_10(isize n, Bytes dst) {
  assert(dst.data);

  const bool negative = n < 0;
  const usize magnitude = isize_magnitude(n);

  if (!negative) {
    return encode_usize_base_10(magnitude, dst);
  }

  assert(dst.len >= 1);
  dst.data[0] = '-';

  const usize digits =
      encode_usize_base_10(magnitude, bytes_make(dst.data + 1, dst.len - 1));

  return 1 + digits;
}

typedef struct {
  Bytes container;
  usize len;
} BytesBuffer;

__attribute__((warn_unused_result)) static Error
bytes_buffer_make(usize cap, Arena *arena, BytesBuffer *dst) {
  assert(arena);
  assert(dst);

  u8 *alloc = arena_alloc(arena, __alignof__(u8), sizeof(u8), cap);
  if (!alloc) {
    return (Error){.kind = ErrKindOOM};
  }

  dst->container.data = alloc;
  dst->container.len = cap;
  dst->len = 0;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static usize
bytes_buffer_space(BytesBuffer bb) {
  assert(bb.len <= bb.container.len);

  return bb.container.len - bb.len;
}

__attribute__((warn_unused_result)) static Bytes
bytes_buffer_space_bytes(BytesBuffer bb) {
  assert(bb.len <= bb.container.len);

  return (Bytes){
      .data = bb.container.data + bb.len,
      .len = bytes_buffer_space(bb),
  };
}

__attribute__((warn_unused_result)) static Bytes
bytes_buffer_to_bytes(BytesBuffer bb) {
  return bytes_take(bb.container, bb.len);
}

__attribute__((warn_unused_result)) static bool
bytes_buffer_extend_within_cap(BytesBuffer *bb, Bytes s) {
  assert(bb);

  if (!s.data || 0 == s.len) {
    return true;
  }

  if (bytes_buffer_space(*bb) < s.len) {
    return false;
  }

  memcpy(bb->container.data + bb->len, s.data, s.len);
  assert(!__builtin_add_overflow(bb->len, s.len, &bb->len));

  return true;
}

__attribute__((warn_unused_result)) static bool
bytes_buffer_append_usize_within_cap(BytesBuffer *bb, usize n) {
  assert(bb);

  const usize digits = usize_digits_base_10(n);

  if (bytes_buffer_space(*bb) < digits) {
    return false;
  }

  const Bytes bb_dst = {
      .data = bb->container.data + bb->len,
      .len = bytes_buffer_space(*bb),
  };
  assert(digits == encode_usize_base_10(n, bb_dst));

  assert(!__builtin_add_overflow(bb->len, digits, &bb->len));

  return true;
}

typedef struct {
  usize idx;
  bool found;
} Find;

typedef enum {
  FindOptionsIndexAtNeedleStart = 0,
  FindOptionsIndexAfterNeedleEnd = 1,
} FindOptions;

__attribute__((warn_unused_result)) static Find
bytes_find(Bytes haystack, Bytes needle, FindOptions options) {
  if (needle.len > haystack.len) {
    return (Find){0};
  }

  if (0 == needle.len) {
    return (Find){.found = true, .idx = 0};
  }

  assert(haystack.data);
  assert(needle.data);

  for (usize i = 0; i <= haystack.len - needle.len; i++) {
    if (0 == memcmp(&haystack.data[i], needle.data, needle.len)) {
      const usize idx =
          (options & FindOptionsIndexAfterNeedleEnd) ? i + needle.len : i;

      // Sanity check.
      assert(haystack.data[i] == needle.data[0]);
      assert(haystack.data[i + needle.len - 1] == needle.data[needle.len - 1]);

      return (Find){
          .found = true,
          .idx = idx,
      };
    }
  }

  return (Find){0};
}

// Like Go's `strings.Cut`: split around the first `needle`. Calling it again on
// `right` while `found` gives the same pieces as Go's `strings.Split`.
typedef struct {
  Bytes left;
  Bytes right;
  bool found;
} Split;

__attribute__((warn_unused_result)) static Split bytes_split(Bytes haystack,
                                                             Bytes needle) {
  // Go splits per rune on an empty needle. Bytes have no runes, so it is a bug.
  assert(needle.len > 0);
  assert(needle.data);

  const Find find = bytes_find(haystack, needle, FindOptionsIndexAtNeedleStart);

  if (!find.found) {
    return (Split){.left = haystack};
  }

  assert(find.idx + needle.len <= haystack.len);

  const Split res = {
      .left = bytes_take(haystack, find.idx),
      .right.data = haystack.data + find.idx + needle.len,
      .right.len = haystack.len - find.idx - needle.len,
      .found = true,
  };
  assert(res.left.data == haystack.data);
  assert(res.left.len + needle.len + res.right.len == haystack.len);
  assert(res.right.data == res.left.data + res.left.len + needle.len);
  assert(
      bytes_eq(needle, bytes_make(haystack.data + res.left.len, needle.len)));

  return res;
}

__attribute__((warn_unused_result)) static bool bytes_eat(Bytes *haystack,
                                                          Bytes needle) {
  assert(haystack);

  if (!bytes_starts_with(*haystack, needle)) {
    return false;
  }

  bytes_advance(haystack, needle.len);

  return true;
}

__attribute__((warn_unused_result)) static bool
bytes_advance_until(Bytes *haystack, Bytes needle, FindOptions opts) {
  assert(haystack);

  const Find find = bytes_find(*haystack, needle, opts);
  if (!find.found) {
    return false;
  }

  haystack->data += find.idx;
  haystack->len -= find.idx;

  return true;
}

// ----------------- Pool (free list) ------------
// A free chunk holds the link to the next free chunk in its first bytes. An
// acquired chunk is all user data.
typedef struct PoolNode PoolNode;

struct PoolNode {
  PoolNode *next;
};

// Every chunk is aligned to this.
#define POOL_ALIGN 8

typedef struct {
  // NULL when every chunk is acquired.
  PoolNode *head;
  // Distance between two chunks: the user size, rounded up so that a chunk
  // fits a `PoolNode` and the next chunk stays aligned.
  usize chunk_size;

  // Lives in the arena given to `pool_make`.
  u8 *buffer;
  usize buffer_len;
} Pool;

// Is `ptr` the start of a chunk of this pool?
__attribute__((warn_unused_result)) static bool pool_owns(const Pool *pool,
                                                          const void *ptr) {
  assert(pool);
  assert(pool->buffer);
  assert(pool->chunk_size);

  const usize addr = (usize)ptr;
  const usize start = (usize)pool->buffer;
  if (addr < start || addr >= start + pool->buffer_len) {
    return false;
  }

  return 0 == (addr - start) % pool->chunk_size;
}

__attribute__((warn_unused_result)) static Error
pool_make(Pool *pool, Arena *arena, usize chunk_size, usize elems_count) {
  assert(pool);
  assert(arena);
  assert(chunk_size);
  assert(elems_count);

  const usize real_chunk_size = usize_round_up_multiple_of(
      chunk_size < sizeof(PoolNode) ? sizeof(PoolNode) : chunk_size,
      POOL_ALIGN);
  assert(real_chunk_size >= chunk_size);
  assert(real_chunk_size >= sizeof(PoolNode));
  assert(0 == real_chunk_size % __alignof__(PoolNode));

  u8 *const buffer =
      arena_alloc(arena, POOL_ALIGN, real_chunk_size, elems_count);
  if (!buffer) {
    return (Error){.kind = ErrKindOOM};
  }
  assert(0 == (usize)buffer % POOL_ALIGN);

  // Every chunk starts free, in address order.
  for (usize i = 0; i < elems_count; i++) {
    PoolNode *const node = (PoolNode *)(void *)(buffer + i * real_chunk_size);
    node->next = (i + 1 < elems_count)
                     ? (PoolNode *)(void *)(buffer + (i + 1) * real_chunk_size)
                     : NULL;
  }

  *pool = (Pool){
      .head = (PoolNode *)(void *)buffer,
      // Cannot overflow since `arena_alloc` did not.
      .buffer_len = real_chunk_size * elems_count,
      .chunk_size = real_chunk_size,
      .buffer = buffer,
  };
  assert(pool_owns(pool, pool->head));

  return (Error){.kind = ErrKindNone};
}

// The chunk is zeroed. Returns NULL when every chunk is acquired.
__attribute__((warn_unused_result)) static void *pool_acquire(Pool *pool) {
  assert(pool);
  assert(pool->buffer);
  assert(pool->chunk_size);

  if (!pool->head) {
    return NULL;
  }

  PoolNode *const res = pool->head;
  assert(pool_owns(pool, res));

  pool->head = res->next;
  // Catches a chunk written to after release.
  assert(!pool->head || pool_owns(pool, pool->head));

  memset(res, 0, pool->chunk_size);

  return res;
}

// `ptr` must have come from `pool_acquire` on this pool, and not be used after.
static void pool_release(Pool *pool, void *ptr) {
  assert(pool);
  assert(pool->buffer);
  assert(pool->chunk_size);
  assert(ptr);
  assert(pool_owns(pool, ptr));
  // Cheap partial check against a double release.
  assert(ptr != pool->head);

  PoolNode *const node = (PoolNode *)ptr;

  node->next = pool->head;
  pool->head = node;
}

typedef enum {
  TlvWireTypeVarint = 0,
  TlvWireTypeI64 = 1,
  TlvWireTypeLen = 2,
  TlvWireTypeSGroup = 3,
  TlvWireTypeEGroup = 4,
  TlvWireTypeI32 = 5,
} TlvWireType;

typedef struct {
  u32 field_num;
  TlvWireType wire_type;
  // Borrows from the input. For a varint, the encoded bytes. For a LEN, the
  // bytes after the length.
  Bytes value;
} Tlv;

// Protobuf varint: 7 bits per byte, low group first. The high bit is set when
// more bytes follow. `dst` and `advanced` are only written on success.
__attribute__((warn_unused_result)) static Error
varint_read(Bytes src, u64 *dst, usize *advanced) {
  assert(dst);
  assert(advanced);

  // A u64 is at most 10 groups of 7 bits.
  const usize max_bytes = 10;
  u64 res = 0;
  for (usize i = 0; i < min(src.len, max_bytes); i++) {
    assert(src.data);
    const u8 byte = src.data[i];
    const u64 group = byte & 0x7f;

    // The last byte only holds the top bit of a u64.
    if (max_bytes - 1 == i && group > 1) {
      return (Error){.kind = ErrKindInvalidData};
    }
    res |= group << (7 * i);

    if (0 == (byte & 0x80)) {
      *dst = res;
      *advanced = i + 1;
      return (Error){0};
    }
  }

  // Cut short, or too long.
  return (Error){.kind = ErrKindInvalidData};
}

// One protobuf record: a tag, then a value whose size depends on the wire type.
// See https://protobuf.dev/programming-guides/encoding/. `dst` and
// `dst_advanced` are only written on success. Empty input is an error: the
// caller stops when nothing is left.
__attribute__((warn_unused_result)) static Error tlv_read(Bytes src, Tlv *dst,
                                                          usize *dst_advanced) {
  assert(dst);
  assert(dst_advanced);

  Bytes remaining = src;

  u64 tag = 0;
  usize advanced = 0;
  Error err = varint_read(remaining, &tag, &advanced);
  if (ErrKindNone != err.kind) {
    return err;
  }
  bytes_advance(&remaining, advanced);

  // Field numbers are 29 bits and 0 is reserved.
  if (tag > UINT32_MAX) {
    return (Error){.kind = ErrKindInvalidData};
  }
  const u32 field_num = (u32)(tag >> 3);
  if (0 == field_num) {
    return (Error){.kind = ErrKindInvalidData};
  }

  const u8 wire_type = tag & 7;
  Bytes value = {0};
  switch (wire_type) {
  case TlvWireTypeVarint: {
    u64 varint = 0;
    err = varint_read(remaining, &varint, &advanced);
    if (ErrKindNone != err.kind) {
      return err;
    }
    value = bytes_take(remaining, advanced);
    break;
  }

  case TlvWireTypeI64:
    if (remaining.len < sizeof(u64)) {
      return (Error){.kind = ErrKindInvalidData};
    }
    value = bytes_take(remaining, sizeof(u64));
    break;

  case TlvWireTypeLen: {
    u64 length = 0;
    err = varint_read(remaining, &length, &advanced);
    if (ErrKindNone != err.kind) {
      return err;
    }
    bytes_advance(&remaining, advanced);

    if (length > remaining.len) {
      return (Error){.kind = ErrKindInvalidData};
    }
    value = bytes_take(remaining, (usize)length);
    break;
  }

  case TlvWireTypeSGroup:
  case TlvWireTypeEGroup:
    return (Error){.kind = ErrKindUnsupported};

  case TlvWireTypeI32:
    if (remaining.len < sizeof(u32)) {
      return (Error){.kind = ErrKindInvalidData};
    }
    value = bytes_take(remaining, sizeof(u32));
    break;

  default:
    return (Error){.kind = ErrKindInvalidData};
  }

  bytes_advance(&remaining, value.len);
  assert(remaining.data > src.data);
  assert(remaining.len < src.len);

  *dst = (Tlv){
      .field_num = field_num,
      .wire_type = (TlvWireType)wire_type,
      .value = value,
  };
  *dst_advanced = src.len - remaining.len;

  return (Error){0};
}
