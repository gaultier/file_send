#pragma once

#include "lib.c"

// Both borrow from the parsed input.
typedef struct {
  Bytes key;
  Bytes value;
} HttpHeader;

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
  __builtin_unreachable();
}
