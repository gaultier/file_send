#pragma once

// The whole test suite, plus the mock `IO` implementations it runs the real
// code against. Included by `main.c` last, so a test can reach anything the
// program defines.

// Arena memory comes straight from `mmap` and is therefore zeroed, which
// makes a read of never-written memory look like a perfectly valid zeroed
// struct. Poison it so such a read shows up as an obviously bogus value
// instead.
__attribute__((warn_unused_result)) static Arena test_arena(usize bytes_count) {
  const IO io = io_platform_make();
  Arena arena = {0};
  assert(ErrKindNone == arena_valloc(&io, bytes_count, &arena).kind);
  assert(arena.start);
  assert(arena.end);
  assert((usize)arena.end - (usize)arena.start >= bytes_count);

  memset(arena.start, 0xAA, bytes_count);
  return arena;
}

__attribute__((warn_unused_result)) static Slice_u8
test_slice(const char *input) {
  assert(input);

  return slice_u8_make((u8 *)input, strlen(input));
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

// Every `errno` the syscalls this program makes are documented to set, and
// what each one is supposed to come back as. A value landing in the
// `ErrInvalidData` default by accident rather than on purpose is exactly the
// kind of thing that goes unnoticed, so list them explicitly.
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

// A fake `IO` for the arena. It records what `arena_valloc` asked the OS for,
// answers with a page size the host does not have, and can make the mapping
// fail on demand: the real `mmap` only fails for sizes so large that the
// request says nothing about *which* error comes back.
//
// Anything it does not fake is delegated to `real` rather than reimplemented,
// so the arena still hands back memory that can be written to.
typedef struct {
  IO real;
  usize page_size;

  // When set, `valloc` fails with this instead of mapping.
  ErrorKind alloc_fails_with;

  // What the arena actually asked for.
  usize alloc_calls;
  usize alloc_bytes_count;
  usize protect_calls;
  void *protect_ptr;
  usize protect_size;
} TestIoCtx;

__attribute__((warn_unused_result)) static usize
test_io_get_page_size(const IO *io) {
  TestIoCtx *const c = io->ctx;
  assert(c);

  return c->page_size;
}

__attribute__((warn_unused_result)) static Error
test_io_valloc(const IO *io, usize bytes_count, u8 **res) {
  TestIoCtx *const c = io->ctx;
  assert(c);

  c->alloc_calls += 1;
  c->alloc_bytes_count = bytes_count;

  if (ErrKindNone != c->alloc_fails_with) {
    return (Error){.kind = c->alloc_fails_with};
  }

  return c->real.valloc(&c->real, bytes_count, res);
}

__attribute__((warn_unused_result)) static Error
test_io_vprotect_none(const IO *io, void *ptr, usize size) {
  TestIoCtx *const c = io->ctx;
  assert(c);

  c->protect_calls += 1;
  c->protect_ptr = ptr;
  c->protect_size = size;

  return c->real.vprotect_none(&c->real, ptr, size);
}

// Only the slots `arena_valloc` reaches for; the rest stay null so that a
// call to any of them crashes rather than silently doing something real.
__attribute__((warn_unused_result)) static IO test_io_platform_make(TestIoCtx *ctx) {
  assert(ctx);

  return (IO){
      .get_page_size = test_io_get_page_size,
      .valloc = test_io_valloc,
      .vprotect_none = test_io_vprotect_none,
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
    TestIoCtx ctx = {.real = io_platform_make(), .page_size = page_size};
    const IO io = test_io_platform_make(&ctx);
    Arena arena = {0};

    assert(ErrKindNone == arena_valloc(&io, 1, &arena).kind);

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
    TestIoCtx ctx = {.real = io_platform_make(), .page_size = page_size};
    const IO io = test_io_platform_make(&ctx);
    Arena arena = {0};

    assert(ErrKindNone == arena_valloc(&io, page_size + 1, &arena).kind);
    assert(3 * page_size == ctx.alloc_bytes_count);
    assert((void *)arena.end == ctx.protect_ptr);
  }

  // An exact multiple is not rounded up past itself.
  {
    TestIoCtx ctx = {.real = io_platform_make(), .page_size = page_size};
    const IO io = test_io_platform_make(&ctx);
    Arena arena = {0};

    assert(ErrKindNone == arena_valloc(&io, 2 * page_size, &arena).kind);
    assert(3 * page_size == ctx.alloc_bytes_count);
  }

  // A failed mapping is reported, not asserted, and leaves the caller's arena
  // untouched. Nothing is protected either: there is no mapping to protect.
  {
    TestIoCtx ctx = {.real = io_platform_make(),
                     .page_size = page_size,
                     .alloc_fails_with = ErrKindOOM};
    const IO io = test_io_platform_make(&ctx);
    Arena arena = {.start = (u8 *)0xAA, .end = (u8 *)0xBB};

    assert(ErrKindOOM == arena_valloc(&io, 1, &arena).kind);
    assert(1 == ctx.alloc_calls);
    assert(0 == ctx.protect_calls);
    assert((u8 *)0xAA == arena.start);
    assert((u8 *)0xBB == arena.end);
  }

  // The reason is passed through rather than flattened into `ErrOOM`: the
  // real `mmap` can only be provoked into `ENOMEM`, so this is the only way
  // to check that the error travels verbatim.
  {
    TestIoCtx ctx = {.real = io_platform_make(),
                     .page_size = page_size,
                     .alloc_fails_with = ErrOSKindPermission};
    const IO io = test_io_platform_make(&ctx);
    Arena arena = {0};

    assert(ErrOSKindPermission == arena_valloc(&io, 1, &arena).kind);
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
  const IO io = io_platform_make();

  i32 saved = -1;
  assert(ErrKindNone == io.stdout_silence(&io, &saved).kind);

  return saved;
}

static void test_stdout_restore(i32 saved) {
  const IO io = io_platform_make();

  assert(ErrKindNone == io.stdout_restore(&io, saved).kind);
}

// A scripted `IO` for the TCP server. Every slot it fakes can be made to fail,
// and `accept` is told in advance how many connections to hand over before it
// stops: `io_listen_and_serve_tcp_ipv4` loops forever otherwise, so without
// this there is no way to call it from a test at all.
typedef struct {
  // Failure injection, one per slot. `ErrKindNone` means "succeed".
  ErrorKind socket_fails_with;
  ErrorKind reuse_fails_with;
  ErrorKind bind_fails_with;
  ErrorKind listen_fails_with;
  ErrorKind thread_create_fails_with;
  ErrorKind read_fails_with;

  // `accept` succeeds this many times, then reports `accept_ends_with` to
  // unwind the loop. The call at `conn_reset_at` (1-based, 0 for never)
  // reports a reset instead, which the server is meant to shrug off.
  usize accept_success_max;
  usize conn_reset_at;
  ErrorKind accept_ends_with;
  usize accept_handed_over;

  // Run the handler on this thread instead of spawning one, so it is covered
  // without the test having to join anything.
  bool run_thread_inline;

  usize socket_calls;
  usize reuse_calls;
  usize bind_calls;
  usize listen_calls;
  usize accept_calls;
  usize close_calls;
  usize thread_create_calls;
  usize read_calls;

  Ipv4Addr bound_addr;
  i32 backlog;
} TestServerCtx;

__attribute__((warn_unused_result)) static Error
test_server_socket(const IO *io, SocketDomain domain, SocketType type,
                   i32 *fd) {
  TestServerCtx *const c = io->ctx;
  assert(c);
  assert(fd);
  assert(SocketDomainIpv4 == domain);
  assert(SocketTypeTcp == type);

  c->socket_calls += 1;
  if (ErrKindNone != c->socket_fails_with) {
    return (Error){.kind = c->socket_fails_with};
  }

  // A recognisable descriptor: a real one would never be this.
  *fd = 4242;
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
test_server_enable_socket_reuse(const IO *io, i32 fd) {
  TestServerCtx *const c = io->ctx;
  assert(c);
  assert(4242 == fd);

  c->reuse_calls += 1;
  return (Error){.kind = c->reuse_fails_with};
}

__attribute__((warn_unused_result)) static Error
test_server_tcp_bind_ipv4(const IO *io, i32 fd, Ipv4Addr addr) {
  TestServerCtx *const c = io->ctx;
  assert(c);
  assert(4242 == fd);

  c->bind_calls += 1;
  c->bound_addr = addr;
  return (Error){.kind = c->bind_fails_with};
}

__attribute__((warn_unused_result)) static Error
test_server_listen(const IO *io, i32 fd, i32 backlog) {
  TestServerCtx *const c = io->ctx;
  assert(c);
  assert(4242 == fd);

  c->listen_calls += 1;
  c->backlog = backlog;
  return (Error){.kind = c->listen_fails_with};
}

__attribute__((warn_unused_result)) static Error
test_server_accept(const IO *io, i32 listen_socket, i32 *dst_accept_socket,
                   Ipv4Addr *dst_accept_addr) {
  TestServerCtx *const c = io->ctx;
  assert(c);
  assert(4242 == listen_socket);
  assert(dst_accept_socket);
  assert(dst_accept_addr);

  c->accept_calls += 1;

  if (c->accept_calls == c->conn_reset_at) {
    return (Error){.kind = ErrKindConnReset};
  }

  // A reset hands over nothing, so it does not count against the budget.
  if (c->accept_handed_over >= c->accept_success_max) {
    return (Error){.kind = c->accept_ends_with};
  }
  c->accept_handed_over += 1;

  *dst_accept_socket = (i32)(5000 + c->accept_calls);
  *dst_accept_addr = (Ipv4Addr){.ip = 0x7f000001, .port = 4000};
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error test_server_close(const IO *io,
                                                                   i32 fd) {
  TestServerCtx *const c = io->ctx;
  assert(c);
  assert(fd > 0);

  c->close_calls += 1;
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
test_server_thread_create(const IO *io, ThreadCallback cb, void *data) {
  TestServerCtx *const c = io->ctx;
  assert(c);
  assert(cb);
  assert(data);

  c->thread_create_calls += 1;
  if (ErrKindNone != c->thread_create_fails_with) {
    return (Error){.kind = c->thread_create_fails_with};
  }

  if (c->run_thread_inline) {
    (void)cb(data);
  }
  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
test_server_read(const IO *io, i32 fd, Slice_u8 data, usize *dst_read) {
  TestServerCtx *const c = io->ctx;
  assert(c);
  assert(fd > 0);
  assert(dst_read);

  c->read_calls += 1;
  if (ErrKindNone != c->read_fails_with) {
    return (Error){.kind = c->read_fails_with};
  }

  const u8 msg[] = "hello";
  assert(data.len >= sizeof(msg) - 1);
  memcpy(data.data, msg, sizeof(msg) - 1);
  *dst_read = sizeof(msg) - 1;

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static IO
test_io_server_make(TestServerCtx *ctx) {
  assert(ctx);

  return (IO){
      .socket = test_server_socket,
      .enable_socket_reuse = test_server_enable_socket_reuse,
      .tcp_bind_ipv4 = test_server_tcp_bind_ipv4,
      .listen = test_server_listen,
      .accept = test_server_accept,
      .close = test_server_close,
      .thread_create = test_server_thread_create,
      .read = test_server_read,
      .ctx = ctx,
  };
}

// Setting up the listener: each step's failure stops the sequence and travels
// back verbatim, and every failure that happens after the socket exists hands
// the descriptor back.
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
    const IO io = test_io_server_make(&ctx);
    TorrentNetworkCtx network_ctx = {0};

    const i32 saved = test_stdout_silence();
    const Error err = io_listen_and_serve_tcp_ipv4(&io, &network_ctx, addr,
                                                   torrent_client_on_accept);
    test_stdout_restore(saved);

    assert(cases[i].expected == err.kind);
    assert(cases[i].expected_closes == ctx.close_calls);
    // Nothing past the failing step ran.
    assert(0 == ctx.accept_calls);
  }

  // The listener is bound to what it was asked for, with a backlog.
  {
    TestServerCtx ctx = {.accept_ends_with = ErrKindInvalidData};
    const IO io = test_io_server_make(&ctx);
    TorrentNetworkCtx network_ctx = {0};

    const i32 saved = test_stdout_silence();
    assert(ErrKindInvalidData ==
           io_listen_and_serve_tcp_ipv4(&io, &network_ctx, addr,
                                        torrent_client_on_accept)
               .kind);
    test_stdout_restore(saved);

    assert(addr.ip == ctx.bound_addr.ip);
    assert(addr.port == ctx.bound_addr.port);
    assert(ctx.backlog > 0);
    // The listener is closed on the way out.
    assert(1 == ctx.close_calls);
  }
}

// The accept loop and the handler behind it.
static void test_io_listen_and_serve_accept(void) {
  const Ipv4Addr addr = {.ip = 0x7f000001, .port = 12345};

  // A peer that vanishes between the handshake and the `accept` is one dead
  // connection, not a dead server: the loop goes back around. Nothing but a
  // fake can produce that at a chosen moment.
  {
    TestServerCtx ctx = {.accept_success_max = 2,
                         .conn_reset_at = 1,
                         .accept_ends_with = ErrKindInvalidData};
    const IO io = test_io_server_make(&ctx);
    TorrentNetworkCtx network_ctx = {0};

    const i32 saved = test_stdout_silence();
    assert(ErrKindInvalidData ==
           io_listen_and_serve_tcp_ipv4(&io, &network_ctx, addr,
                                        torrent_client_on_accept)
               .kind);
    test_stdout_restore(saved);

    // The reset was shrugged off, so both connections still arrived.
    assert(2 == ctx.thread_create_calls);
    assert(4 == ctx.accept_calls);
  }

  // A handler that cannot be started releases its slot and hangs up, rather
  // than leaking either.
  {
    TestServerCtx ctx = {.accept_success_max = 3,
                         .thread_create_fails_with = ErrKindOOM,
                         .accept_ends_with = ErrKindInvalidData};
    const IO io = test_io_server_make(&ctx);
    TorrentNetworkCtx network_ctx = {0};

    const i32 saved = test_stdout_silence();
    assert(ErrKindInvalidData ==
           io_listen_and_serve_tcp_ipv4(&io, &network_ctx, addr,
                                        torrent_client_on_accept)
               .kind);
    test_stdout_restore(saved);

    assert(3 == ctx.thread_create_calls);
    // One per accepted connection, plus the listener itself.
    assert(4 == ctx.close_calls);

    // Every slot was handed back, so the pool is as empty as it started.
    for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
      assert(0 == network_ctx.pool.occupied[i]);
    }
  }

  // Running the handler inline covers it without a second thread: it reads
  // once and hangs up.
  {
    TestServerCtx ctx = {.accept_success_max = 1,
                         .run_thread_inline = true,
                         .accept_ends_with = ErrKindInvalidData};
    const IO io = test_io_server_make(&ctx);
    TorrentNetworkCtx network_ctx = {0};

    const i32 saved = test_stdout_silence();
    assert(ErrKindInvalidData ==
           io_listen_and_serve_tcp_ipv4(&io, &network_ctx, addr,
                                        torrent_client_on_accept)
               .kind);
    test_stdout_restore(saved);

    assert(1 == ctx.read_calls);
    assert(2 == ctx.close_calls);
    for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
      assert(0 == network_ctx.pool.occupied[i]);
    }
  }

  // A read that fails still hangs up and still frees the slot.
  {
    TestServerCtx ctx = {.accept_success_max = 1,
                         .run_thread_inline = true,
                         .read_fails_with = ErrKindConnReset,
                         .accept_ends_with = ErrKindInvalidData};
    const IO io = test_io_server_make(&ctx);
    TorrentNetworkCtx network_ctx = {0};

    const i32 saved = test_stdout_silence();
    assert(ErrKindInvalidData ==
           io_listen_and_serve_tcp_ipv4(&io, &network_ctx, addr,
                                        torrent_client_on_accept)
               .kind);
    test_stdout_restore(saved);

    assert(1 == ctx.read_calls);
    assert(2 == ctx.close_calls);
    for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
      assert(0 == network_ctx.pool.occupied[i]);
    }
  }
}

// Backpressure: one more connection than the pool holds. Reaching this with
// real sockets would mean opening `TORRENT_CLIENTS_MAX` of them.
static void test_torrent_client_pool_exhaustion(void) {
  const Ipv4Addr addr = {.ip = 0x7f000001, .port = 12345};

  // The pool is a megabyte or so of slots: too much for the stack.
  static TorrentNetworkCtx network_ctx;
  memset(&network_ctx, 0, sizeof(network_ctx));

  TestServerCtx ctx = {.accept_success_max = TORRENT_CLIENTS_MAX + 1,
                       .accept_ends_with = ErrKindInvalidData};
  const IO io = test_io_server_make(&ctx);

  const i32 saved = test_stdout_silence();
  assert(ErrKindInvalidData ==
         io_listen_and_serve_tcp_ipv4(&io, &network_ctx, addr,
                                      torrent_client_on_accept)
             .kind);
  test_stdout_restore(saved);

  // Nothing ever finishes, so the pool fills and the last connection is
  // refused rather than overrunning the slots.
  // `TORRENT_CLIENTS_MAX + 1` connections were handed over, one more than
  // the pool holds, plus the call that ends the loop.
  assert(TORRENT_CLIENTS_MAX + 2 == ctx.accept_calls);
  assert(TORRENT_CLIENTS_MAX + 1 == ctx.accept_handed_over);
  // The last one never reached `thread_create`: the pool refused it first.
  assert(TORRENT_CLIENTS_MAX == ctx.thread_create_calls);

  // Every group is full.
  for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
    assert(~(PoolSlotGroup)0 == network_ctx.pool.occupied[i]);
  }

  // The refused connection was hung up on, and so was the listener.
  assert(2 == ctx.close_calls);
}

static void test_arena_valloc(void) {
  const IO io = io_platform_make();

  // A request the kernel cannot satisfy. `mmap` reports `MAP_FAILED`, not
  // NULL, so this also pins down that conversion, and that the `ENOMEM` it
  // sets comes back as `ErrOOM` rather than a bare failure.
  Arena arena = {0};
  assert(ErrKindOOM == arena_valloc(&io, (usize)1 << 62, &arena).kind);

  // A failed call leaves the caller's arena alone.
  assert(NULL == arena.start);
  assert(NULL == arena.end);
}

static void test_slice_u8(void) {
  u8 data[] = {'a', 'b', 'c'};

  // slice_u8_first.
  {
    u8 first = 0xAA;
    assert(ErrKindNone != slice_u8_first(slice_u8_make(NULL, 0), &first).kind);
    assert(ErrKindNone != slice_u8_first(slice_u8_make(data, 0), &first).kind);
    // Nothing is written when there is no first byte.
    assert(0xAA == first);

    assert(ErrKindNone ==
           slice_u8_first(slice_u8_make(data, sizeof(data)), &first).kind);
    assert('a' == first);

    // Peeking does not consume.
    Slice_u8 slice = slice_u8_make(data, sizeof(data));
    assert(ErrKindNone == slice_u8_first(slice, &first).kind);
    assert(sizeof(data) == slice.len);
  }
  // slice_u8_skip.
  {
    Slice_u8 empty = slice_u8_make(NULL, 0);
    assert(ErrKindNone != slice_u8_skip(&empty, 1).kind);

    Slice_u8 slice = slice_u8_make(data, sizeof(data));

    // Past the end: refused, and the slice is unchanged.
    assert(ErrKindNone != slice_u8_skip(&slice, sizeof(data) + 1).kind);
    assert(sizeof(data) == slice.len);
    assert(data == slice.data);

    assert(ErrKindNone == slice_u8_skip(&slice, 0).kind);
    assert(sizeof(data) == slice.len);

    assert(ErrKindNone == slice_u8_skip(&slice, 2).kind);
    assert(1 == slice.len);

    u8 first = 0;
    assert(ErrKindNone == slice_u8_first(slice, &first).kind);
    assert('c' == first);

    // Skipping exactly to the end is legal.
    assert(ErrKindNone == slice_u8_skip(&slice, 1).kind);
    assert(0 == slice.len);
    assert(ErrKindNone != slice_u8_first(slice, &first).kind);
  }
  // slice_u8_take.
  {
    const Slice_u8 slice = slice_u8_make(data, sizeof(data));

    assert(0 == slice_u8_take(slice, 0).len);
    assert(sizeof(data) == slice_u8_take(slice, sizeof(data)).len);

    const Slice_u8 taken = slice_u8_take(slice, 2);
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
    const Slice_u8 got =
        path_last_component(test_slice(cases[i].input), PATH_SEPARATOR_UNIX);

    assert(!slice_u8_is_empty(got));
    assert(slice_u8_eq_cstr(got, cases[i].expected));
  }

  // A null slice is the same as an empty one.
  assert(slice_u8_eq_cstr(
      path_last_component(slice_u8_make(NULL, 0), PATH_SEPARATOR_UNIX), "."));

  // The result borrows from the input: no copy, and it is a suffix of the
  // path (after the trailing separators are removed).
  {
    const Slice_u8 path = test_slice("/a/b/c.zip");
    const Slice_u8 got = path_last_component(path, PATH_SEPARATOR_UNIX);
    assert(path.data + path.len - got.len == got.data);
  }
  // The input is passed by value, so the caller's slice is untouched even
  // though the function trims trailing separators.
  {
    Slice_u8 path = test_slice("/a/b/");
    const Slice_u8 got = path_last_component(path, PATH_SEPARATOR_UNIX);
    assert(slice_u8_eq_cstr(got, "b"));
    assert(5 == path.len);
  }
}

static void test_path_get_ext(void) {
  // Expectations generated with Go's `filepath.Ext`. A `NULL` expectation
  // means "no extension": `slice_u8_eq_cstr` reads it as "empty", and an
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
    const Slice_u8 path = test_slice(cases[i].input);
    const Slice_u8 got = path_get_ext(path, PATH_SEPARATOR_UNIX);

    assert(slice_u8_eq_cstr(got, cases[i].expected));

    // Nothing is copied: a non-empty result is always a suffix of the input.
    if (!slice_u8_is_empty(got)) {
      assert(got.data >= path.data);
      assert(got.data + got.len == path.data + path.len);
      assert('.' == got.data[0]);
    }
  }

  // A null slice is the same as an empty one.
  assert(slice_u8_is_empty(
      path_get_ext(slice_u8_make(NULL, 0), PATH_SEPARATOR_UNIX)));

  // The input is passed by value, so the caller's slice is untouched.
  {
    Slice_u8 path = test_slice("/a/b/c.zip");
    const Slice_u8 got = path_get_ext(path, PATH_SEPARATOR_UNIX);
    assert(slice_u8_eq_cstr(got, ".zip"));
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
    Slice_u8 got = {0};

    assert(ErrKindNone == path_with_ext(test_slice(cases[i].input),
                                        test_slice(cases[i].ext),
                                        PATH_SEPARATOR_UNIX, &got, &arena)
                              .kind);
    assert(slice_u8_eq_cstr(got, cases[i].expected));

    // The result is a fresh copy, never a view into the input.
    assert(got.data != test_slice(cases[i].input).data);
  }

  // An empty path has no name to extend.
  {
    Arena arena = test_arena(256);
    Slice_u8 got = {0};
    assert(ErrKindInvalidData ==
           path_with_ext(test_slice(""), test_slice("torrent"),
                         PATH_SEPARATOR_UNIX, &got, &arena)
               .kind);
    assert(slice_u8_is_empty(got));
  }

  // A null slice is the same as an empty one.
  {
    Arena arena = test_arena(256);
    Slice_u8 got = {0};
    assert(ErrKindInvalidData ==
           path_with_ext(slice_u8_make(NULL, 0), test_slice("torrent"),
                         PATH_SEPARATOR_UNIX, &got, &arena)
               .kind);
  }

  // A trailing separator leaves no last component.
  {
    const char *const inputs[] = {"/", "//", "a/", "/a/b/", "a/b///"};

    for (usize i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
      Arena arena = test_arena(256);
      Slice_u8 got = {0};
      assert(ErrKindInvalidData ==
             path_with_ext(test_slice(inputs[i]), test_slice("torrent"),
                           PATH_SEPARATOR_UNIX, &got, &arena)
                 .kind);
    }
  }

  // Out of memory is reported rather than asserted, and leaves `dst` alone.
  {
    u8 mem[8] = {0};
    Arena arena = arena_from_mem(mem, sizeof(mem));
    Slice_u8 got = {0};

    // "a.torrent" is nine bytes, one more than the arena holds.
    assert(ErrKindOOM == path_with_ext(test_slice("a.pdf"),
                                       test_slice("torrent"),
                                       PATH_SEPARATOR_UNIX, &got, &arena)
                             .kind);
    assert(slice_u8_is_empty(got));
  }

  // The input is passed by value and only read: the caller's slice and the
  // bytes behind it are untouched.
  {
    Arena arena = test_arena(256);
    char input[] = "/a/b/c.zip";
    const Slice_u8 path = slice_u8_make((u8 *)input, sizeof(input) - 1);
    Slice_u8 got = {0};

    assert(ErrKindNone == path_with_ext(path, test_slice("torrent"),
                                        PATH_SEPARATOR_UNIX, &got, &arena)
                              .kind);
    assert(slice_u8_eq_cstr(got, "/a/b/c.torrent"));
    assert(slice_u8_eq_cstr(path, "/a/b/c.zip"));
    assert(0 == strcmp(input, "/a/b/c.zip"));
  }

  // The result is exactly as long as it claims: the arena bump matches the
  // reported length, so nothing is written past the end.
  {
    Arena arena = test_arena(256);
    const u8 *const before = arena.start;
    Slice_u8 got = {0};

    assert(ErrKindNone == path_with_ext(test_slice("a.pdf"),
                                        test_slice("torrent"),
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
    Slice_u8 data = slice_u8_make((u8 *)cases[i].input, strlen(cases[i].input));
    const Slice_u8 before = data;

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
    Slice_u8 data = slice_u8_make(NULL, 0);
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
    Slice_u8 data = test_slice(cases[i].input);

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

static void test_bencode_parse_string(void) {
  const struct {
    const char *input;
    bool ok;
    const char *str;
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
    Slice_u8 data = test_slice(cases[i].input);

    // Poisoned so that a write on the failure path is visible.
    BencodeValue value = {.kind = BencodeKindDict};
    assert((ErrKindNone == bencode_parse_string(&data, &value).kind) ==
           cases[i].ok);

    if (!cases[i].ok) {
      // A failed parse consumes nothing and writes nothing.
      assert(strlen(cases[i].input) == data.len);
      assert(BencodeKindDict == value.kind);
      continue;
    }

    assert(BencodeKindString == value.kind);
    assert(strlen(cases[i].str) == value.v.s.len);
    assert(0 == memcmp(value.v.s.data, cases[i].str, value.v.s.len));
    assert(cases[i].remaining == data.len);
  }

  // The string points into the input, it is not copied.
  {
    const char *const input = "4:spam";
    Slice_u8 data = test_slice(input);

    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse_string(&data, &value).kind);
    assert((u8 *)input + 2 == value.v.s.data);
  }
}

__attribute__((warn_unused_result)) static bool
test_bencode_is_string(BencodeValue value, const char *expected) {
  assert(expected);

  const usize len = strlen(expected);
  return BencodeKindString == value.kind && len == value.v.s.len &&
         (0 == len || 0 == memcmp(value.v.s.data, expected, len));
}

__attribute__((warn_unused_result)) static BencodeValue
test_bencode_make_string(const char *data, usize len) {
  if (0 != len) {
    assert(data);
  }

  return (BencodeValue){.kind = BencodeKindString,
                        .v.s = slice_u8_make((u8 *)data, len)};
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
      {"3:abc", true, BencodeKindString, 0, 0},
      {"0:", true, BencodeKindString, 0, 0},
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
      // Dict keys must be strings, sorted by raw byte value, with no
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

    Slice_u8 data = test_slice(cases[i].input);
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

  // Every digit dispatches to the string parser.
  {
    for (u8 c = '0'; c <= '9'; c++) {
      Arena arena = test_arena(1 * KiB);
      Arena scratch = test_arena(1 * KiB);

      const char input[] = {(char)c, ':', 0};
      Slice_u8 data = test_slice(input);

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

    Slice_u8 data = test_slice("ld1:a1:beli2eee");
    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind);
    assert(BencodeKindList == value.kind);
    assert(2 == value.v.list.len);

    const BencodeValue dict = value.v.list.data[0];
    assert(BencodeKindDict == dict.kind);
    assert(2 == dict.v.list.len);
    assert(test_bencode_is_string(dict.v.list.data[0], "a"));
    assert(test_bencode_is_string(dict.v.list.data[1], "b"));

    const BencodeValue list = value.v.list.data[1];
    assert(BencodeKindList == list.kind);
    assert(1 == list.v.list.len);
    assert(BencodeKindInteger == list.v.list.data[0].kind);
    assert(2 == list.v.list.data[0].v.num);
  }

  // Strings point into the input, they are not copied.
  {
    Arena arena = test_arena(1 * KiB);
    Arena scratch = test_arena(1 * KiB);

    const char *const input = "l4:spame";
    Slice_u8 data = test_slice(input);
    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind);
    assert((u8 *)input + 3 == value.v.list.data[0].v.s.data);
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

      Slice_u8 data = slice_u8_make(input, 2 * depth);
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

    Slice_u8 data = slice_u8_make(input, sizeof(input));
    BencodeValue value = {0};
    assert(ErrKindNone == bencode_parse(&data, &arena, scratch, &value).kind);
    assert(BencodeKindList == value.kind);
    assert(children_len == value.v.list.len);

    for (usize i = 0; i < children_len; i++) {
      assert(test_bencode_is_string(value.v.list.data[i], ""));
    }
  }

  // Out of scratch: the scratch arena cannot even hold the `values` array.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(8);

    Slice_u8 data = test_slice("li1ei2ee");
    BencodeValue value = {0};
    assert(ErrKindNone != bencode_parse(&data, &arena, scratch, &value).kind);
  }
  // Out of output arena: the children of a container do not fit.
  {
    Arena arena = test_arena(8);
    Arena scratch = test_arena(4 * KiB);

    Slice_u8 data = test_slice("li1ee");
    BencodeValue value = {0};
    assert(ErrKindNone != bencode_parse(&data, &arena, scratch, &value).kind);

    // An empty container needs no allocation at all, so it still succeeds.
    Slice_u8 data_empty = test_slice("le");
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

    Slice_u8 data_a = test_slice("li1ei2ee");
    BencodeValue a = {0};
    assert(ErrKindNone == bencode_parse(&data_a, &arena, scratch, &a).kind);
    assert(scratch_start == scratch.start);

    Slice_u8 data_b = test_slice("li3ee");
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

    Slice_u8 data = slice_u8_make(NULL, 0);
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
    Slice_u8 data = test_slice(input);

    // Poisoned so that a write on the failure path is visible.
    BencodeValue value = {.kind = BencodeKindString};
    assert(ErrKindNone != bencode_parse(&data, &arena, scratch, &value).kind);

    assert(arena_start == arena.start);
    assert(strlen(input) == data.len);
    assert(BencodeKindString == value.kind);
  }
}

static void test_bytes_cmp(void) {
  // A corpus in the order `bytes_cmp` must put it in. The embedded zeroes and
  // the bytes above 0x7f are there on purpose: neither `strcmp` nor a signed
  // char comparison orders these correctly.
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
      u8 *const a = (u8 *)sorted[i].data;
      u8 *const b = (u8 *)sorted[j].data;

      const i32 res = bytes_cmp(a, sorted[i].len, b, sorted[j].len);
      if (i < j) {
        assert(res < 0);
      } else if (i > j) {
        assert(res > 0);
      } else {
        assert(0 == res);
      }

      // Antisymmetric: swapping the arguments flips the sign.
      const i32 swapped = bytes_cmp(b, sorted[j].len, a, sorted[i].len);
      assert((res < 0) == (swapped > 0));
      assert((res > 0) == (swapped < 0));
      assert((0 == res) == (0 == swapped));
    }
  }

  // A NULL pointer is legal as long as the length is zero, and every empty
  // byte string is equal to every other one.
  {
    assert(0 == bytes_cmp(NULL, 0, NULL, 0));
    assert(0 == bytes_cmp(NULL, 0, (u8 *)"", 0));
    assert(bytes_cmp(NULL, 0, (u8 *)"a", 1) < 0);
    assert(bytes_cmp((u8 *)"a", 1, NULL, 0) > 0);
  }

  // Longer than a word, differing only in the last byte.
  {
    u8 x[64];
    u8 y[64];
    memset(x, 'z', sizeof(x));
    memset(y, 'z', sizeof(y));
    y[sizeof(y) - 1] = 'z' + 1;

    assert(bytes_cmp(x, sizeof(x), y, sizeof(y)) < 0);
    assert(bytes_cmp(y, sizeof(y), x, sizeof(x)) > 0);

    // Against itself, and against a prefix of itself.
    assert(0 == bytes_cmp(x, sizeof(x), x, sizeof(x)));
    assert(bytes_cmp(x, sizeof(x) - 1, x, sizeof(x)) < 0);
    assert(bytes_cmp(x, sizeof(x), x, sizeof(x) - 1) > 0);
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
        test_bencode_make_string("a", 1), num,
        test_bencode_make_string("b", 1), num,
        test_bencode_make_string("c", 1), num,
    };
    const BencodeList list = {.len = 6, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  // Out of order, anywhere in the dict.
  {
    BencodeValue children[] = {
        test_bencode_make_string("b", 1),
        num,
        test_bencode_make_string("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  {
    BencodeValue children[] = {
        test_bencode_make_string("a", 1), num,
        test_bencode_make_string("c", 1), num,
        test_bencode_make_string("b", 1), num,
    };
    const BencodeList list = {.len = 6, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Duplicate keys: sorted is not enough, the order has to be strict.
  {
    BencodeValue children[] = {
        test_bencode_make_string("a", 1),
        num,
        test_bencode_make_string("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Keys must be strings.
  {
    BencodeValue children[] = {num, num};
    const BencodeList list = {.len = 2, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // ... including a non-string key that is not the first one.
  {
    BencodeValue children[] = {test_bencode_make_string("a", 1), num, num, num};
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Values are not constrained, only keys are.
  {
    const BencodeValue nested_list = {.kind = BencodeKindList};
    const BencodeValue nested_dict = {.kind = BencodeKindDict};
    BencodeValue children[] = {
        test_bencode_make_string("a", 1),
        nested_list,
        test_bencode_make_string("b", 1),
        nested_dict,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  // An odd number of children is not key/value pairs.
  {
    BencodeValue children[] = {test_bencode_make_string("a", 1), num,
                               test_bencode_make_string("b", 1)};
    const BencodeList list = {.len = 3, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  {
    BencodeValue children[] = {test_bencode_make_string("a", 1)};
    const BencodeList list = {.len = 1, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // The empty key is legal and sorts before every other key.
  {
    BencodeValue children[] = {
        test_bencode_make_string("", 0),
        num,
        test_bencode_make_string("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  // A key that is a prefix of the next one is in order; the reverse is not.
  {
    BencodeValue children[] = {
        test_bencode_make_string("a", 1),
        num,
        test_bencode_make_string("ab", 2),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  {
    BencodeValue children[] = {
        test_bencode_make_string("ab", 2),
        num,
        test_bencode_make_string("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Ordering is by raw byte value, so `0x80` sorts *after* `0x7f`. A signed
  // comparison would get this pair backwards.
  {
    BencodeValue children[] = {
        test_bencode_make_string("\x7f", 1),
        num,
        test_bencode_make_string("\x80", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
  {
    BencodeValue children[] = {
        test_bencode_make_string("\x80", 1),
        num,
        test_bencode_make_string("\x7f", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone != bencode_validate_dict(list).kind);
  }
  // Keys are compared over their whole length, zero bytes included.
  {
    BencodeValue children[] = {
        test_bencode_make_string("a\x00"
                                 "a",
                                 3),
        num,
        test_bencode_make_string("a\x00"
                                 "b",
                                 3),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(ErrKindNone == bencode_validate_dict(list).kind);
  }
}

// Hash `data` in one `Update` call, the simplest possible use of the API.
static void test_sha256_once(Slice_u8 data, u8 res[SHA256_DIGEST_LENGTH]) {
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

static void test_sha256_expect_hex(Slice_u8 data, const char *expected_hex) {
  u8 expected[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(expected_hex, expected);

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  test_sha256_once(data, actual);

  assert(0 == memcmp(actual, expected, sizeof(actual)));
}

static void test_sha256_vectors(void) {
  // FIPS 180-2 / NIST CAVP vectors.
  test_sha256_expect_hex(
      test_slice(""),
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  test_sha256_expect_hex(
      test_slice("abc"),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56 bytes: the shortest message whose padding needs a second block.
  test_sha256_expect_hex(
      test_slice("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  test_sha256_expect_hex(
      test_slice("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijk"
                 "lmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
      "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

  // A NUL byte is data like any other: the API takes a length, never a C
  // string.
  test_sha256_expect_hex(
      slice_u8_make((u8 *)"\x00", 1),
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
  test_sha256_once(slice_u8_make(input, sizeof(input)), expected);

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
    // slice has no data pointer at all.
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
    test_sha256_once(slice_u8_make(input, len), digest);
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
  test_sha256_once(test_slice("abc"), expected);

  Sha256Ctx ctx = {0};
  sha256_init(&ctx);
  const Slice_u8 other = test_slice("some other message entirely");
  sha256_update(&ctx, other.data, other.len);

  u8 discarded[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&ctx, discarded);

  sha256_init(&ctx);
  const Slice_u8 abc = test_slice("abc");
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
__attribute__((warn_unused_result)) static Slice_u8
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

  return slice_u8_make(buf, TEST_MERKLE_MAX_LEN);
}

// Known answer tests. Sizes bracket every boundary the tree construction has:
// shorter than a block, exactly a block, one byte past a block, an exact
// power of two number of blocks, and block counts needing one or several
// padding leaves.
static void test_torrent_merkle_vectors(void) {
  const IO io = io_platform_make();
  Arena data_arena = {0};
  assert(ErrKindNone ==
         arena_valloc(&io, TEST_MERKLE_MAX_LEN + 4 * KiB, &data_arena).kind);
  assert(data_arena.start);
  const Slice_u8 data = test_merkle_data(&data_arena);

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
             torrent_build_merkle_tree(slice_u8_make(data.data, vectors[i].len),
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
  const IO io = io_platform_make();
  Arena data_arena = {0};
  assert(ErrKindNone ==
         arena_valloc(&io, TEST_MERKLE_MAX_LEN + 4 * KiB, &data_arena).kind);
  assert(data_arena.start);
  const Slice_u8 data = test_merkle_data(&data_arena);

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
             torrent_build_merkle_tree(slice_u8_make(data.data, len),
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
          sha256_digest(slice_u8_make(data.data + offset, block_len), layer[l]);
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
  const IO io = io_platform_make();
  Arena data_arena = {0};
  assert(ErrKindNone == arena_valloc(&io, 64 * KiB, &data_arena).kind);
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
  assert(ErrKindNone == torrent_build_merkle_tree(slice_u8_make(buf, len),
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
    sha256_digest(slice_u8_make(buf + offset, block_len), expected);
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
  sha256_digest(slice_u8_make(zero_block, TORRENT_BLOCK_SIZE), leaf[3]);

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

  assert(ErrKindNone == torrent_build_merkle_tree((Slice_u8){0}, 256 * KiB,
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
  const IO io = io_platform_make();
  Arena data_arena = {0};
  assert(ErrKindNone == arena_valloc(&io, 64 * KiB, &data_arena).kind);
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
  assert(ErrKindNone != torrent_build_merkle_tree(slice_u8_make(buf, len),
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
// things: the text, that the returned slice really does start at `dst.data`,
// and that nothing outside that slice was touched.
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

  const usize written = encode_usize_base_10(n, slice_u8_make(buf, dst_len));

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
  const usize widest = encode_usize_base_10(SIZE_MAX, slice_u8_make(buf, 20));
  assert(20 == widest);
  assert(0 == memcmp(buf, "18446744073709551615", 20));

  memset(buf, '#', sizeof(buf));
  const usize one = encode_usize_base_10(7, slice_u8_make(buf, 1));
  assert(1 == one);
  assert('7' == buf[0]);
  assert('#' == buf[1]);

  memset(buf, '#', sizeof(buf));
  const usize three = encode_usize_base_10(123, slice_u8_make(buf, 3));
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
    const usize got = encode_usize_base_10(n, slice_u8_make(buf, sizeof(buf)));

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

    Slice_u8 to_parse = slice_u8_make(terminated, got + 1);
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

  const usize written = encode_isize_base_10(n, slice_u8_make(buf, dst_len));

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
  const usize widest = encode_isize_base_10(INT64_MIN, slice_u8_make(buf, 20));
  assert(20 == widest);
  assert(0 == memcmp(buf, "-9223372036854775808", 20));

  memset(buf, '#', sizeof(buf));
  const usize two = encode_isize_base_10(-7, slice_u8_make(buf, 2));
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
    const usize got = encode_isize_base_10(n, slice_u8_make(buf, sizeof(buf)));

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

    Slice_u8 to_parse = slice_u8_make(framed, got + 2);
    BencodeValue parsed = {0};
    assert(ErrKindNone == bencode_parse_num(&to_parse, &parsed).kind);
    assert(BencodeKindInteger == parsed.kind);
    assert(n == parsed.v.num);
  }
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
  Slice_u8 dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
                  .len = cap};
  assert(dst.data);
  memset(dst.data, '#', cap);

  const usize written = bencode_encode_in_place(b, slice_u8_take(dst, size));

  // Comparing from the front of `dst` pins the anchoring.
  assert(expected_len == written);
  assert(0 == memcmp(dst.data, expected, written));

  // Nothing past what it reports was touched.
  for (usize i = written; i < cap; i++) {
    assert('#' == dst.data[i]);
  }
}

__attribute__((warn_unused_result)) static BencodeValue
test_bencode_int(isize n) {
  return (BencodeValue){.kind = BencodeKindInteger, .v.num = n};
}

__attribute__((warn_unused_result)) static BencodeValue
test_bencode_str(const char *s) {
  return (BencodeValue){.kind = BencodeKindString, .v.s = test_slice(s)};
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

  test_bencode_encode_once(test_bencode_str(""), "0:");
  test_bencode_encode_once(test_bencode_str("a"), "1:a");
  test_bencode_encode_once(test_bencode_str("spam"), "4:spam");
  test_bencode_encode_once(test_bencode_str("piece length"), "12:piece length");

  // A string whose `data` is null, which is how the `""` key of a v2 file
  // tree is built. `memcpy` wants valid pointers even for a zero byte copy.
  const BencodeValue null_str = {.kind = BencodeKindString, .v.s = {0}};
  test_bencode_encode_once(null_str, "0:");
}

// Strings are byte strings: NULs and high bytes pass through untouched, and
// the length prefix counts bytes rather than stopping at a terminator.
static void test_bencode_encode_binary_string(void) {
  u8 raw[6] = {0x00, 0xff, 'a', 0x00, 0x80, '\n'};
  const BencodeValue b = {.kind = BencodeKindString,
                          .v.s = slice_u8_make(raw, sizeof(raw))};

  Arena arena = test_arena(64 * KiB);
  const usize cap = bencode_encode_exact_size(b, 0);
  Slice_u8 dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
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
  items[0] = test_bencode_str("spam");
  items[1] = test_bencode_int(42);
  const BencodeValue list = {
      .kind = BencodeKindList, .v.list.len = 2, .v.list.data = items};
  test_bencode_encode_once(list, "l4:spami42ee");

  // `d3:keyl4:spami42eee`: a container nested inside a dict.
  BencodeValue *pair =
      arena_alloc(&arena, __alignof__(BencodeValue), sizeof(BencodeValue), 2);
  assert(pair);
  pair[0] = test_bencode_str("key");
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
        (BencodeValue){.kind = BencodeKindString, .v.s = slice_u8_make(key, 4)};
    entries[2 * i + 1] = test_bencode_int((isize)i);
  }

  const BencodeValue dict = {
      .kind = BencodeKindDict, .v.list.len = pairs * 2, .v.list.data = entries};

  const usize cap = bencode_encode_exact_size(dict, 0);
  Slice_u8 dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
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
  Slice_u8 to_parse = slice_u8_take(dst, written);
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

    Slice_u8 input = test_slice(documents[i]);
    const Slice_u8 original = input;

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

  const Slice_u8 name = test_slice("f.bin");
  BencodeValue info = {0};
  PieceHash *piece_hashes = NULL;
  usize piece_hashes_count = 0;
  Slice_u8 pieces_root_slice = {0};
  assert(ErrKindNone ==
         torrent_make_info_dict_v2(name, 16 * TORRENT_BLOCK_SIZE,
                                   slice_u8_make(file_data, file_len), name,
                                   &info, &pieces_root_slice, &piece_hashes,
                                   &piece_hashes_count, &arena)
             .kind);

  const usize cap = bencode_encode_exact_size(info, 0);
  Slice_u8 dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
                  .len = cap};
  assert(dst.data);

  const Slice_u8 got = slice_u8_take(dst, bencode_encode_in_place(info, dst));
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
static void test_digests_from_hex(const char *hex, Slice_u8 dst) {
  assert(hex);
  assert(0 == dst.len % SHA256_DIGEST_LENGTH);
  assert(2 * dst.len == strlen(hex));

  for (usize i = 0; i < dst.len / SHA256_DIGEST_LENGTH; i++) {
    char one[2 * SHA256_DIGEST_LENGTH + 1] = {0};
    memcpy(one, hex + i * 2 * SHA256_DIGEST_LENGTH, 2 * SHA256_DIGEST_LENGTH);
    test_digest_from_hex(one, dst.data + i * SHA256_DIGEST_LENGTH);
  }
}

// Is `needle` present in `haystack`? `memmem` is not C99, and an empty needle
// is not a question this asks.
__attribute__((warn_unused_result)) static bool
test_slice_contains(Slice_u8 haystack, Slice_u8 needle) {
  assert(!slice_u8_is_empty(needle));

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

  const Slice_u8 name = test_slice("f.bin");
  const Slice_u8 announce = test_slice("http://localhost:12345");

  BencodeValue info = {0};
  Slice_u8 pieces_root = {0};
  PieceHash *piece_hashes = NULL;
  usize piece_hashes_count = 0;
  assert(ErrKindNone ==
         torrent_make_info_dict_v2(name, 16 * TORRENT_BLOCK_SIZE,
                                   slice_u8_make(file_data, file_len), name,
                                   &info, &pieces_root, &piece_hashes,
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
  assert(test_bencode_is_string(metainfo.v.list.data[0], "announce"));
  assert(test_bencode_is_string(metainfo.v.list.data[2], "info"));
  assert(test_bencode_is_string(metainfo.v.list.data[4], "piece layers"));

  const BencodeValue announce_value = metainfo.v.list.data[1];
  assert(BencodeKindString == announce_value.kind);
  assert(announce.len == announce_value.v.s.len);
  assert(0 == memcmp(announce_value.v.s.data, announce.data, announce.len));

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
    assert(BencodeKindString == layer_key.kind);
    assert(SHA256_DIGEST_LENGTH == layer_key.v.s.len);
    assert(0 ==
           memcmp(layer_key.v.s.data, expected_root, SHA256_DIGEST_LENGTH));
    assert(!slice_u8_eq_cstr(layer_key.v.s, "f.bin"));

    const BencodeValue layer_value = layers.v.list.data[1];
    assert(BencodeKindString == layer_value.kind);
    assert(expected_layer_len == layer_value.v.s.len);

    Slice_u8 expected_layer = {.data =
                                   arena_alloc(&arena, __alignof__(u8),
                                               sizeof(u8), expected_layer_len),
                               .len = expected_layer_len};
    assert(expected_layer.data);
    test_digests_from_hex(expected_layer_hex, expected_layer);
    assert(0 == memcmp(layer_value.v.s.data, expected_layer.data,
                       expected_layer_len));

    // And it is exactly the piece hashes the merkle build handed over,
    // concatenated, in order, with nothing inserted between them.
    assert(0 == memcmp(layer_value.v.s.data, piece_hashes, expected_layer_len));
  }

  // The infohash is over the info dict alone, so encoding it on its own and
  // then finding those exact bytes inside the metainfo encoding is the
  // property everything downstream depends on: nesting must not perturb them.
  Slice_u8 info_encoded = {0};
  assert(ErrKindNone == bencode_encode(info, &info_encoded, &arena).kind);

  u8 infohash[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest(info_encoded, infohash);
  u8 expected_infohash[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(expected_infohash_hex, expected_infohash);
  assert(0 == memcmp(infohash, expected_infohash, sizeof(infohash)));

  Slice_u8 metainfo_encoded = {0};
  assert(ErrKindNone ==
         bencode_encode(metainfo, &metainfo_encoded, &arena).kind);
  assert(metainfo_encoded.len > info_encoded.len);
  assert(test_slice_contains(metainfo_encoded, info_encoded));

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
  Slice_u8 to_parse = metainfo_encoded;
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
    sha256_digest(slice_u8_make(data, len), dispatched);

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

// Removing a scratch file the harness itself made. It goes to the real
// platform for the same reason `test_stdout_silence` does: the file is on the
// real filesystem whatever `io` the test under it happens to be driving.
__attribute__((warn_unused_result)) static Error
test_remove_file(Slice_u8 path) {
  const IO io = io_platform_make();

  return io.remove_file(&io, path);
}

// A path under `TMPDIR` unique to this process, so a test run does not
// collide with a stale file or with another run.
__attribute__((warn_unused_result)) static Slice_u8
test_tmp_path(char *buf, usize buf_len, const char *name) {
  const IO io = io_platform_make();

  const char *const dir = getenv("TMPDIR");
  const i32 n = snprintf(buf, buf_len, "%s/file_send_test_%zu_%s",
                         dir ? dir : "/tmp", io.get_process_id(&io), name);
  assert(n > 0);
  assert((usize)n < buf_len);

  return slice_u8_make((u8 *)buf, (usize)n);
}

// The failure paths of `open`, which are the ones a caller actually has to
// handle: they are reached through the vtable like any other caller would.
static void test_io_open_errors(void) {
  const IO io = io_platform_make();
  i32 fd = -1;

  // An empty path is rejected before the syscall.
  assert(
      ErrKindInvalidData ==
      io.open(&io, slice_u8_make(NULL, 0), FileOpenOptionsReadOnly, &fd).kind);
  assert(ErrKindInvalidData ==
         io.open(&io, test_slice(""), FileOpenOptionsReadOnly, &fd).kind);

  // So is one too long for the fixed buffer, and the limit rides along in
  // `data` rather than an `errno` that was never set.
  {
    char long_path[5000] = {0};
    memset(long_path, 'a', sizeof(long_path) - 1);
    const Slice_u8 path = slice_u8_make((u8 *)long_path, sizeof(long_path) - 1);

    const Error err = io.open(&io, path, FileOpenOptionsReadOnly, &fd);
    assert(ErrKindRange == err.kind);
    assert(4095 == err.data);
  }

  // A missing file is the OS's complaint, carried verbatim.
  {
    char buf[256] = {0};
    const Slice_u8 path = test_tmp_path(buf, sizeof(buf), "does_not_exist");
    (void)test_remove_file(path);

    const Error err = io.open(&io, path, FileOpenOptionsReadOnly, &fd);
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
  const IO io = io_platform_make();

  char buf[256] = {0};
  const Slice_u8 path = test_tmp_path(buf, sizeof(buf), "round_trip");
  (void)test_remove_file(path);

  // Bencode is binary, so a NUL in the middle must survive.
  const u8 payload[] = {'d', '3', ':', 'a', 'b', 'c', 0x00, 'e'};
  const Slice_u8 data = slice_u8_make((u8 *)payload, sizeof(payload));

  assert(ErrKindNone == io.write_all_to_file(&io, path, data).kind);

  {
    Slice_u8 got = {0};
    assert(ErrKindNone ==
           io.map_file(&io, path, FileOpenOptionsReadOnly, &got).kind);
    assert(data.len == got.len);
    assert(0 == memcmp(data.data, got.data, data.len));
  }

  // Rewriting with fewer bytes truncates: without `O_TRUNC` the old tail
  // would still be there, which for a bencode file is silent corruption.
  {
    const u8 shorter[] = {'i', '1', 'e'};
    const Slice_u8 data_shorter = slice_u8_make((u8 *)shorter, sizeof(shorter));
    assert(ErrKindNone == io.write_all_to_file(&io, path, data_shorter).kind);

    Slice_u8 got = {0};
    assert(ErrKindNone ==
           io.map_file(&io, path, FileOpenOptionsReadOnly, &got).kind);
    assert(sizeof(shorter) == got.len);
    assert(0 == memcmp(shorter, got.data, sizeof(shorter)));
  }

  // Writing nothing is a no-op, not a truncation: the file is left as it was.
  {
    assert(ErrKindNone ==
           io.write_all_to_file(&io, path, slice_u8_make(NULL, 0)).kind);

    Slice_u8 got = {0};
    assert(ErrKindNone ==
           io.map_file(&io, path, FileOpenOptionsReadOnly, &got).kind);
    assert(3 == got.len);
  }

  assert(ErrKindNone == test_remove_file(path).kind);

  // Mapping what is no longer there fails rather than handing back an empty
  // slice.
  {
    Slice_u8 got = {0};
    assert(ErrKindNone !=
           io.map_file(&io, path, FileOpenOptionsReadOnly, &got).kind);
  }

  // An empty file has nothing to map: `mmap` rejects a zero length, and that
  // is reported rather than handed back as an empty slice.
  {
    char empty_buf[256] = {0};
    const Slice_u8 empty_path =
        test_tmp_path(empty_buf, sizeof(empty_buf), "empty");
    (void)test_remove_file(empty_path);

    i32 fd = -1;
    assert(ErrKindNone ==
           io.open(&io, empty_path,
                   FileOpenOptionsWriteOnly | FileOpenOptionsCreate, &fd)
               .kind);
    assert(ErrKindNone == io.close(&io, fd).kind);

    Slice_u8 got = {0};
    assert(ErrKindNone !=
           io.map_file(&io, empty_path, FileOpenOptionsReadOnly, &got).kind);
    assert(slice_u8_is_empty(got));

    assert(ErrKindNone == test_remove_file(empty_path).kind);
  }

  // A directory opens but cannot be mapped, which walks the `mmap` failure
  // path with the descriptor already in hand.
  {
    Slice_u8 got = {0};
    assert(ErrKindNone !=
           io.map_file(&io, test_slice("/tmp"), FileOpenOptionsReadOnly, &got)
               .kind);
  }

  // A path `open` rejects without setting `errno` still comes back as the
  // range error, not as whatever `errno` happened to hold.
  {
    char long_path[5000] = {0};
    memset(long_path, 'a', sizeof(long_path) - 1);
    const Slice_u8 too_long =
        slice_u8_make((u8 *)long_path, sizeof(long_path) - 1);

    Slice_u8 got = {0};
    assert(ErrKindRange ==
           io.map_file(&io, too_long, FileOpenOptionsReadOnly, &got).kind);

    const u8 byte = 'x';
    assert(ErrKindRange ==
           io.write_all_to_file(&io, too_long, slice_u8_make((u8 *)&byte, 1))
               .kind);
  }
}

// Every allocation failure inside the torrent builder, walked by squeezing
// the arenas. The sizes are found by bisection rather than reasoned about:
// what matters is that each step reports rather than aborting, and that a
// caller never sees a half built torrent.
static void test_torrent_gen_torrent_file_data_oom(void) {
  const Slice_u8 file_path = test_slice("some_dir/payload.bin");
  const Slice_u8 announce = test_slice("http://localhost:12345");

  Arena data_arena = test_arena(64 * KiB);
  const usize data_len = 40 * KiB;
  u8 *const data_bytes = arena_alloc(&data_arena, 1, sizeof(u8), data_len);
  assert(data_bytes);
  for (usize i = 0; i < data_len; i++) {
    data_bytes[i] = (u8)(i * 31);
  }
  const Slice_u8 file_data = slice_u8_make(data_bytes, data_len);

  // The whole thing succeeds when there is room, which is what makes the
  // failures below meaningful.
  usize needed_scratch = 0;
  {
    Arena arena = test_arena(64 * KiB);
    Arena scratch = test_arena(64 * KiB);
    const u8 *const scratch_start = scratch.start;
    Slice_u8 torrent = {0};
    u8 info_hash[SHA256_DIGEST_LENGTH] = {0};

    assert(ErrKindNone ==
           torrent_gen_torrent_file_data(file_path, file_data, announce,
                                         &torrent, info_hash, scratch, &arena)
               .kind);
    assert(!slice_u8_is_empty(torrent));

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
    Slice_u8 torrent = {.data = (u8 *)0xAA, .len = 1};
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
    Slice_u8 torrent = {0};
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
  };

  // `slice_u8_from_cstr` only ever runs on these in anger, so it rides along
  // here rather than earning a test of its own.
  assert(slice_u8_eq_cstr(slice_u8_from_cstr((char *)"torrent"), "torrent"));
  assert(slice_u8_is_empty(slice_u8_from_cstr((char *)"")));

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
  const IO io = io_platform_make();

  // `fstat` on a descriptor that was never open.
  {
    usize size = 0xAA;
    const Error err = io.file_size(&io, -1, &size);
    assert(ErrKindNone != err.kind);
    assert(EBADF == (i32)err.data);
    // Nothing is written when there is nothing to report.
    assert(0xAA == size);
  }

  // `close` of the same.
  {
    const Error err = io.close(&io, -1);
    assert(ErrKindNone != err.kind);
    assert(EBADF == (i32)err.data);
  }

  // `mprotect` wants a page aligned address, so an odd one is rejected
  // without having to find an unmapped page first.
  {
    const Error err = io.vprotect_none(&io, (void *)1, 4096);
    assert(ErrKindNone != err.kind);
  }

  // An empty slice contains nothing, without reading through a null pointer.
  assert(!slice_u8_contains_byte(slice_u8_make(NULL, 0), 'x'));

  // Mapping for writing takes the other protection branch. The file is still
  // opened read only and the mapping is private, so the bytes on disk are
  // safe either way.
  {
    char buf[256] = {0};
    const Slice_u8 path = test_tmp_path(buf, sizeof(buf), "map_write");
    const u8 payload[] = {'a', 'b', 'c'};

    assert(ErrKindNone ==
           io.write_all_to_file(&io, path,
                                slice_u8_make((u8 *)payload, sizeof(payload)))
               .kind);

    Slice_u8 got = {0};
    assert(ErrKindNone ==
           io.map_file(&io, path, FileOpenOptionsWriteOnly, &got).kind);
    assert(sizeof(payload) == got.len);

    assert(ErrKindNone == test_remove_file(path).kind);
  }
}

// The allocation failures inside the two dictionary builders. Sweeping the
// arena a few bytes at a time walks each early return in turn; which size
// trips which is not the point, only that every one reports instead of
// aborting or half building something.
static void test_torrent_make_dicts_oom(void) {
  const Slice_u8 name = test_slice("payload.bin");
  const Slice_u8 announce = test_slice("http://localhost:12345");

  Arena data_arena = test_arena(2 * MiB);
  // More than one 256 KiB piece, so there is a piece layer to allocate and
  // not just a root: the layer is the largest allocation of the two.
  const usize data_len = 1 * MiB;
  u8 *const bytes = arena_alloc(&data_arena, 1, sizeof(u8), data_len);
  assert(bytes);
  for (usize i = 0; i < data_len; i++) {
    bytes[i] = (u8)(i * 31);
  }
  const Slice_u8 file_data = slice_u8_make(bytes, data_len);

  // One run with room to spare, to build what the metainfo builder needs.
  Arena big = test_arena(1 * MiB);
  BencodeValue info_dict = {0};
  Slice_u8 pieces_root = {0};
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
    Slice_u8 root = {0};
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

// A partial override: the real `map_file` and `write_all_to_file` run against
// faked primitives. That is the whole reason a slot is handed the vtable --
// the composites are the program's operations, the primitives underneath them
// are the platform's, and only the second kind is worth faking.
//
// Anything not faked is delegated to `real`, so `open` still yields a
// descriptor the rest of the code can use. A fake never invents one: it
// either fails or hands back a genuine one.
typedef struct {
  IO real;

  ErrorKind open_fails_with;
  ErrorKind file_size_fails_with;
  ErrorKind write_fails_with;

  // Report at most this many bytes per `write`, so the loop has to go around
  // more than once. Zero means "as many as asked".
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

__attribute__((warn_unused_result)) static Error
test_file_open(const IO *io, Slice_u8 path, FileOpenOptions opts, i32 *fd) {
  TestFileCtx *const c = io->ctx;
  assert(c);

  c->open_calls += 1;
  if (ErrKindNone != c->open_fails_with) {
    return (Error){.kind = c->open_fails_with};
  }

  return c->real.open(&c->real, path, opts, fd);
}

__attribute__((warn_unused_result)) static Error test_file_close(const IO *io,
                                                                 i32 fd) {
  TestFileCtx *const c = io->ctx;
  assert(c);

  c->close_calls += 1;
  return c->real.close(&c->real, fd);
}

__attribute__((warn_unused_result)) static Error
test_file_file_size(const IO *io, i32 fd, usize *dst_size) {
  TestFileCtx *const c = io->ctx;
  assert(c);

  c->file_size_calls += 1;
  if (ErrKindNone != c->file_size_fails_with) {
    return (Error){.kind = c->file_size_fails_with};
  }

  return c->real.file_size(&c->real, fd, dst_size);
}

__attribute__((warn_unused_result)) static Error
test_file_write(const IO *io, i32 fd, Slice_u8 data, usize *dst_written) {
  TestFileCtx *const c = io->ctx;
  assert(c);
  assert(dst_written);

  c->write_calls += 1;

  if (c->write_calls == c->write_interrupted_at) {
    return (Error){.kind = ErrKindInterrupted};
  }
  if (ErrKindNone != c->write_fails_with) {
    return (Error){.kind = c->write_fails_with};
  }
  if (c->write_calls == c->write_zero_at) {
    *dst_written = 0;
    return (Error){.kind = ErrKindNone};
  }

  const usize chunk = (0 != c->write_chunk && c->write_chunk < data.len)
                          ? c->write_chunk
                          : data.len;
  return c->real.write(&c->real, fd, slice_u8_take(data, chunk), dst_written);
}

__attribute__((warn_unused_result)) static IO
test_io_file_make(TestFileCtx *ctx) {
  assert(ctx);

  return (IO){
      // The operations under test, real.
      .map_file = unix_map_file,
      .write_all_to_file = unix_write_all_to_file,
      // The primitives beneath them, faked.
      .open = test_file_open,
      .close = test_file_close,
      .file_size = test_file_file_size,
      .write = test_file_write,
      .ctx = ctx,
  };
}

// The error paths inside the composites, which no real file can produce: a
// `open` that fails after the path was fine, an `fstat` that fails on a
// descriptor that just opened, a short write, an interrupted one.
static void test_io_composites_mocked(void) {
  char buf[256] = {0};
  const Slice_u8 path = test_tmp_path(buf, sizeof(buf), "composites");
  const u8 payload[] = {'d', '3', ':', 'a', 'b', 'c', 0x00, 'e'};
  const Slice_u8 data = slice_u8_make((u8 *)payload, sizeof(payload));

  // A failed `open` stops `map_file` before anything else is tried.
  {
    TestFileCtx ctx = {.real = io_platform_make(),
                       .open_fails_with = ErrKindTooManyFiles};
    const IO io = test_io_file_make(&ctx);
    Slice_u8 got = {0};

    assert(ErrKindTooManyFiles ==
           io.map_file(&io, path, FileOpenOptionsReadOnly, &got).kind);
    assert(1 == ctx.open_calls);
    assert(0 == ctx.file_size_calls);
    // Nothing was opened, so nothing is closed.
    assert(0 == ctx.close_calls);
    assert(slice_u8_is_empty(got));
  }

  // The same for `write_all_to_file`.
  {
    TestFileCtx ctx = {.real = io_platform_make(),
                       .open_fails_with = ErrOSKindPermission};
    const IO io = test_io_file_make(&ctx);

    assert(ErrOSKindPermission == io.write_all_to_file(&io, path, data).kind);
    assert(0 == ctx.write_calls);
    assert(0 == ctx.close_calls);
  }

  // Write the file for real, so there is something to map.
  {
    TestFileCtx ctx = {.real = io_platform_make()};
    const IO io = test_io_file_make(&ctx);

    assert(ErrKindNone == io.write_all_to_file(&io, path, data).kind);
    assert(1 == ctx.write_calls);
    assert(1 == ctx.close_calls);
  }

  // A failed `fstat` on a descriptor that just opened: unreachable with a
  // real file, and it is the path that has to hand the descriptor back.
  {
    TestFileCtx ctx = {.real = io_platform_make(), .file_size_fails_with = ErrKindRange};
    const IO io = test_io_file_make(&ctx);
    Slice_u8 got = {0};

    assert(ErrKindRange ==
           io.map_file(&io, path, FileOpenOptionsReadOnly, &got).kind);
    assert(1 == ctx.file_size_calls);
    assert(1 == ctx.close_calls);
    assert(slice_u8_is_empty(got));
  }

  // A short write keeps its place and goes around again until everything has
  // landed. One byte at a time is the extreme case of it.
  {
    TestFileCtx ctx = {.real = io_platform_make(), .write_chunk = 1};
    const IO io = test_io_file_make(&ctx);

    assert(ErrKindNone == io.write_all_to_file(&io, path, data).kind);
    assert(sizeof(payload) == ctx.write_calls);

    const IO real = io_platform_make();
    Slice_u8 got = {0};
    assert(ErrKindNone ==
           real.map_file(&real, path, FileOpenOptionsReadOnly, &got).kind);
    assert(data.len == got.len);
    assert(0 == memcmp(data.data, got.data, data.len));
  }

  // A signal before any progress is not a failure: the call is reissued and
  // the same bytes go out.
  {
    TestFileCtx ctx = {
        .real = io_platform_make(), .write_chunk = 2, .write_interrupted_at = 2};
    const IO io = test_io_file_make(&ctx);

    assert(ErrKindNone == io.write_all_to_file(&io, path, data).kind);
    // Four chunks of two, plus the interrupted call that carried nothing.
    assert(5 == ctx.write_calls);

    const IO real = io_platform_make();
    Slice_u8 got = {0};
    assert(ErrKindNone ==
           real.map_file(&real, path, FileOpenOptionsReadOnly, &got).kind);
    assert(data.len == got.len);
    assert(0 == memcmp(data.data, got.data, data.len));
  }

  // A write that reports no progress and no error would spin forever, so it
  // is treated as the peer hanging up.
  {
    TestFileCtx ctx = {.real = io_platform_make(), .write_zero_at = 1};
    const IO io = test_io_file_make(&ctx);

    assert(ErrKindConnReset == io.write_all_to_file(&io, path, data).kind);
    assert(1 == ctx.write_calls);
    // The descriptor is handed back even on the way out.
    assert(1 == ctx.close_calls);
  }

  // A failed write reports, and still closes.
  {
    TestFileCtx ctx = {.real = io_platform_make(), .write_fails_with = ErrKindConnReset};
    const IO io = test_io_file_make(&ctx);

    assert(ErrKindConnReset == io.write_all_to_file(&io, path, data).kind);
    assert(1 == ctx.close_calls);
  }

  assert(ErrKindNone == test_remove_file(path).kind);
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
      {"arena_alloc", test_arena_alloc},
      {"unix_error_from_errno", test_unix_error_from_errno},
      {"arena_valloc", test_arena_valloc},
      {"arena_valloc_mocked", test_arena_valloc_mocked},
      {"io_listen_and_serve_setup_failures",
       test_io_listen_and_serve_setup_failures},
      {"io_listen_and_serve_accept", test_io_listen_and_serve_accept},
      {"torrent_client_pool_exhaustion", test_torrent_client_pool_exhaustion},
      {"io_syscall_failures", test_io_syscall_failures},
      {"torrent_make_dicts_oom", test_torrent_make_dicts_oom},
      {"io_composites_mocked", test_io_composites_mocked},
      {"error_kind_to_cstr", test_error_kind_to_cstr},
      {"io_open_errors", test_io_open_errors},
      {"io_file_round_trip", test_io_file_round_trip},
      {"torrent_gen_torrent_file_data_oom",
       test_torrent_gen_torrent_file_data_oom},
      {"slice_u8", test_slice_u8},
      {"path_last_component", test_path_last_component},
      {"path_get_ext", test_path_get_ext},
      {"path_with_ext", test_path_with_ext},
      {"ascii_num_parse", test_ascii_num_parse},
      {"bencode_parse_num", test_bencode_parse_num},
      {"bencode_parse_string", test_bencode_parse_string},
      {"bencode_parse", test_bencode_parse},
      {"bytes_cmp", test_bytes_cmp},
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
      {"bencode_encode_leaves", test_bencode_encode_leaves},
      {"bencode_encode_binary_string", test_bencode_encode_binary_string},
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
