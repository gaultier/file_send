- peer as an actor: built. `torrent_peer_run` in `torrent.c` is the whole of a
  peer's decision making -- no io, no allocation, no clock, and so no way to
  fail -- and everything around it is the bridge that turns bytes into events
  and commands into io operations. The reasoning sketched here now lives in the
  comments beside the code. What is left:
    - `SendBitfield`, `SendRequests` and `WriteBlock` are not commands yet,
      because each needs state that does not exist: the `have` and `requested`
      bitsets, and a file to write into. `torrent_peer_run`'s `Handshaked` case
      carries the `TODO` where the first of them goes.
    - `interested` goes out unconditionally on reaching `Handshaked`, and it is
      a claim that is not checked: it says the peer has something we want, and
      `share` is a seed with nothing to want. It stays until there is a `have`
      bitset to compare against theirs, at which point it becomes conditional
      and `uninterested` becomes reachable. Harmless meanwhile -- a peer that is
      unchoked and asked for nothing costs the other end one slot.
    - The `TODO` for `SendBitfield` sits above the `interested` and `unchoke`
      pushes on purpose: BEP 3 has the bitfield as the *first* message after the
      handshake or not at all, so anything queued before it forecloses ever
      sending one. Order matters in that block in a way it does not elsewhere.
    - No peer-to-peer events, which is what would make feedback loops possible.
      A block arriving elsewhere needs nothing from anyone right away, so it is
      shared state and the next peer to get an event reads it:
        - a `have` bitset per torrent, which replaces the broadcast
        - a `requested` bitset, with the owning slot and a deadline, which is the
          one genuinely cross-peer decision: two peers fetching the same block
        - `file fully downloaded` as a flag peers read, not an event delivered to
          each of them
    - A third deadline, for a request timing out. It is a third pair of "what
      resets it" and "what happens when it fires" and not a variant of the two
      that exist: reset by sending a request, and answered by a block to ask
      someone else for rather than by a peer to close.
    - Backpressure is the one leak in "cannot fail": if a command says write and
      the send buffer is full, either the bridge defers or `peer->can_send`
      becomes an input. `SendKeepAlive` is the only command that can reach it
      today, and it drops the keep-alive -- right for that one, a full buffer
      being the opposite of the silence it breaks, and not a general answer.
    - Request pacing. One command must mean "request up to N blocks", or a
      `bitfield` from a peer that has everything outruns the array.
    - The slot generation counter is not there, and is not needed while the
      bridge closes the way it does: a hang-up waits for every outstanding
      operation to report before the slot goes back, so no completion can land on
      a slot that has been reused. It becomes necessary the moment a slot is
      released with something still in flight.
    - `test_peer_run` in `test.c` is the harness that drives a peer through the
      fake io, and it now sits beside `test_torrent_peer_run`, which is the actor
      itself. One of the two needs renaming.
    - The walk in `torrent_peers_deadlines_run` is O(N) over the pool's occupied
      bitset, and that is the starting point rather than the end of it. A
      128-bucket one-second wheel (`u16 head[128]`, `u16 next_in_bucket` on the
      peer) makes it O(1), and the deadlines are coarse and bounded enough --
      nothing beyond a couple of minutes -- that no overflow list is needed. What
      it costs is cancellation coming back: a re-arm has to move buckets and a
      closing peer leaves an entry behind, dropped lazily against the slot
      generation when it pops. Worth it somewhere past a few thousand peers, and
      not before.
- first bind the TCP socket, then broadcast the port over UDP
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
  - the monotonic clock is done: `IO.monotonic_ns`, backed by
    `clock_gettime(CLOCK_MONOTONIC)` in `unix.c` and shared by both readiness
    backends. Not a timer operation -- no `IoActionKind` for it, nothing armed,
    nothing to cancel -- and on `IO` rather than `Env` because the loop is the
    only caller that wants it every turn. `main`'s loop sizes each `run_for_ns`
    by the nearest deadline instead of a fixed tick, so `io_run_until`'s
    parameter is a ceiling and not a period, and the fake in `test.c` answers
    the clock from a field a test assigns. Win32 needs
    `QueryPerformanceCounter` when it gets an `IO` at all. Still to do:
    `ErrKindTooManyFiles` on accept should back off by setting
    `server->retry_accept_at` rather than bringing the listener down, and the
    walk that finds due peers would find it -- which means the walk has to take
    the listener as well as the pool.
  - `accept4` with `SOCK_NONBLOCK` on Linux, and `EPOLL_CTL_ADD` without the
    `EEXIST` round trip, would each save a syscall per connection
  - `epoll_pwait2` takes a `timespec`, so `run_for_ns` would not have to round
    its nanoseconds up to a millisecond. This matters more once deadlines size
    the wait: the rounding stops being a curiosity and becomes a wake up to a
    millisecond late. Harmless for this protocol, but it is the same fix.
  - nothing drives `IO.connect` on either platform yet, so it is untested
  - once the peer protocol writes to sockets, measure whether `write` and
    `send_to` should make the syscall in the slot rather than on the next turn
    of the loop, as libuv's `uv_write` does. The reasoning, and what it would
    cost, is on the `IO` struct in `lib.c`; the short of it is that it buys a
    return-to-loop and not a syscall, needs the answer kept on the completion,
    and is only safe while nothing else is pending on that descriptor.
