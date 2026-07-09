#include "hash.h"

#include <ctime>
#include <fcntl.h>
#include <unistd.h>

static inline uint64_t rotl64(uint64_t x, int bits) {
    return (x << bits) | (x >> (64 - bits));
}

// SipHash consumes the input as little-endian 64-bit words. Assemble bytes
// explicitly instead of casting the pointer, so the result is identical on
// any CPU endianness and unaligned reads are never an issue.
static inline uint64_t read_u64_le(const unsigned char* p) {
    return (uint64_t)p[0]       | (uint64_t)p[1] << 8  |
           (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24 |
           (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
           (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

// The ARX (add-rotate-xor) mixing step. The rotation amounts are the
// published constants from the SipHash paper — vetted by cryptanalysis,
// not something to tweak or re-derive.
static inline void sipround(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
    v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
    v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);
}

uint64_t siphash(const void* data, size_t len,
                 uint64_t k0, uint64_t k1,
                 int crounds, int drounds) {
    const unsigned char* in = static_cast<const unsigned char*>(data);

    // Internal state: four lanes seeded by the secret key XORed with
    // fixed constants (ASCII of "somepseudorandomlygeneratedbytes").
    uint64_t v0 = 0x736f6d6570736575ull ^ k0;
    uint64_t v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0;
    uint64_t v3 = 0x7465646279746573ull ^ k1;

    // Absorb the input, one full 8-byte block at a time.
    size_t full_blocks_len = len - (len % 8);
    const unsigned char* end = in + full_blocks_len;
    for (; in != end; in += 8) {
        uint64_t m = read_u64_le(in);
        v3 ^= m;
        for (int i = 0; i < crounds; i++) sipround(v0, v1, v2, v3);
        v0 ^= m;
    }

    // Last block: the 0-7 leftover bytes, plus the input length in the top
    // byte — so "ab" + "" and "a" + "b" style ambiguities can't collide.
    uint64_t b = (uint64_t)(len & 0xff) << 56;
    for (size_t i = 0; i < (len % 8); i++) {
        b |= (uint64_t)in[i] << (8 * i);
    }
    v3 ^= b;
    for (int i = 0; i < crounds; i++) sipround(v0, v1, v2, v3);
    v0 ^= b;

    // Finalization: flip a constant into the state, extra rounds, fold the
    // four lanes into one 64-bit result.
    v2 ^= 0xff;
    for (int i = 0; i < drounds; i++) sipround(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

namespace {

struct Seed {
    uint64_t k0;
    uint64_t k1;
};

Seed make_seed() {
    Seed s;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, &s, sizeof s);
        close(fd);
        if (n == (ssize_t)sizeof s) return s;
    }
    // /dev/urandom exists on every Linux/macOS box; this fallback is for
    // exotic environments only. Weaker (guessable) seed, but the server
    // still runs — and a weak seed merely degrades us back to FNV-1a-level
    // predictability, never to incorrectness.
    s.k0 = (uint64_t)time(nullptr) * 0x9e3779b97f4a7c15ull;
    s.k1 = ((uint64_t)getpid() << 32) ^ (uint64_t)clock();
    return s;
}

}  // namespace

uint64_t hash_key(const std::string& key) {
    // Seeded once per process, on first use. A restart rerolls the seed,
    // which also means bucket placement differs between runs — nothing may
    // depend on hash values being stable across processes.
    static const Seed seed = make_seed();
    // 1-2 rounds, not the reference 2-4: Redis's choice. We need flood
    // resistance (unpredictability), not full MAC-grade security margin,
    // and hashing sits on the hot path of every single command.
    return siphash(key.data(), key.size(), seed.k0, seed.k1, 1, 2);
}
