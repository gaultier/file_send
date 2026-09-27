- first bind the TCP socket, then broadcast the port over UDP
- peer protocol
- listen for UDP broadcasts
- async io, no threads. Darwin (kqueue) and Linux (epoll) done. Still to do:
  - io_uring, as `linux_io_uring_make` next to `linux_io_epoll_make`. It is a
    peer of the epoll backend and not an `#ifdef` inside it: the kernel makes
    the syscall there, so neither `unix_io_perform` nor `linux_advance`
    applies, and the submission ring replaces both the pending list and epoll.
    Two things to settle first:
    - `IoAction.v.open.path` is borrowed from the caller and has no NUL. The
      readiness backends copy it into a buffer at the moment they call `open`,
      which is fine because that is on this thread; a ring has to keep it
      alive until the kernel is done with it.
    - pick the default at runtime, by asking the running kernel, rather than
      at build time.
  - Win32: `win32.c` builds and links on both targets, and every slot of both
    vtables is null, so it faults on first use. In order: `Env`'s memory
    (`GetSystemInfo`, `VirtualAlloc`, `VirtualProtect`), which is what `main`
    needs before anything else; then the rest of `Env`; then IOCP for `IO`,
    which like io_uring has the kernel make the syscall, so an overlapped
    operation starts at submission and `GetQueuedCompletionStatusEx` collects
    what finished. A `WITH_TESTS` build stays Unix-only until then: the fakes
    in `test.c` call `unix_*` directly.
  - a timer operation, so `ErrKindTooManyFiles` on accept can back off instead
    of bringing the listener down
  - `accept4` with `SOCK_NONBLOCK` on Linux, and `EPOLL_CTL_ADD` without the
    `EEXIST` round trip, would each save a syscall per connection
  - `epoll_pwait2` takes a `timespec`, so `run_for_ns` would not have to round
    its nanoseconds up to a millisecond
  - nothing drives `IO.connect` on either platform yet, so it is untested
