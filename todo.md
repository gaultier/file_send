- peer as an actor:
    - Each peer has an array of max 8 commands
    - Code bridges the io bytes -> events and commands -> io operations
    - `peer_run` is easy to test (pure)
    - a peer can send an event to other peers (e.g.: new block arrived). This could create feedback loops, perhaps could be avoided with shared state (since we are single threaded) e.g. a bitset per file to record which blocks we already have, shared by all peers.
```
// NOTE: `Command[]` allows for pipelining writes or queuing a several IO operations e.g.:  upon receiving a block, a socket read (for the next block) + a file write (for this block).
// `Event` is a superset of 'peer messages' + 'network events' e.g. timeout, events from other peer connections (e.g.: new block received), 'file fully downloaded' (so perhaps close the connection), etc.
Command[] peer_run(Event event) {
    switch (peer->state) {
  case TorrentPeerStateInitial: {
      assert(EventKindNone == event.kind);

      // TODO: Do we need 'sagas' e.g. 'queued handshake' + 'handshake sent (confirmed)' ?
      peer->state = TorrentPeerStateSentHandshake;
      return Command[]{SendHandshake};
  }break;

  case TorrentPeerStateSentHandshake: {
      if(EventKindHandshake != event.kind) {
          // Invalid.
          return Command[]{Close};
      }

      // Fully handshaked.
      peer->state = TorrentPeerStateHandshaked; 
  
      return Command[]{HaveAll}; // Or: Bitfield 0b111111111111...
  }break;

    
//    [...]
    }

    // No other handler decided on commands to send so idle for a bit.

    return Command[]{SendKeepAlive, IdleFor1Minute}; // After which we need to send a keep alive.
}
```
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
  - a timer operation, so `ErrKindTooManyFiles` on accept can back off instead
    of bringing the listener down
  - `accept4` with `SOCK_NONBLOCK` on Linux, and `EPOLL_CTL_ADD` without the
    `EEXIST` round trip, would each save a syscall per connection
  - `epoll_pwait2` takes a `timespec`, so `run_for_ns` would not have to round
    its nanoseconds up to a millisecond
  - nothing drives `IO.connect` on either platform yet, so it is untested
  - once the peer protocol writes to sockets, measure whether `write` and
    `send_to` should make the syscall in the slot rather than on the next turn
    of the loop, as libuv's `uv_write` does. The reasoning, and what it would
    cost, is on the `IO` struct in `lib.c`; the short of it is that it buys a
    return-to-loop and not a syscall, needs the answer kept on the completion,
    and is only safe while nothing else is pending on that descriptor.
