#pragma once

#include "lib.c"
#include "unix.c"

#include <sys/epoll.h>

// The Linux `Platform`. One epoll set, one thread, and -- unlike the Darwin one
// -- a list of its own.
//
// The Darwin backend keeps no queue because kqueue will hold one for it:
// `EVFILT_USER` means "hand this back on the next turn of the loop", so a
// submitted operation can be given straight to the kernel and collected with
// everything else. epoll has nothing of the kind. An `eventfd` wakes the loop
// but carries no per-operation data, so it cannot say *which* operation to try.
// So an operation that has not been tried yet waits here instead, and the loop
// works through that list before it waits on anything.
//
// The second difference is what epoll hands back. kqueue keys a readiness event
// on the descriptor and can be handed the descriptor in a user event's `data`,
// so between the two the kernel always carries it. epoll has one 64-bit `data`
// per registration and this backend needs two things out of it -- which
// completion, and which descriptor -- which do not both fit. So `data.u32`
// carries an index into `ops` below, and that is where both live.
//
// The third is that a registration is per descriptor and not per
// descriptor-and-filter. One operation per descriptor at a time, therefore; see
// `linux_arm`.

// Reports taken from the kernel in one turn of the loop. More than this and the
// rest wait for the next turn, which is what happens to anything submitted
// while the callbacks are running anyway.
#define EPOLL_MAX_EVENTS 1024

// Operations this backend can be holding at once: a listener's accept, one per
// connection being served, and a few for whatever the startup path is doing.
// Past this a slot reports backpressure rather than taking the operation.
//
// Its own number, and not the client pool's: a `Platform` knows nothing about
// what is being served over it. The same as the Darwin backend's changelist, so
// that the two refuse at the same point rather than at two numbers nobody
// chose.
#define IO_LINUX_OPS_MAX 2048

// No operation. `IO_LINUX_OPS_MAX` is nowhere near this, so it cannot be a
// real index.
#define IO_LINUX_OP_NONE ((u32)UINT32_MAX)

// One operation in flight, and the list link that says what it is waiting for.
// The link is an index and not a pointer so that it fits in half a word, and it
// lives here and not on `PlatformCompletion` because a completion is the
// caller's description of what it wants done and not this backend's
// bookkeeping: another backend keeps none of this.
typedef struct {
  PlatformCompletion *completion;
  i32 fd;
  // Next on whichever list this is on, or `IO_LINUX_OP_NONE`. One list at a
  // time: free, or submitted-and-not-yet-tried. An operation that epoll is
  // watching is on neither -- the kernel is holding it, by index.
  u32 next;
} PlatformLinuxOp;

typedef struct {
  // Must be first: an `PlatformLinux *` is a `Platform *`, which is what lets a
  // slot reached through the vtable find its way back to this.
  Platform platform;

  i32 epoll_fd;

  PlatformLinuxOp ops[IO_LINUX_OPS_MAX];
  // Free slots, and operations waiting for their first try. Singly linked
  // through `PlatformLinuxOp.next`; the submitted list keeps a tail so that
  // submissions are tried in the order they were made.
  u32 free_head;
  u32 submitted_head;
  u32 submitted_tail;

  // Operations epoll is watching. A wait with none of them can only run the
  // timeout down, so the loop does not make one.
  usize armed_count;
} PlatformLinux;

__attribute__((warn_unused_result)) static u32
linux_op_alloc(PlatformLinux *platform_linux, PlatformCompletion *completion,
               i32 fd) {
  assert(platform_linux);
  assert(completion);

  const u32 idx = platform_linux->free_head;
  if (IO_LINUX_OP_NONE == idx) {
    return IO_LINUX_OP_NONE;
  }
  assert(idx < IO_LINUX_OPS_MAX);

  platform_linux->free_head = platform_linux->ops[idx].next;
  platform_linux->ops[idx] = (PlatformLinuxOp){
      .completion = completion, .fd = fd, .next = IO_LINUX_OP_NONE};

  return idx;
}

static void linux_op_free(PlatformLinux *platform_linux, u32 idx) {
  assert(platform_linux);
  assert(idx < IO_LINUX_OPS_MAX);
  assert(platform_linux->ops[idx].completion);

  platform_linux->ops[idx] = (PlatformLinuxOp){
      .completion = NULL, .fd = -1, .next = platform_linux->free_head};
  platform_linux->free_head = idx;
}

// "Try this operation on the next turn of the loop." The whole of what a slot
// does, which is why no slot here touches the operating system.
__attribute__((warn_unused_result)) static Error
linux_submit(PlatformLinux *platform_linux, PlatformCompletion *completion,
             i32 fd) {
  assert(platform_linux);
  assert(completion);
  assert(completion->cb);

  const u32 idx = linux_op_alloc(platform_linux, completion, fd);
  if (IO_LINUX_OP_NONE == idx) {
    // Nothing was submitted. `ErrKindAgain` and not `ErrKindOOM`: the caller is
    // not out of memory, it is early, and the same call works once the loop has
    // been turned.
    return (Error){.kind = ErrKindAgain};
  }

  if (IO_LINUX_OP_NONE == platform_linux->submitted_tail) {
    assert(IO_LINUX_OP_NONE == platform_linux->submitted_head);
    platform_linux->submitted_head = idx;
  } else {
    platform_linux->ops[platform_linux->submitted_tail].next = idx;
  }
  platform_linux->submitted_tail = idx;

  return (Error){.kind = ErrKindNone};
}

// "Say when this descriptor is worth trying again."
//
// A registration is per descriptor, so this cannot be used for two operations on
// one descriptor at a time: the second would replace the first, and the first
// would never be reported. Nothing does that -- a connection has one operation
// in flight and the composites are sequences -- and kqueue would have tolerated
// it, so it is written down here rather than left to be discovered.
__attribute__((warn_unused_result)) static Error
linux_arm(PlatformLinux *platform_linux, u32 idx) {
  assert(platform_linux);
  assert(idx < IO_LINUX_OPS_MAX);

  const PlatformLinuxOp *const op = &platform_linux->ops[idx];
  assert(op->completion);
  assert(op->fd >= 0);
  assert(unix_platform_action_can_wait(op->completion->action.kind));

  u32 events = 0;
  switch (op->completion->action.kind) {
  case PlatformActionKindRead:
  case PlatformActionKindAccept:
    events = EPOLLIN;
    break;

  // A connect that is still going finishes by making the socket writable, so it
  // waits on the same event as a write.
  case PlatformActionKindWrite:
  case PlatformActionKindConnect:
  case PlatformActionKindSendTo:
    events = EPOLLOUT;
    break;

  case PlatformActionKindNone:
  case PlatformActionKindOpen:
  case PlatformActionKindClose:
  case PlatformActionKindFileSize:
  case PlatformActionKindRemoveFile:
    assert(0 && "unreachable");
    break;
  }
  assert(0 != events);

  // `EPOLLONESHOT`: one report per submitted operation, so a registration never
  // outlives the operation that asked for it. It disarms the descriptor rather
  // than removing it, which is what makes the `EEXIST` below ordinary.
  struct epoll_event event = {0};
  event.events = events | EPOLLONESHOT;
  event.data.u32 = idx;

  if (0 == epoll_ctl(platform_linux->epoll_fd, EPOLL_CTL_ADD, op->fd, &event)) {
    platform_linux->armed_count += 1;
    return (Error){.kind = ErrKindNone};
  }

  // The descriptor is in the set already, disarmed by the `EPOLLONESHOT` of the
  // operation before this one. Re-arming it is a modification, not an addition;
  // this is the ordinary path for the second and every later operation on a
  // descriptor, and the reason the set is never explicitly emptied -- closing a
  // descriptor takes it out.
  if (EEXIST == errno) {
    if (-1 ==
        epoll_ctl(platform_linux->epoll_fd, EPOLL_CTL_MOD, op->fd, &event)) {
      return unix_error_from_errno(errno);
    }

    platform_linux->armed_count += 1;
    return (Error){.kind = ErrKindNone};
  }

  return unix_error_from_errno(errno);
}

// One operation's turn: try it, and either run its callback or ask to be told
// when to try again. The only place a callback is ever called from, so "the
// callback runs from inside `run_for_ns`" holds for an operation that was ready
// the moment it was submitted just as much as for one that waited on a peer.
static void linux_advance(PlatformLinux *platform_linux, u32 idx) {
  assert(platform_linux);
  assert(idx < IO_LINUX_OPS_MAX);

  // Copied out: the slot is handed back before the callback runs, and the
  // callback is free to submit again and be given this very slot.
  PlatformCompletion *const completion = platform_linux->ops[idx].completion;
  const i32 fd = platform_linux->ops[idx].fd;
  assert(completion);
  assert(completion->cb);

  usize res = 0;
  Error err = unix_platform_perform(completion, fd, &res);

  if (ErrKindAgain == err.kind &&
      unix_platform_action_can_wait(completion->action.kind)) {
    const Error err_arm = linux_arm(platform_linux, idx);
    if (ErrKindNone == err_arm.kind) {
      // The slot stays: the kernel is holding this operation by its index.
      return;
    }

    // epoll would not watch it. That is this loop's trouble and not the
    // operation's, and the operation has to be answered either way: dropping it
    // would leave whatever submitted it waiting for ever.
    err = err_arm;
    res = 0;
  }

  linux_op_free(platform_linux, idx);
  completion->cb(completion, err, res);
}

__attribute__((warn_unused_result)) static Error
linux_run_for_ns(Platform *platform, usize ns) {
  assert(platform);

  PlatformLinux *const platform_linux = (PlatformLinux *)platform;

  // What was submitted before this turn began. A callback below may submit more;
  // that goes on the list proper and waits for the next turn, so one turn cannot
  // be spun forever by a callback that keeps resubmitting.
  {
    u32 idx = platform_linux->submitted_head;
    platform_linux->submitted_head = IO_LINUX_OP_NONE;
    platform_linux->submitted_tail = IO_LINUX_OP_NONE;

    while (IO_LINUX_OP_NONE != idx) {
      // Read before the slot can be handed back and given to a callback.
      const u32 next = platform_linux->ops[idx].next;
      linux_advance(platform_linux, idx);
      idx = next;
    }
  }

  // Nothing is with the kernel, so a wait could only run the timeout down.
  if (0 == platform_linux->armed_count) {
    return (Error){.kind = ErrKindNone};
  }

  // epoll counts in milliseconds. Rounded up, and never down to zero while
  // there is still time asked for: a zero would turn a wait into a poll and the
  // caller's loop into a spin.
  // TODO: `epoll_pwait2` takes a `timespec`, which would not need this.
  i32 timeout_ms = -1;
  {
    const usize ms = (ns + Millisecond - 1) / Millisecond;
    if (0 == ms) {
      timeout_ms = (0 == ns) ? 0 : 1;
    } else if (ms > (usize)INT32_MAX) {
      timeout_ms = INT32_MAX;
    } else {
      timeout_ms = (i32)ms;
    }
  }

  struct epoll_event events[EPOLL_MAX_EVENTS] = {0};
  const i32 ret = epoll_wait(platform_linux->epoll_fd, events, EPOLL_MAX_EVENTS,
                             timeout_ms);
  if (-1 == ret) {
    // A signal during the wait reported nothing and broke nothing: the next turn
    // waits again.
    if (EINTR == errno) {
      return (Error){.kind = ErrKindNone};
    }
    return unix_error_from_errno(errno);
  }

  assert(ret >= 0);
  assert(ret <= EPOLL_MAX_EVENTS);

  for (i32 i = 0; i < ret; i++) {
    const u32 idx = events[i].data.u32;
    assert(idx < IO_LINUX_OPS_MAX);
    assert(platform_linux->ops[idx].completion);

    // `EPOLLONESHOT`, so this registration is already disarmed whether or not
    // the operation finishes here.
    assert(platform_linux->armed_count > 0);
    platform_linux->armed_count -= 1;

    // `EPOLLERR` and `EPOLLHUP` need nothing of their own: they make the
    // descriptor ready, and the syscall `linux_advance` is about to make reports
    // what actually went wrong. Which is one thing epoll makes easier than
    // kqueue, where a rejected registration comes back as an event.
    linux_advance(platform_linux, idx);
  }

  return (Error){.kind = ErrKindNone};
}

// ---------- The slots ----------
//
// Each one records what was asked for and asks for a turn of the loop. Nothing
// here touches the operating system, so the only thing a slot can report is that
// it had no room to take the operation; what the operation itself has to say
// reaches the callback.

__attribute__((warn_unused_result)) static Error
linux_open(Platform *platform, PlatformCompletion *completion, Bytes path,
           FileOpenOptions options, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action =
      (PlatformAction){.kind = PlatformActionKindOpen,
                       .v.open = {.path = path, .options = options}};

  return linux_submit((PlatformLinux *)platform, completion, -1);
}

__attribute__((warn_unused_result)) static Error
linux_close(Platform *platform, PlatformCompletion *completion, i32 fd,
            PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindClose};

  return linux_submit((PlatformLinux *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
linux_read(Platform *platform, PlatformCompletion *completion, i32 fd,
           Bytes data, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);
  assert(data.data);
  assert(data.len > 0);

  completion->cb = cb;
  completion->action =
      (PlatformAction){.kind = PlatformActionKindRead, .v.read.data = data};

  return linux_submit((PlatformLinux *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
linux_write(Platform *platform, PlatformCompletion *completion, i32 fd,
            Bytes data, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);
  assert(data.data);
  assert(data.len > 0);

  completion->cb = cb;
  completion->action =
      (PlatformAction){.kind = PlatformActionKindWrite, .v.write.data = data};

  return linux_submit((PlatformLinux *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
linux_accept(Platform *platform, PlatformCompletion *completion,
             i32 listen_socket, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindAccept};

  return linux_submit((PlatformLinux *)platform, completion, listen_socket);
}

__attribute__((warn_unused_result)) static Error
linux_connect(Platform *platform, PlatformCompletion *completion, i32 fd,
              Ipv4Addr addr, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindConnect,
                                        .v.connect.addr = addr};

  return linux_submit((PlatformLinux *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
linux_send_to(Platform *platform, PlatformCompletion *completion, i32 fd,
              Ipv4Addr addr, Bytes data, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);
  assert(data.data);
  assert(data.len > 0);

  completion->cb = cb;
  completion->action =
      (PlatformAction){.kind = PlatformActionKindSendTo,
                       .v.send_to = {.addr = addr, .data = data}};

  return linux_submit((PlatformLinux *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
linux_file_size(Platform *platform, PlatformCompletion *completion, i32 fd,
                PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindFileSize};

  return linux_submit((PlatformLinux *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
linux_remove_file(Platform *platform, PlatformCompletion *completion,
                  Bytes path, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindRemoveFile,
                                        .v.remove_file.path = path};

  return linux_submit((PlatformLinux *)platform, completion, -1);
}

__attribute__((warn_unused_result)) static Error
linux_platform_epoll_make(Arena *arena, Platform **dst) {
  assert(arena);
  assert(dst);

  PlatformLinux *const platform_linux =
      arena_alloc(arena, __alignof__(PlatformLinux), sizeof(PlatformLinux), 1);
  if (!platform_linux) {
    return (Error){.kind = ErrKindOOM};
  }
  // The arena does not zero what it hands out, and every index below has to
  // mean something.
  memset(platform_linux, 0, sizeof(*platform_linux));

  // `EPOLL_CLOEXEC`: nothing here execs, but a descriptor that would survive one
  // is the kind of thing that is only ever noticed much later.
  const i32 ret = epoll_create1(EPOLL_CLOEXEC);
  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  platform_linux->epoll_fd = ret;
  platform_linux->submitted_head = IO_LINUX_OP_NONE;
  platform_linux->submitted_tail = IO_LINUX_OP_NONE;

  // Every slot free, chained in order so that the first operations of a run take
  // the first slots.
  platform_linux->free_head = 0;
  for (u32 i = 0; i < IO_LINUX_OPS_MAX; i++) {
    platform_linux->ops[i].completion = NULL;
    platform_linux->ops[i].fd = -1;
    platform_linux->ops[i].next =
        (i + 1 < IO_LINUX_OPS_MAX) ? (i + 1) : IO_LINUX_OP_NONE;
  }

  // The slots that never wait are the ones `env_platform_make` fills in.
  platform_linux->platform = *env_platform_make();
  platform_linux->platform.run_for_ns = linux_run_for_ns;
  platform_linux->platform.monotonic_ns = unix_monotonic_ns;
  platform_linux->platform.open = linux_open;
  platform_linux->platform.close = linux_close;
  platform_linux->platform.read = linux_read;
  platform_linux->platform.write = linux_write;
  platform_linux->platform.accept = linux_accept;
  platform_linux->platform.connect = linux_connect;
  platform_linux->platform.send_to = linux_send_to;
  platform_linux->platform.file_size = linux_file_size;
  platform_linux->platform.remove_file = linux_remove_file;

  *dst = &platform_linux->platform;

  return (Error){.kind = ErrKindNone};
}

// Linux has two answers, and only one of them is written. io_uring is not an
// `#ifdef` inside the epoll backend but a peer of it: the kernel makes the
// syscall there, so nothing of `linux_advance` or `unix_platform_perform`
// applies and the submission ring replaces both the list above and epoll
// itself. It goes here as `linux_io_uring_make`, with the default below picking
// it when the running kernel has it.
__attribute__((warn_unused_result)) static Error
platform_make(Arena *arena, PlatformBackend backend, Platform **dst) {
  assert(arena);
  assert(dst);

  switch (backend) {
  // epoll for now. When io_uring lands this asks the kernel which it has, which
  // is why a caller that does not care should be asking for `Default` rather
  // than naming `Epoll` and being stuck with it.
  case PlatformBackendDefault:
  case PlatformBackendEpoll:
    return linux_platform_epoll_make(arena, dst);

  // TODO: implement.
  case PlatformBackendIoUring:
    return (Error){.kind = ErrKindUnsupported};

  case PlatformBackendKqueue:
  case PlatformBackendIocp:
    return (Error){.kind = ErrKindUnsupported};
  }

  assert(0 && "unreachable");
  return (Error){.kind = ErrKindUnsupported};
}
