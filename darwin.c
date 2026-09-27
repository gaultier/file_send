#pragma once

#include "lib.c"
#include "unix.c"

#include <sys/event.h>

// How many reports one turn of the loop takes. More than that and the rest wait
// for the next turn, which is the same thing that happens to anything submitted
// while the callbacks are running.
#define KQUEUE_MAX_EVENTS 1024

#define NS_PER_SEC (1000 * 1000 * 1000)

// The Darwin `IO`: one kqueue, one thread, and no queue of its own.
//
// A submitted operation is not tried in the call that submits it -- a callback
// would then run nested inside whatever submitted the operation, and a chain of
// ready operations would recurse as deep as it is long. It is not put on a list
// here either: `kevent` takes a list of changes and a list of events in the
// same call, so the changes waiting to be made *are* the queue, and the kernel
// hands each one back with the completion's own address attached.
//
// Everything therefore goes in twice over, and the two entries mean different
// things:
//
//   - `EVFILT_USER`, keyed on the completion, triggered as it is added: "try
//     this operation on the next turn of the loop". Every submission starts
//     here, whether or not kqueue could have watched it, which is what lets a
//     read of a file and a read of a socket be one operation to a caller. It is
//     also the only thing that works for a connect, which has to be started
//     before there is anything to wait for. It cannot be keyed on the
//     descriptor: an `open` has none yet, an `unlink` never has one, and two
//     operations on one descriptor would collide in kqueue's `(ident, filter)`
//     key space.
//
//   - `EVFILT_READ` or `EVFILT_WRITE`, keyed on the descriptor, added only
//     after the operation answered `EAGAIN`: "say when it is worth trying
//     again". Which is why every descriptor that gets here is non-blocking: a
//     blocking one would not answer `EAGAIN`, it would stop the one thread that
//     runs the loop.
//
// Between them those two carry the descriptor, so no completion has to: a
// readiness event is keyed on it, and a user event is handed it in `data`,
// which kqueue returns untouched. Which of the two a descriptor came out of is
// what `darwin_event_fd` answers.
typedef struct {
  // Must be first: an `IoDarwin *` is an `IO *`, which is what lets a slot
  // reached through the vtable find its way back to this. `IO` has no `ctx`
  // field for the same reason -- the implementation is the object, and a
  // pointer from it to itself would be one more thing to keep in step.
  IO io;

  i32 kqueue_fd;

  // Changes not yet handed to the kernel. A slot appends and returns; the next
  // `run_for_ns` passes the lot to `kevent` alongside the wait, so submitting
  // any number of operations costs no syscall of its own.
  //
  // Twice the event list, because a full event list can re-arm every operation
  // it reported and a listener can submit one more on top.
  struct kevent changelist[2 * KQUEUE_MAX_EVENTS];
  usize changelist_len;

  // Operations the kernel is holding: submitted, not yet reported. A wait with
  // none of them can only run the timeout down, so the loop does not make one.
  usize in_flight;
} IoDarwin;


// Append one change. The only way anything reaches the kernel, so it is also
// the one place the count of what is in flight goes up.
__attribute__((warn_unused_result)) static Error
darwin_change_push(IoDarwin *io_darwin, uintptr_t ident, i16 filter, u32 fflags,
                   i64 data, IoCompletion *completion) {
  assert(io_darwin);
  assert(completion);

  const usize cap =
      sizeof(io_darwin->changelist) / sizeof(io_darwin->changelist[0]);
  if (io_darwin->changelist_len >= cap) {
    // Nothing was submitted. `ErrKindAgain` and not `ErrKindOOM`: the caller is
    // not out of memory, it is early, and the same call works once the loop has
    // been turned.
    return (Error){.kind = ErrKindAgain};
  }

  struct kevent *const change = &io_darwin->changelist[io_darwin->changelist_len];
  io_darwin->changelist_len += 1;

  *change = (struct kevent){0};
  change->ident = ident;
  change->filter = filter;
  // `EV_ONESHOT`: one report per submitted operation, so a registration never
  // outlives the completion that asked for it. That matters because `udata` is
  // that completion, and the caller is free to reuse it the moment its callback
  // has run.
  change->flags = EV_ADD | EV_ONESHOT;
  change->fflags = fflags;
  change->data = data;
  change->udata = completion;

  io_darwin->in_flight += 1;

  return (Error){.kind = ErrKindNone};
}

// "Try this operation on the next turn of the loop, on this descriptor." Keyed
// on the completion, with the descriptor in `data`, for the reasons in the
// comment on `IoDarwin`. `fd` is -1 for an operation that has none.
__attribute__((warn_unused_result)) static Error
darwin_submit(IoDarwin *io_darwin, IoCompletion *completion, i32 fd) {
  assert(io_darwin);
  assert(completion);
  assert(completion->cb);

  return darwin_change_push(io_darwin, (uintptr_t)completion, EVFILT_USER,
                            NOTE_TRIGGER, (i64)fd, completion);
}

// "Say when this descriptor is worth trying again."
__attribute__((warn_unused_result)) static Error
darwin_arm(IoDarwin *io_darwin, IoCompletion *completion, i32 fd) {
  assert(io_darwin);
  assert(completion);
  assert(fd >= 0);
  assert(unix_io_action_can_wait(completion->action.kind));

  i16 filter = 0;
  switch (completion->action.kind) {
  case IoActionKindRead:
  case IoActionKindAccept:
    filter = EVFILT_READ;
    break;

  // A connect that is still going finishes by making the socket writable, so
  // it waits on the same filter as a write.
  case IoActionKindWrite:
  case IoActionKindConnect:
  case IoActionKindSendTo:
    filter = EVFILT_WRITE;
    break;

  case IoActionKindNone:
  case IoActionKindOpen:
  case IoActionKindClose:
  case IoActionKindFileSize:
  case IoActionKindRemoveFile:
    assert(0 && "unreachable");
    break;
  }
  assert(0 != filter);

  // Keyed on the descriptor, which is what a readiness event reports back, so
  // `data` has nothing to carry here.
  return darwin_change_push(io_darwin, (uintptr_t)(u32)fd, filter, 0, 0,
                            completion);
}

// Which descriptor an event is about. A readiness event is keyed on it; a user
// event is keyed on the completion and was handed the descriptor in `data`.
__attribute__((warn_unused_result)) static i32
darwin_event_fd(const struct kevent *event) {
  assert(event);

  if (EVFILT_USER == event->filter) {
    return (i32)event->data;
  }

  return (i32)event->ident;
}


// One report from the kernel: try the operation, and either run its callback or
// ask to be told when to try again. The only place a callback is ever called
// from, so "the callback runs from inside `run_for_ns`" holds for an operation
// that was ready the moment it was submitted just as much as for one that
// waited on a peer.
static void darwin_advance(IoDarwin *io_darwin, IoCompletion *completion,
                           i32 fd) {
  assert(io_darwin);
  assert(completion);
  assert(completion->cb);

  usize res = 0;
  Error err = unix_io_perform(completion, fd, &res);

  if (ErrKindAgain == err.kind &&
      unix_io_action_can_wait(completion->action.kind)) {
    const Error err_arm = darwin_arm(io_darwin, completion, fd);
    if (ErrKindNone == err_arm.kind) {
      return;
    }

    // There was no room to wait. That is this loop's trouble and not the
    // operation's, and the operation has to be answered either way: dropping it
    // would leave whatever submitted it waiting for ever.
    err = err_arm;
    res = 0;
  }

  completion->cb(completion, err, res);
}

__attribute__((warn_unused_result)) static Error darwin_run_for_ns(IO *io,
                                                                  usize ns) {
  assert(io);

  IoDarwin *const io_darwin = (IoDarwin *)io;

  // Nothing submitted and nothing outstanding, so a wait could only run the
  // timeout down. Returning instead leaves the caller to decide what to do with
  // the time.
  if (0 == io_darwin->in_flight) {
    assert(0 == io_darwin->changelist_len);
    return (Error){.kind = ErrKindNone};
  }

  struct kevent events[KQUEUE_MAX_EVENTS] = {0};
  const struct timespec timeout = {
      .tv_sec = (time_t)(ns / NS_PER_SEC),
      .tv_nsec = (long)(ns % NS_PER_SEC),
  };

  assert(io_darwin->changelist_len <= INT_MAX);
  const i32 ret =
      kevent(io_darwin->kqueue_fd, io_darwin->changelist,
             (i32)io_darwin->changelist_len, events, KQUEUE_MAX_EVENTS, &timeout);

  // The changes were applied whatever the wait went on to do, and a change
  // applied twice would be a second registration for an operation that has
  // one. Cleared before the callbacks below run, so that what they submit
  // accumulates for the next turn rather than being sent again with it.
  io_darwin->changelist_len = 0;

  if (-1 == ret) {
    // A signal during the wait reported nothing and broke nothing: the next
    // turn waits again.
    if (EINTR == errno) {
      return (Error){.kind = ErrKindNone};
    }
    return unix_error_from_errno(errno);
  }

  assert(ret >= 0);
  assert(ret <= KQUEUE_MAX_EVENTS);

  for (i32 i = 0; i < ret; i++) {
    const struct kevent *const event = &events[i];

    IoCompletion *const completion = event->udata;
    assert(completion);
    assert(completion->cb);

    // `EV_ONESHOT`, so the registration this event came from is already gone,
    // whether or not the operation finishes here.
    assert(io_darwin->in_flight > 0);
    io_darwin->in_flight -= 1;

    // A change the kernel would not accept comes back as an event rather than
    // as a failed call, with the reason in `data`: a descriptor that was closed
    // under the operation is the ordinary way to get one. The operation is over
    // either way, so its callback gets the reason.
    if (event->flags & EV_ERROR) {
      assert(event->data > 0);
      completion->cb(completion, unix_error_from_errno((i32)event->data), 0);
      continue;
    }

    darwin_advance(io_darwin, completion, darwin_event_fd(event));
  }

  return (Error){.kind = ErrKindNone};
}

// ---------- The slots ----------
//
// Each one records what was asked for and asks for a turn of the loop. Nothing
// here touches the operating system, so the only thing a slot can report is
// that it had no room to take the operation; what the operation itself has to
// say reaches the callback.

__attribute__((warn_unused_result)) static Error
darwin_open(IO *io, IoCompletion *completion, Slice_u8 path,
            FileOpenOptions options, IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (IoAction){.kind = IoActionKindOpen,
                                  .v.open = {.path = path, .options = options}};

  return darwin_submit((IoDarwin *)io, completion, -1);
}

__attribute__((warn_unused_result)) static Error
darwin_close(IO *io, IoCompletion *completion, i32 fd, IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (IoAction){.kind = IoActionKindClose};

  return darwin_submit((IoDarwin *)io, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_read(IO *io, IoCompletion *completion, i32 fd, Slice_u8 data,
            IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);
  assert(data.data);
  assert(data.len > 0);

  completion->cb = cb;
  completion->action = (IoAction){.kind = IoActionKindRead, .v.read.data = data};

  return darwin_submit((IoDarwin *)io, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_write(IO *io, IoCompletion *completion, i32 fd, Slice_u8 data,
             IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);
  assert(data.data);
  assert(data.len > 0);

  completion->cb = cb;
  completion->action =
      (IoAction){.kind = IoActionKindWrite, .v.write.data = data};

  return darwin_submit((IoDarwin *)io, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_accept(IO *io, IoCompletion *completion, i32 listen_socket,
              IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (IoAction){.kind = IoActionKindAccept};

  return darwin_submit((IoDarwin *)io, completion, listen_socket);
}

__attribute__((warn_unused_result)) static Error
darwin_connect(IO *io, IoCompletion *completion, i32 fd, Ipv4Addr addr,
               IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action =
      (IoAction){.kind = IoActionKindConnect, .v.connect.addr = addr};

  return darwin_submit((IoDarwin *)io, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_send_to(IO *io, IoCompletion *completion, i32 fd, Ipv4Addr addr,
               Slice_u8 data, IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);
  assert(data.data);
  assert(data.len > 0);

  completion->cb = cb;
  completion->action = (IoAction){.kind = IoActionKindSendTo,
                                  .v.send_to = {.addr = addr, .data = data}};

  return darwin_submit((IoDarwin *)io, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_file_size(IO *io, IoCompletion *completion, i32 fd, IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (IoAction){.kind = IoActionKindFileSize};

  return darwin_submit((IoDarwin *)io, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_remove_file(IO *io, IoCompletion *completion, Slice_u8 path,
                   IoCallback cb) {
  assert(io);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action =
      (IoAction){.kind = IoActionKindRemoveFile, .v.remove_file.path = path};

  return darwin_submit((IoDarwin *)io, completion, -1);
}

__attribute__((warn_unused_result)) static Error
darwin_io_kqueue_make(Arena *arena, const Env *env, IO **dst) {
  assert(arena);
  assert(env);
  assert(dst);

  IoDarwin *const io =
      arena_alloc(arena, __alignof__(IoDarwin), sizeof(IoDarwin), 1);
  if (!io) {
    return (Error){.kind = ErrKindOOM};
  }
  // The arena does not zero what it hands out, and an empty changelist is a
  // length of zero.
  memset(io, 0, sizeof(*io));

  const i32 ret = kqueue();
  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  io->kqueue_fd = ret;
  io->io = (IO){
      .env = env,
      .run_for_ns = darwin_run_for_ns,
      .open = darwin_open,
      .close = darwin_close,
      .read = darwin_read,
      .write = darwin_write,
      .accept = darwin_accept,
      .connect = darwin_connect,
      .send_to = darwin_send_to,
      .file_size = darwin_file_size,
      .remove_file = darwin_remove_file,
  };

  *dst = &io->io;

  return (Error){.kind = ErrKindNone};
}

// Darwin has the one backend, so the choice is between asking for it and asking
// for it by name. A Linux backend named here would be a lie, and a caller that
// asked for one gets told so rather than silently getting kqueue.
__attribute__((warn_unused_result)) static Error
io_platform_make(Arena *arena, const Env *env, IoBackend backend, IO **dst) {
  assert(arena);
  assert(env);
  assert(dst);

  switch (backend) {
  case IoBackendDefault:
  case IoBackendKqueue:
    return darwin_io_kqueue_make(arena, env, dst);

  case IoBackendEpoll:
  case IoBackendIoUring:
  case IoBackendIocp:
    return (Error){.kind = ErrKindUnsupported};
  }

  assert(0 && "unreachable");
  return (Error){.kind = ErrKindUnsupported};
}
