#pragma once

#include "http.c"

static void http_handler_on_close(IoCompletion *completion, Error err,
                                  usize res) {
  assert(completion);
  (void)err;
  (void)res;

  HttpHandler *const handler = completion->ctx;
  assert(handler);

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

  http_handler_init(handler, server, accept_addr, accept_socket);

  log(&handler->logger, LogLevelInfo, "accepted");

  const Error err = io->close(io, &handler->completion_close, handler->socket,
                              http_handler_on_close);
  if (ErrKindNone != err.kind) {
    log_err(&handler->logger, "failed to hang up", err);
    (void)io->env->close_socket(io->env, handler->socket);
    http_handler_pool_release(&server->pool, handler);
  }
}
