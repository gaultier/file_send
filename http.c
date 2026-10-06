#pragma once

#include "lib.c"

typedef struct {
  Bytes key;
  Bytes value;
} HttpHeader;

__attribute__((warn_unused_result)) static Find
http_find_headers_end(Bytes haystack) {
  return bytes_find(haystack, bytes_from_cstr("\r\n\r\n"),
                    FindOptionsIndexAfterNeedleEnd);
}

__attribute__((warn_unused_result)) static Error
http_parse_headers(Bytes src, HttpHeader *headers, usize *headers_len,
                   usize headers_cap) {
  assert(headers);
  assert(headers_len > 0);
  assert(headers_cap > 0);

  if (0 == src.len) {
    return (Error){0};
  }

  assert(src.data);

  Bytes remaining = src;
  const Bytes header_separator = bytes_from_cstr("\r\n");
  const Bytes key_value_separator = bytes_from_cstr(": ");
  const Bytes end = bytes_from_cstr("\r\n\r\n");

  for (usize _i = 0; _i < src.len; _i++) {
    assert(*headers_len <= headers_cap);
    assert(remaining.len <= src.len);

    if (0 == remaining.len) {
      break;
    }

    Split header_split = bytes_split(remaining, header_separator);
    if (!header_split.found) {
      break;
    }

    if (0 == header_split.left.len) {
      break;
    }

    // Found a new header but there is no more room: OOM.
    if (*headers_len == headers_cap) {
      return (Error){.kind = ErrKindOOM};
    }

    remaining = header_split.right;

    const Bytes kv = header_split.left;

    Split kv_split = bytes_split(kv, key_value_separator);
    if (!kv_split.found) {
      return (Error){.kind = ErrKindInvalidData};
    }

    if (0 == kv_split.left.len) {
      return (Error){.kind = ErrKindInvalidData};
    }

    if (0 == kv_split.right.len) {
      return (Error){.kind = ErrKindInvalidData};
    }

    headers[*headers_len] =
        (HttpHeader){.key = kv_split.left, .value = kv_split.right};
    *headers_len += 1;
    assert(*headers_len <= headers_cap);
    assert(*headers_len <= _i);
  }

  if (bytes_starts_with(remaining, end)) {
    return (Error){0};
  }

  return (Error){.kind = ErrKindInvalidData};
}
