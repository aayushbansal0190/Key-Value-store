# kvstore

An in-memory key-value store written in C++17, built to learn how Redis works
by rebuilding the core of it from scratch. Single-threaded, epoll-based, no
external libraries. It speaks RESP2, so the official `redis-cli` and normal
Redis client libraries talk to it without changes.

```
$ redis-cli -p 6379 set user:1 aayush ex 100
OK
$ redis-cli -p 6379 get user:1
"aayush"
$ redis-cli -p 6379 ttl user:1
(integer) 98
```

## Features

- Custom chaining hash table with incremental resize (never blocks to rehash).
- Keys hashed with SipHash-1-2 seeded from `/dev/urandom`, so key names can't be
  chosen to force collisions.
- RESP2 protocol plus a plain-text inline mode for `telnet`/`nc`.
- Single-threaded epoll event loop: non-blocking sockets, per-connection read and
  write buffers, many concurrent clients, no locks anywhere.
- Key expiry: `SET ... EX/PX`, `EXPIRE`, `PEXPIREAT`, `TTL`, `PERSIST`. Checked
  lazily on access and swept periodically in the background.
- LRU eviction under a `--maxmemory` budget, tracked with a doubly linked list
  through the entries.
- Append-only-file persistence with `always`/`everysec`/`no` fsync policies.
  Kill the server, restart, the data (including TTLs) is still there.
- Commands: `PING ECHO SET GET DEL EXISTS EXPIRE TTL PERSIST PEXPIREAT KEYS
  FLUSHALL DBSIZE INFO`.

## Build and run

```sh
make            # builds build/kvstore
make test       # builds and runs the test suite
./build/kvstore --port 6379 --maxmemory 100mb --appendonly yes --appendfsync everysec
```

| Flag            | Default   | Meaning                                    |
|-----------------|-----------|--------------------------------------------|
| `--port`        | 6379      | TCP port                                   |
| `--maxmemory`   | 0 (off)   | byte budget (`100mb`, `1gb`); enables LRU  |
| `--appendonly`  | no        | enable AOF persistence                     |
| `--appendfsync` | everysec  | `always`, `everysec`, or `no`              |
| `--dir`         | .         | directory for `appendonly.aof`             |

The event loop uses epoll, so the server is Linux-only. On macOS I develop
against a container:

```sh
make docker-test    # build and run the tests in a Linux container
make docker-run     # run the server, port 6379 mapped to the host
make docker-bench   # run the benchmark workloads
```

## How it fits together

```
clients ──> epoll event loop ──> RESP / inline parser ──> command layer ──> hash table
                 │                                              │
          100ms tick:                                     write commands
          TTL sweep, fsync                                      │
                                                          append-only file ──> disk
```

One thread does everything. Commands run one at a time in the order their bytes
arrive, which is why the store needs no locks. On startup with `--appendonly`,
the AOF is replayed through the same parser and command layer before the server
accepts connections, so clients only ever see fully restored state.

The hash table uses chaining rather than open addressing on purpose: chain nodes
have stable addresses, so the LRU list can be threaded directly through the
entries and a resize can move nodes between buckets without invalidating
anything. Resizing is incremental, a few buckets per operation, so no single
command pays for migrating the whole table.

## Benchmarks

`bench/loadgen.cpp` is a threaded load generator (a client, so threads are fine
here). Numbers below are 50 connections, 32-byte values, measured in a container
on an M2 Mac, so bare-metal Linux would be faster. Run them with `make
docker-bench`.

| Workload           | Throughput    | p50     | p99     |
|--------------------|---------------|---------|---------|
| SET, no pipelining | ~160k ops/sec | ~270 us | ~600 us |
| SET, pipeline 16   | ~1.0M ops/sec | ~48 us  | ~110 us |
| GET, pipeline 16   | ~1.1M ops/sec | ~41 us  | ~94 us  |

Without pipelining the limit is the network round-trip per request, not the
server. Pipelining batches requests so the round-trip cost is amortized and the
engine's actual throughput shows, the same reason `redis-benchmark -P 16` reports
much higher numbers than `-P 1`.

## What it does not do (vs Redis)

These are scope choices, not accidents:

- True LRU via a linked list, where Redis samples a few keys and evicts the
  oldest of the sample. Exact, at the cost of a little bookkeeping per access.
- String values only. No lists, hashes, sets, sorted sets, pub/sub, or the
  commands that go with them (`INCR`, `LPUSH`, and so on).
- One eviction policy (`allkeys-lru`), where Redis makes it configurable.
- The AOF grows without bound; there is no rewrite/compaction yet, and no RDB
  snapshots.
- Everything runs on the main thread, including the `everysec` fsync.
- `used_memory` is an estimate (struct plus key and value bytes), not an
  allocator-exact figure.
- Single database, no clustering, replication, auth, transactions, or `SCAN`.
  `KEYS` exists but is O(n) and meant for debugging.

## Layout

```
src/     server, event loop, hash table, RESP, commands, AOF, config, hashing
tests/   one test file per module, run by `make test`
bench/   loadgen.cpp, the load generator
```
