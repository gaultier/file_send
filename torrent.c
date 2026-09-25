#pragma once

#include "lib.c"
#include "sha2.c"

// ---------- Bencode ----------
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

// `i123e`
// `i-123e`
//
// `*input` is only advanced, and `*res` only written, when the parse
// succeeds.
__attribute__((warn_unused_result)) static Error
bencode_parse_num(Slice_u8 *input, BencodeValue *res) {
  assert(input);
  assert(input->data);
  assert(res);

  Slice_u8 remaining = *input;

  if (ErrKindNone != slice_u8_consume(&remaining, 'i').kind) {
    return (Error){.kind = ErrKindInvalidData};
  }

  const bool negative_sign =
      ErrKindNone == slice_u8_consume(&remaining, '-').kind;

  // Also rejects `ie` and `i-e`: a number needs at least one digit. A run of
  // digits too wide for a `usize` comes back as `ErrRange`, which is passed
  // through rather than flattened: the input is well formed, just too big.
  usize magnitude = 0;
  {
    const Error err = ascii_num_parse(&remaining, &magnitude);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  // `i-0e` is invalid bencode.
  if (negative_sign && 0 == magnitude) {
    return (Error){.kind = ErrKindInvalidData};
  }

  isize num = 0;
  {
    const Error err = isize_from_usize(magnitude, negative_sign, &num);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  if (ErrKindNone != slice_u8_consume(&remaining, 'e').kind) {
    return (Error){.kind = ErrKindInvalidData};
  }

  *input = remaining;
  *res = (BencodeValue){.kind = BencodeKindInteger, .v.num = num};
  return (Error){.kind = ErrKindNone};
}

// `4:spam`
//
// The string is not copied: it points into `*input`.
// `*input` is only advanced, and `*res` only written, when the parse
// succeeds.
__attribute__((warn_unused_result)) static Error
bencode_parse_string(Slice_u8 *input, BencodeValue *res) {
  assert(input);
  assert(input->data);
  assert(res);

  Slice_u8 remaining = *input;

  // Also rejects a leading `:` or any non-digit: a length needs a digit.
  usize len = 0;
  {
    const Error err = ascii_num_parse(&remaining, &len);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  if (ErrKindNone != slice_u8_consume(&remaining, ':').kind) {
    return (Error){.kind = ErrKindInvalidData};
  }

  // Truncated body.
  if (len > remaining.len) {
    return (Error){.kind = ErrKindInvalidData};
  }

  const Slice_u8 s = slice_u8_take(remaining, len);
  slice_u8_advance(&remaining, len);

  *input = remaining;
  *res = (BencodeValue){.kind = BencodeKindString, .v.s = s};

  assert(res->v.s.len == len);
  if (0 != res->v.s.len) {
    assert(res->v.s.data);
  }
  return (Error){.kind = ErrKindNone};
}

// Compare two byte strings lexicographically: the first differing byte
// decides, and when one is a prefix of the other, the shorter one sorts
// first. Returns <0, 0, or >0, like `memcmp`.
//
// Bytes are compared as unsigned values, so `0x80` sorts after `0x7f`. This
// is a total order over arbitrary bytes, embedded zeroes included, and it is
// the ordering bencode requires of dict keys.
__attribute__((warn_unused_result)) static i32
bytes_cmp(const u8 *a, usize a_len, const u8 *b, usize b_len) {
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

__attribute__((warn_unused_result)) static Error
bencode_validate_dict(BencodeList list) {
  if (0 != list.len) {
    assert(NULL != list.data);
  }

  // Mismatched key-value pairs?
  if (list.len % 2 != 0) {
    return (Error){.kind = ErrKindInvalidData};
  }

  for (usize i = 0; i < list.len; i += 2) {
    const BencodeValue key = list.data[i];

    if (key.kind != BencodeKindString) {
      return (Error){.kind = ErrKindInvalidData};
    }

    if (i > 1) {
      const BencodeValue previous = list.data[i - 2];

      if (bytes_cmp(previous.v.s.data, previous.v.s.len, key.v.s.data,
                    key.v.s.len) >= 0) {
        return (Error){.kind = ErrKindInvalidData};
      }
    }
  }

  return (Error){.kind = ErrKindNone};
}

#define BENCODE_MAX_DEPTH 128

// Parse one complete bencode value, with all of its children, into `*res`.
//
// `*input` and `*arena` are only advanced when the parse succeeds: rolling
// back a bump allocator is just restoring its start pointer, so a failed
// parse leaves the caller with neither consumed input nor consumed memory.
//
// `scratch` is taken by value and is not consumed by the call.
//
// Nothing calls this yet, which is why it is marked unused: `share` maps the
// `.torrent` it is handed and then ignores the bytes, so the one caller this is
// waiting for does not exist. It is kept, and tested, because that is the gap
// to close and not a reason to throw the parser away.
__attribute__((unused, warn_unused_result)) static Error
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
    return (Error){.kind = ErrKindInvalidData};
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
    return (Error){.kind = ErrKindOOM};
  }
  usize values_count = 0;

  BencodeContainer containers[BENCODE_MAX_DEPTH] = {0};
  usize containers_count = 0;

  const usize MAX_LEN = remaining.len;

  for (usize _i = 0; _i < MAX_LEN; _i++) {
    u8 current = 0;
    if (ErrKindNone != slice_u8_first(remaining, &current).kind) {
      return (Error){.kind = ErrKindInvalidData};
    }

    switch (current) {
    case 'i':
      // Parsed straight into its final slot: no intermediate copy.
      assert(values_count < values_cap);
      {
        const Error err = bencode_parse_num(&remaining, &values[values_count]);
        if (ErrKindNone != err.kind) {
          return err;
        }
      }
      values_count++;
      break;

    case 'l':
    case 'd':
      if (containers_count >= BENCODE_MAX_DEPTH) {
        return (Error){.kind = ErrKindInvalidData};
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
        return (Error){.kind = ErrKindInvalidData};
      }

      slice_u8_advance(&remaining, 1);

      // Time to pop `containers`.
      const BencodeContainer container = containers[containers_count - 1];
      containers[containers_count - 1] = (BencodeContainer){0};
      containers_count--;

      const usize children_len = values_count - container.children_start;

      // Should always be key-value pairs.
      if (!container.is_list && children_len % 2 != 0) {
        return (Error){.kind = ErrKindInvalidData};
      }

      // Now record the value for this container.
      BencodeValue value = {.kind = container.is_list ? BencodeKindList
                                                      : BencodeKindDict};
      // If there are any children, we need to allocate (right-sized) space
      // for them.
      if (children_len > 0) {
        BencodeValue *children =
            arena_alloc(&arena_local, __alignof__(BencodeValue),
                        sizeof(BencodeValue), children_len);
        // OOM?
        if (!children) {
          return (Error){.kind = ErrKindOOM};
        }

        // Copy the items from `values` to the new right-sized allocation.
        value.v.list.data = memcpy(children, values + container.children_start,
                                   children_len * sizeof(BencodeValue));
        value.v.list.len = children_len;

        if (BencodeKindDict == value.kind) {
          const Error err = bencode_validate_dict(value.v.list);
          if (ErrKindNone != err.kind) {
            return err;
          }
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
      {
        const Error err =
            bencode_parse_string(&remaining, &values[values_count]);
        if (ErrKindNone != err.kind) {
          return err;
        }
      }
      values_count++;
      break;

      // Unknown character.
    default:
      return (Error){.kind = ErrKindInvalidData};
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
      return (Error){.kind = ErrKindNone};
    }
  }

  return (Error){.kind = ErrKindInvalidData};
}

static void bencode_print_indent(usize indent) {
  for (usize i = 0; i < indent; i++) {
    printf(" ");
  }
}

// Print `v` in a JSON-ish form.
//
// The caller owns the cursor: it has already written whatever precedes the
// value on the current line (the leading indentation, or a `key: ` prefix),
// so this never indents the value itself. `indent` is the column the *line*
// the value starts on begins at, which is what the children and the closing
// bracket are aligned against. Nothing is written after the value either: a
// trailing newline is the caller's to add.
__attribute__((unused)) static void bencode_print(BencodeValue v,
                                                  usize indent) {
  switch (v.kind) {
  case BencodeKindInteger:
    printf("%zd", v.v.num);
    break;

  case BencodeKindString:
    // Bencode strings are arbitrary bytes, and `%s` would stop at the first
    // NUL however large a precision it is given, so the bytes go out through
    // `fwrite` instead.
    printf("\"");
    assert(v.v.s.len == fwrite(v.v.s.data, 1, v.v.s.len, stdout));
    printf("\"");
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
      // The value is indented against the start of the key's line, not
      // against the column it happens to start at, so a nested container
      // closes underneath its key.
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

static const usize TORRENT_BLOCK_SIZE = 16 * KiB;

// Arbitrary, only needs to be a byte pattern the tree cannot produce on
// its own. See the post condition in `torrent_build_merkle_tree`.
#define MERKLE_PIECE_POISON 0xAA

typedef struct {
  u8 digest[SHA256_DIGEST_LENGTH];
} PieceHash;

// Everything about a file's merkle tree that does not vary from node to node.
// Built once by `torrent_build_merkle_tree` and passed down by pointer: the
// recursion cannot then disagree with itself about where the piece layer
// sits, and the divisions and `ctz`s happen once rather than once per node.
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
  // Capacity `pieces_count`, filled in as the recursion crosses
  // `piece_depth`.
  PieceHash *const piece_hashes;
} MerkleTree;

__attribute__((warn_unused_result)) static MerkleTree
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
  } else {
    assert(data.len <= piece_length_in_bytes);
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

// Hash the subtree rooted at (`depth`, `tree_width_idx`) into `dst`,
// recording piece hashes into `tree->piece_hashes` on the way past
// `tree->piece_depth`. `tree_width_idx` is the index within its own level, so
// at `max_depth` it is the block index.
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
      memset(dst, 0, SHA256_DIGEST_LENGTH);
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
  // node case alone. An index at or past `pieces_count` is a subtree made
  // only of padding, which BEP 52 leaves out of the piece layer.
  if (tree->has_piece_layer && tree->piece_depth == depth &&
      tree_width_idx < tree->pieces_count) {
    memcpy(tree->piece_hashes[tree_width_idx].digest, dst,
           SHA256_DIGEST_LENGTH);
  }
}

// Build the merkle tree for one file, yielding its root (`pieces root` in the
// info dictionary) and its piece layer (`piece layers` at the torrent root).
// An empty file has neither, per BEP 52, and leaves `root` zeroed.
__attribute__((warn_unused_result)) static Error
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
  memset(root, 0, SHA256_DIGEST_LENGTH);

  if (0 == data.len) {
    return (Error){.kind = ErrKindNone};
  }
  assert(data.data);

  const usize pieces_count = ceil_usize(data.len, piece_length_in_bytes);
  assert(pieces_count > 0);

  PieceHash *const hashes = arena_alloc(arena, __alignof__(PieceHash),
                                        sizeof(PieceHash), pieces_count);
  // OOM?
  if (!hashes) {
    return (Error){.kind = ErrKindOOM};
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

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error torrent_make_metainfo_dict_v2(
    Slice_u8 pieces_root, Slice_u8 announce_url, BencodeList info_dict,
    const PieceHash *piece_hashes, usize piece_hashes_count, BencodeValue *dst,
    Arena *arena) {
  assert(!slice_u8_is_empty(announce_url));
  assert(info_dict.len > 0);
  assert(SHA256_DIGEST_LENGTH == pieces_root.len);
  assert(dst);
  assert(arena);

  // `piece layers` is always present, even with nothing in it: a file that
  // fits in a single piece has no layer, and the key is still emitted as an
  // empty dict.
  const usize kv_count = 3;

  dst->kind = BencodeKindDict;
  dst->v.list.len = 2 * kv_count;
  dst->v.list.data = arena_alloc(arena, __alignof__(BencodeValue),
                                 sizeof(BencodeValue), dst->v.list.len);
  if (!dst->v.list.data) {
    return (Error){.kind = ErrKindOOM};
  }

  // `metainfo["announce"] = announce_url`
  {
    BencodeValue *const announce_key = &dst->v.list.data[0];
    announce_key->kind = BencodeKindString;
    announce_key->v.s = slice_u8_make((u8 *)"announce", sizeof("announce") - 1);

    BencodeValue *const announce_value = &dst->v.list.data[1];
    announce_value->kind = BencodeKindString;
    announce_value->v.s = announce_url;
  }

  // `metainfo["info"] = info_dict`
  {
    BencodeValue *const info_key = &dst->v.list.data[2];
    info_key->kind = BencodeKindString;
    info_key->v.s = slice_u8_make((u8 *)"info", sizeof("info") - 1);

    dst->v.list.data[3].kind = BencodeKindDict;
    dst->v.list.data[3].v.list = info_dict;
  }

  // `metainfo["piece layers"] = {}`
  {
    assert(dst->v.list.len == 2 * kv_count);

    BencodeValue *const pieces_key = &dst->v.list.data[4];
    pieces_key->kind = BencodeKindString;
    pieces_key->v.s =
        slice_u8_make((u8 *)"piece layers", sizeof("piece layers") - 1);

    BencodeValue *const pieces_value = &dst->v.list.data[5];
    pieces_value->kind = BencodeKindDict;

    // A file that fits in a single piece gets no entry at all, per BEP 52,
    // which leaves the dict empty.
    if (0 == piece_hashes_count) {
      pieces_value->v.list = (BencodeList){0};
      return (Error){.kind = ErrKindNone};
    }

    assert(piece_hashes);
    pieces_value->v.list.len = 2;
    pieces_value->v.list.data =
        arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                    pieces_value->v.list.len);
    if (!pieces_value->v.list.data) {
      return (Error){.kind = ErrKindOOM};
    }

    // `metainfo["piece layers"][pieces_root] = piece_hashes`
    //
    // Keyed by the merkle root, not by the file name: that is what ties a
    // layer back to its file across the whole torrent, and it is what every
    // other client looks up.
    {
      BencodeValue *const layer_key = &pieces_value->v.list.data[0];
      layer_key->kind = BencodeKindString;
      layer_key->v.s = pieces_root;

      BencodeValue *const layer_value = &pieces_value->v.list.data[1];
      layer_value->kind = BencodeKindString;
      layer_value->v.s.len = piece_hashes_count * SHA256_DIGEST_LENGTH;
      layer_value->v.s.data =
          arena_alloc(arena, __alignof__(u8), sizeof(u8), layer_value->v.s.len);
      if (!layer_value->v.s.data) {
        return (Error){.kind = ErrKindOOM};
      }

      memcpy(layer_value->v.s.data, piece_hashes, layer_value->v.s.len);
    }
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error torrent_make_info_dict_v2(
    Slice_u8 name, usize piece_length_in_bytes, Slice_u8 file_data,
    Slice_u8 file_name, BencodeValue *dst_info_dict, Slice_u8 *dst_pieces_root,
    PieceHash **dst_piece_hashes, usize *dst_piece_hashes_count, Arena *arena) {
  assert(piece_length_in_bytes >= 16 * KiB);      // Per spec.
  assert(is_power_of_two(piece_length_in_bytes)); // Per spec.
  assert(dst_info_dict);
  assert(dst_pieces_root);
  assert(dst_piece_hashes);
  assert(dst_piece_hashes_count);
  assert(arena);

  *dst_pieces_root = (Slice_u8){0};
  *dst_piece_hashes = NULL;
  *dst_piece_hashes_count = 0;

  const usize dict_items_count = 4;
  *dst_info_dict = (BencodeValue){
      .kind = BencodeKindDict,
      .v.list.len = dict_items_count * 2,
      .v.list.data = arena_alloc(arena, __alignof__(BencodeValue),
                                 sizeof(BencodeValue), dict_items_count * 2),
  };
  if (NULL == dst_info_dict->v.list.data) {
    return (Error){.kind = ErrKindOOM};
  }

  // `info["name"] = name`
  {
    BencodeValue *const key = &dst_info_dict->v.list.data[4];
    key->kind = BencodeKindString;
    key->v.s = slice_u8_make((u8 *)"name", sizeof("name") - 1);

    BencodeValue *const value = &dst_info_dict->v.list.data[5];
    value->kind = BencodeKindString;
    value->v.s = name;
  }

  // `info["piece length"] = piece_length_in_bytes`
  {
    BencodeValue *const key = &dst_info_dict->v.list.data[6];
    key->kind = BencodeKindString;
    key->v.s = slice_u8_make((u8 *)"piece length", sizeof("piece length") - 1);

    BencodeValue *const value = &dst_info_dict->v.list.data[7];
    value->kind = BencodeKindInteger;
    const Error err =
        isize_from_usize(piece_length_in_bytes, false, &value->v.num);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  // `info["meta version"] = 2`
  {

    BencodeValue *const key = &dst_info_dict->v.list.data[2];
    key->kind = BencodeKindString;
    key->v.s = slice_u8_make((u8 *)"meta version", sizeof("meta version") - 1);

    BencodeValue *const value = &dst_info_dict->v.list.data[3];
    value->kind = BencodeKindInteger;
    value->v.num = 2;
  }

  // `info["file tree"] = ...`
  {
    BencodeValue *const file_tree_key = &dst_info_dict->v.list.data[0];
    file_tree_key->kind = BencodeKindString;
    file_tree_key->v.s =
        slice_u8_make((u8 *)"file tree", sizeof("file tree") - 1);

    u8 root[SHA256_DIGEST_LENGTH] = {0};
    const Error err = torrent_build_merkle_tree(
        file_data, piece_length_in_bytes, dst_piece_hashes,
        dst_piece_hashes_count, root, arena);
    if (ErrKindNone != err.kind) {
      return err;
    }
    // If the file data does not fit within one piece, then the piece layer is
    // required (per spec).
    if (file_data.len > piece_length_in_bytes) {
      assert(*dst_piece_hashes);
      assert(*dst_piece_hashes_count > 0);
    }

    BencodeValue *const file_tree_dict = &dst_info_dict->v.list.data[1];
    file_tree_dict->kind = BencodeKindDict;
    file_tree_dict->v.list.len = 2;
    file_tree_dict->v.list.data =
        arena_alloc(arena, __alignof__(BencodeValue), sizeof(BencodeValue),
                    file_tree_dict->v.list.len);
    if (NULL == file_tree_dict->v.list.data) {
      return (Error){.kind = ErrKindOOM};
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
        return (Error){.kind = ErrKindOOM};
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
          return (Error){.kind = ErrKindOOM};
        }

        // `info["file tree"][file_name][""]["length"] = file_data.length`
        {
          BencodeValue *const length_key = &empty_dict->v.list.data[0];
          length_key->kind = BencodeKindString;
          length_key->v.s = slice_u8_make((u8 *)"length", sizeof("length") - 1);

          BencodeValue *const length_value = &empty_dict->v.list.data[1];
          length_value->kind = BencodeKindInteger;
          const Error length_err =
              isize_from_usize(file_data.len, false, &length_value->v.num);
          if (ErrKindNone != length_err.kind) {
            return length_err;
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
            return (Error){.kind = ErrKindOOM};
          }
          memcpy(pieces_root_value->v.s.data, root, SHA256_DIGEST_LENGTH);

          // `piece layers` is keyed by this exact digest, so hand it back
          // rather than making the caller dig it out of the tree.
          *dst_pieces_root = pieces_root_value->v.s;
        }
      }
    }
  }

  return (Error){.kind = ErrKindNone};
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

// Sizing is a whole subtree walk, so the checks against it live in the
// wrapper below rather than here, where they would run once per level of
// nesting.
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
__attribute__((warn_unused_result)) static usize
bencode_encode_in_place(BencodeValue b, Slice_u8 dst) {
  assert(dst.data);

  const usize written = bencode_encode_rec(b, dst, 0);
  assert(dst.len == written);

  return written;
}

__attribute__((warn_unused_result)) static Error
bencode_encode(BencodeValue b, Slice_u8 *dst, Arena *arena) {
  assert(dst);
  assert(arena);

  const usize size = bencode_encode_exact_size(b, 0);

  *dst =
      (Slice_u8){.data = arena_alloc(arena, __alignof__(u8), sizeof(u8), size),
                 .len = size};
  if (!dst->data) {
    return (Error){.kind = ErrKindOOM};
  }

  const usize written = bencode_encode_in_place(b, *dst);
  assert(written == size);
  assert(written == dst->len);

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error
torrent_gen_torrent_file_data(Slice_u8 file_path, Slice_u8 file_data,
                              Slice_u8 announce_url, Slice_u8 *dst_torrent,
                              u8 dst_info_hash[SHA256_DIGEST_LENGTH],
                              Arena scratch, Arena *arena) {
  // TODO: Consider passing a scratch arena for some allocations.

  assert(dst_torrent);
  assert(dst_info_hash);
  assert(arena);

  Error err = {0};

  const Slice_u8 file_name =
      path_last_component(file_path, PATH_SEPARATOR_UNIX);

  BencodeValue info_dict = {0};
  PieceHash *piece_hashes = NULL;
  usize piece_hashes_count = 0;
  Slice_u8 pieces_root = {0};
  err = torrent_make_info_dict_v2(file_name, TORRENT_BLOCK_SIZE * 16, file_data,
                                  file_name, &info_dict, &pieces_root,
                                  &piece_hashes, &piece_hashes_count, &scratch);

  if (ErrKindNone != err.kind) {
    return err;
  }

  Slice_u8 info_dict_encoded = {0};
  err = bencode_encode(info_dict, &info_dict_encoded, &scratch);
  if (ErrKindNone != err.kind) {
    return err;
  }

  sha256_digest(info_dict_encoded, dst_info_hash);

  BencodeValue metainfo_dict = {0};
  err = torrent_make_metainfo_dict_v2(
      pieces_root, announce_url, info_dict.v.list, piece_hashes,
      piece_hashes_count, &metainfo_dict, &scratch);
  if (ErrKindNone != err.kind) {
    return err;
  }

  Slice_u8 metainfo_dict_encoded = {0};
  err = bencode_encode(metainfo_dict, &metainfo_dict_encoded, arena);
  if (ErrKindNone != err.kind) {
    return err;
  }

  *dst_torrent = metainfo_dict_encoded;

  return (Error){.kind = ErrKindNone};
}

typedef struct TorrentNetworkCtx TorrentNetworkCtx;

typedef struct {
  // The caller's, passed through `io_listen_and_serve_tcp_ipv4`. The vtable's
  // own context lives in `io->ctx`.
  void *cb_ctx;
  const IO *io;
  TorrentNetworkCtx *network_ctx;
  i32 socket;
  Ipv4Addr addr;
  // More: torrent, etc.
} TorrentClientHandleCtx;

#define TORRENT_CLIENTS_MAX 1024

typedef u64 PoolSlotGroup;

// Unit is bits.
#define POOL_SLOTS_PER_GROUP (sizeof(PoolSlotGroup) * 8)
_Static_assert(0 == (TORRENT_CLIENTS_MAX % POOL_SLOTS_PER_GROUP),
               "must be a multiple");

#define POOL_SLOT_GROUPS (TORRENT_CLIENTS_MAX / POOL_SLOTS_PER_GROUP)

typedef struct {
  // Bitset.
  // Bit `i` of group `g` means: `slots[g * POOL_SLOTS_PER_GROUP + i]` is
  // occupied.
  PoolSlotGroup occupied[POOL_SLOT_GROUPS];
  TorrentClientHandleCtx slots[TORRENT_CLIENTS_MAX];
} TorrentClientHandleCtxPool;

struct TorrentNetworkCtx {
  TorrentClientHandleCtxPool pool;
  // More...
};

__attribute__((warn_unused_result)) static TorrentClientHandleCtx *
torrent_client_ctx_pool_acquire(TorrentClientHandleCtxPool *pool) {
  assert(pool);

  for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
    // 'Relaxed' means that we sometimes can report the slot group as full when
    // it's not. It's a TOCTOU window we accept and there is no easy fix.
    const PoolSlotGroup slot_group =
        __atomic_load_n(&pool->occupied[i], __ATOMIC_RELAXED);

    const i32 first_unset_bit = __builtin_ffsll((i64)~slot_group);

    // Slot full, keep scanning to find a free slot?
    if (0 == first_unset_bit) {
      continue;
    }

    const u32 bit_idx = (u32)(first_unset_bit - 1);
    const PoolSlotGroup mask = 1ULL << bit_idx;

    // Mark the slot as occupied.
    const PoolSlotGroup prev =
        __atomic_fetch_or(&pool->occupied[i], mask, __ATOMIC_ACQUIRE);

    // Since there is only one concurrent caller of 'pool_acquire' no
    // one could have concurrently acquired the slot that was free at the start
    // of this loop iteration.
    assert(0 == (prev & mask));

    const usize slot_idx = i * POOL_SLOTS_PER_GROUP + bit_idx;
    assert(slot_idx < TORRENT_CLIENTS_MAX);

    TorrentClientHandleCtx *res = &pool->slots[slot_idx];
    assert(0 == res->cb_ctx);
    assert(0 == res->io);
    assert(0 == res->network_ctx);
    assert(0 == res->socket);
    assert(0 == res->addr.ip);
    assert(0 == res->addr.port);
    return res;
  }

  return NULL;
}

static void torrent_client_ctx_pool_release(TorrentClientHandleCtxPool *pool,
                                            TorrentClientHandleCtx *slot) {
  assert(pool);
  assert(slot);

  assert(slot >= pool->slots);
  const usize slot_idx = (usize)(slot - pool->slots);
  assert(slot_idx < TORRENT_CLIENTS_MAX);

  const usize slot_group_idx = slot_idx / POOL_SLOTS_PER_GROUP;
  assert(slot_group_idx < POOL_SLOT_GROUPS);
  const u32 bit_idx = slot_idx % POOL_SLOTS_PER_GROUP;

  // We are still the owner so we are responsible for zeroing it.
  memset(slot, 0, sizeof(*slot));

  const PoolSlotGroup mask = ~(1ULL << bit_idx);
  const PoolSlotGroup prev = __atomic_fetch_and(&pool->occupied[slot_group_idx],
                                                mask, __ATOMIC_RELEASE);

  // Sanity check against double release of the same slot: the slot was indeed
  // occupied before.
  assert(0 != (prev & ~mask));
}

static void *torrent_client_handle(void *vctx) {
  assert(vctx);

  TorrentClientHandleCtx *const client_ctx = vctx;
  assert(client_ctx->io);

  const u32 ip = client_ctx->addr.ip;
  printf("accepted: %u.%u.%u.%u:%hu\n", ip >> 24 & 0xff, ip >> 16 & 0xff,
         ip >> 8 & 0xff, ip >> 0 & 0xff, client_ctx->addr.port);

  usize read_count = 0;
  u8 buf[4096] = {0};
  Slice_u8 slice_read = slice_u8_make(buf, sizeof(buf));

  Error err = client_ctx->io->read(client_ctx->io, client_ctx->socket,
                                   slice_read, &read_count);
  if (ErrKindNone != err.kind) {
    goto end;
  }

  const Slice_u8 slice_read_actual = slice_u8_take(slice_read, read_count);
  printf("read: %.*s\n", (i32)slice_read_actual.len, slice_read_actual.data);

end:
  (void)client_ctx->io->close(client_ctx->io, client_ctx->socket);

  puts("torrent_client_handle end");

  // Responsible for freeing our context.
  torrent_client_ctx_pool_release(&client_ctx->network_ctx->pool, client_ctx);

  return NULL;
}

__attribute__((warn_unused_result)) static Error
torrent_client_on_accept(const IO *io, void *vctx, Ipv4Addr accept_addr,
                         i32 accept_socket) {
  assert(io);
  assert(vctx);
  TorrentNetworkCtx *const network_ctx = vctx;

  puts("accepted");

  Error err = {.kind = ErrKindNone};
  TorrentClientHandleCtx *const client_ctx =
      torrent_client_ctx_pool_acquire(&network_ctx->pool);
  if (!client_ctx) {
    fprintf(stderr, "backpressure: no available pool slot for client\n");
    (void)io->close(io, accept_socket);
    return (Error){.kind = ErrKindOOM};
  }

  assert(client_ctx);
  client_ctx->cb_ctx = vctx;
  client_ctx->addr = accept_addr;
  client_ctx->socket = accept_socket;
  client_ctx->io = io;
  client_ctx->network_ctx = network_ctx;

  err = io->thread_create(io, torrent_client_handle, client_ctx);
  if (ErrKindNone != err.kind) {
    // The thread never started, so nothing else will free the context or hang
    // up on the peer.
    torrent_client_ctx_pool_release(&network_ctx->pool, client_ctx);
    (void)io->close(io, accept_socket);
  }

  // Nothing to cleanup: the client handler finished successfully and is
  // responsible for the cleanup.

  return err;
}

__attribute__((warn_unused_result)) static Error
torrent_make_udp_broadcast_message(Slice_u8 url, u16 port, Slice_u8 info_hash,
                                   Arena *arena, Slice_u8 *dst) {
  assert(arena);
  assert(dst);

  StringBuffer sb = {0};
  Error err = sb_make(100, arena, &sb);
  if (ErrKindNone == err.kind) {
    return err;
  }

  assert(sb_extend_within_cap(&sb, slice_u8_from_cstr("BT-SEARCH * HTTP/1.1\r\n"
                                                      "Host: ")));
  assert(sb_extend_within_cap(&sb, url));
  assert(sb_extend_within_cap(&sb, slice_u8_from_cstr("\r\n"
                                                      "Port: ")));

  assert(sb_append_usize_within_cap(&sb, port));
  assert(sb_extend_within_cap(&sb, slice_u8_from_cstr("\r\n"
                                                      "Infohash: ")));

  assert(sb_extend_within_cap(&sb, info_hash));

  assert(sb_extend_within_cap(&sb, slice_u8_from_cstr("\r\n"
                                                      "\r\n"
                                                      "\r\n")));

  *dst = slice_u8_take(sb.container, sb.len);

  return (Error){.kind = ErrKindNone};
}
