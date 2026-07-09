#include "hashtable.h"

#include <random>

#include "clock.h"
#include "hash.h"  // seeded SipHash-1-2; see hash.h for why keyed hashing

// Bucket counts are powers of two so the index is a mask (h & (n-1)),
// avoiding the slower integer modulo.
static size_t bucket_index(uint64_t hash, size_t buckets) {
    return static_cast<size_t>(hash) & (buckets - 1);
}

static bool is_expired(const HashTable::Entry* e, long long now) {
    return e->expires_at >= 0 && e->expires_at <= now;
}

// Estimated footprint of one entry: the struct itself plus the key/value
// bytes. Deliberately size(), not capacity(): deterministic and explainable,
// at the cost of ignoring string over-allocation (Redis instead asks its
// allocator zmalloc for exact numbers).
static size_t entry_bytes(const HashTable::Entry* e) {
    return HashTable::entry_size(e->key, e->value);
}

HashTable::HashTable() {
    ht_[0].assign(INITIAL_BUCKETS, nullptr);
}

HashTable::~HashTable() {
    for (auto& table : ht_) {
        for (Entry* e : table) {
            while (e) {
                Entry* next = e->next;
                delete e;
                e = next;
            }
        }
    }
}

void HashTable::start_rehash() {
    // Doubling keeps the load factor's amortized-O(1) argument intact and
    // preserves the power-of-two invariant.
    ht_[1].assign(ht_[0].size() * 2, nullptr);
    rehash_idx_ = 0;
}

// Migrate ONE non-empty bucket from ht_[0] to ht_[1]. Called from every
// public operation, so the rehash rides along on normal traffic and no
// single command ever pays for moving the whole table (the stop-the-world
// rehash spike this design exists to avoid).
void HashTable::rehash_step() {
    if (!is_rehashing()) return;

    // Skip runs of empty buckets, but boundedly: a nearly-drained table must
    // not make one unlucky get() scan thousands of empties (Redis caps this
    // the same way, empty_visits).
    size_t empty_seen = 0;
    while (rehash_idx_ < ht_[0].size() && ht_[0][rehash_idx_] == nullptr) {
        rehash_idx_++;
        if (++empty_seen >= 10) return;
    }

    if (rehash_idx_ < ht_[0].size()) {
        // Re-insert every node of this chain into the new table. Nodes are
        // reused as-is (stable addresses); only their bucket changes.
        Entry* e = ht_[0][rehash_idx_];
        ht_[0][rehash_idx_] = nullptr;
        while (e) {
            Entry* next = e->next;
            size_t idx = bucket_index(hash_key(e->key), ht_[1].size());
            e->next = ht_[1][idx];
            ht_[1][idx] = e;
            e = next;
        }
        rehash_idx_++;
    }

    // Old table fully drained: the new table becomes THE table.
    if (rehash_idx_ >= ht_[0].size()) {
        ht_[0] = std::move(ht_[1]);
        ht_[1].clear();
        rehash_idx_ = NOT_REHASHING;
    }
}

// ---- intrusive LRU list (Phase 6) ----
// The list is threaded through the entries themselves via lru_prev/lru_next.
// All three operations are pure pointer surgery: O(1), no allocation, no
// hashing — which is why touching a key on every read is affordable.

void HashTable::lru_push_front(Entry* e) {
    e->lru_prev = nullptr;
    e->lru_next = lru_head_;
    if (lru_head_) lru_head_->lru_prev = e;
    lru_head_ = e;
    if (!lru_tail_) lru_tail_ = e;  // first entry: it is both head and tail
}

void HashTable::lru_unlink(Entry* e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else lru_head_ = e->lru_next;   // e was the head
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else lru_tail_ = e->lru_prev;   // e was the tail
    e->lru_prev = e->lru_next = nullptr;
}

void HashTable::lru_touch(Entry* e) {
    if (lru_head_ == e) return;     // already the most recent: nothing to do
    lru_unlink(e);
    lru_push_front(e);
}

void HashTable::destroy_entry(Entry* e) {
    lru_unlink(e);
    used_memory_ -= entry_bytes(e);
    used_--;
    delete e;
}

void HashTable::evict_if_needed(const Entry* just_written) {
    if (maxmemory_ == 0) return;  // unlimited
    // Evict from the LRU tail (least recently used) first. Stop at the
    // just-written entry: evicting what the caller is in the middle of
    // storing would hand back a dangling pointer.
    while (used_memory_ > maxmemory_ && lru_tail_ && lru_tail_ != just_written) {
        evicted_++;
        // COPY the key first: erase_entry is about to free the entry that
        // owns it, so passing lru_tail_->key by reference would leave the
        // parameter dangling mid-call. (erase_entry re-finds the bucket by
        // hash — O(1) average — reusing the one chain-unlink code path.)
        std::string victim = lru_tail_->key;
        erase_entry(victim);
    }
}

HashTable::Entry* HashTable::find_entry(const std::string& key) {
    uint64_t hash = hash_key(key);
    // During a rehash the key may be in either table, so check both.
    int tables = is_rehashing() ? 2 : 1;
    for (int t = 0; t < tables; t++) {
        size_t idx = bucket_index(hash, ht_[t].size());
        for (Entry* e = ht_[t][idx]; e; e = e->next) {
            if (e->key == key) return e;
        }
    }
    return nullptr;
}

bool HashTable::erase_entry(const std::string& key) {
    uint64_t hash = hash_key(key);
    int tables = is_rehashing() ? 2 : 1;
    for (int t = 0; t < tables; t++) {
        size_t idx = bucket_index(hash, ht_[t].size());
        // Walk with a pointer-to-pointer so unlinking the head and unlinking
        // a middle node are the same code path (no "previous node" special case).
        Entry** link = &ht_[t][idx];
        while (*link) {
            Entry* e = *link;
            if (e->key == key) {
                *link = e->next;       // chain unlink (we hold the position)
                destroy_entry(e);      // LRU unlink + memory accounting + free
                return true;
            }
            link = &e->next;
        }
    }
    return false;
}

HashTable::Entry* HashTable::find(const std::string& key) {
    rehash_step();
    Entry* e = find_entry(key);
    if (!e) return nullptr;
    // Lazy expiry: a logically-dead key is reaped the moment anything
    // touches it, and reported as absent — the caller can't tell the
    // difference between "expired just now" and "never existed" (correct:
    // semantically both are misses).
    if (is_expired(e, now_ms())) {
        expired_++;
        erase_entry(key);
        return nullptr;
    }
    // Every live access counts as a "use": move to the front of the LRU
    // list so the tail is always the genuinely least-recently-used key.
    lru_touch(e);
    return e;
}

bool HashTable::set(const std::string& key, const std::string& value) {
    // find() runs the rehash step and lazily reaps an expired old version,
    // so an overwrite of a dead key correctly behaves as a fresh insert.
    // (It also LRU-touches the entry: an overwrite is a "use".)
    if (Entry* existing = find(key)) {
        used_memory_ -= existing->value.size();  // re-account the value bytes
        existing->value = value;
        used_memory_ += existing->value.size();
        existing->expires_at = -1;  // Redis semantics: plain SET clears TTL
        evict_if_needed(existing);  // a larger value can push us over budget
        return false;
    }

    // Trigger growth at load factor 1.0 (one entry per bucket on average).
    // Chains stay short on average; the table doubles before they degrade.
    if (!is_rehashing() && used_ >= ht_[0].size()) {
        start_rehash();
    }

    // New keys go into the NEWEST table so ht_[0] only ever drains during a
    // rehash — otherwise the migration could chase a moving target forever.
    std::vector<Entry*>& table = is_rehashing() ? ht_[1] : ht_[0];
    size_t idx = bucket_index(hash_key(key), table.size());
    Entry* e = new Entry{key, value, table[idx]};  // push at chain head: O(1)
    table[idx] = e;
    used_++;
    used_memory_ += entry_bytes(e);
    lru_push_front(e);        // brand-new key = most recently used
    evict_if_needed(e);       // over budget? evict from the tail, not e
    return true;
}

const std::string* HashTable::get(const std::string& key) {
    Entry* e = find(key);
    return e ? &e->value : nullptr;
}

bool HashTable::erase(const std::string& key) {
    // find() first: an expired key is reaped there and reads as absent, so
    // DEL on a dead key correctly reports 0 (Redis behaves the same).
    if (!find(key)) return false;
    return erase_entry(key);
}

std::vector<std::string> HashTable::keys() const {
    // Expired-but-unreaped entries are SKIPPED, not reaped: this is const,
    // and a debug command shouldn't mutate the store. They die via the
    // normal lazy/active paths.
    std::vector<std::string> out;
    out.reserve(used_);
    long long now = now_ms();
    for (const auto& table : ht_) {
        for (Entry* e : table) {
            for (; e; e = e->next) {
                if (!is_expired(e, now)) out.push_back(e->key);
            }
        }
    }
    return out;
}

void HashTable::clear() {
    for (auto& table : ht_) {
        for (Entry*& head : table) {
            while (head) {
                Entry* next = head->next;
                delete head;  // wholesale: no per-entry unlink bookkeeping
                head = next;
            }
        }
    }
    // Back to day-one shape: small table, no rehash in flight, empty LRU.
    ht_[0].assign(INITIAL_BUCKETS, nullptr);
    ht_[1].clear();
    rehash_idx_ = NOT_REHASHING;
    used_ = 0;
    used_memory_ = 0;
    lru_head_ = lru_tail_ = nullptr;
}

int HashTable::expire_cycle(long long now) {
    if (used_ == 0) return 0;

    // Idle servers still make progress on a pending rehash: without traffic,
    // ops-piggybacked rehashing stalls; the tick un-stalls it.
    rehash_step();

    static std::mt19937 rng{std::random_device{}()};

    int total_reaped = 0;
    // Adaptive rounds, strictly capped: a mostly-dead sample means many more
    // corpses likely remain, so sweep harder — but never unboundedly, the
    // event loop is waiting.
    for (int round = 0; round < 16; round++) {
        int checked = 0;
        int reaped = 0;

        for (int b = 0; b < 20; b++) {
            // Pick one random bucket across both tables (mid-rehash entries
            // live in either).
            size_t total_buckets =
                ht_[0].size() + (is_rehashing() ? ht_[1].size() : 0);
            size_t r = std::uniform_int_distribution<size_t>(
                0, total_buckets - 1)(rng);
            std::vector<Entry*>& table = (r < ht_[0].size()) ? ht_[0] : ht_[1];
            size_t idx = (r < ht_[0].size()) ? r : r - ht_[0].size();

            // Sweep the whole chain of that bucket (chains average ~1 entry
            // at our load factor, so this stays cheap).
            Entry** link = &table[idx];
            while (*link) {
                Entry* e = *link;
                checked++;
                if (is_expired(e, now)) {
                    *link = e->next;   // chain unlink
                    destroy_entry(e);  // LRU unlink + memory accounting + free
                    expired_++;
                    reaped++;
                } else {
                    link = &e->next;
                }
            }
        }

        total_reaped += reaped;
        // <25% of sampled entries were dead (or nothing sampled): table is
        // clean enough; stop until the next tick.
        if (checked == 0 || reaped * 4 < checked) break;
    }
    return total_reaped;
}
