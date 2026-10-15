#pragma once

#include "http.c"

typedef enum : u8 {
  OTEL_SPAN_KIND_UNSPECIFIED,
  OTEL_SPAN_KIND_INTERNAL,
  OTEL_SPAN_KIND_SERVER,
  OTEL_SPAN_KIND_CLIENT,
  OTEL_SPAN_KIND_PRODUCER,
  OTEL_SPAN_KIND_CONSUMER,
} OtelSpanKind;

typedef enum : u8 {
  OTEL_STATUS_CODE_UNSET,
  OTEL_STATUS_CODE_OK,
  OTEL_STATUS_CODE_ERROR,
} OtelStatusCode;

typedef enum {
  OTEL_VALUE_KIND_STRING,
  OTEL_VALUE_KIND_BOOL,
  OTEL_VALUE_KIND_I64,
  OTEL_VALUE_KIND_DOUBLE,
  OTEL_VALUE_KIND_BYTES,
} OtelValueKind;

typedef struct {
  OtelValueKind kind;
  u64 val;
} OtelValue;

typedef struct {
  Bytes trace_id;
  Bytes id;
  // TODO: trace_state
  Bytes parent_id;
  // From 2026-01-01.
  u64 start_time_nano;
  u64 end_time_nano;
  Bytes name;
  Bytes attrs_keys;
  OtelValue *attrs_values;
  u32 flags;
  u16 attrs_count;
  OtelSpanKind kind;
  OtelStatusCode status_code;

  // TODO: attributes
  // TODO: events
  // TODO: links
} OtelSpan;

__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_traces_data(Bytes input, OtelSpan *spans,
                                usize *spans_count, usize spans_cap,
                                const Logger *logger);

static void otel_span_log(const OtelSpan *span, const Logger *logger);

static void http_handler_on_close(PlatformCompletion *completion, Error err,
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
  Platform *const platform = handler->platform;
  assert(platform);

  const Error err = platform->close(platform, &handler->completion,
                                    handler->socket, http_handler_on_close);
  if (ErrKindNone == err.kind) {
    return;
  }

  // The only release outside `http_handler_on_close`: no callback is coming,
  // so clean up now.
  log_err(&handler->logger, "failed to submit close", err);
  (void)platform->close_socket(platform, handler->socket);
  http_handler_release(handler);
}

static void otel_on_write(PlatformCompletion *completion, Error write_err,
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

static void otel_on_read(PlatformCompletion *completion, Error read_err,
                         usize read_count) {
  assert(completion);
  assert(completion->ctx);

  HttpHandler *const handler = completion->ctx;
  Platform *const platform = handler->platform;
  assert(platform);

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
  HttpRequestStatusLine sl = {0};
  usize advanced = 0;
  Error err = http_parse_req(recv, &sl, handler->headers, &handler->headers_len,
                             handler->headers_cap, &advanced);
  if (ErrKindAgain == err.kind) {
    // The headers do not fit.
    if (0 == bytes_buffer_space(handler->recv)) {
      log(&handler->logger, LogLevelError, "headers too big");
      otel_handler_close(handler);
      return;
    }

    err = platform->read(platform, &handler->completion, handler->socket,
                         bytes_buffer_space_bytes(handler->recv), otel_on_read);
    if (ErrKindNone != err.kind) {
      log_err(&handler->logger, "failed to read", err);
      otel_handler_close(handler);
    }
    return;
  }
  if (ErrKindNone != err.kind) {
    log_err(&handler->logger, "failed to parse http request", err);
    otel_handler_close(handler);
    return;
  }
  assert(advanced <= recv.len);
  assert(handler->headers_len <= handler->headers_cap);

  // HTTP/1.x only. A higher minor version is handled as 1.1 (RFC 9110 section
  // 2.5).
  if (1 != sl.version_major) {
    log(&handler->logger, LogLevelError, "unsupported http version: %u.%u",
        sl.version_major, sl.version_minor);
    otel_handler_close(handler);
    return;
  }

  log(&handler->logger, LogLevelDebug, "parsed %zu HTTP headers",
      handler->headers_len);

  const Bytes body = bytes_subslice(recv, advanced, recv.len);
  {
    const usize max_spans_in_batch = 1024; // TODO: Adjust.
    OtelSpan *spans = arena_alloc(&handler->arena, __alignof__(OtelSpan),
                                  sizeof(OtelSpan), max_spans_in_batch);
    usize spans_count = 0;
    if (!spans) {
      assert(0 && "oom");
    }
    err = otel_parse_protobuf_traces_data(body, spans, &spans_count,
                                          max_spans_in_batch, &handler->logger);
    if (ErrKindNone != err.kind) {
      log_err(&handler->logger, "failed to parse protobuf trace", err);
    } else {

      for (usize i = 0; i < spans_count; i++) {
        otel_span_log(&spans[i], &handler->logger);
      }
    }
  }

  // FIXME
  assert(ErrKindNone ==
         bytes_buffer_make(128, &handler->arena, &handler->resp).kind);
  assert(bytes_buffer_extend_within_cap(
      &handler->resp,
      bytes_from_cstr("HTTP/1.1 200\r\nConnection:Close\r\n\r\nHello")));

  err = platform->write(platform, &handler->completion, handler->socket,
                        bytes_buffer_to_bytes(handler->resp), otel_on_write);
  if (ErrKindNone != err.kind) {
    log_err(&handler->logger, "failed to write", err);
    otel_handler_close(handler);
    return;
  }
}

static void otel_on_accept(Platform *platform, void *vctx, Ipv4Addr accept_addr,
                           i32 accept_socket) {
  assert(platform);
  assert(vctx);
  HttpServer *const server = vctx;

  HttpHandler *const handler = pool_acquire(&server->handler_pool);
  if (!handler) {
    log(&server->logger, LogLevelError,
        "backpressure: no available pool slot for request");
    (void)platform->close_socket(platform, accept_socket);
    return;
  }

  http_handler_init(handler, server, platform, accept_addr, accept_socket);

  log(&handler->logger, LogLevelInfo, "accepted");

  const Error err =
      platform->read(platform, &handler->completion, accept_socket,
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

// Logged as hex, cut short past 32 bytes.
static void otel_log_hex(const Logger *logger, const char *name, Bytes value) {
  assert(logger);
  assert(name);

  char hex[2 * 32 + 1] = {0};
  const usize encoded = otel_hex_encode(value, hex, sizeof(hex) - 1);
  assert(encoded <= value.len);

  log(logger, LogLevelDebug, "%s=%s%s", name, hex,
      encoded < value.len ? "..." : "");
}

__attribute__((warn_unused_result)) static Error
otel_log_bytes(const Logger *logger, const char *name, Tlv tlv) {
  assert(logger);
  assert(name);

  const Error err = otel_protobuf_expect(tlv, TlvWireTypeLen);
  if (ErrKindNone != err.kind) {
    return err;
  }

  otel_log_hex(logger, name, tlv.value);
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

__attribute__((warn_unused_result)) static const char *
otel_span_kind_to_cstr(OtelSpanKind kind) {
  switch (kind) {
  case OTEL_SPAN_KIND_UNSPECIFIED:
    return "unspecified";
  case OTEL_SPAN_KIND_INTERNAL:
    return "internal";
  case OTEL_SPAN_KIND_SERVER:
    return "server";
  case OTEL_SPAN_KIND_CLIENT:
    return "client";
  case OTEL_SPAN_KIND_PRODUCER:
    return "producer";
  case OTEL_SPAN_KIND_CONSUMER:
    return "consumer";
  default:
    return "invalid";
  }
}

__attribute__((warn_unused_result)) static const char *
otel_status_code_to_cstr(OtelStatusCode status_code) {
  switch (status_code) {
  case OTEL_STATUS_CODE_UNSET:
    return "unset";
  case OTEL_STATUS_CODE_OK:
    return "ok";
  case OTEL_STATUS_CODE_ERROR:
    return "error";
  default:
    return "invalid";
  }
}

static void otel_span_log(const OtelSpan *span, const Logger *logger) {
  assert(span);
  assert(logger);

  otel_log_hex(logger, "span.trace_id", span->trace_id);
  otel_log_hex(logger, "span.id", span->id);
  otel_log_hex(logger, "span.parent_id", span->parent_id);
  log(logger, LogLevelDebug, "span.start_time_nano=%" PRIu64,
      span->start_time_nano);
  log(logger, LogLevelDebug, "span.end_time_nano=%" PRIu64,
      span->end_time_nano);
  log(logger, LogLevelDebug, "span.name=%.*s",
      (i32)min(span->name.len, (usize)INT32_MAX), span->name.data);
  otel_log_hex(logger, "span.attrs_keys", span->attrs_keys);
  log(logger, LogLevelDebug, "span.attrs_count=%u", span->attrs_count);
  if (span->attrs_count > 0) {
    assert(span->attrs_values);
  }
  for (usize i = 0; i < span->attrs_count; i++) {
    const OtelValue value = span->attrs_values[i];
    log(logger, LogLevelDebug, "span.attrs_values[%zu]=kind:%u val:%#" PRIx64,
        i, value.kind, value.val);
  }
  log(logger, LogLevelDebug, "span.flags=%#x", span->flags);
  log(logger, LogLevelDebug, "span.kind=%s",
      otel_span_kind_to_cstr(span->kind));
  log(logger, LogLevelDebug, "span.status_code=%s",
      otel_status_code_to_cstr(span->status_code));
}

// `AnyValue`, `ArrayValue`, `KeyValueList` and `KeyValue` call each other.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_any_value(Bytes body, const Logger *logger, u32 depth);

// `KeyValue`.
__attribute__((warn_unused_result)) static Error
otel_parse_protobuf_key_value(Bytes body, const Logger *logger, u32 depth) {
  assert(logger);
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
        err = otel_parse_protobuf_any_value(tlv.value, logger, depth);
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
otel_parse_protobuf_array_value(Bytes body, const Logger *logger, u32 depth) {
  assert(logger);
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
        err = otel_parse_protobuf_any_value(tlv.value, logger, depth + 1);
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
                                   u32 depth) {
  assert(logger);
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
        err = otel_parse_protobuf_key_value(tlv.value, logger, depth + 1);
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
otel_parse_protobuf_any_value(Bytes body, const Logger *logger, u32 depth) {
  assert(logger);

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
        err = otel_parse_protobuf_array_value(tlv.value, logger, depth);
      }
      break;

    case 6: // KeyValueList kvlist_value
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_key_value_list(tlv.value, logger, depth);
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
otel_parse_protobuf_entity_ref(Bytes body, const Logger *logger) {
  assert(logger);

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
otel_parse_protobuf_resource(Bytes body, const Logger *logger) {
  assert(logger);

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
        err = otel_parse_protobuf_key_value(tlv.value, logger, 0);
      }
      break;

    case 2: // uint32 dropped_attributes_count
      err = otel_log_uint(logger, "resource.dropped_attributes_count", tlv);
      break;

    case 3: // repeated EntityRef entity_refs
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_entity_ref(tlv.value, logger);
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
otel_parse_protobuf_instrumentation_scope(Bytes body, const Logger *logger) {
  assert(logger);

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
        err = otel_parse_protobuf_key_value(tlv.value, logger, 0);
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
otel_parse_protobuf_span_event(Bytes body, const Logger *logger) {
  assert(logger);

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
        err = otel_parse_protobuf_key_value(tlv.value, logger, 0);
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
otel_parse_protobuf_span_link(Bytes body, const Logger *logger) {
  assert(logger);

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
        err = otel_parse_protobuf_key_value(tlv.value, logger, 0);
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
otel_parse_protobuf_status(Bytes body, OtelStatusCode *status_code,
                           const Logger *logger) {
  assert(status_code);
  assert(logger);

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

    case 3: { // StatusCode code
      u64 value = 0;
      err = otel_protobuf_varint(tlv, &value);
      if (ErrKindNone != err.kind) {
        return err;
      }
      if (value <= OTEL_STATUS_CODE_ERROR) {
        *status_code = (OtelStatusCode)value;
      }
      err = otel_log_uint(logger, "status.code", tlv);
    } break;

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
otel_parse_protobuf_span(Bytes body, OtelSpan *span, const Logger *logger) {
  assert(span);
  assert(logger);

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
    case 1: { // bytes trace_id
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      if (16 != tlv.value.len) {
        return (Error){.kind = ErrKindInvalidData};
      }
      span->trace_id = tlv.value;
    } break;

    case 2: { // bytes span_id
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      if (8 != tlv.value.len) {
        return (Error){.kind = ErrKindInvalidData};
      }
      span->id = tlv.value;
    } break;

    case 3: // string trace_state
      err = otel_log_string(logger, "span.trace_state", tlv);
      break;

    case 4: { // bytes parent_span_id
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }

      // Empty for a root span.
      if (0 != tlv.value.len && 8 != tlv.value.len) {
        return (Error){.kind = ErrKindInvalidData};
      }
      span->parent_id = tlv.value;
    } break;

    case 5: { // string name
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone != err.kind) {
        return err;
      }
      span->name = tlv.value;
    } break;

    case 6: { // SpanKind kind
      u64 kind = 0;
      err = otel_protobuf_varint(tlv, &kind);
      if (ErrKindNone != err.kind) {
        return err;
      }

      if (kind <= OTEL_SPAN_KIND_CONSUMER /* MAX */) {
        span->kind = (OtelSpanKind)kind;
      }
    } break;

    case 7: // fixed64 start_time_unix_nano
      err = otel_protobuf_fixed64(tlv, &span->start_time_nano);
      if (ErrKindNone != err.kind) {
        return err;
      }

      break;

    case 8: // fixed64 end_time_unix_nano
      err = otel_protobuf_fixed64(tlv, &span->end_time_nano);
      if (ErrKindNone != err.kind) {
        return err;
      }
      break;

    case 9: // repeated KeyValue attributes
      // TODO
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_key_value(tlv.value, logger, 0);
      }
      break;

    case 10: // uint32 dropped_attributes_count
      err = otel_log_uint(logger, "span.dropped_attributes_count", tlv);
      break;

    case 11: // repeated Event events
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_span_event(tlv.value, logger);
      }
      break;

    case 12: // uint32 dropped_events_count
      err = otel_log_uint(logger, "span.dropped_events_count", tlv);
      break;

    case 13: // repeated Link links
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_span_link(tlv.value, logger);
      }
      break;

    case 14: // uint32 dropped_links_count
      err = otel_log_uint(logger, "span.dropped_links_count", tlv);
      break;

    case 15: // Status status
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_status(tlv.value, &span->status_code, logger);
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
otel_parse_protobuf_scope_spans(Bytes body, OtelSpan *spans, usize *spans_count,
                                usize spans_cap, const Logger *logger) {
  assert(logger);
  assert(spans);
  assert(spans_count);
  assert(spans_cap);

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
        err = otel_parse_protobuf_instrumentation_scope(tlv.value, logger);
      }
      break;

    case 2: // repeated Span spans
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      OtelSpan span = {0};
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_span(tlv.value, &span, logger);
      }

      assert(spans_cap >= *spans_count);

      if (spans_cap == *spans_count) {
        return (Error){.kind = ErrKindRange};
      }

      spans[*spans_count] = span;
      *spans_count += 1;

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
otel_parse_protobuf_resource_spans(Bytes body, OtelSpan *spans,
                                   usize *spans_count, usize spans_cap,
                                   const Logger *logger) {
  assert(spans);
  assert(spans_count);
  assert(spans_cap);
  assert(logger);

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
        err = otel_parse_protobuf_resource(tlv.value, logger);
      }
      break;

    case 2: // repeated ScopeSpans scope_spans
      err = otel_protobuf_expect(tlv, TlvWireTypeLen);
      if (ErrKindNone == err.kind) {
        err = otel_parse_protobuf_scope_spans(tlv.value, spans, spans_count,
                                              spans_cap, logger);
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
otel_parse_protobuf_traces_data(Bytes input, OtelSpan *spans,
                                usize *spans_count, usize spans_cap,
                                const Logger *logger) {
  assert(spans);
  assert(spans_count);
  assert(spans_cap);
  assert(logger);

  *spans_count = 0;

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
        err = otel_parse_protobuf_resource_spans(tlv.value, spans, spans_count,
                                                 spans_cap, logger);
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
