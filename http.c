#pragma once

#include "lib.c"

// Both borrow from the parsed input.
typedef struct {
  Bytes key;
  Bytes value;
} HttpHeader;

typedef enum {
  HTTP_METHOD_OPTIONS,
  HTTP_METHOD_GET,
  HTTP_METHOD_HEAD,
  HTTP_METHOD_POST,
  HTTP_METHOD_PUT,
  HTTP_METHOD_DELETE,
  HTTP_METHOD_TRACE,
  HTTP_METHOD_CONNECT,
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
  const Env *env;

  Pool handler_pool;
  Pool memory_blocks_pool;
};

#define HTTP_HANDLER_ARENA_SIZE (20 * KiB)

__attribute__((warn_unused_result)) static Error
http_server_init(HttpServer *server, Arena arena, usize inflight_requests_max,
                 u32 log_level_mask, const Env *env) {
  assert(server);
  assert(inflight_requests_max > 0);

  *server = (HttpServer){
      .logger = logger_make(log_level_mask, (Bytes){0}),
      .env = env,
  };

  Error err = pool_make(&server->handler_pool, &arena, sizeof(HttpHandler),
                        inflight_requests_max);
  if (ErrKindNone != err.kind) {
    return err;
  }

  err = pool_make(&server->memory_blocks_pool, &arena, HTTP_HANDLER_ARENA_SIZE,
                  inflight_requests_max);
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
  u8 *const handler_memory = pool_acquire(&server->memory_blocks_pool);
  assert(handler_memory);
  handler->arena = arena_from_mem(handler_memory, HTTP_HANDLER_ARENA_SIZE);

  assert(ErrKindNone ==
         bytes_buffer_make(8 * KiB, &handler->arena, &handler->recv).kind);
  handler->headers_cap = 128;
  handler->headers = arena_alloc(&handler->arena, __alignof__(HttpHeader),
                                 sizeof(HttpHeader), handler->headers_cap);
  assert(handler->headers);
}

// Gives back what `http_handler_init` took. `handler` is not to be used after.
static void http_handler_release(HttpHandler *handler) {
  assert(handler);
  HttpServer *const server = handler->server;
  assert(server);

  // `arena.start` moves as the arena is used, `arena.end` does not.
  pool_release(&server->memory_blocks_pool,
               handler->arena.end - HTTP_HANDLER_ARENA_SIZE);
  pool_release(&server->handler_pool, handler);
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
  if (bytes_eq_cstr(method, "OPTIONS")) {
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
  // `DIGIT "." DIGIT` (RFC 9112 section 2.3). Which versions are supported is
  // up to the caller.
  const Bytes version = split.right;
  if (3 != version.len || !char_is_digit_ascii(version.data[0]) ||
      '.' != version.data[1] || !char_is_digit_ascii(version.data[2])) {
    return (Error){.kind = ErrKindInvalidData};
  }
  res->version_major = (u8)(version.data[0] - '0');
  res->version_minor = (u8)(version.data[2] - '0');
  assert(res->version_major <= 9);
  assert(res->version_minor <= 9);

  *advanced = src.len - remaining.len;
  return (Error){0};
}
