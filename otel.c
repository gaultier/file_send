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

// Each `otel_parse_protobuf_*` takes the body of its message: the records
// after the message's own tag and length, which the parent read. A field is
// passed down as `tlv.value`. Unknown fields are skipped.

// Reads the next record and moves `remaining` past it.
__attribute__((warn_unused_result)) static Error
otel_protobuf_next(Bytes *remaining, Tlv *tlv) {
  assert(remaining);
  assert(remaining->len > 0);
  assert(tlv);

  usize advanced = 0;
  const Error err = tlv_read(*remaining, tlv, &advanced);
  if (ErrKindNone != err.kind) {
    return err;
  }
  assert(advanced > 0);
  bytes_advance(remaining, advanced);

  return (Error){0};
}

// A known field must have the wire type of the schema.
__attribute__((warn_unused_result)) static Error
otel_protobuf_expect(Tlv tlv, TlvWireType wire_type) {
  if (wire_type != tlv.wire_type) {
    return (Error){.kind = ErrKindInvalidData};
  }
  return (Error){0};
}

// `AnyValue`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_any_value(Bytes body, const Logger *logger, Arena *arena) {
  assert(logger);
  assert(arena);

  // A oneof: the last member on the wire wins. Empty when unset.
  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // string_value
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      log(logger, LogLevelDebug, "string_value=%.*s", (i32)tlv.value.len,
          tlv.value.data);
      break;

    default:
      break;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `KeyValue`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_key_value(Bytes body, const Logger *logger, Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // string key
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      log(logger, LogLevelDebug, "key=%.*s", (i32)tlv.value.len,
          tlv.value.data);
      break;

    case 2: // AnyValue value
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      err = otel_parse_protobuf_any_value(tlv.value, logger, arena);
      if (ErrKindNone != err.kind) {
        return err;
      }
      break;

    default:
      break;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `Resource`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_resource(Bytes body, const Logger *logger, Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // repeated KeyValue attributes
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      err = otel_parse_protobuf_key_value(tlv.value, logger, arena);
      if (ErrKindNone != err.kind) {
        return err;
      }
      break;

    case 2: // uint32 dropped_attributes_count
      err = otel_protobuf_expect(tlv, TlvWireTypeVarint);
      if (ErrKindNone != err.kind) {
        return err;
      }
      break;

    default:
      break;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `ResourceSpans`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_resource_spans(Bytes body, const Logger *logger,
                                   Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // Resource resource
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      err = otel_parse_protobuf_resource(tlv.value, logger, arena);
      if (ErrKindNone != err.kind) {
        return err;
      }
      break;

    case 2: // repeated ScopeSpans scope_spans
    case 3: // string schema_url
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      // TODO.
      break;

    default:
      break;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `TracesData`, the top level message: `input` is the whole payload.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_traces_data(Bytes input, const Logger *logger,
                                Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = input;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < input.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // repeated ResourceSpans resource_spans
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      err = otel_parse_protobuf_resource_spans(tlv.value, logger, arena);
      if (ErrKindNone != err.kind) {
        return err;
      }
      break;

    default:
      break;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}
