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

static const usize KiB = 1024;
static const usize MiB = 1024 * KiB;

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
  // The thing asked for is not something this build can do: an `IO` backend the
  // platform does not have, say. Not a failure at runtime so much as a question
  // that has no answer here.
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
slice_u8_from_cstr(const char *s) {
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

__attribute__((warn_unused_result)) static bool slice_u8_eq(Slice_u8 a,
                                                            Slice_u8 b) {
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
slice_u8_eq_cstr(Slice_u8 s, const char *cstr) {
  if (!cstr) {
    return slice_u8_is_empty(s);
  }
  return slice_u8_eq(s, slice_u8_from_cstr(cstr));
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

__attribute__((warn_unused_result)) static bool
slice_u8_starts_with(Slice_u8 s, Slice_u8 prefix) {
  if (s.len < prefix.len) {
    return false;
  }

  return slice_u8_eq(prefix, slice_u8_take(s, prefix.len));

  return true;
}

__attribute__((warn_unused_result)) static Slice_u8 slice_u8_make(u8 *data,
                                                                  usize len) {
  if (0 != len) {
    assert(data);
  }

  return (Slice_u8){.data = data, .len = len};
}

__attribute__((warn_unused_result)) static Error
slice_u8_expect_u8(Slice_u8 *slice, u8 expected) {
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

__attribute__((warn_unused_result)) static bool
slice_u8_consume_u8(Slice_u8 *slice, u8 *dst) {
  assert(slice);
  if (slice->len < sizeof(*dst)) {
    return false;
  }

  if (dst) {
    *dst = slice->data[0];
  }

  slice_u8_advance(slice, sizeof(*dst));

  return true;
}

__attribute__((warn_unused_result)) static bool
slice_u8_consume_u32_be(Slice_u8 *slice, u32 *dst) {
  assert(slice);
  assert(dst);

  if (slice->len < sizeof(*dst)) {
    return false;
  }

  *dst = (u32)slice->data[0] << 24 | (u32)slice->data[1] << 16 |
         (u32)slice->data[2] << 8 | (u32)slice->data[3];

  slice_u8_advance(slice, sizeof(*dst));

  return true;
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

typedef struct Env Env;

// Everything the program needs from the operating system that never waits on
// anything outside this process. No bytes cross a socket or a disk here, so
// there is nothing for an event loop to wait for and the call is the whole
// operation; `IO` is for the rest.
//
// Every descriptor this hands out is non-blocking. That is not an option but a
// requirement of `IO`: a readiness loop is only correct if the syscall it
// retries can answer `ErrKindAgain` instead of parking the one thread that
// runs the loop.
struct Env {
  usize (*get_page_size)(const Env *env);
  Error (*valloc)(const Env *env, usize bytes_count, u8 **res);
  Error (*vprotect_none)(const Env *env, void *ptr, usize size);
  usize (*get_process_id)(const Env *env);

  Error (*socket)(const Env *env, SocketDomain domain, SocketType type,
                  i32 *fd);
  Error (*listen)(const Env *env, i32 fd, i32 backlog);
  Error (*tcp_bind_ipv4)(const Env *env, i32 listen_socket, Ipv4Addr addr);
  Error (*enable_socket_reuse)(const Env *env, i32 fd);
  Error (*udp_multicast_open_ipv4)(const Env *env, u32 ipv4, i32 *dst_fd);

  // Hang up on a descriptor nobody is waiting on. `IO` has a `close` too, and
  // the difference is who wants the answer: `IO`'s is a step in a chain of
  // callbacks, and the step after it needs to know the descriptor is gone. This
  // one is for a connection that is being refused, where there is no chain, no
  // completion to keep and nothing to do with the answer -- and it is here
  // rather than there because `close` on a non-blocking descriptor does not
  // wait for anything, so submitting it would buy nothing.
  //
  // The distinction is not cosmetic: a refusal with nowhere to keep a
  // completion would have to share one, and two refusals in the same turn of
  // the loop would then overwrite a close that had not happened yet.
  Error (*close_socket)(const Env *env, i32 fd);

  // Map `size` bytes of `fd`. The kernel only records the mapping; the reads
  // it stands for happen later, on a page fault, where no loop can see them.
  // That makes this memory management and not an operation to submit.
  Error (*map_fd)(const Env *env, i32 fd, usize size, FileOpenOptions opts,
                  Slice_u8 *dst);

  // Nothing in the program proper calls this any more: one thread runs one
  // `IO` loop, and a second thread driving the same loop would be a race. It
  // stays because spawning a thread is still a thing the platform does
  // differently, and something outside the loop may yet need one.
  Error (*thread_create)(const Env *env, ThreadCallback cb, void *data);

  // Swallow this process's own standard output until the matching restore,
  // which is handed back whatever `stdout_silence` produced. Only the test
  // harness calls these, to keep a chatty run quiet; the program proper never
  // redirects itself. They are slots and not three lines of `dup` in the
  // harness because what it takes to do this is exactly the kind of thing
  // that differs per platform.
  Error (*stdout_silence)(const Env *env, i32 *dst_saved);
  Error (*stdout_restore)(const Env *env, i32 saved);

  // The implementation's own state, reached by every slot above as
  // `env->ctx`. The real platform is stateless and leaves it null; a test's
  // fake is what it is for.
  void *ctx;
};

// One per process: an `IO` holds on to the `Env` it was made with, so a
// caller cannot be handed something it has to keep alive itself.
__attribute__((warn_unused_result)) static const Env *env_platform_make(void);

// Named before the struct body so a composite built out of these slots can
// take the vtable it belongs to.
typedef struct IO IO;

typedef enum {
  // A completion that was never submitted. Named so that the zeroed slot a
  // pool hands out is a recognisable "no operation" rather than a read.
  IoActionKindNone,
  IoActionKindOpen,
  IoActionKindClose,
  IoActionKindRead,
  IoActionKindWrite,
  IoActionKindAccept,
  IoActionKindConnect,
  IoActionKindSendTo,
  IoActionKindFileSize,
  IoActionKindRemoveFile,
} IoActionKind;

// What an operation was asked to do, kept with the operation because the
// answer arrives long after the call that asked for it has returned. A slot
// fills this in from its arguments, which is what lets the caller's arguments
// be temporaries.
typedef struct {
  IoActionKind kind;
  union {
    struct {
      Slice_u8 path;
      FileOpenOptions options;
    } open;
    struct {
      Slice_u8 data;
    } read;
    struct {
      Slice_u8 data;
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
      Slice_u8 data;
    } send_to;
    struct {
      Slice_u8 path;
    } remove_file;
  } v;
} IoAction;

typedef struct IoCompletion IoCompletion;

// How an operation answers. `res` is a byte count for a read, a write or a
// send, a descriptor for an `open` or an `accept`, a size for a `file_size`,
// and is unused by a `close` and a `remove_file`.
//
// The callback runs on the thread that called `run_for_ns`, with the
// completion no longer in the implementation's hands: submitting the next
// operation on the same completion from inside it is the ordinary way to
// chain, and what the state machines below do.
typedef void (*IoCallback)(IoCompletion *completion, Error err, usize res);

struct IoCompletion {
  // The submitter's, and never the implementation's: the two have different
  // lifetimes and owners. A slot leaves it alone, so it can be set once when
  // the completion is created and left for every operation after that.
  void *ctx;
  // There is no descriptor here. A descriptor is an argument of the operation,
  // and an implementation is already holding the operation for the caller, so
  // it keeps the descriptor wherever it keeps the rest of its bookkeeping
  // rather than making every completion eight bytes wider.
  IoCallback cb;
  IoAction action;
};

// Every slot here submits an operation and returns at once, without making the
// syscall: that happens on the next turn of the loop. An `Error` from a slot
// means the operation was never submitted and its callback will never run --
// the only reason for one is that the implementation has no room left to take
// the operation, which is backpressure and not a failure of the operation.
// Anything the operation itself has to report reaches the callback.
//
// libuv makes the write syscall in `uv_write` itself and only waits for
// writability if that answers `EAGAIN`, while leaving a read to wait
// (`uv__read_start` has a standing `TODO: try to do the read inline?`). The
// asymmetry is well judged -- a send buffer usually has room, whereas data
// usually has not arrived -- and it is deliberately not copied here yet:
//
//   - It is not a syscall cheaper. Submitting a change and being handed it back
//     are one `kevent` on Darwin, and the syscall follows immediately, so the
//     whole of the difference is that the bytes leave after control returns to
//     the loop instead of before. Which matters more the more the loop does per
//     turn, and not at all for a file, which never answers `EAGAIN`.
//   - For a read it would be worse, not better: attempting at submission leaves
//     less time for data to arrive than attempting a turn later does.
//   - The callback has to stay deferred either way -- libuv defers it too, via
//     `write_completed_queue` -- so an operation the slot completed itself
//     needs its answer kept until the loop delivers it, which is three more
//     fields on every completion.
//   - And it is only safe while nothing else is pending on that descriptor, or
//     the stream reorders. libuv guards on `empty_queue` for exactly that.
//
// Nothing here writes to a socket yet, so there is no measurement to weigh any
// of that against. Revisit with the peer protocol, when there is one.
//
// The completion belongs to the implementation from the moment it is submitted
// until its callback runs, so it has to outlive that and must not be touched,
// moved or reused in between. A completion carries no list pointers because
// nothing here keeps a list of its own: the operating system is asked to hold
// the submitted operations, and it is handed the completion's address to hand
// back.
struct IO {
  const Env *env;

  // Try whatever was submitted, then wait up to `ns` for the operations that
  // were not ready, running the callback of each one that becomes ready.
  //
  // A callback may submit more work. That work is picked up by a later call
  // and not by this one, so a callback that resubmits cannot spin one tick
  // forever, and no callback ever runs nested inside another.
  Error (*run_for_ns)(IO *io, usize ns);

  // Nanoseconds on a clock that only goes forward, from an epoch nobody is told
  // about: good for a difference and for nothing else. This is the whole of
  // what a deadline needs, and it is why there is no timer operation here --
  // nothing is armed, so there is nothing to cancel and nothing that can
  // outlive what armed it.
  //
  // On `IO` and not on `Env`, though asking the platform the time is `Env`'s
  // sort of job: the loop is what wants it every turn, to size the next
  // `run_for_ns` against the nearest deadline, and it already has an `IO`.
  //
  // No `Error`, unlike every other slot: this cannot fail. `clock_gettime` on a
  // monotonic clock with a good pointer has nothing to report, and a caller
  // deciding whether a deadline has passed has nothing to do with a failure
  // anyway.
  u64 (*monotonic_ns)(IO *io);

  Error (*open)(IO *io, IoCompletion *completion, Slice_u8 path,
                FileOpenOptions options, IoCallback cb);
  Error (*close)(IO *io, IoCompletion *completion, i32 fd, IoCallback cb);
  Error (*read)(IO *io, IoCompletion *completion, i32 fd, Slice_u8 data,
                IoCallback cb);
  Error (*write)(IO *io, IoCompletion *completion, i32 fd, Slice_u8 data,
                 IoCallback cb);
  // On success the callback's `res` is the accepted socket, and the peer is in
  // `completion->action.v.accept.addr`.
  Error (*accept)(IO *io, IoCompletion *completion, i32 listen_socket,
                  IoCallback cb);
  Error (*connect)(IO *io, IoCompletion *completion, i32 fd, Ipv4Addr addr,
                   IoCallback cb);
  Error (*send_to)(IO *io, IoCompletion *completion, i32 fd, Ipv4Addr addr,
                   Slice_u8 data, IoCallback cb);
  Error (*file_size)(IO *io, IoCompletion *completion, i32 fd, IoCallback cb);
  Error (*remove_file)(IO *io, IoCompletion *completion, Slice_u8 path,
                       IoCallback cb);
};

// Which multiplexer an `IO` is built on. A platform may have more than one, and
// they are not interchangeable in what they cost or in what they can do, only
// in what they promise: whichever one a caller picks, the `IO` above it behaves
// the same.
typedef enum {
  // Whatever the platform's own answer is. The only thing a caller with no
  // reason to prefer one should ask for.
  IoBackendDefault,
  // Darwin.
  IoBackendKqueue,
  // Linux. `IoBackendEpoll` is readiness, like kqueue: it says a descriptor is
  // ready and this process makes the syscall. `IoBackendIoUring` is not -- the
  // kernel makes the syscall -- so it is a different implementation and not a
  // different `#ifdef` inside the same one.
  IoBackendEpoll,
  IoBackendIoUring,
  // Win32. Like io_uring and unlike the other two, the kernel makes the
  // syscall, so it is a completion port and not a readiness loop.
  IoBackendIocp,
} IoBackend;

__attribute__((warn_unused_result)) static const char *
io_backend_to_cstr(IoBackend backend) {
  switch (backend) {
  case IoBackendDefault:
    return "default";
  case IoBackendKqueue:
    return "kqueue";
  case IoBackendEpoll:
    return "epoll";
  case IoBackendIoUring:
    return "io_uring";
  case IoBackendIocp:
    return "iocp";
  }

  assert(0 && "unreachable");
  return "?";
}

// On success `*dst` is the implementation, allocated out of `arena` and holding
// on to `env`, so both have to outlive it. A `backend` this platform was not
// built with is `ErrKindUnsupported`, which is the whole of what a caller has
// to handle to ask for one.
//
// Allocated, and not a value handed back the way an `Env` is, because an `IO`
// is an object and not just a vtable: the Darwin one owns a kqueue and the list
// of changes waiting to go to it. Its size is a perfectly ordinary compile-time
// constant -- but a per-platform one, and naming the platform's type is exactly
// what a caller holding an `IO *` must not do. So whoever knows the size does
// the allocating, out of an arena the caller owns, which also leaves the caller
// free to make more than one: `env_platform_make`'s trick of returning a
// pointer to a `static` would not do here, because two of these must not share
// a kqueue.
__attribute__((warn_unused_result)) static Error
io_platform_make(Arena *arena, const Env *env, IoBackend backend, IO **dst);

// ---------- IO: waiting for one thing ----------

// Turn the loop until `*done`. The server proper never waits like this: it has
// other connections to get on with, and its own loop is the one below in
// `main`. The startup path has nothing to do until the file it was pointed at
// is mapped, and a test checking what one syscall reports has nothing to do
// either.
__attribute__((warn_unused_result)) static Error
io_run_until(IO *io, const bool *done, usize tick_ns) {
  assert(io);
  assert(done);
  assert(tick_ns > 0);

  while (!*done) {
    const Error err = io->run_for_ns(io, tick_ns);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  return (Error){.kind = ErrKindNone};
}

// A tenth of a second: long enough that a wait for a peer is not a hundred
// syscalls a second, short enough that a caller turning the loop by hand is
// not left staring at it.
#define IO_TICK_NS (100 * Millisecond)

// One operation, waited out. The completion, what it answered, and the flag
// that says it has, in one place, because a caller that waits for a single
// operation needs all three and nothing else.
typedef struct {
  IoCompletion completion;
  Error err;
  usize res;
  bool done;
} IoOnce;

static void io_once_on_done(IoCompletion *completion, Error err, usize res) {
  assert(completion);

  IoOnce *const once = completion->ctx;
  assert(once);
  assert(!once->done);

  once->err = err;
  once->res = res;
  once->done = true;
}

static void io_once_init(IoOnce *once) {
  assert(once);

  *once = (IoOnce){0};
  once->completion.ctx = once;
}

// Wait for the operation submitted on `once`, and report what it answered
// rather than what the waiting itself did: a caller that cannot turn the loop
// has bigger trouble than the operation, and gets that error instead.
__attribute__((warn_unused_result)) static Error io_once_wait(IO *io,
                                                              IoOnce *once) {
  assert(io);
  assert(once);

  const Error err = io_run_until(io, &once->done, IO_TICK_NS);
  if (ErrKindNone != err.kind) {
    return err;
  }

  return once->err;
}

// ---------- IO: mapping a whole file ----------

// `map_file` and `write_all_to_file` are the program's operations and not the
// platform's: one call here is several to the operating system. They are state
// machines over the slots rather than slots of their own, which is also what
// makes them the same code on every platform, and what lets a test fake the
// primitives underneath them without faking a filesystem.
//
// Each step hands the next one the same completion, which the context owns.
typedef struct {
  IO *io;
  IoCompletion completion;
  FileOpenOptions opts;
  Slice_u8 *dst;
  i32 fd;
  // The first failure of the sequence, reported once `done` is set. A later
  // step never overwrites it: the cleanup runs whatever happened, and its own
  // trouble is not what the caller asked about.
  Error err;
  bool done;
} IoMapFile;

static void io_map_file_on_close(IoCompletion *completion, Error err,
                                 usize res) {
  assert(completion);
  (void)res;

  IoMapFile *const ctx = completion->ctx;
  assert(ctx);

  if (ErrKindNone == ctx->err.kind) {
    ctx->err = err;
  }
  ctx->done = true;
}

// Hand the descriptor back, then finish. Reached from every exit after the
// file is open, the failing ones included: the mapping holds its own
// reference to the file, so the descriptor has done its job either way.
static void io_map_file_close(IoMapFile *ctx) {
  assert(ctx);
  assert(ctx->io);
  assert(ctx->fd >= 0);

  const Error err =
      ctx->io->close(ctx->io, &ctx->completion, ctx->fd, io_map_file_on_close);
  if (ErrKindNone != err.kind) {
    // The close was never submitted, so nothing else is going to finish this.
    // The descriptor is leaked, which is the lesser of the two problems.
    if (ErrKindNone == ctx->err.kind) {
      ctx->err = err;
    }
    ctx->done = true;
  }
}

static void io_map_file_on_file_size(IoCompletion *completion, Error err,
                                     usize res) {
  assert(completion);

  IoMapFile *const ctx = completion->ctx;
  assert(ctx);
  assert(ctx->io);

  if (ErrKindNone != err.kind) {
    ctx->err = err;
    io_map_file_close(ctx);
    return;
  }

  const Env *const env = ctx->io->env;
  assert(env);

  ctx->err = env->map_fd(env, ctx->fd, res, ctx->opts, ctx->dst);
  io_map_file_close(ctx);
}

static void io_map_file_on_open(IoCompletion *completion, Error err,
                                usize res) {
  assert(completion);

  IoMapFile *const ctx = completion->ctx;
  assert(ctx);
  assert(ctx->io);

  if (ErrKindNone != err.kind) {
    // There is no descriptor, so there is nothing to hand back.
    ctx->err = err;
    ctx->done = true;
    return;
  }

  ctx->fd = (i32)res;
  assert(ctx->fd >= 0);

  const Error err_size = ctx->io->file_size(ctx->io, &ctx->completion, ctx->fd,
                                            io_map_file_on_file_size);
  if (ErrKindNone != err_size.kind) {
    ctx->err = err_size;
    io_map_file_close(ctx);
  }
}

// Map the whole of `path`. `*ctx` has to outlive the operation; `*dst` is only
// written on success, and `ctx->err` is the answer once `ctx->done`.
__attribute__((warn_unused_result)) static Error
io_map_file(IO *io, IoMapFile *ctx, Slice_u8 path, FileOpenOptions opts,
            Slice_u8 *dst) {
  assert(io);
  assert(ctx);
  assert(dst);

  *ctx = (IoMapFile){.io = io, .opts = opts, .dst = dst, .fd = -1};
  ctx->completion.ctx = ctx;

  // Opened read only whatever the mapping is for: the mapping is private, so
  // a write to it never reaches the file and the descriptor does not need the
  // right to make one.
  return io->open(io, &ctx->completion, path, FileOpenOptionsReadOnly,
                  io_map_file_on_open);
}

// ---------- IO: writing a whole file ----------

typedef struct {
  IO *io;
  IoCompletion completion;
  // What is left to write. A short write is ordinary, so this shrinks a
  // callback at a time rather than in one go.
  Slice_u8 remaining;
  i32 fd;
  Error err;
  bool done;
} IoWriteAllToFile;

static void io_write_all_to_file_on_close(IoCompletion *completion, Error err,
                                          usize res) {
  assert(completion);
  (void)res;

  IoWriteAllToFile *const ctx = completion->ctx;
  assert(ctx);

  if (ErrKindNone == ctx->err.kind) {
    ctx->err = err;
  }
  ctx->done = true;
}

static void io_write_all_to_file_close(IoWriteAllToFile *ctx) {
  assert(ctx);
  assert(ctx->io);
  assert(ctx->fd >= 0);

  const Error err = ctx->io->close(ctx->io, &ctx->completion, ctx->fd,
                                   io_write_all_to_file_on_close);
  if (ErrKindNone != err.kind) {
    if (ErrKindNone == ctx->err.kind) {
      ctx->err = err;
    }
    ctx->done = true;
  }
}

static void io_write_all_to_file_on_write(IoCompletion *completion, Error err,
                                          usize res);

// Submit the next slice of the write, or hand the descriptor back if there is
// none left.
static void io_write_all_to_file_step(IoWriteAllToFile *ctx) {
  assert(ctx);
  assert(ctx->io);

  if (0 == ctx->remaining.len) {
    io_write_all_to_file_close(ctx);
    return;
  }

  const Error err =
      ctx->io->write(ctx->io, &ctx->completion, ctx->fd, ctx->remaining,
                     io_write_all_to_file_on_write);
  if (ErrKindNone != err.kind) {
    ctx->err = err;
    io_write_all_to_file_close(ctx);
  }
}

static void io_write_all_to_file_on_write(IoCompletion *completion, Error err,
                                          usize res) {
  assert(completion);

  IoWriteAllToFile *const ctx = completion->ctx;
  assert(ctx);

  // A signal before any progress is not a failure: reissue the same slice.
  if (ErrKindInterrupted == err.kind) {
    io_write_all_to_file_step(ctx);
    return;
  }

  if (ErrKindNone != err.kind) {
    ctx->err = err;
    io_write_all_to_file_close(ctx);
    return;
  }

  // A short write is ordinary; a write of nothing would go round for ever.
  if (0 == res) {
    ctx->err = (Error){.kind = ErrKindConnReset};
    io_write_all_to_file_close(ctx);
    return;
  }

  assert(res <= ctx->remaining.len);
  slice_u8_advance(&ctx->remaining, res);

  io_write_all_to_file_step(ctx);
}

static void io_write_all_to_file_on_open(IoCompletion *completion, Error err,
                                         usize res) {
  assert(completion);

  IoWriteAllToFile *const ctx = completion->ctx;
  assert(ctx);

  if (ErrKindNone != err.kind) {
    ctx->err = err;
    ctx->done = true;
    return;
  }

  ctx->fd = (i32)res;
  assert(ctx->fd >= 0);

  io_write_all_to_file_step(ctx);
}

// Replace the contents of `path` with `data`. `*ctx` has to outlive the
// operation, and `ctx->err` is the answer once `ctx->done`.
__attribute__((warn_unused_result)) static Error
io_write_all_to_file(IO *io, IoWriteAllToFile *ctx, Slice_u8 path,
                     Slice_u8 data) {
  assert(io);
  assert(ctx);

  *ctx = (IoWriteAllToFile){.io = io, .remaining = data, .fd = -1};
  ctx->completion.ctx = ctx;

  // TODO: Should we still 'touch' the file?
  if (!data.data || 0 == data.len) {
    ctx->done = true;
    return (Error){.kind = ErrKindNone};
  }

  return io->open(io, &ctx->completion, path,
                  FileOpenOptionsWriteOnly | FileOpenOptionsCreate |
                      FileOpenOptionsTruncate,
                  io_write_all_to_file_on_open);
}

// ---------- IO: the file composites, waited out ----------

// The startup path reads the torrent it was pointed at and writes the one it
// produced, and has nothing to do until each is finished. These turn the loop
// on its behalf so that it reads as the sequence it is; nothing that serves a
// peer may use them.
__attribute__((warn_unused_result)) static Error
io_map_file_blocking(IO *io, Slice_u8 path, FileOpenOptions opts,
                     Slice_u8 *dst) {
  assert(io);
  assert(dst);

  IoMapFile ctx = {0};
  const Error err_submit = io_map_file(io, &ctx, path, opts, dst);
  if (ErrKindNone != err_submit.kind) {
    return err_submit;
  }

  const Error err_run = io_run_until(io, &ctx.done, IO_TICK_NS);
  if (ErrKindNone != err_run.kind) {
    return err_run;
  }

  return ctx.err;
}

__attribute__((warn_unused_result)) static Error
io_write_all_to_file_blocking(IO *io, Slice_u8 path, Slice_u8 data) {
  assert(io);

  IoWriteAllToFile ctx = {0};
  const Error err_submit = io_write_all_to_file(io, &ctx, path, data);
  if (ErrKindNone != err_submit.kind) {
    return err_submit;
  }

  const Error err_run = io_run_until(io, &ctx.done, IO_TICK_NS);
  if (ErrKindNone != err_run.kind) {
    return err_run;
  }

  return ctx.err;
}

// ---------- IO: serving TCP ----------

// `accept_socket` belongs to the callback from the moment it is handed over,
// including hanging up on it when the callback itself fails.
typedef void (*AcceptCallback)(IO *io, void *cb_ctx, Ipv4Addr accept_addr,
                               i32 accept_socket);

// A listener and the one accept it always has in flight. Set up synchronously,
// because every step of the setup is an `Env` call that answers on the spot;
// from the first accept on it is callbacks, and `err` and `done` are how the
// end of it is reported.
typedef struct {
  IO *io;
  IoCompletion completion;
  void *cb_ctx;
  AcceptCallback on_accept;
  i32 listen_socket;
  // Why the listener is not running. Set before `done` ever is, so a caller
  // that turns the loop until `done` always has the reason waiting for it.
  Error err;
  // Nothing of this listener's is in flight any more: it either never started
  // or it has stopped and hung up on its own socket. Not "idle" -- a running
  // listener always has an accept outstanding and never sets this.
  bool done;
} IoServer;

static void io_server_on_accept(IoCompletion *completion, Error err, usize res);

// Wait for the next connection. The listener holds exactly one accept at a
// time: a second would be a second completion, and one connection arriving at
// a time is all a single thread can do anything with.
static void io_server_arm(IoServer *server);

static void io_server_on_close(IoCompletion *completion, Error err, usize res) {
  assert(completion);
  (void)err;
  (void)res;

  IoServer *const server = completion->ctx;
  assert(server);

  // Whatever hanging up on the listener reported is of no interest: the
  // listener is already gone, and `server->err` is why.
  server->done = true;
}

// Take the listener down, remembering why. The first reason wins: a later
// failure is a consequence of this one.
static void io_server_shutdown(IoServer *server, Error err) {
  assert(server);
  assert(server->io);
  assert(server->listen_socket >= 0);

  if (ErrKindNone == server->err.kind) {
    server->err = err;
  }

  const Error err_close =
      server->io->close(server->io, &server->completion, server->listen_socket,
                        io_server_on_close);
  if (ErrKindNone != err_close.kind) {
    server->done = true;
  }
}

static void io_server_arm(IoServer *server) {
  assert(server);
  assert(server->io);

  const Error err =
      server->io->accept(server->io, &server->completion, server->listen_socket,
                         io_server_on_accept);
  if (ErrKindNone != err.kind) {
    io_server_shutdown(server, err);
  }
}

static void io_server_on_accept(IoCompletion *completion, Error err,
                                usize res) {
  assert(completion);

  IoServer *const server = completion->ctx;
  assert(server);
  assert(server->io);
  assert(server->on_accept);

  // The peer is allowed to vanish between the handshake and the `accept`.
  // That is one dead connection, not a dead server.
  if (ErrKindConnReset == err.kind) {
    io_server_arm(server);
    return;
  }

  if (ErrKindNone != err.kind) {
    // TODO: `ErrKindTooManyFiles` is transient and deserves a backoff instead
    // of tearing the listener down, which needs a timer in `IO`.
    io_server_shutdown(server, err);
    return;
  }

  const i32 accept_socket = (i32)res;
  assert(accept_socket >= 0);

  server->on_accept(server->io, server->cb_ctx,
                    completion->action.v.accept.addr, accept_socket);

  // The callback owns that connection now, whether it managed anything with
  // it or not, so the listener goes straight back to waiting.
  io_server_arm(server);
}

// Bind and start listening, and put the first accept in flight. A non-`None`
// answer means the listener never came up and `on_accept` will never be
// called; the listener then closes itself, so a caller that wants the
// descriptor actually gone turns the loop until `server->done` as usual.
//
// Once this succeeds the listener runs until it cannot: turn the loop, and
// read `server->err` when `server->done`.
__attribute__((warn_unused_result)) static Error
io_listen_and_serve_tcp_ipv4(IO *io, IoServer *server, void *cb_ctx,
                             Ipv4Addr listen_addr, AcceptCallback on_accept) {
  assert(io);
  assert(server);
  assert(on_accept);
  assert(io->env);

  const Env *const env = io->env;

  *server = (IoServer){
      .io = io, .cb_ctx = cb_ctx, .on_accept = on_accept, .listen_socket = -1};
  server->completion.ctx = server;

  {
    const Error err = env->socket(env, SocketDomainIpv4, SocketTypeTcp,
                                  &server->listen_socket);
    if (ErrKindNone != err.kind) {
      // No socket, so nothing to hand back and nothing to wait for.
      server->err = err;
      server->done = true;
      return err;
    }
  }
  assert(server->listen_socket >= 0);

  {
    const Error err = env->enable_socket_reuse(env, server->listen_socket);
    if (ErrKindNone != err.kind) {
      io_server_shutdown(server, err);
      return err;
    }
  }

  {
    // A port left behind by a previous run is an ordinary answer, not a bug in
    // this process, so it travels back as an `Error`.
    const Error err =
        env->tcp_bind_ipv4(env, server->listen_socket, listen_addr);
    if (ErrKindNone != err.kind) {
      io_server_shutdown(server, err);
      return err;
    }
  }

  {
    const Error err = env->listen(env, server->listen_socket, 1024);
    if (ErrKindNone != err.kind) {
      io_server_shutdown(server, err);
      return err;
    }
  }

  io_server_arm(server);

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
arena_valloc(const Env *env, usize bytes_count, Arena *res) {
  assert(env);
  assert(res);

  const usize page_size = env->get_page_size(env);
  assert(page_size > 0);

  const usize usable_bytes = usize_round_up_multiple_of(bytes_count, page_size);
  usize os_alloc_size = 0;
  // Guard page.
  assert(!__builtin_add_overflow(usable_bytes, page_size, &os_alloc_size));

  u8 *arena_memory = NULL;
  {
    const Error err = env->valloc(env, os_alloc_size, &arena_memory);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(arena_memory);

  assert(ErrKindNone ==
         env->vprotect_none(env, arena_memory + usable_bytes, page_size).kind);

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

__attribute__((warn_unused_result)) static bool
sb_append_usize_within_cap(StringBuffer *sb, usize n) {
  assert(sb);

  const usize digits = usize_digits_base_10(n);

  if (sb_space(*sb) < digits) {
    return false;
  }

  const Slice_u8 sb_dst = {
      .data = sb->container.data + sb->len,
      .len = sb_space(*sb),
  };
  assert(digits == encode_usize_base_10(n, sb_dst));

  assert(!__builtin_add_overflow(sb->len, digits, &sb->len));

  return true;
}

static void sha256_encode_hex_trunc(u8 digest[32 /* SHA256_DIGEST_LENGTH */],
                                    u8 dst[40]) {
  const usize trunc = 20;
  const u8 lut[] = "0123456789abcdef";

  for (usize i = 0; i < trunc; i++) {
    const u8 byte = digest[i];
    const u8 c1 = byte & 15; // i.e. `% 16`.
    const u8 c2 = byte >> 4; // i.e. `/ 16`

    dst[i * 2] = lut[c2];
    dst[i * 2 + 1] = lut[c1];
  }
}
