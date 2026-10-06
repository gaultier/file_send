#pragma once

#include "lib.c"
#include "sha2.c"

// ---------- Bencode ----------
typedef enum {
  BencodeKindInteger,
  BencodeKindBytes,
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
    Bytes bytes;      // Bytes
    BencodeList list; // List or Dict (stored as contiguous key-value pairs)
  } v;
};

// `i123e`
// `i-123e`
//
// `*input` is only advanced, and `*res` only written, when the parse
// succeeds.
__attribute__((warn_unused_result)) static Error
bencode_parse_num(Bytes *input, BencodeValue *res) {
  assert(input);
  assert(input->data);
  assert(res);

  Bytes remaining = *input;

  if (ErrKindNone != bytes_expect_u8(&remaining, 'i').kind) {
    return (Error){.kind = ErrKindInvalidData};
  }

  const bool negative_sign =
      ErrKindNone == bytes_expect_u8(&remaining, '-').kind;

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

  if (ErrKindNone != bytes_expect_u8(&remaining, 'e').kind) {
    return (Error){.kind = ErrKindInvalidData};
  }

  *input = remaining;
  *res = (BencodeValue){.kind = BencodeKindInteger, .v.num = num};
  return (Error){.kind = ErrKindNone};
}

// `4:spam`
//
// The bytes are not copied: it points into `*input`.
// `*input` is only advanced, and `*res` only written, when the parse
// succeeds.
__attribute__((warn_unused_result)) static Error
bencode_parse_bytes(Bytes *input, BencodeValue *res) {
  assert(input);
  assert(input->data);
  assert(res);

  Bytes remaining = *input;

  // Also rejects a leading `:` or any non-digit: a length needs a digit.
  usize len = 0;
  {
    const Error err = ascii_num_parse(&remaining, &len);
    if (ErrKindNone != err.kind) {
      return err;
    }
  }

  if (ErrKindNone != bytes_expect_u8(&remaining, ':').kind) {
    return (Error){.kind = ErrKindInvalidData};
  }

  // Truncated body.
  if (len > remaining.len) {
    return (Error){.kind = ErrKindInvalidData};
  }

  const Bytes s = bytes_take(remaining, len);
  bytes_advance(&remaining, len);

  *input = remaining;
  *res = (BencodeValue){.kind = BencodeKindBytes, .v.bytes = s};

  assert(res->v.bytes.len == len);
  if (0 != res->v.bytes.len) {
    assert(res->v.bytes.data);
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
__attribute__((warn_unused_result)) static i32 bytes_cmp(Bytes a, Bytes b) {
  if (0 != a.len) {
    assert(a.data);
  }
  if (0 != b.len) {
    assert(b.data);
  }

  // Not `memcmp`: it is undefined to hand it a NULL pointer even for a length
  // of zero, and an empty byte string is legal here.
  const usize len = a.len < b.len ? a.len : b.len;
  for (usize i = 0; i < len; i++) {
    if (a.data[i] != b.data[i]) {
      return a.data[i] < b.data[i] ? -1 : 1;
    }
  }

  // Equal up to the shorter length: the prefix sorts first.
  if (a.len == b.len) {
    return 0;
  }
  return a.len < b.len ? -1 : 1;
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

    if (key.kind != BencodeKindBytes) {
      return (Error){.kind = ErrKindInvalidData};
    }

    if (i > 1) {
      const BencodeValue previous = list.data[i - 2];
      assert(BencodeKindBytes == previous.kind);

      if (bytes_cmp(previous.v.bytes, key.v.bytes) >= 0) {
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
bencode_parse(Bytes *input, Arena *arena, Arena scratch, BencodeValue *res) {
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

  Bytes remaining = *input;
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
    if (ErrKindNone != bytes_first(remaining, &current).kind) {
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
      bytes_advance(&remaining, 1);

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

      bytes_advance(&remaining, 1);

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
            bencode_parse_bytes(&remaining, &values[values_count]);
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

  case BencodeKindBytes:
    // Bencode byte strings are arbitrary bytes, and `%s` would stop at the
    // first NUL however large a precision it is given, so the bytes go out
    // through `fwrite` instead.
    printf("\"");
    assert(v.v.bytes.len == fwrite(v.v.bytes.data, 1, v.v.bytes.len, stdout));
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
  const Bytes data;
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
torrent_merkle_tree_make(Bytes data, usize piece_length_in_bytes,
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
      const Bytes block_data = {
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
torrent_build_merkle_tree(Bytes data, usize piece_length_in_bytes,
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
    Bytes pieces_root, Bytes announce_url, BencodeList info_dict,
    const PieceHash *piece_hashes, usize piece_hashes_count, BencodeValue *dst,
    Arena *arena) {
  assert(!bytes_is_empty(announce_url));
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
    announce_key->kind = BencodeKindBytes;
    announce_key->v.bytes =
        bytes_make((u8 *)"announce", sizeof("announce") - 1);

    BencodeValue *const announce_value = &dst->v.list.data[1];
    announce_value->kind = BencodeKindBytes;
    announce_value->v.bytes = announce_url;
  }

  // `metainfo["info"] = info_dict`
  {
    BencodeValue *const info_key = &dst->v.list.data[2];
    info_key->kind = BencodeKindBytes;
    info_key->v.bytes = bytes_make((u8 *)"info", sizeof("info") - 1);

    dst->v.list.data[3].kind = BencodeKindDict;
    dst->v.list.data[3].v.list = info_dict;
  }

  // `metainfo["piece layers"] = {}`
  {
    assert(dst->v.list.len == 2 * kv_count);

    BencodeValue *const pieces_key = &dst->v.list.data[4];
    pieces_key->kind = BencodeKindBytes;
    pieces_key->v.bytes =
        bytes_make((u8 *)"piece layers", sizeof("piece layers") - 1);

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
    // other peer looks up.
    {
      BencodeValue *const layer_key = &pieces_value->v.list.data[0];
      layer_key->kind = BencodeKindBytes;
      layer_key->v.bytes = pieces_root;

      BencodeValue *const layer_value = &pieces_value->v.list.data[1];
      layer_value->kind = BencodeKindBytes;
      layer_value->v.bytes.len = piece_hashes_count * SHA256_DIGEST_LENGTH;
      layer_value->v.bytes.data = arena_alloc(
          arena, __alignof__(u8), sizeof(u8), layer_value->v.bytes.len);
      if (!layer_value->v.bytes.data) {
        return (Error){.kind = ErrKindOOM};
      }

      memcpy(layer_value->v.bytes.data, piece_hashes, layer_value->v.bytes.len);
    }
  }

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error torrent_make_info_dict_v2(
    Bytes name, usize piece_length_in_bytes, Bytes file_data, Bytes file_name,
    BencodeValue *dst_info_dict, Bytes *dst_pieces_root,
    PieceHash **dst_piece_hashes, usize *dst_piece_hashes_count, Arena *arena) {
  assert(piece_length_in_bytes >= 16 * KiB);      // Per spec.
  assert(is_power_of_two(piece_length_in_bytes)); // Per spec.
  assert(dst_info_dict);
  assert(dst_pieces_root);
  assert(dst_piece_hashes);
  assert(dst_piece_hashes_count);
  assert(arena);

  *dst_pieces_root = (Bytes){0};
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
    key->kind = BencodeKindBytes;
    key->v.bytes = bytes_make((u8 *)"name", sizeof("name") - 1);

    BencodeValue *const value = &dst_info_dict->v.list.data[5];
    value->kind = BencodeKindBytes;
    value->v.bytes = name;
  }

  // `info["piece length"] = piece_length_in_bytes`
  {
    BencodeValue *const key = &dst_info_dict->v.list.data[6];
    key->kind = BencodeKindBytes;
    key->v.bytes = bytes_make((u8 *)"piece length", sizeof("piece length") - 1);

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
    key->kind = BencodeKindBytes;
    key->v.bytes = bytes_make((u8 *)"meta version", sizeof("meta version") - 1);

    BencodeValue *const value = &dst_info_dict->v.list.data[3];
    value->kind = BencodeKindInteger;
    value->v.num = 2;
  }

  // `info["file tree"] = ...`
  {
    BencodeValue *const file_tree_key = &dst_info_dict->v.list.data[0];
    file_tree_key->kind = BencodeKindBytes;
    file_tree_key->v.bytes =
        bytes_make((u8 *)"file tree", sizeof("file tree") - 1);

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
      file_name_key->kind = BencodeKindBytes;
      file_name_key->v.bytes = file_name;

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
        empty_key->kind = BencodeKindBytes;
        empty_key->v.bytes = (Bytes){0};

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
          length_key->kind = BencodeKindBytes;
          length_key->v.bytes =
              bytes_make((u8 *)"length", sizeof("length") - 1);

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
          pieces_root_key->kind = BencodeKindBytes;
          pieces_root_key->v.bytes =
              bytes_make((u8 *)"pieces root", sizeof("pieces root") - 1);

          BencodeValue *const pieces_root_value = &empty_dict->v.list.data[3];
          pieces_root_value->kind = BencodeKindBytes;
          pieces_root_value->v.bytes.len = SHA256_DIGEST_LENGTH;
          pieces_root_value->v.bytes.data = arena_alloc(
              arena, __alignof__(u8), sizeof(u8), SHA256_DIGEST_LENGTH);
          if (NULL == pieces_root_value->v.bytes.data) {
            return (Error){.kind = ErrKindOOM};
          }
          memcpy(pieces_root_value->v.bytes.data, root, SHA256_DIGEST_LENGTH);

          // `piece layers` is keyed by this exact digest, so hand it back
          // rather than making the caller dig it out of the tree.
          *dst_pieces_root = pieces_root_value->v.bytes;
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
  case BencodeKindBytes:
    // <length> `:` <bytes>
    assert(!__builtin_add_overflow(res, usize_digits_base_10(b.v.bytes.len) + 1,
                                   &res));
    assert(!__builtin_add_overflow(res, b.v.bytes.len, &res));
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
bencode_encode_rec(BencodeValue b, Bytes dst, usize depth) {
  assert(depth <= BENCODE_MAX_DEPTH);
  assert(dst.data);
  assert(dst.len >= 2);
  u8 *const dst_before = dst.data;

  switch (b.kind) {
  case BencodeKindInteger: {
    dst.data[0] = 'i';
    bytes_advance(&dst, 1);

    bytes_advance(&dst, encode_isize_base_10(b.v.num, dst));

    dst.data[0] = 'e';
    bytes_advance(&dst, 1);

  } break;
  case BencodeKindBytes: {
    bytes_advance(&dst, encode_usize_base_10(b.v.bytes.len, dst));

    dst.data[0] = ':';
    bytes_advance(&dst, 1);

    if (b.v.bytes.len > 0) {
      memcpy(dst.data, b.v.bytes.data, b.v.bytes.len);
      bytes_advance(&dst, b.v.bytes.len);
    }
  } break;
  case BencodeKindList:
  case BencodeKindDict: {
    dst.data[0] = b.kind == BencodeKindList ? 'l' : 'd';
    bytes_advance(&dst, 1);

    for (usize i = 0; i < b.v.list.len; i++) {
      const BencodeValue item = b.v.list.data[i];
      bytes_advance(&dst, bencode_encode_rec(item, dst, depth + 1));
    }

    dst.data[0] = 'e';
    bytes_advance(&dst, 1);
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
bencode_encode_in_place(BencodeValue b, Bytes dst) {
  assert(dst.data);

  const usize written = bencode_encode_rec(b, dst, 0);
  assert(dst.len == written);

  return written;
}

__attribute__((warn_unused_result)) static Error
bencode_encode(BencodeValue b, Bytes *dst, Arena *arena) {
  assert(dst);
  assert(arena);

  const usize size = bencode_encode_exact_size(b, 0);

  *dst = (Bytes){.data = arena_alloc(arena, __alignof__(u8), sizeof(u8), size),
                 .len = size};
  if (!dst->data) {
    return (Error){.kind = ErrKindOOM};
  }

  const usize written = bencode_encode_in_place(b, *dst);
  assert(written == size);
  assert(written == dst->len);

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static Error torrent_gen_torrent_file_data(
    Bytes file_path, Bytes file_data, Bytes announce_url, Bytes *dst_torrent,
    u8 dst_info_hash[SHA256_DIGEST_LENGTH], Arena scratch, Arena *arena) {
  assert(dst_torrent);
  assert(dst_info_hash);
  assert(arena);

  Error err = {0};

  const Bytes file_name = path_last_component(file_path, PATH_SEPARATOR_UNIX);

  BencodeValue info_dict = {0};
  PieceHash *piece_hashes = NULL;
  usize piece_hashes_count = 0;
  Bytes pieces_root = {0};
  err = torrent_make_info_dict_v2(file_name, TORRENT_BLOCK_SIZE * 16, file_data,
                                  file_name, &info_dict, &pieces_root,
                                  &piece_hashes, &piece_hashes_count, &scratch);

  if (ErrKindNone != err.kind) {
    return err;
  }

  Bytes info_dict_encoded = {0};
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

  Bytes metainfo_dict_encoded = {0};
  err = bencode_encode(metainfo_dict, &metainfo_dict_encoded, arena);
  if (ErrKindNone != err.kind) {
    return err;
  }

  *dst_torrent = metainfo_dict_encoded;

  return (Error){.kind = ErrKindNone};
}

// The tag byte each message carries, so the values are the wire's and not ours:
// they are spelled out to keep it that way, since the parser compares a byte
// straight off the socket against them.
typedef enum {
  TorrentMessageKindChoke = 0,
  TorrentMessageKindUnchoke = 1,
  TorrentMessageKindInterested = 2,
  TorrentMessageKindUninterested = 3,
  TorrentMessageKindHave = 4,
  TorrentMessageKindBitfield = 5,
  TorrentMessageKindRequest = 6,
  TorrentMessageKindPiece = 7,
  TorrentMessageKindCancel = 8,
  // TODO: v2 adds more.

  // The three below are not tags and have no byte on the wire, so they take
  // values from the top of the byte, where no tag reaches.

  // A length of zero with nothing behind it.
  TorrentMessageKindKeepAlive = 0xff,
  // A whole, well formed message whose tag this build does not know: a later
  // version of the protocol, or an extension this does not speak.
  TorrentMessageKindUnknown = 0xfe,
  // No message: fewer bytes have arrived than one needs.
  //
  // Not zero, because zero is `choke` on the wire and the wire's numbering is
  // what this enum is for. So a zeroed `TorrentPeerMessage` reads as `choke`,
  // and the parser sets the kind before it can be read rather than leaving it
  // to whoever declared it.
  TorrentMessageKindNone = 0xfd,
} TorrentMessageKind;

// The name of `kind`, for diagnostics only. The protocol's own names, so a log
// line can be read against a packet capture.
__attribute__((warn_unused_result)) static const char *
torrent_message_kind_to_cstr(TorrentMessageKind kind) {
  switch (kind) {
  case TorrentMessageKindChoke:
    return "choke";
  case TorrentMessageKindUnchoke:
    return "unchoke";
  case TorrentMessageKindInterested:
    return "interested";
  case TorrentMessageKindUninterested:
    return "not interested";
  case TorrentMessageKindHave:
    return "have";
  case TorrentMessageKindBitfield:
    return "bitfield";
  case TorrentMessageKindRequest:
    return "request";
  case TorrentMessageKindPiece:
    return "piece";
  case TorrentMessageKindCancel:
    return "cancel";
  case TorrentMessageKindKeepAlive:
    return "keep-alive";
  case TorrentMessageKindUnknown:
    return "unknown";
  case TorrentMessageKindNone:
    return "none";
  }

  assert(0 && "unreachable");
}

typedef struct {
  u32 idx;
  u32 begin;
  u32 len;
} PeerMessageIndexBeginLength;

typedef struct {
  u32 idx;
  u32 begin;
  // TODO: Data.
} PeerMessagePiece;

// What was skipped. Only a log line wants this, but a log line does want it: a
// peer whose messages are all being stepped over is a build that is behind the
// protocol, and the tag is the only thing that says which extension it is.
typedef struct {
  u8 tag;
  // The message's length prefix, so the tag byte is counted: the bytes stepped
  // over, as the wire gave them.
  u32 size;
} PeerMessageUnknown;

typedef struct {
  TorrentMessageKind kind;
  union {
    u32 have;                                  // Have
    PeerMessageIndexBeginLength idx_begin_len; // Request | Cancel
    PeerMessagePiece piece;
    PeerMessageUnknown unknown; // Unknown

    // TODO: Bitfield.
  } v;
} TorrentPeerMessage;

typedef enum {
  TorrentPeerStateInitial,
  TorrentPeerStateSentHandshake,
  TorrentPeerStateHandshaked,
} TorrentPeerState;

// The name of `state`, for diagnostics only.
__attribute__((warn_unused_result)) static const char *
torrent_peer_state_to_cstr(TorrentPeerState state) {
  switch (state) {
  case TorrentPeerStateInitial:
    return "initial";
  case TorrentPeerStateSentHandshake:
    return "sent-handshake";
  case TorrentPeerStateHandshaked:
    return "handshaked";
  }

  assert(0 && "unreachable");
}

// ---------- The peer as an actor ----------
//
// `torrent_peer_run` below is the whole of a peer's decision making, and it
// cannot fail: no io, no allocation, no clock, and the answer is decided by
// `peer->state` and the event alone. Anything that would be an error is a
// command, `Close` above all, which is what lets it return a plain count
// instead of an `Error`.
//
// Everything else in this file is the bridge: it turns bytes into events and
// commands into io operations, and it owns `recv_buf`, `send_buf` and all the
// serialising. `SendHandshake` means "put one on the wire", not "here it is".

// The commands one event can produce. Eight is the bound because the fan-out is
// a write, a file operation and a close at the very worst; the assert in
// `torrent_peer_command_push` is what keeps that true.
#define TORRENT_PEER_COMMANDS_MAX 8

// Every event is the transport reporting, one peer message, or time having
// moved far enough that something is due. Zero is not one of them: `Initial`
// waits for `Accepted` or `Connected`, which are real events carrying real
// news, rather than for a sentinel that a zeroed struct would produce by
// accident.
typedef enum {
  TorrentEventKindNone = 0,
  // The transport has a connection: one answered, and one asked for.
  TorrentEventKindAccepted,
  TorrentEventKindConnected,
  // The transport says the connection is over: end of file, a reset, or an
  // operation that failed. Nothing more will be read from it or written to it.
  TorrentEventKindHangup,
  // The peer's bytes are not the protocol: a handshake that matches nothing, or
  // a message whose length and tag disagree. Not a `Hangup`, because there the
  // transport is gone and here it is the peer that is wrong.
  TorrentEventKindMalformed,
  TorrentEventKindHandshake,
  TorrentEventKindMessage,
  // One of this peer's deadlines has passed. Which one is not said, because the
  // peer's own fields already say it.
  TorrentEventKindDeadline,
} TorrentEventKind;

// The name of `kind`, for diagnostics only.
__attribute__((warn_unused_result)) static const char *
torrent_event_kind_to_cstr(TorrentEventKind kind) {
  switch (kind) {
  case TorrentEventKindNone:
    return "none";
  case TorrentEventKindAccepted:
    return "accepted";
  case TorrentEventKindConnected:
    return "connected";
  case TorrentEventKindHangup:
    return "hangup";
  case TorrentEventKindMalformed:
    return "malformed";
  case TorrentEventKindHandshake:
    return "handshake";
  case TorrentEventKindMessage:
    return "message";
  case TorrentEventKindDeadline:
    return "deadline";
  }

  assert(0 && "unreachable");
  return "?";
}

typedef struct {
  TorrentEventKind kind;
  union {
    // A message the parser understood. An unknown tag never gets here: the
    // bridge steps over it, so this stays closed.
    TorrentPeerMessage msg; // Message
  } v;
} TorrentEvent;

// Never a read: the bridge keeps one outstanding whatever the state, so this
// enum having no way to ask for one is the point. A state that forgot to ask
// for a read would be a peer going quiet, which is a whole class of bug that
// only shows up hours later.
//
// No `ArmTimer` either: a deadline is set by assigning to the peer, which
// `torrent_peer_run` does itself, so it needs no help from the bridge.
typedef enum {
  TorrentCommandKindNone = 0,
  TorrentCommandKindSendHandshake,
  TorrentCommandKindSendKeepAlive,
  // Always last in a batch, which `torrent_peer_command_push` keeps true: the
  // slot may be back in the pool before the bridge looks at the next command,
  // so there must not be one.
  TorrentCommandKindClose,

  TorrentCommandKindInterested,
  TorrentCommandKindUnchoke,
  // TODO: `SendBitfield`, `SendRequests` and `WriteBlock`, once there is a
  // torrent behind a peer to have pieces of. Each of them needs state that does
  // not exist yet -- the `have` and `requested` bitsets, and a file to write
  // into -- so they are not sitting here as values with nothing behind them.
} TorrentCommandKind;

// The name of `kind`, for diagnostics only.
__attribute__((warn_unused_result)) static const char *
torrent_command_kind_to_cstr(TorrentCommandKind kind) {
  switch (kind) {
  case TorrentCommandKindNone:
    return "none";
  case TorrentCommandKindSendHandshake:
    return "send-handshake";
  case TorrentCommandKindSendKeepAlive:
    return "send-keep-alive";
  case TorrentCommandKindClose:
    return "close";
  case TorrentCommandKindInterested:
    return "interested";
  case TorrentCommandKindUnchoke:
    return "unchoke";
  }

  assert(0 && "unreachable");
  return "?";
}

typedef struct {
  TorrentCommandKind kind;
  // TODO: a union, once `WriteBlock` has a block to carry.
} TorrentCommand;

// Does carrying this out put bytes on the wire? Which is what the keep-alive
// deadline measures, so every command that goes out is asked.
__attribute__((warn_unused_result)) static bool
torrent_command_kind_sends(TorrentCommandKind kind) {
  switch (kind) {
  case TorrentCommandKindSendHandshake:
  case TorrentCommandKindSendKeepAlive:
  case TorrentCommandKindInterested:
  case TorrentCommandKindUnchoke:
    return true;
  case TorrentCommandKindNone:
  case TorrentCommandKindClose:
    return false;
  }

  assert(0 && "unreachable");
  return false;
}

typedef struct TorrentNetworkCtx TorrentNetworkCtx;

#define TORRENT_INFO_HASH_LEN 20

#define TORRENT_PEER_ID_LEN 20

// One length byte, the 19 it counts, 8 reserved bytes, the info hash and the
// peer id: 68 bytes, never more and never fewer.
#define TORRENT_PEER_HANDSHAKE_LEN (1 + 19 + 8 + 20 + 20)

#define TORRENT_PEER_RECV_BUF_CAP 1024

#define TORRENT_PEER_SEND_BUF_CAP 1024

typedef struct {
  // The caller's, passed through `io_listen_and_serve_tcp_ipv4`.
  void *cb_ctx;
  IO *io;
  TorrentNetworkCtx *network_ctx;
  i32 socket;
  Ipv4Addr addr;

  IoCompletion completion_read;
  // TODO: Pipelining?
  IoCompletion completion_write;
  // The close reuses neither of those: a hang-up can be decided while a read or
  // a write is still with the kernel, and one completion holds one operation.
  IoCompletion completion_close;

  // What the kernel is still holding. Each completion carries one operation at
  // a time, and both the read path and the write path drive the tick, so
  // without this the tick arms a second read over the one already in flight.
  bool read_in_flight;
  bool write_in_flight;
  // The connection is over; the hang-up is waiting for the operations above to
  // report before it can be submitted, because the slot cannot be handed back
  // while a completion inside it is still registered.
  bool closing;

  u8 recv_buf[TORRENT_PEER_RECV_BUF_CAP];
  usize recv_len;

  u8 send_buf[TORRENT_PEER_SEND_BUF_CAP];
  usize send_len;

  Bytes info_hash;
  u8 id[20];
  // More: torrent, etc.
  TorrentPeerState state;

  // Deadlines, as absolute instants on the clock `IO.monotonic_ns` reads;
  // `TORRENT_PEER_DEADLINE_NONE` is one that is not set. Absolute and not
  // intervals, so that the walk below compares them without repeating the
  // arithmetic and so that re-arming one is a single assignment.
  //
  // What the peer has not said, and what we have not said. Two and not one
  // because they measure opposite directions; see the constants below.
  u64 idle_due_at;
  u64 keep_alive_due_at;
  // The earliest of the two, recomputed by every `torrent_peer_run`. The loop
  // reads it to decide when to come back, so nothing else may write it.
  u64 deadline_at;

  // Scratch: valid from a `torrent_peer_run` returning until the next call
  // overwrites them from index 0. They live here so that the bridge hands
  // nothing in and gets back only how many were written.
  TorrentCommand commands[TORRENT_PEER_COMMANDS_MAX];

  Logger logger;
} TorrentPeer;

// Deadlines live on the peer as absolute instants, and this is one that is not
// set. `UINT64_MAX` rather than zero, so that "not set" compares as further
// away than anything real and needs no case of its own.
#define TORRENT_PEER_DEADLINE_NONE UINT64_MAX

// Two deadlines and not one, because they measure opposite directions. Idle
// measures what the peer has not said and closes; keep-alive measures what we
// have not said and sends. Reset either on the other's traffic and there is a
// bug waiting: a keep-alive reset by what arrives means a peer that floods us
// while we stay quiet never hears from us and closes us, and an idle reset by
// what we send means our own keep-alives hold a dead peer open for ever.
//
// A minute would be too short for the idle side whatever else changed. BEP 3
// has keep-alives going out about every two minutes, so a peer silent for
// ninety seconds is behaving, and an idle timeout at or under two minutes drops
// peers for nothing. Idle is twice keep-alive so that one in flight cannot race
// the close, and so that one lost keep-alive is survivable.
#define TORRENT_PEER_KEEP_ALIVE_NS (2 * Minute)
#define TORRENT_PEER_IDLE_NS (2 * TORRENT_PEER_KEEP_ALIVE_NS)

// Before the handshake, silence is never legitimate: a peer that connects sends
// its handshake at once. So the same `idle_due_at` field carries a much tighter
// interval until `Handshaked`, which is what stops a peer that connects and
// then says nothing from holding a pool slot for four minutes. One deadline
// with the interval chosen by the state, because closing on silence is one rule
// either way.
#define TORRENT_PEER_HANDSHAKE_NS (30 * Second)

// The longest one turn of the event loop waits when no deadline is nearer. Not
// a tick: nothing is polled and a turn ending is not a reason to do anything.
// It is only here so that a loop with no peers at all still comes back.
#define TORRENT_PEER_LOOP_WAIT_NS_MAX (5 * Second)

// The earliest deadline still live, which is what the loop reads to decide when
// to call again. Recomputed at the end of every `torrent_peer_run`, so it is
// never stale and there is nothing to keep in sync.
//
// TODO: a third will join these once requests exist, and it is a third pair and
// not a variant of these two: reset by sending a request, and answered by a
// block to ask someone else for rather than by a peer to close.
__attribute__((warn_unused_result)) static u64
torrent_peer_deadline_earliest(const TorrentPeer *peer) {
  assert(peer);

  u64 earliest = TORRENT_PEER_DEADLINE_NONE;
  const u64 deadlines[] = {peer->idle_due_at, peer->keep_alive_due_at};

  for (usize i = 0; i < sizeof(deadlines) / sizeof(deadlines[0]); i++) {
    if (deadlines[i] < earliest) {
      earliest = deadlines[i];
    }
  }

  return earliest;
}

// One command onto the peer's array. The assert is where the bound is kept, and
// it is here rather than at the end of `torrent_peer_run` so that it fires in
// the handler that wrote too many, before the array is already past its end.
static void torrent_peer_command_push(TorrentPeer *peer, u32 *commands_len,
                                      const TorrentCommand command) {
  assert(peer);
  assert(commands_len);
  assert(*commands_len < TORRENT_PEER_COMMANDS_MAX);
  assert(TorrentCommandKindNone != command.kind);
  // Nothing follows a `Close`: the slot may be back in the pool before the
  // bridge reaches the next command, so there must not be one.
  assert(0 == *commands_len ||
         TorrentCommandKindClose != peer->commands[*commands_len - 1].kind);

  peer->commands[(*commands_len)++] = command;
}

// `torrent_peer_run` cannot fail, so the answer is a count and never an
// `Error`. At most `TORRENT_PEER_COMMANDS_MAX`, and zero whenever the event
// asks nothing of this peer, which is the ordinary answer. The commands are in
// `peer->commands[0..n)` and are the bridge's until it calls again.
//
// `now_ns` is monotonic and passed in rather than read here, which is what
// keeps this a function of its arguments: deciding on a deadline needs the
// time, and reading a clock would be the one syscall in here. `last_run_at` is
// deliberately not a parameter beside it -- every deadline is relative to
// something the protocol did and each of those has its own instant, so
// measuring from when this last ran drifts off what was meant, this being
// called constantly for unrelated reasons.
__attribute__((warn_unused_result)) static u32
torrent_peer_run(TorrentPeer *peer, const TorrentEvent event,
                 const u64 now_ns) {
  assert(peer);
  assert(TorrentEventKindNone != event.kind);

  u32 commands_len = 0;

  // `break` and one tail return, not a return per case: `-Wunreachable-code` is
  // on, so a `break` behind a `return` would not build.
  switch (peer->state) {
  case TorrentPeerStateInitial: {
    // Nothing goes out before the transport says there is somewhere to put it.
    if (TorrentEventKindAccepted != event.kind &&
        TorrentEventKindConnected != event.kind) {
      torrent_peer_command_push(
          peer, &commands_len,
          (TorrentCommand){.kind = TorrentCommandKindClose});
      break;
    }

    // No saga, and no 'handshake sent (confirmed)' state to go with it: this
    // state means the handshake is the transport's problem now. A TCP write
    // that fails is not recoverable, so the connection ends, and a request is
    // timed from its own write completing rather than from here.
    peer->state = TorrentPeerStateSentHandshake;
    // The tight interval, until there is a handshake to be generous about.
    peer->idle_due_at = now_ns + TORRENT_PEER_HANDSHAKE_NS;
    torrent_peer_command_push(
        peer, &commands_len,
        (TorrentCommand){.kind = TorrentCommandKindSendHandshake});
  } break;

  case TorrentPeerStateSentHandshake: {
    // A `Deadline` needs no case of its own here, and that is the point of one
    // idle deadline rather than a handshake one beside it: anything that is not
    // the handshake closes, silence included.
    if (TorrentEventKindHandshake != event.kind) {
      torrent_peer_command_push(
          peer, &commands_len,
          (TorrentCommand){.kind = TorrentCommandKindClose});
      break;
    }

    peer->state = TorrentPeerStateHandshaked;
    // Silence is allowed from here on, so the interval opens up.
    peer->idle_due_at = now_ns + TORRENT_PEER_IDLE_NS;
    peer->keep_alive_due_at = now_ns + TORRENT_PEER_KEEP_ALIVE_NS;

    // TODO: a `SendBitfield` here, once there are pieces to announce.
    torrent_peer_command_push(
        peer, &commands_len,
        (TorrentCommand){.kind = TorrentCommandKindInterested});
    torrent_peer_command_push(
        peer, &commands_len,
        (TorrentCommand){.kind = TorrentCommandKindUnchoke});
  } break;

  case TorrentPeerStateHandshaked: {
    // The bridge produces these in `Initial` and `SentHandshake` only, so one
    // here is the bridge having lost track of the state rather than anything a
    // peer can cause.
    assert(TorrentEventKindAccepted != event.kind);
    assert(TorrentEventKindConnected != event.kind);
    assert(TorrentEventKindHandshake != event.kind);

    if (TorrentEventKindDeadline == event.kind) {
      // Nothing heard for long enough that the connection is not worth its
      // slot. The pool is the reason to care: a peer that says nothing is
      // holding a slot another peer could use.
      if (now_ns >= peer->idle_due_at) {
        torrent_peer_command_push(
            peer, &commands_len,
            (TorrentCommand){.kind = TorrentCommandKindClose});
        break;
      }

      // Keep-alive is this and only this, and it is a separate deadline because
      // it is about what we have not said rather than about what the peer has
      // not. Anything going out re-arms it below, so a peer we are busy with
      // never gets one, and a peer sending extensions we skip does not earn one
      // per message.
      if (now_ns >= peer->keep_alive_due_at) {
        torrent_peer_command_push(
            peer, &commands_len,
            (TorrentCommand){.kind = TorrentCommandKindSendKeepAlive});
      }
      break;
    }

    // The transport is gone, or the peer is talking nonsense. Either way there
    // is nothing left to say to it.
    if (TorrentEventKindHangup == event.kind ||
        TorrentEventKindMalformed == event.kind) {
      torrent_peer_command_push(
          peer, &commands_len,
          (TorrentCommand){.kind = TorrentCommandKindClose});
      break;
    }

    // Anything said at all is the peer being alive, which is the whole of the
    // re-arm: one assignment, and no structure to tell about it.
    assert(TorrentEventKindMessage == event.kind);
    peer->idle_due_at = now_ns + TORRENT_PEER_IDLE_NS;

    // TODO: act on the message. This is where the `have` and `requested`
    // bitsets are read and written, and where a block becomes a file write plus
    // whatever the pacing says to request next.

    // Nothing here wanted anything, so nothing is what goes out.
  } break;
  }

  // Anything going out is us saying something, so the keep-alive deadline
  // moves: it measures our own silence, and there is about to be none. One
  // place for it, rather than an assignment beside every push that could forget
  // one.
  for (u32 i = 0; i < commands_len; i++) {
    if (torrent_command_kind_sends(peer->commands[i].kind)) {
      peer->keep_alive_due_at = now_ns + TORRENT_PEER_KEEP_ALIVE_NS;
      break;
    }
  }

  // The loop reads this to decide when to call again, so it is recomputed on
  // every path rather than wherever a deadline happened to move.
  peer->deadline_at = torrent_peer_deadline_earliest(peer);

  return commands_len;
}

#define TORRENT_PEERS_MAX 1024

typedef u64 PoolSlotGroup;

// Unit is bits.
#define POOL_SLOTS_PER_GROUP (sizeof(PoolSlotGroup) * 8)
_Static_assert(0 == (TORRENT_PEERS_MAX % POOL_SLOTS_PER_GROUP),
               "must be a multiple");

#define POOL_SLOT_GROUPS (TORRENT_PEERS_MAX / POOL_SLOTS_PER_GROUP)

typedef struct {
  // Bitset.
  // Bit `i` of group `g` means: `slots[g * POOL_SLOTS_PER_GROUP + i]` is
  // occupied.
  PoolSlotGroup occupied[POOL_SLOT_GROUPS];
  TorrentPeer slots[TORRENT_PEERS_MAX];
} TorrentpeerHandleCtxPool;

struct TorrentNetworkCtx {
  TorrentpeerHandleCtxPool pool;
  Bytes info_hash;
  u32 log_level_mask;
  // More...
};

// The most messages a full receive buffer can hold. A keep-alive is four bytes
// of length and nothing behind it, and nothing on the wire is shorter, so this
// is the bound on how many come out of one read.
#define TORRENT_PEER_MSGS_PER_BUF_MAX (TORRENT_PEER_RECV_BUF_CAP / sizeof(u32))

// The payload of a `request` or a `cancel`: three `u32`s, and nothing else is a
// legal length for one.
#define TORRENT_PEER_MSG_IDX_BEGIN_LEN_SIZE (1 + 3 * sizeof(u32))

// The payload of a `have`: one `u32`.
#define TORRENT_PEER_MSG_HAVE_SIZE (1 + sizeof(u32))

// A message that is a tag and no payload, so its length prefix says one byte.
#define TORRENT_PEER_MSG_EMPTY_SIZE 1
// What that costs in the send buffer: the prefix as well as the tag.
#define TORRENT_PEER_MSG_EMPTY_LEN (sizeof(u32) + TORRENT_PEER_MSG_EMPTY_SIZE)

// The `idx` and `begin` in front of a `piece`'s data.
#define TORRENT_PEER_MSG_PIECE_HEADER_SIZE (1 + 2 * sizeof(u32))

// One message off the front of `*data`, if a whole one is there.
//
// An error is a peer talking nonsense, and the connection is over. Short of
// that, `dst_msg->kind` is the whole answer and a caller needs every value of
// it:
//
// - `TorrentMessageKindNone` is "not yet": fewer bytes have arrived than the
//   message needs, and `*data` is left exactly as it was so that the same call
//   works once more of it turns up. TCP cuts a stream wherever it likes, and
//   half a message is the ordinary case, not a broken peer.
// - `TorrentMessageKindUnknown` is a whole message with a tag this does not
//   know, stepped over: `*data` has moved past it and there may well be another
//   behind it. It is not waited on, since a message left in the buffer is one
//   the caller comes back to for ever. `v.unknown` says what was skipped, which
//   is the only reason a caller would care.
// - Anything else is that message, with `*data` advanced past it and the rest
// of
//   `*dst_msg` filled in.
//
// So the two answers that carry no message still differ in `*data`, and a
// caller that stops on `None` as though it were `Unknown` stalls.
__attribute__((warn_unused_result)) static Error
torrent_peer_parse_message(Bytes *data, TorrentPeerMessage *dst_msg) {
  assert(data);
  assert(dst_msg);

  // Set before anything can go wrong, so no path out of here leaves a kind the
  // caller did not get from this call.
  memset(dst_msg, 0, sizeof(*dst_msg));
  dst_msg->kind = TorrentMessageKindNone;

  // A copy, and `*data` is only moved on at the very end: a message that turns
  // out to be half-arrived must leave the caller's `Bytes` untouched, or the
  // bytes it did consume are lost before the rest ever gets here.
  Bytes remaining = *data;

  u32 msg_size = 0;
  if (!bytes_consume_u32_be(&remaining, &msg_size)) {
    return (Error){.kind = ErrKindNone};
  }

  // A length the buffer could never hold is not something to wait for: the peer
  // would be waited on until the buffer filled and then for ever after.
  if (msg_size > TORRENT_PEER_RECV_BUF_CAP - sizeof(msg_size)) {
    return (Error){.kind = ErrKindInvalidData};
  }

  // A keep-alive is the length on its own, with no tag behind it.
  if (0 == msg_size) {
    dst_msg->kind = TorrentMessageKindKeepAlive;
    *data = remaining;
    return (Error){.kind = ErrKindNone};
  }

  // The length counts the tag and the payload, so the whole of it has to be
  // here before any of it is read.
  if (remaining.len < msg_size) {
    return (Error){.kind = ErrKindNone};
  }

  // Nothing past this message, so a payload that is shorter than its length
  // claims is the peer's mistake and not a short read.
  Bytes body = bytes_take(remaining, msg_size);
  bytes_advance(&remaining, msg_size);
  assert(msg_size == body.len);
  assert(remaining.len < data->len);

  u8 msg_tag = 0;
  // The length is at least 1 and the body is that long, so the tag is there.
  assert(bytes_consume_u8(&body, &msg_tag));
  assert(msg_size - 1 == body.len);

  switch (msg_tag) {
  case TorrentMessageKindChoke:
  case TorrentMessageKindUnchoke:
  case TorrentMessageKindInterested:
  case TorrentMessageKindUninterested:
    // The tag and nothing else.
    if (1 != msg_size) {
      return (Error){.kind = ErrKindInvalidData};
    }
    dst_msg->kind = msg_tag;
    assert(0 == body.len);
    break;

  case TorrentMessageKindHave:
    if (TORRENT_PEER_MSG_HAVE_SIZE != msg_size) {
      return (Error){.kind = ErrKindInvalidData};
    }
    dst_msg->kind = msg_tag;
    assert(bytes_consume_u32_be(&body, &dst_msg->v.have));
    assert(0 == body.len);
    break;

  case TorrentMessageKindBitfield:
    // One bit per piece, so the length is whatever the torrent needs. The bits
    // themselves are skipped for now.
    // TODO: keep them.
    dst_msg->kind = msg_tag;
    assert(msg_size - 1 == body.len);
    break;

  case TorrentMessageKindRequest:
  case TorrentMessageKindCancel:
    if (TORRENT_PEER_MSG_IDX_BEGIN_LEN_SIZE != msg_size) {
      return (Error){.kind = ErrKindInvalidData};
    }
    dst_msg->kind = msg_tag;
    assert(bytes_consume_u32_be(&body, &dst_msg->v.idx_begin_len.idx));
    assert(bytes_consume_u32_be(&body, &dst_msg->v.idx_begin_len.begin));
    assert(bytes_consume_u32_be(&body, &dst_msg->v.idx_begin_len.len));
    assert(0 == body.len);
    break;

  case TorrentMessageKindPiece:
    // The two numbers that say where this block goes, then the block. A piece
    // with no block at all is a peer talking nonsense.
    if (msg_size <= TORRENT_PEER_MSG_PIECE_HEADER_SIZE) {
      return (Error){.kind = ErrKindInvalidData};
    }
    dst_msg->kind = msg_tag;
    assert(bytes_consume_u32_be(&body, &dst_msg->v.piece.idx));
    assert(bytes_consume_u32_be(&body, &dst_msg->v.piece.begin));
    // What is left is the block, which is why a `piece` with nothing left is
    // refused above.
    assert(body.len > 0);
    assert(msg_size - TORRENT_PEER_MSG_PIECE_HEADER_SIZE == body.len);
    // TODO: keep the block.
    break;

    // TODO: v2 messages;

  default:
    // The length says where it ends, so it is stepped over without losing the
    // stream, which a guess at its shape would. The caller reports it: what was
    // skipped is in the message, so this stays a function of its bytes alone.
    dst_msg->kind = TorrentMessageKindUnknown;
    dst_msg->v.unknown.tag = msg_tag;
    dst_msg->v.unknown.size = msg_size;
    break;
  }

  // Every path out of the switch either set a kind or reported an error. A tag
  // this knows keeps the wire's own number; anything else is `Unknown`.
  assert(msg_tag == (u8)dst_msg->kind ||
         TorrentMessageKindUnknown == dst_msg->kind);

  *data = remaining;
  // A message was taken, so the caller's `Bytes` is strictly shorter: that is
  // what keeps the loop that calls this from running for ever.
  assert(data->len < remaining.len + msg_size + sizeof(msg_size));

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static TorrentPeer *
torrent_peer_ctx_pool_acquire(TorrentpeerHandleCtxPool *pool) {
  assert(pool);

  for (usize i = 0; i < POOL_SLOT_GROUPS; i++) {
    const PoolSlotGroup slot_group = pool->occupied[i];

    const i32 first_unset_bit = __builtin_ffsll((i64)~slot_group);

    // This group is full; there may be a free slot in a later one.
    if (0 == first_unset_bit) {
      continue;
    }

    const u32 bit_idx = (u32)(first_unset_bit - 1);
    const PoolSlotGroup mask = 1ULL << bit_idx;

    // The read above is still good: one thread runs the loop, and between that
    // read and this write there is nothing for it to have been doing but this.
    assert(0 == (pool->occupied[i] & mask));
    pool->occupied[i] |= mask;

    const usize slot_idx = i * POOL_SLOTS_PER_GROUP + bit_idx;
    assert(slot_idx < TORRENT_PEERS_MAX);

    TorrentPeer *res = &pool->slots[slot_idx];
    assert(0 == res->cb_ctx);
    assert(0 == res->io);
    assert(0 == res->network_ctx);
    assert(0 == res->socket);
    assert(0 == res->addr.ip);
    assert(0 == res->addr.port);
    // Nothing is in flight on a free slot, so nothing is pointing at its
    // completion either.
    assert(IoActionKindNone == res->completion_read.action.kind);
    assert(NULL == res->completion_read.cb);
    assert(IoActionKindNone == res->completion_write.action.kind);
    assert(NULL == res->completion_write.cb);
    assert(IoActionKindNone == res->completion_close.action.kind);
    assert(NULL == res->completion_close.cb);
    assert(!res->read_in_flight);
    assert(!res->write_in_flight);
    assert(!res->closing);
    return res;
  }

  return NULL;
}

static void torrent_peer_ctx_pool_release(TorrentpeerHandleCtxPool *pool,
                                          TorrentPeer *slot) {
  assert(pool);
  assert(slot);

  assert(slot >= pool->slots);
  const usize slot_idx = (usize)(slot - pool->slots);
  assert(slot_idx < TORRENT_PEERS_MAX);

  const usize slot_group_idx = slot_idx / POOL_SLOTS_PER_GROUP;
  assert(slot_group_idx < POOL_SLOT_GROUPS);
  const u32 bit_idx = slot_idx % POOL_SLOTS_PER_GROUP;

  // We are still the owner so we are responsible for zeroing it.
  memset(slot, 0, sizeof(*slot));

  const PoolSlotGroup mask = 1ULL << bit_idx;

  // Sanity check against double release of the same slot: it was occupied.
  assert(0 != (pool->occupied[slot_group_idx] & mask));
  pool->occupied[slot_group_idx] &= ~mask;
}

// Everything that is true of a live connection, wherever one is looked at.
// Called at the top of each of its callbacks: a slot is reached only through
// them, so a field that goes wrong is caught at the next one rather than
// wherever it happens to show.
static void torrent_peer_assert_invariants(const TorrentPeer *peer) {
  assert(peer);
  assert(peer->io);
  assert(peer->io->env);
  assert(peer->network_ctx);
  assert(peer->socket >= 0);
  assert(TORRENT_INFO_HASH_LEN == peer->info_hash.len);
  assert(peer->info_hash.data == peer->network_ctx->info_hash.data);

  // The buffers hold what they say they hold.
  assert(peer->recv_len <= TORRENT_PEER_RECV_BUF_CAP);
  assert(peer->send_len <= TORRENT_PEER_SEND_BUF_CAP);

  // Each completion finds its way back to this slot, which is how every
  // callback above gets its `peer`.
  assert(peer->completion_read.ctx == peer);
  assert(peer->completion_write.ctx == peer);
  assert(peer->completion_close.ctx == peer);

  // The initial state is the one before anything has been sent or received.
  assert(TorrentPeerStateInitial != peer->state ||
         (0 == peer->recv_len && 0 == peer->send_len));

  // Not a field anyone may set: it is what the last `torrent_peer_run`
  // computed, and the walk that finds due peers reads it. One that had drifted
  // would mean a peer woken at the wrong moment, or never.
  assert(peer->deadline_at == torrent_peer_deadline_earliest(peer));

  // What the buffers hold beyond that is not invariant: a read reports before
  // the tick that drains it, so between the two there is a whole handshake, or
  // a whole message, still sitting there.
}

// A connection, one callback at a time.
static void torrent_peer_on_close(IoCompletion *completion, Error err,
                                  usize res) {
  assert(completion);
  (void)err;
  (void)res;

  TorrentPeer *const peer = completion->ctx;
  torrent_peer_assert_invariants(peer);
  assert(&peer->completion_close == completion);
  // The hang-up is the last thing to happen on a connection, so nothing else
  // can still be pointing into the slot that is about to be handed back.
  assert(peer->closing);
  assert(!peer->read_in_flight);
  assert(!peer->write_in_flight);

  log(&peer->logger, LogLevelInfo, "closed");

  // Whatever hanging up reported is of no use to anyone: the connection is
  // over, and there is nothing left to do differently.
  torrent_peer_ctx_pool_release(&peer->network_ctx->pool, peer);
}

// Hang up and hand the slot back. Every way a connection can end goes through
// here, so the slot is released exactly once however it ended.
// End the connection. Called from wherever a connection turns out to be over,
// which is any of the callbacks, so it has to cope with being called while
// another operation is still with the kernel: the descriptor cannot be closed
// under a registration that still points at this slot. The last callback to
// report comes back here and finishes the job.
static void torrent_peer_close(TorrentPeer *peer) {
  torrent_peer_assert_invariants(peer);
  log(&peer->logger, LogLevelInfo, "queuing close");

  peer->closing = true;

  // Something is still in flight. Its callback sees `closing` and comes back.
  if (peer->read_in_flight || peer->write_in_flight) {
    return;
  }

  IO *const io = peer->io;

  const Error err = io->close(io, &peer->completion_close, peer->socket,
                              torrent_peer_on_close);
  if (ErrKindNone != err.kind) {
    // The close was never submitted, so `torrent_peer_on_close` will not run:
    // hang up here instead, and hand the slot back, or the connection would
    // cost a slot for the life of the process.
    log_err(&peer->logger, "failed to hang up on a peer", err);
    (void)io->env->close_socket(io->env, peer->socket);
    torrent_peer_ctx_pool_release(&peer->network_ctx->pool, peer);
  }
}

__attribute__((warn_unused_result)) static bool
torrent_check_handshake(Bytes data, Bytes info_hash_expected, Bytes *peer_id) {
  assert(peer_id);
  // Raw bytes, not the hex form: a hex info hash would simply match nothing,
  // and the peer would look like it answered with the wrong torrent.
  assert(TORRENT_INFO_HASH_LEN == info_hash_expected.len);

  // One length byte, then the 19 bytes it counts, then 8 reserved bytes, the
  // info hash and the peer id: 68 bytes, never more and never fewer. The
  // length byte is part of `handshake_header_expected` below, not of the 19.

  if (TORRENT_PEER_HANDSHAKE_LEN != data.len) {
    return false;
  }

  const Bytes handshake_header_expected =
      bytes_from_cstr("\x13"
                      "BitTorrent protocol");
  if (!bytes_starts_with(data, handshake_header_expected)) {
    return false;
  }
  bytes_advance(&data, handshake_header_expected.len);

  // 8 reserved bytes.
  bytes_advance(&data, 8);

  const Bytes info_hash_actual = bytes_take(data, TORRENT_INFO_HASH_LEN);
  if (!bytes_eq(info_hash_actual, info_hash_expected)) {
    return false;
  }
  bytes_advance(&data, TORRENT_INFO_HASH_LEN);

  *peer_id = bytes_take(data, TORRENT_PEER_ID_LEN);
  assert(TORRENT_PEER_ID_LEN == peer_id->len);

  return true;
}

static void torrent_peer_tick(TorrentPeer *peer, IO *io, u64 now_ns);

__attribute__((warn_unused_result)) static bool
torrent_peer_dispatch(TorrentPeer *peer, TorrentEvent event, u64 now_ns);

static void torrent_peer_on_read(IoCompletion *completion, Error err,
                                 usize res);

static void torrent_peer_on_write(IoCompletion *completion, Error err,
                                  usize res);

// Wait for more from the peer, after the bytes already buffered: a read aimed
// at the front of the buffer would overwrite a message that is still arriving.
__attribute__((warn_unused_result)) static Error
torrent_peer_read(TorrentPeer *peer, IO *io) {
  torrent_peer_assert_invariants(peer);
  assert(io == peer->io);
  // There is somewhere to put what arrives.
  assert(peer->recv_len < TORRENT_PEER_RECV_BUF_CAP);
  assert(!peer->read_in_flight);
  assert(!peer->closing);

  log(&peer->logger, LogLevelDebug, "queuing read: space=%zu",
      TORRENT_PEER_RECV_BUF_CAP - peer->recv_len);

  const Bytes dst = bytes_make(peer->recv_buf + peer->recv_len,
                               TORRENT_PEER_RECV_BUF_CAP - peer->recv_len);

  const Error err = io->read(io, &peer->completion_read, peer->socket, dst,
                             torrent_peer_on_read);
  if (ErrKindNone == err.kind) {
    peer->read_in_flight = true;
  }

  return err;
}

// Send what is in the send buffer. One write at a time, like the read: what a
// short write leaves behind goes out from the callback.
__attribute__((warn_unused_result)) static Error
torrent_peer_write(TorrentPeer *peer, IO *io) {
  torrent_peer_assert_invariants(peer);
  assert(io == peer->io);
  // There is something to send.
  assert(peer->send_len > 0);
  assert(!peer->write_in_flight);
  assert(!peer->closing);

  log(&peer->logger, LogLevelDebug, "queuing write: space=%zu", peer->send_len);

  const Bytes src = bytes_make(peer->send_buf, peer->send_len);

  const Error err = io->write(io, &peer->completion_write, peer->socket, src,
                              torrent_peer_on_write);
  if (ErrKindNone == err.kind) {
    peer->write_in_flight = true;
  }

  return err;
}

static void torrent_peer_on_write(IoCompletion *completion, Error err,
                                  usize res) {
  assert(completion);

  TorrentPeer *const peer = completion->ctx;
  torrent_peer_assert_invariants(peer);
  assert(&peer->completion_write == completion);
  assert(peer->write_in_flight);
  // Nothing goes out that was not asked for.
  assert(peer->send_len > 0);
  assert(res <= peer->send_len);

  log(&peer->logger, LogLevelDebug, "on_write");

  peer->write_in_flight = false;

  // The hang-up was waiting for this to report.
  if (peer->closing) {
    torrent_peer_close(peer);
    return;
  }

  // A failure, or a write that took nothing: going back with the same bytes
  // would take nothing again, so there is no way forward on this connection
  // either way. `Hangup` is a `Close` in every state, so there is nothing left
  // to do here whatever comes back.
  if (ErrKindNone != err.kind || 0 == res) {
    if (ErrKindNone != err.kind) {
      log_err(&peer->logger, "failed to write to a peer", err);
    }

    const bool alive = torrent_peer_dispatch(
        peer, (TorrentEvent){.kind = TorrentEventKindHangup},
        peer->io->monotonic_ns(peer->io));
    assert(!alive);
    return;
  }

  // Drop what went out, keeping what did not at the front of the buffer. A
  // short write is the ordinary case on a socket whose send buffer is nearly
  // full.
  assert(res <= peer->send_len);
  const usize unsent = peer->send_len - res;
  memmove(peer->send_buf, peer->send_buf + res, unsent);
  peer->send_len = unsent;

  torrent_peer_tick(peer, peer->io, peer->io->monotonic_ns(peer->io));
}

static void torrent_peer_on_read(IoCompletion *completion, Error err,
                                 usize res) {
  assert(completion);

  TorrentPeer *const peer = completion->ctx;
  torrent_peer_assert_invariants(peer);
  assert(&peer->completion_read == completion);
  assert(peer->read_in_flight);
  // What was asked for fitted in what was left of the buffer, so what came back
  // does too.
  assert(res <= TORRENT_PEER_RECV_BUF_CAP - peer->recv_len);

  log(&peer->logger, LogLevelDebug, "on_read");

  peer->read_in_flight = false;

  // The hang-up was waiting for this to report.
  if (peer->closing) {
    torrent_peer_close(peer);
    return;
  }

  // A failure, or nothing read and nothing wrong, which is end of file: the
  // peer hung up, and asking again would answer 0 for ever since a descriptor
  // at end of file stays readable. `Hangup` is a `Close` in every state, so
  // there is nothing left to do here whatever comes back.
  if (ErrKindNone != err.kind || 0 == res) {
    if (ErrKindNone != err.kind) {
      log_err(&peer->logger, "failed to read from a peer", err);
    }

    const bool alive = torrent_peer_dispatch(
        peer, (TorrentEvent){.kind = TorrentEventKindHangup},
        peer->io->monotonic_ns(peer->io));
    assert(!alive);
    return;
  }

  assert(!__builtin_add_overflow(peer->recv_len, res, &peer->recv_len));
  assert(peer->recv_len <= TORRENT_PEER_RECV_BUF_CAP);

  torrent_peer_tick(peer, peer->io, peer->io->monotonic_ns(peer->io));
}

// Put our own handshake in the send buffer. 68 bytes, the same shape as the one
// a peer sends us.
static void torrent_peer_queue_handshake(TorrentPeer *peer) {
  torrent_peer_assert_invariants(peer);
  // Nothing has been queued before it, so it goes out first, which is what a
  // peer waits for before saying anything itself.
  assert(0 == peer->send_len);
  // `SentHandshake` and not `Initial`: the state moved when the peer decided to
  // send one, and this is the bridge carrying that decision out.
  assert(TorrentPeerStateSentHandshake == peer->state);
  _Static_assert(TORRENT_PEER_HANDSHAKE_LEN <= TORRENT_PEER_SEND_BUF_CAP,
                 "the handshake has to fit");

  u8 *const dst = peer->send_buf;
  memcpy(dst,
         "\x13"
         "BitTorrent protocol",
         20);
  // The 8 reserved bytes: no extension is claimed yet.
  memset(dst + 20, 0, 8);
  memcpy(dst + 28, peer->info_hash.data, TORRENT_INFO_HASH_LEN);
  memcpy(dst + 48, peer->id, TORRENT_PEER_ID_LEN);

  peer->send_len = TORRENT_PEER_HANDSHAKE_LEN;

  // What goes out has to pass the same check a peer's does, or no peer will
  // answer it.
  Bytes queued_peer_id = {0};
  assert(torrent_check_handshake(
      bytes_make(peer->send_buf, TORRENT_PEER_HANDSHAKE_LEN), peer->info_hash,
      &queued_peer_id));

  log(&peer->logger, LogLevelDebug, "queued handshake");
}

// Put a keep-alive in the send buffer: the length prefix on its own, four zero
// bytes with no tag behind them.
static void torrent_peer_queue_keep_alive(TorrentPeer *peer) {
  torrent_peer_assert_invariants(peer);
  _Static_assert(sizeof(u32) <= TORRENT_PEER_SEND_BUF_CAP,
                 "a keep-alive has to fit");

  // A send buffer this full means there are already bytes waiting to go out,
  // which is the opposite of the silence a keep-alive is there to break: not
  // sending one says nothing untrue, and the deadline has moved on either way.
  if (TORRENT_PEER_SEND_BUF_CAP - peer->send_len < sizeof(u32)) {
    log(&peer->logger, LogLevelDebug,
        "dropped a keep-alive: %zu byte(s) already queued", peer->send_len);
    return;
  }

  memset(peer->send_buf + peer->send_len, 0, sizeof(u32));
  peer->send_len += sizeof(u32);

  log(&peer->logger, LogLevelDebug, "queued keep-alive");
}

// Put a message that is a tag and nothing else in the send buffer: the length
// prefix saying one byte, then the tag. `choke`, `unchoke`, `interested` and
// `uninterested` are all this shape, so they are all this function.
static void torrent_peer_queue_msg_empty(TorrentPeer *peer,
                                         const TorrentMessageKind kind) {
  torrent_peer_assert_invariants(peer);
  // Only the four tags that carry nothing. Every other message has a payload
  // and a length that has to count it.
  assert(TorrentMessageKindChoke == kind || TorrentMessageKindUnchoke == kind ||
         TorrentMessageKindInterested == kind ||
         TorrentMessageKindUninterested == kind);
  _Static_assert(TORRENT_PEER_MSG_EMPTY_LEN <= TORRENT_PEER_SEND_BUF_CAP,
                 "an empty message has to fit");

  const usize send_len_before = peer->send_len;

  // The whole message or none of it: half a message on the wire is a length
  // prefix the peer would read the next message as the body of.
  if (TORRENT_PEER_SEND_BUF_CAP - peer->send_len < TORRENT_PEER_MSG_EMPTY_LEN) {
    log(&peer->logger, LogLevelDebug, "dropped a message: kind=%s send_len=%zu",
        torrent_message_kind_to_cstr(kind), peer->send_len);
    return;
  }

  // The length counts the tag, and the tag is all there is.
  u8_write_u32_be(peer->send_buf + peer->send_len, TORRENT_PEER_MSG_EMPTY_SIZE);
  peer->send_len += sizeof(u32);

  peer->send_buf[peer->send_len++] = (u8)kind;

  log(&peer->logger, LogLevelDebug, "queued a message: kind=%s send_len=%zu",
      torrent_message_kind_to_cstr(kind), peer->send_len);

  assert(send_len_before + TORRENT_PEER_MSG_EMPTY_LEN == peer->send_len);
  assert(peer->send_len <= TORRENT_PEER_SEND_BUF_CAP);
}

// Hand one event to the peer and carry out whatever it asks for. This is the
// whole of the bridge's write side: every command turns into bytes in the send
// buffer or into a hang-up, and nothing else here decides anything.
//
// `now_ns` is passed in and not read here, so that everything one turn of the
// loop does happens at the one instant. The walk that finds due peers compares
// against it, and a second read a few instructions later would let a peer be
// called about a deadline that, by that clock, has not passed.
//
// Answers false once the connection is over, in which case the slot may already
// be back in the pool and `peer` is not to be touched again.
__attribute__((warn_unused_result)) static bool
torrent_peer_dispatch(TorrentPeer *peer, const TorrentEvent event,
                      const u64 now_ns) {
  torrent_peer_assert_invariants(peer);
  assert(!peer->closing);

  log(&peer->logger, LogLevelDebug, "%s in %s",
      torrent_event_kind_to_cstr(event.kind),
      torrent_peer_state_to_cstr(peer->state));

  const u32 commands_len = torrent_peer_run(peer, event, now_ns);
  assert(commands_len <= TORRENT_PEER_COMMANDS_MAX);

  for (u32 i = 0; i < commands_len; i++) {
    const TorrentCommand command = peer->commands[i];

    log(&peer->logger, LogLevelDebug, "command %s",
        torrent_command_kind_to_cstr(command.kind));

    switch (command.kind) {
    case TorrentCommandKindSendHandshake:
      torrent_peer_queue_handshake(peer);
      break;

    case TorrentCommandKindSendKeepAlive:
      torrent_peer_queue_keep_alive(peer);
      break;

    case TorrentCommandKindInterested:
      torrent_peer_queue_msg_empty(peer, TorrentMessageKindInterested);
      break;

    case TorrentCommandKindUnchoke:
      torrent_peer_queue_msg_empty(peer, TorrentMessageKindUnchoke);
      break;

    case TorrentCommandKindClose:
      // Nothing follows it, which `torrent_peer_command_push` keeps true, so
      // this is also the last time `peer` is looked at.
      assert(i + 1 == commands_len);
      torrent_peer_close(peer);
      return false;

    case TorrentCommandKindNone:
      // `torrent_peer_command_push` refuses one, so this is unreachable and is
      // here because `-Wswitch-enum` wants every value named -- which is the
      // point: a command added without a case here fails to build.
      assert(0 && "a command that says nothing");
      break;
    }
  }

  return true;
}

// The two operations, in the one place each is asked for. Both are guarded by
// what is already in flight, because a completion holds one operation at a time
// and every path through the bridge ends up here.
//
// A read is never a command: one is kept outstanding whatever the state, so no
// state can stall the connection by forgetting to ask for one.
//
// Like `torrent_peer_dispatch`, this can end the connection -- an operation
// that cannot even be submitted has no callback coming -- so `peer` is not to
// be touched afterwards.
static void torrent_peer_pump(TorrentPeer *peer, IO *io) {
  torrent_peer_assert_invariants(peer);
  assert(io == peer->io);
  assert(!peer->closing);

  if (peer->send_len > 0 && !peer->write_in_flight) {
    const Error err = torrent_peer_write(peer, io);
    if (ErrKindNone != err.kind) {
      log_err(&peer->logger, "failed to write to a peer", err);
      torrent_peer_close(peer);
      return;
    }
  }

  // Nothing more to be had from what has arrived, so wait for more of it. The
  // buffer cannot already be full: a message too big for it is refused as
  // malformed, and everything smaller has been taken out of it.
  assert(peer->recv_len < TORRENT_PEER_RECV_BUF_CAP);
  if (!peer->read_in_flight) {
    const Error err = torrent_peer_read(peer, io);
    if (ErrKindNone != err.kind) {
      log_err(&peer->logger, "failed to read from a peer", err);
      torrent_peer_close(peer);
      return;
    }
  }
}

// The bridge's read side: whatever has arrived, turned into events one at a
// time, and then a read and a write put out for what comes next. Run from
// whichever callback has just reported.
static void torrent_peer_tick(TorrentPeer *peer, IO *io, const u64 now_ns) {
  torrent_peer_assert_invariants(peer);
  assert(io == peer->io);
  assert(!peer->closing);

  log(&peer->logger, LogLevelDebug, "tick in %s",
      torrent_peer_state_to_cstr(peer->state));

  // Their handshake, once the whole of it is here. Exactly the 68 bytes and not
  // a byte more: a peer is free to put its first message in the same packet,
  // and those bytes belong to the drain below.
  if (TorrentPeerStateSentHandshake == peer->state &&
      peer->recv_len >= TORRENT_PEER_HANDSHAKE_LEN) {
    const Bytes recv = bytes_make(peer->recv_buf, TORRENT_PEER_HANDSHAKE_LEN);

    Bytes peer_id = {0};
    const bool valid = torrent_check_handshake(recv, peer->info_hash, &peer_id);

    if (valid) {
      assert(TORRENT_PEER_ID_LEN == peer_id.len);
      log(&peer->logger, LogLevelInfo, "received valid handshake");

      // Dropped before the event goes out, moving whatever arrived behind it to
      // the front: what the peer decides must never be about bytes that are
      // still sitting here.
      const usize remaining = peer->recv_len - TORRENT_PEER_HANDSHAKE_LEN;
      assert(remaining < TORRENT_PEER_RECV_BUF_CAP);
      memmove(peer->recv_buf, peer->recv_buf + TORRENT_PEER_HANDSHAKE_LEN,
              remaining);
      peer->recv_len = remaining;
    } else {
      log(&peer->logger, LogLevelError, "received invalid handshake");
    }

    const TorrentEvent event = {.kind = valid ? TorrentEventKindHandshake
                                              : TorrentEventKindMalformed};
    if (!torrent_peer_dispatch(peer, event, now_ns)) {
      return;
    }
  }

  if (TorrentPeerStateHandshaked == peer->state) {
    // Every whole message in the buffer, not just the first: one read can carry
    // several, and a peer that sent three and then went quiet would otherwise
    // have two of them sitting unread for as long as it stayed quiet.
    // Bounded, and not `for (;;)`: a full buffer holds at most
    // `TORRENT_PEER_MSGS_PER_BUF_MAX` messages, so one more pass than that
    // finds nothing left. A loop that could run longer than its own input is a
    // loop that can run for ever.
    bool drained = false;
    for (usize i = 0; i < TORRENT_PEER_MSGS_PER_BUF_MAX + 1; i++) {
      const usize recv_len_before = peer->recv_len;

      Bytes recv = bytes_make(peer->recv_buf, peer->recv_len);
      TorrentPeerMessage msg = {0};

      const Error err = torrent_peer_parse_message(&recv, &msg);
      if (ErrKindNone != err.kind) {
        log(&peer->logger, LogLevelError, "received invalid message");
        log_err(&peer->logger, "failed to parse a peer message", err);
        // `Malformed` is a `Close` in every state, so there is nothing left
        // to do here whatever comes back.
        const bool alive = torrent_peer_dispatch(
            peer, (TorrentEvent){.kind = TorrentEventKindMalformed}, now_ns);
        assert(!alive);
        return;
      }

      // What is left is the start of a message that has not all arrived, and it
      // stays where it is until the rest of it does. This is the only way out
      // of the loop.
      if (TorrentMessageKindNone == msg.kind) {
        assert(recv.len == recv_len_before);
        log(&peer->logger, LogLevelDebug,
            "no whole message, %zu byte(s) buffered", peer->recv_len);
        drained = true;
        break;
      }

      // Drop what was parsed or skipped, keeping the rest at the front of the
      // buffer, and before the event goes out for the same reason the handshake
      // is dropped before its own.
      const usize consumed = recv_len_before - recv.len;
      memmove(peer->recv_buf, peer->recv_buf + consumed, recv.len);
      peer->recv_len = recv.len;

      // The buffer is strictly shorter than it was, which is what keeps this
      // loop finite: a pass that took bytes out of `recv` without taking
      // them out of the buffer would run here for ever on the same bytes.
      assert(peer->recv_len < recv_len_before);

      // Skipped, and no event: stepping over it is the bridge's own business,
      // which is what keeps a `Message` event a message the peer understood.
      //
      // Nor does it count as the peer being alive. Four minutes of nothing this
      // build can read is a peer there is nothing to be had from, whatever it
      // is saying, and the slot is better spent on one that speaks this
      // protocol.
      if (TorrentMessageKindUnknown == msg.kind) {
        log(&peer->logger, LogLevelError,
            "skipped message with unknown tag: tag=%u len=%u",
            msg.v.unknown.tag, msg.v.unknown.size);
        continue;
      }

      log(&peer->logger, LogLevelDebug, "received %s",
          torrent_message_kind_to_cstr(msg.kind));

      // No message closes a connection yet, so this way out is not taken
      // today. It is the contract all the same: the moment a message can be
      // answered with a `Close` -- a `piece` that does not hash, a `request`
      // for a piece this does not have -- carrying on round the loop here would
      // be a use of a slot that is back in the pool.
      const TorrentEvent event = {.kind = TorrentEventKindMessage,
                                  .v.msg = msg};
      if (!torrent_peer_dispatch(peer, event, now_ns)) {
        return;
      }
    }

    // Every pass took at least four bytes out, so the bound above cannot be
    // reached with a message still in the buffer.
    assert(drained);
  }

  torrent_peer_pump(peer, io);
}

// Point a freshly acquired slot at a connection. Its own function because the
// listener is not the only way a connection starts: an outgoing `connect` lands
// in the same state, waiting for the same handshake.
static void torrent_peer_init(TorrentPeer *peer, IO *io,
                              TorrentNetworkCtx *network_ctx, Ipv4Addr addr,
                              i32 socket, u32 ip) {
  assert(peer);
  assert(io);
  assert(network_ctx);
  assert(TORRENT_INFO_HASH_LEN == network_ctx->info_hash.len);
  assert(socket >= 0);
  // A slot straight from the pool, so the state machine is at its start.
  assert(TorrentPeerStateInitial == peer->state);
  assert(0 == peer->recv_len);

  peer->cb_ctx = network_ctx;
  peer->addr = addr;
  peer->socket = socket;
  peer->io = io;
  peer->network_ctx = network_ctx;
  peer->completion_read.ctx = peer;
  peer->completion_write.ctx = peer;
  peer->completion_close.ctx = peer;
  peer->info_hash = network_ctx->info_hash;
  // TODO: a real peer id, generated once for the process. Two peers sharing one
  // makes a remote think it has connected to itself.
  peer->id[0] = 1;

  // No deadline until the first `torrent_peer_run`, which is the `Accepted` or
  // `Connected` event right behind this call. A zeroed slot would read as a
  // deadline in the distant past and be woken at once, for ever.
  peer->idle_due_at = TORRENT_PEER_DEADLINE_NONE;
  peer->keep_alive_due_at = TORRENT_PEER_DEADLINE_NONE;
  peer->deadline_at = TORRENT_PEER_DEADLINE_NONE;

  char log_prefix[32] = {0};
  snprintf(log_prefix, sizeof(log_prefix), "[peer %u.%u.%u.%u:%hu] ",
           ip >> 24 & 0xff, ip >> 16 & 0xff, ip >> 8 & 0xff, ip >> 0 & 0xff,
           addr.port);

  peer->logger =
      logger_make(network_ctx->log_level_mask, bytes_from_cstr(log_prefix));

  log(&peer->logger, LogLevelDebug, "init");
}

static void torrent_peer_on_accept(IO *io, void *vctx, Ipv4Addr accept_addr,
                                   i32 accept_socket) {
  assert(io);
  assert(vctx);
  TorrentNetworkCtx *const network_ctx = vctx;
  assert(TORRENT_INFO_HASH_LEN == network_ctx->info_hash.len);

  const u32 ip = accept_addr.ip;

  TorrentPeer *const peer = torrent_peer_ctx_pool_acquire(&network_ctx->pool);
  if (!peer) {
    fprintf(stderr, "backpressure: no available pool slot for peer\n");
    // Refused, so there is no slot to keep a completion in and nothing waiting
    // on the answer: this is the hang-up `Env` is for.
    (void)io->env->close_socket(io->env, accept_socket);
    return;
  }

  torrent_peer_init(peer, io, network_ctx, accept_addr, accept_socket, ip);
  log(&peer->logger, LogLevelInfo, "accepted");

  // The transport having a connection is the first thing the peer is told, and
  // its answer is the handshake. The pump puts that on the wire and asks for
  // the read that waits for theirs; it hangs up on the peer itself if either
  // cannot even be submitted, so there is nothing left to do here.
  //
  // `Accepted` in the initial state is never answered with a `Close`, so this
  // way out is not taken today either; it is here for the same reason as the
  // one in the drain above.
  if (!torrent_peer_dispatch(peer,
                             (TorrentEvent){.kind = TorrentEventKindAccepted},
                             io->monotonic_ns(io))) {
    return;
  }

  torrent_peer_pump(peer, io);
}

// Give every peer whose deadline has passed a `Deadline` event, and answer with
// the earliest deadline still outstanding across the pool, which is how long
// the loop may wait before coming back here.
//
// This is the whole of what a timer library would be, and it is a walk over the
// pool's occupied bitset rather than a heap: what it compares are integers, and
// only a peer that is actually due is called, so a quiet turn costs a few
// hundred comparisons and no calls at all. In exchange there is no structure to
// keep in sync -- re-arming is one assignment inside `torrent_peer_run`, and a
// closing peer leaves nothing behind.
//
// TODO: a 128-bucket one-second wheel (`u16 head[128]`, `u16 next_in_bucket` on
// the peer) makes the whole of this O(1), and the deadlines here are coarse and
// bounded enough that it needs no overflow list. What it costs is cancellation
// coming back: a re-arm has to move buckets and a closing peer leaves an entry
// behind. Worth it somewhere past a few thousand peers, and not before.
__attribute__((warn_unused_result)) static u64
torrent_peers_deadlines_run(TorrentNetworkCtx *network_ctx, IO *io,
                            const u64 now_ns) {
  assert(network_ctx);
  assert(io);

  TorrentpeerHandleCtxPool *const pool = &network_ctx->pool;

  u64 earliest = TORRENT_PEER_DEADLINE_NONE;

  for (usize group_idx = 0; group_idx < POOL_SLOT_GROUPS; group_idx++) {
    // The group's word is re-read every pass rather than taken once, with the
    // bits already visited masked off: this walk is what calls into the code
    // that ends connections, so a slot can go while it is in progress. The mask
    // is also what bounds the loop, every pass clearing one bit.
    PoolSlotGroup visited = 0;

    for (usize i = 0; i < POOL_SLOTS_PER_GROUP; i++) {
      const PoolSlotGroup group = pool->occupied[group_idx] & ~visited;
      // A group with nothing left in it costs this one test, which is the whole
      // reason the pool's occupancy is a bitset.
      if (0 == group) {
        break;
      }

      const i32 first_set_bit = __builtin_ffsll((i64)group);
      assert(0 != first_set_bit);

      const u32 bit_idx = (u32)(first_set_bit - 1);
      visited |= 1ULL << bit_idx;

      TorrentPeer *const peer =
          &pool->slots[group_idx * POOL_SLOTS_PER_GROUP + bit_idx];

      // Its hang-up is waiting on an operation to report, so it is past being
      // told anything and there is no point waiting on it either.
      if (peer->closing) {
        continue;
      }

      u64 due_at = peer->deadline_at;

      if (now_ns >= due_at) {
        if (!torrent_peer_dispatch(
                peer, (TorrentEvent){.kind = TorrentEventKindDeadline},
                now_ns)) {
          continue;
        }

        // Nothing is left due: a deadline that fired was either re-armed or
        // ended the connection, which is what keeps the wait above zero.
        due_at = peer->deadline_at;
        assert(due_at > now_ns);

        // Read before the pump and not after, because the pump can end the
        // connection too -- an operation that cannot even be submitted has no
        // callback coming -- and a released slot is a zeroed one whose deadline
        // reads as long past. Waking once more for a peer that has gone costs
        // one walk; taking that zero as the minimum would spin.
        torrent_peer_pump(peer, io);
      }

      if (due_at < earliest) {
        earliest = due_at;
      }
    }
  }

  return earliest;
}

// How long the loop may wait before the nearest deadline, given what the walk
// above answered. A ceiling and not a period: a turn ends early for whichever
// deadline is nearest, and `TORRENT_PEER_LOOP_WAIT_NS_MAX` is only how long to
// wait when there is no deadline at all.
__attribute__((warn_unused_result)) static usize
torrent_peers_wait_ns(const u64 earliest, const u64 now_ns) {
  // The walk leaves nothing due behind it.
  assert(earliest > now_ns);

  const u64 until_ns = earliest - now_ns;
  if (until_ns >= TORRENT_PEER_LOOP_WAIT_NS_MAX) {
    return (usize)TORRENT_PEER_LOOP_WAIT_NS_MAX;
  }

  return (usize)until_ns;
}

__attribute__((warn_unused_result)) static Error
torrent_make_udp_broadcast_message(Bytes url, u16 port, Bytes info_hash,
                                   Arena *arena, Bytes *dst) {
  assert(arena);
  assert(dst);

  BytesBuffer bb = {0};
  Error err = bytes_buffer_make(128 + url.len, arena, &bb);
  if (ErrKindNone != err.kind) {
    return err;
  }

  assert(bytes_buffer_extend_within_cap(
      &bb, bytes_from_cstr("BT-SEARCH * HTTP/1.1\r\n"
                           "Host: ")));
  assert(bytes_buffer_extend_within_cap(&bb, url));
  assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("\r\n"
                                                             "Port: ")));

  assert(bytes_buffer_append_usize_within_cap(&bb, port));
  assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("\r\n"
                                                             "Infohash: ")));

  assert(bytes_buffer_extend_within_cap(&bb, info_hash));

  // Three CRLFs after the cookie value, not two: the one that ends the header
  // line, then the blank line that ends the block, then one more. That is what
  // libtorrent 2.1 puts on the wire, captured from the group:
  //
  //   ...Infohash: 363b69d6...\r\ncookie: 58eac522\r\n\r\n\r\n
  assert(bytes_buffer_extend_within_cap(&bb, bytes_from_cstr("\r\n"
                                                             "cookie: fixme\r\n"
                                                             "\r\n"
                                                             "\r\n")));

  *dst = bytes_take(bb.container, bb.len);

  return (Error){.kind = ErrKindNone};
}

__attribute__((warn_unused_result)) static BencodeValue *
torrent_find_info_dict_in_metainfo(BencodeValue metainfo) {
  for (usize i = 1; i < metainfo.v.list.len; i += 2) {
    const BencodeValue k = metainfo.v.list.data[i - 1];
    BencodeValue *const v = &metainfo.v.list.data[i];
    if (BencodeKindBytes == k.kind && bytes_eq_cstr(k.v.bytes, "info") &&
        BencodeKindDict == v->kind) {
      return v;
    }
  }
  return NULL;
}

__attribute__((warn_unused_result)) static Error
torrent_validate_info_dict(BencodeValue info_dict) {
  assert(BencodeKindDict == info_dict.kind);

  const BencodeList l = info_dict.v.list;

  if (0 == l.len) {
    return (Error){.kind = ErrKindInvalidData};
  }

  if (0 != l.len % 2) {
    return (Error){.kind = ErrKindInvalidData};
  }

  for (usize i = 0; i < l.len; i += 2) {
    const BencodeValue k = l.data[i];

    if (BencodeKindBytes != k.kind) {
      return (Error){.kind = ErrKindInvalidData};
    }

    // `i > 0`, not `i > 2`: `i` steps in twos, so `i == 2` is the first
    // adjacent pair of keys and skipping it would let `d1:b..1:a..e` through.
    if (i > 0) {
      const BencodeValue prev_k = l.data[i - 2];
      assert(BencodeKindBytes == prev_k.kind);

      if (bytes_cmp(prev_k.v.bytes, k.v.bytes) >= 0) {
        return (Error){.kind = ErrKindInvalidData};
      }
    }
  }

  return (Error){.kind = ErrKindNone};
}
