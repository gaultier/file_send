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
#include <sys/wait.h>
#include <unistd.h>

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
} MerkleNode;

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

// `multiple` must be a power of two, which every page size is.
__attribute((warn_unused_result)) static usize
usize_round_up_multiple_of(usize n, usize multiple) {
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

  assert(0 != val && 0 == (val & (val - 1)) && "not a power of two");

  return val;
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
  assert(data || 0 == len);

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
  assert(res->v.s.data || 0 == res->v.s.len);
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
  assert(a || 0 == a_len);
  assert(b || 0 == b_len);

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
  assert(NULL != list.data || 0 == list.len);

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
  assert(input->data || 0 == input->len);
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
  usize values_len = 0;

  BencodeContainer containers[BENCODE_MAX_DEPTH] = {0};
  usize containers_len = 0;

  const usize MAX_LEN = remaining.len;

  for (usize _i = 0; _i < MAX_LEN; _i++) {
    u8 current = 0;
    if (!slice_u8_first(remaining, &current)) {
      return false;
    }

    switch (current) {
    case 'i':
      // Parsed straight into its final slot: no intermediate copy.
      assert(values_len < values_cap);
      if (!bencode_parse_num(&remaining, &values[values_len])) {
        return false;
      }
      values_len++;
      break;

    case 'l':
    case 'd':
      if (containers_len >= BENCODE_MAX_DEPTH) {
        return false;
      }
      slice_u8_advance(&remaining, 1);

      containers[containers_len].is_list = current == 'l';
      containers[containers_len].children_start = values_len;
      containers_len++;

      // The next loop iteration will parse the items.
      continue;

    case 'e': {
      if (0 == containers_len) {
        // Stray `e`, reject.
        return false;
      }

      slice_u8_advance(&remaining, 1);

      // Time to pop `containers`.
      const BencodeContainer container = containers[containers_len - 1];
      containers[containers_len - 1] = (BencodeContainer){0};
      containers_len--;

      const usize children_len = values_len - container.children_start;

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
    case '9':
      // Parsed straight into its final slot: no intermediate copy.
      assert(values_len < values_cap);
      if (!bencode_parse_string(&remaining, &values[values_len])) {
        return false;
      }
      values_len++;
      break;

      // Unknown character.
    default:
      return false;
    }

    // We just finished to correctly parse a value.
    assert(values_len <= values_cap);

    // No containers meaning: nothing is currently open.
    // So, we are at the root, which we need to return to the caller,
    // because `root != values[0]` in the general case.
    if (0 == containers_len) {
      assert(1 == values_len);

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
static void sha256_init(Sha256Ctx *ctx) {
  assert(ctx);

  *ctx = (Sha256Ctx){
      .h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
            0x9b05688c, 0x1f83d9ab, 0x5be0cd19},
  };
}

static void sha256_update(Sha256Ctx *ctx, Slice_u8 data) {
  assert(ctx);
  assert(data.data || 0 == data.len);
  assert(ctx->partial_len < SHA256_CBLOCK);

  const u8 *remaining = data.data;
  usize len = data.len;
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

    sha256_compress(ctx->h, ctx->partial);
    ctx->partial_len = 0;
  }

  while (len >= SHA256_CBLOCK) {
    sha256_compress(ctx->h, remaining);
    remaining += SHA256_CBLOCK;
    len -= SHA256_CBLOCK;
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
  sha256_update(ctx, slice_u8_make(padding, padding_len));

  u8 len_bytes[8] = {0};
  for (usize i = 0; i < 8; i++) {
    len_bytes[i] = (u8)(len_bits >> (56 - 8 * i));
  }
  sha256_update(ctx, slice_u8_make(len_bytes, sizeof(len_bytes)));
  assert(0 == ctx->partial_len);

  for (usize i = 0; i < 8; i++) {
    u32_to_bytes_be(ctx->h[i], res + i * 4);
  }

  *ctx = (Sha256Ctx){0};
}

static void sha_digest(Slice_u8 data, u8 res[SHA256_DIGEST_LENGTH]) {
  Sha256Ctx sha = {0};
  sha256_init(&sha);
  sha256_update(&sha, data);
  sha256_final(&sha, res);
}

static void sha256_print_hex(u8 digest[SHA256_DIGEST_LENGTH]) {
  const u8 lut[] = "0123456789abcdef";

  for (usize i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    const u8 byte = digest[i];
    const u8 c1 = lut[byte & 16];
    const u8 c2 = lut[byte >> 4];
    printf("%c%c", c2, c1);
  }
}

static const usize TORRENT_BLOCK_SIZE = 16 * KiB;
// static const usize TORRENT_PIECES_PER_BLOCK = 16;

__attribute((warn_unused_result)) static bool
torrent_compute_merkle_tree(Slice_u8 data, MerkleNode **nodes,
                            usize *nodes_count, Arena *arena) {
  assert(nodes);
  assert(arena);
  assert(arena->start);

  *nodes_count = next_power_of_two(data.len);
  *nodes = arena_alloc(arena, __alignof__(MerkleNode), sizeof(MerkleNode),
                       *nodes_count);
  if (!*nodes) {
    return false;
  }

  usize i = 0;
  assert(*nodes);
  for (i = 0; i < data.len / TORRENT_BLOCK_SIZE; i++) {
    const Slice_u8 block_data = {.data = &data.data[i * TORRENT_BLOCK_SIZE],
                                 .len = TORRENT_BLOCK_SIZE};

    assert(i < *nodes_count);
    MerkleNode *const node = &((*nodes)[i]);

    sha_digest(block_data, node->digest);

    sha256_print_hex(node->digest);
    puts("");
  }

  // Last block.
  assert(i * TORRENT_BLOCK_SIZE <= data.len);
  if (i * TORRENT_BLOCK_SIZE < data.len) {
    const Slice_u8 block_data = {.data = &data.data[i * TORRENT_BLOCK_SIZE],
                                 .len = data.len - i * TORRENT_BLOCK_SIZE};

    assert(i < *nodes_count);
    MerkleNode *const node = &((*nodes)[i]);

    sha_digest(block_data, node->digest);

    sha256_print_hex(node->digest);
    puts("\n");
  }

  // TODO: Fill remaining nodes with 0 or sha256(0).

  // TODO: build the binary tree.

  return true;
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
    assert(1 == res || res / 2 < n);
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
  assert(data || 0 == len);

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
      // Trailing data is left for the caller, exactly like the scalar parsers.
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
      assert(value.v.list.data || 0 == value.v.list.len);
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
  sha256_update(&ctx, data);
  sha256_final(&ctx, res);
}

// Expected digests are given as hex, the way every SHA-256 test vector in the
// wild is published, so that a vector can be pasted in unmodified.
static void test_sha256_expect_hex(Slice_u8 data, const char *expected_hex) {
  assert(expected_hex);
  assert(2 * SHA256_DIGEST_LENGTH == strlen(expected_hex));

  u8 expected[SHA256_DIGEST_LENGTH] = {0};
  for (usize i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    u32 byte = 0;
    assert(1 == sscanf(expected_hex + 2 * i, "%2x", &byte));
    expected[i] = (u8)byte;
  }

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
    sha256_update(&ctx, slice_u8_make(chunk, sizeof(chunk)));
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
      sha256_update(&ctx, slice_u8_make(input + i, 1));
    }

    u8 actual[SHA256_DIGEST_LENGTH] = {0};
    sha256_final(&ctx, actual);
    assert(0 == memcmp(actual, expected, sizeof(actual)));
  }

  for (usize split = 0; split <= sizeof(input); split++) {
    Sha256Ctx ctx = {0};
    sha256_init(&ctx);
    sha256_update(&ctx, slice_u8_make(input, split));
    // An empty `Update` in the middle must be a no-op, including when the
    // slice has no data pointer at all.
    sha256_update(&ctx, (Slice_u8){0});
    sha256_update(&ctx, slice_u8_make(input + split, sizeof(input) - split));

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
    sha256_update(&outer, slice_u8_make(digest, sizeof(digest)));
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
  sha256_update(&ctx, test_slice("some other message entirely"));

  u8 discarded[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&ctx, discarded);

  sha256_init(&ctx);
  sha256_update(&ctx, test_slice("abc"));

  u8 actual[SHA256_DIGEST_LENGTH] = {0};
  sha256_final(&ctx, actual);
  assert(0 == memcmp(actual, expected, sizeof(actual)));
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
    Arena arena = arena_valloc(32 * MiB);

    MerkleNode *nodes = NULL;
    usize nodes_count = 0;
    assert(torrent_compute_merkle_tree(input, &nodes, &nodes_count, &arena));

  } else {
    fprintf(stderr, "unknown command\n");
    exit(1);
  }
}
