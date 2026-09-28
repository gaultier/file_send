#pragma once

#include "lib.c"
#include <stdarg.h>

typedef enum {
  LogLevelDebug = 1,
  LogLevelInfo = 2,
  LogLevelError = 4
} LogLevel;

typedef struct {
  u32 level_mask;
  u8 prefix[32];
} Logger;

__attribute__((warn_unused_result)) static Logger log_make(u32 level_mask,
                                                           Slice_u8 prefix) {
  Logger logger = {.level_mask = level_mask};
  memcpy(&logger.prefix, prefix.data,
         prefix.len < sizeof(logger.prefix) ? prefix.len
                                            : sizeof(logger.prefix));

  return logger;
}

static void log(const Logger *logger, LogLevel level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void log(const Logger *logger, LogLevel level, const char *fmt, ...) {
  assert(logger);
  assert(fmt);

  if (0 == (level & logger->level_mask)) {
    return;
  }

  printf("%.*s", (i32)sizeof(logger->prefix), logger->prefix);

  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);

  printf("\n");
}
