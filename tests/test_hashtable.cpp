#include "../src/clock.h"
#include "../src/hashtable.h"
#include "test.h"

#include <string>
#include <unordered_map>  // allowed in tests: used as the reference "truth"
#include <vector>

static void test_basic_set_get() {
    HashTable ht;
    CHECK_EQ(ht.size(), 0u);
    CHECK(ht.get("missing") == nullptr);

    CHECK(ht.set("name", "aayush"));          // new key -> true
    CHECK_EQ(ht.size(), 1u);
    const std::string* v = ht.get("name");
    CHECK(v && *v == "aayush");

    CHECK(!ht.set("name", "bansal"));         // overwrite -> false
    CHECK_EQ(ht.size(), 1u);                  // overwrite must not grow size
    v = ht.get("name");
    CHECK(v && *v == "bansal");
}

static void test_erase() {
    HashTable ht;
    ht.set("a", "1");
    ht.set("b", "2");

    CHECK(ht.erase("a"));
    CHECK_EQ(ht.size(), 1u);
    CHECK(ht.get("a") == nullptr);
    CHECK(ht.get("b") != nullptr);            // neighbor unaffected

    CHECK(!ht.erase("a"));                    // double-erase -> false
    CHECK(!ht.erase("never-existed"));
}

static void test_empty_and_binary_keys() {
    HashTable ht;
    CHECK(ht.set("", "empty-key"));           // "" is a valid key
    const std::string* v = ht.get("");
    CHECK(v && *v == "empty-key");

    std::string binary("k\0y", 3);            // embedded NUL must not truncate
    CHECK(ht.set(binary, "bin"));
    CHECK(ht.get(binary) != nullptr);
    CHECK(ht.get("k") == nullptr);            // "k" is a different key
}

// The Phase 1 milestone test: grow from 4 buckets to thousands while
// continuously verifying reads against std::unordered_map as ground truth.
static void test_resize_under_load() {
    HashTable ht;
    std::unordered_map<std::string, std::string> truth;
    const int N = 10000;

    for (int i = 0; i < N; i++) {
        std::string key = "key:" + std::to_string(i);
        std::string value = "val:" + std::to_string(i * 7);
        ht.set(key, value);
        truth[key] = value;

        // Spot-check an older key mid-growth, so reads run WHILE rehashes
        // are in flight, not just at the end.
        if (i % 97 == 0 && i > 0) {
            std::string old = "key:" + std::to_string(i / 2);
            const std::string* v = ht.get(old);
            CHECK(v && *v == truth[old]);
        }
    }

    CHECK_EQ(ht.size(), truth.size());
    CHECK(ht.bucket_count() >= 4096);         // must have grown far past 4

    int mismatches = 0;
    for (const auto& [key, value] : truth) {
        const std::string* v = ht.get(key);
        if (!v || *v != value) mismatches++;
    }
    CHECK_EQ(mismatches, 0);
}

// Catch a key while a rehash is actually in progress and prove both-table
// lookup, insert-into-new-table, and erase-from-either-table all behave.
static void test_ops_during_rehash() {
    HashTable ht;
    int i = 0;
    while (!ht.is_rehashing()) {              // fill until a rehash starts
        ht.set("k" + std::to_string(i), std::to_string(i));
        i++;
    }
    int filled = i;

    CHECK(ht.get("k0") != nullptr);           // old key, likely still in ht[0]
    ht.set("fresh", "new");                    // lands in ht[1]
    CHECK(ht.get("fresh") != nullptr);
    CHECK(ht.erase("k0"));                    // erase must search both tables
    CHECK(ht.get("k0") == nullptr);

    // Overwrite during rehash must update, not duplicate.
    std::string mid = "k" + std::to_string(filled / 2);
    size_t before = ht.size();
    ht.set(mid, "overwritten");
    CHECK_EQ(ht.size(), before);
    const std::string* v = ht.get(mid);
    CHECK(v && *v == "overwritten");
}

static void test_insert_erase_churn() {
    HashTable ht;
    // Interleave inserts and erases so chains form and unlink at every
    // position (head/middle/tail) across many bucket layouts.
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 2000; i++) {
            ht.set("churn:" + std::to_string(i), std::to_string(round));
        }
        for (int i = 0; i < 2000; i += 2) {
            CHECK(ht.erase("churn:" + std::to_string(i)));
        }
        for (int i = 1; i < 2000; i += 2) {
            const std::string* v = ht.get("churn:" + std::to_string(i));
            CHECK(v && *v == std::to_string(round));
        }
        for (int i = 1; i < 2000; i += 2) {
            CHECK(ht.erase("churn:" + std::to_string(i)));
        }
        CHECK_EQ(ht.size(), 0u);
    }
}

// Tests drive expiry by writing expires_at timestamps in the past — no
// sleeps needed, and the lazy path is exercised exactly as in production.
static void test_lazy_expiry() {
    HashTable ht;
    ht.set("alive", "1");
    ht.set("dead", "2");
    CHECK_EQ(ht.size(), 2u);

    ht.find("dead")->expires_at = now_ms() - 10;  // died 10ms ago

    CHECK(ht.get("dead") == nullptr);   // lazy check reports it absent...
    CHECK_EQ(ht.size(), 1u);            // ...and reaped it on the spot
    CHECK(ht.get("alive") != nullptr);  // neighbor untouched

    // DEL of an already-expired key must report "didn't exist".
    ht.set("dead2", "x");
    ht.find("dead2")->expires_at = now_ms() - 10;
    CHECK(!ht.erase("dead2"));
    CHECK_EQ(ht.size(), 1u);
}

static void test_overwrite_clears_ttl() {
    HashTable ht;
    ht.set("k", "v1");
    ht.find("k")->expires_at = now_ms() + 100000;  // 100s from now
    ht.set("k", "v2");                             // plain SET again
    CHECK(ht.find("k") != nullptr);
    CHECK_EQ(ht.find("k")->expires_at, -1ll);      // TTL gone (Redis semantics)

    // Overwriting an EXPIRED key behaves as a fresh insert, not a resurrection.
    ht.find("k")->expires_at = now_ms() - 10;
    CHECK(ht.set("k", "v3"));                      // true = treated as NEW key
    CHECK_EQ(ht.size(), 1u);
    CHECK(*ht.get("k") == "v3");
}

static void test_active_expire_cycle() {
    HashTable ht;
    // 200 keys, half of them long dead. The sweep must reap all corpses
    // (repeated cycles converge) and leave every living key untouched.
    for (int i = 0; i < 200; i++) {
        ht.set("k" + std::to_string(i), "v");
    }
    long long past = now_ms() - 1000;
    for (int i = 0; i < 200; i += 2) {
        ht.find("k" + std::to_string(i))->expires_at = past;
    }
    CHECK_EQ(ht.size(), 200u);

    // The sweep is SAMPLING — its contract is bounded work and steady
    // progress, NOT exhaustive reaping in any fixed number of cycles (a
    // corpse in a never-sampled bucket legitimately survives a while).
    // So: assert progress here, and exactness below via the lazy path.
    long long now = now_ms();
    for (int i = 0; i < 50; i++) ht.expire_cycle(now);
    CHECK(ht.size() < 200u);       // corpses reaped (statistically certain)
    CHECK(ht.size() >= 100u);      // and never a LIVE key — no over-reaping

    // Lazy expiry mops up whatever sampling missed: after touching every
    // key, the count is EXACTLY the 100 survivors.
    for (int i = 0; i < 200; i++) {
        const std::string* v = ht.get("k" + std::to_string(i));
        if (i % 2 == 0) CHECK(v == nullptr);   // dead: reaped by sweep OR just now
        else            CHECK(v != nullptr);   // survivor intact
    }
    CHECK_EQ(ht.size(), 100u);
}

static void test_used_memory_accounting() {
    HashTable ht;
    CHECK_EQ(ht.used_memory(), 0u);

    ht.set("key", "12345");  // 3 key bytes + 5 value bytes
    size_t one = ht.used_memory();
    CHECK_EQ(one, sizeof(HashTable::Entry) + 3 + 5);

    ht.set("key", "123");                        // smaller value: shrink by 2
    CHECK_EQ(ht.used_memory(), one - 2);
    ht.set("key", "1234567");                    // larger value: grow by 4
    CHECK_EQ(ht.used_memory(), one + 2);

    ht.set("key2", "x");
    CHECK_EQ(ht.used_memory(), (one + 2) + sizeof(HashTable::Entry) + 4 + 1);

    // Deleting everything must return the count to EXACTLY zero — any drift
    // here compounds forever and silently breaks eviction.
    CHECK(ht.erase("key"));
    CHECK(ht.erase("key2"));
    CHECK_EQ(ht.used_memory(), 0u);
}

static void test_lru_eviction_order() {
    HashTable ht;
    // Uniform key/value sizes so "exactly three entries fit" is arithmetic.
    ht.set("k1", "v1");
    ht.set("k2", "v2");
    ht.set("k3", "v3");
    ht.set_maxmemory(ht.used_memory());  // budget = exactly these three

    // Recency (old -> new) is k1, k2, k3. Touch k1: now k2 is the LRU.
    CHECK(ht.get("k1") != nullptr);

    ht.set("k4", "v4");                  // over budget -> evict ONE from tail
    CHECK_EQ(ht.size(), 3u);
    CHECK(ht.get("k2") == nullptr);      // the true LRU died...
    CHECK(ht.get("k1") != nullptr);      // ...not the older-but-touched k1
    CHECK(ht.get("k3") != nullptr);
    CHECK(ht.get("k4") != nullptr);
    CHECK_EQ(ht.evicted_keys(), 1u);
}

static void test_eviction_never_evicts_the_key_being_written() {
    HashTable ht;
    ht.set_maxmemory(1);  // absurd budget: nothing fits
    ht.set("big", std::string(1000, 'x'));
    // The just-written key must survive even though we're over budget —
    // evicting it would hand the caller a dangling entry. (Redis instead
    // refuses such writes with an OOM error; noted simplification.)
    CHECK_EQ(ht.size(), 1u);
    CHECK(ht.get("big") != nullptr);

    ht.set("big2", std::string(1000, 'y'));  // evicts big, keeps big2
    CHECK_EQ(ht.size(), 1u);
    CHECK(ht.get("big") == nullptr);
    CHECK(ht.get("big2") != nullptr);
}

static void test_expiry_updates_memory_and_lru() {
    HashTable ht;
    ht.set("a", "v");
    ht.set("b", "v");
    ht.set("c", "v");
    size_t full = ht.used_memory();

    // Lazy-reap b: its bytes must come back AND it must leave the LRU list.
    ht.find("b")->expires_at = now_ms() - 10;
    CHECK(ht.get("b") == nullptr);
    CHECK_EQ(ht.used_memory(), full - (sizeof(HashTable::Entry) + 2));

    // Force evictions across the whole remaining list: if the reaped entry
    // were still linked in, lru_unlink would touch freed memory here.
    ht.set_maxmemory(1);
    ht.set("d", "v");  // evicts a and c, keeps d
    CHECK_EQ(ht.size(), 1u);
    CHECK(ht.get("d") != nullptr);
}

// The stable-address claim, put on trial: incremental rehashes RELINK every
// entry into new buckets. If entries were copied (open addressing style),
// the LRU pointers would dangle; because nodes are reused, the recency list
// survives any number of rehashes untouched.
static void test_lru_survives_rehash() {
    HashTable ht;
    ht.set("first", "v");  // never touched again: stays the global LRU tail
    for (int i = 0; i < 500; i++) {
        ht.set("k" + std::to_string(i), "v");
    }
    CHECK(ht.bucket_count() >= 128);  // several rehashes definitely happened

    ht.set_maxmemory(ht.used_memory());
    ht.set("last", "v");                    // forces eviction from the tail
    CHECK(ht.get("first") == nullptr);      // and the tail is still "first"
    CHECK(ht.get("last") != nullptr);
    CHECK(ht.get("k499") != nullptr);       // recent keys untouched
    CHECK(ht.evicted_keys() >= 1u);
}

static void test_keys_lists_live_only() {
    HashTable ht;
    for (int i = 0; i < 200; i++) ht.set("k" + std::to_string(i), "v");
    CHECK_EQ(ht.keys().size(), 200u);

    // An expired key is excluded from keys() without being reaped by it
    // (keys() is const): size() still counts it until a find/sweep.
    ht.set("dead", "v");
    ht.find("dead")->expires_at = 1;  // already in the past
    auto ks = ht.keys();
    bool found_dead = false;
    for (const auto& k : ks) if (k == "dead") found_dead = true;
    CHECK(!found_dead);
    CHECK_EQ(ks.size(), 200u);
}

static void test_clear_resets_everything() {
    HashTable ht;
    for (int i = 0; i < 300; i++) ht.set("k" + std::to_string(i), "value");
    CHECK(ht.bucket_count() >= 128);  // grown well past the initial 4
    size_t evicted_before = ht.evicted_keys();

    ht.clear();
    CHECK_EQ(ht.size(), 0u);
    CHECK_EQ(ht.used_memory(), 0u);       // accounting back to zero
    CHECK(!ht.is_rehashing());            // any in-flight rehash abandoned
    CHECK_EQ(ht.bucket_count(), 4u);      // shrunk back to the initial table
    CHECK_EQ(ht.evicted_keys(), evicted_before);  // lifetime stats survive

    // Fully usable afterwards.
    ht.set("fresh", "v");
    CHECK(ht.get("fresh") != nullptr);
    CHECK_EQ(ht.keys().size(), 1u);
}

int main() {
    test_basic_set_get();
    test_erase();
    test_empty_and_binary_keys();
    test_resize_under_load();
    test_ops_during_rehash();
    test_insert_erase_churn();
    test_lazy_expiry();
    test_overwrite_clears_ttl();
    test_active_expire_cycle();
    test_used_memory_accounting();
    test_lru_eviction_order();
    test_eviction_never_evicts_the_key_being_written();
    test_expiry_updates_memory_and_lru();
    test_lru_survives_rehash();
    test_keys_lists_live_only();
    test_clear_resets_everything();
    return test_summary("test_hashtable");
}
