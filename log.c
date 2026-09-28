#pragma once

#include "lib.c"
#include <stdarg.h>

typedef enum {
  LogLevelDebug = 1,
  LogLevelInfo = 2,
  LogLevelError = 4,
  LogLevelAll = LogLevelDebug | LogLevelInfo | LogLevelError,
} LogLevel;

typedef struct {
  u32 level_mask;
  u8 prefix[32];
} Logger;

__attribute__((warn_unused_result)) static Logger logger_make(u32 level_mask,
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
//
static void log_err(const Logger *logger, const char *context, Error err) {
  assert(context);

  if (0 == err.data) {
    log(logger, LogLevelError, "%s: %s\n", context,
        error_kind_to_cstr(err.kind));
    return;
  }

  char os_msg[256] = {0};

  if (!platform_error_describe(err.data, os_msg, sizeof(os_msg))) {
    // The description did not fit or the number is not one the system knows;
    // the number itself is still worth printing.
    log(logger, LogLevelError, "%s: %s (errno %" PRIu64 ")\n", context,
        error_kind_to_cstr(err.kind), err.data);
    return;
  }

  log(logger, LogLevelError, "%s: %s (errno %" PRIu64 ": %s)\n", context,
      error_kind_to_cstr(err.kind), err.data, os_msg);
}
