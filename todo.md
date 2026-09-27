- first bind the TCP socket, then broadcast the port over UDP
- peer protocol
- listen for UDP broadcasts
- async io: Darwin (kqueue) done, no threads. Still to do:
  - Linux (io_uring) and Win32 (IOCP) `io_platform_make`
  - a timer operation, so `ErrKindTooManyFiles` on accept can back off instead
    of bringing the listener down
  - batch the kqueue changelist further: it is already one `kevent` per turn of
    the loop, but a file operation still costs a turn per step
  - nothing drives `IO.connect` yet, so it is untested
