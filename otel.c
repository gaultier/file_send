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
// passed down as `tlv.value`. Unknown fields are skipped. Found values are
// only logged for now.

// `AnyValue` nests through `ArrayValue` and `KeyValueList`: this bounds the
// recursion.
static const u32 OTEL_PROTOBUF_DEPTH_MAX = 32;

// Reads the next record and moves `remaining` past it.
__attribute__((warn_unused_result)) static Error
otel_protobuf_next(Bytes *remaining, Tlv *tlv) {
  assert(remaining);
  assert(remaining->len > 0);
  assert(remaining->data);
  assert(tlv);

  const Bytes before = *remaining;
  usize advanced = 0;
  const Error err = tlv_read(before, tlv, &advanced);
  if (ErrKindNone != err.kind) {
    return err;
  }
  assert(advanced > 0);
  assert(advanced <= before.len);
  assert(tlv->field_num > 0);
  // The value sits inside the record that was just read.
  assert(before.data < tlv->value.data);
  assert(tlv->value.data + tlv->value.len == before.data + advanced);

  bytes_advance(remaining, advanced);
  assert(remaining->len < before.len);

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

// The value of a VARINT record.
__attribute__((warn_unused_result)) static Error
otel_protobuf_varint(Tlv tlv, u64 *dst) {
  assert(dst);

  const Error err = otel_protobuf_expect(tlv, TlvWireTypeVarint);
  if (ErrKindNone != err.kind) {
    return err;
  }

  // `tlv_read` already checked the encoding.
  usize advanced = 0;
  assert(ErrKindNone == varint_read(tlv.value, dst, &advanced).kind);
  assert(advanced == tlv.value.len);

  return (Error){0};
}

// The value of an I64 record, little endian.
__attribute__((warn_unused_result)) static Error
otel_protobuf_fixed64(Tlv tlv, u64 *dst) {
  assert(dst);

  const Error err = otel_protobuf_expect(tlv, TlvWireTypeI64);
  if (ErrKindNone != err.kind) {
    return err;
  }
  assert(sizeof(u64) == tlv.value.len);

  u64 res = 0;
  for (usize i = 0; i < sizeof(u64); i++) {
    res |= (u64)tlv.value.data[i] << (8 * i);
  }
  *dst = res;

  return (Error){0};
}

// The value of an I32 record, little endian.
__attribute__((warn_unused_result)) static Error
otel_protobuf_fixed32(Tlv tlv, u32 *dst) {
  assert(dst);

  const Error err = otel_protobuf_expect(tlv, TlvWireTypeI32);
  if (ErrKindNone != err.kind) {
    return err;
  }
  assert(sizeof(u32) == tlv.value.len);

  u32 res = 0;
  for (usize i = 0; i < sizeof(u32); i++) {
    res |= (u32)tlv.value.data[i] << (8 * i);
  }
  *dst = res;

  return (Error){0};
}

// Writes the lowercase hex of `src` to `dst`, as much as fits, without a
// terminator. Returns how many bytes of `src` were encoded.
__attribute__((warn_unused_result)) static usize
otel_hex_encode(Bytes src, char *dst, usize dst_cap) {
  assert(dst);

  const char digits[] = "0123456789abcdef";
  const usize count = min(src.len, dst_cap / 2);
  for (usize i = 0; i < count; i++) {
    const u8 byte = src.data[i];
    dst[2 * i] = digits[byte >> 4];
    dst[2 * i + 1] = digits[byte & 0xf];
  }
  assert(2 * count <= dst_cap);

  return count;
}

__attribute__((warn_unused_result)) static Error
otel_log_string(const Logger *logger, const char *name, Tlv tlv) {
  assert(logger);
  assert(name);

  const Error err = otel_protobuf_expect(tlv, TlvWireTypeLen);
  if (ErrKindNone != err.kind) {
    return err;
  }

  log(logger, LogLevelDebug, "%s=%.*s", name,
      (i32)min(tlv.value.len, (usize)INT32_MAX), tlv.value.data);
  return (Error){0};
}

// Bytes are logged as hex, cut short past 32 bytes.
__attribute__((warn_unused_result)) static Error
otel_log_bytes(const Logger *logger, const char *name, Tlv tlv) {
  assert(logger);
  assert(name);

  const Error err = otel_protobuf_expect(tlv, TlvWireTypeLen);
  if (ErrKindNone != err.kind) {
    return err;
  }

  char hex[2 * 32 + 1] = {0};
  const usize encoded = otel_hex_encode(tlv.value, hex, sizeof(hex) - 1);
  assert(encoded <= tlv.value.len);

  log(logger, LogLevelDebug, "%s=%s%s", name, hex,
      encoded < tlv.value.len ? "..." : "");
  return (Error){0};
}

// For uint32, uint64 and enums.
__attribute__((warn_unused_result)) static Error
otel_log_uint(const Logger *logger, const char *name, Tlv tlv) {
  assert(logger);
  assert(name);

  u64 value = 0;
  const Error err = otel_protobuf_varint(tlv, &value);
  if (ErrKindNone != err.kind) {
    return err;
  }

  log(logger, LogLevelDebug, "%s=%" PRIu64, name, value);
  return (Error){0};
}

// For int32 and int64: a negative int32 is sign extended to 64 bits on the
// wire.
__attribute__((warn_unused_result)) static Error
otel_log_int(const Logger *logger, const char *name, Tlv tlv) {
  assert(logger);
  assert(name);

  u64 value = 0;
  const Error err = otel_protobuf_varint(tlv, &value);
  if (ErrKindNone != err.kind) {
    return err;
  }

  log(logger, LogLevelDebug, "%s=%" PRId64, name, (i64)value);
  return (Error){0};
}

__attribute__((warn_unused_result)) static Error
otel_log_bool(const Logger *logger, const char *name, Tlv tlv) {
  assert(logger);
  assert(name);

  u64 value = 0;
  const Error err = otel_protobuf_varint(tlv, &value);
  if (ErrKindNone != err.kind) {
    return err;
  }

  log(logger, LogLevelDebug, "%s=%s", name, value ? "true" : "false");
  return (Error){0};
}

__attribute__((warn_unused_result)) static Error
otel_log_fixed64(const Logger *logger, const char *name, Tlv tlv) {
  assert(logger);
  assert(name);

  u64 value = 0;
  const Error err = otel_protobuf_fixed64(tlv, &value);
  if (ErrKindNone != err.kind) {
    return err;
  }

  log(logger, LogLevelDebug, "%s=%" PRIu64, name, value);
  return (Error){0};
}

__attribute__((warn_unused_result)) static Error
otel_log_fixed32(const Logger *logger, const char *name, Tlv tlv) {
  assert(logger);
  assert(name);

  u32 value = 0;
  const Error err = otel_protobuf_fixed32(tlv, &value);
  if (ErrKindNone != err.kind) {
    return err;
  }

  log(logger, LogLevelDebug, "%s=%#x", name, value);
  return (Error){0};
}

__attribute__((warn_unused_result)) static Error
otel_log_double(const Logger *logger, const char *name, Tlv tlv) {
  assert(logger);
  assert(name);

  u64 bits = 0;
  const Error err = otel_protobuf_fixed64(tlv, &bits);
  if (ErrKindNone != err.kind) {
    return err;
  }

  double value = 0;
  memcpy(&value, &bits, sizeof(value));
  log(logger, LogLevelDebug, "%s=%g", name, value);
  return (Error){0};
}

// `AnyValue`, `ArrayValue`, `KeyValueList` and `KeyValue` call each other.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_any_value(Bytes body, const Logger *logger, Arena *arena,
                              u32 depth);

// `KeyValue`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_key_value(Bytes body, const Logger *logger, Arena *arena,
                              u32 depth) {
  assert(logger);
  assert(arena);
  assert(depth <= OTEL_PROTOBUF_DEPTH_MAX);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // string key
      err = otel_log_string(logger, "key", tlv);
      break;

    case 2: // AnyValue value
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_any_value(tlv.value, logger, arena, depth);
      }
      break;

    case 3: // int32 key_strindex
      err = otel_log_int(logger, "key_strindex", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `ArrayValue`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_array_value(Bytes body, const Logger *logger, Arena *arena,
                                u32 depth) {
  assert(logger);
  assert(arena);
  assert(depth < OTEL_PROTOBUF_DEPTH_MAX);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // repeated AnyValue values
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_any_value(tlv.value, logger, arena,
                                            depth + 1);
      }
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `KeyValueList`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_key_value_list(Bytes body, const Logger *logger,
                                   Arena *arena, u32 depth) {
  assert(logger);
  assert(arena);
  assert(depth < OTEL_PROTOBUF_DEPTH_MAX);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // repeated KeyValue values
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_key_value(tlv.value, logger, arena,
                                            depth + 1);
      }
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `AnyValue`. A oneof: the last member on the wire wins. Empty when unset.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_any_value(Bytes body, const Logger *logger, Arena *arena,
                              u32 depth) {
  assert(logger);
  assert(arena);

  if (depth >= OTEL_PROTOBUF_DEPTH_MAX) {
    return (Error){.kind = ErrKindInvalidData};
  }

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // string string_value
      err = otel_log_string(logger, "string_value", tlv);
      break;

    case 2: // bool bool_value
      err = otel_log_bool(logger, "bool_value", tlv);
      break;

    case 3: // int64 int_value
      err = otel_log_int(logger, "int_value", tlv);
      break;

    case 4: // double double_value
      err = otel_log_double(logger, "double_value", tlv);
      break;

    case 5: // ArrayValue array_value
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_array_value(tlv.value, logger, arena, depth);
      }
      break;

    case 6: // KeyValueList kvlist_value
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_key_value_list(tlv.value, logger, arena,
                                                 depth);
      }
      break;

    case 7: // bytes bytes_value
      err = otel_log_bytes(logger, "bytes_value", tlv);
      break;

    case 8: // int32 string_value_strindex
      err = otel_log_int(logger, "string_value_strindex", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `EntityRef`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_entity_ref(Bytes body, const Logger *logger, Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // string schema_url
      err = otel_log_string(logger, "entity_ref.schema_url", tlv);
      break;

    case 2: // string type
      err = otel_log_string(logger, "entity_ref.type", tlv);
      break;

    case 3: // repeated string id_keys
      err = otel_log_string(logger, "entity_ref.id_key", tlv);
      break;

    case 4: // repeated string description_keys
      err = otel_log_string(logger, "entity_ref.description_key", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
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
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // repeated KeyValue attributes
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_key_value(tlv.value, logger, arena, 0);
      }
      break;

    case 2: // uint32 dropped_attributes_count
      err = otel_log_uint(logger, "resource.dropped_attributes_count", tlv);
      break;

    case 3: // repeated EntityRef entity_refs
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_entity_ref(tlv.value, logger, arena);
      }
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `InstrumentationScope`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_instrumentation_scope(Bytes body, const Logger *logger,
                                          Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // string name
      err = otel_log_string(logger, "scope.name", tlv);
      break;

    case 2: // string version
      err = otel_log_string(logger, "scope.version", tlv);
      break;

    case 3: // repeated KeyValue attributes
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_key_value(tlv.value, logger, arena, 0);
      }
      break;

    case 4: // uint32 dropped_attributes_count
      err = otel_log_uint(logger, "scope.dropped_attributes_count", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `Span.Event`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_span_event(Bytes body, const Logger *logger,
                               Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // fixed64 time_unix_nano
      err = otel_log_fixed64(logger, "event.time_unix_nano", tlv);
      break;

    case 2: // string name
      err = otel_log_string(logger, "event.name", tlv);
      break;

    case 3: // repeated KeyValue attributes
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_key_value(tlv.value, logger, arena, 0);
      }
      break;

    case 4: // uint32 dropped_attributes_count
      err = otel_log_uint(logger, "event.dropped_attributes_count", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `Span.Link`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_span_link(Bytes body, const Logger *logger, Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // bytes trace_id
      err = otel_log_bytes(logger, "link.trace_id", tlv);
      break;

    case 2: // bytes span_id
      err = otel_log_bytes(logger, "link.span_id", tlv);
      break;

    case 3: // string trace_state
      err = otel_log_string(logger, "link.trace_state", tlv);
      break;

    case 4: // repeated KeyValue attributes
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_key_value(tlv.value, logger, arena, 0);
      }
      break;

    case 5: // uint32 dropped_attributes_count
      err = otel_log_uint(logger, "link.dropped_attributes_count", tlv);
      break;

    case 6: // fixed32 flags
      err = otel_log_fixed32(logger, "link.flags", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `Status`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_status(Bytes body, const Logger *logger, Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 2: // string message
      err = otel_log_string(logger, "status.message", tlv);
      break;

    case 3: // StatusCode code
      err = otel_log_uint(logger, "status.code", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `Span`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_span(Bytes body, const Logger *logger, Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // bytes trace_id
      err = otel_log_bytes(logger, "span.trace_id", tlv);
      break;

    case 2: // bytes span_id
      err = otel_log_bytes(logger, "span.span_id", tlv);
      break;

    case 3: // string trace_state
      err = otel_log_string(logger, "span.trace_state", tlv);
      break;

    case 4: // bytes parent_span_id
      err = otel_log_bytes(logger, "span.parent_span_id", tlv);
      break;

    case 5: // string name
      err = otel_log_string(logger, "span.name", tlv);
      break;

    case 6: // SpanKind kind
      err = otel_log_uint(logger, "span.kind", tlv);
      break;

    case 7: // fixed64 start_time_unix_nano
      err = otel_log_fixed64(logger, "span.start_time_unix_nano", tlv);
      break;

    case 8: // fixed64 end_time_unix_nano
      err = otel_log_fixed64(logger, "span.end_time_unix_nano", tlv);
      break;

    case 9: // repeated KeyValue attributes
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_key_value(tlv.value, logger, arena, 0);
      }
      break;

    case 10: // uint32 dropped_attributes_count
      err = otel_log_uint(logger, "span.dropped_attributes_count", tlv);
      break;

    case 11: // repeated Event events
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_span_event(tlv.value, logger, arena);
      }
      break;

    case 12: // uint32 dropped_events_count
      err = otel_log_uint(logger, "span.dropped_events_count", tlv);
      break;

    case 13: // repeated Link links
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_span_link(tlv.value, logger, arena);
      }
      break;

    case 14: // uint32 dropped_links_count
      err = otel_log_uint(logger, "span.dropped_links_count", tlv);
      break;

    case 15: // Status status
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_status(tlv.value, logger, arena);
      }
      break;

    case 16: // fixed32 flags
      err = otel_log_fixed32(logger, "span.flags", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}

// `ScopeSpans`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_scope_spans(Bytes body, const Logger *logger,
                                Arena *arena) {
  assert(logger);
  assert(arena);

  Bytes remaining = body;
  // Each record is at least one byte, so this bounds the records.
  for (usize i = 0; i < body.len; i++) {
    if (0 == remaining.len) {
      break;
    }
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // InstrumentationScope scope
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err =
            otel_parse_protobuf_instrumentation_scope(tlv.value, logger, arena);
      }
      break;

    case 2: // repeated Span spans
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_span(tlv.value, logger, arena);
      }
      break;

    case 3: // string schema_url
      err = otel_log_string(logger, "scope_spans.schema_url", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
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
    assert(remaining.len <= body.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // Resource resource
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_resource(tlv.value, logger, arena);
      }
      break;

    case 2: // repeated ScopeSpans scope_spans
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_scope_spans(tlv.value, logger, arena);
      }
      break;

    case 3: // string schema_url
      err = otel_log_string(logger, "resource_spans.schema_url", tlv);
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
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
    assert(remaining.len <= input.len - i);
    Tlv tlv = {0};
    Error err = otel_protobuf_next(&remaining, &tlv);
    if (ErrKindNone != err.kind) {
      return err;
    }

    switch (tlv.field_num) {
    case 1: // repeated ResourceSpans resource_spans
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_resource_spans(tlv.value, logger, arena);
      }
      break;

    default:
      break;
    }
    if (ErrKindNone != err.kind) {
      return err;
    }
  }
  assert(0 == remaining.len);

  return (Error){0};
}
