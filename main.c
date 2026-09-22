#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef uint8_t u8;
typedef int32_t i32;
typedef int64_t i64;
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
  bool is_list;
  usize children_start;
} BencodeContainer;

typedef struct {
  usize len;
  BencodeValue *data;
} BencodeList;

struct BencodeValue {
  BencodeKind kind;
  union {
    isize num;        // Integer
    Slice_u8 s;       // String
    BencodeList list; // List or Dict (stored as contiguous key-value pairs)
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

// Convert `magnitude`, optionally negated, to an isize.
// Returns false if the value does not fit.
static bool isize_from_usize(usize magnitude, bool negative, isize *res) {
  assert(res);

  // The overflow builtins compute in infinite precision and report whether the
  // result fits the *destination* type, so both of these are checked
  // conversions: `0 - magnitude` is a checked negation, `magnitude + 0` a
  // checked cast. This handles the asymmetric boundary (`|ISIZE_MIN|` is a
  // valid magnitude but not a valid positive value) with no special case.
  return negative ? !__builtin_sub_overflow(0, magnitude, res)
                  : !__builtin_add_overflow(magnitude, 0, res);
}

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

  const Arena res = {.start = mem, .end = (u8 *)end};
  assert(res.start <= res.end);
  return res;
}

static u8 *unix_virtual_mem_alloc(usize bytes_count) {
  assert(bytes_count > 0);
  void *alloc = mmap(NULL, bytes_count, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);

  if ((void *)-1 == alloc) {
    return NULL;
  }

  return alloc;
}

static usize unix_get_page_size(void) {
  i64 res = sysconf(_SC_PAGE_SIZE);
  if (res == -1) {
    return 0;
  }

  return (usize)res;
}

static bool unix_vprotect_none(void *ptr, usize size) {
  if (-1 == mprotect(ptr, size, PROT_NONE)) {
    return false;
  }
  return true;
}

// `multiple` must be a power of two, which every page size is.
static usize usize_round_up_multiple_of(usize n, usize multiple) {
  assert(multiple != 0);
  assert(0 == (multiple & (multiple - 1)) && "not a power of two");

  usize res = 0;
  assert(!__builtin_add_overflow(n, multiple - 1, &res));
  res &= ~(multiple - 1);

  assert(0 == (res & (multiple - 1)));
  assert(res >= n);
  assert(res - n < multiple);
  return res;
}

static Arena arena_valloc(usize bytes_count) {
  const usize page_size = unix_get_page_size();
  assert(page_size > 0);

  const usize usable_bytes = usize_round_up_multiple_of(bytes_count, page_size);
  usize os_alloc_size = 0;
  // Guard page.
  assert(!__builtin_add_overflow(usable_bytes, page_size, &os_alloc_size));

  u8 *const arena_memory = unix_virtual_mem_alloc(os_alloc_size);
  Arena res = {0};

  if (arena_memory == NULL) {
    return res;
  }

  assert(unix_vprotect_none(arena_memory + usable_bytes, page_size));

  // Right-align the arena against the guard page so that *any* write past
  // `arena.end` faults immediately, then round the start down to the
  // strictest alignment `arena_alloc` hands out. Rounding down can only make
  // the arena slightly larger than requested, never smaller.
  const usize max_align = 8;
  usize start = (usize)arena_memory + usable_bytes - bytes_count;
  start -= start % max_align;
  assert(start >= (usize)arena_memory);
  assert(0 == start % max_align);

  return arena_from_mem((u8 *)start,
                        (usize)arena_memory + usable_bytes - start);
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
    return false;
  }

  if (slice->len < count) {
    return false;
  }

  slice->len -= count;
  slice->data += count;
  return true;
}

// The caller must have already established that `count` bytes are available:
// silently returning a short slice would turn a malformed length into a
// successful parse of truncated data.
static Slice_u8 slice_u8_take(Slice_u8 input, usize count) {
  assert(count <= input.len);

  return (Slice_u8){.data = input.data, .len = count};
}

static Slice_u8 slice_u8_make(u8 *data, usize len) {
  assert(data || 0 == len);

  return (Slice_u8){.data = data, .len = len};
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
      assert(res.consumed < MAX_LEN);
      return res;
    }

    if (current.value == '0' && res.consumed == 0) {
      has_leading_zero = true;
    }

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

  // Unreachable: a usize overflows after at most 20 digits, so the overflow
  // checks above always return before `MAX_LEN` iterations.
  assert(0 && "unreachable");
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

  // No digit consumed e.g. `ie`: invalid.
  if (parsed_usize.consumed == 0) {
    return res;
  }

  assert(slice_u8_skip(&parser->data, parsed_usize.consumed));

  // `i-0e` is invalid bencode.
  if (negative_sign && parsed_usize.num == 0) {
    return res;
  }

  isize num = 0;
  if (!isize_from_usize(parsed_usize.num, negative_sign, &num)) {
    return res;
  }
  res.bencode.v.num = num;

  if (!bencode_parse_consume(parser, 'e')) {
    return res;
  };

  res.ok = true;
  assert(res.bencode.kind == BencodeKindInteger);
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
  assert(res.bencode.kind == BencodeKindString);
  assert(res.bencode.v.s.len == parsed_usize.num);
  assert(res.bencode.v.s.data || 0 == res.bencode.v.s.len);
  return res;
}

#define BENCODE_MAX_DEPTH 128

static BencodeParseResult bencode_parse(BencodeParser *parser, Arena *arena,
                                        Arena scratch) {
  assert(parser);
  assert(arena);
  assert(arena->start <= arena->end);
  assert(parser->data.data || 0 == parser->data.len);

  BencodeParseResult res = {0};

  // Nothing to do?
  if (0 == parser->data.len) {
    return res;
  }

  // At most, there are as many bencode values as `input bytes/2+1` since each
  // value takes at least 2 bytes.
  const usize values_cap = parser->data.len / 2 + 1;
  BencodeValue *values = arena_alloc(&scratch, __alignof__(BencodeValue),
                                     sizeof(BencodeValue), values_cap);
  // OOM?
  if (!values) {
    return res;
  }
  usize values_len = 0;

  BencodeContainer containers[BENCODE_MAX_DEPTH] = {0};
  usize containers_len = 0;

  const usize MAX_LEN = parser->data.len;

  for (usize _i = 0; _i < MAX_LEN; _i++) {
    const At_U8 current = slice_u8_first(parser->data);
    if (!current.ok) {
      return res;
    }

    switch (current.value) {
    case 'i': {
      const BencodeParseResult item = bencode_parse_num(parser);
      if (!item.ok) {
        return res;
      }

      assert(values_len < values_cap);
      values[values_len++] = item.bencode;
    } break;

    case 'l':
    case 'd':
      if (containers_len >= BENCODE_MAX_DEPTH) {
        return res;
      }
      assert(slice_u8_skip(&parser->data, 1));

      containers[containers_len].is_list = current.value == 'l';
      containers[containers_len].children_start = values_len;
      containers_len++;

      // The next loop iteration will parse the items.
      continue;

    case 'e': {
      if (0 == containers_len) {
        // Stray `e`, reject.
        return res;
      }

      assert(slice_u8_skip(&parser->data, 1));

      // Time to pop `containers`.
      const BencodeContainer container = containers[containers_len - 1];
      containers[containers_len - 1] = (BencodeContainer){0};
      containers_len--;

      const usize children_len = values_len - container.children_start;

      // Should always be key-value pairs.
      if (!container.is_list && children_len % 2 != 0) {
        return res;
      }

      // Now record the value for this container.
      BencodeValue value = {.kind = container.is_list ? BencodeKindList
                                                      : BencodeKindDict};
      // If there are any children, we need to allocate (right-sized) space for
      // them.
      if (children_len > 0) {
        BencodeValue *children =
            arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                        children_len);
        // OOM?
        if (!children) {
          return res;
        }

        // Copy the items from `values` to the new right-sized allocation.
        value.v.list.data = memcpy(children, values + container.children_start,
                                   children_len * sizeof(BencodeValue));
        value.v.list.len = children_len;
      }

      // Pop all the items for this container, at once. The popped slots are
      // scrubbed so that a stale child cannot be mistaken for a live value.
      memset(values + container.children_start, 0,
             children_len * sizeof(BencodeValue));
      values_len = container.children_start;
      assert(values_len < values_cap);

      // Do not forget to record this new bencode value!
      values[values_len++] = value;
    } break;

    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9': {
      BencodeParseResult item = bencode_parse_string(parser);
      if (!item.ok) {
        return res;
      }
      assert(values_len < values_cap);
      values[values_len++] = item.bencode;
    } break;

      // Unknown character.
    default:
      return res;
    }

    // We just finished to correctly parse a value.
    assert(values_len <= values_cap);

    // No containers meaning: nothing is currently open.
    // So, we are at the root, which we need to return to the caller,
    // because `root != values[0]` in the general case.
    if (0 == containers_len) {
      assert(1 == values_len);

      res.bencode = values[0];
      res.ok = true;
      return res;
    }
  }

  return res;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Arena memory comes straight from `mmap` and is therefore zeroed, which makes
// a read of never-written memory look like a perfectly valid zeroed struct.
// Poison it so such a read shows up as an obviously bogus value instead.
static Arena test_arena(usize bytes_count) {
  Arena arena = arena_valloc(bytes_count);
  assert(arena.start);
  assert(arena.end);
  assert((usize)arena.end - (usize)arena.start >= bytes_count);

  memset(arena.start, 0xAA, bytes_count);
  return arena;
}

static BencodeParser test_parser(const char *input) {
  assert(input);

  return (BencodeParser){
      .data = slice_u8_make((u8 *)input, strlen(input)),
  };
}

static void test_char_is_digit_ascii(void) {
  assert(char_is_digit_ascii('0'));
  assert(char_is_digit_ascii('5'));
  assert(char_is_digit_ascii('9'));

  assert(!char_is_digit_ascii('0' - 1));
  assert(!char_is_digit_ascii('9' + 1));
  assert(!char_is_digit_ascii(0));
  assert(!char_is_digit_ascii(255));
}

static void test_isize_from_usize(void) {
  isize res = 0;

  // Positive.
  assert(isize_from_usize(0, false, &res) && 0 == res);
  assert(isize_from_usize(123, false, &res) && 123 == res);
  assert(isize_from_usize((usize)SSIZE_MAX, false, &res) && SSIZE_MAX == res);
  assert(!isize_from_usize((usize)SSIZE_MAX + 1, false, &res));
  assert(!isize_from_usize(SIZE_MAX, false, &res));

  // Negative. `|ISIZE_MIN|` is one greater than `ISIZE_MAX`.
  assert(isize_from_usize(0, true, &res) && 0 == res);
  assert(isize_from_usize(123, true, &res) && -123 == res);
  assert(isize_from_usize((usize)SSIZE_MAX, true, &res) && -SSIZE_MAX == res);
  assert(isize_from_usize((usize)SSIZE_MAX + 1, true, &res) &&
         (-SSIZE_MAX - 1) == res);
  assert(!isize_from_usize((usize)SSIZE_MAX + 2, true, &res));
  assert(!isize_from_usize(SIZE_MAX, true, &res));
}

static void test_usize_round_up_multiple_of(void) {
  // A multiple of 1 rounds nothing.
  assert(0 == usize_round_up_multiple_of(0, 1));
  assert(1 == usize_round_up_multiple_of(1, 1));
  assert(SIZE_MAX == usize_round_up_multiple_of(SIZE_MAX, 1));

  // Zero is already a multiple of everything.
  assert(0 == usize_round_up_multiple_of(0, 8));
  assert(0 == usize_round_up_multiple_of(0, 16384));

  // Exact multiples are left alone.
  assert(8 == usize_round_up_multiple_of(8, 8));
  assert(16 == usize_round_up_multiple_of(16, 8));
  assert(16384 == usize_round_up_multiple_of(16384, 16384));
  assert(32768 == usize_round_up_multiple_of(32768, 16384));

  // Everything else goes up to the next one.
  assert(8 == usize_round_up_multiple_of(1, 8));
  assert(8 == usize_round_up_multiple_of(7, 8));
  assert(16 == usize_round_up_multiple_of(9, 8));
  assert(16384 == usize_round_up_multiple_of(1, 16384));
  assert(16384 == usize_round_up_multiple_of(1 * KiB, 16384));
  assert(16384 == usize_round_up_multiple_of(16383, 16384));
  assert(32768 == usize_round_up_multiple_of(16385, 16384));

  // The largest input that does not overflow `n + multiple - 1`, which is
  // itself an exact multiple.
  assert(SIZE_MAX - 8191 == usize_round_up_multiple_of(SIZE_MAX - 8191, 8192));

  // The postcondition holds for every power of two, and rounding an already
  // rounded value changes nothing.
  for (usize multiple = 1; multiple <= ((usize)1 << 20); multiple *= 2) {
    for (usize n = 0; n < 4 * multiple; n += (multiple / 4) + 1) {
      const usize res = usize_round_up_multiple_of(n, multiple);

      assert(res >= n);
      assert(res - n < multiple);
      assert(0 == (res & (multiple - 1)));
      assert(res == usize_round_up_multiple_of(res, multiple));
    }
  }
}

static void test_arena_alloc(void) {
  // Alignment: a 1 byte allocation leaves the head misaligned, the next
  // 8-aligned allocation has to round up over 7 bytes of padding.
  {
    Arena arena = test_arena(1 * KiB);

    u8 *const a = arena_alloc(&arena, 1, sizeof(u8), 1);
    assert(a);
    assert(0 != ((usize)arena.start % 8));

    void *const b = arena_alloc(&arena, 8, 8, 1);
    assert(b);
    assert(0 == ((usize)b % 8));
    assert((usize)b == (usize)a + 8);
  }
  // An allocation that exactly fills the arena is legal; the next one is not.
  {
    Arena arena = test_arena(64);

    void *const p = arena_alloc(&arena, 8, 8, 8);
    assert(p);
    assert(arena.start == arena.end);

    assert(NULL == arena_alloc(&arena, 1, sizeof(u8), 1));
  }
  // OOM leaves the arena untouched.
  {
    Arena arena = test_arena(64);
    u8 *const start_before = arena.start;

    assert(NULL == arena_alloc(&arena, 1, sizeof(u8), 65));
    assert(arena.start == start_before);

    // Still usable afterwards.
    assert(arena_alloc(&arena, 1, sizeof(u8), 64));
    assert(arena.start == arena.end);
  }
  // Multiple allocations are contiguous when alignment allows it.
  {
    Arena arena = test_arena(1 * KiB);

    BencodeValue *const a =
        arena_alloc(&arena, __alignof__(BencodeValue), sizeof(BencodeValue), 2);
    BencodeValue *const b =
        arena_alloc(&arena, __alignof__(BencodeValue), sizeof(BencodeValue), 3);
    assert(a);
    assert(b);
    assert(b == a + 2);
    assert((usize)arena.start == (usize)a + 5 * sizeof(BencodeValue));
  }
}

static void test_arena_valloc(void) {
  // A request the kernel cannot satisfy. `mmap` reports `MAP_FAILED`, not NULL,
  // so this also pins down that conversion.
  const Arena arena = arena_valloc((usize)1 << 62);
  assert(NULL == arena.start);
  assert(NULL == arena.end);
}

static void test_slice_u8(void) {
  u8 data[] = {'a', 'b', 'c'};

  // slice_u8_first.
  {
    assert(!slice_u8_first(slice_u8_make(NULL, 0)).ok);
    assert(!slice_u8_first(slice_u8_make(data, 0)).ok);

    const At_U8 first = slice_u8_first(slice_u8_make(data, sizeof(data)));
    assert(first.ok);
    assert('a' == first.value);
  }
  // slice_u8_skip.
  {
    Slice_u8 empty = slice_u8_make(NULL, 0);
    assert(!slice_u8_skip(&empty, 1));

    Slice_u8 slice = slice_u8_make(data, sizeof(data));

    // Past the end: refused, and the slice is unchanged.
    assert(!slice_u8_skip(&slice, sizeof(data) + 1));
    assert(sizeof(data) == slice.len);
    assert(data == slice.data);

    assert(slice_u8_skip(&slice, 0));
    assert(sizeof(data) == slice.len);

    assert(slice_u8_skip(&slice, 2));
    assert(1 == slice.len);
    assert('c' == slice_u8_first(slice).value);

    // Skipping exactly to the end is legal.
    assert(slice_u8_skip(&slice, 1));
    assert(0 == slice.len);
    assert(!slice_u8_first(slice).ok);
  }
  // slice_u8_take.
  {
    const Slice_u8 slice = slice_u8_make(data, sizeof(data));

    assert(0 == slice_u8_take(slice, 0).len);
    assert(sizeof(data) == slice_u8_take(slice, sizeof(data)).len);

    const Slice_u8 taken = slice_u8_take(slice, 2);
    assert(2 == taken.len);
    assert(data == taken.data);
  }
}

static void test_ascii_num_parse(void) {
  // No data at all.
  {
    const ParseUsize res = ascii_num_parse(slice_u8_make(NULL, 0));
    assert(!res.ok);
    assert(0 == res.consumed);
  }
  // Terminated by a non-digit.
  {
    const ParseUsize res = ascii_num_parse(slice_u8_make((u8 *)"123e", 4));
    assert(res.ok);
    assert(123 == res.num);
    assert(3 == res.consumed);
  }
  // Unterminated: the digits run to the end of the input.
  {
    const ParseUsize res = ascii_num_parse(slice_u8_make((u8 *)"123", 3));
    assert(!res.ok);
  }
  // No digit at all: valid, but consumes nothing. Callers have to check.
  {
    const ParseUsize res = ascii_num_parse(slice_u8_make((u8 *)"e", 1));
    assert(res.ok);
    assert(0 == res.consumed);
    assert(0 == res.num);
  }
  // A single zero is fine.
  {
    const ParseUsize res = ascii_num_parse(slice_u8_make((u8 *)"0e", 2));
    assert(res.ok);
    assert(0 == res.num);
    assert(1 == res.consumed);
  }
  // Leading zeroes are not.
  {
    const ParseUsize res = ascii_num_parse(slice_u8_make((u8 *)"0123e", 5));
    assert(!res.ok);
  }
  {
    const ParseUsize res = ascii_num_parse(slice_u8_make((u8 *)"00e", 3));
    assert(!res.ok);
  }
  // The largest representable usize.
  {
    const char *const input = "18446744073709551615e";
    const ParseUsize res =
        ascii_num_parse(slice_u8_make((u8 *)input, strlen(input)));
    assert(res.ok);
    assert(SIZE_MAX == res.num);
    assert(20 == res.consumed);
  }
  // Overflow on the final add: SIZE_MAX + 1.
  {
    const char *const input = "18446744073709551616e";
    const ParseUsize res =
        ascii_num_parse(slice_u8_make((u8 *)input, strlen(input)));
    assert(!res.ok);
  }
  // Overflow on the multiply: 20 nines.
  {
    const char *const input = "99999999999999999999e";
    const ParseUsize res =
        ascii_num_parse(slice_u8_make((u8 *)input, strlen(input)));
    assert(!res.ok);
  }
}

static void test_bencode_parse_num(void) {
  const struct {
    const char *input;
    bool ok;
    isize num;
  } cases[] = {
      {"i0e", true, 0},
      {"i1e", true, 1},
      {"i-1e", true, -1},
      {"i-123e", true, -123},
      {"i9223372036854775807e", true, SSIZE_MAX},
      {"i-9223372036854775808e", true, -SSIZE_MAX - 1},
      // Out of range for an isize.
      {"i9223372036854775808e", false, 0},
      {"i-9223372036854775809e", false, 0},
      {"i18446744073709551615e", false, 0},
      // Out of range for a usize.
      {"i18446744073709551616e", false, 0},
      // Malformed.
      {"i-0e", false, 0},
      {"ie", false, 0},
      {"i-e", false, 0},
      {"i0123e", false, 0},
      {"i123", false, 0},
      {"i123x", false, 0},
      {"i", false, 0},
      {"", false, 0},
      {"42e", false, 0},
      // Trailing data is left for the caller.
      {"i42ei43e", true, 42},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    BencodeParser parser = test_parser(cases[i].input);
    const BencodeParseResult res = bencode_parse_num(&parser);

    assert(res.ok == cases[i].ok);
    if (cases[i].ok) {
      assert(BencodeKindInteger == res.bencode.kind);
      assert(cases[i].num == res.bencode.v.num);
    }
  }
}

static void test_bencode_parse_string(void) {
  const struct {
    const char *input;
    bool ok;
    const char *str;
    usize remaining;
  } cases[] = {
      {"0:", true, "", 0},
      {"1:a", true, "a", 0},
      {"4:spam", true, "spam", 0},
      {"3:0:x", true, "0:x", 0},
      // Trailing data is left for the caller.
      {"4:spameggs", true, "spam", 4},
      // The length exceeds what is left.
      {"5:spam", false, NULL, 0},
      {"1:", false, NULL, 0},
      // Malformed.
      {"4spam", false, NULL, 0},
      {"4", false, NULL, 0},
      {"e", false, NULL, 0},
      {"", false, NULL, 0},
      {"04:spam", false, NULL, 0},
      {":spam", false, NULL, 0},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    BencodeParser parser = test_parser(cases[i].input);
    const BencodeParseResult res = bencode_parse_string(&parser);

    assert(res.ok == cases[i].ok);
    if (!cases[i].ok) {
      continue;
    }

    assert(BencodeKindString == res.bencode.kind);
    assert(strlen(cases[i].str) == res.bencode.v.s.len);
    assert(0 ==
           memcmp(res.bencode.v.s.data, cases[i].str, res.bencode.v.s.len));
    assert(cases[i].remaining == parser.data.len);
  }
}

static bool test_bencode_is_string(BencodeValue value, const char *expected) {
  assert(expected);

  const usize len = strlen(expected);
  return BencodeKindString == value.kind && len == value.v.s.len &&
         (0 == len || 0 == memcmp(value.v.s.data, expected, len));
}

static void test_bencode_parse(void) {
  const struct {
    const char *input;
    bool ok;
    BencodeKind kind;
    // Only meaningful for a list or a dict.
    usize children_len;
    // What the parser leaves behind for the caller.
    usize remaining;
  } cases[] = {
      // Scalars at the root: `bencode_parse` dispatches, the result is the
      // value itself and not a one-element container.
      {"i42e", true, BencodeKindInteger, 0, 0},
      {"i-1e", true, BencodeKindInteger, 0, 0},
      {"3:abc", true, BencodeKindString, 0, 0},
      {"0:", true, BencodeKindString, 0, 0},
      // Empty containers allocate nothing.
      {"le", true, BencodeKindList, 0, 0},
      {"de", true, BencodeKindDict, 0, 0},
      // Flat containers.
      {"l4:spami456ee", true, BencodeKindList, 2, 0},
      {"li1ei2ei3ee", true, BencodeKindList, 3, 0},
      {"d3:key5:valuee", true, BencodeKindDict, 2, 0},
      // Nesting: a closed container is one child of its parent, whatever it
      // holds.
      {"llee", true, BencodeKindList, 1, 0},
      {"lli1eee", true, BencodeKindList, 1, 0},
      {"ld1:a1:beli2eee", true, BencodeKindList, 2, 0},
      {"d1:ali1ei2ee1:bd1:ci3eee", true, BencodeKindDict, 4, 0},
      // Trailing data is left for the caller, exactly like the scalar parsers.
      {"i42etrailing", true, BencodeKindInteger, 0, 8},
      {"lee", true, BencodeKindList, 0, 1},
      {"lei42e", true, BencodeKindList, 0, 4},
      // A dict holds key/value pairs, so an odd number of children is
      // malformed.
      {"d3:keye", false, 0, 0, 0},
      {"di1ee", false, 0, 0, 0},
      // NOTE: bencode requires dict keys to be strings (and to be sorted);
      // neither is enforced yet, so this currently parses. Update this case
      // when it stops doing so.
      {"di1ei2ee", true, BencodeKindDict, 2, 0},
      // Unterminated containers, at every depth.
      {"l", false, 0, 0, 0},
      {"d", false, 0, 0, 0},
      {"li1e", false, 0, 0, 0},
      {"lli1ee", false, 0, 0, 0},
      {"d3:key5:value", false, 0, 0, 0},
      // A stray `e` closes nothing.
      {"e", false, 0, 0, 0},
      {"i42ee", true, BencodeKindInteger, 0, 1},
      // A malformed item aborts the whole parse, however deeply nested.
      {"li-0ee", false, 0, 0, 0},
      {"l5:spame", false, 0, 0, 0},
      {"lli0123eee", false, 0, 0, 0},
      {"ld1:a1:bi0123eee", false, 0, 0, 0},
      // Unknown characters, at the root and nested.
      {"x", false, 0, 0, 0},
      {"lxe", false, 0, 0, 0},
      {":", false, 0, 0, 0},
      {"-1e", false, 0, 0, 0},
      // Empty input.
      {"", false, 0, 0, 0},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);

    BencodeParser parser = test_parser(cases[i].input);
    const BencodeParseResult res = bencode_parse(&parser, &arena, scratch);

    assert(res.ok == cases[i].ok);
    if (!cases[i].ok) {
      continue;
    }

    assert(cases[i].kind == res.bencode.kind);
    assert(cases[i].remaining == parser.data.len);

    if (BencodeKindList == res.bencode.kind ||
        BencodeKindDict == res.bencode.kind) {
      assert(cases[i].children_len == res.bencode.v.list.len);
      // An empty container owns no allocation at all.
      assert(res.bencode.v.list.data || 0 == res.bencode.v.list.len);
    }
  }

  // Every digit dispatches to the string parser.
  {
    for (u8 c = '0'; c <= '9'; c++) {
      Arena arena = test_arena(1 * KiB);
      Arena scratch = test_arena(1 * KiB);

      const char input[] = {(char)c, ':', 0};
      BencodeParser parser = test_parser(input);
      const BencodeParseResult res = bencode_parse(&parser, &arena, scratch);

      // Only `0:` has a body short enough to succeed.
      assert(res.ok == ('0' == c));
    }
  }

  // The whole tree, spelled out: children are stored in input order and the
  // nested containers point at their own right-sized allocations.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);

    BencodeParser parser = test_parser("ld1:a1:beli2eee");
    const BencodeParseResult res = bencode_parse(&parser, &arena, scratch);
    assert(res.ok);
    assert(BencodeKindList == res.bencode.kind);
    assert(2 == res.bencode.v.list.len);

    const BencodeValue dict = res.bencode.v.list.data[0];
    assert(BencodeKindDict == dict.kind);
    assert(2 == dict.v.list.len);
    assert(test_bencode_is_string(dict.v.list.data[0], "a"));
    assert(test_bencode_is_string(dict.v.list.data[1], "b"));

    const BencodeValue list = res.bencode.v.list.data[1];
    assert(BencodeKindList == list.kind);
    assert(1 == list.v.list.len);
    assert(BencodeKindInteger == list.v.list.data[0].kind);
    assert(2 == list.v.list.data[0].v.num);
  }

  // Strings point into the input, they are not copied.
  {
    Arena arena = test_arena(1 * KiB);
    Arena scratch = test_arena(1 * KiB);

    const char *const input = "l4:spame";
    BencodeParser parser = test_parser(input);
    const BencodeParseResult res = bencode_parse(&parser, &arena, scratch);
    assert(res.ok);
    assert((u8 *)input + 3 == res.bencode.v.list.data[0].v.s.data);
  }

  // Nesting is bounded, and the bound is not off by one.
  {
    // `BENCODE_MAX_DEPTH` nested lists, then one more.
    for (usize depth = BENCODE_MAX_DEPTH; depth <= BENCODE_MAX_DEPTH + 1;
         depth++) {
      Arena arena = test_arena(16 * KiB);
      Arena scratch = test_arena(16 * KiB);

      u8 input[2 * (BENCODE_MAX_DEPTH + 1)] = {0};
      memset(input, 'l', depth);
      memset(input + depth, 'e', depth);

      BencodeParser parser = {.data = slice_u8_make(input, 2 * depth)};
      const BencodeParseResult res = bencode_parse(&parser, &arena, scratch);

      assert(res.ok == (depth <= BENCODE_MAX_DEPTH));
      if (res.ok) {
        assert(BencodeKindList == res.bencode.kind);
        assert(1 == res.bencode.v.list.len);
      }
    }
  }

  // A long flat list: `values` is sized from the input length, so this is the
  // case that comes closest to filling it.
  {
    const usize children_len = 200;
    Arena arena = test_arena(64 * KiB);
    Arena scratch = test_arena(64 * KiB);

    u8 input[1 + 2 * 200 + 1] = {0};
    input[0] = 'l';
    for (usize i = 0; i < children_len; i++) {
      input[1 + 2 * i] = '0';
      input[1 + 2 * i + 1] = ':';
    }
    input[1 + 2 * children_len] = 'e';

    BencodeParser parser = {.data = slice_u8_make(input, sizeof(input))};
    const BencodeParseResult res = bencode_parse(&parser, &arena, scratch);
    assert(res.ok);
    assert(BencodeKindList == res.bencode.kind);
    assert(children_len == res.bencode.v.list.len);

    for (usize i = 0; i < children_len; i++) {
      assert(test_bencode_is_string(res.bencode.v.list.data[i], ""));
    }
  }

  // Out of scratch: the scratch arena cannot even hold the `values` array.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(8);

    BencodeParser parser = test_parser("li1ei2ee");
    assert(!bencode_parse(&parser, &arena, scratch).ok);
  }
  // Out of output arena: the children of a container do not fit.
  {
    Arena arena = test_arena(8);
    Arena scratch = test_arena(4 * KiB);

    BencodeParser parser = test_parser("li1ee");
    assert(!bencode_parse(&parser, &arena, scratch).ok);

    // An empty container needs no allocation at all, so it still succeeds.
    BencodeParser parser_empty = test_parser("le");
    const BencodeParseResult res =
        bencode_parse(&parser_empty, &arena, scratch);
    assert(res.ok);
    assert(0 == res.bencode.v.list.len);
  }

  // `scratch` is taken by value: it is not consumed, and two parses in a row
  // do not tread on each other. `arena` *is* consumed, so the first result
  // stays valid while the second one is built.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);
    const u8 *const scratch_start = scratch.start;

    BencodeParser parser_a = test_parser("li1ei2ee");
    const BencodeParseResult a = bencode_parse(&parser_a, &arena, scratch);
    assert(a.ok);
    assert(scratch_start == scratch.start);

    BencodeParser parser_b = test_parser("li3ee");
    const BencodeParseResult b = bencode_parse(&parser_b, &arena, scratch);
    assert(b.ok);
    assert(scratch_start == scratch.start);

    assert(a.bencode.v.list.data != b.bencode.v.list.data);
    assert(2 == a.bencode.v.list.len);
    assert(1 == a.bencode.v.list.data[0].v.num);
    assert(2 == a.bencode.v.list.data[1].v.num);
    assert(1 == b.bencode.v.list.len);
    assert(3 == b.bencode.v.list.data[0].v.num);
  }

  // An input with no data at all.
  {
    Arena arena = test_arena(1 * KiB);
    Arena scratch = test_arena(1 * KiB);

    BencodeParser parser = {.data = slice_u8_make(NULL, 0)};
    assert(!bencode_parse(&parser, &arena, scratch).ok);
  }
}

static void test(const char *filter) {
  const struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
      {"char_is_digit_ascii", test_char_is_digit_ascii},
      {"isize_from_usize", test_isize_from_usize},
      {"usize_round_up_multiple_of", test_usize_round_up_multiple_of},
      {"arena_alloc", test_arena_alloc},
      {"arena_valloc", test_arena_valloc},
      {"slice_u8", test_slice_u8},
      {"ascii_num_parse", test_ascii_num_parse},
      {"bencode_parse_num", test_bencode_parse_num},
      {"bencode_parse_string", test_bencode_parse_string},
      {"bencode_parse", test_bencode_parse},
  };

  usize run = 0;
  for (usize i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (filter && 0 != strcmp(filter, tests[i].name)) {
      continue;
    }

    printf("test %s\n", tests[i].name);
    tests[i].fn();
    run++;
  }

  // A filter that matches nothing is a typo, not a pass.
  assert(run > 0);
  printf("%zu test(s) passed\n", run);
}

int main(i32 argc, char *argv[]) {
  assert(argc >= 1);
  assert(argv);

  test(argc > 1 ? argv[1] : NULL);
}
