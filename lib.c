#pragma once

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Writes the operating system's own description of `os_error` into `dst`, and
// answers whether there was one to write. Declared here and defined by the
// platform file, which the unity build reaches only much further down: what it
// takes to render a system error is per system, but wanting to render one is
// not.
__attribute__((warn_unused_result)) static bool
platform_error_describe(u64 os_error, char *dst, usize dst_len);

// Renders `err` to stderr, appending the operating system's own description
// when the error carries an `errno`.
static void error_print(const char *context, Error err) {
  assert(context);

  if (0 == err.data) {
    fprintf(stderr, "%s: %s\n", context, error_kind_to_cstr(err.kind));
    return;
  }

  char os_msg[256] = {0};

  if (!platform_error_describe(err.data, os_msg, sizeof(os_msg))) {
    // The description did not fit or the number is not one the system knows;
    // the number itself is still worth printing.
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
// they are available, which is why this cannot fail: every parser here has to
// know the length before it advances anyway, either to `slice_u8_take` the
// bytes or to pick which error a short input deserves, so a checking variant
// would only ever re-check what the caller just established.
static void slice_u8_advance(Slice_u8 *slice, usize count) {
  assert(slice);
  assert(slice->data);
  assert(count <= slice->len);

  slice->len -= count;
  slice->data += count;
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
  Error (*udp_multicast_open_ipv4)(const IO *io, u32 ipv4, i32 *dst_fd);
  Error (*udp_send_to_ipv4)(const IO *io, i32 fd, Ipv4Addr addr, const u8 *buf,
                            usize len, usize *dst_sent);
  Error (*read)(const IO *io, i32 fd, Slice_u8 data, usize *dst_read);
  Error (*write)(const IO *io, i32 fd, Slice_u8 data, usize *dst_written);
  Error (*file_size)(const IO *io, i32 fd, usize *dst_size);
  Error (*map_file)(const IO *io, Slice_u8 path, FileOpenOptions opts,
                    Slice_u8 *dst);
  Error (*write_all_to_file)(const IO *io, Slice_u8 path, Slice_u8 data);
  Error (*remove_file)(const IO *io, Slice_u8 path);
  usize (*get_page_size)(const IO *io);
  Error (*valloc)(const IO *io, usize bytes_count, u8 **res);
  Error (*vprotect_none)(const IO *io, void *ptr, usize size);
  usize (*get_process_id)(const IO *io);

  // Swallow this process's own standard output until the matching restore,
  // which is handed back whatever `stdout_silence` produced. Only the test
  // harness calls these, to keep a chatty run quiet; the program proper never
  // redirects itself. They are slots and not three lines of `dup` in the
  // harness because what it takes to do this is exactly the kind of thing
  // that differs per platform.
  Error (*stdout_silence)(const IO *io, i32 *dst_saved);
  Error (*stdout_restore)(const IO *io, i32 saved);

  // The implementation's own state, reached by every slot above as
  // `io->ctx`. It is not the caller's: a callback's user data travels
  // separately, because the two have different lifetimes and owners.
  void *ctx;
};

typedef Error (*AcceptCallback)(const IO *io, void *cb_ctx,
                                Ipv4Addr accept_addr, i32 accept_socket);

__attribute__((warn_unused_result)) static Error
io_listen_and_serve_tcp_ipv4(const IO *io, void *cb_ctx, Ipv4Addr listen_addr,
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
    const Error err_on_accept =
        on_accept(io, cb_ctx, accept_addr, accept_socket);
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
         io->vprotect_none(io, arena_memory + usable_bytes, page_size).kind);

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

typedef struct {
  Slice_u8 container;
  usize len;
} StringBuffer;

__attribute__((warn_unused_result)) static Error
sb_make(usize cap, Arena *arena, StringBuffer *dst) {
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

__attribute__((warn_unused_result)) static usize sb_space(StringBuffer sb) {
  assert(sb.len <= sb.container.len);

  return sb.container.len - sb.len;
}

__attribute__((warn_unused_result)) static bool
sb_extend_within_cap(StringBuffer *sb, Slice_u8 s) {
  assert(sb);

  if (!s.data || 0 == s.len) {
    return true;
  }

  if (sb_space(*sb) < s.len) {
    return false;
  }

  memcpy(sb->container.data + sb->len, s.data, s.len);
  assert(!__builtin_add_overflow(sb->len, s.len, &sb->len));

  return true;
}
