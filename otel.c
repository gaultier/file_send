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

  http_handler_release(handler);
}

// Ends the connection. `handler` is not to be used after.
static void otel_handler_close(HttpHandler *handler) {
  assert(handler);
  IO *const io = handler->io;
  assert(io);

  const Error err = io->close(io, &handler->completion, handler->socket,
                              http_handler_on_close);
  if (ErrKindNone == err.kind) {
    return;
  }

  // The only release outside `http_handler_on_close`: no callback is coming,
  // so clean up now.
  log_err(&handler->logger, "failed to submit close", err);
  (void)io->env->close_socket(io->env, handler->socket);
  http_handler_release(handler);
}

static void otel_on_write(IoCompletion *completion, Error write_err,
                          usize write_count) {
  assert(completion);
  assert(completion->ctx);

  HttpHandler *const handler = completion->ctx;

  if (ErrKindNone != write_err.kind) {
    log_err(&handler->logger, "write error", write_err);
  }
  (void)write_count;

  otel_handler_close(handler);
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
    otel_handler_close(handler);
    return;
  }

  if (0 == read_count) {
    log(&handler->logger, LogLevelDebug, "closing connection: 0 bytes read");
    otel_handler_close(handler);
    return;
  }

  assert(!__builtin_add_overflow(handler->recv.len, read_count,
                                 &handler->recv.len));
  assert(handler->recv.len <= handler->recv.container.len);
  log(&handler->logger, LogLevelDebug, "read %zu bytes", read_count);
  fwrite(handler->recv.container.data, 1, handler->recv.len, stdout);
  puts("");

  const Bytes recv = bytes_buffer_to_bytes(handler->recv);
  const Find find = http_find_headers_end(recv);
  if (!find.found) {
    // The headers do not fit.
    if (0 == bytes_buffer_space(handler->recv)) {
      log(&handler->logger, LogLevelError, "headers too big");
      otel_handler_close(handler);
      return;
    }

    const Error err =
        io->read(io, &handler->completion, handler->socket,
                 bytes_buffer_space_bytes(handler->recv), otel_on_read);
    if (ErrKindNone != err.kind) {
      log_err(&handler->logger, "failed to read", err);
      otel_handler_close(handler);
    }
    return;
  }
  assert(find.idx <= recv.len);

  Bytes headers = bytes_take(recv, find.idx);
  HttpRequestStatusLine sl = {0};
  usize advanced = 0;
  Error err = http_parse_req_status_line(headers, &sl, &advanced);
  if (ErrKindNone != err.kind) {
    log_err(&handler->logger, "failed to parse http status line", err);
    otel_handler_close(handler);
    return;
  }
  assert(advanced <= headers.len);

  // HTTP/1.x only. A higher minor version is handled as 1.1 (RFC 9110 section
  // 2.5).
  if (1 != sl.version_major) {
    log(&handler->logger, LogLevelError, "unsupported http version: %u.%u",
        sl.version_major, sl.version_minor);
    otel_handler_close(handler);
    return;
  }

  bytes_advance(&headers, advanced);
  fwrite(headers.data, 1, headers.len, stdout);
  puts("");

  err = http_parse_headers(headers, handler->headers, &handler->headers_len,
                           handler->headers_cap);
  if (ErrKindNone != err.kind) {
    log_err(&handler->logger, "failed to parse http headers", err);
    otel_handler_close(handler);
    return;
  }
  assert(handler->headers_len <= handler->headers_cap);

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
  if (ErrKindNone != err.kind) {
    log_err(&handler->logger, "failed to write", err);
    otel_handler_close(handler);
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

  const Error err =
      io->read(io, &handler->completion, accept_socket,
               bytes_buffer_space_bytes(handler->recv), otel_on_read);
  if (ErrKindNone != err.kind) {
    log_err(&handler->logger, "failed to read", err);
    otel_handler_close(handler);
    return;
  }
}

__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_string(Bytes input, const Logger *logger, Arena *arena,
                           usize *advanced) {
  assert(logger);
  assert(arena);
  assert(advanced);

  Bytes remaining = input;

  Tlv tlv = {0};
  Error err = tlv_read(remaining, &tlv, advanced);
  if (ErrKindNone != err.kind) {
    return err;
  }
  bytes_advance(&remaining, *advanced);

  if (TlvWireTypeLen != tlv.wire_type) {
    return (Error){.kind = ErrKindInvalidData};
  }

  fprintf(stdout, "key=%.*s %#x %#x %#x\n", (i32)tlv.value.len, tlv.value.data,
          tlv.value.data[0], tlv.value.data[1], tlv.value.data[2]);

  return (Error){0};
}

__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_key_value(Bytes input, const Logger *logger, Arena *arena,
                              usize *advanced) {
  assert(logger);
  assert(arena);
  assert(advanced);

  Bytes remaining = input;

  Error err = otel_parse_protobuf_string(remaining, logger, arena, advanced);
  if (ErrKindNone != err.kind) {
    return err;
  }
  bytes_advance(&remaining, *advanced);

  // TODO: Value.

  return (Error){0};
}

__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_key_values(Bytes input, const Logger *logger, Arena *arena,
                               usize *advanced) {
  assert(logger);
  assert(arena);
  assert(advanced);

  Bytes remaining = input;

  for (usize i = 0; i < input.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    Error err =
        otel_parse_protobuf_key_value(remaining, logger, arena, advanced);
    if (ErrKindNone != err.kind) {
      return err;
    }
    bytes_advance(&remaining, *advanced);
  }

  return (Error){0};
}

__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_resource(Bytes input, const Logger *logger, Arena *arena,
                             usize *advanced) {
  assert(logger);
  assert(arena);
  assert(advanced);

  Bytes remaining = input;

  Error err =
      otel_parse_protobuf_key_values(remaining, logger, arena, advanced);
  if (ErrKindNone != err.kind) {
    return err;
  }
  bytes_advance(&remaining, *advanced);

  return (Error){0};
}

__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_resource_spans(Bytes input, const Logger *logger,
                                   Arena *arena, usize *advanced) {
  assert(logger);
  assert(arena);
  assert(advanced);

  Bytes remaining = input;

  Error err = otel_parse_protobuf_resource(remaining, logger, arena, advanced);
  if (ErrKindNone != err.kind) {
    return err;
  }
  bytes_advance(&remaining, *advanced);

  return (Error){0};
}

__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_traces_data(Bytes input, const Logger *logger, Arena *arena,
                                usize *advanced) {
  assert(logger);
  assert(arena);
  assert(advanced);

  Bytes remaining = input;

  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < input.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    Error err =
        otel_parse_protobuf_resource_spans(remaining, logger, arena, advanced);
    if (ErrKindNone != err.kind) {
      return err;
    }
    bytes_advance(&remaining, *advanced);
  }

  return (Error){0};
}
