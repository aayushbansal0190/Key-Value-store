#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Core SipHash (Aumasson & Bernstein), round counts as parameters:
// crounds mixing rounds per 8-byte block, drounds finalization rounds.
// Parameterized so tests can run the reference SipHash-2-4 against the
// official published vectors, while production uses the faster 1-2.
uint64_t siphash(const void* data, size_t len,
                 uint64_t k0, uint64_t k1,
                 int crounds, int drounds);

// The store's hash: SipHash-1-2 (Redis's variant since 4.0) keyed with a
// process-wide 128-bit secret seed drawn from /dev/urandom at first use.
// Keyed hashing is our hash-flooding defense: without the seed, an attacker
// cannot precompute keys that collide into one bucket. A restart rerolls
// the seed. (Predecessor in this repo was FNV-1a; swapped after deciding
// user-influenced key names must be survivable.)
uint64_t hash_key(const std::string& key);
