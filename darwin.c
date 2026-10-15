#pragma once

#include "lib.c"
#include "unix.c"

#include <sys/event.h>

// How many reports one turn of the loop takes. More than that and the rest wait
// for the next turn, which is the same thing that happens to anything submitted
// while the callbacks are running.
#define KQUEUE_MAX_EVENTS 1024

// The Darwin `Platform`: one kqueue, one thread, and no queue of its own.
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
  // Must be first: an `PlatformDarwin *` is a `Platform *`, which is what lets
  // a slot reached through the vtable find its way back to this. This does not
  // use `ctx` for the same reason -- the implementation is the object, and a
  // pointer from it to itself would be one more thing to keep in step.
  Platform platform;

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
} PlatformDarwin;

// Append one change. The only way anything reaches the kernel, so it is also
// the one place the count of what is in flight goes up.
__attribute__((warn_unused_result)) static Error
darwin_change_push(PlatformDarwin *platform_darwin, uintptr_t ident, i16 filter,
                   u32 fflags, i64 data, PlatformCompletion *completion) {
  assert(platform_darwin);
  assert(completion);

  const usize cap = sizeof(platform_darwin->changelist) /
                    sizeof(platform_darwin->changelist[0]);
  if (platform_darwin->changelist_len >= cap) {
    // Nothing was submitted. `ErrKindAgain` and not `ErrKindOOM`: the caller is
    // not out of memory, it is early, and the same call works once the loop has
    // been turned.
    return (Error){.kind = ErrKindAgain};
  }

  struct kevent *const change =
      &platform_darwin->changelist[platform_darwin->changelist_len];
  platform_darwin->changelist_len += 1;

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

  platform_darwin->in_flight += 1;

  return (Error){.kind = ErrKindNone};
}

// "Try this operation on the next turn of the loop, on this descriptor." Keyed
// on the completion, with the descriptor in `data`, for the reasons in the
// comment on `PlatformDarwin`. `fd` is -1 for an operation that has none.
__attribute__((warn_unused_result)) static Error
darwin_submit(PlatformDarwin *platform_darwin, PlatformCompletion *completion,
              i32 fd) {
  assert(platform_darwin);
  assert(completion);
  assert(completion->cb);

  return darwin_change_push(platform_darwin, (uintptr_t)completion, EVFILT_USER,
                            NOTE_TRIGGER, (i64)fd, completion);
}

// "Say when this descriptor is worth trying again."
__attribute__((warn_unused_result)) static Error
darwin_arm(PlatformDarwin *platform_darwin, PlatformCompletion *completion,
           i32 fd) {
  assert(platform_darwin);
  assert(completion);
  assert(fd >= 0);
  assert(unix_platform_action_can_wait(completion->action.kind));

  i16 filter = 0;
  switch (completion->action.kind) {
  case PlatformActionKindRead:
  case PlatformActionKindAccept:
    filter = EVFILT_READ;
    break;

  // A connect that is still going finishes by making the socket writable, so
  // it waits on the same filter as a write.
  case PlatformActionKindWrite:
  case PlatformActionKindConnect:
  case PlatformActionKindSendTo:
    filter = EVFILT_WRITE;
    break;

  case PlatformActionKindNone:
  case PlatformActionKindOpen:
  case PlatformActionKindClose:
  case PlatformActionKindFileSize:
  case PlatformActionKindRemoveFile:
    assert(0 && "unreachable");
    break;
  }
  assert(0 != filter);

  // Keyed on the descriptor, which is what a readiness event reports back, so
  // `data` has nothing to carry here.
  return darwin_change_push(platform_darwin, (uintptr_t)(u32)fd, filter, 0, 0,
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
static void darwin_advance(PlatformDarwin *platform_darwin,
                           PlatformCompletion *completion, i32 fd) {
  assert(platform_darwin);
  assert(completion);
  assert(completion->cb);

  usize res = 0;
  Error err = unix_platform_perform(completion, fd, &res);

  if (ErrKindAgain == err.kind &&
      unix_platform_action_can_wait(completion->action.kind)) {
    const Error err_arm = darwin_arm(platform_darwin, completion, fd);
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

__attribute__((warn_unused_result)) static Error
darwin_run_for_ns(Platform *platform, usize ns) {
  assert(platform);

  PlatformDarwin *const platform_darwin = (PlatformDarwin *)platform;

  // Nothing submitted and nothing outstanding, so a wait could only run the
  // timeout down. Returning instead leaves the caller to decide what to do with
  // the time.
  if (0 == platform_darwin->in_flight) {
    assert(0 == platform_darwin->changelist_len);
    return (Error){.kind = ErrKindNone};
  }

  struct kevent events[KQUEUE_MAX_EVENTS] = {0};
  const struct timespec timeout = {
      .tv_sec = (time_t)(ns / Second),
      .tv_nsec = (long)(ns % Second),
  };

  assert(platform_darwin->changelist_len <= INT_MAX);
  const i32 ret =
      kevent(platform_darwin->kqueue_fd, platform_darwin->changelist,
             (i32)platform_darwin->changelist_len, events, KQUEUE_MAX_EVENTS,
             &timeout);

  // The changes were applied whatever the wait went on to do, and a change
  // applied twice would be a second registration for an operation that has
  // one. Cleared before the callbacks below run, so that what they submit
  // accumulates for the next turn rather than being sent again with it.
  platform_darwin->changelist_len = 0;

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

    PlatformCompletion *const completion = event->udata;
    assert(completion);
    assert(completion->cb);

    // `EV_ONESHOT`, so the registration this event came from is already gone,
    // whether or not the operation finishes here.
    assert(platform_darwin->in_flight > 0);
    platform_darwin->in_flight -= 1;

    // A change the kernel would not accept comes back as an event rather than
    // as a failed call, with the reason in `data`: a descriptor that was closed
    // under the operation is the ordinary way to get one. The operation is over
    // either way, so its callback gets the reason.
    if (event->flags & EV_ERROR) {
      assert(event->data > 0);
      completion->cb(completion, unix_error_from_errno((i32)event->data), 0);
      continue;
    }

    darwin_advance(platform_darwin, completion, darwin_event_fd(event));
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
darwin_open_async(Platform *platform, PlatformCompletion *completion,
                  Bytes path, FileOpenOptions options, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action =
      (PlatformAction){.kind = PlatformActionKindOpen,
                       .v.open = {.path = path, .options = options}};

  return darwin_submit((PlatformDarwin *)platform, completion, -1);
}

__attribute__((warn_unused_result)) static Error
darwin_close_async(Platform *platform, PlatformCompletion *completion, i32 fd,
                   PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindClose};

  return darwin_submit((PlatformDarwin *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_read_async(Platform *platform, PlatformCompletion *completion, i32 fd,
                  Bytes data, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);
  assert(data.data);
  assert(data.len > 0);

  completion->cb = cb;
  completion->action =
      (PlatformAction){.kind = PlatformActionKindRead, .v.read.data = data};

  return darwin_submit((PlatformDarwin *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_write_async(Platform *platform, PlatformCompletion *completion, i32 fd,
                   Bytes data, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);
  assert(data.data);
  assert(data.len > 0);

  completion->cb = cb;
  completion->action =
      (PlatformAction){.kind = PlatformActionKindWrite, .v.write.data = data};

  return darwin_submit((PlatformDarwin *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_accept_async(Platform *platform, PlatformCompletion *completion,
                    i32 listen_socket, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindAccept};

  return darwin_submit((PlatformDarwin *)platform, completion, listen_socket);
}

__attribute__((warn_unused_result)) static Error
darwin_connect_async(Platform *platform, PlatformCompletion *completion, i32 fd,
                     Ipv4Addr addr, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindConnect,
                                        .v.connect.addr = addr};

  return darwin_submit((PlatformDarwin *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_send_to_async(Platform *platform, PlatformCompletion *completion, i32 fd,
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

  return darwin_submit((PlatformDarwin *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_file_size_async(Platform *platform, PlatformCompletion *completion,
                       i32 fd, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindFileSize};

  return darwin_submit((PlatformDarwin *)platform, completion, fd);
}

__attribute__((warn_unused_result)) static Error
darwin_remove_file_async(Platform *platform, PlatformCompletion *completion,
                         Bytes path, PlatformCallback cb) {
  assert(platform);
  assert(completion);
  assert(cb);

  completion->cb = cb;
  completion->action = (PlatformAction){.kind = PlatformActionKindRemoveFile,
                                        .v.remove_file.path = path};

  return darwin_submit((PlatformDarwin *)platform, completion, -1);
}

__attribute__((warn_unused_result)) static Error
darwin_platform_kqueue_make(Arena *arena, Platform **dst) {
  assert(arena);
  assert(dst);

  PlatformDarwin *const platform_darwin = arena_alloc(
      arena, __alignof__(PlatformDarwin), sizeof(PlatformDarwin), 1);
  if (!platform_darwin) {
    return (Error){.kind = ErrKindOOM};
  }
  // The arena does not zero what it hands out, and an empty changelist is a
  // length of zero.
  memset(platform_darwin, 0, sizeof(*platform_darwin));

  const i32 ret = kqueue();
  if (-1 == ret) {
    return unix_error_from_errno(errno);
  }

  platform_darwin->kqueue_fd = ret;
  // The slots that never wait are the ones `env_platform_make` fills in.
  platform_darwin->platform = *env_platform_make();
  platform_darwin->platform.run_for_ns = darwin_run_for_ns;
  platform_darwin->platform.monotonic_ns = unix_monotonic_ns;
  platform_darwin->platform.open_async = darwin_open_async;
  platform_darwin->platform.close_async = darwin_close_async;
  platform_darwin->platform.read_async = darwin_read_async;
  platform_darwin->platform.write_async = darwin_write_async;
  platform_darwin->platform.accept_async = darwin_accept_async;
  platform_darwin->platform.connect_async = darwin_connect_async;
  platform_darwin->platform.send_to_async = darwin_send_to_async;
  platform_darwin->platform.file_size_async = darwin_file_size_async;
  platform_darwin->platform.remove_file_async = darwin_remove_file_async;

  *dst = &platform_darwin->platform;

  return (Error){.kind = ErrKindNone};
}

// Darwin has the one backend, so the choice is between asking for it and asking
// for it by name. A Linux backend named here would be a lie, and a caller that
// asked for one gets told so rather than silently getting kqueue.
__attribute__((warn_unused_result)) static Error
platform_make(Arena *arena, PlatformBackend backend, Platform **dst) {
  assert(arena);
  assert(dst);

  switch (backend) {
  case PlatformBackendDefault:
  case PlatformBackendKqueue:
    return darwin_platform_kqueue_make(arena, dst);

  case PlatformBackendEpoll:
  case PlatformBackendIoUring:
  case PlatformBackendIocp:
    return (Error){.kind = ErrKindUnsupported};
  }

  assert(0 && "unreachable");
  return (Error){.kind = ErrKindUnsupported};
}
