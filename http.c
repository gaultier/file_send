#pragma once

#include "lib.c"

__attribute__((warn_unused_result)) static Find
http_find_headers_end(Bytes haystack) {
  return bytes_find(haystack, bytes_from_cstr("\r\n\r\n"),
                    FindOptionsIndexAfterNeedleEnd);
}
