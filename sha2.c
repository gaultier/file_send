#pragma once

#include "lib.c"

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
__attribute__((warn_unused_result)) static u32 u32_rotate_right(u32 x,
                                                                u32 count) {
  assert(count >= 1);
  assert(count <= 31);

  return (x >> count) | (x << (32 - count));
}

__attribute__((warn_unused_result)) static u32
u32_from_bytes_be(const u8 *data) {
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

  // Unlike the x86 extension, the ARM one keeps the working variables in
  // their natural order, so the state needs no shuffling on the way in or
  // out.
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
    // round trips through the stack, but that traffic is off the critical
    // path and hides in the shadow of the `sha256h` chain. Both ways of
    // removing it
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
// Cached because this sits on the hot path of every hash and the query is not
// free. Racing callers compute the same answer, so the race is benign.
__attribute__((warn_unused_result)) static bool sha256_neon_supported(void) {
  static i32 cached = -1;

  if (cached < 0) {
#if defined(__APPLE__)
    i32 present = 0;
    usize present_size = sizeof(present);
    const bool ok = 0 == sysctlbyname("hw.optional.arm.FEAT_SHA256", &present,
                                      &present_size, NULL, 0);
    cached = (ok && 0 != present) ? 1 : 0;
#elif defined(__linux__)
    // The kernel publishes AArch64 feature bits in the ELF auxiliary vector;
    // `getauxval` reads the copy the loader already saved, so it is not a
    // syscall. An unknown type yields 0, which falls back to the scalar path.
    const unsigned long hwcap = getauxval(AT_HWCAP);
    cached = (0 != (hwcap & HWCAP_SHA2)) ? 1 : 0;
#else
    cached = 0;
#endif
  }

  return 1 == cached;
}

#endif

// Compress `blocks_count` consecutive blocks. The implementation is chosen
// once here rather than per block, so the check stays out of the inner loop.
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

static void sha256_update(Sha256Ctx *ctx, const u8 *data, usize len) {
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

#define SHA256_DIGEST_LENGTH 32

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
__attribute__((unused)) static void
sha256_print_hex(const u8 digest[SHA256_DIGEST_LENGTH]) {
  const u8 lut[] = "0123456789abcdef";

  for (usize i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    const u8 byte = digest[i];
    const u8 c1 = lut[byte & 15];
    const u8 c2 = lut[byte >> 4];
    printf("%c%c", c2, c1);
  }
}

static void sha256_digest_pair(const u8 left[SHA256_DIGEST_LENGTH],
                               const u8 right[SHA256_DIGEST_LENGTH],
                               u8 dst[SHA256_DIGEST_LENGTH]) {
  Sha256Ctx sha = {0};
  sha256_init(&sha);
  sha256_update(&sha, left, SHA256_DIGEST_LENGTH);
  sha256_update(&sha, right, SHA256_DIGEST_LENGTH);
  sha256_final(&sha, dst);
}
