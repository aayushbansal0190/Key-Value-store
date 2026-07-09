#pragma once

// Wall-clock milliseconds since the Unix epoch.
//
// Wall clock (CLOCK_REALTIME), not the monotonic clock, deliberately:
// expiry moments are stored as ABSOLUTE timestamps, which Phase 7's AOF
// replay must be able to persist and honor across restarts ("this key
// dies at 14:03:07", not "in 40 seconds from some boot"). The cost is
// sensitivity to system clock jumps (NTP adjustments) — the same trade
// Redis makes.
long long now_ms();
