#pragma once

#include "http.c"
#include "log.c"

// FIXME
#define HTTP_INFLIGHT_REQUESTS_MAX (1024)

typedef u64 HttpPoolSlotGroup;

// Unit is bits.
#define HTTP_POOL_SLOTS_PER_GROUP (sizeof(HttpPoolSlotGroup) * 8)
_Static_assert(0 == (HTTP_INFLIGHT_REQUESTS_MAX % HTTP_POOL_SLOTS_PER_GROUP),
               "must be a multiple");

#define HTTP_POOL_SLOT_GROUPS                                                  \
  (HTTP_INFLIGHT_REQUESTS_MAX / HTTP_POOL_SLOTS_PER_GROUP)

typedef struct HttpServer HttpServer;

typedef struct {
  Logger logger;
  i32 socket;
  Arena arena;
  IO *io;
  HttpServer *server;
  IoCompletion completion_close;
} HttpHandler;

typedef struct {
  // Bitset.
  // Bit `i` of group `g` means: `slots[g * POOL_SLOTS_PER_GROUP + i]` is
  // occupied.
  HttpPoolSlotGroup occupied[HTTP_POOL_SLOT_GROUPS];
  HttpHandler slots[HTTP_INFLIGHT_REQUESTS_MAX];
} HttpHandlerPool;

struct HttpServer {
  u32 log_level_mask;
  HttpHandlerPool pool;
};

__attribute__((warn_unused_result)) static HttpHandler *
http_handler_pool_acquire(HttpHandlerPool *pool) {
  assert(pool);

  for (usize i = 0; i < HTTP_POOL_SLOT_GROUPS; i++) {
    const HttpPoolSlotGroup slot_group = pool->occupied[i];

    const i32 first_unset_bit = __builtin_ffsll((i64)~slot_group);

    // This group is full; there may be a free slot in a later one.
    if (0 == first_unset_bit) {
      continue;
    }

    const u32 bit_idx = (u32)(first_unset_bit - 1);
    const HttpPoolSlotGroup mask = 1ULL << bit_idx;

    // The read above is still good: one thread runs the loop, and between that
    // read and this write there is nothing for it to have been doing but this.
    assert(0 == (pool->occupied[i] & mask));
    pool->occupied[i] |= mask;

    const usize slot_idx = i * HTTP_POOL_SLOTS_PER_GROUP + bit_idx;
    assert(slot_idx < HTTP_INFLIGHT_REQUESTS_MAX);

    HttpHandler *res = &pool->slots[slot_idx];
    assert(0 == res->io);
    assert(0 == res->socket);
    return res;
  }

  return NULL;
}

static void http_handler_init(HttpHandler *handler, u32 log_level_mask,
                              Ipv4Addr addr, i32 socket) {
  assert(handler);

  char log_prefix[32] = {0};
  const u32 ip = addr.ip;

  snprintf(log_prefix, sizeof(log_prefix), "[peer %u.%u.%u.%u:%hu] ",
           ip >> 24 & 0xff, ip >> 16 & 0xff, ip >> 8 & 0xff, ip >> 0 & 0xff,
           addr.port);

  handler->logger = logger_make(log_level_mask, bytes_from_cstr(log_prefix));
  handler->socket = socket;
}

static void http_handler_pool_release(HttpHandlerPool *pool,
                                      HttpHandler *slot) {
  assert(pool);
  assert(slot);

  assert(slot >= pool->slots);
  const usize slot_idx = (usize)(slot - pool->slots);
  assert(slot_idx < HTTP_INFLIGHT_REQUESTS_MAX);

  const usize slot_group_idx = slot_idx / HTTP_POOL_SLOTS_PER_GROUP;
  assert(slot_group_idx < HTTP_POOL_SLOT_GROUPS);
  const u32 bit_idx = slot_idx % HTTP_POOL_SLOTS_PER_GROUP;

  // We are still the owner so we are responsible for zeroing it.
  memset(slot, 0, sizeof(*slot));

  const HttpPoolSlotGroup mask = 1ULL << bit_idx;

  // Sanity check against double release of the same slot: it was occupied.
  assert(0 != (pool->occupied[slot_group_idx] & mask));
  pool->occupied[slot_group_idx] &= ~mask;
}

static void http_handler_on_close(IoCompletion *completion, Error err,
                                  usize res) {
  assert(completion);
  (void)err;
  (void)res;

  HttpHandler *const handler = completion->ctx;
  // http_handler_assert_invariants(handler);
  assert(&handler->completion_close == completion);

  log(&handler->logger, LogLevelInfo, "closed");

  http_handler_pool_release(&handler->server->pool, handler);
}

static void otel_on_accept(IO *io, void *vctx, Ipv4Addr accept_addr,
                           i32 accept_socket) {
  assert(io);
  assert(vctx);
  HttpServer *const server = vctx;

  HttpHandler *const handler = http_handler_pool_acquire(&server->pool);
  if (!handler) {
    fprintf(stderr, "backpressure: no available pool slot for request\n");
    (void)io->env->close_socket(io->env, accept_socket);
    return;
  }

  http_handler_init(handler, server->log_level_mask, accept_addr,
                    accept_socket);

  log(&handler->logger, LogLevelInfo, "accepted");

  const Error err = io->close(io, &handler->completion_close, handler->socket,
                              http_handler_on_close);
  if (ErrKindNone != err.kind) {
    log_err(&handler->logger, "failed to hang up", err);
    (void)io->env->close_socket(io->env, handler->socket);
    http_handler_pool_release(&server->pool, handler);
  }
}
