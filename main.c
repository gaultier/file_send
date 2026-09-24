#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

// The ARMv8 SHA-256 extension. Only the AArch64 spelling is implemented; every
// other target falls back to the scalar block function below, which stays the
// reference the vector one is checked against.
#if defined(__aarch64__)
#include <arm_neon.h>
#define SHA256_HAS_NEON 1
#else
#define SHA256_HAS_NEON 0
#endif

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;
typedef size_t usize;
typedef ssize_t isize;

static const usize KiB = 1024;
static const usize MiB = 1024 * KiB;

typedef struct {
  // Start of the arena allocation.
  // Increases with each allocation.
  u8 *start;
  // Size of the full arena.
  // Only used to detect the OOM case.
  u8 *end;
} Arena;

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

#define SHA256_DIGEST_LENGTH 32

typedef struct {
  u8 digest[SHA256_DIGEST_LENGTH];
} PieceHash;

__attribute((warn_unused_result)) static bool char_is_digit_ascii(u8 c) {
  return '0' <= c && c <= '9';
}

// Convert `magnitude`, optionally negated, to an isize.
// Returns false if the value does not fit.
__attribute((warn_unused_result)) static bool
isize_from_usize(usize magnitude, bool negative, isize *res) {
  assert(res);

  // The overflow builtins compute in infinite precision and report whether the
  // result fits the *destination* type, so both of these are checked
  // conversions: `0 - magnitude` is a checked negation, `magnitude + 0` a
  // checked cast. This handles the asymmetric boundary (`|ISIZE_MIN|` is a
  // valid magnitude but not a valid positive value) with no special case.
  return negative ? !__builtin_sub_overflow(0, magnitude, res)
                  : !__builtin_add_overflow(magnitude, 0, res);
}

__attribute((warn_unused_result)) static void *
arena_alloc(Arena *arena, usize align, usize elem_size, usize elem_count) {
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

__attribute((warn_unused_result)) static Arena
arena_from_mem(u8 *mem, usize bytes_count) {
  assert(mem);
  assert(bytes_count);

  usize end = 0;
  assert(!__builtin_add_overflow((usize)mem, bytes_count, &end));

  const Arena res = {.start = mem, .end = (u8 *)end};
  assert(res.start <= res.end);
  return res;
}

__attribute((warn_unused_result)) static u8 *
unix_virtual_mem_alloc(usize bytes_count) {
  assert(bytes_count > 0);
  void *alloc = mmap(NULL, bytes_count, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);

  if ((void *)-1 == alloc) {
    return NULL;
  }

  return alloc;
}

__attribute((warn_unused_result)) static usize unix_get_page_size(void) {
  i64 res = sysconf(_SC_PAGE_SIZE);
  assert(-1 != res && "unreachable");

  return (usize)res;
}

__attribute((warn_unused_result)) static bool unix_vprotect_none(void *ptr,
                                                                 usize size) {
  if (-1 == mprotect(ptr, size, PROT_NONE)) {
    return false;
  }
  return true;
}

__attribute((warn_unused_result)) static bool is_power_of_two(usize value) {
  return (value != 0) && ((value & (value - 1)) == 0);
}

// `multiple` must be a power of two, which every page size is.
__attribute((warn_unused_result)) static usize
usize_round_up_multiple_of(usize n, usize multiple) {
  assert(multiple != 0);
  assert(is_power_of_two(multiple));

  usize res = 0;
  assert(!__builtin_add_overflow(n, multiple - 1, &res));
  res &= ~(multiple - 1);

  assert(0 == (res & (multiple - 1)));
  assert(res >= n);
  assert(res - n < multiple);
  return res;
}

__attribute((warn_unused_result)) static usize next_power_of_two(usize val) {
  if (0 == val) {
    return 1;
  }

  val -= 1;
  val |= val >> 1;
  val |= val >> 2;
  val |= val >> 4;
  val |= val >> 8;
  val |= val >> 16;
  val |= val >> 32;
  val += 1;

  assert(0 != val);
  assert(is_power_of_two(val));

  return val;
}

__attribute((warn_unused_result)) static usize ceil_usize(usize numerator,
                                                          usize denominator) {
  assert(denominator);

  return numerator / denominator + (numerator % denominator != 0);
}

__attribute((warn_unused_result)) static Arena arena_valloc(usize bytes_count) {
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

// Peek at the first byte of `slice`, leaving it in place.
// Returns false, and does not touch `*res`, if there is no first byte.
__attribute((warn_unused_result)) static bool slice_u8_first(Slice_u8 slice,
                                                             u8 *res) {
  assert(res);

  if (!slice.data) {
    return false;
  }

  if (slice.len == 0) {
    return false;
  }

  *res = slice.data[0];
  return true;
}

__attribute((warn_unused_result)) static bool slice_u8_skip(Slice_u8 *slice,
                                                            usize count) {
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

// Advance past `count` bytes. The caller must have already established that
// they are available, which is why this cannot fail; use `slice_u8_skip` when
// the input may be short. Unlike an `assert(slice_u8_skip(...))`, the advance
// still happens when asserts are compiled out.
static void slice_u8_advance(Slice_u8 *slice, usize count) {
  assert(slice);
  assert(slice->data);
  assert(count <= slice->len);

  slice->len -= count;
  slice->data += count;
}

// The caller must have already established that `count` bytes are available:
// silently returning a short slice would turn a malformed length into a
// successful parse of truncated data.
__attribute((warn_unused_result)) static Slice_u8 slice_u8_take(Slice_u8 input,
                                                                usize count) {
  assert(count <= input.len);

  return (Slice_u8){.data = input.data, .len = count};
}

__attribute((warn_unused_result)) static Slice_u8 slice_u8_make(u8 *data,
                                                                usize len) {
  if (0 != len) {
    assert(data);
  }

  return (Slice_u8){.data = data, .len = len};
}

// Consume the first byte of `*slice` if it is `expected`.
//
// `*slice` is only advanced on a match, which is what makes it usable as a
// speculative `if (!consume(...)) { return false; }` inside a parse that rolls
// back.
__attribute((warn_unused_result)) static bool slice_u8_consume(Slice_u8 *slice,
                                                               u8 expected) {
  assert(slice);
  assert(slice->data);

  u8 actual = 0;
  if (!slice_u8_first(*slice, &actual)) {
    return false;
  }
  if (actual != expected) {
    return false;
  }

  slice_u8_advance(slice, 1);
  return true;
}

// Parse a run of ASCII digits from the front of `*data`, consuming them.
//
// Rejects a run with no digits at all, one that is not terminated by a
// non-digit, one with a leading zero, and one that overflows a `usize`.
// `*data` is only advanced, and `*res` only written, when the parse succeeds.
__attribute((warn_unused_result)) static bool ascii_num_parse(Slice_u8 *data,
                                                              usize *res) {
  assert(data);
  assert(res);

  if (!data->data) {
    return false;
  }

  Slice_u8 remaining = *data;
  usize num = 0;
  bool has_leading_zero = false;

  const usize MAX_LEN = 30;

  for (usize consumed = 0; consumed < MAX_LEN; consumed++) {
    u8 current = 0;

    // Unterminated.
    if (!slice_u8_first(remaining, &current)) {
      return false;
    }

    // End.
    if (!char_is_digit_ascii(current)) {
      // No digit at all e.g. `e` or `:spam`: not a number.
      if (0 == consumed) {
        return false;
      }

      assert(consumed < MAX_LEN);
      *data = remaining;
      *res = num;
      return true;
    }

    if (current == '0' && consumed == 0) {
      has_leading_zero = true;
    }

    // Leading zeroes forbidden except `i0e`.
    if (consumed > 0 && has_leading_zero) {
      return false;
    }

    const usize digit = current - '0';
    if (__builtin_mul_overflow(num, 10, &num)) {
      return false;
    }
    if (__builtin_add_overflow(num, digit, &num)) {
      return false;
    }

    slice_u8_advance(&remaining, 1);
  }

  assert(0 && "unreachable");
}

// `i123e`
// `i-123e`
//
// `*input` is only advanced, and `*res` only written, when the parse
// succeeds.
__attribute((warn_unused_result)) static bool
bencode_parse_num(Slice_u8 *input, BencodeValue *res) {
  assert(input);
  assert(input->data);
  assert(res);

  Slice_u8 remaining = *input;

  if (!slice_u8_consume(&remaining, 'i')) {
    return false;
  }

  const bool negative_sign = slice_u8_consume(&remaining, '-');

  // Also rejects `ie` and `i-e`: a number needs at least one digit.
  usize magnitude = 0;
  if (!ascii_num_parse(&remaining, &magnitude)) {
    return false;
  }

  // `i-0e` is invalid bencode.
  if (negative_sign && 0 == magnitude) {
    return false;
  }

  isize num = 0;
  if (!isize_from_usize(magnitude, negative_sign, &num)) {
    return false;
  }

  if (!slice_u8_consume(&remaining, 'e')) {
    return false;
  }

  *input = remaining;
  *res = (BencodeValue){.kind = BencodeKindInteger, .v.num = num};
  return true;
}

// `4:spam`
//
// The string is not copied: it points into `*input`.
// `*input` is only advanced, and `*res` only written, when the parse
// succeeds.
__attribute((warn_unused_result)) static bool
bencode_parse_string(Slice_u8 *input, BencodeValue *res) {
  assert(input);
  assert(input->data);
  assert(res);

  Slice_u8 remaining = *input;

  // Also rejects a leading `:` or any non-digit: a length needs a digit.
  usize len = 0;
  if (!ascii_num_parse(&remaining, &len)) {
    return false;
  }

  if (!slice_u8_consume(&remaining, ':')) {
    return false;
  }

  // Truncated body.
  if (len > remaining.len) {
    return false;
  }

  const Slice_u8 s = slice_u8_take(remaining, len);
  slice_u8_advance(&remaining, len);

  *input = remaining;
  *res = (BencodeValue){.kind = BencodeKindString, .v.s = s};

  assert(res->v.s.len == len);
  if (0 != res->v.s.len) {
    assert(res->v.s.data);
  }
  return true;
}

// Compare two byte strings lexicographically: the first differing byte decides,
// and when one is a prefix of the other, the shorter one sorts first.
// Returns <0, 0, or >0, like `memcmp`.
//
// Bytes are compared as unsigned values, so `0x80` sorts after `0x7f`. This is
// a total order over arbitrary bytes, embedded zeroes included, and it is the
// ordering bencode requires of dict keys.
__attribute((warn_unused_result)) static i32 bytes_cmp(u8 *a, usize a_len,
                                                       u8 *b, usize b_len) {
  if (0 != a_len) {
    assert(a);
  }
  if (0 != b_len) {
    assert(b);
  }

  // Not `memcmp`: it is undefined to hand it a NULL pointer even for a length
  // of zero, and an empty byte string is legal here.
  const usize len = a_len < b_len ? a_len : b_len;
  for (usize i = 0; i < len; i++) {
    if (a[i] != b[i]) {
      return a[i] < b[i] ? -1 : 1;
    }
  }

  // Equal up to the shorter length: the prefix sorts first.
  if (a_len == b_len) {
    return 0;
  }
  return a_len < b_len ? -1 : 1;
}

static bool bencode_validate_dict(BencodeList list) {
  if (0 != list.len) {
    assert(NULL != list.data);
  }

  // Mismatched key-value pairs?
  if (list.len % 2 != 0) {
    return false;
  }

  for (usize i = 0; i < list.len; i += 2) {
    const BencodeValue key = list.data[i];

    if (key.kind != BencodeKindString) {
      return false;
    }

    if (i > 1) {
      const BencodeValue previous = list.data[i - 2];

      if (bytes_cmp(previous.v.s.data, previous.v.s.len, key.v.s.data,
                    key.v.s.len) >= 0) {
        return false;
      }
    }
  }

  return true;
}

#define BENCODE_MAX_DEPTH 128

// Parse one complete bencode value, with all of its children, into `*res`.
//
// `*input` and `*arena` are only advanced when the parse succeeds: rolling
// back a bump allocator is just restoring its start pointer, so a failed parse
// leaves the caller with neither consumed input nor consumed memory.
//
// `scratch` is taken by value and is not consumed by the call.
__attribute((warn_unused_result)) static bool
bencode_parse(Slice_u8 *input, Arena *arena, Arena scratch, BencodeValue *res) {
  assert(input);
  assert(arena);
  assert(arena->start <= arena->end);
  if (0 != input->len) {
    assert(input->data);
  }
  assert(res);

  // Nothing to do?
  if (0 == input->len) {
    return false;
  }

  Slice_u8 remaining = *input;
  Arena arena_local = *arena;

  // At most, there are as many bencode values as `input bytes/2+1` since each
  // value takes at least 2 bytes.
  const usize values_cap = remaining.len / 2 + 1;
  BencodeValue *values = arena_alloc(&scratch, __alignof__(BencodeValue),
                                     sizeof(BencodeValue), values_cap);
  // OOM?
  if (!values) {
    return false;
  }
  usize values_count = 0;

  BencodeContainer containers[BENCODE_MAX_DEPTH] = {0};
  usize containers_count = 0;

  const usize MAX_LEN = remaining.len;

  for (usize _i = 0; _i < MAX_LEN; _i++) {
    u8 current = 0;
    if (!slice_u8_first(remaining, &current)) {
      return false;
    }

    switch (current) {
    case 'i':
      // Parsed straight into its final slot: no intermediate copy.
      assert(values_count < values_cap);
      if (!bencode_parse_num(&remaining, &values[values_count])) {
        return false;
      }
      values_count++;
      break;

    case 'l':
    case 'd':
      if (containers_count >= BENCODE_MAX_DEPTH) {
        return false;
      }
      slice_u8_advance(&remaining, 1);

      containers[containers_count].is_list = current == 'l';
      containers[containers_count].children_start = values_count;
      containers_count++;

      // The next loop iteration will parse the items.
      continue;

    case 'e': {
      if (0 == containers_count) {
        // Stray `e`, reject.
        return false;
      }

      slice_u8_advance(&remaining, 1);

      // Time to pop `containers`.
      const BencodeContainer container = containers[containers_count - 1];
      containers[containers_count - 1] = (BencodeContainer){0};
      containers_count--;

      const usize children_len = values_count - container.children_start;

      // Should always be key-value pairs.
      if (!container.is_list && children_len % 2 != 0) {
        return false;
      }

      // Now record the value for this container.
      BencodeValue value = {.kind = container.is_list ? BencodeKindList
                                                      : BencodeKindDict};
      // If there are any children, we need to allocate (right-sized) space for
      // them.
      if (children_len > 0) {
        BencodeValue *children =
            arena_alloc(&arena_local, __alignof__(BencodeValue),
                        sizeof(BencodeValue), children_len);
        // OOM?
        if (!children) {
          return false;
        }

        // Copy the items from `values` to the new right-sized allocation.
        value.v.list.data = memcpy(children, values + container.children_start,
                                   children_len * sizeof(BencodeValue));
        value.v.list.len = children_len;

        if (BencodeKindDict == value.kind &&
            !bencode_validate_dict(value.v.list)) {
          return false;
        }
      }

      // Pop all the items for this container, at once. The popped slots are
      // scrubbed so that a stale child cannot be mistaken for a live value.
      memset(values + container.children_start, 0,
             children_len * sizeof(BencodeValue));
      values_count = container.children_start;
      assert(values_count < values_cap);

      // Do not forget to record this new bencode value!
      values[values_count++] = value;
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
    case '9':
      // Parsed straight into its final slot: no intermediate copy.
      assert(values_count < values_cap);
      if (!bencode_parse_string(&remaining, &values[values_count])) {
        return false;
      }
      values_count++;
      break;

      // Unknown character.
    default:
      return false;
    }

    // We just finished to correctly parse a value.
    assert(values_count <= values_cap);

    // No containers meaning: nothing is currently open.
    // So, we are at the root, which we need to return to the caller,
    // because `root != values[0]` in the general case.
    if (0 == containers_count) {
      assert(1 == values_count);

      // The single success exit: everything is committed here, at once.
      *input = remaining;
      *arena = arena_local;
      *res = values[0];
      return true;
    }
  }

  return false;
}

static void bencode_print_indent(usize indent) {
  for (usize i = 0; i < indent; i++) {
    printf(" ");
  }
}

// Print `v` in a JSON-ish form.
//
// The caller owns the cursor: it has already written whatever precedes the
// value on the current line (the leading indentation, or a `key: ` prefix), so
// this never indents the value itself. `indent` is the column the *line* the
// value starts on begins at, which is what the children and the closing
// bracket are aligned against. Nothing is written after the value either: a
// trailing newline is the caller's to add.
void bencode_print(BencodeValue v, usize indent) {
  switch (v.kind) {
  case BencodeKindInteger:
    printf("%zd", v.v.num);
    break;

  case BencodeKindString:
    printf("\"%.*s\"", (i32)v.v.s.len, v.v.s.data);
    break;

  case BencodeKindDict:
    // An empty container has no children to lay out, so it stays on one line.
    if (0 == v.v.list.len) {
      printf("{}");
      break;
    }

    printf("{\n");
    for (usize i = 0; i < v.v.list.len; i += 2) {
      if (i > 0) {
        printf(",\n");
      }
      bencode_print_indent(indent + 2);
      bencode_print(v.v.list.data[i], indent + 2);
      printf(": ");
      // The value is indented against the start of the key's line, not against
      // the column it happens to start at, so a nested container closes
      // underneath its key.
      bencode_print(v.v.list.data[i + 1], indent + 2);
    }
    printf("\n");
    bencode_print_indent(indent);
    printf("}");
    break;

  case BencodeKindList:
    if (0 == v.v.list.len) {
      printf("[]");
      break;
    }

    printf("[\n");
    for (usize i = 0; i < v.v.list.len; i++) {
      if (i > 0) {
        printf(",\n");
      }
      bencode_print_indent(indent + 2);
      bencode_print(v.v.list.data[i], indent + 2);
    }
    printf("\n");
    bencode_print_indent(indent);
    printf("]");
    break;

  default:
    assert(0 && "unreachable");
  }
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

// SHA-256 as specified in FIPS 180-4, in plain C: no hardware intrinsics, so
// the same code runs on every target and stays diffable against the spec.
//
// The usual three step API: one context, any number of `Update` calls, one
// `Final`. None of the three can fail, so none of them returns anything:
//
//   SHA256_CTX ctx = {0};
//   SHA256_Init(&ctx);
//   SHA256_Update(&ctx, data);
//   u8 digest[SHA256_DIGEST_LENGTH] = {0};
//   SHA256_Final(&ctx, digest);

#define SHA256_CBLOCK 64

typedef struct {
  // Chaining state: the eight working variables between blocks.
  u32 h[8];
  // Total number of message bytes fed in so far. Only the low 61 bits can
  // matter: the padding stores the length in bits, in 64 bits.
  u64 len;
  // Bytes of a not-yet-complete block held back from a previous `Update`.
  u32 partial_len;
  u8 partial[SHA256_CBLOCK];
} Sha256Ctx;

// First 32 bits of the fractional parts of the cube roots of the first 64
// primes (FIPS 180-4, 4.2.2).
static const u32 sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// `count` must be in 1..31: a rotation by 0 would shift a `u32` by 32, which
// is undefined behaviour.
__attribute((warn_unused_result)) static u32 u32_rotate_right(u32 x,
                                                              u32 count) {
  assert(count >= 1);
  assert(count <= 31);

  return (x >> count) | (x << (32 - count));
}

__attribute((warn_unused_result)) static u32 u32_from_bytes_be(const u8 *data) {
  assert(data);

  return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) |
         (u32)data[3];
}

static void u32_to_bytes_be(u32 value, u8 *res) {
  assert(res);

  res[0] = (u8)(value >> 24);
  res[1] = (u8)(value >> 16);
  res[2] = (u8)(value >> 8);
  res[3] = (u8)value;
}

// Mix one full 64 byte block into the chaining state (FIPS 180-4, 6.2.2).
static void sha256_compress(u32 h[8], const u8 block[SHA256_CBLOCK]) {
  assert(h);
  assert(block);

  u32 w[64] = {0};
  for (usize i = 0; i < 16; i++) {
    w[i] = u32_from_bytes_be(block + i * 4);
  }
  for (usize i = 16; i < 64; i++) {
    const u32 s0 = u32_rotate_right(w[i - 15], 7) ^
                   u32_rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const u32 s1 = u32_rotate_right(w[i - 2], 17) ^
                   u32_rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  u32 a = h[0], b = h[1], c = h[2], d = h[3];
  u32 e = h[4], f = h[5], g = h[6], hh = h[7];

  for (usize i = 0; i < 64; i++) {
    const u32 s1 = u32_rotate_right(e, 6) ^ u32_rotate_right(e, 11) ^
                   u32_rotate_right(e, 25);
    const u32 ch = (e & f) ^ (~e & g);
    const u32 t1 = hh + s1 + ch + sha256_k[i] + w[i];
    const u32 s0 = u32_rotate_right(a, 2) ^ u32_rotate_right(a, 13) ^
                   u32_rotate_right(a, 22);
    const u32 maj = (a & b) ^ (a & c) ^ (b & c);
    const u32 t2 = s0 + maj;

    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

// First 32 bits of the fractional parts of the square roots of the first 8
// primes (FIPS 180-4, 5.3.3).
#if SHA256_HAS_NEON

// `+crypto` is a function level target, not a build flag, because `-march`
// does not necessarily enable it: on this toolchain plain `-march=native`
// leaves `__ARM_FEATURE_SHA2` undefined and the intrinsics below refuse to
// compile without it.
__attribute__((target("+crypto"))) static void
sha256_compress_blocks_neon(u32 h[8], const u8 *blocks, usize blocks_count) {
  assert(h);
  if (0 != blocks_count) {
    assert(blocks);
  }

  // Unlike the x86 extension, the ARM one keeps the working variables in their
  // natural order, so the state needs no shuffling on the way in or out.
  uint32x4_t state0 = vld1q_u32(&h[0]); // a b c d
  uint32x4_t state1 = vld1q_u32(&h[4]); // e f g h

  // The chaining state stays in registers for the whole run, so a multi block
  // hash reads and writes `h` once instead of once per block.
  for (usize b = 0; b < blocks_count; b++) {
    const u8 *const block = blocks + b * SHA256_CBLOCK;

    const uint32x4_t state0_in = state0;
    const uint32x4_t state1_in = state1;

    // The message is big endian, the vector unit little endian.
    uint32x4_t msg[4] = {0};
    for (usize i = 0; i < 4; i++) {
      msg[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + i * 16)));
    }

    // Each instruction pair consumes four rounds at once, so 64 rounds are 16
    // iterations rather than the scalar version's 64.
    uint32x4_t wk = vaddq_u32(msg[0], vld1q_u32(&sha256_k[0]));

    // Unrolled by four, which measured fastest. The win is front end, not
    // register pressure: the subscripts stay runtime values and `msg` still
    // round trips through the stack, but that traffic is off the critical path
    // and hides in the shadow of the `sha256h` chain. Both ways of removing it
    // -- unrolling all sixteen groups, and hand writing them against four
    // named vector variables the way OpenSSL's asm does -- measured slower
    // here, because the larger footprint costs more than the traffic did.
#pragma clang loop unroll_count(4)
    for (usize i = 0; i < 16; i++) {
      const usize j = i & 3;

      // The next group's `w + k` reads a message word that this group's
      // schedule is about to overwrite, so compute it first.
      uint32x4_t wk_next = wk;
      if (i < 15) {
        wk_next =
            vaddq_u32(msg[(j + 1) & 3], vld1q_u32(&sha256_k[4 * (i + 1)]));
      }

      // The last four groups consume the schedule without extending it: there
      // is no `w[64]`.
      if (i < 12) {
        msg[j] = vsha256su0q_u32(msg[j], msg[(j + 1) & 3]);
      }

      // `SHA256H` needs the old `a b c d`, which `SHA256H` itself overwrites.
      const uint32x4_t abcd = state0;
      state0 = vsha256hq_u32(state0, state1, wk);
      state1 = vsha256h2q_u32(state1, abcd, wk);

      if (i < 12) {
        msg[j] = vsha256su1q_u32(msg[j], msg[(j + 2) & 3], msg[(j + 3) & 3]);
      }

      wk = wk_next;
    }

    state0 = vaddq_u32(state0, state0_in);
    state1 = vaddq_u32(state1, state1_in);
  }

  vst1q_u32(&h[0], state0);
  vst1q_u32(&h[4], state1);
}

// The extension is optional even on AArch64, so ask rather than assume.
// Cached because this sits on the hot path of every hash and `sysctlbyname` is
// a syscall. Racing callers compute the same answer, so the race is benign.
__attribute((warn_unused_result)) static bool sha256_neon_supported(void) {
  static i32 cached = -1;

  if (cached < 0) {
    i32 present = 0;
    usize present_size = sizeof(present);
    const bool ok = 0 == sysctlbyname("hw.optional.arm.FEAT_SHA256", &present,
                                      &present_size, NULL, 0);
    cached = (ok && 0 != present) ? 1 : 0;
  }

  return 1 == cached;
}

#endif

// Compress `blocks_count` consecutive blocks. The implementation is chosen once
// here rather than per block, so the check stays out of the inner loop.
static void sha256_compress_blocks(u32 h[8], const u8 *blocks,
                                   usize blocks_count) {
  assert(h);
  if (0 != blocks_count) {
    assert(blocks);
  }

#if SHA256_HAS_NEON
  if (sha256_neon_supported()) {
    sha256_compress_blocks_neon(h, blocks, blocks_count);
    return;
  }
#endif

  for (usize i = 0; i < blocks_count; i++) {
    sha256_compress(h, blocks + i * SHA256_CBLOCK);
  }
}

static void sha256_init(Sha256Ctx *ctx) {
  assert(ctx);

  *ctx = (Sha256Ctx){
      .h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
            0x9b05688c, 0x1f83d9ab, 0x5be0cd19},
  };
}

static void sha256_update(Sha256Ctx *ctx, u8 *data, usize len) {
  assert(ctx);
  if (0 != len) {
    assert(data);
  }
  assert(ctx->partial_len < SHA256_CBLOCK);

  const u8 *remaining = data;
  ctx->len += len;

  // Top up a partial block from a previous call first. It is only compressed
  // once it is full: a short `Update` must leave it partial, not hash it.
  if (ctx->partial_len > 0) {
    const usize wanted = SHA256_CBLOCK - ctx->partial_len;
    const usize taken = len < wanted ? len : wanted;

    memcpy(ctx->partial + ctx->partial_len, remaining, taken);
    ctx->partial_len += (u32)taken;
    remaining += taken;
    len -= taken;

    if (ctx->partial_len < SHA256_CBLOCK) {
      assert(0 == len);
      return;
    }

    sha256_compress_blocks(ctx->h, ctx->partial, 1);
    ctx->partial_len = 0;
  }

  {
    const usize blocks_count = len / SHA256_CBLOCK;
    sha256_compress_blocks(ctx->h, remaining, blocks_count);
    remaining += blocks_count * SHA256_CBLOCK;
    len -= blocks_count * SHA256_CBLOCK;
  }

  if (len > 0) {
    memcpy(ctx->partial, remaining, len);
  }
  ctx->partial_len = (u32)len;
}

// `*ctx` is left zeroed, so it cannot be used again without another
// `SHA256_Init`, and the chaining state of the message does not linger.
static void sha256_final(Sha256Ctx *ctx, u8 res[SHA256_DIGEST_LENGTH]) {
  assert(ctx);
  assert(res);

  // The length goes in the last 8 bytes of the last block, so pad with a
  // single one bit then zeroes up to the next offset 56 (mod 64).
  const u64 len_bits = ctx->len * 8;
  const usize len_mod = (usize)(ctx->len % SHA256_CBLOCK);
  const usize padding_len =
      len_mod < 56 ? 56 - len_mod : 56 + SHA256_CBLOCK - len_mod;

  u8 padding[SHA256_CBLOCK] = {0x80};
  sha256_update(ctx, padding, padding_len);

  u8 len_bytes[8] = {0};
  for (usize i = 0; i < 8; i++) {
    len_bytes[i] = (u8)(len_bits >> (56 - 8 * i));
  }
  sha256_update(ctx, len_bytes, sizeof(len_bytes));
  assert(0 == ctx->partial_len);

  for (usize i = 0; i < 8; i++) {
    u32_to_bytes_be(ctx->h[i], res + i * 4);
  }

  *ctx = (Sha256Ctx){0};
}

static void sha256_digest(Slice_u8 data, u8 dst[SHA256_DIGEST_LENGTH]) {
  Sha256Ctx sha = {0};
  sha256_init(&sha);
  sha256_update(&sha, data.data, data.len);
  sha256_final(&sha, dst);
}

// Debugging aid, so kept even when nothing calls it.
__attribute((unused)) static void
sha256_print_hex(const u8 digest[SHA256_DIGEST_LENGTH]) {
  const u8 lut[] = "0123456789abcdef";

  for (usize i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    const u8 byte = digest[i];
    const u8 c1 = lut[byte & 15];
    const u8 c2 = lut[byte >> 4];
    printf("%c%c", c2, c1);
  }
}

static void sha256_digest_pair(u8 left[SHA256_DIGEST_LENGTH],
                               u8 right[SHA256_DIGEST_LENGTH],
                               u8 dst[SHA256_DIGEST_LENGTH]) {
  Sha256Ctx sha = {0};
  sha256_init(&sha);
  sha256_update(&sha, left, SHA256_DIGEST_LENGTH);
  sha256_update(&sha, right, SHA256_DIGEST_LENGTH);
  sha256_final(&sha, dst);
}

static const usize TORRENT_BLOCK_SIZE = 16 * KiB;

// Arbitrary, only needs to be a byte pattern the tree cannot produce on
// its own. See the post condition in `torrent_build_merkle_tree`.
#define MERKLE_PIECE_POISON 0xAA

// Everything about a file's merkle tree that does not vary from node to node.
// Built once by `torrent_build_merkle_tree` and passed down by pointer: the
// recursion cannot then disagree with itself about where the piece layer sits,
// and the divisions and `ctz`s happen once rather than once per node.
typedef struct {
  const Slice_u8 data;
  // Depth of the leaf layer, counting down from `0` at the root. Equivalently
  // `log2` of the block count rounded up to a power of two.
  const usize max_depth;
  // Depth at which one subtree spans exactly one piece. Only meaningful when
  // `has_piece_layer`.
  const usize piece_depth;
  // Pieces that hold file data, not counting the padding the tree is rounded
  // up with.
  const usize pieces_count;
  // BEP 52 gives no piece layer to a file that fits inside a single piece.
  const bool has_piece_layer;
  // Capacity `pieces_count`, filled in as the recursion crosses `piece_depth`.
  PieceHash *const piece_hashes;
} MerkleTree;

__attribute((warn_unused_result)) static MerkleTree
torrent_merkle_tree_make(Slice_u8 data, usize piece_length_in_bytes,
                         PieceHash *piece_hashes) {
  assert(data.data);
  assert(data.len > 0); // An empty file has no tree at all, per spec.
  assert(piece_length_in_bytes >= TORRENT_BLOCK_SIZE); // Per spec.
  assert(is_power_of_two(piece_length_in_bytes));      // Per spec.
  assert(piece_hashes);

  // A leaf is a block.
  const usize blocks_count = ceil_usize(data.len, TORRENT_BLOCK_SIZE);
  assert(blocks_count > 0);

  const usize blocks_per_piece = piece_length_in_bytes / TORRENT_BLOCK_SIZE;
  assert(blocks_per_piece > 0);
  // Load bearing: `ctzll` below is only a `log2` because of this.
  assert(is_power_of_two(blocks_per_piece));

  // `next_power_of_two` gives the padded leaf count, so its `log2` is the
  // depth those leaves sit at.
  const usize max_depth =
      (usize)__builtin_ctzll(next_power_of_two(blocks_count));
  // How many levels above the leaves one piece sits. Zero when a piece is a
  // single block, in which case the piece layer *is* the leaf layer.
  const usize piece_bits = (usize)__builtin_ctzll(blocks_per_piece);

  // Every `1 << depth` below would be undefined past this, and `max_depth` is
  // the largest depth any of them use.
  assert(max_depth < 8 * sizeof(usize));
  // The padded leaf layer covers every block, which is the whole point of
  // rounding up to a power of two.
  assert(blocks_count <= ((usize)1 << max_depth));

  // More than one piece means more than `blocks_per_piece` blocks, which
  // pushes the leaves strictly below the piece level, so the subtraction
  // cannot underflow. A single piece file skips the layer entirely and its
  // `piece_depth` is never read.
  const bool has_piece_layer = ceil_usize(blocks_count, blocks_per_piece) > 1;
  if (has_piece_layer) {
    assert(piece_bits < max_depth);
  }

  const MerkleTree tree = {
      .data = data,
      .max_depth = max_depth,
      .piece_depth = has_piece_layer ? max_depth - piece_bits : 0,
      .pieces_count = ceil_usize(blocks_count, blocks_per_piece),
      .has_piece_layer = has_piece_layer,
      .piece_hashes = piece_hashes,
  };

  assert(tree.pieces_count > 0);
  // A piece spans at least one block, so there cannot be more of them.
  assert(tree.pieces_count <= blocks_count);
  // The same count the other way round, from bytes rather than blocks. This
  // arithmetic has been wrong twice, and the caller derives its allocation
  // from the byte form, so the two must agree.
  assert(tree.pieces_count == ceil_usize(data.len, piece_length_in_bytes));
  assert(tree.piece_depth <= tree.max_depth);
  if (tree.has_piece_layer) {
    // The piece layer is a real layer, so it cannot hold more entries than it
    // has nodes.
    assert(tree.pieces_count <= ((usize)1 << tree.piece_depth));
  }

  return tree;
}

// Hash the subtree rooted at (`depth`, `tree_width_idx`) into `dst`, recording
// piece hashes into `tree->piece_hashes` on the way past `tree->piece_depth`.
// `tree_width_idx` is the index within its own level, so at `max_depth` it is
// the block index.
static void torrent_build_merkle_sub_tree(const MerkleTree *tree,
                                          usize tree_width_idx, usize depth,
                                          u8 dst[SHA256_DIGEST_LENGTH]) {
  assert(tree);
  assert(dst);
  assert(depth <= tree->max_depth);
  assert(tree_width_idx < ((usize)1 << depth));

  if (tree->max_depth == depth) { // A leaf is one block.
    const usize offset = tree_width_idx * TORRENT_BLOCK_SIZE;

    if (offset < tree->data.len) { // Still inside the file?
      const usize remaining = tree->data.len - offset;
      const Slice_u8 block_data = {
          .data = tree->data.data + offset,
          .len =
              remaining < TORRENT_BLOCK_SIZE ? remaining : TORRENT_BLOCK_SIZE,
      };
      // The last block is the only short one, and no block may read past the
      // mapping the caller handed us.
      assert(block_data.len > 0);
      assert(block_data.len <= TORRENT_BLOCK_SIZE);
      assert(offset + block_data.len <= tree->data.len);
      if (TORRENT_BLOCK_SIZE != block_data.len) {
        assert(offset + block_data.len == tree->data.len);
      }

      sha256_digest(block_data, dst);
    } else { // Past the end of the file: a zero hash, per spec.
      bzero(dst, SHA256_DIGEST_LENGTH);
    }
  } else {
    // Progress: children sit one level down and the leaf case above is the
    // only way out, so the recursion cannot run past `max_depth`.
    assert(depth < tree->max_depth);
    assert(2 * tree_width_idx + 1 < ((usize)1 << (depth + 1)));

    u8 left[SHA256_DIGEST_LENGTH] = {0};
    torrent_build_merkle_sub_tree(tree, 2 * tree_width_idx, depth + 1, left);

    u8 right[SHA256_DIGEST_LENGTH] = {0};
    torrent_build_merkle_sub_tree(tree, 2 * tree_width_idx + 1, depth + 1,
                                  right);

    sha256_digest_pair(left, right, dst);
  }

  // Both branches fall through to here on purpose: with a 16KiB piece length
  // the piece layer is the leaf layer, so recording cannot sit in the inner
  // node case alone. An index at or past `pieces_count` is a subtree made only
  // of padding, which BEP 52 leaves out of the piece layer.
  if (tree->has_piece_layer && tree->piece_depth == depth &&
      tree_width_idx < tree->pieces_count) {
    memcpy(tree->piece_hashes[tree_width_idx].digest, dst,
           SHA256_DIGEST_LENGTH);
  }
}

// Build the merkle tree for one file, yielding its root (`pieces root` in the
// info dictionary) and its piece layer (`piece layers` at the torrent root).
// An empty file has neither, per BEP 52, and leaves `root` zeroed.
__attribute((warn_unused_result)) static bool
torrent_build_merkle_tree(Slice_u8 data, usize piece_length_in_bytes,
                          PieceHash **piece_hashes, usize *piece_hashes_count,
                          u8 root[SHA256_DIGEST_LENGTH], Arena *arena) {
  assert(piece_hashes);
  assert(piece_hashes_count);
  assert(root);
  assert(piece_length_in_bytes >= TORRENT_BLOCK_SIZE); // Per spec.
  assert(is_power_of_two(piece_length_in_bytes));      // Per spec.
  assert(arena);
  assert(arena->start);

  *piece_hashes = NULL;
  *piece_hashes_count = 0;
  bzero(root, SHA256_DIGEST_LENGTH);

  if (0 == data.len) {
    return true;
  }
  assert(data.data);

  const usize pieces_count = ceil_usize(data.len, piece_length_in_bytes);
  assert(pieces_count > 0);

  PieceHash *const hashes = arena_alloc(arena, __alignof__(PieceHash),
                                        sizeof(PieceHash), pieces_count);
  // OOM?
  if (!hashes) {
    return false;
  }

  const MerkleTree tree =
      torrent_merkle_tree_make(data, piece_length_in_bytes, hashes);
  assert(tree.pieces_count == pieces_count);

  // Poison first so that a piece the traversal skips fails the check below
  // instead of passing off arena leftovers as a hash. An unwritten entry is
  // exactly how the piece arithmetic has failed before, and the zero pattern
  // would not do: a padding leaf hashes to zero, so zero is a value the tree
  // can legitimately produce.
  memset(hashes, MERKLE_PIECE_POISON, pieces_count * sizeof(PieceHash));

  torrent_build_merkle_sub_tree(&tree, 0, 0, root);

  // A single piece file has no piece layer, so nothing was written and the
  // allocation stays hidden from the caller rather than handed over unset.
  if (tree.has_piece_layer) {
    u8 poison[SHA256_DIGEST_LENGTH];
    memset(poison, MERKLE_PIECE_POISON, sizeof(poison));
    for (usize i = 0; i < tree.pieces_count; i++) {
      assert(0 != memcmp(hashes[i].digest, poison, sizeof(poison)) &&
             "the traversal missed a piece");
    }

    *piece_hashes = hashes;
    *piece_hashes_count = tree.pieces_count;
  }

  return true;
}

__attribute__((warn_unused_result)) static bool
torrent_make_info_dict_v2(Slice_u8 name, usize piece_length_in_bytes,
                          Slice_u8 file_data, Slice_u8 file_name,
                          BencodeValue *info, Arena *arena) {
  assert(piece_length_in_bytes >= 16 * KiB);      // Per spec.
  assert(is_power_of_two(piece_length_in_bytes)); // Per spec.
  assert(info);
  assert(arena);
  const usize dict_items_count = 4;
  *info = (BencodeValue){
      .kind = BencodeKindDict,
      .v.list.len = dict_items_count * 2,
      .v.list.data = arena_alloc(arena, __alignof__(BencodeValue),
                                 sizeof(BencodeValue), dict_items_count * 2),
  };
  if (NULL == info->v.list.data) {
    return false;
  }

  // `info["name"] = name`
  {
    BencodeValue *const key = &info->v.list.data[4];
    key->kind = BencodeKindString;
    key->v.s.len = sizeof("name") - 1;
    key->v.s.data = (u8 *)"name";

    BencodeValue *const value = &info->v.list.data[5];
    value->kind = BencodeKindString;
    value->v.s = name;
  }

  // `info["piece length"] = piece_length_in_bytes`
  {
    BencodeValue *const key = &info->v.list.data[6];
    key->kind = BencodeKindString;
    key->v.s.len = sizeof("piece length") - 1;
    key->v.s.data = (u8 *)"piece length";

    BencodeValue *const value = &info->v.list.data[7];
    value->kind = BencodeKindInteger;
    if (!isize_from_usize(piece_length_in_bytes, false, &value->v.num)) {
      return false;
    }
  }

  // `info["meta version"] = 2`
  {

    BencodeValue *const key = &info->v.list.data[2];
    key->kind = BencodeKindString;
    key->v.s.len = sizeof("meta version") - 1;
    key->v.s.data = (u8 *)"meta version";

    BencodeValue *const value = &info->v.list.data[3];
    value->kind = BencodeKindInteger;
    value->v.num = 2;
  }

  // `info["file tree"] = ...`
  {
    BencodeValue *const file_tree_key = &info->v.list.data[0];
    file_tree_key->kind = BencodeKindString;
    file_tree_key->v.s.len = sizeof("file tree") - 1;
    file_tree_key->v.s.data = (u8 *)"file tree";

    PieceHash *nodes = NULL;
    usize nodes_count = 0;
    u8 root[SHA256_DIGEST_LENGTH] = {0};
    if (!torrent_build_merkle_tree(file_data, piece_length_in_bytes, &nodes,
                                   &nodes_count, root, arena)) {
      return false;
    }

    BencodeValue *const file_tree_dict = &info->v.list.data[1];
    file_tree_dict->kind = BencodeKindDict;
    file_tree_dict->v.list.len = 2;
    file_tree_dict->v.list.data =
        arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                    file_tree_dict->v.list.len);
    if (NULL == file_tree_dict->v.list.data) {
      return false;
    }

    // `info["file tree"][file_name] = {}`
    {
      BencodeValue *const file_name_key = &file_tree_dict->v.list.data[0];
      file_name_key->kind = BencodeKindString;
      file_name_key->v.s = file_name;

      BencodeValue *const file_name_dict = &file_tree_dict->v.list.data[1];
      file_name_dict->kind = BencodeKindDict;
      file_name_dict->v.list.len = 2;
      file_name_dict->v.list.data =
          arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                      file_name_dict->v.list.len);
      if (NULL == file_name_dict->v.list.data) {
        return false;
      }

      // `info["file tree"][file_name][""] = {}`
      {
        BencodeValue *const empty_key = &file_name_dict->v.list.data[0];
        empty_key->kind = BencodeKindString;
        empty_key->v.s = (Slice_u8){0};

        BencodeValue *const empty_dict = &file_name_dict->v.list.data[1];
        empty_dict->kind = BencodeKindDict;
        empty_dict->v.list.len = 2 * 2;
        empty_dict->v.list.data =
            arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                        empty_dict->v.list.len);
        if (NULL == empty_dict->v.list.data) {
          return false;
        }

        // `info["file tree"][file_name][""]["length"] = file_data.length`
        {
          BencodeValue *const length_key = &empty_dict->v.list.data[0];
          length_key->kind = BencodeKindString;
          length_key->v.s = slice_u8_make((u8 *)"length", sizeof("length") - 1);

          BencodeValue *const length_value = &empty_dict->v.list.data[1];
          length_value->kind = BencodeKindInteger;
          if (!isize_from_usize(file_data.len, false, &length_value->v.num)) {
            return false;
          }
        }

        // `info["file tree"][file_name][""]["pieces root"] = root.digest`
        {
          BencodeValue *const pieces_root_key = &empty_dict->v.list.data[2];
          pieces_root_key->kind = BencodeKindString;
          pieces_root_key->v.s =
              slice_u8_make((u8 *)"pieces root", sizeof("pieces root") - 1);

          BencodeValue *const pieces_root_value = &empty_dict->v.list.data[3];
          pieces_root_value->kind = BencodeKindString;
          pieces_root_value->v.s.len = SHA256_DIGEST_LENGTH;
          pieces_root_value->v.s.data = arena_alloc(
              arena, __alignof__(u8), sizeof(u8), SHA256_DIGEST_LENGTH);
          if (NULL == pieces_root_value->v.s.data) {
            return false;
          }
          memcpy(pieces_root_value->v.s.data, root, SHA256_DIGEST_LENGTH);
        }
      }
    }
  }

  return true;
}

__attribute__((warn_unused_result)) static usize usize_digits_base_10(usize n) {
  usize digits = 1;
  while (n >= 10) {
    n /= 10;
    digits += 1;
  }

  return digits;
}

// `-ISIZE_MIN` is not representable as an `isize`, so the magnitude is taken
// in `usize`, where it always is. Conversion of a negative value to an
// unsigned type is modular, so subtracting it from zero yields exactly the
// magnitude, `ISIZE_MIN` included.
__attribute__((warn_unused_result)) static usize isize_magnitude(isize n) {
  return n < 0 ? (usize)0 - (usize)n : (usize)n;
}

// Decimal width including the sign, the mirror of `usize_digits_base_10`.
__attribute__((warn_unused_result)) static usize isize_digits_base_10(isize n) {
  return (n < 0 ? 1 : 0) + usize_digits_base_10(isize_magnitude(n));
}

// The digits are written at the *front* of `dst`, so a caller can encode
// straight into its own output buffer instead of copying out of a scratch one.
// Base 10 yields the least significant digit first, hence the up front width.
__attribute__((warn_unused_result)) static usize
encode_usize_base_10(usize n, Slice_u8 dst) {
  assert(dst.data);

  const usize digits = usize_digits_base_10(n);
  assert(dst.len >= digits);

  u8 *end = dst.data + digits;

  do {
    assert(end > dst.data);

    const usize digit = n % 10;
    *(--end) = (u8)(digit + '0');

    n /= 10;
  } while (n > 0);

  // The width matched the digits actually written, at the front of `dst`.
  assert(end == dst.data);

  return digits;
}

__attribute__((warn_unused_result)) static usize
encode_isize_base_10(isize n, Slice_u8 dst) {
  assert(dst.data);

  const bool negative = n < 0;
  const usize magnitude = isize_magnitude(n);

  if (!negative) {
    return encode_usize_base_10(magnitude, dst);
  }

  assert(dst.len >= 1);
  dst.data[0] = '-';

  const usize digits =
      encode_usize_base_10(magnitude, slice_u8_make(dst.data + 1, dst.len - 1));

  return 1 + digits;
}

__attribute__((warn_unused_result)) static usize
bencode_encode_exact_size(BencodeValue b, usize depth) {
  assert(depth <= BENCODE_MAX_DEPTH);

  usize res = 0;

  switch (b.kind) {
  case BencodeKindInteger:
    // `i` <digits> `e`
    assert(
        !__builtin_add_overflow(res, 2 + isize_digits_base_10(b.v.num), &res));
    break;
  case BencodeKindString:
    // <length> `:` <bytes>
    assert(!__builtin_add_overflow(res, usize_digits_base_10(b.v.s.len) + 1,
                                   &res));
    assert(!__builtin_add_overflow(res, b.v.s.len, &res));
    break;
  case BencodeKindList:
  case BencodeKindDict:
    // `l` or `d`, then `e`
    assert(!__builtin_add_overflow(res, 2, &res));

    for (usize i = 0; i < b.v.list.len; i++) {
      const usize item_size =
          bencode_encode_exact_size(b.v.list.data[i], depth + 1);
      assert(!__builtin_add_overflow(res, item_size, &res));
    }
    break;
  }

  return res;
}

// Sizing is a whole subtree walk, so the checks against it live in the wrapper
// below rather than here, where they would run once per level of nesting.
__attribute__((warn_unused_result)) static usize
bencode_encode_rec(BencodeValue b, Slice_u8 dst, usize depth) {
  assert(depth <= BENCODE_MAX_DEPTH);
  assert(dst.data);
  assert(dst.len >= 2);
  u8 *const dst_before = dst.data;

  switch (b.kind) {
  case BencodeKindInteger: {
    dst.data[0] = 'i';
    slice_u8_advance(&dst, 1);

    slice_u8_advance(&dst, encode_isize_base_10(b.v.num, dst));

    dst.data[0] = 'e';
    slice_u8_advance(&dst, 1);

  } break;
  case BencodeKindString: {
    slice_u8_advance(&dst, encode_usize_base_10(b.v.s.len, dst));

    dst.data[0] = ':';
    slice_u8_advance(&dst, 1);

    if (b.v.s.len > 0) {
      memcpy(dst.data, b.v.s.data, b.v.s.len);
      slice_u8_advance(&dst, b.v.s.len);
    }
  } break;
  case BencodeKindList:
  case BencodeKindDict: {
    dst.data[0] = b.kind == BencodeKindList ? 'l' : 'd';
    slice_u8_advance(&dst, 1);

    for (usize i = 0; i < b.v.list.len; i++) {
      const BencodeValue item = b.v.list.data[i];
      slice_u8_advance(&dst, bencode_encode_rec(item, dst, depth + 1));
    }

    dst.data[0] = 'e';
    slice_u8_advance(&dst, 1);
  } break;

  default:
    assert(0 && "unreachable");
  }

  assert(dst.data > dst_before);

  const usize written = (usize)(dst.data - dst_before);
  assert(written >= 2);

  return written;
}

// Encode `b` at the front of `dst` and return the number of bytes written.
//
// `dst` must be exactly `bencode_encode_exact_size(b, 0)` bytes, which the
// caller has already computed in order to allocate it. Requiring exactness
// rather than sufficiency costs nothing and buys the check below: the two
// passes are compared without walking the tree a third time, and the
// recursion cannot scribble into slack it was never given.
__attribute__((warn_unused_result)) static usize bencode_encode(BencodeValue b,
                                                                Slice_u8 dst) {
  assert(dst.data);

  const usize written = bencode_encode_rec(b, dst, 0);
  assert(dst.len == written);

  return written;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Arena memory comes straight from `mmap` and is therefore zeroed, which
// makes a read of never-written memory look like a perfectly valid zeroed
// struct. Poison it so such a read shows up as an obviously bogus value
// instead.
__attribute__((warn_unused_result)) static Arena test_arena(usize bytes_count) {
  Arena arena = arena_valloc(bytes_count);
  assert(arena.start);
  assert(arena.end);
  assert((usize)arena.end - (usize)arena.start >= bytes_count);

  memset(arena.start, 0xAA, bytes_count);
  return arena;
}

static Slice_u8 test_slice(const char *input) {
  assert(input);

  return slice_u8_make((u8 *)input, strlen(input));
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
  assert(isize_from_usize(0, false, &res));
  assert(0 == res);
  assert(isize_from_usize(123, false, &res));
  assert(123 == res);
  assert(isize_from_usize((usize)SSIZE_MAX, false, &res));
  assert(SSIZE_MAX == res);
  assert(!isize_from_usize((usize)SSIZE_MAX + 1, false, &res));
  assert(!isize_from_usize(SIZE_MAX, false, &res));

  // Negative. `|ISIZE_MIN|` is one greater than `ISIZE_MAX`.
  assert(isize_from_usize(0, true, &res));
  assert(0 == res);
  assert(isize_from_usize(123, true, &res));
  assert(-123 == res);
  assert(isize_from_usize((usize)SSIZE_MAX, true, &res));
  assert(-SSIZE_MAX == res);
  assert(isize_from_usize((usize)SSIZE_MAX + 1, true, &res));
  assert((-SSIZE_MAX - 1) == res);
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

static void test_next_power_of_two(void) {
  assert(1 == next_power_of_two(0));
  assert(1 == next_power_of_two(1));
  assert(2 == next_power_of_two(2));
  assert(4 == next_power_of_two(3));
  assert(4 == next_power_of_two(4));
  assert(8 == next_power_of_two(5));
  assert(8 == next_power_of_two(8));
  assert(16 == next_power_of_two(9));

  // Block counts, the reason this exists.
  assert(1024 == next_power_of_two(1024));
  assert(2048 == next_power_of_two(1025));

  // The largest representable power of two, and the largest input that still
  // has one.
  const usize max_power_of_two = SIZE_MAX / 2 + 1;
  assert(max_power_of_two == next_power_of_two(max_power_of_two));
  assert(max_power_of_two == next_power_of_two(max_power_of_two - 1));

  // A power of two is left alone; one past it goes up to the next.
  for (usize p = 1; p <= ((usize)1 << 20); p *= 2) {
    assert(p == next_power_of_two(p));
    assert(2 * p == next_power_of_two(p + 1));
  }

  // The postcondition holds everywhere, and rounding an already rounded value
  // changes nothing.
  for (usize n = 0; n < 4096; n++) {
    const usize res = next_power_of_two(n);

    assert(res >= n);
    assert(0 == (res & (res - 1)));
    if (1 != res) {
      assert(res / 2 < n);
    }
    assert(res == next_power_of_two(res));
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
  // A request the kernel cannot satisfy. `mmap` reports `MAP_FAILED`, not
  // NULL, so this also pins down that conversion.
  const Arena arena = arena_valloc((usize)1 << 62);
  assert(NULL == arena.start);
  assert(NULL == arena.end);
}

static void test_slice_u8(void) {
  u8 data[] = {'a', 'b', 'c'};

  // slice_u8_first.
  {
    u8 first = 0xAA;
    assert(!slice_u8_first(slice_u8_make(NULL, 0), &first));
    assert(!slice_u8_first(slice_u8_make(data, 0), &first));
    // Nothing is written when there is no first byte.
    assert(0xAA == first);

    assert(slice_u8_first(slice_u8_make(data, sizeof(data)), &first));
    assert('a' == first);

    // Peeking does not consume.
    Slice_u8 slice = slice_u8_make(data, sizeof(data));
    assert(slice_u8_first(slice, &first));
    assert(sizeof(data) == slice.len);
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

    u8 first = 0;
    assert(slice_u8_first(slice, &first));
    assert('c' == first);

    // Skipping exactly to the end is legal.
    assert(slice_u8_skip(&slice, 1));
    assert(0 == slice.len);
    assert(!slice_u8_first(slice, &first));
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
  const struct {
    const char *input;
    bool ok;
    usize num;
    // What is left of the input afterwards. Only meaningful on success.
    const char *remaining;
  } cases[] = {
      {"123e", true, 123, "e"},
      {"0e", true, 0, "e"},
      {"7:spam", true, 7, ":spam"},
      // The largest representable usize.
      {"18446744073709551615e", true, SIZE_MAX, "e"},
      // Unterminated: the digits run to the end of the input.
      {"123", false, 0, NULL},
      {"", false, 0, NULL},
      // No digit at all. The old API reported this as a success consuming
      // nothing and left it to the caller to notice.
      {"e", false, 0, NULL},
      {":spam", false, 0, NULL},
      {"-1e", false, 0, NULL},
      // A single zero is fine, leading zeroes are not.
      {"0123e", false, 0, NULL},
      {"00e", false, 0, NULL},
      // Overflow on the final add: SIZE_MAX + 1.
      {"18446744073709551616e", false, 0, NULL},
      // Overflow on the multiply: 20 nines.
      {"99999999999999999999e", false, 0, NULL},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Slice_u8 data = slice_u8_make((u8 *)cases[i].input, strlen(cases[i].input));
    const Slice_u8 before = data;

    usize num = 0xAA;
    assert(ascii_num_parse(&data, &num) == cases[i].ok);

    if (!cases[i].ok) {
      // A failed parse consumes nothing and writes nothing.
      assert(before.data == data.data);
      assert(before.len == data.len);
      assert(0xAA == num);
      continue;
    }

    assert(cases[i].num == num);
    assert(strlen(cases[i].remaining) == data.len);
    assert(0 == memcmp(data.data, cases[i].remaining, data.len));
  }

  // No data at all.
  {
    Slice_u8 data = slice_u8_make(NULL, 0);
    usize num = 0;
    assert(!ascii_num_parse(&data, &num));
  }
}

static void test_bencode_parse_num(void) {
  const struct {
    const char *input;
    bool ok;
    isize num;
    // What the parser leaves behind for the caller.
    usize remaining;
  } cases[] = {
      {"i0e", true, 0, 0},
      {"i1e", true, 1, 0},
      {"i-1e", true, -1, 0},
      {"i-123e", true, -123, 0},
      {"i9223372036854775807e", true, SSIZE_MAX, 0},
      {"i-9223372036854775808e", true, -SSIZE_MAX - 1, 0},
      // Out of range for an isize.
      {"i9223372036854775808e", false, 0, 0},
      {"i-9223372036854775809e", false, 0, 0},
      {"i18446744073709551615e", false, 0, 0},
      // Out of range for a usize.
      {"i18446744073709551616e", false, 0, 0},
      // Malformed.
      {"i-0e", false, 0, 0},
      {"ie", false, 0, 0},
      {"i-e", false, 0, 0},
      {"i0123e", false, 0, 0},
      {"i123", false, 0, 0},
      {"i123x", false, 0, 0},
      {"i", false, 0, 0},
      {"", false, 0, 0},
      {"42e", false, 0, 0},
      // Trailing data is left for the caller.
      {"i42ei43e", true, 42, 4},
  };

  for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Slice_u8 data = test_slice(cases[i].input);

    // Poisoned so that a write on the failure path is visible.
    BencodeValue value = {.kind = BencodeKindDict};
    assert(bencode_parse_num(&data, &value) == cases[i].ok);

    if (!cases[i].ok) {
      // A failed parse consumes nothing and writes nothing.
      assert(strlen(cases[i].input) == data.len);
      assert(BencodeKindDict == value.kind);
      continue;
    }

    assert(BencodeKindInteger == value.kind);
    assert(cases[i].num == value.v.num);
    assert(cases[i].remaining == data.len);
  }
}

static void test_bencode_parse_string(void) {
  const struct {
    const char *input;
    bool ok;
    const char *str;
    // What the parser leaves behind for the caller.
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
    Slice_u8 data = test_slice(cases[i].input);

    // Poisoned so that a write on the failure path is visible.
    BencodeValue value = {.kind = BencodeKindDict};
    assert(bencode_parse_string(&data, &value) == cases[i].ok);

    if (!cases[i].ok) {
      // A failed parse consumes nothing and writes nothing.
      assert(strlen(cases[i].input) == data.len);
      assert(BencodeKindDict == value.kind);
      continue;
    }

    assert(BencodeKindString == value.kind);
    assert(strlen(cases[i].str) == value.v.s.len);
    assert(0 == memcmp(value.v.s.data, cases[i].str, value.v.s.len));
    assert(cases[i].remaining == data.len);
  }

  // The string points into the input, it is not copied.
  {
    const char *const input = "4:spam";
    Slice_u8 data = test_slice(input);

    BencodeValue value = {0};
    assert(bencode_parse_string(&data, &value));
    assert((u8 *)input + 2 == value.v.s.data);
  }
}

static bool test_bencode_is_string(BencodeValue value, const char *expected) {
  assert(expected);

  const usize len = strlen(expected);
  return BencodeKindString == value.kind && len == value.v.s.len &&
         (0 == len || 0 == memcmp(value.v.s.data, expected, len));
}

static BencodeValue test_bencode_make_string(const char *data, usize len) {
  if (0 != len) {
    assert(data);
  }

  return (BencodeValue){.kind = BencodeKindString,
                        .v.s = slice_u8_make((u8 *)data, len)};
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
      // Trailing data is left for the caller, exactly like the scalar
      // parsers.
      {"i42etrailing", true, BencodeKindInteger, 0, 8},
      {"lee", true, BencodeKindList, 0, 1},
      {"lei42e", true, BencodeKindList, 0, 4},
      // A dict holds key/value pairs, so an odd number of children is
      // malformed.
      {"d3:keye", false, 0, 0, 0},
      {"di1ee", false, 0, 0, 0},
      // Dict keys must be strings, sorted by raw byte value, with no
      // duplicates. `bencode_validate_dict` is applied as each dict closes.
      {"di1ei2ee", false, 0, 0, 0},
      {"d1:a1:x1:b1:ye", true, BencodeKindDict, 4, 0},
      {"d1:b1:x1:a1:ye", false, 0, 0, 0},
      {"d1:a1:x1:a1:ye", false, 0, 0, 0},
      // Including for a dict nested inside another one.
      {"d1:ad1:b1:c1:a1:dee", false, 0, 0, 0},
      {"d1:ad1:a1:c1:b1:dee", true, BencodeKindDict, 2, 0},
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

    Slice_u8 data = test_slice(cases[i].input);
    u8 *const arena_start = arena.start;

    BencodeValue value = {0};
    assert(bencode_parse(&data, &arena, scratch, &value) == cases[i].ok);

    if (!cases[i].ok) {
      // A failed parse rolls back the input and the output arena both.
      assert(strlen(cases[i].input) == data.len);
      assert(arena_start == arena.start);
      continue;
    }

    assert(cases[i].kind == value.kind);
    assert(cases[i].remaining == data.len);

    if (BencodeKindList == value.kind || BencodeKindDict == value.kind) {
      assert(cases[i].children_len == value.v.list.len);
      // An empty container owns no allocation at all.
      if (0 != value.v.list.len) {
        assert(value.v.list.data);
      }
    }
  }

  // Every digit dispatches to the string parser.
  {
    for (u8 c = '0'; c <= '9'; c++) {
      Arena arena = test_arena(1 * KiB);
      Arena scratch = test_arena(1 * KiB);

      const char input[] = {(char)c, ':', 0};
      Slice_u8 data = test_slice(input);

      BencodeValue value = {0};
      // Only `0:` has a body short enough to succeed.
      assert(bencode_parse(&data, &arena, scratch, &value) == ('0' == c));
    }
  }

  // The whole tree, spelled out: children are stored in input order and the
  // nested containers point at their own right-sized allocations.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);

    Slice_u8 data = test_slice("ld1:a1:beli2eee");
    BencodeValue value = {0};
    assert(bencode_parse(&data, &arena, scratch, &value));
    assert(BencodeKindList == value.kind);
    assert(2 == value.v.list.len);

    const BencodeValue dict = value.v.list.data[0];
    assert(BencodeKindDict == dict.kind);
    assert(2 == dict.v.list.len);
    assert(test_bencode_is_string(dict.v.list.data[0], "a"));
    assert(test_bencode_is_string(dict.v.list.data[1], "b"));

    const BencodeValue list = value.v.list.data[1];
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
    Slice_u8 data = test_slice(input);
    BencodeValue value = {0};
    assert(bencode_parse(&data, &arena, scratch, &value));
    assert((u8 *)input + 3 == value.v.list.data[0].v.s.data);
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

      Slice_u8 data = slice_u8_make(input, 2 * depth);
      BencodeValue value = {0};
      const bool ok = bencode_parse(&data, &arena, scratch, &value);

      assert(ok == (depth <= BENCODE_MAX_DEPTH));
      if (ok) {
        assert(BencodeKindList == value.kind);
        assert(1 == value.v.list.len);
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

    Slice_u8 data = slice_u8_make(input, sizeof(input));
    BencodeValue value = {0};
    assert(bencode_parse(&data, &arena, scratch, &value));
    assert(BencodeKindList == value.kind);
    assert(children_len == value.v.list.len);

    for (usize i = 0; i < children_len; i++) {
      assert(test_bencode_is_string(value.v.list.data[i], ""));
    }
  }

  // Out of scratch: the scratch arena cannot even hold the `values` array.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(8);

    Slice_u8 data = test_slice("li1ei2ee");
    BencodeValue value = {0};
    assert(!bencode_parse(&data, &arena, scratch, &value));
  }
  // Out of output arena: the children of a container do not fit.
  {
    Arena arena = test_arena(8);
    Arena scratch = test_arena(4 * KiB);

    Slice_u8 data = test_slice("li1ee");
    BencodeValue value = {0};
    assert(!bencode_parse(&data, &arena, scratch, &value));

    // An empty container needs no allocation at all, so it still succeeds.
    Slice_u8 data_empty = test_slice("le");
    assert(bencode_parse(&data_empty, &arena, scratch, &value));
    assert(0 == value.v.list.len);
  }

  // `scratch` is taken by value: it is not consumed, and two parses in a row
  // do not tread on each other. `arena` *is* consumed, so the first result
  // stays valid while the second one is built.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);
    const u8 *const scratch_start = scratch.start;

    Slice_u8 data_a = test_slice("li1ei2ee");
    BencodeValue a = {0};
    assert(bencode_parse(&data_a, &arena, scratch, &a));
    assert(scratch_start == scratch.start);

    Slice_u8 data_b = test_slice("li3ee");
    BencodeValue b = {0};
    assert(bencode_parse(&data_b, &arena, scratch, &b));
    assert(scratch_start == scratch.start);

    assert(a.v.list.data != b.v.list.data);
    assert(2 == a.v.list.len);
    assert(1 == a.v.list.data[0].v.num);
    assert(2 == a.v.list.data[1].v.num);
    assert(1 == b.v.list.len);
    assert(3 == b.v.list.data[0].v.num);
  }

  // An input with no data at all.
  {
    Arena arena = test_arena(1 * KiB);
    Arena scratch = test_arena(1 * KiB);

    Slice_u8 data = slice_u8_make(NULL, 0);
    BencodeValue value = {0};
    assert(!bencode_parse(&data, &arena, scratch, &value));
  }

  // A parse that fails *after* allocating gives the memory back: the inner
  // list is allocated, then the unterminated outer one fails.
  {
    Arena arena = test_arena(4 * KiB);
    Arena scratch = test_arena(4 * KiB);
    u8 *const arena_start = arena.start;

    const char *const input = "lli1ee";
    Slice_u8 data = test_slice(input);

    // Poisoned so that a write on the failure path is visible.
    BencodeValue value = {.kind = BencodeKindString};
    assert(!bencode_parse(&data, &arena, scratch, &value));

    assert(arena_start == arena.start);
    assert(strlen(input) == data.len);
    assert(BencodeKindString == value.kind);
  }
}

static void test_bytes_cmp(void) {
  // A corpus in the order `bytes_cmp` must put it in. The embedded zeroes and
  // the bytes above 0x7f are there on purpose: neither `strcmp` nor a signed
  // char comparison orders these correctly.
  const struct {
    const char *data;
    usize len;
  } sorted[] = {
      {"", 0},
      {"\x00", 1},
      {"\x00\x00", 2},
      {"\x00"
       "a",
       2},
      {"a", 1},
      {"a\x00", 2},
      {"ab", 2},
      {"abc", 3},
      {"b", 1},
      {"\x7f", 1},
      {"\x80", 1},
      {"\xfe", 1},
      {"\xff", 1},
      {"\xff\x00", 2},
      {"\xff\xff", 2},
  };
  const usize count = sizeof(sorted) / sizeof(sorted[0]);

  for (usize i = 0; i < count; i++) {
    for (usize j = 0; j < count; j++) {
      u8 *const a = (u8 *)sorted[i].data;
      u8 *const b = (u8 *)sorted[j].data;

      const i32 res = bytes_cmp(a, sorted[i].len, b, sorted[j].len);
      if (i < j) {
        assert(res < 0);
      } else if (i > j) {
        assert(res > 0);
      } else {
        assert(0 == res);
      }

      // Antisymmetric: swapping the arguments flips the sign.
      const i32 swapped = bytes_cmp(b, sorted[j].len, a, sorted[i].len);
      assert((res < 0) == (swapped > 0));
      assert((res > 0) == (swapped < 0));
      assert((0 == res) == (0 == swapped));
    }
  }

  // A NULL pointer is legal as long as the length is zero, and every empty
  // byte string is equal to every other one.
  {
    assert(0 == bytes_cmp(NULL, 0, NULL, 0));
    assert(0 == bytes_cmp(NULL, 0, (u8 *)"", 0));
    assert(bytes_cmp(NULL, 0, (u8 *)"a", 1) < 0);
    assert(bytes_cmp((u8 *)"a", 1, NULL, 0) > 0);
  }

  // Longer than a word, differing only in the last byte.
  {
    u8 x[64];
    u8 y[64];
    memset(x, 'z', sizeof(x));
    memset(y, 'z', sizeof(y));
    y[sizeof(y) - 1] = 'z' + 1;

    assert(bytes_cmp(x, sizeof(x), y, sizeof(y)) < 0);
    assert(bytes_cmp(y, sizeof(y), x, sizeof(x)) > 0);

    // Against itself, and against a prefix of itself.
    assert(0 == bytes_cmp(x, sizeof(x), x, sizeof(x)));
    assert(bytes_cmp(x, sizeof(x) - 1, x, sizeof(x)) < 0);
    assert(bytes_cmp(x, sizeof(x), x, sizeof(x) - 1) > 0);
  }
}

static void test_bencode_validate_dict(void) {
  const BencodeValue num = {.kind = BencodeKindInteger, .v.num = 42};

  // Empty: trivially valid, and a NULL `data` is allowed when `len` is zero.
  {
    const BencodeList list = {0};
    assert(bencode_validate_dict(list));
  }
  // Keys strictly increasing.
  {
    BencodeValue children[] = {
        test_bencode_make_string("a", 1), num,
        test_bencode_make_string("b", 1), num,
        test_bencode_make_string("c", 1), num,
    };
    const BencodeList list = {.len = 6, .data = children};
    assert(bencode_validate_dict(list));
  }
  // Out of order, anywhere in the dict.
  {
    BencodeValue children[] = {
        test_bencode_make_string("b", 1),
        num,
        test_bencode_make_string("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(!bencode_validate_dict(list));
  }
  {
    BencodeValue children[] = {
        test_bencode_make_string("a", 1), num,
        test_bencode_make_string("c", 1), num,
        test_bencode_make_string("b", 1), num,
    };
    const BencodeList list = {.len = 6, .data = children};
    assert(!bencode_validate_dict(list));
  }
  // Duplicate keys: sorted is not enough, the order has to be strict.
  {
    BencodeValue children[] = {
        test_bencode_make_string("a", 1),
        num,
        test_bencode_make_string("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(!bencode_validate_dict(list));
  }
  // Keys must be strings.
  {
    BencodeValue children[] = {num, num};
    const BencodeList list = {.len = 2, .data = children};
    assert(!bencode_validate_dict(list));
  }
  // ... including a non-string key that is not the first one.
  {
    BencodeValue children[] = {test_bencode_make_string("a", 1), num, num, num};
    const BencodeList list = {.len = 4, .data = children};
    assert(!bencode_validate_dict(list));
  }
  // Values are not constrained, only keys are.
  {
    const BencodeValue nested_list = {.kind = BencodeKindList};
    const BencodeValue nested_dict = {.kind = BencodeKindDict};
    BencodeValue children[] = {
        test_bencode_make_string("a", 1),
        nested_list,
        test_bencode_make_string("b", 1),
        nested_dict,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(bencode_validate_dict(list));
  }
  // An odd number of children is not key/value pairs.
  {
    BencodeValue children[] = {test_bencode_make_string("a", 1), num,
                               test_bencode_make_string("b", 1)};
    const BencodeList list = {.len = 3, .data = children};
    assert(!bencode_validate_dict(list));
  }
  {
    BencodeValue children[] = {test_bencode_make_string("a", 1)};
    const BencodeList list = {.len = 1, .data = children};
    assert(!bencode_validate_dict(list));
  }
  // The empty key is legal and sorts before every other key.
  {
    BencodeValue children[] = {
        test_bencode_make_string("", 0),
        num,
        test_bencode_make_string("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(bencode_validate_dict(list));
  }
  // A key that is a prefix of the next one is in order; the reverse is not.
  {
    BencodeValue children[] = {
        test_bencode_make_string("a", 1),
        num,
        test_bencode_make_string("ab", 2),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(bencode_validate_dict(list));
  }
  {
    BencodeValue children[] = {
        test_bencode_make_string("ab", 2),
        num,
        test_bencode_make_string("a", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(!bencode_validate_dict(list));
  }
  // Ordering is by raw byte value, so `0x80` sorts *after* `0x7f`. A signed
  // comparison would get this pair backwards.
  {
    BencodeValue children[] = {
        test_bencode_make_string("\x7f", 1),
        num,
        test_bencode_make_string("\x80", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(bencode_validate_dict(list));
  }
  {
    BencodeValue children[] = {
        test_bencode_make_string("\x80", 1),
        num,
        test_bencode_make_string("\x7f", 1),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(!bencode_validate_dict(list));
  }
  // Keys are compared over their whole length, zero bytes included.
  {
    BencodeValue children[] = {
        test_bencode_make_string("a\x00"
                                 "a",
                                 3),
        num,
        test_bencode_make_string("a\x00"
                                 "b",
                                 3),
        num,
    };
    const BencodeList list = {.len = 4, .data = children};
    assert(bencode_validate_dict(list));
  }
}

// Hash `data` in one `Update` call, the simplest possible use of the API.
static void test_sha256_once(Slice_u8 data, u8 res[SHA256_DIGEST_LENGTH]) {
  Sha256Ctx ctx = {0};
  sha256_init(&ctx);
  sha256_update(&ctx, data.data, data.len);
  sha256_final(&ctx, res);
}

// Expected digests are given as hex, the way every SHA-256 test vector in the
// wild is published, so that a vector can be pasted in unmodified.
static void test_digest_from_hex(const char *hex,
                                 u8 res[SHA256_DIGEST_LENGTH]) {
  assert(hex);
  assert(2 * SHA256_DIGEST_LENGTH == strlen(hex));

  for (usize i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    u32 byte = 0;
    assert(1 == sscanf(hex + 2 * i, "%2x", &byte));
    res[i] = (u8)byte;
  }
}

static void test_sha256_expect_hex(Slice_u8 data, const char *expected_hex) {
  u8 expected[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(expected_hex, expected);

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  test_sha256_once(data, actual);

  assert(0 == memcmp(actual, expected, sizeof(actual)));
}

static void test_sha256_vectors(void) {
  // FIPS 180-2 / NIST CAVP vectors.
  test_sha256_expect_hex(
      test_slice(""),
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  test_sha256_expect_hex(
      test_slice("abc"),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56 bytes: the shortest message whose padding needs a second block.
  test_sha256_expect_hex(
      test_slice("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  test_sha256_expect_hex(
      test_slice("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijk"
                 "lmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
      "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

  // A NUL byte is data like any other: the API takes a length, never a C
  // string.
  test_sha256_expect_hex(
      slice_u8_make((u8 *)"\x00", 1),
      "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d");
}

// The million 'a' vector, fed in odd-sized chunks so that the partial block
// handling is exercised on a message far longer than one block.
static void test_sha256_million_a(void) {
  Sha256Ctx ctx = {0};
  sha256_init(&ctx);

  u8 chunk[1000] = {0};
  memset(chunk, 'a', sizeof(chunk));
  for (usize i = 0; i < 1000; i++) {
    sha256_update(&ctx, chunk, sizeof(chunk));
  }

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&ctx, actual);

  const u8 expected[SHA256_DIGEST_LENGTH] = {
      0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7,
      0xe2, 0x84, 0xd7, 0x3e, 0x67, 0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97,
      0x20, 0x0e, 0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0,
  };
  assert(0 == memcmp(actual, expected, sizeof(actual)));
}

// Any split of the same message must give the same digest: one `Update`, one
// `Update` per byte, and a split at every offset around the block boundary.
static void test_sha256_incremental(void) {
  u8 input[200] = {0};
  for (usize i = 0; i < sizeof(input); i++) {
    input[i] = (u8)(i * 31 + 7);
  }

  u8 expected[SHA256_DIGEST_LENGTH] = {0};
  test_sha256_once(slice_u8_make(input, sizeof(input)), expected);

  {
    Sha256Ctx ctx = {0};
    sha256_init(&ctx);
    for (usize i = 0; i < sizeof(input); i++) {
      sha256_update(&ctx, input + i, 1);
    }

    u8 actual[SHA256_DIGEST_LENGTH] = {0};
    sha256_final(&ctx, actual);
    assert(0 == memcmp(actual, expected, sizeof(actual)));
  }

  for (usize split = 0; split <= sizeof(input); split++) {
    Sha256Ctx ctx = {0};
    sha256_init(&ctx);
    sha256_update(&ctx, input, split);
    // An empty `Update` in the middle must be a no-op, including when the
    // slice has no data pointer at all.
    sha256_update(&ctx, NULL, 0);
    sha256_update(&ctx, input + split, sizeof(input) - split);

    u8 actual[SHA256_DIGEST_LENGTH] = {0};
    sha256_final(&ctx, actual);
    assert(0 == memcmp(actual, expected, sizeof(actual)));
  }
}

// Every message length from 0 to 1024 bytes, which covers every padding case
// and every partial block length. One digest stands for all of them: they are
// themselves hashed, in order, and the result compared to a constant obtained
// from a reference implementation.
static void test_sha256_lengths(void) {
  u8 input[1025] = {0};
  for (usize i = 0; i < sizeof(input); i++) {
    input[i] = (u8)(i * 31 + 7);
  }

  Sha256Ctx outer = {0};
  sha256_init(&outer);

  for (usize len = 0; len < sizeof(input); len++) {
    u8 digest[SHA256_DIGEST_LENGTH] = {0};
    test_sha256_once(slice_u8_make(input, len), digest);
    sha256_update(&outer, digest, sizeof(digest));
  }

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&outer, actual);

  const u8 expected[SHA256_DIGEST_LENGTH] = {
      0x70, 0x2f, 0xea, 0x77, 0xff, 0x7e, 0x99, 0xf9, 0xf5, 0x44, 0x35,
      0x64, 0x87, 0x2a, 0x6d, 0xe2, 0x52, 0x04, 0xa0, 0x69, 0xe1, 0x42,
      0x4f, 0xea, 0xc3, 0xff, 0x3e, 0x25, 0x19, 0x52, 0x2f, 0x15,
  };
  assert(0 == memcmp(actual, expected, sizeof(actual)));
}

// `Init` must fully reset a context, so that reusing one is the same as
// starting from a fresh `{0}` one.
static void test_sha256_reuse(void) {
  u8 expected[SHA256_DIGEST_LENGTH] = {0};
  test_sha256_once(test_slice("abc"), expected);

  Sha256Ctx ctx = {0};
  sha256_init(&ctx);
  const Slice_u8 other = test_slice("some other message entirely");
  sha256_update(&ctx, other.data, other.len);

  u8 discarded[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&ctx, discarded);

  sha256_init(&ctx);
  const Slice_u8 abc = test_slice("abc");
  sha256_update(&ctx, abc.data, abc.len);

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&ctx, actual);
  assert(0 == memcmp(actual, expected, sizeof(actual)));
}

// ---------------------------------------------------------------------------
// BEP 52 merkle tree
// ---------------------------------------------------------------------------

// The largest file any merkle test builds a tree for: 13 blocks, which pads to
// 16 leaves and so exercises a subtree that is entirely padding.
#define TEST_MERKLE_MAX_LEN (208 * KiB)

// A constant fill would not notice two leaves being swapped, so give every
// block distinct content. The expected roots below come from libtorrent 2.1.1
// fed the exact same bytes, checked at both a 16KiB and a 256KiB piece size
// since `pieces root` must not depend on the piece size.
static Slice_u8 test_merkle_data(Arena *arena) {
  u8 *const buf = arena_alloc(arena, 1, 1, TEST_MERKLE_MAX_LEN);
  assert(buf);

  u32 x = 0x12345678;
  for (usize i = 0; i < TEST_MERKLE_MAX_LEN; i++) {
    // Numerical Recipes LCG. Only the top byte is used, the low bits of an
    // LCG being far too regular to tell two blocks apart.
    x = x * 1664525u + 1013904223u;
    buf[i] = (u8)(x >> 24);
  }

  return slice_u8_make(buf, TEST_MERKLE_MAX_LEN);
}

// Known answer tests. Sizes bracket every boundary the tree construction has:
// shorter than a block, exactly a block, one byte past a block, an exact
// power of two number of blocks, and block counts needing one or several
// padding leaves.
static void test_torrent_merkle_vectors(void) {
  Arena data_arena = arena_valloc(TEST_MERKLE_MAX_LEN + 4 * KiB);
  assert(data_arena.start);
  const Slice_u8 data = test_merkle_data(&data_arena);

  const struct {
    usize len;
    const char *root;
  } vectors[] = {
      {1, "0bfe935e70c321c7ca3afc75ce0d0ca2f98b5422e008bb31c00c6d7f1f1c0ad6"},
      {16383,
       "3dd5a06f7768acc49864c34e60083ab8af5d90cb4fad8388c0391eedbb4ef37b"},
      {16384,
       "6c2151ba392602898475e8faea682c1923ca241d0cee54d984b520286e005f0d"},
      {16385,
       "26d66f8578c6efa252c8b8deb3810b923392392618bcfea7ce34e1ca0ccf9211"},
      {32768,
       "e1bdca49b140c3391bb85e630b7cf05a73534b40d4fe2db5e60bfacd45be473a"},
      {40960,
       "809301c68152c8413d38cb646268c1f5caa2dc5bf359d7148eaa9ea3aa0399a7"},
      {81920,
       "4e1b3e51bf462b7d9863084f4c139a2106c729c0e0293b8abf692905ea57eee6"},
      {131072,
       "d4e69a223d5f61c604c613be46f12a017e134baec50d7cc960a25ba3dfcf5fc3"},
      {212992,
       "4c3bf66a99395bf30f382494b6e96fd9976df5ef489ae1a2173af1c1dc841f46"},
  };

  // `pieces root` is defined over 16KiB blocks, so it must come out the same
  // whatever the operator picked for the piece size. The expected values were
  // taken from libtorrent 2.1.1 at both ends of this range.
  const usize piece_lengths_in_bytes[] = {16 * KiB, 32 * KiB, 256 * KiB};

  for (usize i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
    assert(vectors[i].len <= data.len);

    u8 expected[SHA256_DIGEST_LENGTH] = {0};
    test_digest_from_hex(vectors[i].root, expected);

    for (usize p = 0;
         p < sizeof(piece_lengths_in_bytes) / sizeof(piece_lengths_in_bytes[0]);
         p++) {
      // Poisoned, so that a hash the implementation never writes cannot pass
      // itself off as a zeroed padding hash.
      Arena arena = test_arena(64 * KiB);

      PieceHash *pieces = NULL;
      usize pieces_count = 0;
      u8 root[SHA256_DIGEST_LENGTH] = {0};
      assert(torrent_build_merkle_tree(slice_u8_make(data.data, vectors[i].len),
                                       piece_lengths_in_bytes[p], &pieces,
                                       &pieces_count, root, &arena));

      assert(0 == memcmp(root, expected, sizeof(expected)));
    }
  }
}

// The piece layer, which is what actually ships in the `.torrent` next to the
// info dictionary. Checked against an independently built tree rather than
// against the implementation's own intermediate state.
static void test_torrent_merkle_piece_layer(void) {
  Arena data_arena = arena_valloc(TEST_MERKLE_MAX_LEN + 4 * KiB);
  assert(data_arena.start);
  const Slice_u8 data = test_merkle_data(&data_arena);

  const usize lens[] = {1, 16384, 16385, 40960, 81920, 131072, 212992};
  const usize piece_lengths_in_bytes[] = {16 * KiB, 32 * KiB, 64 * KiB,
                                          256 * KiB};

  for (usize i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
    const usize len = lens[i];
    assert(len <= data.len);

    for (usize p = 0;
         p < sizeof(piece_lengths_in_bytes) / sizeof(piece_lengths_in_bytes[0]);
         p++) {
      const usize piece_length_in_bytes = piece_lengths_in_bytes[p];

      Arena arena = test_arena(64 * KiB);

      PieceHash *pieces = NULL;
      usize pieces_count = 0;
      u8 root[SHA256_DIGEST_LENGTH] = {0};
      assert(torrent_build_merkle_tree(slice_u8_make(data.data, len),
                                       piece_length_in_bytes, &pieces,
                                       &pieces_count, root, &arena));

      const usize expected_pieces = ceil_usize(len, piece_length_in_bytes);
      if (expected_pieces > 1) {
        assert(pieces);
        assert(expected_pieces == pieces_count);
      } else { // A file inside one piece has no piece layer, per BEP 52.
        assert(NULL == pieces);
        assert(0 == pieces_count);
      }

      // Build the whole tree the slow, obvious way and read the answers off
      // it. `TEST_MERKLE_MAX_LEN` is 13 blocks, so 16 leaves covers every case.
      const usize blocks = ceil_usize(len, TORRENT_BLOCK_SIZE);
      const usize leaves = next_power_of_two(blocks);
      assert(leaves <= 16);

      u8 layer[16][SHA256_DIGEST_LENGTH] = {{0}};
      for (usize l = 0; l < leaves; l++) {
        if (l < blocks) {
          const usize offset = l * TORRENT_BLOCK_SIZE;
          const usize block_len = len - offset < TORRENT_BLOCK_SIZE
                                      ? len - offset
                                      : TORRENT_BLOCK_SIZE;
          sha256_digest(slice_u8_make(data.data + offset, block_len), layer[l]);
        } // Padding leaves stay zero, per BEP 52.
      }

      // Width of the level where one node spans exactly one piece. Zero when
      // a piece is wider than the whole padded tree, which never matches
      // below and so checks nothing, which is right: there is no piece layer.
      const usize blocks_per_piece = piece_length_in_bytes / TORRENT_BLOCK_SIZE;
      const usize piece_width = leaves / blocks_per_piece;

      for (usize width = leaves;; width /= 2) {
        if (0 != pieces_count && width == piece_width) {
          assert(pieces_count <= width);
          for (usize piece = 0; piece < pieces_count; piece++) {
            assert(0 == memcmp(pieces[piece].digest, layer[piece],
                               SHA256_DIGEST_LENGTH));
          }
        }

        if (1 == width) {
          break;
        }

        for (usize w = 0; w < width / 2; w++) {
          // `w <= 2 * w`, and the pair is read before the write lands, so
          // folding in place cannot clobber an input it still needs.
          sha256_digest_pair(layer[2 * w], layer[2 * w + 1], layer[w]);
        }
      }

      assert(0 == memcmp(root, layer[0], SHA256_DIGEST_LENGTH));
    }
  }
}

// The padding rule, which is the part of BEP 52 that is easiest to get wrong:
// leaves past the end of the file are 32 zero bytes, and only the leaf layer
// is zeroed. Everything above it is hashed normally, so a node covering
// nothing but padding is emphatically not zero.
static void test_torrent_merkle_padding(void) {
  Arena data_arena = arena_valloc(64 * KiB);
  assert(data_arena.start);

  // Three blocks, so the tree pads to four leaves and the last leaf covers no
  // file data at all. The tail block is one byte long.
  const usize len = 2 * TORRENT_BLOCK_SIZE + 1;
  u8 *const buf = arena_alloc(&data_arena, 1, 1, len);
  assert(buf);
  memset(buf, 'a', len);

  Arena arena = test_arena(64 * KiB);

  PieceHash *pieces = NULL;
  usize pieces_count = 0;
  u8 root[SHA256_DIGEST_LENGTH] = {0};
  // One block per piece, so the piece layer is the leaf layer and every leaf
  // that holds file data is observable.
  assert(torrent_build_merkle_tree(slice_u8_make(buf, len), 16 * KiB, &pieces,
                                   &pieces_count, root, &arena));
  assert(pieces);
  assert(3 == pieces_count);

  // The three real leaves are the hashes of the three blocks, the last at its
  // true length rather than zero extended to a full block.
  for (usize l = 0; l < 3; l++) {
    const usize offset = l * TORRENT_BLOCK_SIZE;
    const usize block_len =
        len - offset < TORRENT_BLOCK_SIZE ? len - offset : TORRENT_BLOCK_SIZE;
    u8 expected[SHA256_DIGEST_LENGTH] = {0};
    sha256_digest(slice_u8_make(buf + offset, block_len), expected);
    assert(0 == memcmp(pieces[l].digest, expected, sizeof(expected)));
  }

  // The fourth leaf is padding and so is absent from the piece layer, which
  // is the truncation BEP 52 asks for.
  assert(3 == pieces_count);

  // Now pin the padding rule through the root, the only place it is visible:
  // the padding leaf is 32 zero bytes, not the hash of a zero filled block.
  u8 leaf[4][SHA256_DIGEST_LENGTH] = {{0}};
  for (usize l = 0; l < 3; l++) {
    memcpy(leaf[l], pieces[l].digest, SHA256_DIGEST_LENGTH);
  } // `leaf[3]` stays zero.

  u8 left[SHA256_DIGEST_LENGTH] = {0};
  u8 right[SHA256_DIGEST_LENGTH] = {0};
  u8 expected_root[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest_pair(leaf[0], leaf[1], left);
  sha256_digest_pair(leaf[2], leaf[3], right);
  sha256_digest_pair(left, right, expected_root);
  assert(0 == memcmp(root, expected_root, sizeof(expected_root)));

  // The other plausible reading of "set to zero" gives a different root, so
  // the test above is actually discriminating.
  u8 *const zero_block = arena_alloc(&data_arena, 1, 1, TORRENT_BLOCK_SIZE);
  assert(zero_block);
  memset(zero_block, 0, TORRENT_BLOCK_SIZE);
  sha256_digest(slice_u8_make(zero_block, TORRENT_BLOCK_SIZE), leaf[3]);

  u8 wrong_root[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest_pair(leaf[2], leaf[3], right);
  sha256_digest_pair(left, right, wrong_root);
  assert(0 != memcmp(root, wrong_root, sizeof(wrong_root)));

  // And a node above the leaf layer is hashed normally even when everything
  // under it is padding, so it is emphatically not zero.
  const u8 zero[SHA256_DIGEST_LENGTH] = {0};
  u8 all_padding[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest_pair((u8 *)zero, (u8 *)zero, all_padding);
  assert(0 != memcmp(all_padding, zero, sizeof(zero)));
}

// BEP 52: an empty file has no pieces root at all.
static void test_torrent_merkle_empty(void) {
  Arena arena = test_arena(64 * KiB);

  // Preset to garbage: every out parameter must be cleared, since a caller
  // has no other way to tell that no tree was built.
  PieceHash *pieces = (PieceHash *)(usize)0xdeadbeef;
  usize pieces_count = 123;
  u8 root[SHA256_DIGEST_LENGTH];
  memset(root, 0xcd, sizeof(root));

  assert(torrent_build_merkle_tree((Slice_u8){0}, 256 * KiB, &pieces,
                                   &pieces_count, root, &arena));
  assert(NULL == pieces);
  assert(0 == pieces_count);

  const u8 zero[SHA256_DIGEST_LENGTH] = {0};
  assert(0 == memcmp(root, zero, sizeof(zero)));
}

// The one failure path: an arena too small for the tree is reported, not
// asserted, and leaves nothing half built behind.
static void test_torrent_merkle_oom(void) {
  Arena data_arena = arena_valloc(64 * KiB);
  assert(data_arena.start);

  // Two blocks, so at one block per piece the layer needs two hashes.
  const usize len = TORRENT_BLOCK_SIZE + 1;
  u8 *const buf = arena_alloc(&data_arena, 1, 1, len);
  assert(buf);
  memset(buf, 'a', len);

  Arena arena = test_arena(4 * KiB);

  // Leave room for fewer hashes than the piece layer needs.
  const usize free_bytes = (usize)(arena.end - arena.start);
  assert(free_bytes > sizeof(PieceHash));
  u8 *const hog = arena_alloc(&arena, 1, 1, free_bytes - sizeof(PieceHash));
  assert(hog);

  PieceHash *pieces = NULL;
  usize pieces_count = 0;
  u8 root[SHA256_DIGEST_LENGTH] = {0};
  assert(!torrent_build_merkle_tree(slice_u8_make(buf, len), 16 * KiB, &pieces,
                                    &pieces_count, root, &arena));
  assert(NULL == pieces);
  assert(0 == pieces_count);
}

// ---------------------------------------------------------------------------
// encode_usize_base_10
// ---------------------------------------------------------------------------

// The digits are written at the front of `dst`, so every case checks three
// things: the text, that the returned slice really does start at `dst.data`,
// and that nothing outside that slice was touched.
static void test_usize_digits_base_10(void) {
  assert(1 == usize_digits_base_10(0));
  assert(1 == usize_digits_base_10(9));
  assert(2 == usize_digits_base_10(10));
  assert(2 == usize_digits_base_10(99));
  assert(3 == usize_digits_base_10(100));

  // Every power of ten and the value one below it: the two values that
  // straddle each digit count boundary.
  usize power = 1;
  for (usize digits = 1; digits <= 19; digits++) {
    assert(digits == usize_digits_base_10(power));
    assert(digits == usize_digits_base_10(power * 10 - 1));
    power *= 10;
  }

  // 10^19 and `SIZE_MAX` are both 20 digits, the widest a `usize` gets.
  assert(20 == usize_digits_base_10(power));
  assert(20 == usize_digits_base_10(SIZE_MAX));
}

static void test_encode_usize_once(usize n, const char *expected) {
  assert(expected);

  const usize dst_len = 24;
  u8 buf[64];
  assert(dst_len < sizeof(buf));
  memset(buf, '#', sizeof(buf));

  const usize written = encode_usize_base_10(n, slice_u8_make(buf, dst_len));

  // Comparing from the front of the buffer is what pins the anchoring now that
  // there is no returned pointer: the digits begin at `dst.data`, which is
  // what lets a caller encode straight into its own output buffer.
  assert(strlen(expected) == written);
  assert(0 == memcmp(buf, expected, written));

  // Everything after the digits, inside `dst` and past it, is untouched.
  for (usize i = written; i < sizeof(buf); i++) {
    assert('#' == buf[i]);
  }
}

static void test_encode_usize_base_10(void) {
  // Zero has no digits to peel off, which is the case a `while (n > 0)` loop
  // silently encodes as nothing at all.
  test_encode_usize_once(0, "0");

  for (usize d = 0; d <= 9; d++) {
    const char one_digit[2] = {(char)('0' + d), 0};
    test_encode_usize_once(d, one_digit);
  }

  // Either side of every digit count boundary.
  test_encode_usize_once(9, "9");
  test_encode_usize_once(10, "10");
  test_encode_usize_once(99, "99");
  test_encode_usize_once(100, "100");
  test_encode_usize_once(999, "999");
  test_encode_usize_once(1000, "1000");
  test_encode_usize_once(123456789, "123456789");

  // Trailing zeroes are not leading zeroes: `100` must not come out as `1`.
  test_encode_usize_once(10000000000000000000ull, "10000000000000000000");

  // 20 digits, the widest a `usize` gets.
  test_encode_usize_once(SIZE_MAX - 1, "18446744073709551614");
  test_encode_usize_once(SIZE_MAX, "18446744073709551615");
}

// There is no fixed minimum `dst`: a buffer of exactly the needed width
// works, which is what lets a caller size its output exactly instead of
// padding for the widest possible number.
static void test_encode_usize_base_10_exact_fit(void) {
  u8 buf[24];

  memset(buf, '#', sizeof(buf));
  const usize widest = encode_usize_base_10(SIZE_MAX, slice_u8_make(buf, 20));
  assert(20 == widest);
  assert(0 == memcmp(buf, "18446744073709551615", 20));

  memset(buf, '#', sizeof(buf));
  const usize one = encode_usize_base_10(7, slice_u8_make(buf, 1));
  assert(1 == one);
  assert('7' == buf[0]);
  assert('#' == buf[1]);

  memset(buf, '#', sizeof(buf));
  const usize three = encode_usize_base_10(123, slice_u8_make(buf, 3));
  assert(3 == three);
  assert(0 == memcmp(buf, "123", 3));
  assert('#' == buf[3]);
}

// Cross check against the platform formatter, and read the result back with
// this project's own parser. The parser rejects leading zeroes, so it also
// pins down that the encoder never emits one.
static void test_encode_usize_base_10_round_trip(void) {
  const usize values[] = {
      0,
      1,
      2,
      7,
      9,
      10,
      11,
      42,
      99,
      100,
      101,
      255,
      256,
      999,
      1000,
      65535,
      65536,
      999999,
      1000000,
      4294967295,
      4294967296,
      SIZE_MAX / 2,
      SIZE_MAX - 1,
      SIZE_MAX,
  };

  for (usize i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    const usize n = values[i];

    u8 buf[24];
    memset(buf, '#', sizeof(buf));
    const usize got = encode_usize_base_10(n, slice_u8_make(buf, sizeof(buf)));

    char expected[32] = {0};
    const i32 written = snprintf(expected, sizeof(expected), "%zu", n);
    assert(written > 0);
    assert((usize)written == got);
    assert(0 == memcmp(buf, expected, got));

    // `ascii_num_parse` wants a non-digit terminator, the way `i123e` has
    // one.
    u8 terminated[32] = {0};
    memcpy(terminated, buf, got);
    terminated[got] = 'e';

    Slice_u8 to_parse = slice_u8_make(terminated, got + 1);
    usize parsed = 0;
    assert(ascii_num_parse(&to_parse, &parsed));
    assert(n == parsed);
  }
}

static void test_encode_isize_once(isize n, const char *expected) {
  assert(expected);

  const usize dst_len = 24;
  u8 buf[64];
  assert(dst_len < sizeof(buf));
  memset(buf, '#', sizeof(buf));

  const usize written = encode_isize_base_10(n, slice_u8_make(buf, dst_len));

  // Comparing from the front of the buffer is what pins the anchoring now that
  // there is no returned pointer: the digits begin at `dst.data`, which is
  // what lets a caller encode straight into its own output buffer.
  assert(strlen(expected) == written);
  assert(0 == memcmp(buf, expected, written));

  // Everything after the digits, inside `dst` and past it, is untouched.
  for (usize i = written; i < sizeof(buf); i++) {
    assert('#' == buf[i]);
  }
}

static void test_encode_isize_base_10(void) {
  // Zero is not negative: no `-0`.
  test_encode_isize_once(0, "0");

  test_encode_isize_once(1, "1");
  test_encode_isize_once(-1, "-1");
  test_encode_isize_once(9, "9");
  test_encode_isize_once(-9, "-9");

  // Either side of every digit count boundary, both signs.
  test_encode_isize_once(10, "10");
  test_encode_isize_once(-10, "-10");
  test_encode_isize_once(99, "99");
  test_encode_isize_once(-99, "-99");
  test_encode_isize_once(100, "100");
  test_encode_isize_once(-100, "-100");
  test_encode_isize_once(123456789, "123456789");
  test_encode_isize_once(-123456789, "-123456789");

  // The asymmetric boundary: `|ISIZE_MIN|` is one greater than `ISIZE_MAX`,
  // so negating it in the signed domain would overflow.
  test_encode_isize_once(INT64_MAX, "9223372036854775807");
  test_encode_isize_once(INT64_MIN + 1, "-9223372036854775807");
  test_encode_isize_once(INT64_MIN, "-9223372036854775808");
}

// Exact fit again, this time including the sign: the widest `isize` is the
// negative one, 19 digits plus a sign.
static void test_encode_isize_base_10_exact_fit(void) {
  u8 buf[24];

  memset(buf, '#', sizeof(buf));
  const usize widest = encode_isize_base_10(INT64_MIN, slice_u8_make(buf, 20));
  assert(20 == widest);
  assert(0 == memcmp(buf, "-9223372036854775808", 20));

  memset(buf, '#', sizeof(buf));
  const usize two = encode_isize_base_10(-7, slice_u8_make(buf, 2));
  assert(2 == two);
  assert(0 == memcmp(buf, "-7", 2));
  assert('#' == buf[2]);
}

// Cross check against the platform formatter, then frame the digits as a
// bencode integer and read them back with this project's own parser.
static void test_encode_isize_base_10_round_trip(void) {
  const isize values[] = {
      0,        1,          -1,          2,         -2,        9,
      -9,       10,         -10,         99,        -99,       100,
      -100,     255,        -256,        65535,     -65536,    1000000,
      -1000000, 4294967296, -4294967296, INT64_MAX, INT64_MIN, INT64_MIN + 1,
  };

  for (usize i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    const isize n = values[i];

    u8 buf[24];
    memset(buf, '#', sizeof(buf));
    const usize got = encode_isize_base_10(n, slice_u8_make(buf, sizeof(buf)));

    char expected[32] = {0};
    const i32 written = snprintf(expected, sizeof(expected), "%zd", n);
    assert(written > 0);
    assert((usize)written == got);
    assert(0 == memcmp(buf, expected, got));

    // `i<n>e`, the way a bencode integer is framed.
    u8 framed[32] = {0};
    framed[0] = 'i';
    memcpy(framed + 1, buf, got);
    framed[1 + got] = 'e';

    Slice_u8 to_parse = slice_u8_make(framed, got + 2);
    BencodeValue parsed = {0};
    assert(bencode_parse_num(&to_parse, &parsed));
    assert(BencodeKindInteger == parsed.kind);
    assert(n == parsed.v.num);
  }
}

// ---------------------------------------------------------------------------
// bencode_encode
// ---------------------------------------------------------------------------

// Encode into a poisoned buffer one byte wider than needed, so a write past
// the end is visible, and compare against the exact expected bytes.
static void test_bencode_encode_once(BencodeValue b, const char *expected) {
  assert(expected);
  const usize expected_len = strlen(expected);

  // The sizing pass predicts the encoding to the byte, so it is a check rather
  // than a bound.
  const usize size = bencode_encode_exact_size(b, 0);
  assert(size == expected_len);

  // Allocate one byte more than that, so a write past the end is visible.
  const usize cap = size + 1;

  Arena arena = test_arena(64 * KiB);
  Slice_u8 dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
                  .len = cap};
  assert(dst.data);
  memset(dst.data, '#', cap);

  const usize written = bencode_encode(b, slice_u8_take(dst, size));

  // Comparing from the front of `dst` pins the anchoring.
  assert(expected_len == written);
  assert(0 == memcmp(dst.data, expected, written));

  // Nothing past what it reports was touched.
  for (usize i = written; i < cap; i++) {
    assert('#' == dst.data[i]);
  }
}

static BencodeValue test_bencode_int(isize n) {
  return (BencodeValue){.kind = BencodeKindInteger, .v.num = n};
}

static BencodeValue test_bencode_str(const char *s) {
  return (BencodeValue){.kind = BencodeKindString, .v.s = test_slice(s)};
}

static void test_bencode_encode_leaves(void) {
  test_bencode_encode_once(test_bencode_int(0), "i0e");
  test_bencode_encode_once(test_bencode_int(1), "i1e");
  test_bencode_encode_once(test_bencode_int(-1), "i-1e");
  test_bencode_encode_once(test_bencode_int(42), "i42e");
  test_bencode_encode_once(test_bencode_int(-42), "i-42e");
  test_bencode_encode_once(test_bencode_int(INT64_MAX),
                           "i9223372036854775807e");
  test_bencode_encode_once(test_bencode_int(INT64_MIN),
                           "i-9223372036854775808e");

  test_bencode_encode_once(test_bencode_str(""), "0:");
  test_bencode_encode_once(test_bencode_str("a"), "1:a");
  test_bencode_encode_once(test_bencode_str("spam"), "4:spam");
  test_bencode_encode_once(test_bencode_str("piece length"), "12:piece length");

  // A string whose `data` is null, which is how the `""` key of a v2 file tree
  // is built. `memcpy` wants valid pointers even for a zero byte copy.
  const BencodeValue null_str = {.kind = BencodeKindString, .v.s = {0}};
  test_bencode_encode_once(null_str, "0:");
}

// Strings are byte strings: NULs and high bytes pass through untouched, and the
// length prefix counts bytes rather than stopping at a terminator.
static void test_bencode_encode_binary_string(void) {
  u8 raw[6] = {0x00, 0xff, 'a', 0x00, 0x80, '\n'};
  const BencodeValue b = {.kind = BencodeKindString,
                          .v.s = slice_u8_make(raw, sizeof(raw))};

  Arena arena = test_arena(64 * KiB);
  const usize cap = bencode_encode_exact_size(b, 0);
  Slice_u8 dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
                  .len = cap};
  assert(dst.data);

  const usize written = bencode_encode(b, dst);

  assert(bencode_encode_exact_size(b, 0) == written);
  assert(2 + sizeof(raw) == written);
  assert(0 == memcmp(dst.data, "6:", 2));
  assert(0 == memcmp(dst.data + 2, raw, sizeof(raw)));
}

static void test_bencode_encode_containers(void) {
  Arena arena = test_arena(64 * KiB);

  // Empty containers still carry their framing.
  const BencodeValue empty_list = {.kind = BencodeKindList, .v.list = {0}};
  const BencodeValue empty_dict = {.kind = BencodeKindDict, .v.list = {0}};
  test_bencode_encode_once(empty_list, "le");
  test_bencode_encode_once(empty_dict, "de");

  // `l4:spami42ee`
  BencodeValue *items =
      arena_alloc(&arena, __alignof__(BencodeValue), sizeof(BencodeValue), 2);
  assert(items);
  items[0] = test_bencode_str("spam");
  items[1] = test_bencode_int(42);
  const BencodeValue list = {
      .kind = BencodeKindList, .v.list.len = 2, .v.list.data = items};
  test_bencode_encode_once(list, "l4:spami42ee");

  // `d3:keyl4:spami42eee`: a container nested inside a dict.
  BencodeValue *pair =
      arena_alloc(&arena, __alignof__(BencodeValue), sizeof(BencodeValue), 2);
  assert(pair);
  pair[0] = test_bencode_str("key");
  pair[1] = list;
  const BencodeValue dict = {
      .kind = BencodeKindDict, .v.list.len = 2, .v.list.data = pair};
  test_bencode_encode_once(dict, "d3:keyl4:spami42eee");
}

// Breadth is not depth: a dict with more keys than `BENCODE_MAX_DEPTH` is
// ordinary bencode and must encode.
static void test_bencode_encode_wide_dict(void) {
  const usize pairs = BENCODE_MAX_DEPTH + 1;

  Arena arena = test_arena(1 * MiB);
  BencodeValue *entries = arena_alloc(&arena, __alignof__(BencodeValue),
                                      sizeof(BencodeValue), pairs * 2);
  assert(entries);
  u8 *keys = arena_alloc(&arena, __alignof__(u8), sizeof(u8), pairs * 4);
  assert(keys);

  for (usize i = 0; i < pairs; i++) {
    // `k000`, `k001`, ... : fixed width, so they are already sorted.
    u8 *const key = keys + i * 4;
    key[0] = 'k';
    key[1] = (u8)('0' + (i / 100) % 10);
    key[2] = (u8)('0' + (i / 10) % 10);
    key[3] = (u8)('0' + i % 10);

    entries[2 * i] =
        (BencodeValue){.kind = BencodeKindString, .v.s = slice_u8_make(key, 4)};
    entries[2 * i + 1] = test_bencode_int((isize)i);
  }

  const BencodeValue dict = {
      .kind = BencodeKindDict, .v.list.len = pairs * 2, .v.list.data = entries};

  const usize cap = bencode_encode_exact_size(dict, 0);
  Slice_u8 dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
                  .len = cap};
  assert(dst.data);

  const usize written = bencode_encode(dict, dst);

  assert(bencode_encode_exact_size(dict, 0) == written);
  assert(written > 2);
  assert('d' == dst.data[0]);
  assert('e' == dst.data[written - 1]);
  assert(0 == memcmp(dst.data + 1, "4:k000i0e", 9));

  // It is real bencode, with every pair still there.
  Arena parse_arena = test_arena(1 * MiB);
  Arena scratch = test_arena(1 * MiB);
  Slice_u8 to_parse = slice_u8_take(dst, written);
  BencodeValue parsed = {0};
  assert(bencode_parse(&to_parse, &parse_arena, scratch, &parsed));
  assert(BencodeKindDict == parsed.kind);
  assert(pairs * 2 == parsed.v.list.len);
}

// Encoding is the inverse of parsing: parse a document, encode it back, and the
// bytes must be identical. Bencode has exactly one representation per value, so
// any deviation is a bug in one of the two.
static void test_bencode_encode_round_trip(void) {
  const char *documents[] = {
      "i0e",
      "i-1e",
      "i9223372036854775807e",
      "i-9223372036854775808e",
      "0:",
      "4:spam",
      "le",
      "de",
      "l4:spam4:eggse",
      "li0ei1ei2ee",
      "d3:cow3:moo4:spam4:eggse",
      "d4:spaml1:a1:bee",
      "d9:publisher3:bob18:publisher.location4:homee",
      "lli1ei2eeli3ei4eee",
      "d1:ad1:bd1:cd1:d0:eeee",
  };

  for (usize i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
    Arena arena = test_arena(64 * KiB);
    Arena scratch = test_arena(64 * KiB);

    Slice_u8 input = test_slice(documents[i]);
    const Slice_u8 original = input;

    BencodeValue parsed = {0};
    assert(bencode_parse(&input, &arena, scratch, &parsed));

    test_bencode_encode_once(parsed, documents[i]);

    // And the encoding really is the whole input, not a prefix of it.
    assert(strlen(documents[i]) == original.len);
  }
}

// The v2 info dict, byte for byte against what libtorrent 2.1.1 produces for
// the same file. This pins the encoder, the dict construction and the merkle
// root together: any one of them drifting changes the infohash.
static void test_bencode_encode_torrent_info(void) {
  const usize file_len = 40960;

  Arena arena = test_arena(4 * MiB);
  u8 *const file_data =
      arena_alloc(&arena, __alignof__(u8), sizeof(u8), file_len);
  assert(file_data);
  memset(file_data, 'x', file_len);

  const Slice_u8 name = test_slice("f.bin");
  BencodeValue info = {0};
  assert(torrent_make_info_dict_v2(name, 16 * TORRENT_BLOCK_SIZE,
                                   slice_u8_make(file_data, file_len), name,
                                   &info, &arena));

  const usize cap = bencode_encode_exact_size(info, 0);
  Slice_u8 dst = {.data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), cap),
                  .len = cap};
  assert(dst.data);

  const Slice_u8 got = slice_u8_take(dst, bencode_encode(info, dst));
  assert(bencode_encode_exact_size(info, 0) == got.len);

  // The digest is raw bytes, so build the expectation around it rather than
  // embedding it in a string literal.
  const char *const prefix =
      "d9:file treed5:f.bind0:d6:lengthi40960e11:pieces root32:";
  const char *const suffix =
      "eee12:meta versioni2e4:name5:f.bin12:piece lengthi262144ee";

  u8 pieces_root[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(
      "8431e3abfbd82a618e0b0c4113dff17b644df6bd102fd127df5bd7a531014b3a",
      pieces_root);

  const usize prefix_len = strlen(prefix);
  const usize suffix_len = strlen(suffix);
  assert(prefix_len + SHA256_DIGEST_LENGTH + suffix_len == got.len);

  assert(0 == memcmp(got.data, prefix, prefix_len));
  assert(0 == memcmp(got.data + prefix_len, pieces_root, SHA256_DIGEST_LENGTH));
  assert(0 == memcmp(got.data + prefix_len + SHA256_DIGEST_LENGTH, suffix,
                     suffix_len));

  // The v2 infohash is the digest of exactly these bytes.
  u8 infohash[SHA256_DIGEST_LENGTH] = {0};
  sha256_digest(got, infohash);

  u8 expected_infohash[SHA256_DIGEST_LENGTH] = {0};
  test_digest_from_hex(
      "e556ed46dace469f8f24053de5ad85478349c13544a4710b6d624454b85e1256",
      expected_infohash);
  assert(0 == memcmp(infohash, expected_infohash, sizeof(infohash)));
}

// The vector block function must be indistinguishable from the scalar one, so
// compare them directly rather than only through the public digest: a
// disagreement on one block is otherwise easy to miss behind a passing vector.
static void test_sha256_neon_matches_scalar(void) {
#if !SHA256_HAS_NEON
  return;
#else
  if (!sha256_neon_supported()) {
    return;
  }

  // Blocks and chaining states that the extension is prone to getting wrong:
  // all zeroes, all ones, and the byte order boundaries.
  const u8 patterns[] = {0x00, 0xff, 0x80, 0x01, 0x7f};
  for (usize p = 0; p < sizeof(patterns); p++) {
    u8 block[SHA256_CBLOCK];
    memset(block, patterns[p], sizeof(block));

    u32 scalar[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u32 neon[8] = {0};
    memcpy(neon, scalar, sizeof(neon));

    sha256_compress(scalar, block);
    sha256_compress_blocks_neon(neon, block, 1);
    assert(0 == memcmp(scalar, neon, sizeof(scalar)));
  }

  // Then a deterministic sweep over both inputs. The chaining state is varied
  // too, not just the block: the two differ in how they carry state in and out.
  u32 x = 0x00c0ffee;
  for (usize iter = 0; iter < 20000; iter++) {
    u8 block[SHA256_CBLOCK];
    for (usize i = 0; i < sizeof(block); i++) {
      x = x * 1664525u + 1013904223u;
      block[i] = (u8)(x >> 24);
    }

    u32 scalar[8] = {0};
    u32 neon[8] = {0};
    for (usize i = 0; i < 8; i++) {
      x = x * 1664525u + 1013904223u;
      scalar[i] = x;
      neon[i] = x;
    }

    sha256_compress(scalar, block);
    sha256_compress_blocks_neon(neon, block, 1);
    assert(0 == memcmp(scalar, neon, sizeof(scalar)));
  }

  // Finally a run of consecutive blocks in one call. The vector version keeps
  // the chaining state in registers across the run, so this is the only thing
  // that exercises the hand off from one block to the next.
  {
    const usize blocks_count = 7;
    u8 blocks[7 * SHA256_CBLOCK];
    for (usize i = 0; i < sizeof(blocks); i++) {
      x = x * 1664525u + 1013904223u;
      blocks[i] = (u8)(x >> 24);
    }

    u32 scalar[8] = {0};
    u32 neon[8] = {0};
    for (usize i = 0; i < 8; i++) {
      x = x * 1664525u + 1013904223u;
      scalar[i] = x;
      neon[i] = x;
    }

    for (usize i = 0; i < blocks_count; i++) {
      sha256_compress(scalar, blocks + i * SHA256_CBLOCK);
    }
    sha256_compress_blocks_neon(neon, blocks, blocks_count);
    assert(0 == memcmp(scalar, neon, sizeof(scalar)));

    // A zero length run must leave the state alone.
    sha256_compress_blocks_neon(neon, blocks, 0);
    assert(0 == memcmp(scalar, neon, sizeof(scalar)));
  }
#endif
}

// Every message length that changes how blocks are cut up: empty, short, exact
// multiples, and one byte either side of each. Hashed through the dispatcher,
// which is what callers actually reach.
static void test_sha256_neon_lengths(void) {
#if !SHA256_HAS_NEON
  return;
#else
  if (!sha256_neon_supported()) {
    return;
  }

  const usize max_len = 4 * SHA256_CBLOCK + 8;

  Arena arena = test_arena(64 * KiB);
  u8 *const data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), max_len);
  assert(data);

  u32 x = 0x12345678;
  for (usize i = 0; i < max_len; i++) {
    x = x * 1664525u + 1013904223u;
    data[i] = (u8)(x >> 24);
  }

  for (usize len = 0; len <= max_len; len++) {
    // The dispatcher picks the vector path here.
    u8 dispatched[SHA256_DIGEST_LENGTH] = {0};
    sha256_digest(slice_u8_make(data, len), dispatched);

    // The same message, forced through the scalar block function.
    Sha256Ctx ctx = {0};
    sha256_init(&ctx);

    usize offset = 0;
    while (len - offset >= SHA256_CBLOCK) {
      sha256_compress(ctx.h, data + offset);
      offset += SHA256_CBLOCK;
    }
    ctx.len = len;
    ctx.partial_len = (u32)(len - offset);
    if (ctx.partial_len > 0) {
      memcpy(ctx.partial, data + offset, ctx.partial_len);
    }

    u8 scalar[SHA256_DIGEST_LENGTH] = {0};
    sha256_final(&ctx, scalar);

    assert(0 == memcmp(dispatched, scalar, sizeof(scalar)));
  }
#endif
}

static void test(const char *filter) {
  const struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
      {"char_is_digit_ascii", test_char_is_digit_ascii},
      {"isize_from_usize", test_isize_from_usize},
      {"usize_round_up_multiple_of", test_usize_round_up_multiple_of},
      {"next_power_of_two", test_next_power_of_two},
      {"arena_alloc", test_arena_alloc},
      {"arena_valloc", test_arena_valloc},
      {"slice_u8", test_slice_u8},
      {"ascii_num_parse", test_ascii_num_parse},
      {"bencode_parse_num", test_bencode_parse_num},
      {"bencode_parse_string", test_bencode_parse_string},
      {"bencode_parse", test_bencode_parse},
      {"bytes_cmp", test_bytes_cmp},
      {"bencode_validate_dict", test_bencode_validate_dict},
      {"sha256_vectors", test_sha256_vectors},
      {"sha256_million_a", test_sha256_million_a},
      {"sha256_incremental", test_sha256_incremental},
      {"sha256_lengths", test_sha256_lengths},
      {"sha256_reuse", test_sha256_reuse},
      {"sha256_neon_matches_scalar", test_sha256_neon_matches_scalar},
      {"sha256_neon_lengths", test_sha256_neon_lengths},
      {"torrent_merkle_vectors", test_torrent_merkle_vectors},
      {"torrent_merkle_piece_layer", test_torrent_merkle_piece_layer},
      {"torrent_merkle_padding", test_torrent_merkle_padding},
      {"torrent_merkle_empty", test_torrent_merkle_empty},
      {"torrent_merkle_oom", test_torrent_merkle_oom},
      {"usize_digits_base_10", test_usize_digits_base_10},
      {"encode_usize_base_10", test_encode_usize_base_10},
      {"encode_usize_base_10_exact_fit", test_encode_usize_base_10_exact_fit},
      {"encode_usize_base_10_round_trip", test_encode_usize_base_10_round_trip},
      {"encode_isize_base_10", test_encode_isize_base_10},
      {"encode_isize_base_10_exact_fit", test_encode_isize_base_10_exact_fit},
      {"encode_isize_base_10_round_trip", test_encode_isize_base_10_round_trip},
      {"bencode_encode_leaves", test_bencode_encode_leaves},
      {"bencode_encode_binary_string", test_bencode_encode_binary_string},
      {"bencode_encode_containers", test_bencode_encode_containers},
      {"bencode_encode_wide_dict", test_bencode_encode_wide_dict},
      {"bencode_encode_round_trip", test_bencode_encode_round_trip},
      {"bencode_encode_torrent_info", test_bencode_encode_torrent_info},
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

  printf("%zu test(s) passed\n", run);
}

int main(i32 argc, char *argv[]) {
  assert(argc >= 2);
  assert(argv);

  char *const cmd = argv[1];
  if (0 == strcmp(cmd, "test")) {
    test(argc > 2 ? argv[2] : NULL);
  } else if (0 == strcmp(cmd, "print-bencode")) {
    assert(3 == argc);

    const i32 fd = open(argv[2], O_RDONLY);
    assert(-1 != fd);

    struct stat st = {0};
    assert(-1 != fstat(fd, &st));
    assert(st.st_size > 0);

    void *const bencode_data =
        mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert((void *)-1 != bencode_data);

    Slice_u8 input = slice_u8_make((u8 *)bencode_data, (usize)st.st_size);
    Arena arena = arena_valloc(32 * MiB);
    Arena scratch = arena_valloc(32 * MiB);

    BencodeValue bencode = {0};
    assert(bencode_parse(&input, &arena, scratch, &bencode));

    bencode_print(bencode, 0);
    printf("\n");
  } else if (0 == strcmp(cmd, "gen-merkle-tree")) {
    assert(3 == argc);

    const i32 fd = open(argv[2], O_RDONLY);
    assert(-1 != fd);

    struct stat st = {0};
    assert(-1 != fstat(fd, &st));
    assert(st.st_size > 0);

    void *const input_data =
        mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert((void *)-1 != input_data);

    Slice_u8 input = slice_u8_make((u8 *)input_data, (usize)st.st_size);
    Slice_u8 file_name = {.data = (u8 *)argv[2], .len = strlen(argv[2])};
    Arena arena = arena_valloc(32 * MiB);

    Slice_u8 name = {.data = (u8 *)"test", .len = 4};
    BencodeValue info_dict = {0};
    assert(torrent_make_info_dict_v2(name, TORRENT_BLOCK_SIZE * 16, input,
                                     file_name, &info_dict, &arena));

    bencode_print(info_dict, 0);
    puts("");

    const usize encode_cap = bencode_encode_exact_size(info_dict, 0);
    assert(encode_cap > 0);

    Slice_u8 encoded = {
        .data = arena_alloc(&arena, __alignof__(u8), sizeof(u8), encode_cap),
        .len = encode_cap};
    assert(encoded.data);

    const usize encoded_len = bencode_encode(info_dict, encoded);
    encoded = slice_u8_take(encoded, encoded_len);
    printf("info dict encoded: %.*s\n", (i32)encoded.len, encoded.data);
  } else {
    fprintf(stderr, "unknown command\n");
    exit(1);
  }
}
