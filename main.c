#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef uint8_t u8;
typedef int i32;
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
  usize consumed;
  bool ok;
} ParseUsize;

typedef struct {
  usize len;
  u8 *data;
} Slice_u8;

typedef enum {
  BencodeKindInteger,
  BencodeKindString,
  BencodeKindList,
  BencodeKindDict,
} BencodeKind;

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
    Slice_u8 s;
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
  Slice_u8 data;
} BencodeParser;

typedef struct {
  u8 value;
  bool ok;
} At_U8;

static bool char_is_digit_ascii(u8 c) { return '0' <= c && c <= '9'; }

static void *arena_alloc(Arena *arena, usize align, usize elem_size,
                         usize elem_count) {
  assert(arena != NULL);
  assert(arena->start != NULL);
  assert(arena->end != NULL);
  assert(arena->start <= arena->end);
  assert((align == 1) || (align == 2) || (align == 4) || (align == 8));
  assert(elem_size > 0);
  assert(elem_count > 0);

  usize start = (usize)arena->start;

  // Round `start` up to the next multiple of `align`.
  const usize pad = (align - (start % align)) % align;
  assert(!__builtin_add_overflow(start, pad, &start));

  usize alloc_size = 0;
  assert(!__builtin_mul_overflow(elem_size, elem_count, &alloc_size));

  usize end = 0;
  assert(!__builtin_add_overflow(start, alloc_size, &end));

  // OOM? The arena is left untouched in that case.
  if (end > (usize)arena->end) {
    return NULL;
  }

  arena->start = (u8 *)end;
  assert(arena->start <= arena->end);
  assert((start % align) == 0);

  return (u8 *)start;
}

static Arena arena_from_mem(u8 *mem, usize bytes_count) {
  assert(mem);
  assert(bytes_count);

  usize end = 0;
  assert(!__builtin_add_overflow((usize)mem, bytes_count, &end));
  return (Arena){.start = mem, .end = (u8 *)end};
}

static u8 *unix_virtual_mem_alloc(usize bytes_count) {
  assert(bytes_count > 0);
  void *alloc = mmap(NULL, bytes_count, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
  return alloc;
}

static Arena arena_valloc(usize bytes_count) {
  u8 *const arena_memory = unix_virtual_mem_alloc(bytes_count);
  Arena res = {0};

  if (arena_memory == NULL) {
    fprintf(stderr, "failed to allocate virtual memory: %zu bytes\n",
            bytes_count);
    return res;
  }

  return arena_from_mem(arena_memory, bytes_count);
}

static At_U8 slice_u8_first(Slice_u8 slice) {
  At_U8 res = {0};

  if (!slice.data) {
    return res;
  }

  if (slice.len == 0) {
    return res;
  }

  res.value = slice.data[0];
  res.ok = true;
  return res;
}

static bool slice_u8_skip(Slice_u8 *slice, usize count) {
  assert(slice);
  if (!slice->data) {
    return true;
  }

  if (slice->len < count) {
    return false;
  }

  slice->len -= count;
  slice->data += count;
  return true;
}

static Slice_u8 slice_u8_take(Slice_u8 input, usize count) {
  Slice_u8 res = {.data = input.data,
                  .len = count > input.len ? input.len : count};
  return res;
}

static Slice_u8 slice_u8_make(u8 *data, usize len) {
  return (Slice_u8){.data = data, .len = len};
}

static bool bencode_list_push(BencodeList *list, BencodeValue item,
                              Arena *arena) {
  assert(list);
  assert(list->len <= list->cap);
  assert(arena);

  const usize initial_cap = 8;
  const usize growth_factor = 2;

  // Initial alloc.
  if (list->cap == 0) {
    list->cap = initial_cap;
    list->data = arena_alloc(arena, __alignof__(BencodeValue),
                             sizeof(BencodeValue), list->cap);
    if (!list->data) {
      return false;
    }
  }

  if (list->len == list->cap) {
    assert(list->cap >= initial_cap);

    const usize cap_before = list->cap;
    assert(!__builtin_mul_overflow(list->cap, growth_factor, &list->cap));
    assert(cap_before < list->cap);

    const bool in_place_extend_possible =
        (usize)arena->start == ((usize)list->data + list->len * sizeof(item));
    if (in_place_extend_possible) {
      usize bytes_after = list->cap;
      assert(!__builtin_mul_overflow(list->cap, sizeof(item), &bytes_after));

      assert(!__builtin_add_overflow((usize)arena->start, bytes_after,
                                     (usize *)&arena->start));

      // OOM.
      if (arena->start > arena->end) {
        return false;
      }
    } else {
      void *const bck = list->data;
      list->data = arena_alloc(arena, __alignof__(BencodeValue),
                               sizeof(BencodeValue), list->cap);
      memcpy(bck, list->data, sizeof(item) * list->len);
    }
  }

  list->data[list->len++] = item;

  return true;
}

static bool bencode_parse_consume(BencodeParser *parser, u8 expected) {
  assert(parser);
  assert(parser->data.data);

  At_U8 actual = slice_u8_first(parser->data);

  if (!actual.ok) {
    return false;
  }
  if (actual.value != expected) {
    return false;
  }

  assert(slice_u8_skip(&parser->data, 1));
  return true;
}

static ParseUsize ascii_num_parse(Slice_u8 data) {
  ParseUsize res = {0};
  if (!data.data) {
    return res;
  }

  const usize MAX_LEN = 30;
  bool has_leading_zero = false;

  for (; res.consumed < MAX_LEN; res.consumed++) {
    const At_U8 current = slice_u8_first(data);

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

    has_leading_zero = current.value == '0' && res.consumed == 0;

    // Leading zeroes forbidden except `i0e`.
    if (res.consumed > 0 && has_leading_zero) {
      return res;
    }

    const usize digit = current.value - '0';
    if (__builtin_mul_overflow(res.num, 10, &res.num)) {
      return res;
    }
    if (__builtin_add_overflow(res.num, digit, &res.num)) {
      return res;
    }

    assert(slice_u8_skip(&data, 1));
  }

  // Actually unreachable.
  return res;
}

// `i123e`
// `i-123e`
static BencodeParseResult bencode_parse_num(BencodeParser *parser) {
  assert(parser);
  assert(parser->data.data);

  BencodeParseResult res = {0};
  if (!bencode_parse_consume(parser, 'i')) {
    return res;
  };
  res.bencode.kind = BencodeKindInteger;

  bool negative_sign = bencode_parse_consume(parser, '-');

  ParseUsize parsed_usize = ascii_num_parse(parser->data);

  if (!parsed_usize.ok) {
    return res;
  }

  assert(slice_u8_skip(&parser->data, parsed_usize.consumed));
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
static BencodeParseResult bencode_parse_string(BencodeParser *parser) {
  assert(parser);
  assert(parser->data.data);

  BencodeParseResult res = {0};

  const At_U8 first = slice_u8_first(parser->data);
  if (!first.ok) {
    return res;
  }

  if (!char_is_digit_ascii(first.value)) {
    return res;
  }

  res.bencode.kind = BencodeKindString;

  ParseUsize parsed_usize = ascii_num_parse(parser->data);
  if (!parsed_usize.ok) {
    return res;
  }
  assert(slice_u8_skip(&parser->data, parsed_usize.consumed));

  if (!bencode_parse_consume(parser, ':')) {
    return res;
  };

  if (parsed_usize.num > parser->data.len) {
    return res;
  }

  res.bencode.v.s = slice_u8_take(parser->data, parsed_usize.num);
  assert(slice_u8_skip(&parser->data, parsed_usize.num));
  res.ok = true;
  return res;
}

static BencodeParseResult bencode_parse(BencodeParser *parser, Arena *arena,
                                        Arena scratch);

// FIXME: rec.
static BencodeParseResult bencode_parse_list(BencodeParser *parser,
                                             Arena *arena, Arena scratch) {
  assert(parser);

  BencodeParseResult res = {0};

  if (!bencode_parse_consume(parser, 'l')) {
    return res;
  }

  res.bencode.kind = BencodeKindList;

  const usize remaining_bytes = parser->data.len;
  for (usize _i = 0; _i < remaining_bytes; _i++) {
    if (bencode_parse_consume(parser, 'e')) {
      res.ok = true;
      return res;
    }

    BencodeParseResult item = bencode_parse(parser, arena, scratch);
    if (!item.ok) {
      return res;
    }

    if (!bencode_list_push(&res.bencode.v.list, item.bencode, arena)) {
      return res;
    }
  }

  return res;
}

static BencodeParseResult bencode_parse(BencodeParser *parser, Arena *arena,
                                        Arena scratch) {
  assert(parser);
  assert(arena);

  BencodeParseResult res = {0};

  const usize MAX_LEN = parser->data.len;

  for (usize _i = 0; _i < MAX_LEN; _i++) {
    const At_U8 current = slice_u8_first(parser->data);
    if (!current.ok) {
      return res;
    }

    switch (current.value) {
    case 'i':
      return bencode_parse_num(parser);
    case 'l':
      return bencode_parse_list(parser, arena, scratch);
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
      return bencode_parse_string(parser);
    default:
      return res;
    }
  }

  return res;
}

void test() {
  {
  }
  {
    const char *const bencode_input = "i-123e";

    BencodeParser parser = {
        .data = slice_u8_make((u8 *)bencode_input, strlen(bencode_input)),
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
        .data = slice_u8_make((u8 *)bencode_input, strlen(bencode_input)),
    };
    BencodeParseResult parse_res = bencode_parse_string(&parser);
    __builtin_dump_struct(&parse_res, &printf);
    assert(parse_res.ok);
    assert(parse_res.bencode.kind == BencodeKindString);
    assert(parse_res.bencode.v.s.len == 4);
    assert(__builtin_memcmp(parse_res.bencode.v.s.data, "spam", 4) == 0);
  }
  {
    Arena arena = arena_valloc(1 * KiB);
    Arena scratch = arena_valloc(1 * KiB);

    const char *const bencode_input = "l4:spami456ee";

    BencodeParser parser = {
        .data = slice_u8_make((u8 *)bencode_input, strlen(bencode_input)),
    };
    BencodeParseResult parse_res = bencode_parse(&parser, &arena, scratch);
    assert(parse_res.ok);
    assert(parse_res.bencode.kind == BencodeKindList);
    assert(parse_res.bencode.v.list.len == 2);
  }
}

int main() { test(); }
