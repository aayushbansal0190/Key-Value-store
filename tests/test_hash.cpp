#include "../src/hash.h"
#include "test.h"

#include <cstdint>
#include <string>
#include <vector>

// Official SipHash-2-4 test vectors from the reference implementation
// (github.com/veorq/SipHash). Key bytes are 00 01 02 ... 0f, which as two
// little-endian u64s is:
static const uint64_t K0 = 0x0706050403020100ull;
static const uint64_t K1 = 0x0f0e0d0c0b0a0908ull;

// If these pass, every moving part — state init, block absorption, tail
// padding, finalization — is bit-for-bit correct. Production's 1-2 variant
// runs the exact same code with different round counts.
static void test_reference_vectors_2_4() {
    // Empty input.
    CHECK_EQ(siphash("", 0, K0, K1, 2, 4), 0x726fdb47dd0e0e31ull);

    // Single byte 0x00 (exercises the tail path alone).
    unsigned char one = 0x00;
    CHECK_EQ(siphash(&one, 1, K0, K1, 2, 4), 0x74f839c593dc67fdull);

    // 15 bytes 00..0e — the worked example in the SipHash paper
    // (one full 8-byte block + a 7-byte tail).
    unsigned char msg[15];
    for (int i = 0; i < 15; i++) msg[i] = (unsigned char)i;
    CHECK_EQ(siphash(msg, 15, K0, K1, 2, 4), 0xa129ca6149be45e5ull);
}

static void test_deterministic_within_process() {
    // Same key must hash identically every time (or the table breaks).
    CHECK_EQ(hash_key("user:42"), hash_key("user:42"));
    CHECK_EQ(hash_key(""), hash_key(""));

    std::string binary("k\0y", 3);
    CHECK_EQ(hash_key(binary), hash_key(binary));
    CHECK(hash_key(binary) != hash_key("k"));  // embedded NUL not truncated
}

static void test_seed_changes_output() {
    // The whole point of keyed hashing: a different seed must place keys
    // differently, so collisions can't be precomputed without the seed.
    const char* key = "user:42";
    uint64_t a = siphash(key, 7, K0, K1, 1, 2);
    uint64_t b = siphash(key, 7, K0 ^ 1, K1, 1, 2);   // flip ONE seed bit
    CHECK(a != b);
}

static void test_distribution_sanity() {
    // 10k sequential keys over 1024 buckets: expect ~10 per bucket. A gross
    // implementation bug (e.g., only low bytes mixed) shows up as huge
    // clumps. Bound of 40 is ~10x sigma above the mean — never fires on a
    // correct implementation, always fires on a broken one.
    const size_t BUCKETS = 1024;
    std::vector<int> counts(BUCKETS, 0);
    for (int i = 0; i < 10000; i++) {
        std::string key = "key:" + std::to_string(i);
        counts[hash_key(key) & (BUCKETS - 1)]++;
    }
    int max_count = 0;
    for (int c : counts) max_count = std::max(max_count, c);
    CHECK(max_count < 40);
}

int main() {
    test_reference_vectors_2_4();
    test_deterministic_within_process();
    test_seed_changes_output();
    test_distribution_sanity();
    return test_summary("test_hash");
}
