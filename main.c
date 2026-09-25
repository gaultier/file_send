#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// The ARMv8 SHA-256 extension. Only the AArch64 spelling is implemented; every
// other target falls back to the scalar block function below, which stays the
// reference the vector one is checked against.
#if defined(__aarch64__)
#include <arm_neon.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/auxv.h>
// glibc defines this in `bits/hwcap.h`, musl does not; it is ABI, not a header
// detail.
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
#endif
#define SHA256_HAS_NEON 1
#else
#define SHA256_HAS_NEON 0
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;
typedef size_t usize;
typedef ssize_t isize;

static const usize KiB = 1024;
static const usize MiB = 1024 * KiB;

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
  }

  assert(0 && "unreachable");
}

// Renders `err` to stderr, appending the operating system's own description
// when the error carries an `errno`.
static void error_print(const char *context, Error err) {
  assert(context);

  if (0 == err.data) {
    fprintf(stderr, "%s: %s\n", context, error_kind_to_cstr(err.kind));
    return;
  }

  // `strerror_r` and not `strerror`: a thread is spawned per client, and
  // `strerror` hands back a buffer shared by the whole process.
  char os_msg[256] = {0};
  const i32 ret = strerror_r((i32)err.data, os_msg, sizeof(os_msg));

  if (0 != ret) {
    // The description did not fit or the number is not a known `errno`; the
    // number itself is still worth printing.
    fprintf(stderr, "%s: %s (errno %" PRIu64 ")\n", context,
            error_kind_to_cstr(err.kind), err.data);
    return;
  }

  fprintf(stderr, "%s: %s (errno %" PRIu64 ": %s)\n", context,
          error_kind_to_cstr(err.kind), err.data, os_msg);
}

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
// `path_*` helpers rather than baked into them: they are pure string
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

// ---------- Slice_u8 ----------

typedef struct {
  usize len;
  u8 *data;
} Slice_u8;

__attribute__((warn_unused_result)) static bool slice_u8_is_empty(Slice_u8 s) {
  return NULL == s.data || 0 == s.len;
}

__attribute__((warn_unused_result)) static Slice_u8
slice_u8_from_cstr(char *s) {
  return (Slice_u8){.data = (u8 *)s, .len = strlen(s)};
}

__attribute__((warn_unused_result)) static bool
slice_u8_contains_byte(Slice_u8 s, u8 byte) {
  if (slice_u8_is_empty(s)) {
    return false;
  }

  assert(s.data);

  return NULL != memchr(s.data, byte, s.len);
}

__attribute__((warn_unused_result)) static bool
slice_u8_eq_cstr(Slice_u8 s, const char *cstr) {
  if (!cstr) {
    return slice_u8_is_empty(s);
  }

  const usize cstr_len = strlen(cstr);

  if (cstr_len != s.len) {
    return false;
  }

  assert(s.data);
  assert(s.len > 0);
  assert(cstr_len > 0);
  assert(cstr_len == s.len);

  return 0 == memcmp(s.data, cstr, s.len);
}

// Peek at the first byte of `slice`, leaving it in place.
// Returns `ErrInvalidData`, and does not touch `*res`, if there is no first
// byte.
__attribute__((warn_unused_result)) static Error slice_u8_first(Slice_u8 slice,
                                                                u8 *res) {
  assert(res);

  if (!slice.data) {
    return (Error){.kind = ErrKindInvalidData};
  }

  if (slice.len == 0) {
    return (Error){.kind = ErrKindInvalidData};
  }

  *res = slice.data[0];
  return (Error){.kind = ErrKindNone};
}

// Advance past `count` bytes. The caller must have already established that
// they are available, which is why this cannot fail; use `slice_u8_skip` when
// the input may be short. Unlike an `assert(ErrNone == slice_u8_skip(...))`,
// the advance still happens when asserts are compiled out.
static void slice_u8_advance(Slice_u8 *slice, usize count) {
  assert(slice);
  assert(slice->data);
  assert(count <= slice->len);

  slice->len -= count;
  slice->data += count;
}

__attribute__((warn_unused_result)) static Error slice_u8_skip(Slice_u8 *slice,
                                                               usize count) {
  assert(slice);
  if (!slice->data) {
    return (Error){.kind = ErrKindInvalidData};
  }

  if (slice->len < count) {
    return (Error){.kind = ErrKindInvalidData};
  }

  slice_u8_advance(slice, count);
  return (Error){.kind = ErrKindNone};
}

// The caller must have already established that `count` bytes are available:
// silently returning a short slice would turn a malformed length into a
// successful parse of truncated data.
__attribute__((warn_unused_result)) static Slice_u8
slice_u8_take(Slice_u8 input, usize count) {
  assert(count <= input.len);

  return (Slice_u8){.data = input.data, .len = count};
}

__attribute__((warn_unused_result)) static Slice_u8 slice_u8_make(u8 *data,
                                                                  usize len) {
  if (0 != len) {
    assert(data);
  }

  return (Slice_u8){.data = data, .len = len};
}

// Consume the first byte of `*slice` if it is `expected`.
//
// `*slice` is only advanced on a match, which is what makes it usable as a
// speculative `if (ErrNone != consume(...)) { return ...; }` inside a parse
// that rolls back. A byte that simply does not match is reported the same way
// as a missing one: it is the caller that knows whether that is an error.
__attribute__((warn_unused_result)) static Error
slice_u8_consume(Slice_u8 *slice, u8 expected) {
  assert(slice);
  assert(slice->data);

  u8 actual = 0;
  if (ErrKindNone != slice_u8_first(*slice, &actual).kind) {
    return (Error){.kind = ErrKindInvalidData};
  }
  if (actual != expected) {
    return (Error){.kind = ErrKindInvalidData};
  }

  slice_u8_advance(slice, 1);
  return (Error){.kind = ErrKindNone};
}

// The extension of the last component of `path`, dot included, or an empty
// slice when there is none. The result borrows from `path`: nothing is
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
__attribute__((warn_unused_result)) static Slice_u8 path_get_ext(Slice_u8 path,
                                                                 u8 separator) {
  if (!path.data || 0 == path.len) {
    return (Slice_u8){0};
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
      return slice_u8_make(path.data + i - 1, path.len - (i - 1));
    }
  }

  return (Slice_u8){0};
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
path_with_ext(Slice_u8 path, Slice_u8 ext, u8 separator, Slice_u8 *dst,
              Arena *arena) {
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
  const Slice_u8 old_ext = path_get_ext(path, separator);
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

  *dst = slice_u8_make(data, dst_len);

  return (Error){.kind = ErrKindNone};
}

// Matches https://pkg.go.dev/path/filepath#Base.
//
// The result may be "/", "." or "..": it is the last path element, not a
// validated file name, so callers that need one must check it themselves.
__attribute__((warn_unused_result)) static Slice_u8
path_last_component(Slice_u8 path, u8 separator) {
  //  If the path is empty, Base returns ".".
  if (slice_u8_is_empty(path)) {
    return (Slice_u8){.data = (u8 *)".", .len = 1};
  }

  //  Trailing path separators are removed before extracting the last element.
  while (!slice_u8_is_empty(path)) {
    if (separator == path.data[path.len - 1]) {
      path.len -= 1;
    } else {
      break;
    }
  }

  //  If the path consists entirely of separators, Base returns a single
  //  separator. Only `len` was trimmed above, so the first byte of the
  //  caller's path is still there to borrow it from.
  if (slice_u8_is_empty(path)) {
    assert(path.data);
    return (Slice_u8){.data = path.data, .len = 1};
  }

  // Counts down over one-past-the-byte so the whole walk stays in `usize`:
  // `i` is the start of the component when `path.data[i - 1]` is the
  // separator.
  for (usize i = path.len; i > 0; i--) {
    const u8 c = path.data[i - 1];
    if (separator == c) {
      const Slice_u8 res = {.data = path.data + i, .len = path.len - i};
      // The trailing separators are gone, so there is at least one byte left
      // after the last one.
      assert(!slice_u8_is_empty(res));
      assert(!slice_u8_contains_byte(res, separator));

      return res;
    }
  }

  assert(!slice_u8_contains_byte(path, separator));
  return path;
}

// ---------- IO ----------

// Named before the struct body so its own slots, and the platform wrappers
// that fill them, can take the vtable they belong to: a slot that composes
// other slots calls them through `io`, which is what lets one platform
// implement an operation out of its own primitives without the vtable having
// to agree on what those primitives are.
typedef struct IO IO;

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

struct IO {
  Error (*socket)(const IO *io, SocketDomain domain, SocketType type, i32 *fd);
  Error (*listen)(const IO *io, i32 fd, i32 backlog);
  Error (*open)(const IO *io, Slice_u8 path, FileOpenOptions options, i32 *fd);
  Error (*tcp_bind_ipv4)(const IO *io, i32 listen_socket, Ipv4Addr addr);
  Error (*accept)(const IO *io, i32 listen_socket, i32 *dst_accept_socket,
                  Ipv4Addr *dst_accept_addr);
  Error (*thread_create)(const IO *io, ThreadCallback cb, void *data);
  Error (*close)(const IO *io, i32 fd);
  Error (*enable_socket_reuse)(const IO *io, i32 fd);
  Error (*read)(const IO *io, i32 fd, Slice_u8 data, usize *dst_read);
  Error (*write)(const IO *io, i32 fd, Slice_u8 data, usize *dst_written);
  Error (*file_size)(const IO *io, i32 fd, usize *dst_size);
  Error (*map_file)(const IO *io, Slice_u8 path, FileOpenOptions opts,
                    Slice_u8 *dst);
  Error (*write_all_to_file)(const IO *io, Slice_u8 path, Slice_u8 data);
  usize (*get_page_size)(const IO *io);
  Error (*valloc)(const IO *io, usize bytes_count, u8 **res);
  Error (*vprotect_none)(const IO *io, void *ptr, usize size);

  // The implementation's own state, reached by every slot above as
  // `io->ctx`. It is not the caller's: a callback's user data travels
  // separately, because the two have different lifetimes and owners.
  void *ctx;
};

#include "unix.c"

typedef Error (*AcceptCallback)(const IO *io, void *cb_ctx,
                                Ipv4Addr accept_addr,
                                i32 accept_socket);

__attribute__((warn_unused_result)) static Error
io_listen_and_serve_tcp_ipv4(const IO *io, void *cb_ctx,
                             Ipv4Addr listen_addr,
                             AcceptCallback on_accept) {
  assert(io);
  assert(on_accept);

  i32 listen_socket = 0;
  {
    const Error err_socket =
        io->socket(io, SocketDomainIpv4, SocketTypeTcp, &listen_socket);
    if (ErrKindNone != err_socket.kind) {
      return err_socket;
    }
    puts("opened socket");
  }
  {
    const Error err_reuse = io->enable_socket_reuse(io, listen_socket);

    if (ErrKindNone != err_reuse.kind) {
      (void)io->close(io, listen_socket);
      return err_reuse;
    }
  }

  {
    // A port left behind by a previous run is an ordinary answer, not a bug
    // in this process, so it travels back as an `Error`.
    const Error err_bind = io->tcp_bind_ipv4(io, listen_socket, listen_addr);
    if (ErrKindNone != err_bind.kind) {
      (void)io->close(io, listen_socket);
      return err_bind;
    }
    puts("socket bound");
  }

  {
    const Error err_listen = io->listen(io, listen_socket, 1024);
    if (ErrKindNone != err_listen.kind) {
      (void)io->close(io, listen_socket);
      return err_listen;
    }
    puts("socket listening");
  }

  for (;;) {
    i32 accept_socket = 0;
    Ipv4Addr accept_addr = {0};

    const Error err_accept =
        io->accept(io, listen_socket, &accept_socket, &accept_addr);

    // The peer is allowed to vanish between the handshake and the `accept`.
    // That is one dead connection, not a dead server.
    if (ErrKindConnReset == err_accept.kind) {
      continue;
    }

    if (ErrKindNone != err_accept.kind) {
      // TODO: `ErrTooManyFiles` is transient and deserves a backoff instead
      // of tearing the listener down, which needs a timer in `IO`.
      (void)io->close(io, listen_socket);
      return err_accept;
    }

    // `accept_socket` belongs to the callback from here on, including
    // closing it when the callback itself fails.
    const Error err_on_accept = on_accept(io, cb_ctx, accept_addr, accept_socket);
    if (ErrKindNone != err_on_accept.kind) {
      error_print("failed to handle connection", err_on_accept);
    }
  }
}

// ---------- Misc ----------
__attribute__((warn_unused_result)) static bool is_power_of_two(usize value) {
  return (value != 0) && ((value & (value - 1)) == 0);
}

// `multiple` must be a power of two, which every page size is.
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

// On success `*res` is the arena; on failure it is left alone and the reason
// `mmap` gave is passed through.
__attribute__((warn_unused_result)) static Error
arena_valloc(const IO *io, usize bytes_count, Arena *res) {
  assert(io);
  assert(res);

  const usize page_size = io->get_page_size(io);
  assert(page_size > 0);

  const usize usable_bytes = usize_round_up_multiple_of(bytes_count, page_size);
  usize os_alloc_size = 0;
  // Guard page.
  assert(!__builtin_add_overflow(usable_bytes, page_size, &os_alloc_size));

  u8 *arena_memory = NULL;
  {
    const Error err = io->valloc(io, os_alloc_size, &arena_memory);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(arena_memory);

  assert(ErrKindNone ==
         io->vprotect_none(io, arena_memory + usable_bytes, page_size)
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
__attribute__((warn_unused_result)) static Error ascii_num_parse(Slice_u8 *data,
                                                                 usize *res) {
  assert(data);
  assert(res);

  if (!data->data) {
    return (Error){.kind = ErrKindInvalidData};
  }

  Slice_u8 remaining = *data;
  usize num = 0;
  bool has_leading_zero = false;

  const usize MAX_LEN = 30;

  for (usize consumed = 0; consumed < MAX_LEN; consumed++) {
    u8 current = 0;

    // Unterminated.
    if (ErrKindNone != slice_u8_first(remaining, &current).kind) {
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

    slice_u8_advance(&remaining, 1);
  }

  assert(0 && "unreachable");
}

// ---------- Bencode ----------
typedef enum {
  BencodeKindInteger,
  BencodeKindString,
  BencodeKindList,
  BencodeKindDict,
} BencodeKind;

typedef struct BencodeValue BencodeValue;

typedef struct {
  bool is_list;
  usize children_start;
} BencodeContainer;

typedef struct {
  usize len;
  BencodeValue *data;
} BencodeList;

struct BencodeValue {
  BencodeKind kind;
  union {
    isize num;        // Integer
    Slice_u8 s;       // String
    BencodeList list; // List or Dict (stored as contiguous key-value pairs)
  } v;
};

// `i123e`
// `i-123e`
//
// `*input` is only advanced, and `*res` only written, when the parse
// succeeds.
__attribute__((warn_unused_result)) static Error
bencode_parse_num(Slice_u8 *input, BencodeValue *res) {
  assert(input);
  assert(input->data);
  assert(res);

  Slice_u8 remaining = *input;

  if (ErrKindNone != slice_u8_consume(&remaining, 'i').kind) {
    return (Error){.kind = ErrKindInvalidData};
  }

  const bool negative_sign =
      ErrKindNone == slice_u8_consume(&remaining, '-').kind;

  // Also rejects `ie` and `i-e`: a number needs at least one digit. A run of
  // digits too wide for a `usize` comes back as `ErrRange`, which is passed
  // through rather than flattened: the input is well formed, just too big.
  usize magnitude = 0;
  {
    const Error err = ascii_num_parse(&remaining, &magnitude);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  // `i-0e` is invalid bencode.
  if (negative_sign && 0 == magnitude) {
    return (Error){.kind = ErrKindInvalidData};
  }

  isize num = 0;
  {
    const Error err = isize_from_usize(magnitude, negative_sign, &num);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  if (ErrKindNone != slice_u8_consume(&remaining, 'e').kind) {
    return (Error){.kind = ErrKindInvalidData};
  }

  *input = remaining;
  *res = (BencodeValue){.kind = BencodeKindInteger, .v.num = num};
  return (Error){.kind = ErrKindNone};
}

// `4:spam`
//
// The string is not copied: it points into `*input`.
// `*input` is only advanced, and `*res` only written, when the parse
// succeeds.
__attribute__((warn_unused_result)) static Error
bencode_parse_string(Slice_u8 *input, BencodeValue *res) {
  assert(input);
  assert(input->data);
  assert(res);

  Slice_u8 remaining = *input;

  // Also rejects a leading `:` or any non-digit: a length needs a digit.
  usize len = 0;
  {
    const Error err = ascii_num_parse(&remaining, &len);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  if (ErrKindNone != slice_u8_consume(&remaining, ':').kind) {
    return (Error){.kind = ErrKindInvalidData};
  }

  // Truncated body.
  if (len > remaining.len) {
    return (Error){.kind = ErrKindInvalidData};
  }

  const Slice_u8 s = slice_u8_take(remaining, len);
  slice_u8_advance(&remaining, len);

  *input = remaining;
  *res = (BencodeValue){.kind = BencodeKindString, .v.s = s};

  assert(res->v.s.len == len);
  if (0 != res->v.s.len) {
    assert(res->v.s.data);
  }
  return (Error){.kind = ErrKindNone};
}

// Compare two byte strings lexicographically: the first differing byte
// decides, and when one is a prefix of the other, the shorter one sorts
// first. Returns <0, 0, or >0, like `memcmp`.
//
// Bytes are compared as unsigned values, so `0x80` sorts after `0x7f`. This
// is a total order over arbitrary bytes, embedded zeroes included, and it is
// the ordering bencode requires of dict keys.
__attribute__((warn_unused_result)) static i32
bytes_cmp(const u8 *a, usize a_len, const u8 *b, usize b_len) {
  if (0 != a_len) {
    assert(a);
  }
  if (0 != b_len) {
    assert(b);
  }

  // Not `memcmp`: it is undefined to hand it a NULL pointer even for a length
  // of zero, and an empty byte string is legal here.
  const usize len = a_len < b_len ? a_len : b_len;
  for (usize i = 0; i < len; i++) {
    if (a[i] != b[i]) {
      return a[i] < b[i] ? -1 : 1;
    }
  }

  // Equal up to the shorter length: the prefix sorts first.
  if (a_len == b_len) {
    return 0;
  }
  return a_len < b_len ? -1 : 1;
}

__attribute__((warn_unused_result)) static Error
bencode_validate_dict(BencodeList list) {
  if (0 != list.len) {
    assert(NULL != list.data);
  }

  // Mismatched key-value pairs?
  if (list.len % 2 != 0) {
    return (Error){.kind = ErrKindInvalidData};
  }

  for (usize i = 0; i < list.len; i += 2) {
    const BencodeValue key = list.data[i];

    if (key.kind != BencodeKindString) {
      return (Error){.kind = ErrKindInvalidData};
    }

    if (i > 1) {
      const BencodeValue previous = list.data[i - 2];

      if (bytes_cmp(previous.v.s.data, previous.v.s.len, key.v.s.data,
                    key.v.s.len) >= 0) {
        return (Error){.kind = ErrKindInvalidData};
      }
    }
  }

  return (Error){.kind = ErrKindNone};
}

#define BENCODE_MAX_DEPTH 128

// Parse one complete bencode value, with all of its children, into `*res`.
//
// `*input` and `*arena` are only advanced when the parse succeeds: rolling
// back a bump allocator is just restoring its start pointer, so a failed
// parse leaves the caller with neither consumed input nor consumed memory.
//
// `scratch` is taken by value and is not consumed by the call.
__attribute__((warn_unused_result)) static Error
bencode_parse(Slice_u8 *input, Arena *arena, Arena scratch, BencodeValue *res) {
  assert(input);
  assert(arena);
  assert(arena->start <= arena->end);
  if (0 != input->len) {
    assert(input->data);
  }
  assert(res);

  // Nothing to do?
  if (0 == input->len) {
    return (Error){.kind = ErrKindInvalidData};
  }

  Slice_u8 remaining = *input;
  Arena arena_local = *arena;

  // At most, there are as many bencode values as `input bytes/2+1` since each
  // value takes at least 2 bytes.
  const usize values_cap = remaining.len / 2 + 1;
  BencodeValue *values = arena_alloc(&scratch, __alignof__(BencodeValue),
                                     sizeof(BencodeValue), values_cap);
  // OOM?
  if (!values) {
    return (Error){.kind = ErrKindOOM};
  }
  usize values_count = 0;

  BencodeContainer containers[BENCODE_MAX_DEPTH] = {0};
  usize containers_count = 0;

  const usize MAX_LEN = remaining.len;

  for (usize _i = 0; _i < MAX_LEN; _i++) {
    u8 current = 0;
    if (ErrKindNone != slice_u8_first(remaining, &current).kind) {
      return (Error){.kind = ErrKindInvalidData};
    }

    switch (current) {
    case 'i':
      // Parsed straight into its final slot: no intermediate copy.
      assert(values_count < values_cap);
      {
        const Error err = bencode_parse_num(&remaining, &values[values_count]);
        if (ErrKindNone != err.kind) {
          return err;
        }
      }
      values_count++;
      break;

    case 'l':
    case 'd':
      if (containers_count >= BENCODE_MAX_DEPTH) {
        return (Error){.kind = ErrKindInvalidData};
      }
      slice_u8_advance(&remaining, 1);

      containers[containers_count].is_list = current == 'l';
      containers[containers_count].children_start = values_count;
      containers_count++;

      // The next loop iteration will parse the items.
      continue;

    case 'e': {
      if (0 == containers_count) {
        // Stray `e`, reject.
        return (Error){.kind = ErrKindInvalidData};
      }

      slice_u8_advance(&remaining, 1);

      // Time to pop `containers`.
      const BencodeContainer container = containers[containers_count - 1];
      containers[containers_count - 1] = (BencodeContainer){0};
      containers_count--;

      const usize children_len = values_count - container.children_start;

      // Should always be key-value pairs.
      if (!container.is_list && children_len % 2 != 0) {
        return (Error){.kind = ErrKindInvalidData};
      }

      // Now record the value for this container.
      BencodeValue value = {.kind = container.is_list ? BencodeKindList
                                                      : BencodeKindDict};
      // If there are any children, we need to allocate (right-sized) space
      // for them.
      if (children_len > 0) {
        BencodeValue *children =
            arena_alloc(&arena_local, __alignof__(BencodeValue),
                        sizeof(BencodeValue), children_len);
        // OOM?
        if (!children) {
          return (Error){.kind = ErrKindOOM};
        }

        // Copy the items from `values` to the new right-sized allocation.
        value.v.list.data = memcpy(children, values + container.children_start,
                                   children_len * sizeof(BencodeValue));
        value.v.list.len = children_len;

        if (BencodeKindDict == value.kind) {
          const Error err = bencode_validate_dict(value.v.list);
          if (ErrKindNone != err.kind) {
            return err;
          }
        }
      }

      // Pop all the items for this container, at once. The popped slots are
      // scrubbed so that a stale child cannot be mistaken for a live value.
      memset(values + container.children_start, 0,
             children_len * sizeof(BencodeValue));
      values_count = container.children_start;
      assert(values_count < values_cap);

      // Do not forget to record this new bencode value!
      values[values_count++] = value;
    } break;

    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      // Parsed straight into its final slot: no intermediate copy.
      assert(values_count < values_cap);
      {
        const Error err =
            bencode_parse_string(&remaining, &values[values_count]);
        if (ErrKindNone != err.kind) {
          return err;
        }
      }
      values_count++;
      break;

      // Unknown character.
    default:
      return (Error){.kind = ErrKindInvalidData};
    }

    // We just finished to correctly parse a value.
    assert(values_count <= values_cap);

    // No containers meaning: nothing is currently open.
    // So, we are at the root, which we need to return to the caller,
    // because `root != values[0]` in the general case.
    if (0 == containers_count) {
      assert(1 == values_count);

      // The single success exit: everything is committed here, at once.
      *input = remaining;
      *arena = arena_local;
      *res = values[0];
      return (Error){.kind = ErrKindNone};
    }
  }

  return (Error){.kind = ErrKindInvalidData};
}

static void bencode_print_indent(usize indent) {
  for (usize i = 0; i < indent; i++) {
    printf(" ");
  }
}

// Print `v` in a JSON-ish form.
//
// The caller owns the cursor: it has already written whatever precedes the
// value on the current line (the leading indentation, or a `key: ` prefix),
// so this never indents the value itself. `indent` is the column the *line*
// the value starts on begins at, which is what the children and the closing
// bracket are aligned against. Nothing is written after the value either: a
// trailing newline is the caller's to add.
__attribute__((unused)) static void bencode_print(BencodeValue v,
                                                  usize indent) {
  switch (v.kind) {
  case BencodeKindInteger:
    printf("%zd", v.v.num);
    break;

  case BencodeKindString:
    // Bencode strings are arbitrary bytes, and `%s` would stop at the first
    // NUL however large a precision it is given, so the bytes go out through
    // `fwrite` instead.
    printf("\"");
    assert(v.v.s.len == fwrite(v.v.s.data, 1, v.v.s.len, stdout));
    printf("\"");
    break;

  case BencodeKindDict:
    // An empty container has no children to lay out, so it stays on one line.
    if (0 == v.v.list.len) {
      printf("{}");
      break;
    }

    printf("{\n");
    for (usize i = 0; i < v.v.list.len; i += 2) {
      if (i > 0) {
        printf(",\n");
      }
      bencode_print_indent(indent + 2);
      bencode_print(v.v.list.data[i], indent + 2);
      printf(": ");
      // The value is indented against the start of the key's line, not
      // against the column it happens to start at, so a nested container
      // closes underneath its key.
      bencode_print(v.v.list.data[i + 1], indent + 2);
    }
    printf("\n");
    bencode_print_indent(indent);
    printf("}");
    break;

  case BencodeKindList:
    if (0 == v.v.list.len) {
      printf("[]");
      break;
    }

    printf("[\n");
    for (usize i = 0; i < v.v.list.len; i++) {
      if (i > 0) {
        printf(",\n");
      }
      bencode_print_indent(indent + 2);
      bencode_print(v.v.list.data[i], indent + 2);
    }
    printf("\n");
    bencode_print_indent(indent);
    printf("]");
    break;

  default:
    assert(0 && "unreachable");
  }
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

// SHA-256 as specified in FIPS 180-4, in plain C: no hardware intrinsics, so
// the same code runs on every target and stays diffable against the spec.
//
// The usual three step API: one context, any number of `Update` calls, one
// `Final`. None of the three can fail, so none of them returns anything:
//
//   SHA256_CTX ctx = {0};
//   SHA256_Init(&ctx);
//   SHA256_Update(&ctx, data);
//   u8 digest[SHA256_DIGEST_LENGTH] = {0};
//   SHA256_Final(&ctx, digest);

#define SHA256_CBLOCK 64

typedef struct {
  // Chaining state: the eight working variables between blocks.
  u32 h[8];
  // Total number of message bytes fed in so far. Only the low 61 bits can
  // matter: the padding stores the length in bits, in 64 bits.
  u64 len;
  // Bytes of a not-yet-complete block held back from a previous `Update`.
  u32 partial_len;
  u8 partial[SHA256_CBLOCK];
} Sha256Ctx;

// First 32 bits of the fractional parts of the cube roots of the first 64
// primes (FIPS 180-4, 4.2.2).
static const u32 sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// `count` must be in 1..31: a rotation by 0 would shift a `u32` by 32, which
// is undefined behaviour.
__attribute__((warn_unused_result)) static u32 u32_rotate_right(u32 x,
                                                                u32 count) {
  assert(count >= 1);
  assert(count <= 31);

  return (x >> count) | (x << (32 - count));
}

__attribute__((warn_unused_result)) static u32
u32_from_bytes_be(const u8 *data) {
  assert(data);

  return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) |
         (u32)data[3];
}

static void u32_to_bytes_be(u32 value, u8 *res) {
  assert(res);

  res[0] = (u8)(value >> 24);
  res[1] = (u8)(value >> 16);
  res[2] = (u8)(value >> 8);
  res[3] = (u8)value;
}

// Mix one full 64 byte block into the chaining state (FIPS 180-4, 6.2.2).
static void sha256_compress(u32 h[8], const u8 block[SHA256_CBLOCK]) {
  assert(h);
  assert(block);

  u32 w[64] = {0};
  for (usize i = 0; i < 16; i++) {
    w[i] = u32_from_bytes_be(block + i * 4);
  }
  for (usize i = 16; i < 64; i++) {
    const u32 s0 = u32_rotate_right(w[i - 15], 7) ^
                   u32_rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const u32 s1 = u32_rotate_right(w[i - 2], 17) ^
                   u32_rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  u32 a = h[0], b = h[1], c = h[2], d = h[3];
  u32 e = h[4], f = h[5], g = h[6], hh = h[7];

  for (usize i = 0; i < 64; i++) {
    const u32 s1 = u32_rotate_right(e, 6) ^ u32_rotate_right(e, 11) ^
                   u32_rotate_right(e, 25);
    const u32 ch = (e & f) ^ (~e & g);
    const u32 t1 = hh + s1 + ch + sha256_k[i] + w[i];
    const u32 s0 = u32_rotate_right(a, 2) ^ u32_rotate_right(a, 13) ^
                   u32_rotate_right(a, 22);
    const u32 maj = (a & b) ^ (a & c) ^ (b & c);
    const u32 t2 = s0 + maj;

    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

// First 32 bits of the fractional parts of the square roots of the first 8
// primes (FIPS 180-4, 5.3.3).
#if SHA256_HAS_NEON

// `+crypto` is a function level target, not a build flag, because `-march`
// does not necessarily enable it: on this toolchain plain `-march=native`
// leaves `__ARM_FEATURE_SHA2` undefined and the intrinsics below refuse to
// compile without it.
__attribute__((target("+crypto"))) static void
sha256_compress_blocks_neon(u32 h[8], const u8 *blocks, usize blocks_count) {
  assert(h);
  if (0 != blocks_count) {
    assert(blocks);
  }

  // Unlike the x86 extension, the ARM one keeps the working variables in
  // their natural order, so the state needs no shuffling on the way in or
  // out.
  uint32x4_t state0 = vld1q_u32(&h[0]); // a b c d
  uint32x4_t state1 = vld1q_u32(&h[4]); // e f g h

  // The chaining state stays in registers for the whole run, so a multi block
  // hash reads and writes `h` once instead of once per block.
  for (usize b = 0; b < blocks_count; b++) {
    const u8 *const block = blocks + b * SHA256_CBLOCK;

    const uint32x4_t state0_in = state0;
    const uint32x4_t state1_in = state1;

    // The message is big endian, the vector unit little endian.
    uint32x4_t msg[4] = {0};
    for (usize i = 0; i < 4; i++) {
      msg[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + i * 16)));
    }

    // Each instruction pair consumes four rounds at once, so 64 rounds are 16
    // iterations rather than the scalar version's 64.
    uint32x4_t wk = vaddq_u32(msg[0], vld1q_u32(&sha256_k[0]));

    // Unrolled by four, which measured fastest. The win is front end, not
    // register pressure: the subscripts stay runtime values and `msg` still
    // round trips through the stack, but that traffic is off the critical
    // path and hides in the shadow of the `sha256h` chain. Both ways of
    // removing it
    // -- unrolling all sixteen groups, and hand writing them against four
    // named vector variables the way OpenSSL's asm does -- measured slower
    // here, because the larger footprint costs more than the traffic did.
#pragma clang loop unroll_count(4)
    for (usize i = 0; i < 16; i++) {
      const usize j = i & 3;

      // The next group's `w + k` reads a message word that this group's
      // schedule is about to overwrite, so compute it first.
      uint32x4_t wk_next = wk;
      if (i < 15) {
        wk_next =
            vaddq_u32(msg[(j + 1) & 3], vld1q_u32(&sha256_k[4 * (i + 1)]));
      }

      // The last four groups consume the schedule without extending it: there
      // is no `w[64]`.
      if (i < 12) {
        msg[j] = vsha256su0q_u32(msg[j], msg[(j + 1) & 3]);
      }

      // `SHA256H` needs the old `a b c d`, which `SHA256H` itself overwrites.
      const uint32x4_t abcd = state0;
      state0 = vsha256hq_u32(state0, state1, wk);
      state1 = vsha256h2q_u32(state1, abcd, wk);

      if (i < 12) {
        msg[j] = vsha256su1q_u32(msg[j], msg[(j + 2) & 3], msg[(j + 3) & 3]);
      }

      wk = wk_next;
    }

    state0 = vaddq_u32(state0, state0_in);
    state1 = vaddq_u32(state1, state1_in);
  }

  vst1q_u32(&h[0], state0);
  vst1q_u32(&h[4], state1);
}

// The extension is optional even on AArch64, so ask rather than assume.
// Cached because this sits on the hot path of every hash and the query is not
// free. Racing callers compute the same answer, so the race is benign.
__attribute__((warn_unused_result)) static bool sha256_neon_supported(void) {
  static i32 cached = -1;

  if (cached < 0) {
#if defined(__APPLE__)
    i32 present = 0;
    usize present_size = sizeof(present);
    const bool ok = 0 == sysctlbyname("hw.optional.arm.FEAT_SHA256", &present,
                                      &present_size, NULL, 0);
    cached = (ok && 0 != present) ? 1 : 0;
#elif defined(__linux__)
    // The kernel publishes AArch64 feature bits in the ELF auxiliary vector;
    // `getauxval` reads the copy the loader already saved, so it is not a
    // syscall. An unknown type yields 0, which falls back to the scalar path.
    const unsigned long hwcap = getauxval(AT_HWCAP);
    cached = (0 != (hwcap & HWCAP_SHA2)) ? 1 : 0;
#else
    cached = 0;
#endif
  }

  return 1 == cached;
}

#endif

// Compress `blocks_count` consecutive blocks. The implementation is chosen
// once here rather than per block, so the check stays out of the inner loop.
static void sha256_compress_blocks(u32 h[8], const u8 *blocks,
                                   usize blocks_count) {
  assert(h);
  if (0 != blocks_count) {
    assert(blocks);
  }

#if SHA256_HAS_NEON
  if (sha256_neon_supported()) {
    sha256_compress_blocks_neon(h, blocks, blocks_count);
    return;
  }
#endif

  for (usize i = 0; i < blocks_count; i++) {
    sha256_compress(h, blocks + i * SHA256_CBLOCK);
  }
}

static void sha256_init(Sha256Ctx *ctx) {
  assert(ctx);

  *ctx = (Sha256Ctx){
      .h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
            0x9b05688c, 0x1f83d9ab, 0x5be0cd19},
  };
}

static void sha256_update(Sha256Ctx *ctx, const u8 *data, usize len) {
  assert(ctx);
  if (0 != len) {
    assert(data);
  }
  assert(ctx->partial_len < SHA256_CBLOCK);

  const u8 *remaining = data;
  ctx->len += len;

  // Top up a partial block from a previous call first. It is only compressed
  // once it is full: a short `Update` must leave it partial, not hash it.
  if (ctx->partial_len > 0) {
    const usize wanted = SHA256_CBLOCK - ctx->partial_len;
    const usize taken = len < wanted ? len : wanted;

    memcpy(ctx->partial + ctx->partial_len, remaining, taken);
    ctx->partial_len += (u32)taken;
    remaining += taken;
    len -= taken;

    if (ctx->partial_len < SHA256_CBLOCK) {
      assert(0 == len);
      return;
    }

    sha256_compress_blocks(ctx->h, ctx->partial, 1);
    ctx->partial_len = 0;
  }

  {
    const usize blocks_count = len / SHA256_CBLOCK;
    sha256_compress_blocks(ctx->h, remaining, blocks_count);
    remaining += blocks_count * SHA256_CBLOCK;
    len -= blocks_count * SHA256_CBLOCK;
  }

  if (len > 0) {
    memcpy(ctx->partial, remaining, len);
  }
  ctx->partial_len = (u32)len;
}

#define SHA256_DIGEST_LENGTH 32

// `*ctx` is left zeroed, so it cannot be used again without another
// `SHA256_Init`, and the chaining state of the message does not linger.
static void sha256_final(Sha256Ctx *ctx, u8 res[SHA256_DIGEST_LENGTH]) {
  assert(ctx);
  assert(res);

  // The length goes in the last 8 bytes of the last block, so pad with a
  // single one bit then zeroes up to the next offset 56 (mod 64).
  const u64 len_bits = ctx->len * 8;
  const usize len_mod = (usize)(ctx->len % SHA256_CBLOCK);
  const usize padding_len =
      len_mod < 56 ? 56 - len_mod : 56 + SHA256_CBLOCK - len_mod;

  u8 padding[SHA256_CBLOCK] = {0x80};
  sha256_update(ctx, padding, padding_len);

  u8 len_bytes[8] = {0};
  for (usize i = 0; i < 8; i++) {
    len_bytes[i] = (u8)(len_bits >> (56 - 8 * i));
  }
  sha256_update(ctx, len_bytes, sizeof(len_bytes));
  assert(0 == ctx->partial_len);

  for (usize i = 0; i < 8; i++) {
    u32_to_bytes_be(ctx->h[i], res + i * 4);
  }

  *ctx = (Sha256Ctx){0};
}

static void sha256_digest(Slice_u8 data, u8 dst[SHA256_DIGEST_LENGTH]) {
  Sha256Ctx sha = {0};
  sha256_init(&sha);
  sha256_update(&sha, data.data, data.len);
  sha256_final(&sha, dst);
}

// Debugging aid, so kept even when nothing calls it.
__attribute__((unused)) static void
sha256_print_hex(const u8 digest[SHA256_DIGEST_LENGTH]) {
  const u8 lut[] = "0123456789abcdef";

  for (usize i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    const u8 byte = digest[i];
    const u8 c1 = lut[byte & 15];
    const u8 c2 = lut[byte >> 4];
    printf("%c%c", c2, c1);
  }
}

static void sha256_digest_pair(const u8 left[SHA256_DIGEST_LENGTH],
                               const u8 right[SHA256_DIGEST_LENGTH],
                               u8 dst[SHA256_DIGEST_LENGTH]) {
  Sha256Ctx sha = {0};
  sha256_init(&sha);
  sha256_update(&sha, left, SHA256_DIGEST_LENGTH);
  sha256_update(&sha, right, SHA256_DIGEST_LENGTH);
  sha256_final(&sha, dst);
}

// ---------- Torrent ----------

static const usize TORRENT_BLOCK_SIZE = 16 * KiB;

// Arbitrary, only needs to be a byte pattern the tree cannot produce on
// its own. See the post condition in `torrent_build_merkle_tree`.
#define MERKLE_PIECE_POISON 0xAA

typedef struct {
  u8 digest[SHA256_DIGEST_LENGTH];
} PieceHash;

// Everything about a file's merkle tree that does not vary from node to node.
// Built once by `torrent_build_merkle_tree` and passed down by pointer: the
// recursion cannot then disagree with itself about where the piece layer
// sits, and the divisions and `ctz`s happen once rather than once per node.
typedef struct {
  const Slice_u8 data;
  // Depth of the leaf layer, counting down from `0` at the root. Equivalently
  // `log2` of the block count rounded up to a power of two.
  const usize max_depth;
  // Depth at which one subtree spans exactly one piece. Only meaningful when
  // `has_piece_layer`.
  const usize piece_depth;
  // Pieces that hold file data, not counting the padding the tree is rounded
  // up with.
  const usize pieces_count;
  // BEP 52 gives no piece layer to a file that fits inside a single piece.
  const bool has_piece_layer;
  // Capacity `pieces_count`, filled in as the recursion crosses
  // `piece_depth`.
  PieceHash *const piece_hashes;
} MerkleTree;

__attribute__((warn_unused_result)) static MerkleTree
torrent_merkle_tree_make(Slice_u8 data, usize piece_length_in_bytes,
                         PieceHash *piece_hashes) {
  assert(data.data);
  assert(data.len > 0); // An empty file has no tree at all, per spec.
  assert(piece_length_in_bytes >= TORRENT_BLOCK_SIZE); // Per spec.
  assert(is_power_of_two(piece_length_in_bytes));      // Per spec.
  assert(piece_hashes);

  // A leaf is a block.
  const usize blocks_count = ceil_usize(data.len, TORRENT_BLOCK_SIZE);
  assert(blocks_count > 0);

  const usize blocks_per_piece = piece_length_in_bytes / TORRENT_BLOCK_SIZE;
  assert(blocks_per_piece > 0);
  // Load bearing: `ctzll` below is only a `log2` because of this.
  assert(is_power_of_two(blocks_per_piece));

  // `next_power_of_two` gives the padded leaf count, so its `log2` is the
  // depth those leaves sit at.
  const usize max_depth =
      (usize)__builtin_ctzll(next_power_of_two(blocks_count));
  // How many levels above the leaves one piece sits. Zero when a piece is a
  // single block, in which case the piece layer *is* the leaf layer.
  const usize piece_bits = (usize)__builtin_ctzll(blocks_per_piece);

  // Every `1 << depth` below would be undefined past this, and `max_depth` is
  // the largest depth any of them use.
  assert(max_depth < 8 * sizeof(usize));
  // The padded leaf layer covers every block, which is the whole point of
  // rounding up to a power of two.
  assert(blocks_count <= ((usize)1 << max_depth));

  // More than one piece means more than `blocks_per_piece` blocks, which
  // pushes the leaves strictly below the piece level, so the subtraction
  // cannot underflow. A single piece file skips the layer entirely and its
  // `piece_depth` is never read.
  const bool has_piece_layer = ceil_usize(blocks_count, blocks_per_piece) > 1;
  if (has_piece_layer) {
    assert(piece_bits < max_depth);
  } else {
    assert(data.len <= piece_length_in_bytes);
  }

  const MerkleTree tree = {
      .data = data,
      .max_depth = max_depth,
      .piece_depth = has_piece_layer ? max_depth - piece_bits : 0,
      .pieces_count = ceil_usize(blocks_count, blocks_per_piece),
      .has_piece_layer = has_piece_layer,
      .piece_hashes = piece_hashes,
  };

  assert(tree.pieces_count > 0);
  // A piece spans at least one block, so there cannot be more of them.
  assert(tree.pieces_count <= blocks_count);
  // The same count the other way round, from bytes rather than blocks. This
  // arithmetic has been wrong twice, and the caller derives its allocation
  // from the byte form, so the two must agree.
  assert(tree.pieces_count == ceil_usize(data.len, piece_length_in_bytes));
  assert(tree.piece_depth <= tree.max_depth);
  if (tree.has_piece_layer) {
    // The piece layer is a real layer, so it cannot hold more entries than it
    // has nodes.
    assert(tree.pieces_count <= ((usize)1 << tree.piece_depth));
  }

  return tree;
}

// Hash the subtree rooted at (`depth`, `tree_width_idx`) into `dst`,
// recording piece hashes into `tree->piece_hashes` on the way past
// `tree->piece_depth`. `tree_width_idx` is the index within its own level, so
// at `max_depth` it is the block index.
static void torrent_build_merkle_sub_tree(const MerkleTree *tree,
                                          usize tree_width_idx, usize depth,
                                          u8 dst[SHA256_DIGEST_LENGTH]) {
  assert(tree);
  assert(dst);
  assert(depth <= tree->max_depth);
  assert(tree_width_idx < ((usize)1 << depth));

  if (tree->max_depth == depth) { // A leaf is one block.
    const usize offset = tree_width_idx * TORRENT_BLOCK_SIZE;

    if (offset < tree->data.len) { // Still inside the file?
      const usize remaining = tree->data.len - offset;
      const Slice_u8 block_data = {
          .data = tree->data.data + offset,
          .len =
              remaining < TORRENT_BLOCK_SIZE ? remaining : TORRENT_BLOCK_SIZE,
      };
      // The last block is the only short one, and no block may read past the
      // mapping the caller handed us.
      assert(block_data.len > 0);
      assert(block_data.len <= TORRENT_BLOCK_SIZE);
      assert(offset + block_data.len <= tree->data.len);
      if (TORRENT_BLOCK_SIZE != block_data.len) {
        assert(offset + block_data.len == tree->data.len);
      }

      sha256_digest(block_data, dst);
    } else { // Past the end of the file: a zero hash, per spec.
      memset(dst, 0, SHA256_DIGEST_LENGTH);
    }
  } else {
    // Progress: children sit one level down and the leaf case above is the
    // only way out, so the recursion cannot run past `max_depth`.
    assert(depth < tree->max_depth);
    assert(2 * tree_width_idx + 1 < ((usize)1 << (depth + 1)));

    u8 left[SHA256_DIGEST_LENGTH] = {0};
    torrent_build_merkle_sub_tree(tree, 2 * tree_width_idx, depth + 1, left);

    u8 right[SHA256_DIGEST_LENGTH] = {0};
    torrent_build_merkle_sub_tree(tree, 2 * tree_width_idx + 1, depth + 1,
                                  right);

    sha256_digest_pair(left, right, dst);
  }

  // Both branches fall through to here on purpose: with a 16KiB piece length
  // the piece layer is the leaf layer, so recording cannot sit in the inner
  // node case alone. An index at or past `pieces_count` is a subtree made
  // only of padding, which BEP 52 leaves out of the piece layer.
  if (tree->has_piece_layer && tree->piece_depth == depth &&
      tree_width_idx < tree->pieces_count) {
    memcpy(tree->piece_hashes[tree_width_idx].digest, dst,
           SHA256_DIGEST_LENGTH);
  }
}

// Build the merkle tree for one file, yielding its root (`pieces root` in the
// info dictionary) and its piece layer (`piece layers` at the torrent root).
// An empty file has neither, per BEP 52, and leaves `root` zeroed.
__attribute__((warn_unused_result)) static Error
torrent_build_merkle_tree(Slice_u8 data, usize piece_length_in_bytes,
                          PieceHash **piece_hashes, usize *piece_hashes_count,
                          u8 root[SHA256_DIGEST_LENGTH], Arena *arena) {
  assert(piece_hashes);
  assert(piece_hashes_count);
  assert(root);
  assert(piece_length_in_bytes >= TORRENT_BLOCK_SIZE); // Per spec.
  assert(is_power_of_two(piece_length_in_bytes));      // Per spec.
  assert(arena);
  assert(arena->start);

  *piece_hashes = NULL;
  *piece_hashes_count = 0;
  memset(root, 0, SHA256_DIGEST_LENGTH);

  if (0 == data.len) {
    return (Error){.kind = ErrKindNone};
  }
  assert(data.data);

  const usize pieces_count = ceil_usize(data.len, piece_length_in_bytes);
  assert(pieces_count > 0);

  PieceHash *const hashes = arena_alloc(arena, __alignof__(PieceHash),
                                        sizeof(PieceHash), pieces_count);
  // OOM?
  if (!hashes) {
    return (Error){.kind = ErrKindOOM};
  }

  const MerkleTree tree =
      torrent_merkle_tree_make(data, piece_length_in_bytes, hashes);
  assert(tree.pieces_count == pieces_count);

  // Poison first so that a piece the traversal skips fails the check below
  // instead of passing off arena leftovers as a hash. An unwritten entry is
  // exactly how the piece arithmetic has failed before, and the zero pattern
  // would not do: a padding leaf hashes to zero, so zero is a value the tree
  // can legitimately produce.
  memset(hashes, MERKLE_PIECE_POISON, pieces_count * sizeof(PieceHash));

  torrent_build_merkle_sub_tree(&tree, 0, 0, root);

  // A single piece file has no piece layer, so nothing was written and the
  // allocation stays hidden from the caller rather than handed over unset.
  if (tree.has_piece_layer) {
    u8 poison[SHA256_DIGEST_LENGTH];
    memset(poison, MERKLE_PIECE_POISON, sizeof(poison));
    for (usize i = 0; i < tree.pieces_count; i++) {
      assert(0 != memcmp(hashes[i].digest, poison, sizeof(poison)) &&
             "the traversal missed a piece");
    }

    *piece_hashes = hashes;
    *piece_hashes_count = tree.pieces_count;
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error torrent_make_metainfo_dict_v2(
    Slice_u8 pieces_root, Slice_u8 announce_url, BencodeList info_dict,
    const PieceHash *piece_hashes, usize piece_hashes_count, BencodeValue *dst,
    Arena *arena) {
  assert(!slice_u8_is_empty(announce_url));
  assert(info_dict.len > 0);
  assert(SHA256_DIGEST_LENGTH == pieces_root.len);
  assert(dst);
  assert(arena);

  // `piece layers` is always present, even with nothing in it: a file that
  // fits in a single piece has no layer, and the key is still emitted as an
  // empty dict.
  const usize kv_count = 3;

  dst->kind = BencodeKindDict;
  dst->v.list.len = 2 * kv_count;
  dst->v.list.data = arena_alloc(arena, __alignof__(BencodeValue),
                                 sizeof(BencodeValue), dst->v.list.len);
  if (!dst->v.list.data) {
    return (Error){.kind = ErrKindOOM};
  }

  // `metainfo["announce"] = announce_url`
  {
    BencodeValue *const announce_key = &dst->v.list.data[0];
    announce_key->kind = BencodeKindString;
    announce_key->v.s = slice_u8_make((u8 *)"announce", sizeof("announce") - 1);

    BencodeValue *const announce_value = &dst->v.list.data[1];
    announce_value->kind = BencodeKindString;
    announce_value->v.s = announce_url;
  }

  // `metainfo["info"] = info_dict`
  {
    BencodeValue *const info_key = &dst->v.list.data[2];
    info_key->kind = BencodeKindString;
    info_key->v.s = slice_u8_make((u8 *)"info", sizeof("info") - 1);

    dst->v.list.data[3].kind = BencodeKindDict;
    dst->v.list.data[3].v.list = info_dict;
  }

  // `metainfo["piece layers"] = {}`
  {
    assert(dst->v.list.len == 2 * kv_count);

    BencodeValue *const pieces_key = &dst->v.list.data[4];
    pieces_key->kind = BencodeKindString;
    pieces_key->v.s =
        slice_u8_make((u8 *)"piece layers", sizeof("piece layers") - 1);

    BencodeValue *const pieces_value = &dst->v.list.data[5];
    pieces_value->kind = BencodeKindDict;

    // A file that fits in a single piece gets no entry at all, per BEP 52,
    // which leaves the dict empty.
    if (0 == piece_hashes_count) {
      pieces_value->v.list = (BencodeList){0};
      return (Error){.kind = ErrKindNone};
    }

    assert(piece_hashes);
    pieces_value->v.list.len = 2;
    pieces_value->v.list.data =
        arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                    pieces_value->v.list.len);
    if (!pieces_value->v.list.data) {
      return (Error){.kind = ErrKindOOM};
    }

    // `metainfo["piece layers"][pieces_root] = piece_hashes`
    //
    // Keyed by the merkle root, not by the file name: that is what ties a
    // layer back to its file across the whole torrent, and it is what every
    // other client looks up.
    {
      BencodeValue *const layer_key = &pieces_value->v.list.data[0];
      layer_key->kind = BencodeKindString;
      layer_key->v.s = pieces_root;

      BencodeValue *const layer_value = &pieces_value->v.list.data[1];
      layer_value->kind = BencodeKindString;
      layer_value->v.s.len = piece_hashes_count * SHA256_DIGEST_LENGTH;
      layer_value->v.s.data =
          arena_alloc(arena, __alignof__(u8), sizeof(u8), layer_value->v.s.len);
      if (!layer_value->v.s.data) {
        return (Error){.kind = ErrKindOOM};
      }

      memcpy(layer_value->v.s.data, piece_hashes, layer_value->v.s.len);
    }
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error torrent_make_info_dict_v2(
    Slice_u8 name, usize piece_length_in_bytes, Slice_u8 file_data,
    Slice_u8 file_name, BencodeValue *dst_info_dict, Slice_u8 *dst_pieces_root,
    PieceHash **dst_piece_hashes, usize *dst_piece_hashes_count, Arena *arena) {
  assert(piece_length_in_bytes >= 16 * KiB);      // Per spec.
  assert(is_power_of_two(piece_length_in_bytes)); // Per spec.
  assert(dst_info_dict);
  assert(dst_pieces_root);
  assert(dst_piece_hashes);
  assert(dst_piece_hashes_count);
  assert(arena);

  *dst_pieces_root = (Slice_u8){0};
  *dst_piece_hashes = NULL;
  *dst_piece_hashes_count = 0;

  const usize dict_items_count = 4;
  *dst_info_dict = (BencodeValue){
      .kind = BencodeKindDict,
      .v.list.len = dict_items_count * 2,
      .v.list.data = arena_alloc(arena, __alignof__(BencodeValue),
                                 sizeof(BencodeValue), dict_items_count * 2),
  };
  if (NULL == dst_info_dict->v.list.data) {
    return (Error){.kind = ErrKindOOM};
  }

  // `info["name"] = name`
  {
    BencodeValue *const key = &dst_info_dict->v.list.data[4];
    key->kind = BencodeKindString;
    key->v.s = slice_u8_make((u8 *)"name", sizeof("name") - 1);

    BencodeValue *const value = &dst_info_dict->v.list.data[5];
    value->kind = BencodeKindString;
    value->v.s = name;
  }

  // `info["piece length"] = piece_length_in_bytes`
  {
    BencodeValue *const key = &dst_info_dict->v.list.data[6];
    key->kind = BencodeKindString;
    key->v.s = slice_u8_make((u8 *)"piece length", sizeof("piece length") - 1);

    BencodeValue *const value = &dst_info_dict->v.list.data[7];
    value->kind = BencodeKindInteger;
    const Error err =
        isize_from_usize(piece_length_in_bytes, false, &value->v.num);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  // `info["meta version"] = 2`
  {

    BencodeValue *const key = &dst_info_dict->v.list.data[2];
    key->kind = BencodeKindString;
    key->v.s = slice_u8_make((u8 *)"meta version", sizeof("meta version") - 1);

    BencodeValue *const value = &dst_info_dict->v.list.data[3];
    value->kind = BencodeKindInteger;
    value->v.num = 2;
  }

  // `info["file tree"] = ...`
  {
    BencodeValue *const file_tree_key = &dst_info_dict->v.list.data[0];
    file_tree_key->kind = BencodeKindString;
    file_tree_key->v.s =
        slice_u8_make((u8 *)"file tree", sizeof("file tree") - 1);

    u8 root[SHA256_DIGEST_LENGTH] = {0};
    const Error err = torrent_build_merkle_tree(
        file_data, piece_length_in_bytes, dst_piece_hashes,
        dst_piece_hashes_count, root, arena);
    if (ErrKindNone != err.kind) {
      return err;
    }
    // If the file data does not fit within one piece, then the piece layer is
    // required (per spec).
    if (file_data.len > piece_length_in_bytes) {
      assert(*dst_piece_hashes);
      assert(*dst_piece_hashes_count > 0);
    }

    BencodeValue *const file_tree_dict = &dst_info_dict->v.list.data[1];
    file_tree_dict->kind = BencodeKindDict;
    file_tree_dict->v.list.len = 2;
    file_tree_dict->v.list.data =
        arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                    file_tree_dict->v.list.len);
    if (NULL == file_tree_dict->v.list.data) {
      return (Error){.kind = ErrKindOOM};
    }

    // `info["file tree"][file_name] = {}`
    {
      BencodeValue *const file_name_key = &file_tree_dict->v.list.data[0];
      file_name_key->kind = BencodeKindString;
      file_name_key->v.s = file_name;

      BencodeValue *const file_name_dict = &file_tree_dict->v.list.data[1];
      file_name_dict->kind = BencodeKindDict;
      file_name_dict->v.list.len = 2;
      file_name_dict->v.list.data =
          arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                      file_name_dict->v.list.len);
      if (NULL == file_name_dict->v.list.data) {
        return (Error){.kind = ErrKindOOM};
      }

      // `info["file tree"][file_name][""] = {}`
      {
        BencodeValue *const empty_key = &file_name_dict->v.list.data[0];
        empty_key->kind = BencodeKindString;
        empty_key->v.s = (Slice_u8){0};

        BencodeValue *const empty_dict = &file_name_dict->v.list.data[1];
        empty_dict->kind = BencodeKindDict;
        empty_dict->v.list.len = 2 * 2;
        empty_dict->v.list.data =
            arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                        empty_dict->v.list.len);
        if (NULL == empty_dict->v.list.data) {
          return (Error){.kind = ErrKindOOM};
        }

        // `info["file tree"][file_name][""]["length"] = file_data.length`
        {
          BencodeValue *const length_key = &empty_dict->v.list.data[0];
          length_key->kind = BencodeKindString;
          length_key->v.s = slice_u8_make((u8 *)"length", sizeof("length") - 1);

          BencodeValue *const length_value = &empty_dict->v.list.data[1];
          length_value->kind = BencodeKindInteger;
          const Error length_err =
              isize_from_usize(file_data.len, false, &length_value->v.num);
          if (ErrKindNone != length_err.kind) {
            return length_err;
          }
        }

        // `info["file tree"][file_name][""]["pieces root"] = root.digest`
        {
          BencodeValue *const pieces_root_key = &empty_dict->v.list.data[2];
          pieces_root_key->kind = BencodeKindString;
          pieces_root_key->v.s =
              slice_u8_make((u8 *)"pieces root", sizeof("pieces root") - 1);

          BencodeValue *const pieces_root_value = &empty_dict->v.list.data[3];
          pieces_root_value->kind = BencodeKindString;
          pieces_root_value->v.s.len = SHA256_DIGEST_LENGTH;
          pieces_root_value->v.s.data = arena_alloc(
              arena, __alignof__(u8), sizeof(u8), SHA256_DIGEST_LENGTH);
          if (NULL == pieces_root_value->v.s.data) {
            return (Error){.kind = ErrKindOOM};
          }
          memcpy(pieces_root_value->v.s.data, root, SHA256_DIGEST_LENGTH);

          // `piece layers` is keyed by this exact digest, so hand it back
          // rather than making the caller dig it out of the tree.
          *dst_pieces_root = pieces_root_value->v.s;
        }
      }
    }
  }

  return (Error){.kind = ErrKindNone};
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
encode_usize_base_10(usize n, Slice_u8 dst) {
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
encode_isize_base_10(isize n, Slice_u8 dst) {
  assert(dst.data);

  const bool negative = n < 0;
  const usize magnitude = isize_magnitude(n);

  if (!negative) {
    return encode_usize_base_10(magnitude, dst);
  }

  assert(dst.len >= 1);
  dst.data[0] = '-';

  const usize digits =
      encode_usize_base_10(magnitude, slice_u8_make(dst.data + 1, dst.len - 1));

  return 1 + digits;
}

__attribute__((warn_unused_result)) static usize
bencode_encode_exact_size(BencodeValue b, usize depth) {
  assert(depth <= BENCODE_MAX_DEPTH);

  usize res = 0;

  switch (b.kind) {
  case BencodeKindInteger:
    // `i` <digits> `e`
    assert(
        !__builtin_add_overflow(res, 2 + isize_digits_base_10(b.v.num), &res));
    break;
  case BencodeKindString:
    // <length> `:` <bytes>
    assert(!__builtin_add_overflow(res, usize_digits_base_10(b.v.s.len) + 1,
                                   &res));
    assert(!__builtin_add_overflow(res, b.v.s.len, &res));
    break;
  case BencodeKindList:
  case BencodeKindDict:
    // `l` or `d`, then `e`
    assert(!__builtin_add_overflow(res, 2, &res));

    for (usize i = 0; i < b.v.list.len; i++) {
      const usize item_size =
          bencode_encode_exact_size(b.v.list.data[i], depth + 1);
      assert(!__builtin_add_overflow(res, item_size, &res));
    }
    break;
  }

  return res;
}

// Sizing is a whole subtree walk, so the checks against it live in the
// wrapper below rather than here, where they would run once per level of
// nesting.
__attribute__((warn_unused_result)) static usize
bencode_encode_rec(BencodeValue b, Slice_u8 dst, usize depth) {
  assert(depth <= BENCODE_MAX_DEPTH);
  assert(dst.data);
  assert(dst.len >= 2);
  u8 *const dst_before = dst.data;

  switch (b.kind) {
  case BencodeKindInteger: {
    dst.data[0] = 'i';
    slice_u8_advance(&dst, 1);

    slice_u8_advance(&dst, encode_isize_base_10(b.v.num, dst));

    dst.data[0] = 'e';
    slice_u8_advance(&dst, 1);

  } break;
  case BencodeKindString: {
    slice_u8_advance(&dst, encode_usize_base_10(b.v.s.len, dst));

    dst.data[0] = ':';
    slice_u8_advance(&dst, 1);

    if (b.v.s.len > 0) {
      memcpy(dst.data, b.v.s.data, b.v.s.len);
      slice_u8_advance(&dst, b.v.s.len);
    }
  } break;
  case BencodeKindList:
  case BencodeKindDict: {
    dst.data[0] = b.kind == BencodeKindList ? 'l' : 'd';
    slice_u8_advance(&dst, 1);

    for (usize i = 0; i < b.v.list.len; i++) {
      const BencodeValue item = b.v.list.data[i];
      slice_u8_advance(&dst, bencode_encode_rec(item, dst, depth + 1));
    }

    dst.data[0] = 'e';
    slice_u8_advance(&dst, 1);
  } break;

  default:
    assert(0 && "unreachable");
  }

  assert(dst.data > dst_before);

  const usize written = (usize)(dst.data - dst_before);
  assert(written >= 2);

  return written;
}

// Encode `b` at the front of `dst` and return the number of bytes written.
//
// `dst` must be exactly `bencode_encode_exact_size(b, 0)` bytes, which the
// caller has already computed in order to allocate it. Requiring exactness
// rather than sufficiency costs nothing and buys the check below: the two
// passes are compared without walking the tree a third time, and the
// recursion cannot scribble into slack it was never given.
__attribute__((warn_unused_result)) static usize
bencode_encode_in_place(BencodeValue b, Slice_u8 dst) {
  assert(dst.data);

  const usize written = bencode_encode_rec(b, dst, 0);
  assert(dst.len == written);

  return written;
}

__attribute__((warn_unused_result)) static Error
bencode_encode(BencodeValue b, Slice_u8 *dst, Arena *arena) {
  assert(dst);
  assert(arena);

  const usize size = bencode_encode_exact_size(b, 0);

  *dst =
      (Slice_u8){.data = arena_alloc(arena, __alignof__(u8), sizeof(u8), size),
                 .len = size};
  if (!dst->data) {
    return (Error){.kind = ErrKindOOM};
  }

  const usize written = bencode_encode_in_place(b, *dst);
  assert(written == size);
  assert(written == dst->len);

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
torrent_gen_torrent_file_data(Slice_u8 file_path, Slice_u8 file_data,
                              Slice_u8 announce_url, Slice_u8 *dst_torrent,
                              u8 dst_info_hash[SHA256_DIGEST_LENGTH],
                              Arena scratch, Arena *arena) {
  // TODO: Consider passing a scratch arena for some allocations.

  assert(dst_torrent);
  assert(dst_info_hash);
  assert(arena);

  Error err = {0};

  const Slice_u8 file_name =
      path_last_component(file_path, PATH_SEPARATOR_UNIX);

  BencodeValue info_dict = {0};
  PieceHash *piece_hashes = NULL;
  usize piece_hashes_count = 0;
  Slice_u8 pieces_root = {0};
  err = torrent_make_info_dict_v2(file_name, TORRENT_BLOCK_SIZE * 16, file_data,
                                  file_name, &info_dict, &pieces_root,
                                  &piece_hashes, &piece_hashes_count, &scratch);

  if (ErrKindNone != err.kind) {
    return err;
  }

  Slice_u8 info_dict_encoded = {0};
  err = bencode_encode(info_dict, &info_dict_encoded, &scratch);
  if (ErrKindNone != err.kind) {
    return err;
  }

  sha256_digest(info_dict_encoded, dst_info_hash);

  BencodeValue metainfo_dict = {0};
  err = torrent_make_metainfo_dict_v2(
      pieces_root, announce_url, info_dict.v.list, piece_hashes,
      piece_hashes_count, &metainfo_dict, &scratch);
  if (ErrKindNone != err.kind) {
    return err;
  }

  Slice_u8 metainfo_dict_encoded = {0};
  err = bencode_encode(metainfo_dict, &metainfo_dict_encoded, arena);
  if (ErrKindNone != err.kind) {
    return err;
  }

  *dst_torrent = metainfo_dict_encoded;

  return (Error){.kind = ErrKindNone};
}

typedef struct TorrentNetworkCtx TorrentNetworkCtx;

typedef struct {
  // The caller's, passed through `io_listen_and_serve_tcp_ipv4`. The vtable's
  // own context lives in `io->ctx`.
  void *cb_ctx;
  const IO *io;
  TorrentNetworkCtx *network_ctx;
  i32 socket;
  Ipv4Addr addr;
  // More: torrent, etc.
} TorrentClientHandleCtx;

#define TORRENT_CLIENTS_MAX 1024

typedef u64 PoolSlotGroup;

// Unit is bits.
#define POOL_SLOTS_PER_GROUP (sizeof(PoolSlotGroup) * 8)
_Static_assert(0 == (TORRENT_CLIENTS_MAX % POOL_SLOTS_PER_GROUP),
               "must be a multiple");

#define POOL_SLOT_GROUPS (TORRENT_CLIENTS_MAX / POOL_SLOTS_PER_GROUP)

typedef struct {
  // Bitset.
  // Bit `i` of group `g` means: `slots[g * POOL_SLOTS_PER_GROUP + i]` is
  // occupied.
  PoolSlotGroup occupied[POOL_SLOT_GROUPS];
  TorrentClientHandleCtx slots[TORRENT_CLIENTS_MAX];
} TorrentClientHandleCtxPool;

struct TorrentNetworkCtx {
  TorrentClientHandleCtxPool pool;
  // More...
};

__attribute__((warn_unused_result)) static TorrentClientHandleCtx *
torrent_client_ctx_pool_acquire(TorrentClientHandleCtxPool *pool) {
  assert(pool);

  for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
    // 'Relaxed' means that we sometimes can report the slot group as full when
    // it's not. It's a TOCTOU window we accept and there is no easy fix.
    const PoolSlotGroup slot_group =
        __atomic_load_n(&pool->occupied[i], __ATOMIC_RELAXED);

    const i32 first_unset_bit = __builtin_ffsll((i64)~slot_group);

    // Slot full, keep scanning to find a free slot?
    if (0 == first_unset_bit) {
      continue;
    }

    const u32 bit_idx = (u32)(first_unset_bit - 1);
    const PoolSlotGroup mask = 1ULL << bit_idx;

    // Mark the slot as occupied.
    const PoolSlotGroup prev =
        __atomic_fetch_or(&pool->occupied[i], mask, __ATOMIC_ACQUIRE);

    // Since there is only one concurrent caller of 'pool_acquire' no
    // one could have concurrently acquired the slot that was free at the start
    // of this loop iteration.
    assert(0 == (prev & mask));

    const usize slot_idx = i * POOL_SLOTS_PER_GROUP + bit_idx;
    assert(slot_idx < TORRENT_CLIENTS_MAX);

    TorrentClientHandleCtx *res = &pool->slots[slot_idx];
    assert(0 == res->cb_ctx);
    assert(0 == res->io);
    assert(0 == res->network_ctx);
    assert(0 == res->socket);
    assert(0 == res->addr.ip);
    assert(0 == res->addr.port);
    return res;
  }

  return NULL;
}

static void torrent_client_ctx_pool_release(TorrentClientHandleCtxPool *pool,
                                            TorrentClientHandleCtx *slot) {
  assert(pool);
  assert(slot);

  assert(slot >= pool->slots);
  const usize slot_idx = (usize)(slot - pool->slots);
  assert(slot_idx < TORRENT_CLIENTS_MAX);

  const usize slot_group_idx = slot_idx / POOL_SLOTS_PER_GROUP;
  assert(slot_group_idx < POOL_SLOT_GROUPS);
  const u32 bit_idx = slot_idx % POOL_SLOTS_PER_GROUP;

  // We are still the owner so we are responsible for zeroing it.
  memset(slot, 0, sizeof(*slot));

  const PoolSlotGroup mask = ~(1ULL << bit_idx);
  const PoolSlotGroup prev = __atomic_fetch_and(&pool->occupied[slot_group_idx],
                                                mask, __ATOMIC_RELEASE);

  // Sanity check against double release of the same slot: the slot was indeed
  // occupied before.
  assert(0 != (prev & ~mask));
}

static void *torrent_client_handle(void *vctx) {
  assert(vctx);

  TorrentClientHandleCtx *const client_ctx = vctx;
  assert(client_ctx->io);

  const u32 ip = client_ctx->addr.ip;
  printf("accepted: %u.%u.%u.%u:%hu\n", ip >> 24 & 0xff, ip >> 16 & 0xff,
         ip >> 8 & 0xff, ip >> 0 & 0xff, client_ctx->addr.port);

  usize read_count = 0;
  u8 buf[4096] = {0};
  Slice_u8 slice_read = slice_u8_make(buf, sizeof(buf));

  Error err = client_ctx->io->read(client_ctx->io, client_ctx->socket,
                                   slice_read, &read_count);
  if (ErrKindNone != err.kind) {
    goto end;
  }

  const Slice_u8 slice_read_actual = slice_u8_take(slice_read, read_count);
  printf("read: %.*s\n", (i32)slice_read_actual.len, slice_read_actual.data);

end:
  (void)client_ctx->io->close(client_ctx->io, client_ctx->socket);

  puts("torrent_client_handle end");

  // Responsible for freeing our context.
  torrent_client_ctx_pool_release(&client_ctx->network_ctx->pool, client_ctx);

  return NULL;
}

__attribute__((warn_unused_result)) static Error
torrent_client_on_accept(const IO *io, void *vctx, Ipv4Addr accept_addr,
                         i32 accept_socket) {
  assert(io);
  assert(vctx);
  TorrentNetworkCtx *const network_ctx = vctx;

  puts("accepted");

  Error err = {.kind = ErrKindNone};
  TorrentClientHandleCtx *const client_ctx =
      torrent_client_ctx_pool_acquire(&network_ctx->pool);
  if (!client_ctx) {
    fprintf(stderr, "backpressure: no available pool slot for client\n");
    (void)io->close(io, accept_socket);
    return (Error){.kind = ErrKindOOM};
  }

  assert(client_ctx);
  client_ctx->cb_ctx = vctx;
  client_ctx->addr = accept_addr;
  client_ctx->socket = accept_socket;
  client_ctx->io = io;
  client_ctx->network_ctx = network_ctx;

  err = io->thread_create(io, torrent_client_handle, client_ctx);
  if (ErrKindNone != err.kind) {
    // The thread never started, so nothing else will free the context or hang
    // up on the peer.
    torrent_client_ctx_pool_release(&network_ctx->pool, client_ctx);
    (void)io->close(io, accept_socket);
  }

  // Nothing to cleanup: the client handler finished successfully and is
  // responsible for the cleanup.

  return err;
}

#include "test.c"

int main(i32 argc, char *argv[]) {
  assert(argv);

  const IO io = io_unix_make();

  const char *const cmd = argc >= 2 ? argv[1] : "";
  const usize arena_cap = 32 * MiB;
  Arena arena = {0};
  assert(ErrKindNone == arena_valloc(&io, arena_cap, &arena).kind);

  Arena scratch = {0};
  assert(ErrKindNone == arena_valloc(&io, 1 * MiB, &scratch).kind);

  if (0 == strcmp(cmd, "test")) {
    test(argc > 2 ? argv[2] : NULL);
  } else if (0 == strcmp(cmd, "broadcast")) {
    i32 udp_socket = 0;
    {
      Error err_udp = unix_udp_multicast_open_ipv4(NULL, 0, &udp_socket);
      if (ErrKindNone != err_udp.kind) {
        error_print("failed to open UDP multicast socket", err_udp);
        return 1;
      }
    }
    {
      const u8 msg[] = "Hello!";
      usize sent = 0;
      const Ipv4Addr lsd_addr = {
          .ip = 0xefc0988fUL, // 239.192.152.143
          .port = 6771,
      };

      Error err_sendto = unix_udp_send_to_ipv4(NULL, udp_socket, lsd_addr, msg,
                                               sizeof(msg), &sent);
      if (ErrKindNone != err_sendto.kind) {
        error_print("failed to send UDP multicast message", err_sendto);
        return 1;
      }
    }
  } else if (0 == strcmp(cmd, "gen-torrent")) {
    if (3 != argc) {
      fprintf(stderr, "missing argument\n");
      return 1;
    }

    const Slice_u8 file_path = {.data = (u8 *)argv[2], .len = strlen(argv[2])};
    Slice_u8 input = {0};

    Error err = io.map_file(&io, file_path, FileOpenOptionsReadOnly, &input);
    if (ErrKindNone != err.kind) {
      error_print("failed to open file", err);
      return 1;
    }

    u8 announce_url_cstr[] = "http://localhost:12345";
    Slice_u8 announce_url =
        slice_u8_make(announce_url_cstr, sizeof(announce_url_cstr) - 1);

    Slice_u8 torrent_file_data = {0};
    u8 info_hash[SHA256_DIGEST_LENGTH] = {0};
    err = torrent_gen_torrent_file_data(file_path, input, announce_url,
                                        &torrent_file_data, info_hash, scratch,
                                        &arena);
    if (ErrKindNone != err.kind) {
      error_print("failed to generate torrent file data", err);
      return 1;
    }

    Slice_u8 torrent_file_path = {0};
    err = path_with_ext(file_path, slice_u8_from_cstr((char *)"torrent"),
                        PATH_SEPARATOR_UNIX, &torrent_file_path, &arena);
    if (ErrKindNone != err.kind) {
      error_print("failed to compute the torrent path", err);
      return 1;
    }

    err = io.write_all_to_file(&io, torrent_file_path, torrent_file_data);
    if (ErrKindNone != err.kind) {
      error_print("failed to write torrent file", err);
      return 1;
    }
  } else if (0 == strcmp(cmd, "share")) {
    if (3 != argc) {
      fprintf(stderr, "missing argument\n");
      return 1;
    }
    const Slice_u8 file_path = slice_u8_from_cstr(argv[2]);

    const Slice_u8 file_ext = path_get_ext(file_path, PATH_SEPARATOR_UNIX);
    if (!slice_u8_eq_cstr(file_ext, ".torrent")) {
      fprintf(stderr, "provided file is not a .torrent file: %s\n", argv[2]);
      return 1;
    }

    Slice_u8 input = {0};

    Error err = io.map_file(&io, file_path, FileOpenOptionsReadOnly, &input);
    if (ErrKindNone != err.kind) {
      error_print("failed to open file", err);
      return 1;
    }

    const Ipv4Addr listen_addr = {.port = 12345, .ip = 0};
    TorrentNetworkCtx ctx = {0};
    Error err_listen = io_listen_and_serve_tcp_ipv4(&io, &ctx, listen_addr,
                                                    torrent_client_on_accept);
    if (ErrKindNone != err_listen.kind) {
      error_print("failed to listen and serve", err_listen);
      return 1;
    }

    const usize unused_bytes = (usize)arena.end - (usize)arena.start;
    const usize used_bytes = arena_cap - unused_bytes;
    printf("mem used: %zu\n", used_bytes);
    printf("mem unused: %zu\n", unused_bytes);
  } else {
    fprintf(stderr, "unknown command: %s\n", cmd);
    exit(1);
  }
}
