#pragma once

#ifdef WITH_TESTS

#ifdef PLATFORM_UNIX
#include "unix.c"
#endif

#ifdef PLATFORM_WIN32
#include "win32.c"
#endif

// The whole test suite, plus the mock `IO` implementations it runs the real
// code against. Included by `main.c` last, so a test can reach anything the
// program defines.

// Arena memory comes straight from `mmap` and is therefore zeroed, which
// makes a read of never-written memory look like a perfectly valid zeroed
// struct. Poison it so such a read shows up as an obviously bogus value
// instead.
__attribute__((warn_unused_result)) static Arena test_arena(usize bytes_count) {
  const Env *const env = env_platform_make();
  Arena arena = {0};
  assert(ErrKindNone == arena_valloc(env, bytes_count, &arena).kind);
  assert(arena.start);
  assert(arena.end);
  assert((usize)arena.end - (usize)arena.start >= bytes_count);

  memset(arena.start, 0xAA, bytes_count);
  return arena;
}

__attribute__((warn_unused_result)) static BencodeValue
test_bencode_int(isize n) {
  return (BencodeValue){.kind = BencodeKindInteger, .v.num = n};
}

__attribute__((warn_unused_result)) static BencodeValue
test_bencode_bytes(const char *s) {
  return (BencodeValue){.kind = BencodeKindBytes,
                        .v.bytes = bytes_from_cstr(s)};
}

// Is `needle` present in `haystack`? `memmem` is not C99, and an empty needle
// is not a question this asks.
__attribute__((warn_unused_result)) static bool
test_bytes_contains(Bytes haystack, Bytes needle) {
  assert(!bytes_is_empty(needle));

  if (needle.len > haystack.len) {
    return false;
  }

  for (usize i = 0; i + needle.len <= haystack.len; i++) {
    if (0 == memcmp(haystack.data + i, needle.data, needle.len)) {
      return true;
    }
  }

  return false;
}

static void test_char_is_digit_ascii(void) {
  assert(char_is_digit_ascii('0'));
  assert(char_is_digit_ascii('5'));
  assert(char_is_digit_ascii('9'));

  assert(!char_is_digit_ascii('0' - 1));
  assert(!char_is_digit_ascii('9' + 1));
  assert(!char_is_digit_ascii(0));
  assert(!char_is_digit_ascii(255));
}

static void test_isize_from_usize(void) {
  isize res = 0;

  // Positive.
  assert(ErrKindNone == isize_from_usize(0, false, &res).kind);
  assert(0 == res);
  assert(ErrKindNone == isize_from_usize(123, false, &res).kind);
  assert(123 == res);
  assert(ErrKindNone == isize_from_usize((usize)SSIZE_MAX, false, &res).kind);
  assert(SSIZE_MAX == res);
  assert(ErrKindNone !=
         isize_from_usize((usize)SSIZE_MAX + 1, false, &res).kind);
  assert(ErrKindNone != isize_from_usize(SIZE_MAX, false, &res).kind);

  // Negative. `|ISIZE_MIN|` is one greater than `ISIZE_MAX`.
  assert(ErrKindNone == isize_from_usize(0, true, &res).kind);
  assert(0 == res);
  assert(ErrKindNone == isize_from_usize(123, true, &res).kind);
  assert(-123 == res);
  assert(ErrKindNone == isize_from_usize((usize)SSIZE_MAX, true, &res).kind);
  assert(-SSIZE_MAX == res);
  assert(ErrKindNone ==
         isize_from_usize((usize)SSIZE_MAX + 1, true, &res).kind);
  assert((-SSIZE_MAX - 1) == res);
  assert(ErrKindNone !=
         isize_from_usize((usize)SSIZE_MAX + 2, true, &res).kind);
  assert(ErrKindNone != isize_from_usize(SIZE_MAX, true, &res).kind);
}

static void test_usize_round_up_multiple_of(void) {
  // A multiple of 1 rounds nothing.
  assert(0 == usize_round_up_multiple_of(0, 1));
  assert(1 == usize_round_up_multiple_of(1, 1));
  assert(SIZE_MAX == usize_round_up_multiple_of(SIZE_MAX, 1));

  // Zero is already a multiple of everything.
  assert(0 == usize_round_up_multiple_of(0, 8));
  assert(0 == usize_round_up_multiple_of(0, 16384));

  // Exact multiples are left alone.
  assert(8 == usize_round_up_multiple_of(8, 8));
  assert(16 == usize_round_up_multiple_of(16, 8));
  assert(16384 == usize_round_up_multiple_of(16384, 16384));
  assert(32768 == usize_round_up_multiple_of(32768, 16384));

  // Everything else goes up to the next one.
  assert(8 == usize_round_up_multiple_of(1, 8));
  assert(8 == usize_round_up_multiple_of(7, 8));
  assert(16 == usize_round_up_multiple_of(9, 8));
  assert(16384 == usize_round_up_multiple_of(1, 16384));
  assert(16384 == usize_round_up_multiple_of(1 * KiB, 16384));
  assert(16384 == usize_round_up_multiple_of(16383, 16384));
  assert(32768 == usize_round_up_multiple_of(16385, 16384));

  // The largest input that does not overflow `n + multiple - 1`, which is
  // itself an exact multiple.
  assert(SIZE_MAX - 8191 == usize_round_up_multiple_of(SIZE_MAX - 8191, 8192));

  // The postcondition holds for every power of two, and rounding an already
  // rounded value changes nothing.
  for (usize multiple = 1; multiple <= ((usize)1 << 20); multiple *= 2) {
    for (usize n = 0; n < 4 * multiple; n += (multiple / 4) + 1) {
      const usize res = usize_round_up_multiple_of(n, multiple);

      assert(res >= n);
      assert(res - n < multiple);
      assert(0 == (res & (multiple - 1)));
      assert(res == usize_round_up_multiple_of(res, multiple));
    }
  }
}

// The length prefix every peer message carries. Its counterpart on the read
// side is `bytes_consume_u32_be`, so the two are checked against each other
// as well as against bytes written out by hand.
static void test_u8_write_u32_be(void) {
  // The most significant byte first, whatever the host thinks of that order.
  {
    u8 dst[4] = {0xaa, 0xaa, 0xaa, 0xaa};
    u8_write_u32_be(dst, 0x01020304);
    assert(0x01 == dst[0]);
    assert(0x02 == dst[1]);
    assert(0x03 == dst[2]);
    assert(0x04 == dst[3]);
  }

  // A length of one, which is what `interested`, `unchoke` and their two
  // neighbours carry. Three zero bytes and then the one: written the other way
  // round it is a keep-alive followed by a length in the tens of millions, and
  // a peer answers that with `packet too large`.
  {
    u8 dst[4] = {0xaa, 0xaa, 0xaa, 0xaa};
    u8_write_u32_be(dst, TORRENT_PEER_MSG_EMPTY_SIZE);
    assert(0x00 == dst[0]);
    assert(0x00 == dst[1]);
    assert(0x00 == dst[2]);
    assert(0x01 == dst[3]);
  }

  // The ends of the range, and nothing written past the four bytes.
  {
    const u32 values[] = {0,          1,          0xff,       0x100,
                          0xffff,     0x10000,    0x00ff00ff, 0xffffff00,
                          0xfffffffe, 0xffffffff};

    for (usize i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
      u8 dst[6] = {0xaa, 0, 0, 0, 0, 0xaa};
      u8_write_u32_be(dst + 1, values[i]);

      // Read back by the parser's own helper: the two have to be inverses, or
      // this process cannot read what it just sent.
      Bytes read = bytes_make(dst + 1, 4);
      u32 got = 0;
      assert(bytes_consume_u32_be(&read, &got));
      assert(values[i] == got);
      assert(0 == read.len);

      // The guard bytes on either side.
      assert(0xaa == dst[0]);
      assert(0xaa == dst[5]);
    }
  }
}

static void test_next_power_of_two(void) {
  assert(1 == next_power_of_two(0));
  assert(1 == next_power_of_two(1));
  assert(2 == next_power_of_two(2));
  assert(4 == next_power_of_two(3));
  assert(4 == next_power_of_two(4));
  assert(8 == next_power_of_two(5));
  assert(8 == next_power_of_two(8));
  assert(16 == next_power_of_two(9));

  // Block counts, the reason this exists.
  assert(1024 == next_power_of_two(1024));
  assert(2048 == next_power_of_two(1025));

  // The largest representable power of two, and the largest input that still
  // has one.
  const usize max_power_of_two = SIZE_MAX / 2 + 1;
  assert(max_power_of_two == next_power_of_two(max_power_of_two));
  assert(max_power_of_two == next_power_of_two(max_power_of_two - 1));

  // A power of two is left alone; one past it goes up to the next.
  for (usize p = 1; p <= ((usize)1 << 20); p *= 2) {
    assert(p == next_power_of_two(p));
    assert(2 * p == next_power_of_two(p + 1));
  }

  // The postcondition holds everywhere, and rounding an already rounded value
  // changes nothing.
  for (usize n = 0; n < 4096; n++) {
    const usize res = next_power_of_two(n);

    assert(res >= n);
    assert(0 == (res & (res - 1)));
    if (1 != res) {
      assert(res / 2 < n);
    }
    assert(res == next_power_of_two(res));
  }
}

static void test_arena_alloc(void) {
  // Alignment: a 1 byte allocation leaves the head misaligned, the next
  // 8-aligned allocation has to round up over 7 bytes of padding.
  {
    Arena arena = test_arena(1 * KiB);

    u8 *const a = arena_alloc(&arena, 1, sizeof(u8), 1);
    assert(a);
    assert(0 != ((usize)arena.start % 8));

    void *const b = arena_alloc(&arena, 8, 8, 1);
    assert(b);
    assert(0 == ((usize)b % 8));
    assert((usize)b == (usize)a + 8);
  }
  // An allocation that exactly fills the arena is legal; the next one is not.
  {
    Arena arena = test_arena(64);

    void *const p = arena_alloc(&arena, 8, 8, 8);
    assert(p);
    assert(arena.start == arena.end);

    assert(NULL == arena_alloc(&arena, 1, sizeof(u8), 1));
  }
  // OOM leaves the arena untouched.
  {
    Arena arena = test_arena(64);
    u8 *const start_before = arena.start;

    assert(NULL == arena_alloc(&arena, 1, sizeof(u8), 65));
    assert(arena.start == start_before);

    // Still usable afterwards.
    assert(arena_alloc(&arena, 1, sizeof(u8), 64));
    assert(arena.start == arena.end);
  }
  // Multiple allocations are contiguous when alignment allows it.
  {
    Arena arena = test_arena(1 * KiB);

    BencodeValue *const a =
        arena_alloc(&arena, __alignof__(BencodeValue), sizeof(BencodeValue), 2);
    BencodeValue *const b =
        arena_alloc(&arena, __alignof__(BencodeValue), sizeof(BencodeValue), 3);
    assert(a);
    assert(b);
    assert(b == a + 2);
    assert((usize)arena.start == (usize)a + 5 * sizeof(BencodeValue));
  }
}

#if defined(PLATFORM_UNIX)
// Every `errno` the syscalls this program makes are documented to set, and
// what each one is supposed to come back as. A value landing in the
// `ErrInvalidData` default by accident rather than on purpose is exactly the
// kind of thing that goes unnoticed, so list them explicitly.
//
// Unlike every other test here this one names a platform function rather than
// a vtable slot, because the mapping is what it is checking, so it exists only
// on the platform that has one.
static void test_unix_error_from_errno(void) {
  const struct {
    i32 errno_value;
    ErrorKind expected;
  } cases[] = {
      // Permission.
      {EACCES, ErrOSKindPermission},
      {EPERM, ErrOSKindPermission},

      // Out of memory, including the socket buffer flavour.
      {ENOMEM, ErrKindOOM},
      {ENOBUFS, ErrKindOOM},

      {EOVERFLOW, ErrKindRange},
      {EADDRINUSE, ErrKindAddrInUse},
      {EAGAIN, ErrKindAgain},
      {EWOULDBLOCK, ErrKindAgain},
      {EINTR, ErrKindInterrupted},

      // Every way a peer can vanish.
      {ECONNABORTED, ErrKindConnReset},
      {ECONNRESET, ErrKindConnReset},
      {EPIPE, ErrKindConnReset},

      // Descriptor exhaustion, per process and system wide.
      {EMFILE, ErrKindTooManyFiles},
      {ENFILE, ErrKindTooManyFiles},

      // Unreachable is its own answer: the call was well formed, the
      // destination just could not be reached.
      {EHOSTUNREACH, ErrKindHostUnreachable},

      // Calling the kernel wrong, from every syscall in use: bad descriptor,
      // not a socket, wrong family or protocol, unsupported operation,
      // misaligned address, no such file.
      {EBADF, ErrKindInvalidData},
      {ENOTSOCK, ErrKindInvalidData},
      {EAFNOSUPPORT, ErrKindInvalidData},
      {EPROTOTYPE, ErrKindInvalidData},
      {EPROTONOSUPPORT, ErrKindInvalidData},
      {EOPNOTSUPP, ErrKindInvalidData},
      {EINVAL, ErrKindInvalidData},
      {ENOTSUP, ErrKindInvalidData},
      {ENODEV, ErrKindInvalidData},
      {ENXIO, ErrKindInvalidData},
      {ENOENT, ErrKindInvalidData},
      {EISDIR, ErrKindInvalidData},

      // Nothing is ever mapped to success.
      {0, ErrKindInvalidData},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    assert(cases[i].expected ==
           unix_error_from_errno(cases[i].errno_value).kind);
    // The `errno` value is preserved verbatim for the caller to render.
    assert((u64)cases[i].errno_value ==
           unix_error_from_errno(cases[i].errno_value).data);
    assert(ErrKindNone != unix_error_from_errno(cases[i].errno_value).kind);
  }

  // `EAGAIN` and `EWOULDBLOCK` are allowed to be the same value; whether they
  // are or not, both have to answer `ErrAgain`.
  assert(ErrKindAgain == unix_error_from_errno(EAGAIN).kind);
  assert(ErrKindAgain == unix_error_from_errno(EWOULDBLOCK).kind);
}
#endif // PLATFORM_UNIX

// ---------- The fake `Env` ----------
//
// An `Env` is a vtable and nothing else -- the real one is stateless -- so a
// fake is a value on the test's own stack with `ctx` pointing at whatever it
// needs to remember. Anything it does not fake is delegated to `real` rather
// than reimplemented, so the code under test still gets memory it can write to
// and descriptors it can use.

// A fake `Env` for the arena. It records what `arena_valloc` asked the OS for,
// answers with a page size the host does not have, and can make the mapping
// fail on demand: the real `mmap` only fails for sizes so large that the
// request says nothing about *which* error comes back.
typedef struct {
  const Env *real;
  usize page_size;

  // When set, `valloc` fails with this instead of mapping.
  ErrorKind alloc_fails_with;

  // What the arena actually asked for.
  usize alloc_calls;
  usize alloc_bytes_count;
  usize protect_calls;
  void *protect_ptr;
  usize protect_size;
} TestAllocCtx;

__attribute__((warn_unused_result)) static usize
test_alloc_get_page_size(const Env *env) {
  TestAllocCtx *const c = env->ctx;
  assert(c);

  return c->page_size;
}

__attribute__((warn_unused_result)) static Error
test_alloc_valloc(const Env *env, usize bytes_count, u8 **res) {
  TestAllocCtx *const c = env->ctx;
  assert(c);

  c->alloc_calls += 1;
  c->alloc_bytes_count = bytes_count;

  if (ErrKindNone != c->alloc_fails_with) {
    return (Error){.kind = c->alloc_fails_with};
  }

  return c->real->valloc(c->real, bytes_count, res);
}

__attribute__((warn_unused_result)) static Error
test_alloc_vprotect_none(const Env *env, void *ptr, usize size) {
  TestAllocCtx *const c = env->ctx;
  assert(c);

  c->protect_calls += 1;
  c->protect_ptr = ptr;
  c->protect_size = size;

  return c->real->vprotect_none(c->real, ptr, size);
}

// Only the slots `arena_valloc` reaches for; the rest stay null so that a call
// to any of them crashes rather than silently doing something real.
__attribute__((warn_unused_result)) static Env
test_env_alloc_make(TestAllocCtx *ctx) {
  assert(ctx);

  ctx->real = env_platform_make();

  return (Env){
      .get_page_size = test_alloc_get_page_size,
      .valloc = test_alloc_valloc,
      .vprotect_none = test_alloc_vprotect_none,
      .ctx = ctx,
  };
}

// The arithmetic around the guard page, against a page size the host does not
// use: on this machine `sysconf` reports 16 KiB, so rounding bugs that happen
// to be invisible at that size would otherwise never show up.
static void test_arena_valloc_mocked(void) {
  const usize page_size = 64 * KiB;

  // A single byte still costs a whole page, plus a whole page of guard.
  {
    TestAllocCtx ctx = {.page_size = page_size};
    const Env env = test_env_alloc_make(&ctx);
    Arena arena = {0};

    assert(ErrKindNone == arena_valloc(&env, 1, &arena).kind);

    assert(1 == ctx.alloc_calls);
    assert(2 * page_size == ctx.alloc_bytes_count);

    // The guard is exactly one page, and it begins where the arena ends:
    // that adjacency is the whole point of right-aligning the arena.
    assert(1 == ctx.protect_calls);
    assert(page_size == ctx.protect_size);
    assert((void *)arena.end == ctx.protect_ptr);

    // The usable bytes are the mapping minus the guard.
    assert((usize)(arena.end - arena.start) >= 1);
    assert((usize)(arena.end - arena.start) <= page_size);
  }

  // One byte past a page rounds up to two, so three pages are mapped.
  {
    TestAllocCtx ctx = {.page_size = page_size};
    const Env env = test_env_alloc_make(&ctx);
    Arena arena = {0};

    assert(ErrKindNone == arena_valloc(&env, page_size + 1, &arena).kind);
    assert(3 * page_size == ctx.alloc_bytes_count);
    assert((void *)arena.end == ctx.protect_ptr);
  }

  // An exact multiple is not rounded up past itself.
  {
    TestAllocCtx ctx = {.page_size = page_size};
    const Env env = test_env_alloc_make(&ctx);
    Arena arena = {0};

    assert(ErrKindNone == arena_valloc(&env, 2 * page_size, &arena).kind);
    assert(3 * page_size == ctx.alloc_bytes_count);
  }

  // A failed mapping is reported, not asserted, and leaves the caller's arena
  // untouched. Nothing is protected either: there is no mapping to protect.
  {
    TestAllocCtx ctx = {.page_size = page_size, .alloc_fails_with = ErrKindOOM};
    const Env env = test_env_alloc_make(&ctx);
    Arena arena = {.start = (u8 *)0xAA, .end = (u8 *)0xBB};

    assert(ErrKindOOM == arena_valloc(&env, 1, &arena).kind);
    assert(1 == ctx.alloc_calls);
    assert(0 == ctx.protect_calls);
    assert((u8 *)0xAA == arena.start);
    assert((u8 *)0xBB == arena.end);
  }

  // The reason is passed through rather than flattened into `ErrOOM`: the
  // real `mmap` can only be provoked into `ENOMEM`, so this is the only way
  // to check that the error travels verbatim.
  {
    TestAllocCtx ctx = {.page_size = page_size,
                        .alloc_fails_with = ErrOSKindPermission};
    const Env env = test_env_alloc_make(&ctx);
    Arena arena = {0};

    assert(ErrOSKindPermission == arena_valloc(&env, 1, &arena).kind);
    assert(NULL == arena.start);
  }
}

// The server narrates to stdout. That is the point in production and noise in
// a test, more so at a thousand connections, so it is swallowed for the
// duration.
//
// The real platform, never the `io` the test itself is driving: the tests that
// want quiet are exactly the ones running against a mock, and redirecting this
// process's output is the harness's own business either way.
__attribute__((warn_unused_result)) static i32 test_stdout_silence(void) {
  const Env *const env = env_platform_make();

  i32 saved = -1;
  assert(ErrKindNone == env->stdout_silence(env, &saved).kind);

  return saved;
}

static void test_stdout_restore(i32 saved) {
  const Env *const env = env_platform_make();

  assert(ErrKindNone == env->stdout_restore(env, saved).kind);
}

// ---------- The fake `IO` ----------
//
// A scripted `IO`, shared by every test that drives one. It keeps the same
// discipline as the real implementation -- an operation is submitted, and its
// callback runs from inside `run_for_ns` -- because what is under test is a
// chain of callbacks, and a fake that answered on the spot would exercise none
// of the chaining and would recurse as deep as the chain is long.
//
// What it does not keep is a kernel: where the real implementation hands the
// submitted operations to kqueue and gets them back, this holds them in an
// array of its own.

typedef struct TestIo TestIo;

// How a script answers one operation.
typedef enum {
  // `*dst_err` and `*dst_res` are the answer, and the callback runs with them.
  TestIoPerformDone,
  // The operation is never answered, the way a peer that has gone quiet never
  // answers: the completion is dropped and its callback never runs. The only
  // way for a test to hold a connection open, which is what filling the peer
  // pool needs.
  TestIoPerformParked,
} TestIoPerformResult;

typedef TestIoPerformResult (*TestIoPerform)(TestIo *test_io,
                                             IoCompletion *completion, i32 fd,
                                             Error *dst_err, usize *dst_res);

// Enough for every slot of the peer pool to have an operation in flight, plus
// the listener's own.
#define TEST_IO_IN_FLIGHT_MAX (TORRENT_PEERS_MAX + 16)

struct TestIo {
  // Must be first: a `TestIo *` is an `IO *`, the same arrangement the real
  // implementation uses to find itself from a slot.
  IO io;

  TestIoPerform perform;
  // The script's own state, whatever it is.
  void *script;

  // What the clock answers. A field and not a syscall, so a test moves time by
  // assigning to it: "the connection closes after four minutes of silence" is
  // then three lines and no waiting, which is better than anything a real timer
  // operation would have given.
  u64 now_ns;

  // When set, submitting an operation of this kind fails rather than being
  // taken. Every caller of a slot has a branch for that -- the callback will
  // never run, so whatever it would have cleaned up has to be cleaned up on the
  // spot -- and this is the only way to reach it.
  IoActionKind submit_fails_for;
  ErrorKind submit_fails_with;

  // Submitted, not yet answered, each with the descriptor it was submitted
  // against: a completion does not carry one, so whoever is holding the
  // operation has to, and here that is this.
  IoCompletion *submitted[TEST_IO_IN_FLIGHT_MAX];
  i32 submitted_fd[TEST_IO_IN_FLIGHT_MAX];
  usize submitted_len;
  // What the turn in progress is working through. A second pair of arrays and
  // not an index into the first, because a callback may submit while it runs
  // and that has to go somewhere that is not being iterated.
  IoCompletion *running[TEST_IO_IN_FLIGHT_MAX];
  i32 running_fd[TEST_IO_IN_FLIGHT_MAX];
};

__attribute__((warn_unused_result)) static Error
test_io_submit(IO *io, IoCompletion *completion, i32 fd, IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);

  TestIo *const test_io = (TestIo *)io;
  assert(test_io->perform);

  if (IoActionKindNone != test_io->submit_fails_for &&
      completion->action.kind == test_io->submit_fails_for) {
    return (Error){.kind = test_io->submit_fails_with};
  }

  // One operation per completion, which every backend relies on: the Darwin one
  // keys its `EVFILT_USER` on the completion's address, so a second submission
  // is folded into the first by kqueue and its in-flight count never comes back
  // down. A close landing on a completion that still holds a read is worse
  // again: the slot goes back to the pool with a registration still pointing
  // into it.
  for (usize i = 0; i < test_io->submitted_len; i++) {
    assert(test_io->submitted[i] != completion &&
           "one operation per completion");
  }

  if (test_io->submitted_len >= TEST_IO_IN_FLIGHT_MAX) {
    // The same backpressure the real implementation reports when it has no
    // room left to take an operation.
    return (Error){.kind = ErrKindAgain};
  }

  completion->cb = cb;
  test_io->submitted[test_io->submitted_len] = completion;
  test_io->submitted_fd[test_io->submitted_len] = fd;
  test_io->submitted_len += 1;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error test_io_run_for_ns(IO *io,
                                                                    usize ns) {
  assert(io);
  (void)ns;

  TestIo *const test_io = (TestIo *)io;
  assert(test_io->perform);

  // What was submitted before this turn began, for the same reason the real
  // implementation works off what the kernel had already taken: a callback may
  // submit more, and that waits for the next turn.
  const usize running_len = test_io->submitted_len;
  memcpy(test_io->running, test_io->submitted,
         running_len * sizeof(test_io->running[0]));
  memcpy(test_io->running_fd, test_io->submitted_fd,
         running_len * sizeof(test_io->running_fd[0]));
  test_io->submitted_len = 0;

  for (usize i = 0; i < running_len; i++) {
    IoCompletion *const completion = test_io->running[i];
    assert(completion);
    assert(completion->cb);

    Error err = {.kind = ErrKindNone};
    usize res = 0;
    if (TestIoPerformParked == test_io->perform(test_io, completion,
                                                test_io->running_fd[i], &err,
                                                &res)) {
      continue;
    }

    completion->cb(completion, err, res);
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static u64 test_io_monotonic_ns(IO *io) {
  assert(io);

  const TestIo *const test_io = (const TestIo *)io;
  return test_io->now_ns;
}

// The slots, which do between them exactly what the real ones do: record what
// was asked for, and ask for a turn of the loop.

__attribute__((warn_unused_result)) static Error
test_io_open(IO *io, IoCompletion *completion, Bytes path,
             FileOpenOptions options, IoCallback cb) {
  assert(completion);

  completion->action = (IoAction){.kind = IoActionKindOpen,
                                  .v.open = {.path = path, .options = options}};

  return test_io_submit(io, completion, -1, cb);
}

__attribute__((warn_unused_result)) static Error
test_io_close(IO *io, IoCompletion *completion, i32 fd, IoCallback cb) {
  assert(completion);

  completion->action = (IoAction){.kind = IoActionKindClose};

  return test_io_submit(io, completion, fd, cb);
}

__attribute__((warn_unused_result)) static Error
test_io_read(IO *io, IoCompletion *completion, i32 fd, Bytes data,
             IoCallback cb) {
  assert(completion);
  assert(data.data);
  assert(data.len > 0);

  completion->action =
      (IoAction){.kind = IoActionKindRead, .v.read.data = data};

  return test_io_submit(io, completion, fd, cb);
}

__attribute__((warn_unused_result)) static Error
test_io_write(IO *io, IoCompletion *completion, i32 fd, Bytes data,
              IoCallback cb) {
  assert(completion);
  assert(data.data);
  assert(data.len > 0);

  completion->action =
      (IoAction){.kind = IoActionKindWrite, .v.write.data = data};

  return test_io_submit(io, completion, fd, cb);
}

__attribute__((warn_unused_result)) static Error
test_io_accept(IO *io, IoCompletion *completion, i32 listen_socket,
               IoCallback cb) {
  assert(completion);

  completion->action = (IoAction){.kind = IoActionKindAccept};

  return test_io_submit(io, completion, listen_socket, cb);
}

__attribute__((warn_unused_result)) static Error
test_io_file_size(IO *io, IoCompletion *completion, i32 fd, IoCallback cb) {
  assert(completion);

  completion->action = (IoAction){.kind = IoActionKindFileSize};

  return test_io_submit(io, completion, fd, cb);
}

// `*test_io` has to outlive every operation submitted to it, and `env` has to
// outlive `*test_io`: a composite reaches the platform's `mmap` through
// `io->env`, and the fakes reach the real syscalls the same way the real
// implementation does.
static void test_io_make(TestIo *test_io, const Env *env, TestIoPerform perform,
                         void *script) {
  assert(test_io);
  assert(env);
  assert(perform);

  memset(test_io, 0, sizeof(*test_io));
  test_io->perform = perform;
  test_io->script = script;
  test_io->io = (IO){
      .env = env,
      .run_for_ns = test_io_run_for_ns,
      .monotonic_ns = test_io_monotonic_ns,
      .open = test_io_open,
      .close = test_io_close,
      .read = test_io_read,
      .write = test_io_write,
      .accept = test_io_accept,
      .file_size = test_io_file_size,
      // `connect`, `send_to` and `remove_file` stay null: no test drives one,
      // and a null slot crashes rather than quietly doing something real.
  };
}

// More turns than draining anything here can need. A turn answers every
// operation the fake is holding, so the only reason to need another is a
// callback that submitted one, and nothing under test chains deeper than the
// pool is wide.
#define TEST_IO_DRAIN_TURNS_MAX (4 * TEST_IO_IN_FLIGHT_MAX)

// Turn the loop until nothing is outstanding. A parked operation is dropped
// rather than answered, so a script that parks reaches this too.
static void test_io_drain(TestIo *test_io) {
  assert(test_io);

  bool drained = false;
  for (usize i = 0; i < TEST_IO_DRAIN_TURNS_MAX; i++) {
    if (0 == test_io->submitted_len) {
      drained = true;
      break;
    }

    const Error err = test_io->io.run_for_ns(&test_io->io, 1);
    assert(ErrKindNone == err.kind);
  }

  // A drain that does not finish is a chain of callbacks that never ends, which
  // is a bug in what is under test and not a reason to turn the loop again.
  assert(drained);
}

// ---------- Peer handshakes ----------

// The 68 bytes a peer opens with, spelled out rather than taken from
// `TORRENT_PEER_HANDSHAKE_LEN`: the wire format is fixed, so a change to that
// constant is a bug and not something the tests should follow.
#define TEST_HANDSHAKE_LEN (1 + 19 + 8 + 20 + 20)
_Static_assert(TEST_HANDSHAKE_LEN == TORRENT_PEER_HANDSHAKE_LEN,
               "the code and the wire format have to agree");

// The bytes of a well-formed handshake for `info_hash`, sent by `peer_id`.
// What a connection says the moment the other side's handshake lands: ours
// went out first, then `interested` and `unchoke`, each a length prefix of one
// and its tag. Spelled out rather than taken from the code that writes them,
// because the bug this guards against was a length prefix written with
// `memset`, which put four zero bytes there and turned the tag that followed
// into the top byte of the next message's length.
static const u8 test_peer_greeting[] = {0x00, 0x00, 0x00, 0x01,
                                        TorrentMessageKindInterested,
                                        0x00, 0x00, 0x00, 0x01,
                                        TorrentMessageKindUnchoke};
#define TEST_PEER_GREETING_LEN (TEST_HANDSHAKE_LEN + 10)
_Static_assert(TEST_PEER_GREETING_LEN ==
                   TEST_HANDSHAKE_LEN + sizeof(test_peer_greeting),
               "the two have to agree");

static void test_handshake_fill(u8 dst[TEST_HANDSHAKE_LEN], Bytes info_hash,
                                Bytes peer_id) {
  assert(dst);
  assert(20 == info_hash.len);
  assert(20 == peer_id.len);

  memcpy(dst,
         "\x13"
         "BitTorrent protocol",
         20);
  memset(dst + 20, 0, 8);
  memcpy(dst + 28, info_hash.data, 20);
  memcpy(dst + 48, peer_id.data, 20);
}

// ---------- The TCP server ----------

// The script behind the listener tests. It is split the way the code under test
// is: setting a listener up is `Env` -- a socket, a socket option, a bind, a
// listen, none of which waits for anything -- and everything from the first
// accept on is `IO`.
//
// `accept` is told in advance how many connections to hand over before it
// stops: the listener goes back for another one for as long as it is answered,
// so without an end there is no way to call it from a test at all.
typedef struct {
  // Failure injection, one per thing that can fail. `ErrKindNone` succeeds.
  ErrorKind socket_fails_with;
  ErrorKind reuse_fails_with;
  ErrorKind bind_fails_with;
  ErrorKind listen_fails_with;
  ErrorKind read_fails_with;

  // `accept` succeeds this many times, then reports `accept_ends_with` to bring
  // the listener down. The call at `conn_reset_at` (1-based, 0 for never)
  // reports a reset instead, which the listener is meant to shrug off.
  usize accept_success_max;
  usize conn_reset_at;
  ErrorKind accept_ends_with;
  usize accept_handed_over;

  // Leave every read unanswered, the way a peer that connects and then says
  // nothing does. The only way to hold connections open, and so the only way to
  // fill the peer pool.
  bool read_parks;

  usize socket_calls;
  usize reuse_calls;
  usize bind_calls;
  usize listen_calls;
  usize accept_calls;
  usize read_calls;
  usize write_calls;
  // `IO`'s close, which is a step in a chain: the listener's own, and each
  // connection's.
  usize close_calls;
  // `Env`'s, which is the hang-up on a connection that was refused.
  usize close_socket_calls;

  Ipv4Addr bound_addr;
  i32 backlog;
} TestServerCtx;

// A recognisable descriptor: a real one would never be this.
#define TEST_SERVER_LISTEN_FD 4242
// What the fake OS picks when asked for port 0.
#define TEST_SERVER_ASSIGNED_PORT 54321

__attribute__((warn_unused_result)) static Error
test_server_socket(const Env *env, SocketDomain domain, SocketType type,
                   i32 *fd) {
  TestServerCtx *const c = env->ctx;
  assert(c);
  assert(fd);
  assert(SocketDomainIpv4 == domain);
  assert(SocketTypeTcp == type);

  c->socket_calls += 1;
  if (ErrKindNone != c->socket_fails_with) {
    return (Error){.kind = c->socket_fails_with};
  }

  *fd = TEST_SERVER_LISTEN_FD;
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
test_server_enable_socket_reuse(const Env *env, i32 fd) {
  TestServerCtx *const c = env->ctx;
  assert(c);
  assert(TEST_SERVER_LISTEN_FD == fd);

  c->reuse_calls += 1;
  return (Error){.kind = c->reuse_fails_with};
}

__attribute__((warn_unused_result)) static Error
test_server_tcp_bind_ipv4(const Env *env, i32 fd, Ipv4Addr *addr) {
  TestServerCtx *const c = env->ctx;
  assert(c);
  assert(addr);
  assert(TEST_SERVER_LISTEN_FD == fd);

  c->bind_calls += 1;
  c->bound_addr = *addr;
  if (ErrKindNone != c->bind_fails_with) {
    return (Error){.kind = c->bind_fails_with};
  }

  if (0 == addr->port) {
    addr->port = TEST_SERVER_ASSIGNED_PORT;
  }
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
test_server_listen(const Env *env, i32 fd, i32 backlog) {
  TestServerCtx *const c = env->ctx;
  assert(c);
  assert(TEST_SERVER_LISTEN_FD == fd);

  c->listen_calls += 1;
  c->backlog = backlog;
  return (Error){.kind = c->listen_fails_with};
}

__attribute__((warn_unused_result)) static Error
test_server_close_socket(const Env *env, i32 fd) {
  TestServerCtx *const c = env->ctx;
  assert(c);
  assert(fd > 0);

  c->close_socket_calls += 1;
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Env
test_env_server_make(TestServerCtx *ctx) {
  assert(ctx);

  return (Env){
      .socket = test_server_socket,
      .enable_socket_reuse = test_server_enable_socket_reuse,
      .tcp_bind_ipv4 = test_server_tcp_bind_ipv4,
      .listen = test_server_listen,
      .close_socket = test_server_close_socket,
      .ctx = ctx,
  };
}

static TestIoPerformResult test_server_perform(TestIo *test_io,
                                               IoCompletion *completion, i32 fd,
                                               Error *dst_err, usize *dst_res) {
  TestServerCtx *const c = test_io->script;
  assert(c);
  assert(completion);

  switch (completion->action.kind) {
  case IoActionKindAccept: {
    assert(TEST_SERVER_LISTEN_FD == fd);
    c->accept_calls += 1;

    if (c->accept_calls == c->conn_reset_at) {
      *dst_err = (Error){.kind = ErrKindConnReset};
      return TestIoPerformDone;
    }

    // A reset hands over nothing, so it does not count against the budget.
    if (c->accept_handed_over >= c->accept_success_max) {
      *dst_err = (Error){.kind = c->accept_ends_with};
      return TestIoPerformDone;
    }
    c->accept_handed_over += 1;

    completion->action.v.accept.addr =
        (Ipv4Addr){.ip = 0x7f000001, .port = 4000};
    *dst_res = 5000 + c->accept_calls;
    return TestIoPerformDone;
  }

  case IoActionKindRead: {
    assert(fd > 0);
    c->read_calls += 1;

    if (c->read_parks) {
      return TestIoPerformParked;
    }
    if (ErrKindNone != c->read_fails_with) {
      *dst_err = (Error){.kind = c->read_fails_with};
      return TestIoPerformDone;
    }

    // A whole handshake's worth of bytes, and a wrong handshake: a connection
    // is then decided by its first read, which keeps these tests about the
    // listener. What the peer does with those bytes is
    // `test_torrent_peer_state_machine`.
    u8 msg[TEST_HANDSHAKE_LEN];
    memset(msg, 'x', sizeof(msg));

    const Bytes data = completion->action.v.read.data;
    assert(data.len >= sizeof(msg));
    memcpy(data.data, msg, sizeof(msg));
    *dst_res = sizeof(msg);
    return TestIoPerformDone;
  }

  // Our own handshake on its way out. It all goes in one go, which is what a
  // socket with room in its send buffer does.
  case IoActionKindWrite:
    assert(fd > 0);
    c->write_calls += 1;
    *dst_res = completion->action.v.write.data.len;
    return TestIoPerformDone;

  case IoActionKindClose:
    assert(fd > 0);
    c->close_calls += 1;
    return TestIoPerformDone;

  case IoActionKindNone:
  case IoActionKindOpen:
  case IoActionKindConnect:
  case IoActionKindSendTo:
  case IoActionKindFileSize:
  case IoActionKindRemoveFile:
    break;
  }

  assert(0 && "the server drives none of these");
  return TestIoPerformDone;
}

// The same info hash as `TEST_LSD_INFOHASH`, in the other representation: the
// 20 raw bytes those 40 hex characters spell. A `TorrentNetworkCtx` always
// carries this form, because that is what a peer handshake compares against.
static u8 test_info_hash_bytes[TORRENT_INFO_HASH_LEN] = {
    0x36, 0x3b, 0x69, 0xd6, 0x6a, 0xd2, 0xd5, 0x7c, 0xbd, 0x51,
    0xc2, 0xf2, 0x71, 0x56, 0xae, 0xf8, 0xd0, 0xe6, 0x8a, 0x70};

// Zero a network context and give it an info hash, since a context without one
// is not a state the program can reach: `main` fills it in before the listener
// ever starts.
static void test_network_ctx_init(TorrentNetworkCtx *network_ctx) {
  assert(network_ctx);

  memset(network_ctx, 0, sizeof(*network_ctx));
  network_ctx->info_hash =
      bytes_make(test_info_hash_bytes, TORRENT_INFO_HASH_LEN);
}

// Run the listener to a standstill and report what stopped it. Every one of
// these tests is the same shape: set the listener up, turn the loop until there
// is nothing left in flight, and look at what was recorded.
__attribute__((warn_unused_result)) static Error
test_server_run(TestIo *test_io, TestServerCtx *ctx, const Env *env,
                IoServer *server, TorrentNetworkCtx *network_ctx,
                Ipv4Addr addr) {
  assert(test_io);
  assert(ctx);
  assert(server);
  assert(network_ctx);

  test_io_make(test_io, env, test_server_perform, ctx);

  const i32 saved = test_stdout_silence();

  const Logger logger = logger_make(LogLevelAll, bytes_from_cstr("[test]"));
  const Error err_listen = io_listen_and_serve_tcp_ipv4(
      &test_io->io, server, network_ctx, addr, &logger, torrent_peer_on_accept);

  // Whether the setup failed or the listener ran and stopped, the loop is
  // turned until nothing is outstanding: a listener that came down still has a
  // hang-up of its own to finish.
  const Error err_run = io_run_until(&test_io->io, &server->done, 1);
  assert(ErrKindNone == err_run.kind);

  // The listener stopping says nothing about the connections it handed over:
  // those have callbacks of their own left to run, and a connection only lets
  // go of its slot in the last of them. Keep turning until the fake has nothing
  // in flight at all, which is also what makes the slot counts below mean
  // anything.
  test_io_drain(test_io);

  test_stdout_restore(saved);

  // The setup reports through the return value and through `server->err`
  // alike, so a test can read either; they agree.
  if (ErrKindNone != err_listen.kind) {
    assert(err_listen.kind == server->err.kind);
  }

  return server->err;
}

// Setting the listener up: each step's failure stops the sequence and travels
// back verbatim, and every failure after the socket exists hands the descriptor
// back.
static void test_io_listen_and_serve_setup_failures(void) {
  const Ipv4Addr addr = {.ip = 0x7f000001, .port = 12345};

  const struct {
    const char *name;
    ErrorKind socket;
    ErrorKind reuse;
    ErrorKind bind;
    ErrorKind listen;
    ErrorKind expected;
    usize expected_closes;
  } cases[] = {
      // No socket, so nothing to close.
      {"socket", ErrKindTooManyFiles, ErrKindNone, ErrKindNone, ErrKindNone,
       ErrKindTooManyFiles, 0},
      {"reuse", ErrKindNone, ErrOSKindPermission, ErrKindNone, ErrKindNone,
       ErrOSKindPermission, 1},
      // The port left behind by a previous run is the expected failure here.
      {"bind", ErrKindNone, ErrKindNone, ErrKindAddrInUse, ErrKindNone,
       ErrKindAddrInUse, 1},
      {"listen", ErrKindNone, ErrKindNone, ErrKindNone, ErrOSKindPermission,
       ErrOSKindPermission, 1},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    TestServerCtx ctx = {
        .socket_fails_with = cases[i].socket,
        .reuse_fails_with = cases[i].reuse,
        .bind_fails_with = cases[i].bind,
        .listen_fails_with = cases[i].listen,
        .accept_ends_with = ErrKindInvalidData,
    };
    const Env env = test_env_server_make(&ctx);
    TestIo test_io = {0};
    IoServer server = {0};
    // `static`: the pool is a couple of megabytes of slots, which is more than
    // a stack frame should hold, and a send buffer per slot doubled it.
    static TorrentNetworkCtx network_ctx;
    test_network_ctx_init(&network_ctx);

    const Error err =
        test_server_run(&test_io, &ctx, &env, &server, &network_ctx, addr);

    assert(cases[i].expected == err.kind);
    assert(cases[i].expected_closes == ctx.close_calls);
    // Nothing past the failing step ran.
    assert(0 == ctx.accept_calls);
  }

  // The listener is bound to what it was asked for, with a backlog.
  {
    TestServerCtx ctx = {.accept_ends_with = ErrKindInvalidData};
    const Env env = test_env_server_make(&ctx);
    TestIo test_io = {0};
    IoServer server = {0};
    // `static`: the pool is a couple of megabytes of slots, which is more than
    // a stack frame should hold, and a send buffer per slot doubled it.
    static TorrentNetworkCtx network_ctx;
    test_network_ctx_init(&network_ctx);

    assert(ErrKindInvalidData ==
           test_server_run(&test_io, &ctx, &env, &server, &network_ctx, addr)
               .kind);

    assert(addr.ip == ctx.bound_addr.ip);
    assert(addr.port == ctx.bound_addr.port);
    assert(addr.ip == server.addr.ip);
    assert(addr.port == server.addr.port);
    assert(ctx.backlog > 0);
    // The listener is closed on the way out.
    assert(1 == ctx.close_calls);
  }

  // Port 0: the port the OS picked is the one the server reports.
  {
    TestServerCtx ctx = {.accept_ends_with = ErrKindInvalidData};
    const Env env = test_env_server_make(&ctx);
    TestIo test_io = {0};
    IoServer server = {0};
    static TorrentNetworkCtx network_ctx;
    test_network_ctx_init(&network_ctx);

    const Ipv4Addr any_port = {.ip = addr.ip, .port = 0};
    assert(ErrKindInvalidData == test_server_run(&test_io, &ctx, &env, &server,
                                                 &network_ctx, any_port)
                                     .kind);

    assert(0 == ctx.bound_addr.port);
    assert(addr.ip == server.addr.ip);
    assert(TEST_SERVER_ASSIGNED_PORT == server.addr.port);
  }
}

// The accept loop and the chain of callbacks behind it.
static void test_io_listen_and_serve_accept(void) {
  const Ipv4Addr addr = {.ip = 0x7f000001, .port = 12345};

  // A peer that vanishes between the handshake and the `accept` is one dead
  // connection, not a dead server: the listener goes back for another. Nothing
  // but a fake can produce that at a chosen moment.
  {
    TestServerCtx ctx = {.accept_success_max = 2,
                         .conn_reset_at = 1,
                         .accept_ends_with = ErrKindInvalidData};
    const Env env = test_env_server_make(&ctx);
    TestIo test_io = {0};
    IoServer server = {0};
    // `static`: the pool is a couple of megabytes of slots, which is more than
    // a stack frame should hold, and a send buffer per slot doubled it.
    static TorrentNetworkCtx network_ctx;
    test_network_ctx_init(&network_ctx);

    assert(ErrKindInvalidData ==
           test_server_run(&test_io, &ctx, &env, &server, &network_ctx, addr)
               .kind);

    // The reset was shrugged off, so both connections still arrived and both
    // were read.
    assert(2 == ctx.read_calls);
    assert(4 == ctx.accept_calls);
    // One per connection, plus the listener itself.
    assert(3 == ctx.close_calls);

    // Every slot was handed back, so the pool is as empty as it started.
    for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
      assert(0 == network_ctx.pool.occupied[i]);
    }
  }

  // A connection whose read cannot even be submitted releases its slot and
  // hangs up, rather than leaking either. The callback never runs in that
  // case, so this is the one path that has to clean up on the spot.
  {
    TestServerCtx ctx = {.accept_success_max = 3,
                         .accept_ends_with = ErrKindInvalidData};
    const Env env = test_env_server_make(&ctx);
    TestIo test_io = {0};
    IoServer server = {0};
    // `static`: the pool is a couple of megabytes of slots, which is more than
    // a stack frame should hold, and a send buffer per slot doubled it.
    static TorrentNetworkCtx network_ctx;
    test_network_ctx_init(&network_ctx);

    test_io_make(&test_io, &env, test_server_perform, &ctx);
    test_io.submit_fails_for = IoActionKindRead;
    test_io.submit_fails_with = ErrKindOOM;

    const i32 saved = test_stdout_silence();
    const Logger logger = logger_make(LogLevelAll, bytes_from_cstr("[test]"));
    const Error err_listen =
        io_listen_and_serve_tcp_ipv4(&test_io.io, &server, &network_ctx, addr,
                                     &logger, torrent_peer_on_accept);
    assert(ErrKindNone == err_listen.kind);
    assert(ErrKindNone == io_run_until(&test_io.io, &server.done, 1).kind);
    test_io_drain(&test_io);
    test_stdout_restore(saved);

    assert(ErrKindInvalidData == server.err.kind);
    // Nothing was ever read, and every connection was still hung up on, plus
    // the listener.
    assert(0 == ctx.read_calls);
    assert(3 == ctx.accept_handed_over);
    assert(4 == ctx.close_calls);

    for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
      assert(0 == network_ctx.pool.occupied[i]);
    }
  }

  // The ordinary connection: it is read once and hung up on, and the slot goes
  // back. No thread, and nothing to join -- the read and the close are two more
  // turns of the same loop the accept came from.
  {
    TestServerCtx ctx = {.accept_success_max = 1,
                         .accept_ends_with = ErrKindInvalidData};
    const Env env = test_env_server_make(&ctx);
    TestIo test_io = {0};
    IoServer server = {0};
    // `static`: the pool is a couple of megabytes of slots, which is more than
    // a stack frame should hold, and a send buffer per slot doubled it.
    static TorrentNetworkCtx network_ctx;
    test_network_ctx_init(&network_ctx);

    assert(ErrKindInvalidData ==
           test_server_run(&test_io, &ctx, &env, &server, &network_ctx, addr)
               .kind);

    assert(1 == ctx.read_calls);
    assert(2 == ctx.close_calls);
    for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
      assert(0 == network_ctx.pool.occupied[i]);
    }
  }

  // A read that fails still hangs up and still frees the slot.
  {
    TestServerCtx ctx = {.accept_success_max = 1,
                         .read_fails_with = ErrKindConnReset,
                         .accept_ends_with = ErrKindInvalidData};
    const Env env = test_env_server_make(&ctx);
    TestIo test_io = {0};
    IoServer server = {0};
    // `static`: the pool is a couple of megabytes of slots, which is more than
    // a stack frame should hold, and a send buffer per slot doubled it.
    static TorrentNetworkCtx network_ctx;
    test_network_ctx_init(&network_ctx);

    assert(ErrKindInvalidData ==
           test_server_run(&test_io, &ctx, &env, &server, &network_ctx, addr)
               .kind);

    assert(1 == ctx.read_calls);
    assert(2 == ctx.close_calls);
    for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
      assert(0 == network_ctx.pool.occupied[i]);
    }
  }
}

// Backpressure: one more connection than the pool holds, all of them held open
// by a read that never answers. Reaching this with real sockets would mean
// opening `TORRENT_PEERS_MAX` of them.
static void test_torrent_peer_pool_exhaustion(void) {
  const Ipv4Addr addr = {.ip = 0x7f000001, .port = 12345};

  // The pool is a megabyte or so of slots, and the fake `IO` has room for an
  // operation per slot: too much for the stack, either of them.
  static TorrentNetworkCtx network_ctx;
  static TestIo test_io;
  test_network_ctx_init(&network_ctx);

  TestServerCtx ctx = {.accept_success_max = TORRENT_PEERS_MAX + 1,
                       .read_parks = true,
                       .accept_ends_with = ErrKindInvalidData};
  const Env env = test_env_server_make(&ctx);
  IoServer server = {0};

  assert(
      ErrKindInvalidData ==
      test_server_run(&test_io, &ctx, &env, &server, &network_ctx, addr).kind);

  // Nothing ever finishes, so the pool fills and the last connection is
  // refused rather than overrunning the slots.
  // `TORRENT_PEERS_MAX + 1` connections were handed over, one more than the
  // pool holds, plus the call that ends the loop.
  assert(TORRENT_PEERS_MAX + 2 == ctx.accept_calls);
  assert(TORRENT_PEERS_MAX + 1 == ctx.accept_handed_over);
  // Every connection the pool took was read from; the refused one never was.
  assert(TORRENT_PEERS_MAX == ctx.read_calls);

  // Every group is full: nothing was handed back, because no read answered.
  for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
    assert(~(PoolSlotGroup)0 == network_ctx.pool.occupied[i]);
  }

  // The refused connection was hung up on through `Env`, there being no slot
  // to keep a completion in; the listener went through `IO` as always.
  assert(1 == ctx.close_socket_calls);
  assert(1 == ctx.close_calls);
}

// The two names that go into a peer's log lines. Every enumerator is listed
// here by hand rather than walked over a range, because the point is that a new
// state or a new tag has to be given a name: `-Wswitch-enum` makes the
// converter fail to build, and this makes the name distinct once it is there.
static void test_torrent_peer_to_cstr(void) {
  const TorrentPeerState states[] = {
      TorrentPeerStateInitial,
      TorrentPeerStateSentHandshake,
      TorrentPeerStateHandshaked,
  };

  for (usize i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
    const char *const got = torrent_peer_state_to_cstr(states[i]);
    assert(got);
    assert(strlen(got) > 0);

    // No two states read the same, or a log line could not tell them apart.
    for (usize j = 0; j < i; j++) {
      assert(0 != strcmp(got, torrent_peer_state_to_cstr(states[j])));
    }
  }

  const TorrentMessageKind kinds[] = {
      TorrentMessageKindChoke,      TorrentMessageKindUnchoke,
      TorrentMessageKindInterested, TorrentMessageKindUninterested,
      TorrentMessageKindHave,       TorrentMessageKindBitfield,
      TorrentMessageKindRequest,    TorrentMessageKindPiece,
      TorrentMessageKindCancel,     TorrentMessageKindKeepAlive,
      TorrentMessageKindUnknown,    TorrentMessageKindNone,
  };

  for (usize i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    const char *const got = torrent_message_kind_to_cstr(kinds[i]);
    assert(got);
    assert(strlen(got) > 0);

    for (usize j = 0; j < i; j++) {
      assert(0 != strcmp(got, torrent_message_kind_to_cstr(kinds[j])));
    }
  }

  // The names are the protocol's, so a log line reads against a packet capture.
  assert(0 ==
         strcmp("have", torrent_message_kind_to_cstr(TorrentMessageKindHave)));
  assert(0 == strcmp("bitfield",
                     torrent_message_kind_to_cstr(TorrentMessageKindBitfield)));
  assert(0 == strcmp("keep-alive", torrent_message_kind_to_cstr(
                                       TorrentMessageKindKeepAlive)));

  // And the actor's two, for the same reason: an event or a command with no name
  // of its own makes a peer's log line unreadable exactly where it matters.
  const TorrentEventKind events[] = {
      TorrentEventKindNone,      TorrentEventKindAccepted,
      TorrentEventKindConnected, TorrentEventKindHangup,
      TorrentEventKindMalformed, TorrentEventKindHandshake,
      TorrentEventKindMessage,   TorrentEventKindDeadline,
  };

  for (usize i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
    const char *const got = torrent_event_kind_to_cstr(events[i]);
    assert(got);
    assert(strlen(got) > 0);

    for (usize j = 0; j < i; j++) {
      assert(0 != strcmp(got, torrent_event_kind_to_cstr(events[j])));
    }
  }

  const TorrentCommandKind commands[] = {
      TorrentCommandKindNone,
      TorrentCommandKindSendHandshake,
      TorrentCommandKindSendKeepAlive,
      TorrentCommandKindClose,
  };

  for (usize i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
    const char *const got = torrent_command_kind_to_cstr(commands[i]);
    assert(got);
    assert(strlen(got) > 0);

    for (usize j = 0; j < i; j++) {
      assert(0 != strcmp(got, torrent_command_kind_to_cstr(commands[j])));
    }
  }
}

// ---------- Peer messages ----------

// Every message is a big-endian length, then that many bytes: a tag and its
// payload. The length does not count itself.
//
// Four answers come back and a test has to tell them apart: a whole message,
// "not yet" with the input untouched, a whole message with a tag this does not
// know that is stepped over, and a peer talking nonsense. The second is the one
// a real network produces constantly and the one a parser is most likely to get
// wrong, so most of what follows is about it. It and the third differ only in
// whether the bytes are gone, which is why `data.len` is asserted on both.
static void test_torrent_peer_parse_message(void) {
  // The wire bytes of each message this understands, tags included, so the tag
  // numbers are stated here and not taken from the enum under test.
  const u8 choke[] = {0x00, 0x00, 0x00, 0x01, 0x00};
  const u8 unchoke[] = {0x00, 0x00, 0x00, 0x01, 0x01};
  const u8 interested[] = {0x00, 0x00, 0x00, 0x01, 0x02};
  const u8 uninterested[] = {0x00, 0x00, 0x00, 0x01, 0x03};
  // `have` is tag 4 and carries one piece index.
  const u8 have[] = {0x00, 0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x01, 0x2c};
  // `bitfield` is tag 5, and its length is however many bytes the bits need.
  const u8 bitfield[] = {0x00, 0x00, 0x00, 0x04, 0x05, 0xff, 0x80, 0x01};
  // `request` is tag 6: index, offset, length.
  const u8 request[] = {0x00, 0x00, 0x00, 0x0d, 0x06, 0x00, 0x00, 0x00, 0x07,
                        0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00};
  // `piece` is tag 7: index, offset, then the block.
  const u8 piece[] = {0x00, 0x00, 0x00, 0x0c, 0x07, 0x00, 0x00, 0x00,
                      0x02, 0x00, 0x00, 0x80, 0x00, 0xaa, 0xbb, 0xcc};
  // `cancel` is tag 8, shaped like a `request`.
  const u8 cancel[] = {0x00, 0x00, 0x00, 0x0d, 0x08, 0x00, 0x00, 0x00, 0x07,
                       0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00};
  // A keep-alive is a length of zero and nothing else: no tag, no payload.
  const u8 keep_alive[] = {0x00, 0x00, 0x00, 0x00};

  // A zeroed `TorrentPeerMessage` reads as `choke`, tag zero being a real one,
  // so every case below starts the kind at something it does not expect. A
  // parser that returned without writing a kind would otherwise pass.

  // One whole message at a time, each read down to the last byte.
  {
    const struct {
      const char *name;
      const u8 *bytes;
      usize len;
      TorrentMessageKind kind;
    } cases[] = {
        {"choke", choke, sizeof(choke), TorrentMessageKindChoke},
        {"unchoke", unchoke, sizeof(unchoke), TorrentMessageKindUnchoke},
        {"interested", interested, sizeof(interested),
         TorrentMessageKindInterested},
        {"uninterested", uninterested, sizeof(uninterested),
         TorrentMessageKindUninterested},
        {"have", have, sizeof(have), TorrentMessageKindHave},
        {"bitfield", bitfield, sizeof(bitfield), TorrentMessageKindBitfield},
        {"request", request, sizeof(request), TorrentMessageKindRequest},
        {"piece", piece, sizeof(piece), TorrentMessageKindPiece},
        {"cancel", cancel, sizeof(cancel), TorrentMessageKindCancel},
        {"keep-alive", keep_alive, sizeof(keep_alive),
         TorrentMessageKindKeepAlive},
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      Bytes data = bytes_make((u8 *)cases[i].bytes, cases[i].len);
      TorrentPeerMessage msg = {.kind = TorrentMessageKindNone};

      assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
      assert(cases[i].kind == msg.kind);
      // The whole message was taken, length prefix and all.
      assert(0 == data.len);
    }
  }

  // The payloads, which is what the tag is for.
  {
    Bytes data = bytes_make((u8 *)have, sizeof(have));
    TorrentPeerMessage msg = {.kind = TorrentMessageKindNone};
    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindHave == msg.kind);
    assert(300 == msg.v.have);
  }
  {
    Bytes data = bytes_make((u8 *)request, sizeof(request));
    TorrentPeerMessage msg = {.kind = TorrentMessageKindNone};
    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindRequest == msg.kind);
    assert(7 == msg.v.idx_begin_len.idx);
    assert(0x4000 == msg.v.idx_begin_len.begin);
    assert(0x4000 == msg.v.idx_begin_len.len);
  }
  {
    Bytes data = bytes_make((u8 *)piece, sizeof(piece));
    TorrentPeerMessage msg = {.kind = TorrentMessageKindNone};
    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindPiece == msg.kind);
    assert(2 == msg.v.piece.idx);
    assert(0x8000 == msg.v.piece.begin);
  }

  // Half a message is the ordinary case, not a broken peer: TCP cuts a stream
  // wherever it likes. Every prefix of a `request` short of the whole thing is
  // "not yet", and leaves the input exactly as it was so the same call works
  // once the rest turns up.
  {
    for (usize n = 0; n < sizeof(request); n++) {
      Bytes data = bytes_make((u8 *)request, n);
      TorrentPeerMessage msg = {.kind = TorrentMessageKindUnknown};

      assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
      assert(TorrentMessageKindNone == msg.kind);
      // Untouched: not one of the bytes it did look at was consumed.
      assert(n == data.len);
      assert(request == data.data);
    }
  }

  // The same, byte by byte: a peer feeding a `piece` one byte at a time is
  // answered "not yet" every time until the last one, and then the whole
  // message comes out at once.
  {
    u8 buf[sizeof(piece)] = {0};
    for (usize n = 0; n < sizeof(piece); n++) {
      buf[n] = piece[n];

      Bytes data = bytes_make(buf, n + 1);
      TorrentPeerMessage msg = {.kind = TorrentMessageKindUnknown};

      assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);

      if (n + 1 < sizeof(piece)) {
        assert(TorrentMessageKindNone == msg.kind);
        assert(n + 1 == data.len);
      } else {
        assert(TorrentMessageKindPiece == msg.kind);
        assert(0 == data.len);
      }
    }
  }

  // Several in one packet, which is what a peer that has been waiting sends:
  // each call takes exactly one and leaves the rest.
  {
    u8 run[sizeof(bitfield) + sizeof(unchoke) + sizeof(have) +
           sizeof(keep_alive)] = {0};
    usize at = 0;
    memcpy(run + at, bitfield, sizeof(bitfield));
    at += sizeof(bitfield);
    memcpy(run + at, unchoke, sizeof(unchoke));
    at += sizeof(unchoke);
    memcpy(run + at, have, sizeof(have));
    at += sizeof(have);
    memcpy(run + at, keep_alive, sizeof(keep_alive));

    const TorrentMessageKind expected[] = {
        TorrentMessageKindBitfield, TorrentMessageKindUnchoke,
        TorrentMessageKindHave, TorrentMessageKindKeepAlive};

    Bytes data = bytes_make(run, sizeof(run));
    for (usize i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
      TorrentPeerMessage msg = {.kind = TorrentMessageKindNone};

      assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
      assert(expected[i] == msg.kind);
    }
    // All four, and nothing over.
    assert(0 == data.len);

    TorrentPeerMessage msg = {.kind = TorrentMessageKindUnknown};
    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindNone == msg.kind);
  }

  // A peer talking nonsense, which ends the connection rather than being waited
  // out. The difference matters: waiting for a message that can never arrive is
  // a connection that never ends.
  {
    const struct {
      const char *name;
      u8 bytes[16];
      usize len;
    } bad[] = {
        // `choke` is the tag and nothing else, so a payload behind it is wrong.
        {"choke with a payload", {0x00, 0x00, 0x00, 0x02, 0x00, 0xff}, 6},
        // `have` carries exactly one index.
        {"short have", {0x00, 0x00, 0x00, 0x02, 0x04, 0xff}, 6},
        {"long have",
         {0x00, 0x00, 0x00, 0x06, 0x04, 0x00, 0x00, 0x00, 0x01, 0xff},
         10},
        // `request` carries exactly three numbers.
        {"short request",
         {0x00, 0x00, 0x00, 0x09, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
          0x00, 0x02},
         13},
        // A `piece` with nowhere for the block to be.
        {"piece with no block",
         {0x00, 0x00, 0x00, 0x09, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
          0x00, 0x02},
         13},
        // A length no buffer here could ever hold. Waiting for this one would
        // fill the receive buffer and then wait for ever.
        {"length past the buffer", {0xff, 0xff, 0xff, 0xff}, 4},
    };

    for (usize i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      Bytes data = bytes_make((u8 *)bad[i].bytes, bad[i].len);
      TorrentPeerMessage msg = {.kind = TorrentMessageKindUnknown};

      assert(ErrKindInvalidData ==
             torrent_peer_parse_message(&data, &msg).kind);
      // No message on an error, whatever the caller passed in.
      assert(TorrentMessageKindNone == msg.kind);
    }
  }

  // A tag this does not know is not nonsense: the length says where the message
  // ends, so it is stepped over. Waiting for it instead would leave it in the
  // buffer and the caller would come back to it for ever. It says which tag and
  // how many bytes, which is all a log line needs.
  {
    const u8 unknown[] = {0x00, 0x00, 0x00, 0x03, 0x63, 0xaa, 0xbb};
    Bytes data = bytes_make((u8 *)unknown, sizeof(unknown));
    TorrentPeerMessage msg = {.kind = TorrentMessageKindNone};

    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindUnknown == msg.kind);
    assert(0x63 == msg.v.unknown.tag);
    // The length prefix, so the tag byte counts and the payload's two bytes do.
    assert(3 == msg.v.unknown.size);
    // The whole of it, payload included, so the next message starts where it
    // should.
    assert(0 == data.len);
  }

  // Half of an unknown message is still "not yet": the length arrives before
  // the tag does, so the skip waits for the whole thing like any other message.
  // This is the pair that `*data` used to be the only way to tell apart.
  {
    const u8 unknown[] = {0x00, 0x00, 0x00, 0x03, 0x63, 0xaa, 0xbb};
    for (usize n = 0; n < sizeof(unknown); n++) {
      Bytes data = bytes_make((u8 *)unknown, n);
      TorrentPeerMessage msg = {.kind = TorrentMessageKindUnknown};

      assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
      assert(TorrentMessageKindNone == msg.kind);
      // Untouched, which is what `None` promises and `Unknown` does not.
      assert(n == data.len);
    }
  }

  // One between two known ones, which is the case that matters: skipping the
  // wrong number of bytes loses the stream, and the message behind it is how a
  // test can tell.
  {
    const u8 unknown[] = {0x00, 0x00, 0x00, 0x03, 0x63, 0xaa, 0xbb};
    u8 run[sizeof(choke) + sizeof(unknown) + sizeof(have)] = {0};
    usize at = 0;
    memcpy(run + at, choke, sizeof(choke));
    at += sizeof(choke);
    memcpy(run + at, unknown, sizeof(unknown));
    at += sizeof(unknown);
    memcpy(run + at, have, sizeof(have));

    Bytes data = bytes_make(run, sizeof(run));

    TorrentPeerMessage msg = {.kind = TorrentMessageKindNone};
    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindChoke == msg.kind);

    // The unknown one: exactly its own bytes gone, and no more.
    const usize before = data.len;
    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindUnknown == msg.kind);
    assert(sizeof(unknown) == before - data.len);

    // So the `have` behind it is found, payload and all, rather than the stream
    // being read from the middle of the skipped message.
    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindHave == msg.kind);
    assert(300 == msg.v.have);
    assert(0 == data.len);
  }

  // The largest length the receive buffer can hold is not malformed, it just
  // has not all arrived: the line between "too big" and "not yet" sits here,
  // and an off-by-one either way is a peer dropped or a peer waited on for
  // ever.
  {
    const u32 biggest = TORRENT_PEER_RECV_BUF_CAP - (u32)sizeof(u32);

    u8 head[4] = {(u8)(biggest >> 24), (u8)(biggest >> 16), (u8)(biggest >> 8),
                  (u8)biggest};
    Bytes data = bytes_make(head, sizeof(head));
    TorrentPeerMessage msg = {.kind = TorrentMessageKindUnknown};
    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindNone == msg.kind);

    // One more than that can never be buffered, so it is refused.
    const u32 too_big = biggest + 1;
    u8 head2[4] = {(u8)(too_big >> 24), (u8)(too_big >> 16), (u8)(too_big >> 8),
                   (u8)too_big};
    Bytes data2 = bytes_make(head2, sizeof(head2));
    assert(ErrKindInvalidData ==
           torrent_peer_parse_message(&data2, &msg).kind);
  }

  // Nothing at all is "not yet", not an error: an empty buffer is where every
  // connection starts.
  {
    Bytes data = {0};
    TorrentPeerMessage msg = {.kind = TorrentMessageKindUnknown};
    assert(ErrKindNone == torrent_peer_parse_message(&data, &msg).kind);
    assert(TorrentMessageKindNone == msg.kind);
  }
}

// ---------- The peer state machine ----------

// A recognisable descriptor for the one connection these tests drive.
#define TEST_PEER_FD 77

// The script behind the peer tests. Driving the state machine needs exactly two
// things said about the peer -- what it sends, and in how many pieces -- and no
// real socket can be asked for either, the kernel deciding on its own where a
// stream is cut.
typedef struct {
  // What the peer says, handed over `chunk` bytes at a time. A `chunk` of 0
  // means "everything still unsaid", which is one packet for the whole thing.
  Bytes says;
  usize chunk;
  usize said;

  // Once there is nothing left to say. `ErrKindNone` answers a read of 0 bytes,
  // which is how a peer hanging up looks; anything else is that failure.
  ErrorKind spent_err;

  usize read_calls;
  usize write_calls;
  // How much of the send buffer each write takes. 0 means all of it, which is
  // the ordinary case; a smaller number is the short write a nearly full socket
  // produces, and nothing but a fake can ask for one at a chosen moment.
  usize write_chunk;
  // Everything the peer was sent, in order, so a test can check the handshake
  // that went out and not just that something did.
  u8 written[2 * TEST_HANDSHAKE_LEN];
  usize written_len;
  ErrorKind write_fails_with;

  // `IO`'s close, the hang-up with a callback behind it.
  usize close_calls;
  // `Env`'s, the hang-up taken when the close could not even be submitted.
  usize close_socket_calls;
} TestPeerCtx;

__attribute__((warn_unused_result)) static Error
test_peer_close_socket(const Env *env, i32 fd) {
  TestPeerCtx *const c = env->ctx;
  assert(c);
  assert(TEST_PEER_FD == fd);

  c->close_socket_calls += 1;
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Env
test_env_peer_make(TestPeerCtx *ctx) {
  assert(ctx);

  // A peer reaches `Env` for one thing only, the hang-up it falls back on;
  // every other slot stays null, so reaching for one crashes rather than
  // quietly doing something real.
  return (Env){.close_socket = test_peer_close_socket, .ctx = ctx};
}

static TestIoPerformResult test_peer_perform(TestIo *test_io,
                                             IoCompletion *completion, i32 fd,
                                             Error *dst_err, usize *dst_res) {
  TestPeerCtx *const c = test_io->script;
  assert(c);
  assert(completion);
  assert(TEST_PEER_FD == fd);

  switch (completion->action.kind) {
  case IoActionKindRead: {
    c->read_calls += 1;

    assert(c->said <= c->says.len);
    const usize unsaid = c->says.len - c->said;
    if (0 == unsaid) {
      *dst_err = (Error){.kind = c->spent_err};
      // `*dst_res` stays 0, which with no error is end of file.
      return TestIoPerformDone;
    }

    const Bytes data = completion->action.v.read.data;
    assert(data.len > 0);

    usize n = (0 == c->chunk) ? unsaid : c->chunk;
    if (n > unsaid) {
      n = unsaid;
    }
    if (n > data.len) {
      n = data.len;
    }

    memcpy(data.data, c->says.data + c->said, n);
    c->said += n;
    *dst_res = n;
    return TestIoPerformDone;
  }

  case IoActionKindWrite: {
    c->write_calls += 1;

    if (ErrKindNone != c->write_fails_with) {
      *dst_err = (Error){.kind = c->write_fails_with};
      return TestIoPerformDone;
    }

    const Bytes data = completion->action.v.write.data;
    assert(data.len > 0);

    usize n = (0 == c->write_chunk) ? data.len : c->write_chunk;
    if (n > data.len) {
      n = data.len;
    }

    // Kept so a test can read back what was actually sent.
    assert(c->written_len + n <= sizeof(c->written));
    memcpy(c->written + c->written_len, data.data, n);
    c->written_len += n;

    *dst_res = n;
    return TestIoPerformDone;
  }

  case IoActionKindClose:
    c->close_calls += 1;
    return TestIoPerformDone;

  case IoActionKindNone:
  case IoActionKindOpen:
  case IoActionKindAccept:
  case IoActionKindConnect:
  case IoActionKindSendTo:
  case IoActionKindFileSize:
  case IoActionKindRemoveFile:
    break;
  }

  assert(0 && "a peer drives none of these");
  return TestIoPerformDone;
}

// Is the pool holding nothing at all? Every one of these tests ends there: a
// connection lets go of its slot however it ended, and one that stayed behind
// would be a slot lost for the life of the process.
__attribute__((warn_unused_result)) static bool
test_peer_pool_is_empty(const TorrentNetworkCtx *network_ctx) {
  assert(network_ctx);

  for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
    if (0 != network_ctx->pool.occupied[i]) {
      return false;
    }
  }
  return true;
}

// One connection, from the accept to the slot going back, and the last state it
// was seen in. The listener is not involved: `torrent_peer_on_accept` is called
// with the descriptor a listener would have handed over.
//
// The state is what the call counts cannot show, and it takes turning the loop
// a step at a time to see: a connection ends inside a tick, so whatever state
// it reached is only readable while its hang-up is still in flight. A released
// slot is zeroed, which reads back as the initial state, so only what was seen
// while the slot was still held counts.
__attribute__((warn_unused_result)) static TorrentPeerState
test_peer_run(TestIo *test_io, TestPeerCtx *ctx, const Env *env,
              TorrentNetworkCtx *network_ctx) {
  assert(test_io);
  assert(ctx);
  assert(network_ctx);
  // The pool starts empty, so the connection takes the first slot, and that is
  // the one to watch.
  assert(test_peer_pool_is_empty(network_ctx));
  const TorrentPeer *const peer = &network_ctx->pool.slots[0];

  test_io_make(test_io, env, test_peer_perform, ctx);

  const i32 saved = test_stdout_silence();

  torrent_peer_on_accept(&test_io->io, network_ctx,
                         (Ipv4Addr){.ip = 0x7f000001, .port = 6881},
                         TEST_PEER_FD);

  // More turns than any legitimate run of this script can need: a byte in and a
  // byte out per turn at worst, plus the handshake and the hang-up. A run that
  // wants more than this is one that never ends, which is what a state machine
  // that does not notice end of file does -- a read at end of file answers on
  // the spot, for ever.
  const usize turns_max = ctx->says.len + 2 * TEST_HANDSHAKE_LEN + 16;

  TorrentPeerState last = peer->state;
  bool drained = false;
  for (usize i = 0; i < turns_max; i++) {
    if (0 == test_io->submitted_len) {
      drained = true;
      break;
    }

    const Error err = test_io->io.run_for_ns(&test_io->io, 1);
    assert(ErrKindNone == err.kind);

    if (0 != (network_ctx->pool.occupied[0] & 1)) {
      last = peer->state;
    }
  }
  assert(drained);

  test_stdout_restore(saved);

  return last;
}

// The handshake, in both directions, and how a connection ends. The buffer
// bookkeeping is the whole difficulty: a handshake arrives in as many pieces as
// the network feels like and leaves in as many as the socket will take,
// possibly with the next message already behind it.
static void test_torrent_peer_state_machine(void) {
  // The pool is a couple of megabytes of slots and the fake has room for an
  // operation per slot: too much for the stack, either of them.
  static TorrentNetworkCtx network_ctx;
  static TestIo test_io;

  u8 peer_id_bytes[TORRENT_PEER_ID_LEN] = {'-',  'F',  'S',  '0', '0', '0', '1',
                                           0x00, 0xff, 0x2a, 1,   2,   3,   4,
                                           5,    6,    7,    8,   9,   10};
  const Bytes peer_id = bytes_make(peer_id_bytes, sizeof(peer_id_bytes));

  test_network_ctx_init(&network_ctx);

  u8 handshake[TEST_HANDSHAKE_LEN] = {0};
  test_handshake_fill(handshake, network_ctx.info_hash, peer_id);
  const Bytes whole = bytes_make(handshake, sizeof(handshake));

  // The handshake in one packet. Ours goes out, theirs comes in, and the
  // connection then stays up waiting for messages: it ends only because the
  // fake peer has nothing else to say and the next read reaches end of file.
  {
    TestPeerCtx ctx = {.says = whole};
    const Env env = test_env_peer_make(&ctx);

    assert(TorrentPeerStateHandshaked ==
           test_peer_run(&test_io, &ctx, &env, &network_ctx));

    // Theirs, then the one that finds nothing behind it.
    assert(2 == ctx.read_calls);
    // Ours, then the greeting their handshake set off.
    assert(2 == ctx.write_calls);
    assert(1 == ctx.close_calls);
    assert(0 == ctx.close_socket_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // What we sent. A peer will not talk to us unless our handshake is as
  // well-formed as we demand theirs to be, and carries the torrent we are
  // asking about. Kept, so the cases below can insist on the same bytes however
  // the socket cuts them.
  u8 sent[TEST_HANDSHAKE_LEN] = {0};
  {
    TestPeerCtx ctx = {.says = whole};
    const Env env = test_env_peer_make(&ctx);

    assert(TorrentPeerStateHandshaked ==
           test_peer_run(&test_io, &ctx, &env, &network_ctx));

    // The handshake first, and the greeting behind it.
    assert(TEST_PEER_GREETING_LEN == ctx.written_len);
    memcpy(sent, ctx.written, sizeof(sent));

    // It has to pass the same check we apply to a peer's, against the info hash
    // we are serving. Our peer id is our own, so it is not compared against
    // theirs -- only its length is fixed.
    Bytes sent_peer_id = {0};
    assert(torrent_check_handshake(bytes_make(sent, sizeof(sent)),
                                   network_ctx.info_hash, &sent_peer_id));
    assert(TORRENT_PEER_ID_LEN == sent_peer_id.len);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // A socket that takes only a few bytes at a time. What goes out is the same
  // 68 bytes in the same order, however many writes that takes, which is what
  // the send buffer's bookkeeping is for: what a write did not take moves to
  // the front, and the count behind it has to be what is left rather than what
  // went.
  //
  // The peer is fed in a byte at a time too, so it is still there to be written
  // to while the write drains: one that hung up first would end the connection
  // before the interesting part.
  {
    // Their handshake, then four keep-alives: a peer that is still there while
    // the greeting drains. At one byte a write, the greeting needs ten turns to
    // go, and end of file in any of them would hang up with it half sent.
    u8 quiet[TEST_HANDSHAKE_LEN + 16] = {0};
    memcpy(quiet, handshake, sizeof(handshake));
    const Bytes unhurried = bytes_make(quiet, sizeof(quiet));

    const usize chunks[] = {1, 7, 33, 67, 68};
    for (usize i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
      TestPeerCtx ctx = {
          .says = unhurried, .chunk = 1, .write_chunk = chunks[i]};
      const Env env = test_env_peer_make(&ctx);

      assert(TorrentPeerStateHandshaked ==
             test_peer_run(&test_io, &ctx, &env, &network_ctx));

      assert(TEST_PEER_GREETING_LEN == ctx.written_len);
      assert(0 == memcmp(ctx.written, sent, sizeof(sent)));
      assert(0 == memcmp(ctx.written + TEST_HANDSHAKE_LEN, test_peer_greeting,
                         sizeof(test_peer_greeting)));
      // As many writes as the chunk size needs, and not one more. The handshake
      // and the greeting are counted apart because the second is only queued
      // once the first has gone and their answer has come back, so no write
      // ever carries bytes of both.
      assert((TEST_HANDSHAKE_LEN + chunks[i] - 1) / chunks[i] +
                 (sizeof(test_peer_greeting) + chunks[i] - 1) / chunks[i] ==
             ctx.write_calls);
      assert(1 == ctx.close_calls);
      assert(test_peer_pool_is_empty(&network_ctx));
    }
  }

  // The same 68 bytes coming in, one byte per packet, and still the same
  // handshake. Nothing but a fake can cut a stream this finely, and the cut is
  // what the receive buffer's bookkeeping gets wrong: a read aimed at the front
  // overwrites the byte before it, and the handshake then matches nothing.
  {
    TestPeerCtx ctx = {.says = whole, .chunk = 1};
    const Env env = test_env_peer_make(&ctx);

    assert(TorrentPeerStateHandshaked ==
           test_peer_run(&test_io, &ctx, &env, &network_ctx));

    // One read per byte, plus the one that reaches end of file.
    assert(TEST_HANDSHAKE_LEN + 1 == ctx.read_calls);
    assert(1 == ctx.close_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // A handshake cut anywhere at all adds up to the same 68 bytes.
  {
    const usize chunks[] = {2, 3, 7, 17, 67, 68, 1024};
    for (usize i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
      TestPeerCtx ctx = {.says = whole, .chunk = chunks[i]};
      const Env env = test_env_peer_make(&ctx);

      assert(TorrentPeerStateHandshaked ==
             test_peer_run(&test_io, &ctx, &env, &network_ctx));

      assert(ctx.read_calls > 0);
      assert(1 == ctx.close_calls);
      assert(test_peer_pool_is_empty(&network_ctx));
    }
  }

  // A peer that hangs up halfway through the handshake. The read answers 0
  // bytes with nothing wrong, and a descriptor at end of file stays readable,
  // so asking again would answer 0 for ever: `TEST_PEER_TURNS_MAX` is what says
  // so here, rather than the test running until someone stops it.
  {
    TestPeerCtx ctx = {.says = bytes_take(whole, 20)};
    const Env env = test_env_peer_make(&ctx);

    assert(TorrentPeerStateSentHandshake ==
           test_peer_run(&test_io, &ctx, &env, &network_ctx));

    // The 20 bytes, then the end of file.
    assert(2 == ctx.read_calls);
    assert(1 == ctx.close_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // A peer that is cut off rather than hanging up politely.
  {
    TestPeerCtx ctx = {.says = bytes_take(whole, 20),
                       .spent_err = ErrKindConnReset};
    const Env env = test_env_peer_make(&ctx);

    assert(TorrentPeerStateSentHandshake ==
           test_peer_run(&test_io, &ctx, &env, &network_ctx));

    assert(2 == ctx.read_calls);
    assert(1 == ctx.close_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // A peer of someone else's torrent: 68 well-formed bytes carrying the wrong
  // info hash. It is told nothing and hung up on, and there is no second read.
  {
    u8 wrong[TEST_HANDSHAKE_LEN] = {0};
    memcpy(wrong, handshake, sizeof(wrong));
    wrong[28] ^= 0x01;

    TestPeerCtx ctx = {.says = bytes_make(wrong, sizeof(wrong))};
    const Env env = test_env_peer_make(&ctx);

    assert(TorrentPeerStateSentHandshake ==
           test_peer_run(&test_io, &ctx, &env, &network_ctx));

    assert(1 == ctx.read_calls);
    assert(1 == ctx.close_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // The write fails while the read is still with the kernel. The hang-up cannot
  // be submitted yet -- a completion holds one operation at a time, and handing
  // the slot back under a live registration is how a connection ends up being
  // reported twice -- so it waits for the read to report and goes out then.
  {
    TestPeerCtx ctx = {.says = whole, .write_fails_with = ErrKindConnReset};
    const Env env = test_env_peer_make(&ctx);

    (void)test_peer_run(&test_io, &ctx, &env, &network_ctx);

    assert(1 == ctx.write_calls);
    // Exactly one hang-up, however many operations were outstanding when the
    // connection was given up on.
    assert(1 == ctx.close_calls);
    assert(0 == ctx.close_socket_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // The read that cannot even be submitted. Its callback never runs, so the
  // connection has to be given up on there and then -- and the write that did
  // go out is still in flight, so the hang-up waits for it.
  {
    TestPeerCtx ctx = {.says = whole};
    const Env env = test_env_peer_make(&ctx);

    test_io_make(&test_io, &env, test_peer_perform, &ctx);
    test_io.submit_fails_for = IoActionKindRead;
    test_io.submit_fails_with = ErrKindOOM;

    const i32 saved = test_stdout_silence();
    torrent_peer_on_accept(&test_io.io, &network_ctx,
                           (Ipv4Addr){.ip = 0x7f000001, .port = 6881},
                           TEST_PEER_FD);
    test_io_drain(&test_io);
    test_stdout_restore(saved);

    assert(0 == ctx.read_calls);
    assert(1 == ctx.write_calls);
    assert(1 == ctx.close_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // The write that cannot be submitted. Nothing is in flight at that point, so
  // the hang-up goes out immediately.
  {
    TestPeerCtx ctx = {.says = whole};
    const Env env = test_env_peer_make(&ctx);

    test_io_make(&test_io, &env, test_peer_perform, &ctx);
    test_io.submit_fails_for = IoActionKindWrite;
    test_io.submit_fails_with = ErrKindOOM;

    const i32 saved = test_stdout_silence();
    torrent_peer_on_accept(&test_io.io, &network_ctx,
                           (Ipv4Addr){.ip = 0x7f000001, .port = 6881},
                           TEST_PEER_FD);
    test_io_drain(&test_io);
    test_stdout_restore(saved);

    assert(0 == ctx.write_calls);
    assert(0 == ctx.read_calls);
    assert(1 == ctx.close_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // The hang-up that cannot be submitted either. Nothing is going to call back,
  // so the connection goes through `Env`, which answers on the spot, and the
  // slot is handed back there and then: one kept for a connection nobody will
  // ever hear from again is lost for the life of the process.
  {
    TestPeerCtx ctx = {.says = whole};
    const Env env = test_env_peer_make(&ctx);

    test_io_make(&test_io, &env, test_peer_perform, &ctx);
    test_io.submit_fails_for = IoActionKindClose;
    test_io.submit_fails_with = ErrKindOOM;

    const i32 saved = test_stdout_silence();
    torrent_peer_on_accept(&test_io.io, &network_ctx,
                           (Ipv4Addr){.ip = 0x7f000001, .port = 6881},
                           TEST_PEER_FD);
    test_io_drain(&test_io);
    test_stdout_restore(saved);

    // Never submitted, so `IO` never performed it.
    assert(0 == ctx.close_calls);
    assert(1 == ctx.close_socket_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }
}

// The messages behind the handshake, which is what libtorrent sends: it
// announces the pieces it has straight after the handshake, in the same packet
// or the next one, and a peer that drops them or mistakes one for another gets
// nowhere.
static void test_torrent_peer_messages(void) {
  static TorrentNetworkCtx network_ctx;
  static TestIo test_io;

  u8 peer_id_bytes[TORRENT_PEER_ID_LEN];
  memset(peer_id_bytes, 'P', sizeof(peer_id_bytes));

  test_network_ctx_init(&network_ctx);

  // A handshake, then the opening pair libtorrent sends: what it has, and that
  // it is not choking us.
  const u8 msgs[] = {// bitfield, 3 bytes of bits.
                     0x00, 0x00, 0x00, 0x04, 0x05, 0xff, 0x80, 0x01,
                     // unchoke.
                     0x00, 0x00, 0x00, 0x01, 0x01,
                     // have, piece 300.
                     0x00, 0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x01, 0x2c,
                     // keep-alive.
                     0x00, 0x00, 0x00, 0x00};

  u8 says[TEST_HANDSHAKE_LEN + sizeof(msgs)] = {0};
  test_handshake_fill(says, network_ctx.info_hash,
                      bytes_make(peer_id_bytes, sizeof(peer_id_bytes)));
  memcpy(says + TEST_HANDSHAKE_LEN, msgs, sizeof(msgs));
  const Bytes whole = bytes_make(says, sizeof(says));

  // All of it in one packet: the handshake is taken, and then every message
  // behind it, without another read in between.
  {
    TestPeerCtx ctx = {.says = whole};
    const Env env = test_env_peer_make(&ctx);

    assert(TorrentPeerStateHandshaked ==
           test_peer_run(&test_io, &ctx, &env, &network_ctx));

    // One read for everything, then the one that reaches end of file. A parser
    // that stopped after the first message would need three more.
    assert(2 == ctx.read_calls);
    assert(1 == ctx.close_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // The same stream cut every possible way. A message split across reads is the
  // ordinary case, and none of these cuts may drop a message, mistake one for
  // another, or end the connection.
  {
    const usize chunks[] = {1, 2, 3, 5, 8, 13, 67, 68, 69, 70, 91, 1024};
    for (usize i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
      TestPeerCtx ctx = {.says = whole, .chunk = chunks[i]};
      const Env env = test_env_peer_make(&ctx);

      assert(TorrentPeerStateHandshaked ==
             test_peer_run(&test_io, &ctx, &env, &network_ctx));

      // It got to the end of the stream and hung up there, rather than giving
      // up on a half-arrived message.
      assert(1 == ctx.close_calls);
      assert(0 == ctx.close_socket_calls);
      assert(test_peer_pool_is_empty(&network_ctx));
    }
  }

  // A peer talking nonsense after a good handshake: the handshake is no
  // guarantee of what follows, and the connection ends rather than the parser
  // being asked again for ever.
  {
    u8 bad[TEST_HANDSHAKE_LEN + 6] = {0};
    test_handshake_fill(bad, network_ctx.info_hash,
                        bytes_make(peer_id_bytes, sizeof(peer_id_bytes)));
    // `choke` is the tag on its own, so a payload behind it cannot be read as
    // anything. An unknown tag is not this: its length still says where it
    // ends, and it is skipped below.
    const u8 nonsense[] = {0x00, 0x00, 0x00, 0x02, 0x00, 0xff};
    memcpy(bad + TEST_HANDSHAKE_LEN, nonsense, sizeof(nonsense));

    TestPeerCtx ctx = {.says = bytes_make(bad, sizeof(bad))};
    const Env env = test_env_peer_make(&ctx);

    assert(TorrentPeerStateHandshaked ==
           test_peer_run(&test_io, &ctx, &env, &network_ctx));

    // One read had all of it, and the connection ended on the message, not on
    // end of file.
    assert(1 == ctx.read_calls);
    assert(1 == ctx.close_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // A tag this build does not know, with a known message behind it. The
  // connection carries on to end of file instead of ending on the tag, and the
  // message behind it is reached: an unknown message left in the buffer would
  // hide everything after it and be re-read until the buffer filled.
  {
    const u8 unknown_then_unchoke[] = {// A tag no version of the protocol has.
                                       0x00, 0x00, 0x00, 0x03, 0x63, 0xaa, 0xbb,
                                       // unchoke.
                                       0x00, 0x00, 0x00, 0x01, 0x01};

    u8 skipped[TEST_HANDSHAKE_LEN + sizeof(unknown_then_unchoke)] = {0};
    test_handshake_fill(skipped, network_ctx.info_hash,
                        bytes_make(peer_id_bytes, sizeof(peer_id_bytes)));
    memcpy(skipped + TEST_HANDSHAKE_LEN, unknown_then_unchoke,
           sizeof(unknown_then_unchoke));

    TestPeerCtx ctx = {.says = bytes_make(skipped, sizeof(skipped))};
    const Env env = test_env_peer_make(&ctx);

    assert(TorrentPeerStateHandshaked ==
           test_peer_run(&test_io, &ctx, &env, &network_ctx));

    // One read for all of it and one that reaches end of file, which is the
    // good path: the unknown tag cost no extra read and ended nothing.
    assert(2 == ctx.read_calls);
    assert(1 == ctx.close_calls);
    assert(0 == ctx.close_socket_calls);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // The same stream cut every possible way, so the skip is not something that
  // only works when the whole message lands in one read.
  {
    const u8 unknown_then_unchoke[] = {
        0x00, 0x00, 0x00, 0x03, 0x63, 0xaa, 0xbb, 0x00, 0x00, 0x00, 0x01, 0x01};

    u8 skipped[TEST_HANDSHAKE_LEN + sizeof(unknown_then_unchoke)] = {0};
    test_handshake_fill(skipped, network_ctx.info_hash,
                        bytes_make(peer_id_bytes, sizeof(peer_id_bytes)));
    memcpy(skipped + TEST_HANDSHAKE_LEN, unknown_then_unchoke,
           sizeof(unknown_then_unchoke));
    const Bytes skipped_says = bytes_make(skipped, sizeof(skipped));

    const usize chunks[] = {1, 2, 3, 5, 8, 13, 67, 68, 69, 70, 1024};
    for (usize i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
      TestPeerCtx ctx = {.says = skipped_says, .chunk = chunks[i]};
      const Env env = test_env_peer_make(&ctx);

      assert(TorrentPeerStateHandshaked ==
             test_peer_run(&test_io, &ctx, &env, &network_ctx));

      assert(1 == ctx.close_calls);
      assert(0 == ctx.close_socket_calls);
      assert(test_peer_pool_is_empty(&network_ctx));
    }
  }
}

// What the receive buffer looks like as messages come out of it. The call
// counts above cannot see this, and it is where a parser that forgets to
// compact the buffer goes wrong: the messages it has already handled stay in
// front of the ones it has not, and it reads the same one for ever.
static void test_torrent_peer_recv_buf_compacted(void) {
  static TorrentNetworkCtx network_ctx;
  static TestIo test_io;

  u8 peer_id_bytes[TORRENT_PEER_ID_LEN];
  memset(peer_id_bytes, 'P', sizeof(peer_id_bytes));
  const Bytes peer_id = bytes_make(peer_id_bytes, sizeof(peer_id_bytes));

  // Two whole messages and the first three bytes of a third length prefix,
  // which is a cut no peer can be asked for but every network makes.
  const u8 trailing[] = {0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
                         0x00, 0x01, 0x00, 0x00, 0x00, 0x00};

  test_network_ctx_init(&network_ctx);

  u8 handshake[TEST_HANDSHAKE_LEN] = {0};
  test_handshake_fill(handshake, network_ctx.info_hash, peer_id);

  TestPeerCtx ctx = {0};
  const Env env = test_env_peer_make(&ctx);
  test_io_make(&test_io, &env, test_peer_perform, &ctx);

  TorrentPeer *const peer = torrent_peer_ctx_pool_acquire(&network_ctx.pool);
  assert(peer);
  const Ipv4Addr peer_addr = {.ip = 0x7f000001, .port = 6881};
  torrent_peer_init(peer, &test_io.io, &network_ctx, peer_addr, TEST_PEER_FD,
                    peer_addr.ip);

  // Our own handshake has gone out already, which is the state a connection is
  // in from its first tick onwards: the initial state is the one before
  // anything has been sent, and it never holds bytes.
  peer->state = TorrentPeerStateSentHandshake;

  // The handshake, two messages and a fragment, as one read would have left
  // them. The tick is called by hand because the point is the buffer it works
  // on, and only a caller can put arbitrary bytes there.
  memcpy(peer->recv_buf, handshake, sizeof(handshake));
  memcpy(peer->recv_buf + sizeof(handshake), trailing, sizeof(trailing));
  peer->recv_len = sizeof(handshake) + sizeof(trailing);

  const i32 saved = test_stdout_silence();
  torrent_peer_tick(peer, &test_io.io, test_io.now_ns);
  test_stdout_restore(saved);

  // A handshake with bytes behind it is still a handshake: what follows is not
  // part of it, and does not make it the wrong length.
  assert(TorrentPeerStateHandshaked == peer->state);

  // The handshake and both whole messages are gone. What is left is the three
  // bytes of the next length prefix, at the front of the buffer, where the next
  // read appends to them.
  assert(3 == peer->recv_len);
  assert(0 == memcmp(peer->recv_buf, trailing + 10, 3));

  // Still waiting on that peer, with a read out for the rest of it.
  assert(peer->read_in_flight);
  assert(!peer->closing);
  assert(!test_peer_pool_is_empty(&network_ctx));

  // And the connection ends the ordinary way once the peer says nothing more.
  test_io_drain(&test_io);
  assert(1 == ctx.close_calls);
  assert(test_peer_pool_is_empty(&network_ctx));
}


// ---------- The peer as an actor ----------

// A peer holding only the fields `torrent_peer_run` reads. It touches no io, no
// buffers and no logger, and that is the property this whole test rests on:
// every case below is a value in and a value out.
__attribute__((warn_unused_result)) static TorrentPeer
test_peer_actor_make(TorrentPeerState state) {
  TorrentPeer peer = {0};
  peer.state = state;
  peer.idle_due_at = TORRENT_PEER_DEADLINE_NONE;
  peer.keep_alive_due_at = TORRENT_PEER_DEADLINE_NONE;
  peer.deadline_at = TORRENT_PEER_DEADLINE_NONE;

  return peer;
}

// A peer that handshook at `now_ns`, which is where every `Handshaked` case
// starts from.
__attribute__((warn_unused_result)) static TorrentPeer
test_peer_actor_handshaked(u64 now_ns) {
  TorrentPeer peer = test_peer_actor_make(TorrentPeerStateHandshaked);
  peer.idle_due_at = now_ns + TORRENT_PEER_IDLE_NS;
  peer.keep_alive_due_at = now_ns + TORRENT_PEER_KEEP_ALIVE_NS;
  peer.deadline_at = torrent_peer_deadline_earliest(&peer);

  return peer;
}

// One call, with the two things that hold of every one of them checked in
// passing: the batch is within its bound with nothing behind a `Close`, and
// `deadline_at` is left as the earliest of whatever the call decided on. The
// walk reads that field and nothing else recomputes it, so a call that left it
// stale is a peer woken at the wrong moment, or never.
__attribute__((warn_unused_result)) static u32
test_peer_actor_run(TorrentPeer *peer, TorrentEventKind kind, u64 now_ns) {
  assert(peer);

  const u32 commands_len =
      torrent_peer_run(peer, (TorrentEvent){.kind = kind}, now_ns);

  assert(commands_len <= TORRENT_PEER_COMMANDS_MAX);
  assert(peer->deadline_at == torrent_peer_deadline_earliest(peer));

  for (u32 i = 0; i + 1 < commands_len; i++) {
    assert(TorrentCommandKindClose != peer->commands[i].kind);
  }

  return commands_len;
}

// The whole of a peer's decision making, called directly. There is no io here
// and nothing to drive: the point of the actor is that its answer is decided by
// the state, the event and the time alone, so this is a table of those three
// against what comes back.
static void test_torrent_peer_run(void) {
  // Not zero, so that a deadline left unset cannot pass for one that happens to
  // be due, and far enough in that a case can look backwards from it.
  const u64 now = 1000 * Second;

  // The earliest of nothing is nothing, which is what a peer with no deadline at
  // all answers and what the loop reads as "wait for the ceiling".
  {
    const TorrentPeer peer = test_peer_actor_make(TorrentPeerStateInitial);
    assert(TORRENT_PEER_DEADLINE_NONE == torrent_peer_deadline_earliest(&peer));
  }

  // Which commands put bytes on the wire. That is what moves the keep-alive
  // deadline, and a wrong answer here is a peer we go quiet on.
  assert(torrent_command_kind_sends(TorrentCommandKindSendHandshake));
  assert(torrent_command_kind_sends(TorrentCommandKindSendKeepAlive));
  assert(!torrent_command_kind_sends(TorrentCommandKindClose));
  assert(!torrent_command_kind_sends(TorrentCommandKindNone));

  // The transport having a connection is the only thing the initial state acts
  // on, and answering one is the same thing to a peer as asking for one.
  {
    const TorrentEventKind starts[] = {TorrentEventKindAccepted,
                                       TorrentEventKindConnected};

    for (usize i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
      TorrentPeer peer = test_peer_actor_make(TorrentPeerStateInitial);

      assert(1 == test_peer_actor_run(&peer, starts[i], now));
      assert(TorrentCommandKindSendHandshake == peer.commands[0].kind);
      assert(TorrentPeerStateSentHandshake == peer.state);

      // The tight interval and not the four minutes a handshaked peer gets: a
      // peer that connects and then says nothing must not hold a pool slot.
      assert(now + TORRENT_PEER_HANDSHAKE_NS == peer.idle_due_at);
      // And the handshake going out is this process saying something, which is
      // what the other deadline measures.
      assert(now + TORRENT_PEER_KEEP_ALIVE_NS == peer.keep_alive_due_at);
      assert(peer.idle_due_at == peer.deadline_at);
    }
  }

  // Anything else there cannot have happened before there is a connection, so
  // there is nothing to do with it but hang up.
  {
    const TorrentEventKind rest[] = {
        TorrentEventKindHangup, TorrentEventKindMalformed,
        TorrentEventKindHandshake, TorrentEventKindMessage,
        TorrentEventKindDeadline};

    for (usize i = 0; i < sizeof(rest) / sizeof(rest[0]); i++) {
      TorrentPeer peer = test_peer_actor_make(TorrentPeerStateInitial);

      assert(1 == test_peer_actor_run(&peer, rest[i], now));
      assert(TorrentCommandKindClose == peer.commands[0].kind);
      assert(TorrentPeerStateInitial == peer.state);
      // A peer on its way out is not waited on, and a `Close` is not this
      // process saying anything either.
      assert(TORRENT_PEER_DEADLINE_NONE == peer.deadline_at);
    }
  }

  // Their handshake, which is the one thing that state is waiting for.
  {
    TorrentPeer peer = test_peer_actor_make(TorrentPeerStateSentHandshake);
    peer.idle_due_at = now + TORRENT_PEER_HANDSHAKE_NS;
    peer.keep_alive_due_at = now + TORRENT_PEER_KEEP_ALIVE_NS;
    peer.deadline_at = torrent_peer_deadline_earliest(&peer);

    const u64 later = now + 5 * Second;

    // `interested` says we want what they have, `unchoke` says they may ask
    // for what we have. A bitfield will join them, once there are pieces to
    // announce.
    assert(2 == test_peer_actor_run(&peer, TorrentEventKindHandshake, later));
    assert(TorrentCommandKindInterested == peer.commands[0].kind);
    assert(TorrentCommandKindUnchoke == peer.commands[1].kind);
    assert(TorrentPeerStateHandshaked == peer.state);

    // Silence is allowed from here on, so the interval opens up.
    assert(later + TORRENT_PEER_IDLE_NS == peer.idle_due_at);
    assert(later + TORRENT_PEER_KEEP_ALIVE_NS == peer.keep_alive_due_at);
    // Keep-alive is the nearer of the two, which is the whole point of idle
    // being twice it: there is room for one to go out and be answered before the
    // close, and one lost keep-alive is survivable.
    assert(peer.keep_alive_due_at == peer.deadline_at);
  }

  // And anything that is not their handshake ends it, silence included. That is
  // what one idle deadline carrying a tighter interval buys over a handshake
  // deadline beside it: `Deadline` needs no case of its own here.
  {
    const TorrentEventKind rest[] = {
        TorrentEventKindAccepted, TorrentEventKindConnected,
        TorrentEventKindHangup,   TorrentEventKindMalformed,
        TorrentEventKindMessage,  TorrentEventKindDeadline};

    for (usize i = 0; i < sizeof(rest) / sizeof(rest[0]); i++) {
      TorrentPeer peer = test_peer_actor_make(TorrentPeerStateSentHandshake);
      peer.idle_due_at = now + TORRENT_PEER_HANDSHAKE_NS;
      peer.deadline_at = torrent_peer_deadline_earliest(&peer);

      assert(1 == test_peer_actor_run(&peer, rest[i], now + Second));
      assert(TorrentCommandKindClose == peer.commands[0].kind);
      assert(TorrentPeerStateSentHandshake == peer.state);
    }
  }

  // A message is the peer being alive, and that is the whole of the re-arm.
  {
    TorrentPeer peer = test_peer_actor_handshaked(now);
    const u64 later = now + 3 * Minute;

    // Past the keep-alive instant and still nothing goes out: only a `Deadline`
    // acts on a deadline, which is what stops a busy peer from earning a
    // keep-alive per message.
    assert(later > peer.keep_alive_due_at);
    assert(later < peer.idle_due_at);
    assert(0 == test_peer_actor_run(&peer, TorrentEventKindMessage, later));

    assert(later + TORRENT_PEER_IDLE_NS == peer.idle_due_at);
    // Our own silence is unchanged: what the peer said is not us saying
    // anything, and resetting this on what arrives would mean a peer that floods
    // us never hears from us and closes us.
    assert(now + TORRENT_PEER_KEEP_ALIVE_NS == peer.keep_alive_due_at);
    assert(peer.keep_alive_due_at == peer.deadline_at);
  }

  // Nothing heard in four minutes, so the connection is not worth its slot. Both
  // deadlines are past by then and only one answer comes back: bytes sent to a
  // peer that is about to be hung up on are bytes for nothing.
  {
    TorrentPeer peer = test_peer_actor_handshaked(now);
    const u64 later = peer.idle_due_at;

    assert(later > peer.keep_alive_due_at);
    assert(1 == test_peer_actor_run(&peer, TorrentEventKindDeadline, later));
    assert(TorrentCommandKindClose == peer.commands[0].kind);
    // Nothing went out, so nothing re-armed our own silence either.
    assert(now + TORRENT_PEER_KEEP_ALIVE_NS == peer.keep_alive_due_at);
  }

  // Two minutes of us saying nothing, which is the one thing a keep-alive is
  // for. The close is still two minutes off, so it is not that.
  {
    TorrentPeer peer = test_peer_actor_handshaked(now);
    const u64 later = peer.keep_alive_due_at;

    assert(later < peer.idle_due_at);
    assert(1 == test_peer_actor_run(&peer, TorrentEventKindDeadline, later));
    assert(TorrentCommandKindSendKeepAlive == peer.commands[0].kind);

    // Re-armed from now, and the peer's own silence left alone: our keep-alive
    // must not hold a dead peer open.
    assert(later + TORRENT_PEER_KEEP_ALIVE_NS == peer.keep_alive_due_at);
    assert(now + TORRENT_PEER_IDLE_NS == peer.idle_due_at);
    // Which makes the close the nearer of the two from here on.
    assert(peer.idle_due_at == peer.deadline_at);
  }

  // A deadline event with nothing actually due. The walk does not produce one,
  // comparing before it calls, but the answer has to be "nothing" rather than a
  // close: one missed comparison would otherwise hang up on every peer at once.
  {
    TorrentPeer peer = test_peer_actor_handshaked(now);
    const u64 later = now + Second;

    assert(0 == test_peer_actor_run(&peer, TorrentEventKindDeadline, later));
    assert(now + TORRENT_PEER_IDLE_NS == peer.idle_due_at);
    assert(now + TORRENT_PEER_KEEP_ALIVE_NS == peer.keep_alive_due_at);
  }

  // The transport going, and the peer talking nonsense: two different facts with
  // the one answer, and both of them a command rather than an error, which is
  // what lets this return a count and never an `Error`.
  {
    const TorrentEventKind overs[] = {TorrentEventKindHangup,
                                      TorrentEventKindMalformed};

    for (usize i = 0; i < sizeof(overs) / sizeof(overs[0]); i++) {
      TorrentPeer peer = test_peer_actor_handshaked(now);

      assert(1 == test_peer_actor_run(&peer, overs[i], now + Second));
      assert(TorrentCommandKindClose == peer.commands[0].kind);
      assert(TorrentPeerStateHandshaked == peer.state);
    }
  }
}

// A recognisable instant to start from: not zero, so that a deadline left unset
// cannot pass for one that is genuinely due, and far enough in that a case can
// look backwards from it.
#define TEST_PEER_NOW_NS (1000 * Second)

// A connection in `Handshaked` with a read outstanding and nothing more to say,
// which is the state every deadline below is measured from. The slot is the
// pool's first, these starting with it empty.
__attribute__((warn_unused_result)) static TorrentPeer *
test_peer_handshaked(TestIo *test_io, TestPeerCtx *ctx, const Env *env,
                     TorrentNetworkCtx *network_ctx, Bytes handshake) {
  assert(test_io);
  assert(ctx);
  assert(network_ctx);
  assert(test_peer_pool_is_empty(network_ctx));

  // One read per turn gets one thing: their handshake, then their keep-alive.
  // Without the cap a single read takes both, and the second turn below would
  // find end of file where a live peer's silence should be.
  *ctx = (TestPeerCtx){.says = handshake, .chunk = TEST_HANDSHAKE_LEN};
  test_io_make(test_io, env, test_peer_perform, ctx);
  test_io->now_ns = TEST_PEER_NOW_NS;

  const i32 saved = test_stdout_silence();
  torrent_peer_on_accept(&test_io->io, network_ctx,
                         (Ipv4Addr){.ip = 0x7f000001, .port = 6881},
                         TEST_PEER_FD);
  // One turn is the whole of it: ours goes out and theirs comes back without
  // the loop being turned in between, and what is left outstanding is the read
  // waiting for a first message that never comes.
  const Error err = test_io->io.run_for_ns(&test_io->io, 1);
  assert(ErrKindNone == err.kind);

  TorrentPeer *const peer = &network_ctx->pool.slots[0];
  assert(TorrentPeerStateHandshaked == peer->state);
  assert(peer->read_in_flight);
  assert(!peer->closing);
  assert(1 == ctx->write_calls);
  assert(1 == ctx->read_calls);

  // The greeting was queued in that same turn and submitted by the pump, so it
  // takes one more turn to reach the wire. Turning it here leaves the send
  // buffer empty, so that every byte the deadline tests see afterwards is one a
  // deadline put there. The read armed beside it runs in that turn too, which
  // is why the script has one keep-alive left to answer it with: at end of file
  // the connection would end here instead.
  const Error err_greeting = test_io->io.run_for_ns(&test_io->io, 1);
  assert(ErrKindNone == err_greeting.kind);
  test_stdout_restore(saved);

  assert(2 == ctx->write_calls);
  assert(2 == ctx->read_calls);
  assert(0 == peer->send_len);
  assert(!peer->write_in_flight);
  assert(peer->read_in_flight);

  // The bytes themselves, in order.
  assert(TEST_PEER_GREETING_LEN == ctx->written_len);
  assert(0 == memcmp(ctx->written + TEST_HANDSHAKE_LEN, test_peer_greeting,
                     sizeof(test_peer_greeting)));

  // Both were set the moment the handshake arrived, and no time has passed
  // since.
  assert(TEST_PEER_NOW_NS + TORRENT_PEER_IDLE_NS == peer->idle_due_at);
  assert(TEST_PEER_NOW_NS + TORRENT_PEER_KEEP_ALIVE_NS ==
         peer->keep_alive_due_at);
  assert(peer->keep_alive_due_at == peer->deadline_at);

  return peer;
}

// Finish whatever is outstanding and insist the slot came back. Every case below
// ends here: a connection lets go of its slot however it ended, and one left
// behind is a slot lost for the life of the process.
static void test_peer_drain_empty(TestIo *test_io,
                                  TorrentNetworkCtx *network_ctx) {
  assert(test_io);
  assert(network_ctx);

  const i32 saved = test_stdout_silence();
  test_io_drain(test_io);
  test_stdout_restore(saved);

  assert(test_peer_pool_is_empty(network_ctx));
}

// The walk that finds due peers, and the wait it sizes. This is what a timer
// library would have been here, and what it replaces is `uv_timer`: not a
// platform abstraction but a min-heap peeked to size the one timeout argument
// `kevent` and `epoll_wait` already take. Here the peeking is a comparison per
// occupied slot and the arming is an assignment, so there is no structure to
// keep in sync at all.
static void test_torrent_peer_deadlines(void) {
  static TorrentNetworkCtx network_ctx;
  static TestIo test_io;

  u8 peer_id_bytes[TORRENT_PEER_ID_LEN];
  memset(peer_id_bytes, 'P', sizeof(peer_id_bytes));

  test_network_ctx_init(&network_ctx);

  // The four zero bytes a keep-alive is: a length prefix of nothing, with no tag
  // behind it. Spelled out rather than taken from the code that writes them.
  const u8 keep_alive[] = {0x00, 0x00, 0x00, 0x00};

  // Their handshake and one keep-alive behind it, then nothing: what a peer
  // that is there but has nothing to say sounds like. The keep-alive is what
  // `test_peer_handshaked`'s second turn reads; every turn after it is end of
  // file, which is how each block below gets its slot back.
  u8 handshake[TEST_HANDSHAKE_LEN + sizeof(keep_alive)] = {0};
  test_handshake_fill(handshake, network_ctx.info_hash,
                      bytes_make(peer_id_bytes, sizeof(peer_id_bytes)));
  memcpy(handshake + TEST_HANDSHAKE_LEN, keep_alive, sizeof(keep_alive));
  const Bytes whole = bytes_make(handshake, sizeof(handshake));

  // The wait, on its own: the nearest deadline when it is nearer than the
  // ceiling, the ceiling when it is not, and the ceiling again when there is no
  // deadline at all. Zero is what must never come back, a loop that waits for no
  // time being a loop that spins.
  {
    const u64 now = TEST_PEER_NOW_NS;

    assert(1 == torrent_peers_wait_ns(now + 1, now));
    assert(TORRENT_PEER_LOOP_WAIT_NS_MAX - 1 ==
           torrent_peers_wait_ns(now + TORRENT_PEER_LOOP_WAIT_NS_MAX - 1, now));
    assert(TORRENT_PEER_LOOP_WAIT_NS_MAX ==
           torrent_peers_wait_ns(now + TORRENT_PEER_LOOP_WAIT_NS_MAX, now));
    assert(TORRENT_PEER_LOOP_WAIT_NS_MAX ==
           torrent_peers_wait_ns(now + TORRENT_PEER_KEEP_ALIVE_NS, now));
    assert(TORRENT_PEER_LOOP_WAIT_NS_MAX ==
           torrent_peers_wait_ns(TORRENT_PEER_DEADLINE_NONE, now));
  }

  // An empty pool: nothing to walk, and nothing to wait for either.
  {
    TestPeerCtx ctx = {0};
    const Env env = test_env_peer_make(&ctx);
    test_io_make(&test_io, &env, test_peer_perform, &ctx);

    assert(test_peer_pool_is_empty(&network_ctx));
    assert(TORRENT_PEER_DEADLINE_NONE ==
           torrent_peers_deadlines_run(&network_ctx, &test_io.io,
                                       TEST_PEER_NOW_NS));
  }

  // A second after the handshake nothing is due, so nothing is called at all and
  // the answer is what the loop waits on: the keep-alive, the nearer of the two.
  {
    TestPeerCtx ctx = {0};
    const Env env = test_env_peer_make(&ctx);
    TorrentPeer *const peer =
        test_peer_handshaked(&test_io, &ctx, &env, &network_ctx, whole);

    test_io.now_ns = TEST_PEER_NOW_NS + Second;
    assert(peer->keep_alive_due_at ==
           torrent_peers_deadlines_run(&network_ctx, &test_io.io,
                                       test_io.now_ns));

    // Nothing went out and nothing moved.
    assert(2 == ctx.write_calls);
    assert(TEST_PEER_GREETING_LEN == ctx.written_len);
    assert(TEST_PEER_NOW_NS + TORRENT_PEER_KEEP_ALIVE_NS ==
           peer->keep_alive_due_at);
    assert(!peer->closing);

    test_peer_drain_empty(&test_io, &network_ctx);
    assert(1 == ctx.close_calls);
  }

  // Two minutes of silence in both directions: the keep-alive is due, the close
  // is not, and four zero bytes go out.
  {
    TestPeerCtx ctx = {0};
    const Env env = test_env_peer_make(&ctx);
    TorrentPeer *const peer =
        test_peer_handshaked(&test_io, &ctx, &env, &network_ctx, whole);

    test_io.now_ns = peer->keep_alive_due_at;
    const u64 earliest = torrent_peers_deadlines_run(
        &network_ctx, &test_io.io, test_io.now_ns);

    // Re-armed from now, and the peer's own silence left alone: exactly one
    // keep-alive interval is left before the close, which is what it buys.
    assert(test_io.now_ns + TORRENT_PEER_KEEP_ALIVE_NS ==
           peer->keep_alive_due_at);
    assert(TEST_PEER_NOW_NS + TORRENT_PEER_IDLE_NS == peer->idle_due_at);
    assert(peer->idle_due_at == earliest);

    // Queued and submitted by the walk; performed by the loop, and not before.
    assert(peer->write_in_flight);
    assert(2 == ctx.write_calls);

    test_peer_drain_empty(&test_io, &network_ctx);

    assert(3 == ctx.write_calls);
    assert(TEST_PEER_GREETING_LEN + sizeof(keep_alive) == ctx.written_len);
    assert(0 == memcmp(ctx.written + TEST_PEER_GREETING_LEN, keep_alive,
                       sizeof(keep_alive)));
    assert(1 == ctx.close_calls);
  }

  // Four minutes of it, and the connection is not worth its slot.
  {
    TestPeerCtx ctx = {0};
    const Env env = test_env_peer_make(&ctx);
    TorrentPeer *const peer =
        test_peer_handshaked(&test_io, &ctx, &env, &network_ctx, whole);

    test_io.now_ns = peer->idle_due_at;
    assert(test_io.now_ns > peer->keep_alive_due_at);

    // Nothing is left to wait for: the one peer in the pool is on its way out.
    assert(TORRENT_PEER_DEADLINE_NONE ==
           torrent_peers_deadlines_run(&network_ctx, &test_io.io,
                                       test_io.now_ns));

    // The hang-up is waiting on the read that is still with the kernel, so the
    // slot is still held and `IO`'s close has not been asked for yet.
    assert(peer->closing);
    assert(0 == ctx.close_calls);
    assert(!test_peer_pool_is_empty(&network_ctx));

    // A second walk is not fooled by that either: a peer whose hang-up is in
    // flight is past being told anything, and waiting on its deadline would be
    // waiting for something nothing will ever act on.
    assert(TORRENT_PEER_DEADLINE_NONE ==
           torrent_peers_deadlines_run(&network_ctx, &test_io.io,
                                       test_io.now_ns));

    test_peer_drain_empty(&test_io, &network_ctx);

    assert(1 == ctx.close_calls);
    // Only the greeting ever went out: no keep-alive on the way to the close.
    assert(2 == ctx.write_calls);
    assert(TEST_PEER_GREETING_LEN == ctx.written_len);
  }

  // The whole life of a silent peer: one keep-alive at two minutes, a close at
  // four. The keep-alive going out must not push the close back -- `idle_due_at`
  // measures what the peer has not said, and this process saying something is
  // not the peer saying something.
  {
    TestPeerCtx ctx = {0};
    const Env env = test_env_peer_make(&ctx);
    TorrentPeer *const peer =
        test_peer_handshaked(&test_io, &ctx, &env, &network_ctx, whole);

    test_io.now_ns = TEST_PEER_NOW_NS + TORRENT_PEER_KEEP_ALIVE_NS;
    assert(peer->idle_due_at ==
           torrent_peers_deadlines_run(&network_ctx, &test_io.io,
                                       test_io.now_ns));
    assert(TEST_PEER_NOW_NS + TORRENT_PEER_IDLE_NS == peer->idle_due_at);

    test_io.now_ns = TEST_PEER_NOW_NS + TORRENT_PEER_IDLE_NS;
    assert(TORRENT_PEER_DEADLINE_NONE ==
           torrent_peers_deadlines_run(&network_ctx, &test_io.io,
                                       test_io.now_ns));
    assert(peer->closing);

    test_peer_drain_empty(&test_io, &network_ctx);

    // The one keep-alive, and nothing after it.
    assert(TEST_PEER_GREETING_LEN + sizeof(keep_alive) == ctx.written_len);
    assert(1 == ctx.close_calls);
  }

  // A keep-alive with no room for it, and one with just enough. A send buffer
  // this full means there are already bytes waiting to go out, which is the
  // opposite of the silence a keep-alive is there to break: not sending one says
  // nothing untrue, and the deadline has moved on either way.
  {
    TestPeerCtx ctx = {0};
    const Env env = test_env_peer_make(&ctx);
    test_io_make(&test_io, &env, test_peer_perform, &ctx);

    assert(test_peer_pool_is_empty(&network_ctx));
    TorrentPeer *const peer = torrent_peer_ctx_pool_acquire(&network_ctx.pool);
    assert(peer);

    const Ipv4Addr addr = {.ip = 0x7f000001, .port = 6881};
    const i32 saved = test_stdout_silence();
    torrent_peer_init(peer, &test_io.io, &network_ctx, addr, TEST_PEER_FD,
                      addr.ip);
    peer->state = TorrentPeerStateHandshaked;

    // One byte short of enough.
    peer->send_len = TORRENT_PEER_SEND_BUF_CAP - sizeof(keep_alive) + 1;
    torrent_peer_queue_keep_alive(peer);
    assert(TORRENT_PEER_SEND_BUF_CAP - sizeof(keep_alive) + 1 ==
           peer->send_len);

    // And exactly enough, which fills the buffer to the brim.
    peer->send_len = TORRENT_PEER_SEND_BUF_CAP - sizeof(keep_alive);
    torrent_peer_queue_keep_alive(peer);
    test_stdout_restore(saved);

    assert(TORRENT_PEER_SEND_BUF_CAP == peer->send_len);
    assert(0 == memcmp(peer->send_buf + TORRENT_PEER_SEND_BUF_CAP -
                           sizeof(keep_alive),
                       keep_alive, sizeof(keep_alive)));

    // Nothing was ever submitted for this one, so the slot goes back by hand
    // rather than through a hang-up there is nothing to hang up on.
    torrent_peer_ctx_pool_release(&network_ctx.pool, peer);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // The four messages that are a tag and nothing else, byte for byte, and the
  // edge of the send buffer they are written at. The room check has to count
  // the length prefix as well as the tag: counting only the tag leaves four
  // bytes looking like enough, and the fifth then lands past the buffer.
  {
    TestPeerCtx ctx = {0};
    const Env env = test_env_peer_make(&ctx);
    test_io_make(&test_io, &env, test_peer_perform, &ctx);

    assert(test_peer_pool_is_empty(&network_ctx));
    TorrentPeer *const peer = torrent_peer_ctx_pool_acquire(&network_ctx.pool);
    assert(peer);

    const Ipv4Addr addr = {.ip = 0x7f000001, .port = 6881};
    const i32 saved = test_stdout_silence();
    torrent_peer_init(peer, &test_io.io, &network_ctx, addr, TEST_PEER_FD,
                      addr.ip);
    peer->state = TorrentPeerStateHandshaked;

    const TorrentMessageKind kinds[] = {
        TorrentMessageKindChoke, TorrentMessageKindUnchoke,
        TorrentMessageKindInterested, TorrentMessageKindUninterested};

    // One after another, so that the second is written where the first left off
    // and not over it.
    for (usize i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
      torrent_peer_queue_msg_empty(peer, kinds[i]);

      const u8 *const written =
          peer->send_buf + i * TORRENT_PEER_MSG_EMPTY_LEN;
      // Three zero bytes and a one: the length, counting the tag behind it.
      assert(0x00 == written[0]);
      assert(0x00 == written[1]);
      assert(0x00 == written[2]);
      assert(0x01 == written[3]);
      assert(kinds[i] == written[4]);
      assert((i + 1) * TORRENT_PEER_MSG_EMPTY_LEN == peer->send_len);
    }

    // One byte short of enough: nothing is written, not even the part that
    // would have fit. Half a message on the wire is worse than none, the peer
    // reading whatever follows as its body.
    peer->send_len = TORRENT_PEER_SEND_BUF_CAP - TORRENT_PEER_MSG_EMPTY_LEN + 1;
    torrent_peer_queue_msg_empty(peer, TorrentMessageKindUnchoke);
    assert(TORRENT_PEER_SEND_BUF_CAP - TORRENT_PEER_MSG_EMPTY_LEN + 1 ==
           peer->send_len);

    // And exactly enough, which fills the buffer to the brim.
    peer->send_len = TORRENT_PEER_SEND_BUF_CAP - TORRENT_PEER_MSG_EMPTY_LEN;
    torrent_peer_queue_msg_empty(peer, TorrentMessageKindInterested);
    test_stdout_restore(saved);

    assert(TORRENT_PEER_SEND_BUF_CAP == peer->send_len);
    assert(TorrentMessageKindInterested ==
           peer->send_buf[TORRENT_PEER_SEND_BUF_CAP - 1]);

    torrent_peer_ctx_pool_release(&network_ctx.pool, peer);
    assert(test_peer_pool_is_empty(&network_ctx));
  }

  // The walk itself, over slots spread across the bitset. Only the peer that is
  // due is called, and the answer is the minimum over the rest.
  {
    TestPeerCtx ctx = {0};
    const Env env = test_env_peer_make(&ctx);
    test_io_make(&test_io, &env, test_peer_perform, &ctx);
    test_io.now_ns = TEST_PEER_NOW_NS;

    assert(test_peer_pool_is_empty(&network_ctx));

    // Placed by hand rather than taken from the pool in order: one in the first
    // group of the bitset and one three groups along, so a walk that only looked
    // where it started would pass the first check below and fail the second.
    const usize slot_idxs[] = {1, 3 * POOL_SLOTS_PER_GROUP + 5};
    const Ipv4Addr addr = {.ip = 0x7f000001, .port = 6881};

    const i32 saved = test_stdout_silence();
    for (usize i = 0; i < sizeof(slot_idxs) / sizeof(slot_idxs[0]); i++) {
      const usize slot_idx = slot_idxs[i];
      network_ctx.pool.occupied[slot_idx / POOL_SLOTS_PER_GROUP] |=
          1ULL << (slot_idx % POOL_SLOTS_PER_GROUP);

      TorrentPeer *const peer = &network_ctx.pool.slots[slot_idx];
      torrent_peer_init(peer, &test_io.io, &network_ctx, addr, TEST_PEER_FD,
                        addr.ip);
      peer->state = TorrentPeerStateHandshaked;
    }

    TorrentPeer *const quiet = &network_ctx.pool.slots[slot_idxs[0]];
    TorrentPeer *const due = &network_ctx.pool.slots[slot_idxs[1]];

    // One with a minute to go, one with its keep-alive due this instant.
    quiet->idle_due_at = TEST_PEER_NOW_NS + Minute;
    quiet->keep_alive_due_at = TEST_PEER_NOW_NS + 2 * Minute;
    quiet->deadline_at = torrent_peer_deadline_earliest(quiet);

    due->idle_due_at = TEST_PEER_NOW_NS + 10 * Minute;
    due->keep_alive_due_at = TEST_PEER_NOW_NS;
    due->deadline_at = torrent_peer_deadline_earliest(due);

    const u64 earliest = torrent_peers_deadlines_run(&network_ctx, &test_io.io,
                                                     test_io.now_ns);

    // The one that was due said something; the one that was not was never
    // called, which is what comparing before calling is there to save.
    assert(sizeof(keep_alive) == due->send_len);
    assert(0 == memcmp(due->send_buf, keep_alive, sizeof(keep_alive)));
    assert(0 == quiet->send_len);
    assert(TEST_PEER_NOW_NS + TORRENT_PEER_KEEP_ALIVE_NS ==
           due->keep_alive_due_at);
    assert(TEST_PEER_NOW_NS + 2 * Minute == quiet->keep_alive_due_at);

    // And the answer is the nearest deadline left, which belongs to the peer
    // that was never called.
    assert(quiet->idle_due_at == earliest);

    // Both are long past by then, so both go and the pool is empty for whatever
    // runs next.
    test_io.now_ns = TEST_PEER_NOW_NS + 20 * Minute;
    assert(TORRENT_PEER_DEADLINE_NONE ==
           torrent_peers_deadlines_run(&network_ctx, &test_io.io,
                                       test_io.now_ns));
    test_stdout_restore(saved);

    test_peer_drain_empty(&test_io, &network_ctx);
    assert(2 == ctx.close_calls);
  }
}

static void test_arena_valloc(void) {
  const Env *const env = env_platform_make();

  // A request the kernel cannot satisfy. `mmap` reports `MAP_FAILED`, not
  // NULL, so this also pins down that conversion, and that the `ENOMEM` it
  // sets comes back as `ErrOOM` rather than a bare failure.
  Arena arena = {0};
  assert(ErrKindOOM == arena_valloc(env, (usize)1 << 62, &arena).kind);

  // A failed call leaves the caller's arena alone.
  assert(NULL == arena.start);
  assert(NULL == arena.end);
}

// The length prefix in front of every peer message, and every index and offset
// inside one: four bytes, most significant first, whatever the host's own order
// is.
static void test_bytes_consume_u32_be(void) {
  const struct {
    u8 bytes[8];
    usize len;
    bool expected;
    u32 value;
    usize left;
  } cases[] = {
      // Most significant byte first, so these bytes spell this number on a
      // little-endian machine and a big-endian one alike.
      {{0x01, 0x02, 0x03, 0x04}, 4, true, 0x01020304, 0},
      {{0x00, 0x00, 0x00, 0x00}, 4, true, 0, 0},
      // The length of a `choke`, which is the shortest message there is.
      {{0x00, 0x00, 0x00, 0x01}, 4, true, 1, 0},
      // The top bit, which a signed reader would lose.
      {{0x80, 0x00, 0x00, 0x00}, 4, true, 0x80000000, 0},
      {{0xff, 0xff, 0xff, 0xff}, 4, true, 0xffffffff, 0},
      // Only the first four are taken; the rest stay for the next reader.
      {{0xde, 0xad, 0xbe, 0xef, 0x0a, 0x0b}, 6, true, 0xdeadbeef, 2},
      // Too short is answered, not read: three bytes are not a length prefix,
      // and the input is left alone for whatever arrives next.
      {{0x01, 0x02, 0x03}, 3, false, 0, 3},
      {{0}, 0, false, 0, 0},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    u8 bytes[8] = {0};
    memcpy(bytes, cases[i].bytes, sizeof(bytes));
    Bytes input = bytes_make(bytes, cases[i].len);

    u32 got = 0xa5a5a5a5;
    assert(cases[i].expected == bytes_consume_u32_be(&input, &got));
    assert(cases[i].left == input.len);

    if (cases[i].expected) {
      assert(cases[i].value == got);
    } else {
      // Untouched when there was nothing to read, so a caller holding three
      // bytes of a length is not handed a number built out of two of them.
      assert(0xa5a5a5a5 == got);
    }
  }

  // Back to back, which is how a `request` is read: three of these in a row off
  // the one input.
  {
    u8 bytes[] = {0x00, 0x00, 0x00, 0x07, 0x00, 0x00,
                  0x40, 0x00, 0x00, 0x00, 0x40, 0x00};
    Bytes input = bytes_make(bytes, sizeof(bytes));

    u32 idx = 0;
    u32 begin = 0;
    u32 len = 0;
    assert(bytes_consume_u32_be(&input, &idx));
    assert(bytes_consume_u32_be(&input, &begin));
    assert(bytes_consume_u32_be(&input, &len));
    assert(7 == idx);
    assert(0x4000 == begin);
    assert(0x4000 == len);
    assert(0 == input.len);
    // And nothing is left to read.
    assert(!bytes_consume_u32_be(&input, &idx));
  }
}

// One byte off the front. Its own test because it is one byte and not four: the
// tag of a peer message sits between the length and the payload, so reading one
// byte too many puts every field behind it out by three.
static void test_bytes_consume_u8(void) {
  // A whole `have`: length 5, tag 4, then the piece index.
  u8 bytes[] = {0x00, 0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x01, 0x00};
  Bytes input = bytes_make(bytes, sizeof(bytes));

  u32 len = 0;
  assert(bytes_consume_u32_be(&input, &len));
  assert(5 == len);

  u8 tag = 0;
  assert(bytes_consume_u8(&input, &tag));
  assert(TorrentMessageKindHave == tag);
  // One byte, so the four behind it are still there and start where they
  // should.
  assert(4 == input.len);

  u32 piece = 0;
  assert(bytes_consume_u32_be(&input, &piece));
  assert(256 == piece);
  assert(0 == input.len);

  // A single byte is enough for one, and nothing is enough for none.
  {
    u8 one[] = {0x2a};
    Bytes s1 = bytes_make(one, sizeof(one));
    u8 got = 0;
    assert(bytes_consume_u8(&s1, &got));
    assert(0x2a == got);
    assert(0 == s1.len);
    assert(!bytes_consume_u8(&s1, &got));
    // Untouched by the failure.
    assert(0x2a == got);
  }

  // The byte is optional: a caller that only wants it gone passes nothing.
  {
    u8 two[] = {0x01, 0x02};
    Bytes s2 = bytes_make(two, sizeof(two));
    assert(bytes_consume_u8(&s2, NULL));
    assert(1 == s2.len);
    assert(0x02 == s2.data[0]);
  }
}

static void test_bytes(void) {
  u8 data[] = {'a', 'b', 'c'};

  // bytes_first.
  {
    u8 first = 0xAA;
    assert(ErrKindNone != bytes_first(bytes_make(NULL, 0), &first).kind);
    assert(ErrKindNone != bytes_first(bytes_make(data, 0), &first).kind);
    // Nothing is written when there is no first byte.
    assert(0xAA == first);

    assert(ErrKindNone ==
           bytes_first(bytes_make(data, sizeof(data)), &first).kind);
    assert('a' == first);

    // Peeking does not consume.
    Bytes input = bytes_make(data, sizeof(data));
    assert(ErrKindNone == bytes_first(input, &first).kind);
    assert(sizeof(data) == input.len);
  }
  // bytes_take.
  {
    const Bytes input = bytes_make(data, sizeof(data));

    assert(0 == bytes_take(input, 0).len);
    assert(sizeof(data) == bytes_take(input, sizeof(data)).len);

    const Bytes taken = bytes_take(input, 2);
    assert(2 == taken.len);
    assert(data == taken.data);
  }
}

static void test_path_last_component(void) {
  // Expectations generated with Go's `path/filepath.Base`.
  const struct {
    const char *input;
    const char *expected;
  } cases[] = {
      // An empty path is the only input that yields ".".
      {"", "."},

      // A path of nothing but separators yields a single separator.
      {"/", "/"},
      {"//", "/"},
      {"///", "/"},

      // No separator at all: the whole path is the component.
      {"a", "a"},
      {"traces.jsonl.zip", "traces.jsonl.zip"},

      // The usual cases.
      {"/a", "a"},
      {"a/b", "b"},
      {"/a/b/c.zip", "c.zip"},
      {"./a", "a"},
      {"../a", "a"},

      // Trailing separators are removed first.
      {"a/", "a"},
      {"/a/b/", "b"},
      {"a/b///", "b"},

      // Repeated separators inside the path are not collapsed, but they
      // cannot produce an empty component either: the last one is always
      // followed by at least one byte.
      {"a//b", "b"},
      {"a//", "a"},

      // Dot components are returned as-is; this function resolves nothing.
      {".", "."},
      {"a/.", "."},
      {"/.", "."},
      {"/./", "."},
      {"..", ".."},
      {"../", ".."},
      {"a/..", ".."},
      {"/..", ".."},
      {"a/../", ".."},
      {"...", "..."},
      {"....", "...."},
      {"a/..b", "..b"},
      {"a/b..", "b.."},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const Bytes got = path_last_component(bytes_from_cstr(cases[i].input),
                                          PATH_SEPARATOR_UNIX);

    assert(!bytes_is_empty(got));
    assert(bytes_eq_cstr(got, cases[i].expected));
  }

  // A NULL `Bytes` is the same as an empty one.
  assert(bytes_eq_cstr(
      path_last_component(bytes_make(NULL, 0), PATH_SEPARATOR_UNIX), "."));

  // The result borrows from the input: no copy, and it is a suffix of the
  // path (after the trailing separators are removed).
  {
    const Bytes path = bytes_from_cstr("/a/b/c.zip");
    const Bytes got = path_last_component(path, PATH_SEPARATOR_UNIX);
    assert(path.data + path.len - got.len == got.data);
  }
  // The input is passed by value, so the caller's `Bytes` is untouched even
  // though the function trims trailing separators.
  {
    Bytes path = bytes_from_cstr("/a/b/");
    const Bytes got = path_last_component(path, PATH_SEPARATOR_UNIX);
    assert(bytes_eq_cstr(got, "b"));
    assert(5 == path.len);
  }
}

static void test_path_get_ext(void) {
  // Expectations generated with Go's `filepath.Ext`. A `NULL` expectation
  // means "no extension": `bytes_eq_cstr` reads it as "empty", and an
  // empty C string cannot be used here because a zero length one is not a
  // valid argument to it.
  const struct {
    const char *input;
    const char *expected;
  } cases[] = {
      // The usual case: the dot is part of the result.
      {"a.pdf", ".pdf"},
      {"/a/b/c.zip", ".zip"},
      {"dtrace_tips.pdf", ".pdf"},

      // No dot at all in the last component.
      {"a", NULL},
      {"Makefile", NULL},
      {"/a/b/c", NULL},

      // Only the last dot counts.
      {"a.b.c", ".c"},
      {"traces.jsonl.zip", ".zip"},

      // A trailing dot is an empty extension, which is not the same as no
      // extension: this is why the dot is kept in the result.
      {"a.", "."},

      // A dot in a directory is not an extension separator.
      {"/x/y.z/f", NULL},
      {"/x/y.z/f.pdf", ".pdf"},
      {"a.b/c", NULL},

      // A leading dot marks a hidden file, so the name is not an extension...
      {".bashrc", NULL},
      {"/a/.bashrc", NULL},
      // ... but a hidden file may still carry one of its own.
      {".config.json", ".json"},
      {"/a/.config.json", ".json"},

      // `.` and `..` are treated as ordinary names; see the note on
      // `path_with_ext`.
      {".", NULL},
      {"..", "."},

      // Nothing to extract from a path that has no last component.
      {"", NULL},
      {"/", NULL},
      {"//", NULL},
      {"a/", NULL},
      {"/a/b/", NULL},

      // Separators repeated inside the path are left alone.
      {"a//b.pdf", ".pdf"},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const Bytes path = bytes_from_cstr(cases[i].input);
    const Bytes got = path_get_ext(path, PATH_SEPARATOR_UNIX);

    assert(bytes_eq_cstr(got, cases[i].expected));

    // Nothing is copied: a non-empty result is always a suffix of the input.
    if (!bytes_is_empty(got)) {
      assert(got.data >= path.data);
      assert(got.data + got.len == path.data + path.len);
      assert('.' == got.data[0]);
    }
  }

  // A NULL `Bytes` is the same as an empty one.
  assert(
      bytes_is_empty(path_get_ext(bytes_make(NULL, 0), PATH_SEPARATOR_UNIX)));

  // The input is passed by value, so the caller's `Bytes` is untouched.
  {
    Bytes path = bytes_from_cstr("/a/b/c.zip");
    const Bytes got = path_get_ext(path, PATH_SEPARATOR_UNIX);
    assert(bytes_eq_cstr(got, ".zip"));
    assert(10 == path.len);
  }
}

static void test_path_with_ext(void) {
  // Expectations generated with Go's
  // `strings.TrimSuffix(p, filepath.Ext(p)) + "." + ext`.
  const struct {
    const char *input;
    const char *ext;
    const char *expected;
  } cases[] = {
      // The usual case: the last component has an extension, it is replaced.
      {"a.pdf", "torrent", "a.torrent"},
      {"dtrace_tips.pdf", "torrent", "dtrace_tips.torrent"},
      {"/a/b/c.zip", "torrent", "/a/b/c.torrent"},
      {"./a.pdf", "torrent", "./a.torrent"},
      {"../a.pdf", "torrent", "../a.torrent"},

      // No extension at all: one is appended rather than the call failing.
      {"a", "torrent", "a.torrent"},
      {"/a/b/c", "torrent", "/a/b/c.torrent"},
      {"Makefile", "torrent", "Makefile.torrent"},

      // Only the last dot counts, the earlier ones are part of the name.
      {"a.b.c", "torrent", "a.b.torrent"},
      {"traces.jsonl.zip", "torrent", "traces.jsonl.torrent"},

      // A trailing dot is an empty extension, and is still replaced.
      {"a.", "torrent", "a.torrent"},

      // A dot in a directory is not an extension separator: the last
      // component owns the extension, or has none.
      {"/x/y.z/f.pdf", "torrent", "/x/y.z/f.torrent"},
      {"/x/y.z/f", "torrent", "/x/y.z/f.torrent"},
      {"a.b/c", "torrent", "a.b/c.torrent"},

      // A leading dot marks a hidden file, not an empty stem, so the name
      // survives and the extension is appended.
      {".bashrc", "torrent", ".bashrc.torrent"},
      {"/a/.bashrc", "torrent", "/a/.bashrc.torrent"},
      // ... but a hidden file may still carry an extension of its own.
      {".config.json", "torrent", ".config.torrent"},
      {"/a/.config.json", "torrent", "/a/.config.torrent"},

      // `.` and `..` are treated as ordinary names; see the note on the
      // function. Pinned so the behaviour cannot drift unnoticed.
      {".", "torrent", "..torrent"},
      {"..", "torrent", "..torrent"},

      // The extension is copied verbatim: no dot is stripped from it, and a
      // single byte is as good as any other.
      {"a.pdf", "x", "a.x"},
      {"a.pdf", ".hidden", "a..hidden"},

      // Separators repeated inside the path are left alone.
      {"a//b.pdf", "torrent", "a//b.torrent"},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Arena arena = test_arena(256);
    Bytes got = {0};

    assert(ErrKindNone == path_with_ext(bytes_from_cstr(cases[i].input),
                                        bytes_from_cstr(cases[i].ext),
                                        PATH_SEPARATOR_UNIX, &got, &arena)
                              .kind);
    assert(bytes_eq_cstr(got, cases[i].expected));

    // The result is a fresh copy, never a view into the input.
    assert(got.data != bytes_from_cstr(cases[i].input).data);
  }

  // An empty path has no name to extend.
  {
    Arena arena = test_arena(256);
    Bytes got = {0};
    assert(ErrKindInvalidData ==
           path_with_ext(bytes_from_cstr(""), bytes_from_cstr("torrent"),
                         PATH_SEPARATOR_UNIX, &got, &arena)
               .kind);
    assert(bytes_is_empty(got));
  }

  // A NULL `Bytes` is the same as an empty one.
  {
    Arena arena = test_arena(256);
    Bytes got = {0};
    assert(ErrKindInvalidData ==
           path_with_ext(bytes_make(NULL, 0), bytes_from_cstr("torrent"),
                         PATH_SEPARATOR_UNIX, &got, &arena)
               .kind);
  }

  // A trailing separator leaves no last component.
  {
    const char *const inputs[] = {"/", "//", "a/", "/a/b/", "a/b///"};

    for (usize i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
      Arena arena = test_arena(256);
      Bytes got = {0};
      assert(ErrKindInvalidData == path_with_ext(bytes_from_cstr(inputs[i]),
                                                 bytes_from_cstr("torrent"),
                                                 PATH_SEPARATOR_UNIX, &got,
                                                 &arena)
                                       .kind);
    }
  }

  // Out of memory is reported rather than asserted, and leaves `dst` alone.
  {
    u8 mem[8] = {0};
    Arena arena = arena_from_mem(mem, sizeof(mem));
    Bytes got = {0};

    // "a.torrent" is nine bytes, one more than the arena holds.
    assert(ErrKindOOM == path_with_ext(bytes_from_cstr("a.pdf"),
                                       bytes_from_cstr("torrent"),
                                       PATH_SEPARATOR_UNIX, &got, &arena)
                             .kind);
    assert(bytes_is_empty(got));
  }

  // The input is passed by value and only read: the caller's `Bytes` and the
  // bytes behind it are untouched.
  {
    Arena arena = test_arena(256);
    char input[] = "/a/b/c.zip";
    const Bytes path = bytes_make((u8 *)input, sizeof(input) - 1);
    Bytes got = {0};

    assert(ErrKindNone == path_with_ext(path, bytes_from_cstr("torrent"),
                                        PATH_SEPARATOR_UNIX, &got, &arena)
                              .kind);
    assert(bytes_eq_cstr(got, "/a/b/c.torrent"));
    assert(bytes_eq_cstr(path, "/a/b/c.zip"));
    assert(0 == strcmp(input, "/a/b/c.zip"));
  }

  // The result is exactly as long as it claims: the arena bump matches the
  // reported length, so nothing is written past the end.
  {
    Arena arena = test_arena(256);
    const u8 *const before = arena.start;
    Bytes got = {0};

    assert(ErrKindNone == path_with_ext(bytes_from_cstr("a.pdf"),
                                        bytes_from_cstr("torrent"),
                                        PATH_SEPARATOR_UNIX, &got, &arena)
                              .kind);
    assert(9 == got.len);
    assert((usize)(arena.start - before) == got.len);
  }
}

static void test_ascii_num_parse(void) {
  const struct {
    const char *input;
    ErrorKind expected;
    usize num;
    // What is left of the input afterwards. Only meaningful on success.
    const char *remaining;
  } cases[] = {
      {"123e", ErrKindNone, 123, "e"},
      {"0e", ErrKindNone, 0, "e"},
      {"7:spam", ErrKindNone, 7, ":spam"},
      // The largest representable usize.
      {"18446744073709551615e", ErrKindNone, SIZE_MAX, "e"},
      // Unterminated: the digits run to the end of the input.
      {"123", ErrKindInvalidData, 0, NULL},
      {"", ErrKindInvalidData, 0, NULL},
      // No digit at all. The old API reported this as a success consuming
      // nothing and left it to the caller to notice.
      {"e", ErrKindInvalidData, 0, NULL},
      {":spam", ErrKindInvalidData, 0, NULL},
      {"-1e", ErrKindInvalidData, 0, NULL},
      // A single zero is fine, leading zeroes are not.
      {"0123e", ErrKindInvalidData, 0, NULL},
      {"00e", ErrKindInvalidData, 0, NULL},
      // Well formed but too wide for a `usize`, which is `ErrRange` rather
      // than malformed input. Overflow on the final add: SIZE_MAX + 1.
      {"18446744073709551616e", ErrKindRange, 0, NULL},
      // Overflow on the multiply: 20 nines.
      {"99999999999999999999e", ErrKindRange, 0, NULL},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Bytes data = bytes_make((u8 *)cases[i].input, strlen(cases[i].input));
    const Bytes before = data;

    usize num = 0xAA;
    assert(cases[i].expected == ascii_num_parse(&data, &num).kind);

    if (ErrKindNone != cases[i].expected) {
      // A failed parse consumes nothing and writes nothing.
      assert(before.data == data.data);
      assert(before.len == data.len);
      assert(0xAA == num);
      continue;
    }

    assert(cases[i].num == num);
    assert(strlen(cases[i].remaining) == data.len);
    assert(0 == memcmp(data.data, cases[i].remaining, data.len));
  }

  // No data at all.
  {
    Bytes data = bytes_make(NULL, 0);
    usize num = 0;
    assert(ErrKindInvalidData == ascii_num_parse(&data, &num).kind);
  }
}

static void test_bencode_parse_num(void) {
  const struct {
    const char *input;
    bool ok;
    isize num;
    // What the parser leaves behind for the caller.
    usize remaining;
  } cases[] = {
      {"i0e", true, 0, 0},
      {"i1e", true, 1, 0},
      {"i-1e", true, -1, 0},
      {"i-123e", true, -123, 0},
      {"i9223372036854775807e", true, SSIZE_MAX, 0},
      {"i-9223372036854775808e", true, -SSIZE_MAX - 1, 0},
      // Out of range for an isize.
      {"i9223372036854775808e", false, 0, 0},
      {"i-9223372036854775809e", false, 0, 0},
      {"i18446744073709551615e", false, 0, 0},
      // Out of range for a usize.
      {"i18446744073709551616e", false, 0, 0},
      // Malformed.
      {"i-0e", false, 0, 0},
      {"ie", false, 0, 0},
      {"i-e", false, 0, 0},
      {"i0123e", false, 0, 0},
      {"i123", false, 0, 0},
      {"i123x", false, 0, 0},
      {"i", false, 0, 0},
      {"", false, 0, 0},
      {"42e", false, 0, 0},
      // Trailing data is left for the caller.
      {"i42ei43e", true, 42, 4},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Bytes data = bytes_from_cstr(cases[i].input);

    // Poisoned so that a write on the failure path is visible.
    BencodeValue value = {.kind = BencodeKindDict};
    assert((ErrKindNone == bencode_parse_num(&data, &value).kind) ==
           cases[i].ok);

    if (!cases[i].ok) {
      // A failed parse consumes nothing and writes nothing.
      assert(strlen(cases[i].input) == data.len);
      assert(BencodeKindDict == value.kind);
      continue;
    }

    assert(BencodeKindInteger == value.kind);
    assert(cases[i].num == value.v.num);
    assert(cases[i].remaining == data.len);
  }
}

static void test_bencode_parse_bytes(void) {
  const struct {
    const char *input;
    bool ok;
    const char *expected;
    // What the parser leaves behind for the caller.
    usize remaining;
  } cases[] = {
      {"0:", true, "", 0},
      {"1:a", true, "a", 0},
      {"4:spam", true, "spam", 0},
      {"3:0:x", true, "0:x", 0},
      // Trailing data is left for the caller.
      {"4:spameggs", true, "spam", 4},
      // The length exceeds what is left.
      {"5:spam", false, NULL, 0},
      {"1:", false, NULL, 0},
      // Malformed.
      {"4spam", false, NULL, 0},
      {"4", false, NULL, 0},
      {"e", false, NULL, 0},
      {"", false, NULL, 0},
      {"04:spam", false, NULL, 0},
      {":spam", false, NULL, 0},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Bytes data = bytes_from_cstr(cases[i].input);

    // Poisoned so that a write on the failure path is visible.
    BencodeValue value = {.kind = BencodeKindDict};
    assert((ErrKindNone == bencode_parse_bytes(&data, &value).kind) ==
           cases[i].ok);

    if (!cases[i].ok) {
      // A failed parse consumes nothing and writes nothing.
      assert(strlen(cases[i].input) == data.len);
      assert(BencodeKindDict == value.kind);
      continue;
    }

    assert(BencodeKindBytes == value.kind);
    assert(strlen(cases[i].expected) == value.v.bytes.len);
    assert(0 ==
           memcmp(value.v.bytes.data, cases[i].expected, value.v.bytes.len));
    assert(cases[i].remaining == data.len);
  }

  // The bytes point into the input, they are not copied.
  {
    const char *const input = "4:spam";
    Bytes data = bytes_from_cstr(input);

    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse_bytes(&data, &value).kind);
    assert((u8 *)input + 2 == value.v.bytes.data);
  }
}

__attribute__((warn_unused_result)) static bool
test_bencode_is_bytes(BencodeValue value, const char *expected) {
  assert(expected);

  const usize len = strlen(expected);
  return BencodeKindBytes == value.kind && len == value.v.bytes.len &&
         (0 == len || 0 == memcmp(value.v.bytes.data, expected, len));
}

__attribute__((warn_unused_result)) static BencodeValue
test_bencode_make_bytes(const char *data, usize len) {
  if (0 != len) {
    assert(data);
  }

  return (BencodeValue){.kind = BencodeKindBytes,
                        .v.bytes = bytes_make((u8 *)data, len)};
}

static void test_bencode_parse(void) {
  const struct {
    const char *input;
    bool ok;
    BencodeKind kind;
    // Only meaningful for a list or a dict.
    usize children_len;
    // What the parser leaves behind for the caller.
    usize remaining;
  } cases[] = {
      // Scalars at the root: `bencode_parse` dispatches, the result is the
      // value itself and not a one-element container.
      {"i42e", true, BencodeKindInteger, 0, 0},
      {"i-1e", true, BencodeKindInteger, 0, 0},
      {"3:abc", true, BencodeKindBytes, 0, 0},
      {"0:", true, BencodeKindBytes, 0, 0},
      // Empty containers allocate nothing.
      {"le", true, BencodeKindList, 0, 0},
      {"de", true, BencodeKindDict, 0, 0},
      // Flat containers.
      {"l4:spami456ee", true, BencodeKindList, 2, 0},
      {"li1ei2ei3ee", true, BencodeKindList, 3, 0},
      {"d3:key5:valuee", true, BencodeKindDict, 2, 0},
      // Nesting: a closed container is one child of its parent, whatever it
      // holds.
      {"llee", true, BencodeKindList, 1, 0},
      {"lli1eee", true, BencodeKindList, 1, 0},
      {"ld1:a1:beli2eee", true, BencodeKindList, 2, 0},
      {"d1:ali1ei2ee1:bd1:ci3eee", true, BencodeKindDict, 4, 0},
      // Trailing data is left for the caller, exactly like the scalar
      // parsers.
      {"i42etrailing", true, BencodeKindInteger, 0, 8},
      {"lee", true, BencodeKindList, 0, 1},
      {"lei42e", true, BencodeKindList, 0, 4},
      // A dict holds key/value pairs, so an odd number of children is
      // malformed.
      {"d3:keye", false, 0, 0, 0},
      {"di1ee", false, 0, 0, 0},
      // Dict keys must be byte strings, sorted by raw byte value, with no
      // duplicates. `bencode_validate_dict` is applied as each dict closes.
      {"di1ei2ee", false, 0, 0, 0},
      {"d1:a1:x1:b1:ye", true, BencodeKindDict, 4, 0},
      {"d1:b1:x1:a1:ye", false, 0, 0, 0},
      {"d1:a1:x1:a1:ye", false, 0, 0, 0},
      // Including for a dict nested inside another one.
      {"d1:ad1:b1:c1:a1:dee", false, 0, 0, 0},
      {"d1:ad1:a1:c1:b1:dee", true, BencodeKindDict, 2, 0},
      // Unterminated containers, at every depth.
      {"l", false, 0, 0, 0},
      {"d", false, 0, 0, 0},
      {"li1e", false, 0, 0, 0},
      {"lli1ee", false, 0, 0, 0},
      {"d3:key5:value", false, 0, 0, 0},
      // A stray `e` closes nothing.
      {"e", false, 0, 0, 0},
      {"i42ee", true, BencodeKindInteger, 0, 1},
      // A malformed item aborts the whole parse, however deeply nested.
      {"li-0ee", false, 0, 0, 0},
      {"l5:spame", false, 0, 0, 0},
      {"lli0123eee", false, 0, 0, 0},
      {"ld1:a1:bi0123eee", false, 0, 0, 0},
      // Unknown characters, at the root and nested.
      {"x", false, 0, 0, 0},
      {"lxe", false, 0, 0, 0},
      {":", false, 0, 0, 0},
      {"-1e", false, 0, 0, 0},
      // Empty input.
      {"", false, 0, 0, 0},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);

    Bytes data = bytes_from_cstr(cases[i].input);
    u8 *const arena_start = arena.start;

    BencodeValue value = {0};
    assert((ErrKindNone ==
            bencode_parse(&data, &arena, scratch, &value).kind) == cases[i].ok);

    if (!cases[i].ok) {
      // A failed parse rolls back the input and the output arena both.
      assert(strlen(cases[i].input) == data.len);
      assert(arena_start == arena.start);
      continue;
    }

    assert(cases[i].kind == value.kind);
    assert(cases[i].remaining == data.len);

    if (BencodeKindList == value.kind || BencodeKindDict == value.kind) {
      assert(cases[i].children_len == value.v.list.len);
      // An empty container owns no allocation at all.
      if (0 != value.v.list.len) {
        assert(value.v.list.data);
      }
    }
  }

  // Every digit dispatches to the bytes parser.
  {
    for (u8 c = '0'; c <= '9'; c++) {
      Arena arena = test_arena(1 * KiB);
      Arena scratch = test_arena(1 * KiB);

      const char input[] = {(char)c, ':', 0};
      Bytes data = bytes_from_cstr(input);

      BencodeValue value = {0};
      // Only `0:` has a body short enough to succeed.
      assert(
          (ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind) ==
          ('0' == c));
    }
  }

  // The whole tree, spelled out: children are stored in input order and the
  // nested containers point at their own right-sized allocations.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);

    Bytes data = bytes_from_cstr("ld1:a1:beli2eee");
    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind);
    assert(BencodeKindList == value.kind);
    assert(2 == value.v.list.len);

    const BencodeValue dict = value.v.list.data[0];
    assert(BencodeKindDict == dict.kind);
    assert(2 == dict.v.list.len);
    assert(test_bencode_is_bytes(dict.v.list.data[0], "a"));
    assert(test_bencode_is_bytes(dict.v.list.data[1], "b"));

    const BencodeValue list = value.v.list.data[1];
    assert(BencodeKindList == list.kind);
    assert(1 == list.v.list.len);
    assert(BencodeKindInteger == list.v.list.data[0].kind);
    assert(2 == list.v.list.data[0].v.num);
  }

  // Byte strings point into the input, they are not copied.
  {
    Arena arena = test_arena(1 * KiB);
    Arena scratch = test_arena(1 * KiB);

    const char *const input = "l4:spame";
    Bytes data = bytes_from_cstr(input);
    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind);
    assert((u8 *)input + 3 == value.v.list.data[0].v.bytes.data);
  }

  // Nesting is bounded, and the bound is not off by one.
  {
    // `BENCODE_MAX_DEPTH` nested lists, then one more.
    for (usize depth = BENCODE_MAX_DEPTH; depth <= BENCODE_MAX_DEPTH + 1;
         depth++) {
      Arena arena = test_arena(16 * KiB);
      Arena scratch = test_arena(16 * KiB);

      u8 input[2 * (BENCODE_MAX_DEPTH + 1)] = {0};
      memset(input, 'l', depth);
      memset(input + depth, 'e', depth);

      Bytes data = bytes_make(input, 2 * depth);
      BencodeValue value = {0};
      const bool ok =
          ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind;

      assert(ok == (depth <= BENCODE_MAX_DEPTH));
      if (ok) {
        assert(BencodeKindList == value.kind);
        assert(1 == value.v.list.len);
      }
    }
  }

  // A long flat list: `values` is sized from the input length, so this is the
  // case that comes closest to filling it.
  {
    const usize children_len = 200;
    Arena arena = test_arena(64 * KiB);
    Arena scratch = test_arena(64 * KiB);

    u8 input[1 + 2 * 200 + 1] = {0};
    input[0] = 'l';
    for (usize i = 0; i < children_len; i++) {
      input[1 + 2 * i] = '0';
      input[1 + 2 * i + 1] = ':';
    }
    input[1 + 2 * children_len] = 'e';

    Bytes data = bytes_make(input, sizeof(input));
    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind);
    assert(BencodeKindList == value.kind);
    assert(children_len == value.v.list.len);

    for (usize i = 0; i < children_len; i++) {
      assert(test_bencode_is_bytes(value.v.list.data[i], ""));
    }
  }

  // Out of scratch: the scratch arena cannot even hold the `values` array.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(8);

    Bytes data = bytes_from_cstr("li1ei2ee");
    BencodeValue value = {0};
    assert(ErrKindNone != bencode_parse(&data, &arena, scratch, &value).kind);
  }
  // Out of output arena: the children of a container do not fit.
  {
    Arena arena = test_arena(8);
    Arena scratch = test_arena(4 * KiB);

    Bytes data = bytes_from_cstr("li1ee");
    BencodeValue value = {0};
    assert(ErrKindNone != bencode_parse(&data, &arena, scratch, &value).kind);

    // An empty container needs no allocation at all, so it still succeeds.
    Bytes data_empty = bytes_from_cstr("le");
    assert(ErrKindNone ==
           bencode_parse(&data_empty, &arena, scratch, &value).kind);
    assert(0 == value.v.list.len);
  }

  // `scratch` is taken by value: it is not consumed, and two parses in a row
  // do not tread on each other. `arena` *is* consumed, so the first result
  // stays valid while the second one is built.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);
    const u8 *const scratch_start = scratch.start;

    Bytes data_a = bytes_from_cstr("li1ei2ee");
    BencodeValue a = {0};
    assert(ErrKindNone == bencode_parse(&data_a, &arena, scratch, &a).kind);
    assert(scratch_start == scratch.start);

    Bytes data_b = bytes_from_cstr("li3ee");
    BencodeValue b = {0};
    assert(ErrKindNone == bencode_parse(&data_b, &arena, scratch, &b).kind);
    assert(scratch_start == scratch.start);

    assert(a.v.list.data != b.v.list.data);
    assert(2 == a.v.list.len);
    assert(1 == a.v.list.data[0].v.num);
    assert(2 == a.v.list.data[1].v.num);
    assert(1 == b.v.list.len);
    assert(3 == b.v.list.data[0].v.num);
  }

  // An input with no data at all.
  {
    Arena arena = test_arena(1 * KiB);
    Arena scratch = test_arena(1 * KiB);

    Bytes data = bytes_make(NULL, 0);
    BencodeValue value = {0};
    assert(ErrKindNone != bencode_parse(&data, &arena, scratch, &value).kind);
  }

  // A parse that fails *after* allocating gives the memory back: the inner
  // list is allocated, then the unterminated outer one fails.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);
    u8 *const arena_start = arena.start;

    const char *const input = "lli1ee";
    Bytes data = bytes_from_cstr(input);

    // Poisoned so that a write on the failure path is visible.
    BencodeValue value = {.kind = BencodeKindBytes};
    assert(ErrKindNone != bencode_parse(&data, &arena, scratch, &value).kind);

    assert(arena_start == arena.start);
    assert(strlen(input) == data.len);
    assert(BencodeKindBytes == value.kind);
  }
}

// `bencode_parse` again, for the three things the table above cannot express.
//
// Its corpus is built with `bytes_from_cstr`, which measures with `strlen`, so
// every case in it is NUL-free seven-bit ASCII. Bencode byte strings are
// arbitrary bytes, and it is precisely the bytes `strlen` and a signed `char`
// cannot carry that break a parser.
static void test_bencode_parse_binary(void) {
  // A byte string body is arbitrary bytes, NUL and 0x80..0xff included, and it
  // borrows from the input rather than being copied or terminated.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);

    // `l` `5:` <a NUL b 0x80 0xff> `e`
    const u8 input[] = {'l', '5', ':', 'a', 0x00, 'b', 0x80, 0xff, 'e'};
    Bytes data = bytes_make((u8 *)input, sizeof(input));

    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind);
    assert(0 == data.len);
    assert(BencodeKindList == value.kind);
    assert(1 == value.v.list.len);

    const BencodeValue bytes = value.v.list.data[0];
    assert(BencodeKindBytes == bytes.kind);
    assert(5 == bytes.v.bytes.len);
    // Borrowed, so the body is the input's own bytes and not a copy.
    assert(input + 3 == bytes.v.bytes.data);
    assert(0 == memcmp(bytes.v.bytes.data, input + 3, 5));
  }

  // A NUL inside a *key* is a byte like any other: it neither terminates the
  // key nor makes two different keys compare equal.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);

    // `d` `2:a\0` `1:x` `2:ab` `1:y` `e`, in order: "a\0" < "ab".
    const u8 input[] = {'d', '2', ':', 'a', 0x00, '1', ':', 'x',
                        '2', ':', 'a', 'b', '1',  ':', 'y', 'e'};
    Bytes data = bytes_make((u8 *)input, sizeof(input));

    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind);
    assert(BencodeKindDict == value.kind);
    assert(4 == value.v.list.len);
    assert(2 == value.v.list.data[0].v.bytes.len);
    assert(0 == memcmp(value.v.list.data[0].v.bytes.data, "a\0", 2));
  }

  // Dict keys are ordered by raw byte value, so 0x01 comes before 0x80. A
  // signed `char` comparison reads 0x80 as -128 and puts it first, which would
  // accept the reversed pair and reject this one; `bytes_cmp` is checked on
  // its own, this checks that `bencode_parse` actually routes dict keys through
  // it.
  {
    const struct {
      u8 first_key;
      u8 second_key;
      bool ok;
    } cases[] = {
        {0x01, 0x80, true},
        {0x80, 0x01, false},
        {0x00, 0xff, true},
        {0xff, 0x00, false},
        // Equal keys are duplicates whatever their value.
        {0x80, 0x80, false},
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      Arena arena = test_arena(4 * KiB);
      Arena scratch = test_arena(4 * KiB);

      const u8 input[] = {'d', '1', ':', cases[i].first_key,  '1', ':',
                          'x', '1', ':', cases[i].second_key, '1', ':',
                          'y', 'e'};
      Bytes data = bytes_make((u8 *)input, sizeof(input));

      BencodeValue value = {0};
      assert(
          (ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind) ==
          cases[i].ok);
    }
  }
}

// The remaining shapes of a dict key: empty ones, and one that is a prefix of
// the next. Both are legal bencode and both are easy to get wrong, a prefix
// because a length-blind compare calls "aa" smaller than "a".
static void test_bencode_parse_dict_keys(void) {
  const struct {
    const char *input;
    bool ok;
    usize children_len;
  } cases[] = {
      // An empty key is a key.
      {"d0:0:e", true, 2},
      {"d0:1:ae", true, 2},
      // And two of them are duplicates.
      {"d0:1:a0:1:be", false, 0},
      // The empty key sorts before every other one.
      {"d0:1:x1:a1:ye", true, 4},
      {"d1:a1:x0:1:ye", false, 0},
      // A prefix sorts before what extends it, whatever the lengths say.
      {"d1:a1:x2:aa1:ye", true, 4},
      {"d2:aa1:x1:a1:ye", false, 0},
      {"d2:aa1:x3:aaa1:ye", true, 4},
      {"d3:aaa1:x2:aa1:ye", false, 0},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);

    Bytes data = bytes_from_cstr(cases[i].input);

    BencodeValue value = {0};
    assert((ErrKindNone ==
            bencode_parse(&data, &arena, scratch, &value).kind) == cases[i].ok);

    if (!cases[i].ok) {
      continue;
    }

    assert(BencodeKindDict == value.kind);
    assert(cases[i].children_len == value.v.list.len);
  }
}

// The depth bound again, but reached through dicts and through the two kinds
// alternating. The existing case nests lists only, and a list is the one
// container that closes without its contents having to pair up.
static void test_bencode_parse_deep_dicts(void) {
  // `d1:a` * (depth - 1), then an empty `de` at the bottom, then the closing
  // `e`s: every dict holds one key and one dict, so the innermost open count
  // reaches exactly `depth`.
  for (usize depth = BENCODE_MAX_DEPTH; depth <= BENCODE_MAX_DEPTH + 1;
       depth++) {
    Arena arena = test_arena(64 * KiB);
    Arena scratch = test_arena(64 * KiB);

    u8 input[5 * (BENCODE_MAX_DEPTH + 1)] = {0};
    usize len = 0;
    for (usize i = 0; i + 1 < depth; i++) {
      memcpy(input + len, "d1:a", 4);
      len += 4;
    }
    memcpy(input + len, "de", 2);
    len += 2;
    for (usize i = 0; i + 1 < depth; i++) {
      input[len++] = 'e';
    }
    assert(len <= sizeof(input));

    Bytes data = bytes_make(input, len);
    BencodeValue value = {0};
    const bool ok =
        ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind;

    assert(ok == (depth <= BENCODE_MAX_DEPTH));
    if (ok) {
      assert(BencodeKindDict == value.kind);
      // The key and the dict below it.
      assert(2 == value.v.list.len);
    }
  }

  // Lists and dicts alternating, so a dict closes with a list as its value and
  // a list closes with a dict as its only item.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);

    Bytes data = bytes_from_cstr("ld1:ali1eeee");
    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind);
    assert(0 == data.len);

    assert(BencodeKindList == value.kind);
    assert(1 == value.v.list.len);

    const BencodeValue dict = value.v.list.data[0];
    assert(BencodeKindDict == dict.kind);
    assert(2 == dict.v.list.len);
    assert(test_bencode_is_bytes(dict.v.list.data[0], "a"));

    const BencodeValue list = dict.v.list.data[1];
    assert(BencodeKindList == list.kind);
    assert(1 == list.v.list.len);
    assert(BencodeKindInteger == list.v.list.data[0].kind);
    assert(1 == list.v.list.data[0].v.num);
  }
}

static void test_bytes_cmp(void) {
  // A corpus in the order `bytes_cmp` must put it in. The embedded zeroes
  // and the bytes above 0x7f are there on purpose: neither `strcmp` nor a
  // signed char comparison orders these correctly.
  const struct {
    const char *data;
    usize len;
  } sorted[] = {
      {"", 0},
      {"\x00", 1},
      {"\x00\x00", 2},
      {"\x00"
       "a",
       2},
      {"a", 1},
      {"a\x00", 2},
      {"ab", 2},
      {"abc", 3},
      {"b", 1},
      {"\x7f", 1},
      {"\x80", 1},
      {"\xfe", 1},
      {"\xff", 1},
      {"\xff\x00", 2},
      {"\xff\xff", 2},
  };
  const usize count = sizeof(sorted) / sizeof(sorted[0]);

  for (usize i = 0; i < count; i++) {
    for (usize j = 0; j < count; j++) {
      const Bytes a = bytes_make((u8 *)sorted[i].data, sorted[i].len);
      const Bytes b = bytes_make((u8 *)sorted[j].data, sorted[j].len);

      const i32 res = bytes_cmp(a, b);
      if (i < j) {
        assert(res < 0);
      } else if (i > j) {
        assert(res > 0);
      } else {
        assert(0 == res);
      }

      // Antisymmetric: swapping the arguments flips the sign.
      const i32 swapped = bytes_cmp(b, a);
      assert((res < 0) == (swapped > 0));
      assert((res > 0) == (swapped < 0));
      assert((0 == res) == (0 == swapped));
    }
  }

  // A NULL pointer is legal as long as the length is zero, and every empty
  // byte string is equal to every other one.
  {
    const Bytes null_empty = bytes_make(NULL, 0);
    assert(0 == bytes_cmp(null_empty, null_empty));
    assert(0 == bytes_cmp(null_empty, bytes_from_cstr("")));
    assert(bytes_cmp(null_empty, bytes_from_cstr("a")) < 0);
    assert(bytes_cmp(bytes_from_cstr("a"), null_empty) > 0);
  }

  // Longer than a word, differing only in the last byte.
  {
    u8 x[64];
    u8 y[64];
    memset(x, 'z', sizeof(x));
    memset(y, 'z', sizeof(y));
    y[sizeof(y) - 1] = 'z' + 1;

    const Bytes sx = bytes_make(x, sizeof(x));
    const Bytes sy = bytes_make(y, sizeof(y));

    assert(bytes_cmp(sx, sy) < 0);
    assert(bytes_cmp(sy, sx) > 0);

    // Against itself, and against a prefix of itself.
    assert(0 == bytes_cmp(sx, sx));
    assert(bytes_cmp(bytes_take(sx, sizeof(x) - 1), sx) < 0);
    assert(bytes_cmp(sx, bytes_take(sx, sizeof(x) - 1)) > 0);
  }
}

static void test_bytes_find(void) {
  const struct {
    const char *haystack;
    const char *needle;
    bool found;
    usize idx;
  } cases[] = {
      {"", "", true, 0},
      {"abc", "", true, 0},
      {"", "a", false, 0},
      {"a", "ab", false, 0},
      {"abc", "abc", true, 0},
      {"abc", "abd", false, 0},
      {"abc", "a", true, 0},
      {"abc", "b", true, 1},
      {"abc", "c", true, 2},
      {"abc", "bc", true, 1},
      {"abc", "d", false, 0},
      // First match wins.
      {"abab", "ab", true, 0},
      {"xabab", "ab", true, 1},
      // Overlapping partial match before the real one.
      {"aaab", "aab", true, 1},
      {"\r\n\r\n", "\r\n\r\n", true, 0},
      {"GET / HTTP/1.1\r\nHost: x\r\n\r\nbody", "\r\n\r\n", true, 23},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const Bytes haystack = bytes_from_cstr(cases[i].haystack);
    const Bytes needle = bytes_from_cstr(cases[i].needle);

    const Find start =
        bytes_find(haystack, needle, FindOptionsIndexAtNeedleStart);
    assert(cases[i].found == start.found);
    assert(cases[i].idx == start.idx);

    const Find end =
        bytes_find(haystack, needle, FindOptionsIndexAfterNeedleEnd);
    assert(cases[i].found == end.found);
    if (cases[i].found) {
      assert(cases[i].idx + needle.len == end.idx);
      assert(end.idx <= haystack.len);
    } else {
      assert(0 == end.idx);
    }
  }

  // Embedded zeroes: `strstr` would stop at the first one.
  {
    const u8 haystack_data[] = {'a', 0, 'b', 0, 'c'};
    const u8 needle_data[] = {0, 'c'};
    const Bytes haystack =
        bytes_make((u8 *)haystack_data, sizeof(haystack_data));
    const Bytes needle = bytes_make((u8 *)needle_data, sizeof(needle_data));

    const Find find =
        bytes_find(haystack, needle, FindOptionsIndexAtNeedleStart);
    assert(find.found);
    assert(3 == find.idx);
  }

  // A NULL pointer is legal as long as the length is zero.
  {
    const Bytes null_empty = bytes_make(NULL, 0);

    const Find in_null =
        bytes_find(null_empty, null_empty, FindOptionsIndexAtNeedleStart);
    assert(in_null.found);
    assert(0 == in_null.idx);

    const Find not_in_null = bytes_find(null_empty, bytes_from_cstr("a"),
                                        FindOptionsIndexAtNeedleStart);
    assert(!not_in_null.found);
  }

  // The needle sits at the very end of a longer haystack.
  {
    u8 haystack_data[64];
    memset(haystack_data, 'z', sizeof(haystack_data));
    haystack_data[sizeof(haystack_data) - 1] = 'y';
    const Bytes haystack = bytes_make(haystack_data, sizeof(haystack_data));

    const Find find = bytes_find(haystack, bytes_from_cstr("zy"),
                                 FindOptionsIndexAfterNeedleEnd);
    assert(find.found);
    assert(sizeof(haystack_data) == find.idx);
  }
}

// Expected pieces come from Go's `strings.Split`.
static void test_bytes_split(void) {
  const struct {
    const char *haystack;
    const char *needle;
    usize pieces_len;
    const char *pieces[4];
  } cases[] = {
      {"", ",", 1, {""}},
      {"abcd", "z", 1, {"abcd"}},
      {"abc", "abcd", 1, {"abc"}},
      {"abcd", "a", 2, {"", "bcd"}},
      {"abcd", "d", 2, {"abc", ""}},
      {"abcd", "bc", 2, {"a", "d"}},
      {"abcd", "abcd", 2, {"", ""}},
      {",", ",", 2, {"", ""}},
      {"1,2,3,4", ",", 4, {"1", "2", "3", "4"}},
      {"a,,b", ",", 3, {"a", "", "b"}},
      {",,", ",", 3, {"", "", ""}},
      // Matches do not overlap.
      {"aaaa", "aa", 3, {"", "", ""}},
      {"aaa", "aa", 2, {"", "a"}},
      {"GET / HTTP/1.1\r\nHost: x\r\n\r\nbody",
       "\r\n\r\n",
       2,
       {"GET / HTTP/1.1\r\nHost: x", "body"}},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const Bytes haystack = bytes_from_cstr(cases[i].haystack);
    const Bytes needle = bytes_from_cstr(cases[i].needle);

    Bytes rest = haystack;
    usize pieces_len = 0;
    // Each split eats at least one byte, so this bounds the pieces.
    for (usize j = 0; j <= haystack.len; j++) {
      const Split split = bytes_split(rest, needle);

      assert(pieces_len < cases[i].pieces_len);
      assert(bytes_eq_cstr(split.left, cases[i].pieces[pieces_len]));
      pieces_len++;

      if (!split.found) {
        assert(bytes_eq(split.left, rest));
        assert(0 == split.right.len);
        break;
      }

      assert(split.left.data == rest.data);
      assert(split.left.len + needle.len + split.right.len == rest.len);
      rest = split.right;
    }
    assert(cases[i].pieces_len == pieces_len);
  }

  // Embedded zeroes: `strstr` would stop at the first one.
  {
    const u8 haystack_data[] = {'a', 0, 'b', 0, 'c'};
    const u8 needle_data[] = {0};
    const Bytes haystack =
        bytes_make((u8 *)haystack_data, sizeof(haystack_data));
    const Bytes needle = bytes_make((u8 *)needle_data, sizeof(needle_data));

    const Split first = bytes_split(haystack, needle);
    assert(first.found);
    assert(bytes_eq_cstr(first.left, "a"));
    assert(first.right.data == haystack.data + 2);
    assert(3 == first.right.len);

    const Split second = bytes_split(first.right, needle);
    assert(second.found);
    assert(bytes_eq_cstr(second.left, "b"));
    assert(bytes_eq_cstr(second.right, "c"));
  }

  // A NULL pointer is legal as long as the length is zero.
  {
    const Split split = bytes_split(bytes_make(NULL, 0), bytes_from_cstr(","));
    assert(!split.found);
    assert(bytes_is_empty(split.left));
    assert(bytes_is_empty(split.right));
  }
}

static void test_http_find_headers_end(void) {
  const struct {
    const char *src;
    bool found;
    usize idx;
  } cases[] = {
      {"", false, 0},
      {"\r\n", false, 0},
      {"\r\n\r", false, 0},
      {"\r\n\r\n", true, 4},
      {"GET / HTTP/1.1\r\nHost: x\r\n\r\nbody", true, 27},
      // Only the first blank line counts: the rest is body.
      {"A: b\r\n\r\n\r\n\r\n", true, 8},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const Find find = http_find_headers_end(bytes_from_cstr(cases[i].src));
    assert(cases[i].found == find.found);
    assert(cases[i].idx == find.idx);
  }
}

// Cases follow RFC 9110 section 5 and RFC 9112 section 5.
static void test_http_parse_headers(void) {
  const struct {
    const char *src;
    ErrorKind err;
    usize headers_len;
    struct {
      const char *key;
      const char *value;
    } headers[3];
  } cases[] = {
      // No headers at all.
      {"\r\n", ErrKindNone, 0, {{0}}},
      // The body is not read.
      {"\r\nbody", ErrKindNone, 0, {{0}}},
      {"A: b\r\n\r\nC: d\r\n\r\n", ErrKindNone, 1, {{"A", "b"}}},
      {"Host: x\r\n\r\n", ErrKindNone, 1, {{"Host", "x"}}},
      // OWS around the value is optional and not part of it.
      {"Host:x\r\n\r\n", ErrKindNone, 1, {{"Host", "x"}}},
      {"Host: \t x \t\r\n\r\n", ErrKindNone, 1, {{"Host", "x"}}},
      // An empty value is valid.
      {"A:\r\n\r\n", ErrKindNone, 1, {{"A", ""}}},
      {"A: \t \r\n\r\n", ErrKindNone, 1, {{"A", ""}}},
      // Whitespace inside the value is kept.
      {"A: b \t c\r\n\r\n", ErrKindNone, 1, {{"A", "b \t c"}}},
      // Only the first colon separates.
      {"A: b:c\r\n\r\n", ErrKindNone, 1, {{"A", "b:c"}}},
      // Every tchar.
      {"!#$%&'*+-.^_`|~09azAZ: v\r\n\r\n",
       ErrKindNone,
       1,
       {{"!#$%&'*+-.^_`|~09azAZ", "v"}}},
      // obs-text.
      {"A: \x80\xff\r\n\r\n", ErrKindNone, 1, {{"A", "\x80\xff"}}},
      {"A: b\r\nA: c\r\nD: e\r\n\r\n",
       ErrKindNone,
       3,
       {{"A", "b"}, {"A", "c"}, {"D", "e"}}},

      // Cut short.
      {"", ErrKindInvalidData, 0, {{0}}},
      {"Host: x", ErrKindInvalidData, 0, {{0}}},
      {"Host: x\r\n", ErrKindInvalidData, 1, {{"Host", "x"}}},
      {"Host: x\r\n\r", ErrKindInvalidData, 1, {{"Host", "x"}}},
      // Only CRLF ends a line.
      {"A: b\n\n", ErrKindInvalidData, 0, {{0}}},
      {"A: b\nC: d\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      {"A: b\rC: d\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      // No colon, or no name.
      {"Host x\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      {": x\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      // Whitespace before the colon.
      {"Host : x\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      {"Host\t: x\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      // obs-fold, and its look-alike on the first line.
      {"A: b\r\n c\r\n\r\n", ErrKindInvalidData, 1, {{"A", "b"}}},
      {"A: b\r\n\tc\r\n\r\n", ErrKindInvalidData, 1, {{"A", "b"}}},
      {" A: b\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      // Not a token.
      {"A(: b\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      {"A\": b\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      {"\x80: b\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      // Control characters in the value.
      {"A: b\x7f\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
      {"A: b\x01\r\n\r\n", ErrKindInvalidData, 0, {{0}}},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const Bytes src = bytes_from_cstr(cases[i].src);
    HttpHeader headers[3] = {0};
    usize headers_len = 0;

    const Error err = http_parse_headers(src, headers, &headers_len,
                                         sizeof(headers) / sizeof(headers[0]));
    assert(cases[i].err == err.kind);
    assert(cases[i].headers_len == headers_len);

    for (usize j = 0; j < headers_len; j++) {
      assert(bytes_eq_cstr(headers[j].key, cases[i].headers[j].key));
      assert(bytes_eq_cstr(headers[j].value, cases[i].headers[j].value));

      // Borrowed from `src`, not copied.
      assert(src.data <= headers[j].key.data);
      assert(headers[j].key.data + headers[j].key.len <= src.data + src.len);
      assert(src.data <= headers[j].value.data);
      assert(headers[j].value.data + headers[j].value.len <=
             src.data + src.len);
    }
  }

  // A NUL in the value, which a C string cannot hold.
  {
    const u8 data[] = {'A', ':', ' ', 'b', 0, 'c', '\r', '\n', '\r', '\n'};
    HttpHeader headers[1] = {0};
    usize headers_len = 0;

    assert(ErrKindInvalidData ==
           http_parse_headers(bytes_make((u8 *)data, sizeof(data)), headers,
                              &headers_len, 1)
               .kind);
    assert(0 == headers_len);
  }

  // A NULL pointer is legal as long as the length is zero.
  {
    HttpHeader headers[1] = {0};
    usize headers_len = 0;

    assert(
        ErrKindInvalidData ==
        http_parse_headers(bytes_make(NULL, 0), headers, &headers_len, 1).kind);
    assert(0 == headers_len);
  }

  // Out of room, and exactly enough room.
  {
    const Bytes src = bytes_from_cstr("A: b\r\nC: d\r\n\r\n");

    HttpHeader one[1] = {0};
    usize one_len = 0;
    assert(ErrKindOOM == http_parse_headers(src, one, &one_len, 1).kind);
    assert(1 == one_len);
    assert(bytes_eq_cstr(one[0].key, "A"));

    HttpHeader two[2] = {0};
    usize two_len = 0;
    assert(ErrKindNone == http_parse_headers(src, two, &two_len, 2).kind);
    assert(2 == two_len);
    assert(bytes_eq_cstr(two[1].key, "C"));
    assert(bytes_eq_cstr(two[1].value, "d"));
  }

  // `http_is_tchar` against the list in RFC 9110 section 5.6.2, byte by byte.
  {
    const char tchars[] = "!#$%&'*+-.^_`|~0123456789"
                          "abcdefghijklmnopqrstuvwxyz"
                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (usize c = 0; c <= 0xff; c++) {
      const bool expected =
          0 != c && NULL != memchr(tchars, (int)c, sizeof(tchars) - 1);
      assert(expected == http_is_tchar((u8)c));
    }
  }
}

static void test_bencode_validate_dict(void) {
  const BencodeValue num = {.kind = BencodeKindInteger, .v.num = 42};

  // Empty: trivially valid, and a NULL `data` is allowed when `len` is zero.
  {
    const BencodeList list = {0};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  // Keys strictly increasing.
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("a", 1), num,
        test_bencode_make_bytes("b", 1), num,
        test_bencode_make_bytes("c", 1), num,
    };
    const BencodeList list = {.len = 6, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  // Out of order, anywhere in the dict.
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("b", 1),
        num,
        test_bencode_make_bytes("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("a", 1), num,
        test_bencode_make_bytes("c", 1), num,
        test_bencode_make_bytes("b", 1), num,
    };
    const BencodeList list = {.len = 6, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Duplicate keys: sorted is not enough, the order has to be strict.
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("a", 1),
        num,
        test_bencode_make_bytes("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Keys must be byte strings.
  {
    BencodeValue children[] = {num, num};
    const BencodeList list = {.len = 2, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // ... including a non-byte-string key that is not the first one.
  {
    BencodeValue children[] = {test_bencode_make_bytes("a", 1), num, num, num};
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Values are not constrained, only keys are.
  {
    const BencodeValue nested_list = {.kind = BencodeKindList};
    const BencodeValue nested_dict = {.kind = BencodeKindDict};
    BencodeValue children[] = {
        test_bencode_make_bytes("a", 1),
        nested_list,
        test_bencode_make_bytes("b", 1),
        nested_dict,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  // An odd number of children is not key/value pairs.
  {
    BencodeValue children[] = {test_bencode_make_bytes("a", 1), num,
                               test_bencode_make_bytes("b", 1)};
    const BencodeList list = {.len = 3, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  {
    BencodeValue children[] = {test_bencode_make_bytes("a", 1)};
    const BencodeList list = {.len = 1, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // The empty key is legal and sorts before every other key.
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("", 0),
        num,
        test_bencode_make_bytes("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  // A key that is a prefix of the next one is in order; the reverse is not.
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("a", 1),
        num,
        test_bencode_make_bytes("ab", 2),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("ab", 2),
        num,
        test_bencode_make_bytes("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Ordering is by raw byte value, so `0x80` sorts *after* `0x7f`. A signed
  // comparison would get this pair backwards.
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("\x7f", 1),
        num,
        test_bencode_make_bytes("\x80", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("\x80", 1),
        num,
        test_bencode_make_bytes("\x7f", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Keys are compared over their whole length, zero bytes included.
  {
    BencodeValue children[] = {
        test_bencode_make_bytes("a\x00"
                                "a",
                                3),
        num,
        test_bencode_make_bytes("a\x00"
                                "b",
                                3),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
}

// Hash `data` in one `Update` call, the simplest possible use of the API.
static void test_sha256_once(Bytes data, u8 res[SHA256_DIGEST_LENGTH]) {
  Sha256Ctx ctx = {0};
  sha256_init(&ctx);
  sha256_update(&ctx, data.data, data.len);
  sha256_final(&ctx, res);
}

// Expected digests are given as hex, the way every SHA-256 test vector in the
// wild is published, so that a vector can be pasted in unmodified.
static void test_digest_from_hex(const char *hex,
                                 u8 res[SHA256_DIGEST_LENGTH]) {
  assert(hex);
  assert(2 * SHA256_DIGEST_LENGTH == strlen(hex));

  for (usize i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    u32 byte = 0;
    assert(1 == sscanf(hex + 2 * i, "%2x", &byte));
    res[i] = (u8)byte;
  }
}

static void test_sha256_expect_hex(Bytes data, const char *expected_hex) {
  u8 expected[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(expected_hex, expected);

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  test_sha256_once(data, actual);

  assert(0 == memcmp(actual, expected, sizeof(actual)));
}

static void test_sha256_vectors(void) {
  // FIPS 180-2 / NIST CAVP vectors.
  test_sha256_expect_hex(
      bytes_from_cstr(""),
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  test_sha256_expect_hex(
      bytes_from_cstr("abc"),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56 bytes: the shortest message whose padding needs a second block.
  test_sha256_expect_hex(
      bytes_from_cstr(
          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  test_sha256_expect_hex(
      bytes_from_cstr(
          "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijk"
          "lmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
      "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

  // A NUL byte is data like any other: the API takes a length, never a C
  // string.
  test_sha256_expect_hex(
      bytes_make((u8 *)"\x00", 1),
      "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d");
}

// The million 'a' vector, fed in odd-sized chunks so that the partial block
// handling is exercised on a message far longer than one block.
static void test_sha256_million_a(void) {
  Sha256Ctx ctx = {0};
  sha256_init(&ctx);

  u8 chunk[1000] = {0};
  memset(chunk, 'a', sizeof(chunk));
  for (usize i = 0; i < 1000; i++) {
    sha256_update(&ctx, chunk, sizeof(chunk));
  }

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&ctx, actual);

  const u8 expected[SHA256_DIGEST_LENGTH] = {
      0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7,
      0xe2, 0x84, 0xd7, 0x3e, 0x67, 0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97,
      0x20, 0x0e, 0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0,
  };
  assert(0 == memcmp(actual, expected, sizeof(actual)));
}

// Any split of the same message must give the same digest: one `Update`, one
// `Update` per byte, and a split at every offset around the block boundary.
static void test_sha256_incremental(void) {
  u8 input[200] = {0};
  for (usize i = 0; i < sizeof(input); i++) {
    input[i] = (u8)(i * 31 + 7);
  }

  u8 expected[SHA256_DIGEST_LENGTH] = {0};
  test_sha256_once(bytes_make(input, sizeof(input)), expected);

  {
    Sha256Ctx ctx = {0};
    sha256_init(&ctx);
    for (usize i = 0; i < sizeof(input); i++) {
      sha256_update(&ctx, input + i, 1);
    }

    u8 actual[SHA256_DIGEST_LENGTH] = {0};
    sha256_final(&ctx, actual);
    assert(0 == memcmp(actual, expected, sizeof(actual)));
  }

  for (usize split = 0; split <= sizeof(input); split++) {
    Sha256Ctx ctx = {0};
    sha256_init(&ctx);
    sha256_update(&ctx, input, split);
    // An empty `Update` in the middle must be a no-op, including when the
    // input has no data pointer at all.
    sha256_update(&ctx, NULL, 0);
    sha256_update(&ctx, input + split, sizeof(input) - split);

    u8 actual[SHA256_DIGEST_LENGTH] = {0};
    sha256_final(&ctx, actual);
    assert(0 == memcmp(actual, expected, sizeof(actual)));
  }
}

// Every message length from 0 to 1024 bytes, which covers every padding case
// and every partial block length. One digest stands for all of them: they are
// themselves hashed, in order, and the result compared to a constant obtained
// from a reference implementation.
static void test_sha256_lengths(void) {
  u8 input[1025] = {0};
  for (usize i = 0; i < sizeof(input); i++) {
    input[i] = (u8)(i * 31 + 7);
  }

  Sha256Ctx outer = {0};
  sha256_init(&outer);

  for (usize len = 0; len < sizeof(input); len++) {
    u8 digest[SHA256_DIGEST_LENGTH] = {0};
    test_sha256_once(bytes_make(input, len), digest);
    sha256_update(&outer, digest, sizeof(digest));
  }

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&outer, actual);

  const u8 expected[SHA256_DIGEST_LENGTH] = {
      0x70, 0x2f, 0xea, 0x77, 0xff, 0x7e, 0x99, 0xf9, 0xf5, 0x44, 0x35,
      0x64, 0x87, 0x2a, 0x6d, 0xe2, 0x52, 0x04, 0xa0, 0x69, 0xe1, 0x42,
      0x4f, 0xea, 0xc3, 0xff, 0x3e, 0x25, 0x19, 0x52, 0x2f, 0x15,
  };
  assert(0 == memcmp(actual, expected, sizeof(actual)));
}

// `Init` must fully reset a context, so that reusing one is the same as
// starting from a fresh `{0}` one.
static void test_sha256_reuse(void) {
  u8 expected[SHA256_DIGEST_LENGTH] = {0};
  test_sha256_once(bytes_from_cstr("abc"), expected);

  Sha256Ctx ctx = {0};
  sha256_init(&ctx);
  const Bytes other = bytes_from_cstr("some other message entirely");
  sha256_update(&ctx, other.data, other.len);

  u8 discarded[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&ctx, discarded);

  sha256_init(&ctx);
  const Bytes abc = bytes_from_cstr("abc");
  sha256_update(&ctx, abc.data, abc.len);

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&ctx, actual);
  assert(0 == memcmp(actual, expected, sizeof(actual)));
}

// ---------------------------------------------------------------------------
// BEP 52 merkle tree
// ---------------------------------------------------------------------------

// The largest file any merkle test builds a tree for: 13 blocks, which pads to
// 16 leaves and so exercises a subtree that is entirely padding.
#define TEST_MERKLE_MAX_LEN (208 * KiB)

// A constant fill would not notice two leaves being swapped, so give every
// block distinct content. The expected roots below come from libtorrent 2.1.1
// fed the exact same bytes, checked at both a 16KiB and a 256KiB piece size
// since `pieces root` must not depend on the piece size.
__attribute__((warn_unused_result)) static Bytes
test_merkle_data(Arena *arena) {
  u8 *const buf = arena_alloc(arena, 1, 1, TEST_MERKLE_MAX_LEN);
  assert(buf);

  u32 x = 0x12345678;
  for (usize i = 0; i < TEST_MERKLE_MAX_LEN; i++) {
    // Numerical Recipes LCG. Only the top byte is used, the low bits of an
    // LCG being far too regular to tell two blocks apart.
    x = x * 1664525u + 1013904223u;
    buf[i] = (u8)(x >> 24);
  }

  return bytes_make(buf, TEST_MERKLE_MAX_LEN);
}

// Known answer tests. Sizes bracket every boundary the tree construction has:
// shorter than a block, exactly a block, one byte past a block, an exact
// power of two number of blocks, and block counts needing one or several
// padding leaves.
static void test_torrent_merkle_vectors(void) {
  const Env *const env = env_platform_make();
  Arena data_arena = {0};
  assert(ErrKindNone ==
         arena_valloc(env, TEST_MERKLE_MAX_LEN + 4 * KiB, &data_arena).kind);
  assert(data_arena.start);
  const Bytes data = test_merkle_data(&data_arena);

  const struct {
    usize len;
    const char *root;
  } vectors[] = {
      {1, "0bfe935e70c321c7ca3afc75ce0d0ca2f98b5422e008bb31c00c6d7f1f1c0ad6"},
      {16383,
       "3dd5a06f7768acc49864c34e60083ab8af5d90cb4fad8388c0391eedbb4ef37b"},
      {16384,
       "6c2151ba392602898475e8faea682c1923ca241d0cee54d984b520286e005f0d"},
      {16385,
       "26d66f8578c6efa252c8b8deb3810b923392392618bcfea7ce34e1ca0ccf9211"},
      {32768,
       "e1bdca49b140c3391bb85e630b7cf05a73534b40d4fe2db5e60bfacd45be473a"},
      {40960,
       "809301c68152c8413d38cb646268c1f5caa2dc5bf359d7148eaa9ea3aa0399a7"},
      {81920,
       "4e1b3e51bf462b7d9863084f4c139a2106c729c0e0293b8abf692905ea57eee6"},
      {131072,
       "d4e69a223d5f61c604c613be46f12a017e134baec50d7cc960a25ba3dfcf5fc3"},
      {212992,
       "4c3bf66a99395bf30f382494b6e96fd9976df5ef489ae1a2173af1c1dc841f46"},
  };

  // `pieces root` is defined over 16KiB blocks, so it must come out the same
  // whatever the operator picked for the piece size. The expected values were
  // taken from libtorrent 2.1.1 at both ends of this range.
  const usize piece_lengths_in_bytes[] = {16 * KiB, 32 * KiB, 256 * KiB};

  for (usize i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
    assert(vectors[i].len <= data.len);

    u8 expected[SHA256_DIGEST_LENGTH] = {0};
    test_digest_from_hex(vectors[i].root, expected);

    for (usize p = 0;
         p < sizeof(piece_lengths_in_bytes) / sizeof(piece_lengths_in_bytes[0]);
         p++) {
      // Poisoned, so that a hash the implementation never writes cannot pass
      // itself off as a zeroed padding hash.
      Arena arena = test_arena(64 * KiB);

      PieceHash *pieces = NULL;
      usize pieces_count = 0;
      u8 root[SHA256_DIGEST_LENGTH] = {0};
      assert(ErrKindNone ==
             torrent_build_merkle_tree(bytes_make(data.data, vectors[i].len),
                                       piece_lengths_in_bytes[p], &pieces,
                                       &pieces_count, root, &arena)
                 .kind);

      assert(0 == memcmp(root, expected, sizeof(expected)));
    }
  }
}

// The piece layer, which is what actually ships in the `.torrent` next to the
// info dictionary. Checked against an independently built tree rather than
// against the implementation's own intermediate state.
static void test_torrent_merkle_piece_layer(void) {
  const Env *const env = env_platform_make();
  Arena data_arena = {0};
  assert(ErrKindNone ==
         arena_valloc(env, TEST_MERKLE_MAX_LEN + 4 * KiB, &data_arena).kind);
  assert(data_arena.start);
  const Bytes data = test_merkle_data(&data_arena);

  const usize lens[] = {1, 16384, 16385, 40960, 81920, 131072, 212992};
  const usize piece_lengths_in_bytes[] = {16 * KiB, 32 * KiB, 64 * KiB,
                                          256 * KiB};

  for (usize i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
    const usize len = lens[i];
    assert(len <= data.len);

    for (usize p = 0;
         p < sizeof(piece_lengths_in_bytes) / sizeof(piece_lengths_in_bytes[0]);
         p++) {
      const usize piece_length_in_bytes = piece_lengths_in_bytes[p];

      Arena arena = test_arena(64 * KiB);

      PieceHash *pieces = NULL;
      usize pieces_count = 0;
      u8 root[SHA256_DIGEST_LENGTH] = {0};
      assert(ErrKindNone ==
             torrent_build_merkle_tree(bytes_make(data.data, len),
                                       piece_length_in_bytes, &pieces,
                                       &pieces_count, root, &arena)
                 .kind);

      const usize expected_pieces = ceil_usize(len, piece_length_in_bytes);
      if (expected_pieces > 1) {
        assert(pieces);
        assert(expected_pieces == pieces_count);
      } else { // A file inside one piece has no piece layer, per BEP 52.
        assert(NULL == pieces);
        assert(0 == pieces_count);
      }

      // Build the whole tree the slow, obvious way and read the answers off
      // it. `TEST_MERKLE_MAX_LEN` is 13 blocks, so 16 leaves covers every
      // case.
      const usize blocks = ceil_usize(len, TORRENT_BLOCK_SIZE);
      const usize leaves = next_power_of_two(blocks);
      assert(leaves <= 16);

      u8 layer[16][SHA256_DIGEST_LENGTH] = {{0}};
      for (usize l = 0; l < leaves; l++) {
        if (l < blocks) {
          const usize offset = l * TORRENT_BLOCK_SIZE;
          const usize block_len = len - offset < TORRENT_BLOCK_SIZE
                                      ? len - offset
                                      : TORRENT_BLOCK_SIZE;
          sha256_digest(bytes_make(data.data + offset, block_len), layer[l]);
        } // Padding leaves stay zero, per BEP 52.
      }

      // Width of the level where one node spans exactly one piece. Zero when
      // a piece is wider than the whole padded tree, which never matches
      // below and so checks nothing, which is right: there is no piece layer.
      const usize blocks_per_piece = piece_length_in_bytes / TORRENT_BLOCK_SIZE;
      const usize piece_width = leaves / blocks_per_piece;

      for (usize width = leaves;; width /= 2) {
        if (0 != pieces_count && width == piece_width) {
          assert(pieces_count <= width);
          for (usize piece = 0; piece < pieces_count; piece++) {
            assert(0 == memcmp(pieces[piece].digest, layer[piece],
                               SHA256_DIGEST_LENGTH));
          }
        }

        if (1 == width) {
          break;
        }

        for (usize w = 0; w < width / 2; w++) {
          // `w <= 2 * w`, and the pair is read before the write lands, so
          // folding in place cannot clobber an input it still needs.
          sha256_digest_pair(layer[2 * w], layer[2 * w + 1], layer[w]);
        }
      }

      assert(0 == memcmp(root, layer[0], SHA256_DIGEST_LENGTH));
    }
  }
}

// The padding rule, which is the part of BEP 52 that is easiest to get wrong:
// leaves past the end of the file are 32 zero bytes, and only the leaf layer
// is zeroed. Everything above it is hashed normally, so a node covering
// nothing but padding is emphatically not zero.
static void test_torrent_merkle_padding(void) {
  const Env *const env = env_platform_make();
  Arena data_arena = {0};
  assert(ErrKindNone == arena_valloc(env, 64 * KiB, &data_arena).kind);
  assert(data_arena.start);

  // Three blocks, so the tree pads to four leaves and the last leaf covers no
  // file data at all. The tail block is one byte long.
  const usize len = 2 * TORRENT_BLOCK_SIZE + 1;
  u8 *const buf = arena_alloc(&data_arena, 1, 1, len);
  assert(buf);
  memset(buf, 'a', len);

  Arena arena = test_arena(64 * KiB);

  PieceHash *pieces = NULL;
  usize pieces_count = 0;
  u8 root[SHA256_DIGEST_LENGTH] = {0};
  // One block per piece, so the piece layer is the leaf layer and every leaf
  // that holds file data is observable.
  assert(ErrKindNone == torrent_build_merkle_tree(bytes_make(buf, len),
                                                  16 * KiB, &pieces,
                                                  &pieces_count, root, &arena)
                            .kind);
  assert(pieces);
  assert(3 == pieces_count);

  // The three real leaves are the hashes of the three blocks, the last at its
  // true length rather than zero extended to a full block.
  for (usize l = 0; l < 3; l++) {
    const usize offset = l * TORRENT_BLOCK_SIZE;
    const usize block_len =
        len - offset < TORRENT_BLOCK_SIZE ? len - offset : TORRENT_BLOCK_SIZE;
    u8 expected[SHA256_DIGEST_LENGTH] = {0};
    sha256_digest(bytes_make(buf + offset, block_len), expected);
    assert(0 == memcmp(pieces[l].digest, expected, sizeof(expected)));
  }

  // The fourth leaf is padding and so is absent from the piece layer, which
  // is the truncation BEP 52 asks for.
  assert(3 == pieces_count);

  // Now pin the padding rule through the root, the only place it is visible:
  // the padding leaf is 32 zero bytes, not the hash of a zero filled block.
  u8 leaf[4][SHA256_DIGEST_LENGTH] = {{0}};
  for (usize l = 0; l < 3; l++) {
    memcpy(leaf[l], pieces[l].digest, SHA256_DIGEST_LENGTH);
  } // `leaf[3]` stays zero.

  u8 left[SHA256_DIGEST_LENGTH] = {0};
  u8 right[SHA256_DIGEST_LENGTH] = {0};
  u8 expected_root[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest_pair(leaf[0], leaf[1], left);
  sha256_digest_pair(leaf[2], leaf[3], right);
  sha256_digest_pair(left, right, expected_root);
  assert(0 == memcmp(root, expected_root, sizeof(expected_root)));

  // The other plausible reading of "set to zero" gives a different root, so
  // the test above is actually discriminating.
  u8 *const zero_block = arena_alloc(&data_arena, 1, 1, TORRENT_BLOCK_SIZE);
  assert(zero_block);
  memset(zero_block, 0, TORRENT_BLOCK_SIZE);
  sha256_digest(bytes_make(zero_block, TORRENT_BLOCK_SIZE), leaf[3]);

  u8 wrong_root[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest_pair(leaf[2], leaf[3], right);
  sha256_digest_pair(left, right, wrong_root);
  assert(0 != memcmp(root, wrong_root, sizeof(wrong_root)));

  // And a node above the leaf layer is hashed normally even when everything
  // under it is padding, so it is emphatically not zero.
  const u8 zero[SHA256_DIGEST_LENGTH] = {0};
  u8 all_padding[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest_pair((u8 *)zero, (u8 *)zero, all_padding);
  assert(0 != memcmp(all_padding, zero, sizeof(zero)));
}

// BEP 52: an empty file has no pieces root at all.
static void test_torrent_merkle_empty(void) {
  Arena arena = test_arena(64 * KiB);

  // Preset to garbage: every out parameter must be cleared, since a caller
  // has no other way to tell that no tree was built.
  PieceHash *pieces = (PieceHash *)(usize)0xdeadbeef;
  usize pieces_count = 123;
  u8 root[SHA256_DIGEST_LENGTH];
  memset(root, 0xcd, sizeof(root));

  assert(ErrKindNone == torrent_build_merkle_tree((Bytes){0}, 256 * KiB,
                                                  &pieces, &pieces_count, root,
                                                  &arena)
                            .kind);
  assert(NULL == pieces);
  assert(0 == pieces_count);

  const u8 zero[SHA256_DIGEST_LENGTH] = {0};
  assert(0 == memcmp(root, zero, sizeof(zero)));
}

// The one failure path: an arena too small for the tree is reported, not
// asserted, and leaves nothing half built behind.
static void test_torrent_merkle_oom(void) {
  const Env *const env = env_platform_make();
  Arena data_arena = {0};
  assert(ErrKindNone == arena_valloc(env, 64 * KiB, &data_arena).kind);
  assert(data_arena.start);

  // Two blocks, so at one block per piece the layer needs two hashes.
  const usize len = TORRENT_BLOCK_SIZE + 1;
  u8 *const buf = arena_alloc(&data_arena, 1, 1, len);
  assert(buf);
  memset(buf, 'a', len);

  Arena arena = test_arena(4 * KiB);

  // Leave room for fewer hashes than the piece layer needs.
  const usize free_bytes = (usize)(arena.end - arena.start);
  assert(free_bytes > sizeof(PieceHash));
  u8 *const hog = arena_alloc(&arena, 1, 1, free_bytes - sizeof(PieceHash));
  assert(hog);

  PieceHash *pieces = NULL;
  usize pieces_count = 0;
  u8 root[SHA256_DIGEST_LENGTH] = {0};
  assert(ErrKindNone != torrent_build_merkle_tree(bytes_make(buf, len),
                                                  16 * KiB, &pieces,
                                                  &pieces_count, root, &arena)
                            .kind);
  assert(NULL == pieces);
  assert(0 == pieces_count);
}

// ---------------------------------------------------------------------------
// encode_usize_base_10
// ---------------------------------------------------------------------------

// The digits are written at the front of `dst`, so every case checks three
// things: the text, that the returned `Bytes` really does start at `dst.data`,
// and that nothing outside that range was touched.
static void test_usize_digits_base_10(void) {
  assert(1 == usize_digits_base_10(0));
  assert(1 == usize_digits_base_10(9));
  assert(2 == usize_digits_base_10(10));
  assert(2 == usize_digits_base_10(99));
  assert(3 == usize_digits_base_10(100));

  // Every power of ten and the value one below it: the two values that
  // straddle each digit count boundary.
  usize power = 1;
  for (usize digits = 1; digits <= 19; digits++) {
    assert(digits == usize_digits_base_10(power));
    assert(digits == usize_digits_base_10(power * 10 - 1));
    power *= 10;
  }

  // 10^19 and `SIZE_MAX` are both 20 digits, the widest a `usize` gets.
  assert(20 == usize_digits_base_10(power));
  assert(20 == usize_digits_base_10(SIZE_MAX));
}

static void test_encode_usize_once(usize n, const char *expected) {
  assert(expected);

  const usize dst_len = 24;
  u8 buf[64];
  assert(dst_len < sizeof(buf));
  memset(buf, '#', sizeof(buf));

  const usize written = encode_usize_base_10(n, bytes_make(buf, dst_len));

  // Comparing from the front of the buffer is what pins the anchoring now
  // that there is no returned pointer: the digits begin at `dst.data`, which
  // is what lets a caller encode straight into its own output buffer.
  assert(strlen(expected) == written);
  assert(0 == memcmp(buf, expected, written));

  // Everything after the digits, inside `dst` and past it, is untouched.
  for (usize i = written; i < sizeof(buf); i++) {
    assert('#' == buf[i]);
  }
}

static void test_encode_usize_base_10(void) {
  // Zero has no digits to peel off, which is the case a `while (n > 0)` loop
  // silently encodes as nothing at all.
  test_encode_usize_once(0, "0");

  for (usize d = 0; d <= 9; d++) {
    const char one_digit[2] = {(char)('0' + d), 0};
    test_encode_usize_once(d, one_digit);
  }

  // Either side of every digit count boundary.
  test_encode_usize_once(9, "9");
  test_encode_usize_once(10, "10");
  test_encode_usize_once(99, "99");
  test_encode_usize_once(100, "100");
  test_encode_usize_once(999, "999");
  test_encode_usize_once(1000, "1000");
  test_encode_usize_once(123456789, "123456789");

  // Trailing zeroes are not leading zeroes: `100` must not come out as `1`.
  test_encode_usize_once(10000000000000000000ull, "10000000000000000000");

  // 20 digits, the widest a `usize` gets.
  test_encode_usize_once(SIZE_MAX - 1, "18446744073709551614");
  test_encode_usize_once(SIZE_MAX, "18446744073709551615");
}

// There is no fixed minimum `dst`: a buffer of exactly the needed width
// works, which is what lets a caller size its output exactly instead of
// padding for the widest possible number.
static void test_encode_usize_base_10_exact_fit(void) {
  u8 buf[24];

  memset(buf, '#', sizeof(buf));
  const usize widest = encode_usize_base_10(SIZE_MAX, bytes_make(buf, 20));
  assert(20 == widest);
  assert(0 == memcmp(buf, "18446744073709551615", 20));

  memset(buf, '#', sizeof(buf));
  const usize one = encode_usize_base_10(7, bytes_make(buf, 1));
  assert(1 == one);
  assert('7' == buf[0]);
  assert('#' == buf[1]);

  memset(buf, '#', sizeof(buf));
  const usize three = encode_usize_base_10(123, bytes_make(buf, 3));
  assert(3 == three);
  assert(0 == memcmp(buf, "123", 3));
  assert('#' == buf[3]);
}

// Cross check against the platform formatter, and read the result back with
// this project's own parser. The parser rejects leading zeroes, so it also
// pins down that the encoder never emits one.
static void test_encode_usize_base_10_round_trip(void) {
  const usize values[] = {
      0,
      1,
      2,
      7,
      9,
      10,
      11,
      42,
      99,
      100,
      101,
      255,
      256,
      999,
      1000,
      65535,
      65536,
      999999,
      1000000,
      4294967295,
      4294967296,
      SIZE_MAX / 2,
      SIZE_MAX - 1,
      SIZE_MAX,
  };

  for (usize i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    const usize n = values[i];

    u8 buf[24];
    memset(buf, '#', sizeof(buf));
    const usize got = encode_usize_base_10(n, bytes_make(buf, sizeof(buf)));

    char expected[32] = {0};
    const i32 written = snprintf(expected, sizeof(expected), "%zu", n);
    assert(written > 0);
    assert((usize)written == got);
    assert(0 == memcmp(buf, expected, got));

    // `ascii_num_parse` wants a non-digit terminator, the way `i123e` has
    // one.
    u8 terminated[32] = {0};
    memcpy(terminated, buf, got);
    terminated[got] = 'e';

    Bytes to_parse = bytes_make(terminated, got + 1);
    usize parsed = 0;
    assert(ErrKindNone == ascii_num_parse(&to_parse, &parsed).kind);
    assert(n == parsed);
  }
}

static void test_encode_isize_once(isize n, const char *expected) {
  assert(expected);

  const usize dst_len = 24;
  u8 buf[64];
  assert(dst_len < sizeof(buf));
  memset(buf, '#', sizeof(buf));

  const usize written = encode_isize_base_10(n, bytes_make(buf, dst_len));

  // Comparing from the front of the buffer is what pins the anchoring now
  // that there is no returned pointer: the digits begin at `dst.data`, which
  // is what lets a caller encode straight into its own output buffer.
  assert(strlen(expected) == written);
  assert(0 == memcmp(buf, expected, written));

  // Everything after the digits, inside `dst` and past it, is untouched.
  for (usize i = written; i < sizeof(buf); i++) {
    assert('#' == buf[i]);
  }
}

static void test_encode_isize_base_10(void) {
  // Zero is not negative: no `-0`.
  test_encode_isize_once(0, "0");

  test_encode_isize_once(1, "1");
  test_encode_isize_once(-1, "-1");
  test_encode_isize_once(9, "9");
  test_encode_isize_once(-9, "-9");

  // Either side of every digit count boundary, both signs.
  test_encode_isize_once(10, "10");
  test_encode_isize_once(-10, "-10");
  test_encode_isize_once(99, "99");
  test_encode_isize_once(-99, "-99");
  test_encode_isize_once(100, "100");
  test_encode_isize_once(-100, "-100");
  test_encode_isize_once(123456789, "123456789");
  test_encode_isize_once(-123456789, "-123456789");

  // The asymmetric boundary: `|ISIZE_MIN|` is one greater than `ISIZE_MAX`,
  // so negating it in the signed domain would overflow.
  test_encode_isize_once(INT64_MAX, "9223372036854775807");
  test_encode_isize_once(INT64_MIN + 1, "-9223372036854775807");
  test_encode_isize_once(INT64_MIN, "-9223372036854775808");
}

// Exact fit again, this time including the sign: the widest `isize` is the
// negative one, 19 digits plus a sign.
static void test_encode_isize_base_10_exact_fit(void) {
  u8 buf[24];

  memset(buf, '#', sizeof(buf));
  const usize widest = encode_isize_base_10(INT64_MIN, bytes_make(buf, 20));
  assert(20 == widest);
  assert(0 == memcmp(buf, "-9223372036854775808", 20));

  memset(buf, '#', sizeof(buf));
  const usize two = encode_isize_base_10(-7, bytes_make(buf, 2));
  assert(2 == two);
  assert(0 == memcmp(buf, "-7", 2));
  assert('#' == buf[2]);
}

// Cross check against the platform formatter, then frame the digits as a
// bencode integer and read them back with this project's own parser.
static void test_encode_isize_base_10_round_trip(void) {
  const isize values[] = {
      0,        1,          -1,          2,         -2,        9,
      -9,       10,         -10,         99,        -99,       100,
      -100,     255,        -256,        65535,     -65536,    1000000,
      -1000000, 4294967296, -4294967296, INT64_MAX, INT64_MIN, INT64_MIN + 1,
  };

  for (usize i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    const isize n = values[i];

    u8 buf[24];
    memset(buf, '#', sizeof(buf));
    const usize got = encode_isize_base_10(n, bytes_make(buf, sizeof(buf)));

    char expected[32] = {0};
    const i32 written = snprintf(expected, sizeof(expected), "%zd", n);
    assert(written > 0);
    assert((usize)written == got);
    assert(0 == memcmp(buf, expected, got));

    // `i<n>e`, the way a bencode integer is framed.
    u8 framed[32] = {0};
    framed[0] = 'i';
    memcpy(framed + 1, buf, got);
    framed[1 + got] = 'e';

    Bytes to_parse = bytes_make(framed, got + 2);
    BencodeValue parsed = {0};
    assert(ErrKindNone == bencode_parse_num(&to_parse, &parsed).kind);
    assert(BencodeKindInteger == parsed.kind);
    assert(n == parsed.v.num);
  }
}

// ---------------------------------------------------------------------------
// torrent_validate_info_dict
// ---------------------------------------------------------------------------

__attribute__((warn_unused_result)) static BencodeValue
test_bencode_dict(BencodeValue *items, usize len) {
  return (BencodeValue){.kind = BencodeKindDict,
                        .v.list = {.data = items, .len = len}};
}

static void test_torrent_validate_info_dict(void) {
  // Key ordering, which is the whole job: bencode wants dict keys strictly
  // ascending by raw byte value, with no duplicates. The pair that goes wrong
  // is varied across the dict on purpose -- the first adjacent pair is as easy
  // to skip as the last.
  {
    const struct {
      const char *k0;
      const char *k1;
      const char *k2;
      bool ok;
    } cases[] = {
        // Sorted.
        {"a", "b", "c", true},
        {"file tree", "meta version", "name", true},
        // The very first adjacent pair out of order.
        {"b", "a", "c", false},
        // The last one.
        {"a", "c", "b", false},
        // Duplicates are not ascending either, at either position.
        {"a", "a", "b", false},
        {"a", "b", "b", false},
        {"a", "a", "a", false},
        // A prefix sorts before what extends it, whichever way round.
        {"a", "aa", "aaa", true},
        {"aa", "a", "b", false},
        {"a", "aaa", "aa", false},
        // Compared as unsigned bytes: 0x01 before 0x80, never the reverse.
        {"\x01", "\x80", "\xff", true},
        {"\x80", "\x01", "\xff", false},
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      BencodeValue items[] = {
          test_bencode_bytes(cases[i].k0), test_bencode_int(1),
          test_bencode_bytes(cases[i].k1), test_bencode_int(2),
          test_bencode_bytes(cases[i].k2), test_bencode_int(3),
      };
      const BencodeValue dict =
          test_bencode_dict(items, sizeof(items) / sizeof(items[0]));

      assert((ErrKindNone == torrent_validate_info_dict(dict).kind) ==
             cases[i].ok);
    }
  }

  // Two keys, so there is exactly one adjacent pair to compare and nothing
  // else can stand in for it.
  {
    const struct {
      const char *k0;
      const char *k1;
      bool ok;
    } cases[] = {
        {"a", "b", true},
        {"b", "a", false},
        {"a", "a", false},
        {"name", "piece length", true},
        {"piece length", "name", false},
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      BencodeValue items[] = {
          test_bencode_bytes(cases[i].k0),
          test_bencode_int(1),
          test_bencode_bytes(cases[i].k1),
          test_bencode_int(2),
      };
      const BencodeValue dict =
          test_bencode_dict(items, sizeof(items) / sizeof(items[0]));

      assert((ErrKindNone == torrent_validate_info_dict(dict).kind) ==
             cases[i].ok);
    }
  }

  // One pair has no ordering to check and is accepted.
  {
    BencodeValue items[] = {test_bencode_bytes("name"), test_bencode_int(1)};
    assert(ErrKindNone ==
           torrent_validate_info_dict(test_bencode_dict(items, 2)).kind);
  }

  // An info dict with nothing in it carries none of the keys a torrent needs.
  {
    assert(ErrKindInvalidData ==
           torrent_validate_info_dict(test_bencode_dict(NULL, 0)).kind);
  }

  // An odd child count is a key without a value.
  {
    BencodeValue items[] = {test_bencode_bytes("a"), test_bencode_int(1),
                            test_bencode_bytes("b")};
    assert(ErrKindInvalidData ==
           torrent_validate_info_dict(test_bencode_dict(items, 3)).kind);
  }

  // Keys are byte strings, at every position, whatever else they might be.
  {
    for (usize bad = 0; bad < 3; bad++) {
      BencodeValue items[] = {
          test_bencode_bytes("a"), test_bencode_int(1),
          test_bencode_bytes("b"), test_bencode_int(2),
          test_bencode_bytes("c"), test_bencode_int(3),
      };
      // An integer where the key should be.
      items[bad * 2] = test_bencode_int(7);

      assert(ErrKindInvalidData ==
             torrent_validate_info_dict(
                 test_bencode_dict(items, sizeof(items) / sizeof(items[0])))
                 .kind);
    }
  }

  // A value may be anything, including a dict or a list: only keys are
  // constrained.
  {
    BencodeValue inner[] = {test_bencode_bytes("length"), test_bencode_int(1)};
    BencodeValue items[] = {
        test_bencode_bytes("file tree"),    test_bencode_dict(inner, 2),
        test_bencode_bytes("name"),         test_bencode_bytes("x"),
        test_bencode_bytes("piece length"), test_bencode_int(262144),
    };
    assert(ErrKindNone ==
           torrent_validate_info_dict(test_bencode_dict(items, 6)).kind);
  }
}

// `torrent_find_info_dict_in_metainfo` and `sha256_encode_hex_trunc` are only
// reached from `main`, so the suite never ran a line of either until now.

static void test_torrent_find_info_dict_in_metainfo(void) {
  BencodeValue info_children[] = {test_bencode_bytes("name"),
                                  test_bencode_bytes("x")};
  const BencodeValue info = test_bencode_dict(info_children, 2);

  // Found, wherever in the dict it sits.
  {
    BencodeValue first[] = {test_bencode_bytes("info"), info,
                            test_bencode_bytes("zzz"), test_bencode_int(1)};
    BencodeValue *const got =
        torrent_find_info_dict_in_metainfo(test_bencode_dict(first, 4));
    assert(got == &first[1]);
    assert(BencodeKindDict == got->kind);
  }
  {
    BencodeValue later[] = {
        test_bencode_bytes("announce"), test_bencode_bytes("http://x"),
        test_bencode_bytes("info"),     info,
        test_bencode_bytes("zzz"),      test_bencode_int(1)};
    assert(&later[3] ==
           torrent_find_info_dict_in_metainfo(test_bencode_dict(later, 6)));
  }

  // A pointer into the caller's list, not a copy: `main` hashes what it finds,
  // so it has to be the same value the metainfo holds.
  {
    BencodeValue items[] = {test_bencode_bytes("info"), info};
    BencodeValue *const got =
        torrent_find_info_dict_in_metainfo(test_bencode_dict(items, 2));
    assert(got == &items[1]);
    got->v.list.len = 0;
    assert(0 == items[1].v.list.len);
  }

  // Not found.
  {
    // No such key.
    BencodeValue none[] = {test_bencode_bytes("announce"), test_bencode_int(1)};
    assert(NULL ==
           torrent_find_info_dict_in_metainfo(test_bencode_dict(none, 2)));

    // The key is there but the value is not a dict.
    BencodeValue not_dict[] = {test_bencode_bytes("info"), test_bencode_int(1)};
    assert(NULL ==
           torrent_find_info_dict_in_metainfo(test_bencode_dict(not_dict, 2)));

    // A key that is not a byte string cannot be "info".
    BencodeValue bad_key[] = {test_bencode_int(1), info};
    assert(NULL ==
           torrent_find_info_dict_in_metainfo(test_bencode_dict(bad_key, 2)));

    // "info" as a *value* is not a key.
    BencodeValue as_value[] = {test_bencode_bytes("a"),
                               test_bencode_bytes("info"),
                               test_bencode_bytes("b"), info};
    assert(NULL ==
           torrent_find_info_dict_in_metainfo(test_bencode_dict(as_value, 4)));

    // A near miss, and an empty metainfo. (Not `near`: `windows.h` still
    // defines that, and `far`, from the 16-bit memory models.)
    BencodeValue almost[] = {test_bencode_bytes("infos"), info};
    assert(NULL ==
           torrent_find_info_dict_in_metainfo(test_bencode_dict(almost, 2)));
    assert(NULL ==
           torrent_find_info_dict_in_metainfo(test_bencode_dict(NULL, 0)));
  }
}

static void test_sha256_encode_hex_trunc(void) {
  // The real digest of the info dict of a torrent this program generated;
  // libtorrent reports the truncated form as the torrent's v1-style hash.
  {
    u8 digest[SHA256_DIGEST_LENGTH] = {0};
    const char *const full =
        "363b69d66ad2d57cbd51c2f27156aef8d0e68a7057d3dd8a5e911bcbb77483e3";
    for (usize i = 0; i < SHA256_DIGEST_LENGTH; i++) {
      u8 byte = 0;
      for (usize n = 0; n < 2; n++) {
        const char c = full[i * 2 + n];
        const u8 nibble = (u8)(c <= '9' ? c - '0' : c - 'a' + 10);
        byte = (u8)((byte << 4) | nibble);
      }
      digest[i] = byte;
    }

    u8 hex[40] = {0};
    sha256_encode_hex_trunc(digest, hex);

    // 20 bytes in, 40 characters out: the tail of the digest is dropped.
    assert(0 == memcmp(hex, "363b69d66ad2d57cbd51c2f27156aef8d0e68a70", 40));
  }

  // Nibble order, at the values where swapping the two halves of a byte shows.
  {
    const struct {
      u8 byte;
      const char *expected;
    } cases[] = {
        {0x00, "00"}, {0x01, "01"}, {0x0f, "0f"}, {0xf0, "f0"}, {0x10, "10"},
        {0xab, "ab"}, {0x7f, "7f"}, {0x80, "80"}, {0xff, "ff"},
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      u8 digest[SHA256_DIGEST_LENGTH] = {0};
      digest[0] = cases[i].byte;

      u8 hex[40] = {0};
      sha256_encode_hex_trunc(digest, hex);

      assert(0 == memcmp(hex, cases[i].expected, 2));
      // Everything else was a zero byte, so everything else is '0'.
      for (usize j = 2; j < sizeof(hex); j++) {
        assert('0' == hex[j]);
      }
    }
  }

  // Only the first 20 bytes are read: the last 12 cannot change the output.
  {
    u8 a[SHA256_DIGEST_LENGTH] = {0};
    u8 b[SHA256_DIGEST_LENGTH] = {0};
    memset(a, 0x5a, sizeof(a));
    memset(b, 0x5a, sizeof(b));
    memset(b + 20, 0xff, sizeof(b) - 20);

    u8 hex_a[40] = {0};
    u8 hex_b[40] = {0};
    sha256_encode_hex_trunc(a, hex_a);
    sha256_encode_hex_trunc(b, hex_b);

    assert(0 == memcmp(hex_a, hex_b, sizeof(hex_a)));
    for (usize i = 0; i < sizeof(hex_a); i += 2) {
      assert(0 == memcmp(hex_a + i, "5a", 2));
    }
  }

  // The lowercase alphabet, which is what BEP 14 and every peer expect.
  {
    u8 digest[SHA256_DIGEST_LENGTH] = {0};
    memset(digest, 0xbe, sizeof(digest));

    u8 hex[40] = {0};
    sha256_encode_hex_trunc(digest, hex);

    for (usize i = 0; i < sizeof(hex); i++) {
      const bool is_lower_hex =
          (hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f');
      assert(is_lower_hex);
    }
  }
}

// ---------------------------------------------------------------------------
// torrent_make_udp_broadcast_message
// ---------------------------------------------------------------------------

// The 40 hex characters of a truncated v2 info hash. This one is real: it is
// the digest of the info dict of a torrent this program generated, and
// libtorrent reports the same string for that file.
#define TEST_LSD_INFOHASH "363b69d66ad2d57cbd51c2f27156aef8d0e68a70"

// A BitTorrent v1 handshake on the wire is 68 bytes: one length byte, the 19
// protocol-name bytes it counts, 8 reserved bytes, the 20-byte info hash and
// the 20-byte peer id.
// Lay a well-formed handshake into `dst`. The reserved bytes are left zeroed;
// a test that cares about them writes them itself.
static void test_torrent_check_handshake(void) {
  // Not text: an info hash and a peer id are arbitrary bytes, so both carry a
  // NUL and a 0xFF to catch anything that treats them as C strings.
  u8 info_hash_bytes[20] = {0x00, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05,
                            0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
                            0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x00};
  const Bytes info_hash = bytes_make(info_hash_bytes, 20);

  u8 peer_id_bytes[20] = {'-', 'F', 'S', '0', '0', '0', '1', 0x00, 0xff, 0x2a,
                          1,   2,   3,   4,   5,   6,   7,   8,    9,    10};
  const Bytes peer_id_expected = bytes_make(peer_id_bytes, 20);

  // The whole point: a handshake whose info hash is the one we asked for is
  // accepted, and `*peer_id` comes back as the 20 bytes that follow it.
  {
    u8 data[TEST_HANDSHAKE_LEN] = {0};
    test_handshake_fill(data, info_hash, peer_id_expected);

    Bytes peer_id = {0};
    assert(torrent_check_handshake(bytes_make(data, sizeof(data)), info_hash,
                                   &peer_id));
    assert(bytes_eq(peer_id_expected, peer_id));

    // A view into the caller's buffer, not a copy: the peer id starts at the
    // 49th byte, right after the info hash.
    assert(data + 48 == peer_id.data);
    assert(20 == peer_id.len);
  }

  // The 8 reserved bytes say which extensions the peer speaks, and this does
  // not care which: all-ones is as acceptable as all-zeroes.
  {
    u8 data[TEST_HANDSHAKE_LEN] = {0};
    test_handshake_fill(data, info_hash, peer_id_expected);
    memset(data + 20, 0xff, 8);

    Bytes peer_id = {0};
    assert(torrent_check_handshake(bytes_make(data, sizeof(data)), info_hash,
                                   &peer_id));
    assert(bytes_eq(peer_id_expected, peer_id));
  }

  // A peer that answered with someone else's torrent. One differing byte is
  // enough, and it is rejected wherever in the hash it sits.
  {
    const usize positions[] = {0, 1, 10, 19};
    for (usize i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
      u8 data[TEST_HANDSHAKE_LEN] = {0};
      test_handshake_fill(data, info_hash, peer_id_expected);
      data[28 + positions[i]] ^= 0x01;

      Bytes peer_id = {0};
      assert(!torrent_check_handshake(bytes_make(data, sizeof(data)), info_hash,
                                      &peer_id));
      // Untouched on rejection, so a caller that forgets to check the return
      // value gets an obviously empty `Bytes` rather than someone else's bytes.
      assert(NULL == peer_id.data);
      assert(0 == peer_id.len);
    }
  }

  // The protocol name has to be exactly `BitTorrent protocol`, and the byte in
  // front of it exactly the 19 that counts it. A peer speaking something else
  // is not one of ours.
  {
    const char *const headers[] = {
        // The length byte, one too small and one too large.
        "\x12"
        "BitTorrent protocol",
        "\x14"
        "BitTorrent protocol",
        // The name itself, wrong at the front, in the middle, at the end.
        "\x13"
        "XitTorrent protocol",
        "\x13"
        "BitTorrent Protocol",
        "\x13"
        "BitTorrent protocoL",
        // A different protocol that happens to be 19 bytes long.
        "\x13"
        "AzureusMessaging!!!",
    };
    for (usize i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
      u8 data[TEST_HANDSHAKE_LEN] = {0};
      test_handshake_fill(data, info_hash, peer_id_expected);
      memcpy(data, headers[i], 20);

      Bytes peer_id = {0};
      assert(!torrent_check_handshake(bytes_make(data, sizeof(data)), info_hash,
                                      &peer_id));
      assert(NULL == peer_id.data);
    }
  }

  // Every length but 68 is refused, and refused by the length check rather
  // than by reading off the end: a short read is not a handshake yet, and a
  // long one has the next message glued to it. The loop starts at 1 because a
  // zero-length `Bytes` has no buffer to point at.
  {
    u8 data[TEST_HANDSHAKE_LEN + 4] = {0};
    test_handshake_fill(data, info_hash, peer_id_expected);
    memcpy(data + TEST_HANDSHAKE_LEN, "\x00\x00\x00\x00", 4);

    for (usize len = 1; len < sizeof(data); len++) {
      if (TEST_HANDSHAKE_LEN == len) {
        continue;
      }

      Bytes peer_id = {0};
      assert(
          !torrent_check_handshake(bytes_make(data, len), info_hash, &peer_id));
      assert(NULL == peer_id.data);
    }
  }

  // The empty `Bytes`, which has no data pointer at all.
  {
    Bytes peer_id = {0};
    assert(!torrent_check_handshake(bytes_make(NULL, 0), info_hash, &peer_id));
    assert(NULL == peer_id.data);
  }

  // An all-zero info hash is a legitimate 20 bytes, and matching it must not
  // be special-cased into a mismatch by anything comparing against NUL.
  {
    u8 zero_hash_bytes[20] = {0};
    const Bytes zero_hash = bytes_make(zero_hash_bytes, 20);

    u8 data[TEST_HANDSHAKE_LEN] = {0};
    test_handshake_fill(data, zero_hash, peer_id_expected);

    Bytes peer_id = {0};
    assert(torrent_check_handshake(bytes_make(data, sizeof(data)), zero_hash,
                                   &peer_id));
    assert(bytes_eq(peer_id_expected, peer_id));

    // ...and the non-zero hash must not match that same handshake.
    Bytes peer_id_other = {0};
    assert(!torrent_check_handshake(bytes_make(data, sizeof(data)), info_hash,
                                    &peer_id_other));
    assert(NULL == peer_id_other.data);
  }

  // An all-zero peer id is equally legitimate, and comes back as 20 bytes
  // rather than as an empty `Bytes`.
  {
    u8 zero_peer_id_bytes[20] = {0};
    const Bytes zero_peer_id = bytes_make(zero_peer_id_bytes, 20);

    u8 data[TEST_HANDSHAKE_LEN] = {0};
    test_handshake_fill(data, info_hash, zero_peer_id);

    Bytes peer_id = {0};
    assert(torrent_check_handshake(bytes_make(data, sizeof(data)), info_hash,
                                   &peer_id));
    assert(20 == peer_id.len);
    assert(bytes_eq(zero_peer_id, peer_id));
  }

  // `*peer_id` is overwritten, not merged into: a caller reusing one variable
  // across two peers gets the second peer's id, not a mix.
  {
    u8 other_peer_id_bytes[20] = {0};
    memset(other_peer_id_bytes, 0x5a, sizeof(other_peer_id_bytes));
    const Bytes other_peer_id = bytes_make(other_peer_id_bytes, 20);

    u8 first[TEST_HANDSHAKE_LEN] = {0};
    test_handshake_fill(first, info_hash, peer_id_expected);
    u8 second[TEST_HANDSHAKE_LEN] = {0};
    test_handshake_fill(second, info_hash, other_peer_id);

    Bytes peer_id = {0};
    assert(torrent_check_handshake(bytes_make(first, sizeof(first)), info_hash,
                                   &peer_id));
    assert(bytes_eq(peer_id_expected, peer_id));

    assert(torrent_check_handshake(bytes_make(second, sizeof(second)),
                                   info_hash, &peer_id));
    assert(bytes_eq(other_peer_id, peer_id));
  }
}

static void test_torrent_make_udp_broadcast_message(void) {
  // The exact bytes. The expected message is not invented here: it is the shape
  // libtorrent 2.1 sends, captured off the 239.192.152.143:6771 group, with
  // only the cookie value differing.
  //
  //   BT-SEARCH * HTTP/1.1\r\nHost: 239.192.152.143:6771\r\nPort: 6881\r\n
  //   Infohash: 363b69d6...\r\ncookie: 58eac522\r\n\r\n\r\n
  //
  // `Host` carries the multicast group the datagram goes to, `Port` the port
  // this peer listens on, so the two port numbers are different on purpose.
  {
    Arena arena = test_arena(4 * KiB);

    Bytes msg = {0};
    assert(ErrKindNone == torrent_make_udp_broadcast_message(
                              bytes_from_cstr("239.192.152.143:6771"), 6881,
                              bytes_from_cstr(TEST_LSD_INFOHASH), &arena, &msg)
                              .kind);

    const char *const expected = "BT-SEARCH * HTTP/1.1\r\n"
                                 "Host: 239.192.152.143:6771\r\n"
                                 "Port: 6881\r\n"
                                 "Infohash: " TEST_LSD_INFOHASH "\r\n"
                                 "cookie: fixme\r\n"
                                 "\r\n"
                                 "\r\n";
    assert(bytes_eq_cstr(msg, expected));
    assert(strlen(expected) == msg.len);
  }

  // The header block ends with three CRLFs after the cookie value, and the
  // request line is a `BT-SEARCH`, not a `GET`. Spelled out separately so a
  // change to either fails with an obvious reason rather than as one long
  // string mismatch.
  {
    Arena arena = test_arena(4 * KiB);

    Bytes msg = {0};
    assert(ErrKindNone == torrent_make_udp_broadcast_message(
                              bytes_from_cstr("239.192.152.143:6771"), 6881,
                              bytes_from_cstr(TEST_LSD_INFOHASH), &arena, &msg)
                              .kind);

    const Bytes first_line = bytes_take(msg, 22);
    assert(bytes_eq_cstr(first_line, "BT-SEARCH * HTTP/1.1\r\n"));

    assert(msg.len >= 6);
    const Bytes tail = {.data = msg.data + msg.len - 6, .len = 6};
    assert(bytes_eq_cstr(tail, "\r\n\r\n\r\n"));
  }

  // The port is written as decimal digits, at both ends of a `u16`.
  {
    const struct {
      u16 port;
      const char *expected_line;
    } cases[] = {
        {0, "\r\nPort: 0\r\n"},
        {1, "\r\nPort: 1\r\n"},
        {6881, "\r\nPort: 6881\r\n"},
        // The widest a `u16` gets, which is what the capacity has to allow for.
        {65535, "\r\nPort: 65535\r\n"},
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      Arena arena = test_arena(4 * KiB);

      Bytes msg = {0};
      assert(ErrKindNone == torrent_make_udp_broadcast_message(
                                bytes_from_cstr("host"), cases[i].port,
                                bytes_from_cstr(TEST_LSD_INFOHASH), &arena,
                                &msg)
                                .kind);

      assert(test_bytes_contains(msg, bytes_from_cstr(cases[i].expected_line)));
    }
  }

  // The capacity is `128 + url.len`, so a long host has to fit rather than trip
  // one of the `assert`s inside: every append is asserted, so an overflow would
  // abort the process instead of coming back as an error.
  {
    Arena arena = test_arena(64 * KiB);

    u8 long_url[512] = {0};
    memset(long_url, 'h', sizeof(long_url));
    const Bytes url = bytes_make(long_url, sizeof(long_url));

    Bytes msg = {0};
    assert(ErrKindNone ==
           torrent_make_udp_broadcast_message(
               url, 65535, bytes_from_cstr(TEST_LSD_INFOHASH), &arena, &msg)
               .kind);

    assert(test_bytes_contains(msg, url));
    // Everything the message holds besides the host name.
    assert(msg.len ==
           sizeof(long_url) + strlen("BT-SEARCH * HTTP/1.1\r\n"
                                     "Host: \r\nPort: 65535\r\n"
                                     "Infohash: " TEST_LSD_INFOHASH
                                     "\r\ncookie: fixme\r\n\r\n\r\n"));
  }

  // An empty host and an empty info hash: `bytes_buffer_extend_within_cap`
  // takes an empty `Bytes` as a no-op, so the message is still well formed,
  // just missing those two values.
  {
    Arena arena = test_arena(4 * KiB);

    Bytes msg = {0};
    assert(ErrKindNone ==
           torrent_make_udp_broadcast_message(bytes_make(NULL, 0), 6881,
                                              bytes_make(NULL, 0), &arena, &msg)
               .kind);

    assert(bytes_eq_cstr(msg, "BT-SEARCH * HTTP/1.1\r\n"
                              "Host: \r\n"
                              "Port: 6881\r\n"
                              "Infohash: \r\n"
                              "cookie: fixme\r\n"
                              "\r\n"
                              "\r\n"));
  }

  // Out of arena: reported, and `*dst` is left as the caller had it.
  {
    Arena arena = test_arena(8);

    Bytes msg = {.data = (u8 *)&arena, .len = 123};
    assert(ErrKindOOM == torrent_make_udp_broadcast_message(
                             bytes_from_cstr("239.192.152.143:6771"), 6881,
                             bytes_from_cstr(TEST_LSD_INFOHASH), &arena, &msg)
                             .kind);

    assert((u8 *)&arena == msg.data);
    assert(123 == msg.len);
  }

  // The message is built in the arena it was handed, and the arena advanced.
  {
    Arena arena = test_arena(4 * KiB);
    const u8 *const before = arena.start;

    Bytes msg = {0};
    assert(ErrKindNone ==
           torrent_make_udp_broadcast_message(
               bytes_from_cstr("host"), 1, bytes_from_cstr("aa"), &arena, &msg)
               .kind);

    assert(msg.data >= before);
    assert(arena.start > before);
  }
}

// ---------------------------------------------------------------------------
// BytesBuffer
// ---------------------------------------------------------------------------

// The bytes built so far. Every caller wants this and there is no accessor for
// it, so the shape is spelled out once here instead of at each call site.
__attribute__((warn_unused_result)) static Bytes test_sb_built(BytesBuffer bb) {
  return bytes_take(bb.container, bb.len);
}

// A capacity of 0 is not tested: `bytes_buffer_make` forwards it to
// `arena_alloc`, which asserts `elem_count > 0`, so it aborts rather than
// failing.
static void test_sb_make(void) {
  // A fresh buffer owns its capacity and holds nothing.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(16, &arena, &bb).kind);

    assert(bb.container.data);
    assert(16 == bb.container.len);
    assert(0 == bb.len);
    assert(16 == bytes_buffer_space(bb));
    assert(bytes_is_empty(test_sb_built(bb)));
  }

  // Out of arena: reported, and `*dst` is left exactly as the caller had it.
  {
    Arena arena = test_arena(8);
    u8 poison[6] = {0};
    BytesBuffer bb = {.container = {.data = poison, .len = sizeof(poison)},
                      .len = 3};
    assert(ErrKindOOM == bytes_buffer_make(4 * KiB, &arena, &bb).kind);

    assert(poison == bb.container.data);
    assert(sizeof(poison) == bb.container.len);
    assert(3 == bb.len);
  }

  // Reusing a `BytesBuffer` that already holds something: `bytes_buffer_make`
  // hands back a buffer that is empty, not one that looks part-filled.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(8, &arena, &bb).kind);
    assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("stale")));
    assert(5 == bb.len);

    assert(ErrKindNone == bytes_buffer_make(8, &arena, &bb).kind);
    assert(0 == bb.len);
    assert(8 == bytes_buffer_space(bb));
    assert(bytes_is_empty(test_sb_built(bb)));
  }

  // Two buffers from one arena do not overlap.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer a = {0};
    BytesBuffer b = {0};
    assert(ErrKindNone == bytes_buffer_make(8, &arena, &a).kind);
    assert(ErrKindNone == bytes_buffer_make(8, &arena, &b).kind);

    assert(a.container.data != b.container.data);
    assert(a.container.data + 8 <= b.container.data ||
           b.container.data + 8 <= a.container.data);
  }
}

static void test_sb_extend_within_cap(void) {
  // Appending in pieces is the same as appending the whole.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(16, &arena, &bb).kind);

    assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("ab")));
    assert(2 == bb.len);
    assert(14 == bytes_buffer_space(bb));

    assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("cde")));
    assert(5 == bb.len);
    assert(bytes_eq_cstr(test_sb_built(bb), "abcde"));
  }

  // Filling the capacity exactly is allowed; one byte more is not.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(5, &arena, &bb).kind);

    assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("abcde")));
    assert(5 == bb.len);
    assert(0 == bytes_buffer_space(bb));

    // Full: even one byte is refused, and nothing changes.
    assert(!bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("f")));
    assert(5 == bb.len);
    assert(bytes_eq_cstr(test_sb_built(bb), "abcde"));
  }

  // A refusal is all-or-nothing: no prefix of the input is written, and the
  // bytes past `len` are left as they were.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(8, &arena, &bb).kind);
    memset(bb.container.data, '#', bb.container.len);

    assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("abc")));
    assert(!bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("defghi")));

    assert(3 == bb.len);
    assert(bytes_eq_cstr(test_sb_built(bb), "abc"));
    for (usize i = 3; i < bb.container.len; i++) {
      assert('#' == bb.container.data[i]);
    }
  }

  // Appending nothing always succeeds and moves nothing, on a full buffer too.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(2, &arena, &bb).kind);

    assert(bytes_buffer_extend_within_cap(&bb, bytes_make(NULL, 0)));
    assert(0 == bb.len);

    assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("xy")));
    assert(0 == bytes_buffer_space(bb));
    assert(bytes_buffer_extend_within_cap(&bb, bytes_make(NULL, 0)));
    assert(2 == bb.len);
  }

  // A body is bytes, not text: NUL and 0x80..0xff go through unchanged, and
  // the NUL does not terminate anything.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(8, &arena, &bb).kind);

    const u8 raw[] = {'a', 0x00, 0x80, 0xff, 'b'};
    assert(bytes_buffer_extend_within_cap(&bb,
                                          bytes_make((u8 *)raw, sizeof(raw))));

    assert(sizeof(raw) == bb.len);
    assert(0 == memcmp(bb.container.data, raw, sizeof(raw)));
  }
}

static void test_sb_append_usize_within_cap(void) {
  // Width is the digit count, and the digits land where `len` points.
  {
    const struct {
      usize n;
      const char *expected;
    } cases[] = {
        {0, "0"},         {7, "7"},
        {9, "9"},         {10, "10"},
        {99, "99"},       {100, "100"},
        {12345, "12345"}, {UINT64_MAX, "18446744073709551615"},
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      Arena arena = test_arena(4 * KiB);
      BytesBuffer bb = {0};
      assert(ErrKindNone == bytes_buffer_make(32, &arena, &bb).kind);

      // A prefix first, so the digits are not written at offset 0.
      assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("n=")));
      assert(bytes_buffer_append_usize_within_cap(&bb, cases[i].n));

      assert(2 + strlen(cases[i].expected) == bb.len);
      assert(0 == memcmp(bb.container.data + 2, cases[i].expected,
                         strlen(cases[i].expected)));
    }
  }

  // The digits fit exactly, and one digit short is refused whole.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(3, &arena, &bb).kind);

    assert(bytes_buffer_append_usize_within_cap(&bb, 123));
    assert(3 == bb.len);
    assert(bytes_eq_cstr(test_sb_built(bb), "123"));
  }
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(3, &arena, &bb).kind);
    memset(bb.container.data, '#', bb.container.len);

    // Four digits into three bytes: refused, and no digit is written.
    assert(!bytes_buffer_append_usize_within_cap(&bb, 1234));
    assert(0 == bb.len);
    for (usize i = 0; i < bb.container.len; i++) {
      assert('#' == bb.container.data[i]);
    }

    // `0` is one digit wide, not zero, so it still needs room.
    assert(bytes_buffer_append_usize_within_cap(&bb, 0));
    assert(1 == bb.len);
  }

  // A full buffer refuses even the narrowest number.
  {
    Arena arena = test_arena(4 * KiB);
    BytesBuffer bb = {0};
    assert(ErrKindNone == bytes_buffer_make(1, &arena, &bb).kind);

    assert(bytes_buffer_append_usize_within_cap(&bb, 5));
    assert(0 == bytes_buffer_space(bb));
    assert(!bytes_buffer_append_usize_within_cap(&bb, 0));
    assert(1 == bb.len);
    assert(bytes_eq_cstr(test_sb_built(bb), "5"));
  }
}

// The two appends interleaved, against the exact bytes: this is the shape
// `torrent_make_udp_broadcast_message` builds, at a capacity with room to
// spare, so a refusal here would be a bug and not a bound.
static void test_sb_build(void) {
  Arena arena = test_arena(4 * KiB);
  BytesBuffer bb = {0};
  assert(ErrKindNone == bytes_buffer_make(64, &arena, &bb).kind);

  assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("Host: ")));
  assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("localhost")));
  assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("\r\nPort: ")));
  assert(bytes_buffer_append_usize_within_cap(&bb, 12345));
  assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("\r\n")));

  const char *const expected = "Host: localhost\r\nPort: 12345\r\n";
  assert(strlen(expected) == bb.len);
  assert(bytes_eq_cstr(test_sb_built(bb), expected));
  assert(64 - strlen(expected) == bytes_buffer_space(bb));
}

// ---------------------------------------------------------------------------
// bencode_encode
// ---------------------------------------------------------------------------

// Encode into a poisoned buffer one byte wider than needed, so a write past
// the end is visible, and compare against the exact expected bytes.
static void test_bencode_encode_once(BencodeValue b, const char *expected) {
  assert(expected);
  const usize expected_len = strlen(expected);

  // The sizing pass predicts the encoding to the byte, so it is a check
  // rather than a bound.
  const usize size = bencode_encode_exact_size(b, 0);
  assert(size == expected_len);

  // Allocate one byte more than that, so a write past the end is visible.
  const usize cap = size + 1;

  Arena arena = test_arena(64 * KiB);
  Bytes dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
               .len = cap};
  assert(dst.data);
  memset(dst.data, '#', cap);

  const usize written = bencode_encode_in_place(b, bytes_take(dst, size));

  // Comparing from the front of `dst` pins the anchoring.
  assert(expected_len == written);
  assert(0 == memcmp(dst.data, expected, written));

  // Nothing past what it reports was touched.
  for (usize i = written; i < cap; i++) {
    assert('#' == dst.data[i]);
  }
}

static void test_bencode_encode_leaves(void) {
  test_bencode_encode_once(test_bencode_int(0), "i0e");
  test_bencode_encode_once(test_bencode_int(1), "i1e");
  test_bencode_encode_once(test_bencode_int(-1), "i-1e");
  test_bencode_encode_once(test_bencode_int(42), "i42e");
  test_bencode_encode_once(test_bencode_int(-42), "i-42e");
  test_bencode_encode_once(test_bencode_int(INT64_MAX),
                           "i9223372036854775807e");
  test_bencode_encode_once(test_bencode_int(INT64_MIN),
                           "i-9223372036854775808e");

  test_bencode_encode_once(test_bencode_bytes(""), "0:");
  test_bencode_encode_once(test_bencode_bytes("a"), "1:a");
  test_bencode_encode_once(test_bencode_bytes("spam"), "4:spam");
  test_bencode_encode_once(test_bencode_bytes("piece length"),
                           "12:piece length");

  // A byte string whose `data` is null, which is how the `""` key of a v2 file
  // tree is built. `memcpy` wants valid pointers even for a zero byte copy.
  const BencodeValue null_bytes = {.kind = BencodeKindBytes, .v.bytes = {0}};
  test_bencode_encode_once(null_bytes, "0:");
}

// Strings are byte strings: NULs and high bytes pass through untouched, and
// the length prefix counts bytes rather than stopping at a terminator.
static void test_bencode_encode_non_ascii_bytes(void) {
  u8 raw[6] = {0x00, 0xff, 'a', 0x00, 0x80, '\n'};
  const BencodeValue b = {.kind = BencodeKindBytes,
                          .v.bytes = bytes_make(raw, sizeof(raw))};

  Arena arena = test_arena(64 * KiB);
  const usize cap = bencode_encode_exact_size(b, 0);
  Bytes dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
               .len = cap};
  assert(dst.data);

  const usize written = bencode_encode_in_place(b, dst);

  assert(bencode_encode_exact_size(b, 0) == written);
  assert(2 + sizeof(raw) == written);
  assert(0 == memcmp(dst.data, "6:", 2));
  assert(0 == memcmp(dst.data + 2, raw, sizeof(raw)));
}

static void test_bencode_encode_containers(void) {
  Arena arena = test_arena(64 * KiB);

  // Empty containers still carry their framing.
  const BencodeValue empty_list = {.kind = BencodeKindList, .v.list = {0}};
  const BencodeValue empty_dict = {.kind = BencodeKindDict, .v.list = {0}};
  test_bencode_encode_once(empty_list, "le");
  test_bencode_encode_once(empty_dict, "de");

  // `l4:spami42ee`
  BencodeValue *items =
      arena_alloc(&arena, __alignof__(BencodeValue), sizeof(BencodeValue), 2);
  assert(items);
  items[0] = test_bencode_bytes("spam");
  items[1] = test_bencode_int(42);
  const BencodeValue list = {
      .kind = BencodeKindList, .v.list.len = 2, .v.list.data = items};
  test_bencode_encode_once(list, "l4:spami42ee");

  // `d3:keyl4:spami42eee`: a container nested inside a dict.
  BencodeValue *pair =
      arena_alloc(&arena, __alignof__(BencodeValue), sizeof(BencodeValue), 2);
  assert(pair);
  pair[0] = test_bencode_bytes("key");
  pair[1] = list;
  const BencodeValue dict = {
      .kind = BencodeKindDict, .v.list.len = 2, .v.list.data = pair};
  test_bencode_encode_once(dict, "d3:keyl4:spami42eee");
}

// Breadth is not depth: a dict with more keys than `BENCODE_MAX_DEPTH` is
// ordinary bencode and must encode.
static void test_bencode_encode_wide_dict(void) {
  const usize pairs = BENCODE_MAX_DEPTH + 1;

  Arena arena = test_arena(1 * MiB);
  BencodeValue *entries = arena_alloc(&arena, __alignof__(BencodeValue),
                                      sizeof(BencodeValue), pairs * 2);
  assert(entries);
  u8 *keys = arena_alloc(&arena, __alignof__(u8), sizeof(u8), pairs * 4);
  assert(keys);

  for (usize i = 0; i < pairs; i++) {
    // `k000`, `k001`, ... : fixed width, so they are already sorted.
    u8 *const key = keys + i * 4;
    key[0] = 'k';
    key[1] = (u8)('0' + (i / 100) % 10);
    key[2] = (u8)('0' + (i / 10) % 10);
    key[3] = (u8)('0' + i % 10);

    entries[2 * i] =
        (BencodeValue){.kind = BencodeKindBytes, .v.bytes = bytes_make(key, 4)};
    entries[2 * i + 1] = test_bencode_int((isize)i);
  }

  const BencodeValue dict = {
      .kind = BencodeKindDict, .v.list.len = pairs * 2, .v.list.data = entries};

  const usize cap = bencode_encode_exact_size(dict, 0);
  Bytes dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
               .len = cap};
  assert(dst.data);

  const usize written = bencode_encode_in_place(dict, dst);

  assert(bencode_encode_exact_size(dict, 0) == written);
  assert(written > 2);
  assert('d' == dst.data[0]);
  assert('e' == dst.data[written - 1]);
  assert(0 == memcmp(dst.data + 1, "4:k000i0e", 9));

  // It is real bencode, with every pair still there.
  Arena parse_arena = test_arena(1 * MiB);
  Arena scratch = test_arena(1 * MiB);
  Bytes to_parse = bytes_take(dst, written);
  BencodeValue parsed = {0};
  assert(ErrKindNone ==
         bencode_parse(&to_parse, &parse_arena, scratch, &parsed).kind);
  assert(BencodeKindDict == parsed.kind);
  assert(pairs * 2 == parsed.v.list.len);
}

// Encoding is the inverse of parsing: parse a document, encode it back, and
// the bytes must be identical. Bencode has exactly one representation per
// value, so any deviation is a bug in one of the two.
static void test_bencode_encode_round_trip(void) {
  const char *documents[] = {
      "i0e",
      "i-1e",
      "i9223372036854775807e",
      "i-9223372036854775808e",
      "0:",
      "4:spam",
      "le",
      "de",
      "l4:spam4:eggse",
      "li0ei1ei2ee",
      "d3:cow3:moo4:spam4:eggse",
      "d4:spaml1:a1:bee",
      "d9:publisher3:bob18:publisher.location4:homee",
      "lli1ei2eeli3ei4eee",
      "d1:ad1:bd1:cd1:d0:eeee",
  };

  for (usize i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
    Arena arena = test_arena(64 * KiB);
    Arena scratch = test_arena(64 * KiB);

    Bytes input = bytes_from_cstr(documents[i]);
    const Bytes original = input;

    BencodeValue parsed = {0};
    assert(ErrKindNone == bencode_parse(&input, &arena, scratch, &parsed).kind);

    test_bencode_encode_once(parsed, documents[i]);

    // And the encoding really is the whole input, not a prefix of it.
    assert(strlen(documents[i]) == original.len);
  }
}

// The v2 info dict, byte for byte against what libtorrent 2.1.1 produces for
// the same file. This pins the encoder, the dict construction and the merkle
// root together: any one of them drifting changes the infohash.
static void test_bencode_encode_torrent_info(void) {
  const usize file_len = 40960;

  Arena arena = test_arena(4 * MiB);
  u8 *const file_data =
      arena_alloc(&arena, __alignof__(u8), sizeof(u8), file_len);
  assert(file_data);
  memset(file_data, 'x', file_len);

  const Bytes name = bytes_from_cstr("f.bin");
  BencodeValue info = {0};
  PieceHash *piece_hashes = NULL;
  usize piece_hashes_count = 0;
  Bytes pieces_root_bytes = {0};
  assert(ErrKindNone ==
         torrent_make_info_dict_v2(name, 16 * TORRENT_BLOCK_SIZE,
                                   bytes_make(file_data, file_len), name, &info,
                                   &pieces_root_bytes, &piece_hashes,
                                   &piece_hashes_count, &arena)
             .kind);

  const usize cap = bencode_encode_exact_size(info, 0);
  Bytes dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
               .len = cap};
  assert(dst.data);

  const Bytes got = bytes_take(dst, bencode_encode_in_place(info, dst));
  assert(bencode_encode_exact_size(info, 0) == got.len);

  // The digest is raw bytes, so build the expectation around it rather than
  // embedding it in a string literal.
  const char *const prefix =
      "d9:file treed5:f.bind0:d6:lengthi40960e11:pieces root32:";
  const char *const suffix =
      "eee12:meta versioni2e4:name5:f.bin12:piece lengthi262144ee";

  u8 pieces_root[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(
      "8431e3abfbd82a618e0b0c4113dff17b644df6bd102fd127df5bd7a531014b3a",
      pieces_root);

  const usize prefix_len = strlen(prefix);
  const usize suffix_len = strlen(suffix);
  assert(prefix_len + SHA256_DIGEST_LENGTH + suffix_len == got.len);

  assert(0 == memcmp(got.data, prefix, prefix_len));
  assert(0 == memcmp(got.data + prefix_len, pieces_root, SHA256_DIGEST_LENGTH));
  assert(0 == memcmp(got.data + prefix_len + SHA256_DIGEST_LENGTH, suffix,
                     suffix_len));

  // The v2 infohash is the digest of exactly these bytes.
  u8 infohash[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest(got, infohash);

  u8 expected_infohash[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(
      "e556ed46dace469f8f24053de5ad85478349c13544a4710b6d624454b85e1256",
      expected_infohash);
  assert(0 == memcmp(infohash, expected_infohash, sizeof(infohash)));
}

// The vector block function must be indistinguishable from the scalar one, so
// compare them directly rather than only through the public digest: a
// Decode a run of concatenated hex digests, the form `piece layers` values
// are published in. `test_digest_from_hex` takes exactly one digest, so feed
// it one 64 character window at a time.
static void test_digests_from_hex(const char *hex, Bytes dst) {
  assert(hex);
  assert(0 == dst.len % SHA256_DIGEST_LENGTH);
  assert(2 * dst.len == strlen(hex));

  for (usize i = 0; i < dst.len / SHA256_DIGEST_LENGTH; i++) {
    char one[2 * SHA256_DIGEST_LENGTH + 1] = {0};
    memcpy(one, hex + i * 2 * SHA256_DIGEST_LENGTH, 2 * SHA256_DIGEST_LENGTH);
    test_digest_from_hex(one, dst.data + i * SHA256_DIGEST_LENGTH);
  }
}

// One case of `torrent_make_metainfo_dict_v2`: build the info dict for a file
// of `file_len` bytes of 'x', wrap it, and check the result against vectors
// generated with libtorrent 2.1.1 from byte-identical input.
//
// `expected_layer_hex` is empty for a file that fits in a single piece, which
// BEP 52 gives no piece layer at all.
static void test_torrent_metainfo_once(usize file_len,
                                       const char *expected_root_hex,
                                       const char *expected_infohash_hex,
                                       const char *expected_layer_hex) {
  Arena arena = test_arena(16 * MiB);
  const Arena scratch = test_arena(16 * MiB);

  u8 *const file_data =
      arena_alloc(&arena, __alignof__(u8), sizeof(u8), file_len);
  assert(file_data);
  memset(file_data, 'x', file_len);

  const Bytes name = bytes_from_cstr("f.bin");
  const Bytes announce = bytes_from_cstr("http://localhost:12345");

  BencodeValue info = {0};
  Bytes pieces_root = {0};
  PieceHash *piece_hashes = NULL;
  usize piece_hashes_count = 0;
  assert(ErrKindNone ==
         torrent_make_info_dict_v2(name, 16 * TORRENT_BLOCK_SIZE,
                                   bytes_make(file_data, file_len), name, &info,
                                   &pieces_root, &piece_hashes,
                                   &piece_hashes_count, &arena)
             .kind);

  // The root the info dict publishes is the one libtorrent computes.
  u8 expected_root[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(expected_root_hex, expected_root);
  assert(SHA256_DIGEST_LENGTH == pieces_root.len);
  assert(0 == memcmp(pieces_root.data, expected_root, sizeof(expected_root)));

  const usize expected_layer_len = strlen(expected_layer_hex) / 2;
  assert(piece_hashes_count * SHA256_DIGEST_LENGTH == expected_layer_len);

  BencodeValue metainfo = {0};
  assert(ErrKindNone == torrent_make_metainfo_dict_v2(
                            pieces_root, announce, info.v.list, piece_hashes,
                            piece_hashes_count, &metainfo, &arena)
                            .kind);

  // Three keys, in the order bencode requires: announce < info < piece
  // layers.
  assert(BencodeKindDict == metainfo.kind);
  assert(2 * 3 == metainfo.v.list.len);
  assert(test_bencode_is_bytes(metainfo.v.list.data[0], "announce"));
  assert(test_bencode_is_bytes(metainfo.v.list.data[2], "info"));
  assert(test_bencode_is_bytes(metainfo.v.list.data[4], "piece layers"));

  const BencodeValue announce_value = metainfo.v.list.data[1];
  assert(BencodeKindBytes == announce_value.kind);
  assert(announce.len == announce_value.v.bytes.len);
  assert(0 == memcmp(announce_value.v.bytes.data, announce.data, announce.len));

  // `info` is nested by value: the same tree, not a re-encoding of it.
  const BencodeValue info_value = metainfo.v.list.data[3];
  assert(BencodeKindDict == info_value.kind);
  assert(info.v.list.len == info_value.v.list.len);
  assert(info.v.list.data == info_value.v.list.data);

  const BencodeValue layers = metainfo.v.list.data[5];
  assert(BencodeKindDict == layers.kind);

  if (0 == expected_layer_len) {
    // A single piece file gets no layer, but the key is emitted regardless.
    assert(0 == piece_hashes_count);
    assert(0 == layers.v.list.len);
    assert(NULL == layers.v.list.data);
  } else {
    assert(2 == layers.v.list.len);

    // Keyed by the merkle root. Emphatically not by the file name: that is
    // the mistake this pins down, and it is invisible in a hex dump.
    const BencodeValue layer_key = layers.v.list.data[0];
    assert(BencodeKindBytes == layer_key.kind);
    assert(SHA256_DIGEST_LENGTH == layer_key.v.bytes.len);
    assert(0 ==
           memcmp(layer_key.v.bytes.data, expected_root, SHA256_DIGEST_LENGTH));
    assert(!bytes_eq_cstr(layer_key.v.bytes, "f.bin"));

    const BencodeValue layer_value = layers.v.list.data[1];
    assert(BencodeKindBytes == layer_value.kind);
    assert(expected_layer_len == layer_value.v.bytes.len);

    Bytes expected_layer = {.data = arena_alloc(&arena, __alignof__(u8),
                                                sizeof(u8), expected_layer_len),
                            .len = expected_layer_len};
    assert(expected_layer.data);
    test_digests_from_hex(expected_layer_hex, expected_layer);
    assert(0 == memcmp(layer_value.v.bytes.data, expected_layer.data,
                       expected_layer_len));

    // And it is exactly the piece hashes the merkle build handed over,
    // concatenated, in order, with nothing inserted between them.
    assert(0 ==
           memcmp(layer_value.v.bytes.data, piece_hashes, expected_layer_len));
  }

  // The infohash is over the info dict alone, so encoding it on its own and
  // then finding those exact bytes inside the metainfo encoding is the
  // property everything downstream depends on: nesting must not perturb them.
  Bytes info_encoded = {0};
  assert(ErrKindNone == bencode_encode(info, &info_encoded, &arena).kind);

  u8 infohash[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest(info_encoded, infohash);
  u8 expected_infohash[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(expected_infohash_hex, expected_infohash);
  assert(0 == memcmp(infohash, expected_infohash, sizeof(infohash)));

  Bytes metainfo_encoded = {0};
  assert(ErrKindNone ==
         bencode_encode(metainfo, &metainfo_encoded, &arena).kind);
  assert(metainfo_encoded.len > info_encoded.len);
  assert(test_bytes_contains(metainfo_encoded, info_encoded));

  // The envelope is what it claims to be, and an empty layer dict really does
  // encode as the two bytes `de` rather than vanishing.
  const char *const prefix = "d8:announce22:http://localhost:123454:infod";
  assert(metainfo_encoded.len > strlen(prefix));
  assert(0 == memcmp(metainfo_encoded.data, prefix, strlen(prefix)));
  const char *const suffix =
      0 == expected_layer_len ? "12:piece layersdee" : "ee";
  assert(metainfo_encoded.len > strlen(suffix));
  assert(0 ==
         memcmp(metainfo_encoded.data + metainfo_encoded.len - strlen(suffix),
                suffix, strlen(suffix)));

  // Re-parsing is the ordering check. `bencode_parse` validates that each
  // dict's keys are sorted and unique as it closes, which the encoder itself
  // never does, so a mis-ordered key only ever shows up here.
  Bytes to_parse = metainfo_encoded;
  BencodeValue reparsed = {0};
  assert(ErrKindNone ==
         bencode_parse(&to_parse, &arena, scratch, &reparsed).kind);
  assert(0 == to_parse.len);
  assert(BencodeKindDict == reparsed.kind);
  assert(2 * 3 == reparsed.v.list.len);
}

static void test_torrent_metainfo_v2(void) {
  // Two full pieces plus a short one. Three pieces is deliberately not a
  // power of two: the tree pads its piece layer out to four, and BEP 52
  // publishes only the three that cover real data, so a layer handed over at
  // the padded width fails here and nowhere else.
  test_torrent_metainfo_once(
      2 * 16 * TORRENT_BLOCK_SIZE + 7,
      "fab5fcffb2780746dc0870e6e1a4c850e5e6fc3e86a3081e3aceba534803482d",
      "5761af1f00979ade4dc7f7306ecfa185f836f1b084fdbb2e4824815aa6adfc64",
      "4f663eeec104d7af532a0e578b229bd1a65d120d4a50d34309a853d313c2767c"
      "4f663eeec104d7af532a0e578b229bd1a65d120d4a50d34309a853d313c2767c"
      "76622ce611630f1e7d0d2d20adb28631d45c276e33d91adef80c4ba69353d14e");

  // Three full pieces plus a short one: four pieces, exercising a layer whose
  // width happens to match the padded tree width.
  test_torrent_metainfo_once(
      3 * 16 * TORRENT_BLOCK_SIZE + 7,
      "5c82a4b29ea8a563def0471f9eb5d0d75f5c949ab78fbd27afc6b064478f80e8",
      "fe1071f8c428a5575aace7e5ec30bd3be04cfd1fcbbf0a8c7cf0864728359ddc",
      "4f663eeec104d7af532a0e578b229bd1a65d120d4a50d34309a853d313c2767c"
      "4f663eeec104d7af532a0e578b229bd1a65d120d4a50d34309a853d313c2767c"
      "4f663eeec104d7af532a0e578b229bd1a65d120d4a50d34309a853d313c2767c"
      "76622ce611630f1e7d0d2d20adb28631d45c276e33d91adef80c4ba69353d14e");

  // Smaller than one piece: no layer, but `piece layers` is still emitted.
  test_torrent_metainfo_once(
      40960, "8431e3abfbd82a618e0b0c4113dff17b644df6bd102fd127df5bd7a531014b3a",
      "e556ed46dace469f8f24053de5ad85478349c13544a4710b6d624454b85e1256", "");
}

// disagreement on one block is otherwise easy to miss behind a passing
// vector.
static void test_sha256_neon_matches_scalar(void) {
#if !SHA256_HAS_NEON
  return;
#else
  if (!sha256_neon_supported()) {
    return;
  }

  // Blocks and chaining states that the extension is prone to getting wrong:
  // all zeroes, all ones, and the byte order boundaries.
  const u8 patterns[] = {0x00, 0xff, 0x80, 0x01, 0x7f};
  for (usize p = 0; p < sizeof(patterns); p++) {
    u8 block[SHA256_CBLOCK];
    memset(block, patterns[p], sizeof(block));

    u32 scalar[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u32 neon[8] = {0};
    memcpy(neon, scalar, sizeof(neon));

    sha256_compress(scalar, block);
    sha256_compress_blocks_neon(neon, block, 1);
    assert(0 == memcmp(scalar, neon, sizeof(scalar)));
  }

  // Then a deterministic sweep over both inputs. The chaining state is varied
  // too, not just the block: the two differ in how they carry state in and
  // out.
  u32 x = 0x00c0ffee;
  for (usize iter = 0; iter < 20000; iter++) {
    u8 block[SHA256_CBLOCK];
    for (usize i = 0; i < sizeof(block); i++) {
      x = x * 1664525u + 1013904223u;
      block[i] = (u8)(x >> 24);
    }

    u32 scalar[8] = {0};
    u32 neon[8] = {0};
    for (usize i = 0; i < 8; i++) {
      x = x * 1664525u + 1013904223u;
      scalar[i] = x;
      neon[i] = x;
    }

    sha256_compress(scalar, block);
    sha256_compress_blocks_neon(neon, block, 1);
    assert(0 == memcmp(scalar, neon, sizeof(scalar)));
  }

  // Finally a run of consecutive blocks in one call. The vector version keeps
  // the chaining state in registers across the run, so this is the only thing
  // that exercises the hand off from one block to the next.
  {
    const usize blocks_count = 7;
    u8 blocks[7 * SHA256_CBLOCK];
    for (usize i = 0; i < sizeof(blocks); i++) {
      x = x * 1664525u + 1013904223u;
      blocks[i] = (u8)(x >> 24);
    }

    u32 scalar[8] = {0};
    u32 neon[8] = {0};
    for (usize i = 0; i < 8; i++) {
      x = x * 1664525u + 1013904223u;
      scalar[i] = x;
      neon[i] = x;
    }

    for (usize i = 0; i < blocks_count; i++) {
      sha256_compress(scalar, blocks + i * SHA256_CBLOCK);
    }
    sha256_compress_blocks_neon(neon, blocks, blocks_count);
    assert(0 == memcmp(scalar, neon, sizeof(scalar)));

    // A zero length run must leave the state alone.
    sha256_compress_blocks_neon(neon, blocks, 0);
    assert(0 == memcmp(scalar, neon, sizeof(scalar)));
  }
#endif
}

// Every message length that changes how blocks are cut up: empty, short,
// exact multiples, and one byte either side of each. Hashed through the
// dispatcher, which is what callers actually reach.
static void test_sha256_neon_lengths(void) {
#if !SHA256_HAS_NEON
  return;
#else
  if (!sha256_neon_supported()) {
    return;
  }

  const usize max_len = 4 * SHA256_CBLOCK + 8;

  Arena arena = test_arena(64 * KiB);
  u8 *const data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), max_len);
  assert(data);

  u32 x = 0x12345678;
  for (usize i = 0; i < max_len; i++) {
    x = x * 1664525u + 1013904223u;
    data[i] = (u8)(x >> 24);
  }

  for (usize len = 0; len <= max_len; len++) {
    // The dispatcher picks the vector path here.
    u8 dispatched[SHA256_DIGEST_LENGTH] = {0};
    sha256_digest(bytes_make(data, len), dispatched);

    // The same message, forced through the scalar block function.
    Sha256Ctx ctx = {0};
    sha256_init(&ctx);

    usize offset = 0;
    while (len - offset >= SHA256_CBLOCK) {
      sha256_compress(ctx.h, data + offset);
      offset += SHA256_CBLOCK;
    }
    ctx.len = len;
    ctx.partial_len = (u32)(len - offset);
    if (ctx.partial_len > 0) {
      memcpy(ctx.partial, data + offset, ctx.partial_len);
    }

    u8 scalar[SHA256_DIGEST_LENGTH] = {0};
    sha256_final(&ctx, scalar);

    assert(0 == memcmp(dispatched, scalar, sizeof(scalar)));
  }
#endif
}

// The real platform `IO`. Unlike an `Env`, it is an object and not just a
// vtable -- it has a kqueue and a changelist -- so it is allocated, out of an
// arena the test owns, and it holds on to the `Env` that `env_platform_make`
// keeps for the process.
//
// `*arena` has to outlive the returned `IO`.
__attribute__((warn_unused_result)) static IO *test_io_real(Arena *arena) {
  assert(arena);

  IO *io = NULL;
  assert(
      ErrKindNone ==
      io_platform_make(arena, env_platform_make(), IoBackendDefault, &io).kind);
  assert(io);

  return io;
}

// Removing a scratch file the harness itself made. It goes to the real platform
// for the same reason `test_stdout_silence` does: the file is on the real
// filesystem whatever `io` the test under it happens to be driving.
__attribute__((warn_unused_result)) static Error test_remove_file(IO *io,
                                                                  Bytes path) {
  assert(io);

  IoOnce once = {0};
  io_once_init(&once);

  const Error err =
      io->remove_file(io, &once.completion, path, io_once_on_done);
  if (ErrKindNone != err.kind) {
    return err;
  }

  return io_once_wait(io, &once);
}

// One `open`, waited out, for the tests that are checking what `open` reports
// rather than doing anything with the descriptor. The descriptor is handed back
// through `*dst_fd` when there is one, and left alone when there is not.
__attribute__((warn_unused_result)) static Error
test_open(IO *io, Bytes path, FileOpenOptions opts, i32 *dst_fd) {
  assert(io);
  assert(dst_fd);

  IoOnce once = {0};
  io_once_init(&once);

  const Error err_submit =
      io->open(io, &once.completion, path, opts, io_once_on_done);
  if (ErrKindNone != err_submit.kind) {
    return err_submit;
  }

  const Error err = io_once_wait(io, &once);
  if (ErrKindNone == err.kind) {
    *dst_fd = (i32)once.res;
  }

  return err;
}

// A path under `TMPDIR` unique to this process, so a test run does not
// collide with a stale file or with another run.
__attribute__((warn_unused_result)) static Bytes
test_tmp_path(char *buf, usize buf_len, const char *name) {
  const Env *const env = env_platform_make();

  const char *const dir = getenv("TMPDIR");
  const i32 n = snprintf(buf, buf_len, "%s/file_send_test_%zu_%s",
                         dir ? dir : "/tmp", env->get_process_id(env), name);
  assert(n > 0);
  assert((usize)n < buf_len);

  return bytes_make((u8 *)buf, (usize)n);
}

// The failure paths of `open`, which are the ones a caller actually has to
// handle: they are reached through the vtable like any other caller would.
static void test_io_open_errors(void) {
  // Room for the platform `IO`, whose changelist is the bulk of it.
  Arena arena = test_arena(256 * KiB);
  IO *const io = test_io_real(&arena);
  i32 fd = -1;

  // An empty path is rejected before the syscall -- but not before the
  // operation is submitted: the check is in the syscall wrapper the loop runs,
  // so it reports through the callback like everything else.
  assert(ErrKindInvalidData ==
         test_open(io, bytes_make(NULL, 0), FileOpenOptionsReadOnly, &fd).kind);
  assert(ErrKindInvalidData ==
         test_open(io, bytes_from_cstr(""), FileOpenOptionsReadOnly, &fd).kind);

  // So is one too long for the fixed buffer, and the limit rides along in
  // `data` rather than an `errno` that was never set.
  {
    char long_path[5000] = {0};
    memset(long_path, 'a', sizeof(long_path) - 1);
    const Bytes path = bytes_make((u8 *)long_path, sizeof(long_path) - 1);

    const Error err = test_open(io, path, FileOpenOptionsReadOnly, &fd);
    assert(ErrKindRange == err.kind);
    assert(4095 == err.data);
  }

  // A missing file is the OS's complaint, carried verbatim.
  {
    char buf[256] = {0};
    const Bytes path = test_tmp_path(buf, sizeof(buf), "does_not_exist");
    (void)test_remove_file(io, path);

    const Error err = test_open(io, path, FileOpenOptionsReadOnly, &fd);
    // `ENOENT` has no kind of its own yet, so it lands in the catch-all;
    // `data` is what tells it apart.
    assert(ErrKindInvalidData == err.kind);
    assert(ENOENT == (i32)err.data);
  }
}

// `write_all_to_file` and `map_file` are only ever used as a pair, so they
// are checked as one: against a real file, because a fake filesystem would
// only be testing itself.
static void test_io_file_round_trip(void) {
  // Room for the platform `IO`, whose changelist is the bulk of it.
  Arena arena = test_arena(256 * KiB);
  IO *const io = test_io_real(&arena);

  char buf[256] = {0};
  const Bytes path = test_tmp_path(buf, sizeof(buf), "round_trip");
  (void)test_remove_file(io, path);

  // Bencode is binary, so a NUL in the middle must survive.
  const u8 payload[] = {'d', '3', ':', 'a', 'b', 'c', 0x00, 'e'};
  const Bytes data = bytes_make((u8 *)payload, sizeof(payload));

  assert(ErrKindNone == io_write_all_to_file_blocking(io, path, data).kind);

  {
    Bytes got = {0};
    assert(ErrKindNone ==
           io_map_file_blocking(io, path, FileOpenOptionsReadOnly, &got).kind);
    assert(data.len == got.len);
    assert(0 == memcmp(data.data, got.data, data.len));
  }

  // Rewriting with fewer bytes truncates: without `O_TRUNC` the old tail
  // would still be there, which for a bencode file is silent corruption.
  {
    const u8 shorter[] = {'i', '1', 'e'};
    const Bytes data_shorter = bytes_make((u8 *)shorter, sizeof(shorter));
    assert(ErrKindNone ==
           io_write_all_to_file_blocking(io, path, data_shorter).kind);

    Bytes got = {0};
    assert(ErrKindNone ==
           io_map_file_blocking(io, path, FileOpenOptionsReadOnly, &got).kind);
    assert(sizeof(shorter) == got.len);
    assert(0 == memcmp(shorter, got.data, sizeof(shorter)));
  }

  // Writing nothing is a no-op, not a truncation: the file is left as it was,
  // and nothing is ever submitted.
  {
    assert(ErrKindNone ==
           io_write_all_to_file_blocking(io, path, bytes_make(NULL, 0)).kind);

    Bytes got = {0};
    assert(ErrKindNone ==
           io_map_file_blocking(io, path, FileOpenOptionsReadOnly, &got).kind);
    assert(3 == got.len);
  }

  assert(ErrKindNone == test_remove_file(io, path).kind);

  // Mapping what is no longer there fails rather than handing back an empty
  // `Bytes`.
  {
    Bytes got = {0};
    assert(ErrKindNone !=
           io_map_file_blocking(io, path, FileOpenOptionsReadOnly, &got).kind);
  }

  // An empty file has nothing to map: `mmap` rejects a zero length, and that
  // is reported rather than handed back as an empty `Bytes`.
  {
    char empty_buf[256] = {0};
    const Bytes empty_path =
        test_tmp_path(empty_buf, sizeof(empty_buf), "empty");
    (void)test_remove_file(io, empty_path);

    i32 fd = -1;
    assert(ErrKindNone ==
           test_open(io, empty_path,
                     FileOpenOptionsWriteOnly | FileOpenOptionsCreate, &fd)
               .kind);
    assert(fd >= 0);
    {
      IoOnce once = {0};
      io_once_init(&once);
      assert(ErrKindNone ==
             io->close(io, &once.completion, fd, io_once_on_done).kind);
      assert(ErrKindNone == io_once_wait(io, &once).kind);
    }

    Bytes got = {0};
    assert(ErrKindNone !=
           io_map_file_blocking(io, empty_path, FileOpenOptionsReadOnly, &got)
               .kind);
    assert(bytes_is_empty(got));

    assert(ErrKindNone == test_remove_file(io, empty_path).kind);
  }

  // A directory opens but cannot be mapped, which walks the `mmap` failure
  // path with the descriptor already in hand.
  {
    Bytes got = {0};
    assert(ErrKindNone != io_map_file_blocking(io, bytes_from_cstr("/tmp"),
                                               FileOpenOptionsReadOnly, &got)
                              .kind);
  }

  // A path `open` rejects without setting `errno` still comes back as the
  // range error, not as whatever `errno` happened to hold.
  {
    char long_path[5000] = {0};
    memset(long_path, 'a', sizeof(long_path) - 1);
    const Bytes too_long = bytes_make((u8 *)long_path, sizeof(long_path) - 1);

    Bytes got = {0};
    assert(
        ErrKindRange ==
        io_map_file_blocking(io, too_long, FileOpenOptionsReadOnly, &got).kind);

    const u8 byte = 'x';
    assert(ErrKindRange == io_write_all_to_file_blocking(
                               io, too_long, bytes_make((u8 *)&byte, 1))
                               .kind);
  }
}

// Every allocation failure inside the torrent builder, walked by squeezing
// the arenas. The sizes are found by bisection rather than reasoned about:
// what matters is that each step reports rather than aborting, and that a
// caller never sees a half built torrent.
static void test_torrent_gen_torrent_file_data_oom(void) {
  const Bytes file_path = bytes_from_cstr("some_dir/payload.bin");
  const Bytes announce = bytes_from_cstr("http://localhost:12345");

  Arena data_arena = test_arena(64 * KiB);
  const usize data_len = 40 * KiB;
  u8 *const data_bytes = arena_alloc(&data_arena, 1, sizeof(u8), data_len);
  assert(data_bytes);
  for (usize i = 0; i < data_len; i++) {
    data_bytes[i] = (u8)(i * 31);
  }
  const Bytes file_data = bytes_make(data_bytes, data_len);

  // The whole thing succeeds when there is room, which is what makes the
  // failures below meaningful.
  usize needed_scratch = 0;
  {
    Arena arena = test_arena(64 * KiB);
    Arena scratch = test_arena(64 * KiB);
    const u8 *const scratch_start = scratch.start;
    Bytes torrent = {0};
    u8 info_hash[SHA256_DIGEST_LENGTH] = {0};

    assert(ErrKindNone ==
           torrent_gen_torrent_file_data(file_path, file_data, announce,
                                         &torrent, info_hash, scratch, &arena)
               .kind);
    assert(!bytes_is_empty(torrent));

    // The caller's scratch is passed by value, so its offset is untouched.
    assert(scratch_start == scratch.start);
    needed_scratch = 64 * KiB;
  }
  assert(needed_scratch > 0);

  // Starving the scratch arena a byte at a time walks every early return in
  // turn: the info dict, its encoding, the metainfo dict.
  usize reported_oom = 0;
  for (usize cap = 64; cap < 16 * KiB; cap *= 2) {
    Arena arena = test_arena(64 * KiB);
    Arena scratch = test_arena(cap);
    Bytes torrent = {.data = (u8 *)0xAA, .len = 1};
    u8 info_hash[SHA256_DIGEST_LENGTH] = {0};

    const Error err = torrent_gen_torrent_file_data(
        file_path, file_data, announce, &torrent, info_hash, scratch, &arena);
    if (ErrKindNone == err.kind) {
      continue;
    }

    reported_oom += 1;
    assert(ErrKindOOM == err.kind);
    // Nothing half built escapes: `dst` is only written on success.
    assert((u8 *)0xAA == torrent.data);
    assert(1 == torrent.len);
  }
  assert(reported_oom > 0);

  // The last allocation is the one that goes in the caller's arena, so
  // starving that one alone reaches the final return.
  {
    Arena arena = test_arena(64);
    Arena scratch = test_arena(64 * KiB);
    Bytes torrent = {0};
    u8 info_hash[SHA256_DIGEST_LENGTH] = {0};

    assert(ErrKindOOM ==
           torrent_gen_torrent_file_data(file_path, file_data, announce,
                                         &torrent, info_hash, scratch, &arena)
               .kind);
    assert(NULL == torrent.data);
  }
}

// Every kind renders as something, and no two share a spelling: a duplicated
// string means two enum members that cannot be told apart in a message, which
// is how `ErrKindConnectionReset` was found. The switch is `-Wswitch-enum`
// checked, so this is about the strings, not about reaching every arm.
static void test_error_kind_to_cstr(void) {
  const ErrorKind kinds[] = {
      ErrKindNone,
      ErrKindOOM,
      ErrKindInvalidData,
      ErrOSKindPermission,
      ErrKindRange,
      ErrKindAddrInUse,
      ErrKindAgain,
      ErrKindInterrupted,
      ErrKindConnReset,
      ErrKindTooManyFiles,
      ErrKindHostUnreachable,
      ErrKindUnsupported,
  };

  const IoBackend backends[] = {IoBackendDefault, IoBackendKqueue,
                                IoBackendEpoll, IoBackendIoUring,
                                IoBackendIocp};
  for (usize i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
    const char *const got = io_backend_to_cstr(backends[i]);
    assert(got);
    assert(strlen(got) > 0);

    for (usize j = 0; j < i; j++) {
      assert(0 != strcmp(got, io_backend_to_cstr(backends[j])));
    }
  }

  // Exactly one backend is the platform's, and every other one is refused
  // rather than quietly standing in for it. Which one is which is the
  // platform's business, so this only counts.
  {
    Arena arena = test_arena(256 * KiB);
    usize supported = 0;
    const IoBackend named[] = {IoBackendKqueue, IoBackendEpoll,
                               IoBackendIoUring, IoBackendIocp};

    for (usize i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
      IO *io = NULL;
      const Error err =
          io_platform_make(&arena, env_platform_make(), named[i], &io);
      if (ErrKindNone == err.kind) {
        assert(io);
        supported += 1;
      } else {
        assert(ErrKindUnsupported == err.kind);
      }
    }
    assert(1 == supported);
  }

  // `bytes_from_cstr` only ever runs on these in anger, so it rides along
  // here rather than earning a test of its own.
  assert(bytes_eq_cstr(bytes_from_cstr((char *)"torrent"), "torrent"));
  assert(bytes_is_empty(bytes_from_cstr((char *)"")));

  for (usize i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    const char *const got = error_kind_to_cstr(kinds[i]);
    assert(got);
    assert(strlen(got) > 0);

    for (usize j = 0; j < i; j++) {
      assert(0 != strcmp(got, error_kind_to_cstr(kinds[j])));
    }
  }
}

// The one-line failure paths of the syscall wrappers. No mock: a descriptor
// that was never open is a cheaper way to make the OS say no than faking it,
// and it exercises the real `errno` mapping rather than a fake's idea of it.
static void test_io_syscall_failures(void) {
  // Room for the platform `IO`, whose changelist is the bulk of it.
  Arena arena = test_arena(256 * KiB);
  IO *const io = test_io_real(&arena);
  const Env *const env = env_platform_make();

  // `fstat` on a descriptor that was never open. The operation is submitted and
  // fails when the loop runs it, which is the only way a caller ever sees this.
  {
    IoOnce once = {0};
    io_once_init(&once);
    once.res = 0xAA;

    assert(ErrKindNone ==
           io->file_size(io, &once.completion, -1, io_once_on_done).kind);
    const Error err = io_once_wait(io, &once);
    assert(ErrKindNone != err.kind);
    assert(EBADF == (i32)err.data);
    // Nothing is reported when there is nothing to report.
    assert(0 == once.res);
  }

  // `close` of the same.
  {
    IoOnce once = {0};
    io_once_init(&once);

    assert(ErrKindNone ==
           io->close(io, &once.completion, -1, io_once_on_done).kind);
    const Error err = io_once_wait(io, &once);
    assert(ErrKindNone != err.kind);
    assert(EBADF == (i32)err.data);
  }

  // `Env`'s hang-up reports the same thing, having nowhere to report it later.
  {
    const Error err = env->close_socket(env, -1);
    assert(ErrKindNone != err.kind);
    assert(EBADF == (i32)err.data);
  }

  // `mprotect` wants a page aligned address, so an odd one is rejected
  // without having to find an unmapped page first.
  {
    const Error err = env->vprotect_none(env, (void *)1, 4096);
    assert(ErrKindNone != err.kind);
  }

  // An empty `Bytes` contains nothing, without reading through a null pointer.
  assert(!bytes_contains_byte(bytes_make(NULL, 0), 'x'));

  // Mapping for writing takes the other protection branch. The file is still
  // opened read only and the mapping is private, so the bytes on disk are
  // safe either way.
  {
    char buf[256] = {0};
    const Bytes path = test_tmp_path(buf, sizeof(buf), "map_write");
    const u8 payload[] = {'a', 'b', 'c'};

    assert(ErrKindNone ==
           io_write_all_to_file_blocking(
               io, path, bytes_make((u8 *)payload, sizeof(payload)))
               .kind);

    Bytes got = {0};
    assert(ErrKindNone ==
           io_map_file_blocking(io, path, FileOpenOptionsWriteOnly, &got).kind);
    assert(sizeof(payload) == got.len);

    assert(ErrKindNone == test_remove_file(io, path).kind);
  }
}

// The allocation failures inside the two dictionary builders. Sweeping the
// arena a few bytes at a time walks each early return in turn; which size
// trips which is not the point, only that every one reports instead of
// aborting or half building something.
static void test_torrent_make_dicts_oom(void) {
  const Bytes name = bytes_from_cstr("payload.bin");
  const Bytes announce = bytes_from_cstr("http://localhost:12345");

  Arena data_arena = test_arena(2 * MiB);
  // More than one 256 KiB piece, so there is a piece layer to allocate and
  // not just a root: the layer is the largest allocation of the two.
  const usize data_len = 1 * MiB;
  u8 *const bytes = arena_alloc(&data_arena, 1, sizeof(u8), data_len);
  assert(bytes);
  for (usize i = 0; i < data_len; i++) {
    bytes[i] = (u8)(i * 31);
  }
  const Bytes file_data = bytes_make(bytes, data_len);

  // One run with room to spare, to build what the metainfo builder needs.
  Arena big = test_arena(1 * MiB);
  BencodeValue info_dict = {0};
  Bytes pieces_root = {0};
  PieceHash *piece_hashes = NULL;
  usize piece_hashes_count = 0;
  assert(ErrKindNone == torrent_make_info_dict_v2(name, TORRENT_BLOCK_SIZE * 16,
                                                  file_data, name, &info_dict,
                                                  &pieces_root, &piece_hashes,
                                                  &piece_hashes_count, &big)
                            .kind);
  assert(piece_hashes_count > 1);

  usize info_ooms = 0;
  for (usize cap = 64; cap < 6 * KiB; cap += 64) {
    Arena arena = test_arena(cap);
    BencodeValue dict = {0};
    Bytes root = {0};
    PieceHash *hashes = NULL;
    usize hashes_count = 0;

    const Error err = torrent_make_info_dict_v2(name, TORRENT_BLOCK_SIZE * 16,
                                                file_data, name, &dict, &root,
                                                &hashes, &hashes_count, &arena);
    if (ErrKindNone != err.kind) {
      assert(ErrKindOOM == err.kind);
      info_ooms += 1;
    }
  }
  assert(info_ooms > 0);

  usize metainfo_ooms = 0;
  usize metainfo_oks = 0;
  for (usize cap = 64; cap < 6 * KiB; cap += 64) {
    Arena arena = test_arena(cap);
    BencodeValue metainfo = {0};

    const Error err = torrent_make_metainfo_dict_v2(
        pieces_root, announce, info_dict.v.list, piece_hashes,
        piece_hashes_count, &metainfo, &arena);
    if (ErrKindNone != err.kind) {
      assert(ErrKindOOM == err.kind);
      metainfo_ooms += 1;
    } else {
      metainfo_oks += 1;
    }
  }
  // Both sides of the boundary, so the sweep is known to have crossed it.
  assert(metainfo_ooms > 0);
  assert(metainfo_oks > 0);
}

// ---------- The file composites ----------

// The real `io_map_file` and `io_write_all_to_file` run against faked
// primitives. That is the whole reason the composites are state machines over
// the slots -- the composites are the program's operations, the primitives
// underneath them are the platform's, and only the second kind is worth faking.
//
// Anything not faked goes to the real syscall, so `open` still yields a
// descriptor the rest of the code can use. A fake never invents one: it either
// fails or hands back a genuine one. The composites reach `mmap` through
// `io->env`, which is the real one throughout.
typedef struct {
  ErrorKind open_fails_with;
  ErrorKind file_size_fails_with;
  ErrorKind write_fails_with;

  // Report at most this many bytes per `write`, so the state machine has to go
  // around more than once. Zero means "as many as asked".
  usize write_chunk;
  // The call at this index (1-based, 0 for never) is interrupted, or reports
  // that it wrote nothing.
  usize write_interrupted_at;
  usize write_zero_at;

  usize open_calls;
  usize close_calls;
  usize write_calls;
  usize file_size_calls;
} TestFileCtx;

static TestIoPerformResult test_file_perform(TestIo *test_io,
                                             IoCompletion *completion, i32 fd,
                                             Error *dst_err, usize *dst_res) {
  TestFileCtx *const c = test_io->script;
  assert(c);
  assert(completion);

  switch (completion->action.kind) {
  case IoActionKindOpen: {
    c->open_calls += 1;
    if (ErrKindNone != c->open_fails_with) {
      *dst_err = (Error){.kind = c->open_fails_with};
      return TestIoPerformDone;
    }

    // `open` produces a descriptor rather than acting on one.
    assert(-1 == fd);
    i32 opened = -1;
    *dst_err = unix_open(completion->action.v.open.path,
                         completion->action.v.open.options, &opened);
    if (ErrKindNone == dst_err->kind) {
      assert(opened >= 0);
      *dst_res = (usize)(u32)opened;
    }
    return TestIoPerformDone;
  }

  case IoActionKindClose:
    c->close_calls += 1;
    *dst_err = unix_close(fd);
    return TestIoPerformDone;

  case IoActionKindFileSize:
    c->file_size_calls += 1;
    if (ErrKindNone != c->file_size_fails_with) {
      *dst_err = (Error){.kind = c->file_size_fails_with};
      return TestIoPerformDone;
    }
    *dst_err = unix_file_size(fd, dst_res);
    return TestIoPerformDone;

  case IoActionKindWrite: {
    c->write_calls += 1;

    if (c->write_calls == c->write_interrupted_at) {
      *dst_err = (Error){.kind = ErrKindInterrupted};
      return TestIoPerformDone;
    }
    if (ErrKindNone != c->write_fails_with) {
      *dst_err = (Error){.kind = c->write_fails_with};
      return TestIoPerformDone;
    }
    if (c->write_calls == c->write_zero_at) {
      *dst_res = 0;
      return TestIoPerformDone;
    }

    const Bytes data = completion->action.v.write.data;
    const usize chunk = (0 != c->write_chunk && c->write_chunk < data.len)
                            ? c->write_chunk
                            : data.len;
    *dst_err = unix_write(fd, bytes_take(data, chunk), dst_res);
    return TestIoPerformDone;
  }

  case IoActionKindNone:
  case IoActionKindRead:
  case IoActionKindAccept:
  case IoActionKindConnect:
  case IoActionKindSendTo:
  case IoActionKindRemoveFile:
    break;
  }

  assert(0 && "the file composites drive none of these");
  return TestIoPerformDone;
}

// The error paths inside the composites, which no real file can produce: an
// `open` that fails after the path was fine, an `fstat` that fails on a
// descriptor that just opened, a short write, an interrupted one.
static void test_io_composites_mocked(void) {
  Arena arena = test_arena(256 * KiB);
  IO *const real = test_io_real(&arena);
  const Env *const env = env_platform_make();

  char buf[256] = {0};
  const Bytes path = test_tmp_path(buf, sizeof(buf), "composites");
  const u8 payload[] = {'d', '3', ':', 'a', 'b', 'c', 0x00, 'e'};
  const Bytes data = bytes_make((u8 *)payload, sizeof(payload));

  // A failed `open` stops `map_file` before anything else is tried.
  {
    TestFileCtx ctx = {.open_fails_with = ErrKindTooManyFiles};
    TestIo test_io = {0};
    test_io_make(&test_io, env, test_file_perform, &ctx);
    Bytes got = {0};

    assert(ErrKindTooManyFiles == io_map_file_blocking(&test_io.io, path,
                                                       FileOpenOptionsReadOnly,
                                                       &got)
                                      .kind);
    assert(1 == ctx.open_calls);
    assert(0 == ctx.file_size_calls);
    // Nothing was opened, so nothing is closed.
    assert(0 == ctx.close_calls);
    assert(bytes_is_empty(got));
  }

  // The same for `write_all_to_file`.
  {
    TestFileCtx ctx = {.open_fails_with = ErrOSKindPermission};
    TestIo test_io = {0};
    test_io_make(&test_io, env, test_file_perform, &ctx);

    assert(ErrOSKindPermission ==
           io_write_all_to_file_blocking(&test_io.io, path, data).kind);
    assert(0 == ctx.write_calls);
    assert(0 == ctx.close_calls);
  }

  // Write the file for real, so there is something to map.
  {
    TestFileCtx ctx = {0};
    TestIo test_io = {0};
    test_io_make(&test_io, env, test_file_perform, &ctx);

    assert(ErrKindNone ==
           io_write_all_to_file_blocking(&test_io.io, path, data).kind);
    assert(1 == ctx.write_calls);
    assert(1 == ctx.close_calls);
  }

  // A failed `fstat` on a descriptor that just opened: unreachable with a
  // real file, and it is the path that has to hand the descriptor back.
  {
    TestFileCtx ctx = {.file_size_fails_with = ErrKindRange};
    TestIo test_io = {0};
    test_io_make(&test_io, env, test_file_perform, &ctx);
    Bytes got = {0};

    assert(ErrKindRange == io_map_file_blocking(&test_io.io, path,
                                                FileOpenOptionsReadOnly, &got)
                               .kind);
    assert(1 == ctx.file_size_calls);
    assert(1 == ctx.close_calls);
    assert(bytes_is_empty(got));
  }

  // A short write keeps its place and submits the rest, again and again until
  // everything has landed. One byte at a time is the extreme case of it.
  {
    TestFileCtx ctx = {.write_chunk = 1};
    TestIo test_io = {0};
    test_io_make(&test_io, env, test_file_perform, &ctx);

    assert(ErrKindNone ==
           io_write_all_to_file_blocking(&test_io.io, path, data).kind);
    assert(sizeof(payload) == ctx.write_calls);

    Bytes got = {0};
    assert(
        ErrKindNone ==
        io_map_file_blocking(real, path, FileOpenOptionsReadOnly, &got).kind);
    assert(data.len == got.len);
    assert(0 == memcmp(data.data, got.data, data.len));
  }

  // A signal before any progress is not a failure: the same slice is submitted
  // again and the same bytes go out.
  {
    TestFileCtx ctx = {.write_chunk = 2, .write_interrupted_at = 2};
    TestIo test_io = {0};
    test_io_make(&test_io, env, test_file_perform, &ctx);

    assert(ErrKindNone ==
           io_write_all_to_file_blocking(&test_io.io, path, data).kind);
    // Four chunks of two, plus the interrupted call that carried nothing.
    assert(5 == ctx.write_calls);

    Bytes got = {0};
    assert(
        ErrKindNone ==
        io_map_file_blocking(real, path, FileOpenOptionsReadOnly, &got).kind);
    assert(data.len == got.len);
    assert(0 == memcmp(data.data, got.data, data.len));
  }

  // A write that reports no progress and no error would go around forever, so
  // it is treated as the peer hanging up.
  {
    TestFileCtx ctx = {.write_zero_at = 1};
    TestIo test_io = {0};
    test_io_make(&test_io, env, test_file_perform, &ctx);

    assert(ErrKindConnReset ==
           io_write_all_to_file_blocking(&test_io.io, path, data).kind);
    assert(1 == ctx.write_calls);
    // The descriptor is handed back even on the way out.
    assert(1 == ctx.close_calls);
  }

  // A failed write reports, and still closes.
  {
    TestFileCtx ctx = {.write_fails_with = ErrKindConnReset};
    TestIo test_io = {0};
    test_io_make(&test_io, env, test_file_perform, &ctx);

    assert(ErrKindConnReset ==
           io_write_all_to_file_blocking(&test_io.io, path, data).kind);
    assert(1 == ctx.close_calls);
  }

  // The composites also have a path where a step is never submitted at all,
  // which is the one place they have to clean up without a callback.
  {
    TestFileCtx ctx = {0};
    TestIo test_io = {0};
    test_io_make(&test_io, env, test_file_perform, &ctx);
    test_io.submit_fails_for = IoActionKindFileSize;
    test_io.submit_fails_with = ErrKindAgain;
    Bytes got = {0};

    assert(ErrKindAgain == io_map_file_blocking(&test_io.io, path,
                                                FileOpenOptionsReadOnly, &got)
                               .kind);
    // The file was opened, so it is handed back even though nothing asked how
    // big it was.
    assert(1 == ctx.open_calls);
    assert(0 == ctx.file_size_calls);
    assert(1 == ctx.close_calls);
    assert(bytes_is_empty(got));
  }

  assert(ErrKindNone == test_remove_file(real, path).kind);
}

static void test(const char *filter) {
  const struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
      {"char_is_digit_ascii", test_char_is_digit_ascii},
      {"isize_from_usize", test_isize_from_usize},
      {"usize_round_up_multiple_of", test_usize_round_up_multiple_of},
      {"next_power_of_two", test_next_power_of_two},
      {"u8_write_u32_be", test_u8_write_u32_be},
      {"arena_alloc", test_arena_alloc},
#if defined(PLATFORM_UNIX)
      {"unix_error_from_errno", test_unix_error_from_errno},
#endif
      {"arena_valloc", test_arena_valloc},
      {"arena_valloc_mocked", test_arena_valloc_mocked},
      {"io_listen_and_serve_setup_failures",
       test_io_listen_and_serve_setup_failures},
      {"io_listen_and_serve_accept", test_io_listen_and_serve_accept},
      {"torrent_peer_pool_exhaustion", test_torrent_peer_pool_exhaustion},
      {"torrent_peer_to_cstr", test_torrent_peer_to_cstr},
      {"torrent_peer_parse_message", test_torrent_peer_parse_message},
      {"torrent_peer_run", test_torrent_peer_run},
      {"torrent_peer_deadlines", test_torrent_peer_deadlines},
      {"torrent_peer_state_machine", test_torrent_peer_state_machine},
      {"torrent_peer_messages", test_torrent_peer_messages},
      {"torrent_peer_recv_buf_compacted", test_torrent_peer_recv_buf_compacted},
      {"io_syscall_failures", test_io_syscall_failures},
      {"torrent_make_dicts_oom", test_torrent_make_dicts_oom},
      {"io_composites_mocked", test_io_composites_mocked},
      {"error_kind_to_cstr", test_error_kind_to_cstr},
      {"io_open_errors", test_io_open_errors},
      {"io_file_round_trip", test_io_file_round_trip},
      {"torrent_gen_torrent_file_data_oom",
       test_torrent_gen_torrent_file_data_oom},
      {"bytes", test_bytes},
      {"bytes_consume_u32_be", test_bytes_consume_u32_be},
      {"bytes_consume_u8", test_bytes_consume_u8},
      {"path_last_component", test_path_last_component},
      {"path_get_ext", test_path_get_ext},
      {"path_with_ext", test_path_with_ext},
      {"ascii_num_parse", test_ascii_num_parse},
      {"bencode_parse_num", test_bencode_parse_num},
      {"bencode_parse_bytes", test_bencode_parse_bytes},
      {"bencode_parse", test_bencode_parse},
      {"bencode_parse_binary", test_bencode_parse_binary},
      {"bencode_parse_dict_keys", test_bencode_parse_dict_keys},
      {"bencode_parse_deep_dicts", test_bencode_parse_deep_dicts},
      {"bytes_cmp", test_bytes_cmp},
      {"bytes_find", test_bytes_find},
      {"bytes_split", test_bytes_split},
      {"http_find_headers_end", test_http_find_headers_end},
      {"http_parse_headers", test_http_parse_headers},
      {"bencode_validate_dict", test_bencode_validate_dict},
      {"sha256_vectors", test_sha256_vectors},
      {"sha256_million_a", test_sha256_million_a},
      {"sha256_incremental", test_sha256_incremental},
      {"sha256_lengths", test_sha256_lengths},
      {"sha256_reuse", test_sha256_reuse},
      {"sha256_neon_matches_scalar", test_sha256_neon_matches_scalar},
      {"sha256_neon_lengths", test_sha256_neon_lengths},
      {"torrent_merkle_vectors", test_torrent_merkle_vectors},
      {"torrent_merkle_piece_layer", test_torrent_merkle_piece_layer},
      {"torrent_merkle_padding", test_torrent_merkle_padding},
      {"torrent_merkle_empty", test_torrent_merkle_empty},
      {"torrent_merkle_oom", test_torrent_merkle_oom},
      {"usize_digits_base_10", test_usize_digits_base_10},
      {"encode_usize_base_10", test_encode_usize_base_10},
      {"encode_usize_base_10_exact_fit", test_encode_usize_base_10_exact_fit},
      {"encode_usize_base_10_round_trip", test_encode_usize_base_10_round_trip},
      {"encode_isize_base_10", test_encode_isize_base_10},
      {"encode_isize_base_10_exact_fit", test_encode_isize_base_10_exact_fit},
      {"encode_isize_base_10_round_trip", test_encode_isize_base_10_round_trip},
      {"torrent_validate_info_dict", test_torrent_validate_info_dict},
      {"torrent_find_info_dict_in_metainfo",
       test_torrent_find_info_dict_in_metainfo},
      {"sha256_encode_hex_trunc", test_sha256_encode_hex_trunc},
      {"torrent_make_udp_broadcast_message",
       test_torrent_make_udp_broadcast_message},
      {"torrent_check_handshake", test_torrent_check_handshake},
      {"bytes_buffer_make", test_sb_make},
      {"bytes_buffer_extend_within_cap", test_sb_extend_within_cap},
      {"bytes_buffer_append_usize_within_cap", test_sb_append_usize_within_cap},
      {"bytes_buffer_build", test_sb_build},
      {"bencode_encode_leaves", test_bencode_encode_leaves},
      {"bencode_encode_non_ascii_bytes", test_bencode_encode_non_ascii_bytes},
      {"bencode_encode_containers", test_bencode_encode_containers},
      {"bencode_encode_wide_dict", test_bencode_encode_wide_dict},
      {"bencode_encode_round_trip", test_bencode_encode_round_trip},
      {"bencode_encode_torrent_info", test_bencode_encode_torrent_info},
      {"torrent_metainfo_v2", test_torrent_metainfo_v2},
  };

  usize run = 0;
  for (usize i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (filter && 0 != strcmp(filter, tests[i].name)) {
      continue;
    }

    printf("test %s\n", tests[i].name);
    tests[i].fn();
    run++;
  }

  printf("%zu test(s) passed\n", run);
}

#endif
