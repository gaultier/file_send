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
  assert(&handler->completion == completion);

  log(&handler->logger, LogLevelInfo, "closed");

  pool_release(&handler->server->memory_blocks_pool,
               handler->arena.end - HTTP_HANDLER_ARENA_SIZE);
  pool_release(&handler->server->handler_pool, handler);
}

static void otel_on_write(IoCompletion *completion, Error write_err,
                          usize write_count) {
  assert(completion);
  assert(completion->ctx);

  HttpHandler *const handler = completion->ctx;
  IO *const io = handler->io;
  assert(io);

  (void)write_err;
  (void)write_count;

  (void)io->close(io, &handler->completion, handler->socket,
                  http_handler_on_close);
}

static void otel_on_read(IoCompletion *completion, Error read_err,
                         usize read_count) {
  assert(completion);
  assert(completion->ctx);

  HttpHandler *const handler = completion->ctx;
  IO *const io = handler->io;
  assert(io);

  if (ErrKindNone != read_err.kind) {
    log(&handler->logger, LogLevelError, "read error: %s",
        error_kind_to_cstr(read_err.kind));
    (void)handler->io->close(handler->io, &handler->completion, handler->socket,
                             http_handler_on_close);
    return;
  }

  if (0 == read_count) {
    log(&handler->logger, LogLevelDebug, "closing connection: 0 bytes read");
    (void)handler->io->close(handler->io, &handler->completion, handler->socket,
                             http_handler_on_close);
    return;
  }

  assert(!__builtin_add_overflow(handler->recv.len, read_count,
                                 &handler->recv.len));
  log(&handler->logger, LogLevelDebug, "read %zu bytes", read_count);
  fwrite(handler->recv.container.data, 1, handler->recv.len, stdout);
  puts("");

  const Bytes recv = bytes_buffer_to_bytes(handler->recv);
  const Find find = http_find_headers_end(recv);
  if (!find.found) {
    Error err = io->read(io, &handler->completion, handler->socket,
                         bytes_buffer_space_bytes(handler->recv), otel_on_read);
    if (ErrKindNone != err.kind) {
      log_err(&handler->logger, "failed to read", err);
      (void)io->env->close_socket(io->env, handler->socket);
      return;
    }
  }

  Bytes headers = bytes_take(recv, find.idx);
  HttpRequestStatusLine sl = {0};
  usize advanced = 0;
  Error err = http_parse_req_status_line(headers, &sl, &advanced);
  if (ErrKindNone != err.kind) {
    log_err(&handler->logger, "failed to parse http status line", err);
    (void)io->env->close_socket(io->env, handler->socket);
    return;
  }

  bytes_advance(&headers, advanced);
  fwrite(headers.data, 1, headers.len, stdout);
  puts("");

  err = http_parse_headers(headers, handler->headers, &handler->headers_len,
                           handler->headers_cap);
  if (ErrKindNone != read_err.kind) {
    log_err(&handler->logger, "failed to parse http headers", err);
    (void)io->env->close_socket(io->env, handler->socket);
    return;
  }

  log(&handler->logger, LogLevelDebug, "parsed %zu HTTP headers",
      handler->headers_len);

  // FIXME
  assert(ErrKindNone ==
         bytes_buffer_make(128, &handler->arena, &handler->resp).kind);
  assert(bytes_buffer_extend_within_cap(
      &handler->resp,
      bytes_from_cstr("HTTP/1.1 200\r\nConnection:Close\r\n\r\nHello")));

  err = io->write(io, &handler->completion, handler->socket,
                  bytes_buffer_to_bytes(handler->resp), otel_on_write);
  if (ErrKindNone != read_err.kind) {
    log_err(&handler->logger, "failed to write", err);
    (void)io->env->close_socket(io->env, handler->socket);
    return;
  }
}

static void otel_on_accept(IO *io, void *vctx, Ipv4Addr accept_addr,
                           i32 accept_socket) {
  assert(io);
  assert(vctx);
  HttpServer *const server = vctx;

  HttpHandler *const handler = pool_acquire(&server->handler_pool);
  if (!handler) {
    log(&server->logger, LogLevelError,
        "backpressure: no available pool slot for request");
    (void)io->env->close_socket(io->env, accept_socket);
    return;
  }

  http_handler_init(handler, server, io, accept_addr, accept_socket);

  log(&handler->logger, LogLevelInfo, "accepted");

  Error err = io->read(io, &handler->completion, accept_socket,
                       bytes_buffer_space_bytes(handler->recv), otel_on_read);
  if (ErrKindNone != err.kind) {
    log_err(&server->logger, "failed to read", err);
    (void)io->env->close_socket(io->env, accept_socket);
    return;
  }
}
