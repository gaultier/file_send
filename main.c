#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

typedef uint8_t u8;
typedef size_t usize;
typedef ssize_t isize;

const usize KiB = 1024;
const usize MiB = 1024 * KiB;

typedef struct {
  // Start of the arena allocation.
  // Increases with each allocation.
  u8 *start;
  // Size of the full arena.
  // Only used to detect the OOM case.
  u8 *end;
} Arena;

typedef struct {
  usize num;
  usize pos;
  bool ok;
} ParseUsize;

typedef enum {
  BencodeKindInteger,
  BencodeKindString,
  BencodeKindList,
  BencodeKindDict,
} BencodeKind;

typedef struct {
  usize len;
  u8 *data;
} BencodeString;

typedef struct BencodeValue BencodeValue;

typedef struct {
  usize len;
  usize cap;
  BencodeValue *data;
} BencodeList;

struct BencodeValue {
  BencodeKind kind;
  union {
    isize num;
    BencodeString s;
    BencodeList list;
  } v;
};

typedef enum {
  BencodeParseKindOk,
  BencodeParseKindUnterminated,
  BencodeParseKindUnexpectedCharacter,
} BencodeParseResultKind;

typedef struct {
  BencodeValue bencode;
  bool ok;
} BencodeParseResult;

typedef struct {
  u8 *data;
  usize len;
  usize pos;
} BencodeParser;

typedef struct {
  u8 value;
  bool ok;
} At_U8;

bool char_is_digit_ascii(u8 c) { return '0' <= c && c <= '9'; }

void *arena_alloc(Arena *arena, usize align, usize elem_size,
                  usize elem_count) {
  assert(arena != NULL);
  assert(arena->start != NULL);
  assert(arena->end != NULL);
  assert((align == 1) || (align == 2) || (align == 4) || (align == 8));
  assert(elem_size > 0);
  assert(elem_count > 0);

  const usize start_before = (usize)arena->start;
  usize start = (usize)arena->start;

  const usize pad = start % align;
  assert(!__builtin_add_overflow(start, pad, &start));

  u8 *const res = (u8 *)arena->start;

  {
    usize alloc_size = 0;
    assert(!__builtin_mul_overflow(elem_size, elem_count, &alloc_size));
    assert(!__builtin_add_overflow(start, alloc_size, &start));
  }

  arena->start = (u8 *)start;
  assert(start_before < (usize)arena->start);
  assert(arena->start <= arena->end); // OOM?

  return res;
}

Arena arena_from_mem(u8 *mem, usize bytes_count) {
  assert(mem);
  assert(bytes_count);

  usize end = 0;
  assert(!__builtin_add_overflow((usize)mem, bytes_count, &end));
  return (Arena){.start = mem, .end = (u8 *)end};
}

u8 *unix_virtual_mem_alloc(usize bytes_count) {
  assert(bytes_count > 0);
  void *alloc = mmap(NULL, bytes_count, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
  return alloc;
}

At_U8 slice_u8_at(u8 *data, usize len, usize idx) {
  At_U8 res = {0};

  if (!data) {
    return res;
  }

  if (idx >= len) {
    return res;
  }

  res.value = data[idx];
  res.ok = true;
  return res;
}

usize slice_remaining(usize len, usize idx) {
  if (len == 0) {
    return 0;
  }

  if (idx >= len) {
    return 0;
  }

  return len - idx;
}

u8 *slice_u8_offset(u8 *data, usize len, usize idx) {
  assert(data);
  assert(idx < len);

  return data + idx;
}

bool bencode_list_push(BencodeList *list, BencodeValue item, Arena *arena) {
  assert(list);
  assert(list->len <= list->cap);
  assert(arena);

  // Initial alloc.
  if (list->cap == 0) {
    list->cap = 8;
    list->data = arena_alloc(arena, __alignof__(BencodeValue),
                             sizeof(BencodeValue), list->cap);
    if (!list->data) {
      return false;
    }
  }

  if (list->len == list->cap) {
    assert(0 && "realloc");
  }

  list->data[list->len++] = item;

  return true;
}

At_U8 bencode_parser_at(BencodeParser parser) {
  return slice_u8_at(parser.data, parser.len, parser.pos);
}

void bencode_parser_advance(BencodeParser *parser, usize count) {
  assert(!__builtin_add_overflow(parser->pos, count, &parser->pos));
}

bool bencode_parse_consume(BencodeParser *parser, u8 expected) {
  assert(parser);

  At_U8 actual = bencode_parser_at(*parser);

  if (!actual.ok) {
    return false;
  }
  if (actual.value != expected) {
    return false;
  }

  bencode_parser_advance(parser, 1);
  return true;
}

ParseUsize ascii_num_parse(u8 *data, usize len) {
  const usize MAX_LEN = 30;
  ParseUsize res = {0};
  bool has_leading_zero = false;

  for (; res.pos < MAX_LEN; res.pos++) {
    const At_U8 current = slice_u8_at(data, len, res.pos);

    // Unterminated.
    if (!current.ok) {
      return res;
    }

    assert(current.ok);

    // End.
    if (!char_is_digit_ascii(current.value)) {
      res.ok = true;
      return res;
    }

    has_leading_zero = current.value == '0' && res.pos == 0;

    // Leading zeroes forbidden except `i0e`.
    if (res.pos > 0 && has_leading_zero) {
      return res;
    }

    const usize digit = current.value - '0';
    if (__builtin_mul_overflow(res.num, 10, &res.num)) {
      return res;
    }
    if (__builtin_add_overflow(res.num, digit, &res.num)) {
      return res;
    }
  }

  // Actually unreachable.
  return res;
}

// `i123e`
// `i-123e`
BencodeParseResult bencode_parse_num(BencodeParser *parser) {
  assert(parser);
  assert(parser->data);
  assert(parser->pos < parser->len);

  BencodeParseResult res = {0};
  if (!bencode_parse_consume(parser, 'i')) {
    return res;
  };
  res.bencode.kind = BencodeKindInteger;

  bool negative_sign = bencode_parse_consume(parser, '-');

  ParseUsize parsed_usize =
      ascii_num_parse(slice_u8_offset(parser->data, parser->len, parser->pos),
                      parser->len - parser->pos);

  if (!parsed_usize.ok) {
    return res;
  }

  bencode_parser_advance(parser, parsed_usize.pos);
  res.bencode.v.num = parsed_usize.num;
  if (negative_sign) {
    if (parsed_usize.num > SSIZE_MAX) {
      return res;
    }

    res.bencode.v.num = -1 * (isize)(parsed_usize.num);
  } else {
    res.bencode.v.num = parsed_usize.num;
  }

  if (!bencode_parse_consume(parser, 'e')) {
    return res;
  };

  res.ok = true;
  return res;
}

// `4:spam`
BencodeParseResult bencode_parse_string(BencodeParser *parser) {
  assert(parser);
  assert(parser->data);
  assert(parser->pos < parser->len);

  BencodeParseResult res = {0};

  const At_U8 first = bencode_parser_at(*parser);
  if (!first.ok) {
    return res;
  }

  if (!char_is_digit_ascii(first.value)) {
    return res;
  }

  res.bencode.kind = BencodeKindString;

  ParseUsize parsed_usize = ascii_num_parse(parser->data, parser->len);
  if (!parsed_usize.ok) {
    return res;
  }
  bencode_parser_advance(parser, parsed_usize.pos);

  if (!bencode_parse_consume(parser, ':')) {
    return res;
  };

  res.bencode.v.s.data =
      slice_u8_offset(parser->data, parser->len, parser->pos);

  const usize remaining_bytes = slice_remaining(parser->len, parser->pos);
  if (parsed_usize.num > remaining_bytes) {
    return res;
  }

  res.bencode.v.s.len = parsed_usize.num;
  res.ok = true;
  return res;
}

BencodeParseResult bencode_parse(BencodeParser *parser, Arena *arena);

// FIXME: rec.
BencodeParseResult bencode_parse_list(BencodeParser *parser, Arena *arena) {
  assert(parser);
  assert(parser->data);
  assert(parser->pos < parser->len);

  BencodeParseResult res = {0};

  if (!bencode_parse_consume(parser, 'l')) {
    return res;
  }

  res.bencode.kind = BencodeKindList;

  const usize remaining_bytes = slice_remaining(parser->len, parser->pos);
  for (usize _i = 0; _i < remaining_bytes; _i++) {
    BencodeParseResult item = bencode_parse(parser, arena);
    if (!item.ok) {
      return res;
    }

    if (!bencode_list_push(&res.bencode.v.list, item.bencode, arena)) {
      return res;
    }
  }

  if (!bencode_parse_consume(parser, 'e')) {
    return res;
  }

  res.ok = true;
  return res;
}

BencodeParseResult bencode_parse(BencodeParser *parser, Arena *arena) {
  assert(parser);
  assert(parser->data);
  assert(arena);

  BencodeParseResult res = {0};

  for (usize _i = 0; _i < parser->len; _i++) {
    const At_U8 current = bencode_parser_at(*parser);
    if (!current.ok) {
      return res;
    }

    switch (current.value) {
    case 'i':
      return bencode_parse_num(parser);
    case 'l':
      return bencode_parse_list(parser, arena);
    case 'd':
      assert(0 && "todo");
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return bencode_parse_num(parser);
    default:
      return res;
    }
  }

  return res;
}

int main() {

  {
    const char *const bencode_input = "i-123e";

    BencodeParser parser = {
        .data = (u8 *)bencode_input,
        .len = strlen(bencode_input),
    };
    BencodeParseResult parse_res = bencode_parse_num(&parser);
    __builtin_dump_struct(&parse_res, &printf);
    assert(parse_res.ok);
    assert(parse_res.bencode.kind == BencodeKindInteger);
    assert(parse_res.bencode.v.num == -123);
  }
  {
    const char *const bencode_input = "4:spam";

    BencodeParser parser = {
        .data = (u8 *)bencode_input,
        .len = strlen(bencode_input),
        .pos = 0,
    };
    BencodeParseResult parse_res = bencode_parse_string(&parser);
    __builtin_dump_struct(&parse_res, &printf);
    assert(parse_res.ok);
    assert(parse_res.bencode.kind == BencodeKindString);
    assert(parse_res.bencode.v.s.len == 4);
    assert(__builtin_memcmp(parse_res.bencode.v.s.data, "spam", 4) == 0);
  }
  {
    const usize arena_memory_bytes_count = 1 * KiB;
    u8 *arena_memory = unix_virtual_mem_alloc(arena_memory_bytes_count);
    if (arena_memory == NULL) {
      fprintf(stderr, "failed to allocate virtual memory: %zu bytes\n",
              arena_memory_bytes_count);
      return 1;
    }

    Arena arena = arena_from_mem(arena_memory, arena_memory_bytes_count);

    const char *const bencode_input = "l4:spami456ee";

    BencodeParser parser = {
        .data = (u8 *)bencode_input,
        .len = strlen(bencode_input),
    };
    BencodeParseResult parse_res = bencode_parse(&parser, &arena);
    __builtin_dump_struct(&parse_res, &printf);
    assert(parse_res.ok);
    assert(parse_res.bencode.kind == BencodeKindList);
    assert(parse_res.bencode.v.list.len == 2);
  }

  return 0;
}
