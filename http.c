#pragma once

#include "lib.c"

// Both borrow from the parsed input.
typedef struct {
  Bytes key;
  Bytes value;
} HttpHeader;

typedef enum {
  HTTP_METHOD_UNKNOWN,
  HTTP_METHOD_OPTIONS,
  HTTP_METHOD_GET,
  HTTP_METHOD_HEAD,
  HTTP_METHOD_POST,
  HTTP_METHOD_PUT,
  HTTP_METHOD_DELETE,
  HTTP_METHOD_TRACE,
  HTTP_METHOD_CONNECT,
  HTTP_METHOD_EXTENSION,
} HttpMethod;

// `GET /en-US/docs/Web/HTTP/Messages HTTP/1.1`.
typedef struct {
  HttpMethod method;
  u8 version_minor;
  u8 version_major;
  Bytes url; // Does not have a scheme, domain, port.
} HttpRequestStatusLine;

__attribute__((warn_unused_result)) static Find
http_find_headers_end(Bytes haystack) {
  return bytes_find(haystack, bytes_from_cstr("\r\n\r\n"),
                    FindOptionsIndexAfterNeedleEnd);
}

// RFC 9110 section 5.6.2.
__attribute__((warn_unused_result)) static bool http_is_tchar(u8 c) {
  if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
      ('0' <= c && c <= '9')) {
    return true;
  }

  switch (c) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

// OWS in RFC 9110 section 5.6.3.
__attribute__((warn_unused_result)) static bool http_is_ows(u8 c) {
  return ' ' == c || '\t' == c;
}

// VCHAR or obs-text in RFC 9110 section 5.5.
__attribute__((warn_unused_result)) static bool http_is_field_vchar(u8 c) {
  return (0x21 <= c && c <= 0x7e) || 0x80 <= c;
}

// `field-line = field-name ":" OWS field-value OWS`, without the CRLF.
__attribute__((warn_unused_result)) static Error
http_parse_field_line(Bytes line, HttpHeader *dst) {
  assert(dst);
  assert(line.len > 0);
  assert(line.data);

  const Split split = bytes_split(line, bytes_from_cstr(":"));
  if (!split.found) {
    return (Error){.kind = ErrKindInvalidData};
  }

  // A token: this also rejects whitespace before the colon and obs-fold, which
  // a server must reject (RFC 9112 section 5.1 and 5.2).
  const Bytes key = split.left;
  if (0 == key.len) {
    return (Error){.kind = ErrKindInvalidData};
  }
  for (usize i = 0; i < key.len; i++) {
    if (!http_is_tchar(key.data[i])) {
      return (Error){.kind = ErrKindInvalidData};
    }
  }

  Bytes value = split.right;
  for (usize i = 0; i < split.right.len; i++) {
    assert(value.len == split.right.len - i);
    if (!http_is_ows(value.data[0])) {
      break;
    }
    bytes_advance(&value, 1);
  }
  for (usize i = 0; i < split.right.len; i++) {
    if (0 == value.len || !http_is_ows(value.data[value.len - 1])) {
      break;
    }
    value.len -= 1;
  }

  // Whitespace may only sit between visible characters, and CR, LF and NUL
  // must be rejected (RFC 9110 section 5.5).
  for (usize i = 0; i < value.len; i++) {
    const u8 c = value.data[i];
    if (!http_is_field_vchar(c) && !http_is_ows(c)) {
      return (Error){.kind = ErrKindInvalidData};
    }
  }
  if (value.len > 0) {
    assert(http_is_field_vchar(value.data[0]));
    assert(http_is_field_vchar(value.data[value.len - 1]));
  }
  assert(value.len <= split.right.len);

  *dst = (HttpHeader){.key = key, .value = value};
  return (Error){0};
}

// Parse the field lines that follow the start line, up to and including the
// empty line that ends them (RFC 9112 section 2.1). `src` starts right after
// the start line. What follows the empty line is the body and is not read.
// The headers borrow from `src`.
__attribute__((warn_unused_result)) static Error
http_parse_headers(Bytes src, HttpHeader *headers, usize *headers_len,
                   usize headers_cap) {
  assert(headers);
  assert(headers_len);
  assert(0 == *headers_len);
  assert(headers_cap > 0);

  const Bytes crlf = bytes_from_cstr("\r\n");
  Bytes remaining = src;

  // Each line eats at least its CRLF, so this bounds the lines.
  for (usize i = 0; i <= src.len / crlf.len; i++) {
    assert(*headers_len <= headers_cap);
    assert(*headers_len <= i);
    assert(remaining.len <= src.len);

    const Split line = bytes_split(remaining, crlf);
    // No CRLF: the headers are cut short.
    if (!line.found) {
      return (Error){.kind = ErrKindInvalidData};
    }
    remaining = line.right;

    if (0 == line.left.len) {
      return (Error){0};
    }

    if (*headers_len == headers_cap) {
      return (Error){.kind = ErrKindOOM};
    }

    HttpHeader header = {0};
    const Error err = http_parse_field_line(line.left, &header);
    if (ErrKindNone != err.kind) {
      return err;
    }
    assert(header.key.len > 0);
    assert(src.data <= header.key.data);
    assert(header.value.data + header.value.len <= src.data + src.len);

    headers[*headers_len] = header;
    *headers_len += 1;
  }

  assert(0 && "unreachable");
}

// ---------- Server ----------

typedef u64 HttpPoolSlotGroup;

// Unit is bits. Fixed by the type, not a setting.
#define HTTP_POOL_SLOTS_PER_GROUP (sizeof(HttpPoolSlotGroup) * 8)

typedef struct HttpServer HttpServer;

typedef struct {
  Logger logger;
  i32 socket;
  Arena arena;
  IO *io;
  HttpServer *server;
  IoCompletion completion;
  BytesBuffer recv;

  HttpHeader *headers;
  usize headers_len;
  usize headers_cap;

  BytesBuffer resp;
} HttpHandler;

struct HttpServer {
  Logger logger;
  // Owned by the server. The pool is carved out of it.
  Arena arena;
  Pool handler_pool;
  const Env *env;
};

// `inflight_requests_max` is rounded up to a whole slot group.
__attribute__((warn_unused_result)) static Error
http_server_init(HttpServer *server, Arena arena, usize inflight_requests_max,
                 u32 log_level_mask, const Env *env) {
  assert(server);
  assert(inflight_requests_max > 0);

  *server = (HttpServer){
      .logger = logger_make(log_level_mask, (Bytes){0}),
      .arena = arena,
      .env = env,
  };

  const usize slots_len = usize_round_up_multiple_of(inflight_requests_max,
                                                     HTTP_POOL_SLOTS_PER_GROUP);
  const usize groups_len = slots_len / HTTP_POOL_SLOTS_PER_GROUP;
  assert(groups_len > 0);
  assert(groups_len * HTTP_POOL_SLOTS_PER_GROUP == slots_len);

  HttpPoolSlotGroup *const occupied =
      arena_alloc(&server->arena, __alignof__(HttpPoolSlotGroup),
                  sizeof(HttpPoolSlotGroup), groups_len);
  if (!occupied) {
    return (Error){.kind = ErrKindOOM};
  }

  HttpHandler *const slots = arena_alloc(
      &server->arena, __alignof__(HttpHandler), sizeof(HttpHandler), slots_len);
  if (!slots) {
    return (Error){.kind = ErrKindOOM};
  }

  // The arena does not promise zeroed memory.
  memset(occupied, 0, groups_len * sizeof(HttpPoolSlotGroup));
  memset(slots, 0, slots_len * sizeof(HttpHandler));

  Error err =
      pool_make(&server->handler_pool, &arena, sizeof(HttpHandler), 1 << 14);
  if (ErrKindNone != err.kind) {
    return err;
  }

  return (Error){.kind = ErrKindNone};
}

static void http_handler_init(HttpHandler *handler, HttpServer *server, IO *io,
                              Ipv4Addr addr, i32 socket) {
  assert(handler);

  char log_prefix[32] = {0};
  const u32 ip = addr.ip;

  snprintf(log_prefix, sizeof(log_prefix), "[peer %u.%u.%u.%u:%hu] ",
           ip >> 24 & 0xff, ip >> 16 & 0xff, ip >> 8 & 0xff, ip >> 0 & 0xff,
           addr.port);

  handler->logger =
      logger_make(server->logger.level_mask, bytes_from_cstr(log_prefix));
  handler->socket = socket;
  handler->server = server;
  handler->io = io;
  handler->completion.ctx = handler;
  // FIXME: Pools.
  assert(ErrKindNone ==
         arena_valloc(server->env, 1 * MiB /* TODO: Revisit */, &handler->arena)
             .kind);
  assert(ErrKindNone ==
         bytes_buffer_make(8 * KiB, &handler->arena, &handler->recv).kind);
  handler->headers_cap = 512;
  handler->headers = arena_alloc(&handler->arena, __alignof__(HttpHeader),
                                 sizeof(HttpHeader), handler->headers_cap);
  assert(handler->headers);
}

__attribute__((warn_unused_result)) static Error
http_parse_req_status_line(Bytes src, HttpRequestStatusLine *res,
                           usize *advanced) {
  assert(res);
  assert(advanced);
  if (!src.len) {
    return (Error){0};
  }

  assert(src.data);

  Bytes remaining = src;

  Split split = bytes_split(remaining, bytes_from_cstr(" "));
  if (!split.found) {
    return (Error){.kind = ErrKindInvalidData};
  }
  const Bytes method = split.left;
  if (bytes_eq_cstr(method, "UNKNOWN")) {
    res->method = HTTP_METHOD_UNKNOWN;
  } else if (bytes_eq_cstr(method, "OPTIONS")) {
    res->method = HTTP_METHOD_OPTIONS;
  } else if (bytes_eq_cstr(method, "GET")) {
    res->method = HTTP_METHOD_GET;
  } else if (bytes_eq_cstr(method, "HEAD")) {
    res->method = HTTP_METHOD_HEAD;
  } else if (bytes_eq_cstr(method, "POST")) {
    res->method = HTTP_METHOD_POST;
  } else if (bytes_eq_cstr(method, "PUT")) {
    res->method = HTTP_METHOD_PUT;
  } else if (bytes_eq_cstr(method, "DELETE")) {
    res->method = HTTP_METHOD_DELETE;
  } else if (bytes_eq_cstr(method, "TRACE")) {
    res->method = HTTP_METHOD_TRACE;
  } else if (bytes_eq_cstr(method, "CONNECT")) {
    res->method = HTTP_METHOD_CONNECT;
  } else if (bytes_eq_cstr(method, "EXTENSION")) {
    res->method = HTTP_METHOD_EXTENSION;
  } else {
    return (Error){.kind = ErrKindInvalidData};
  }

  remaining = split.right;
  split = bytes_split(remaining, bytes_from_cstr(" "));
  if (!split.found) {
    return (Error){.kind = ErrKindInvalidData};
  }
  if (0 == split.left.len) {
    return (Error){.kind = ErrKindInvalidData};
  }
  res->url = split.left;

  remaining = split.right;
  split = bytes_split(remaining, bytes_from_cstr("\r\n"));
  if (!split.found) {
    return (Error){.kind = ErrKindInvalidData};
  }
  if (0 == split.left.len) {
    return (Error){.kind = ErrKindInvalidData};
  }

  remaining = split.right;

  const Bytes http_version = split.left;
  split = bytes_split(http_version, bytes_from_cstr("/"));
  if (!split.found) {
    return (Error){.kind = ErrKindInvalidData};
  }
  if (!bytes_eq_cstr(split.left, "HTTP")) {
    return (Error){.kind = ErrKindInvalidData};
  }
  // FIXME
  res->version_major = 1;
  res->version_minor = 1;

  *advanced = src.len - remaining.len;
  return (Error){0};
}
