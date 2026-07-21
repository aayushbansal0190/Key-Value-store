#pragma once

#include <cstddef>
#include <string>
#include <vector>

// RESP2 — the Redis wire protocol.
//
// A client command is an array of bulk strings:
//   "SET name aayush"  ->  *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\naayush\r\n
// Length prefixes make the protocol BINARY-SAFE (arguments may contain
// spaces, \r\n, even \0 — none of which inline commands can carry) and
// cheap to parse: read a length, jump, never scan for delimiters inside
// data. (Same principle as SipHash's final-block length byte: padded or
// framed data must carry its true length or different inputs collide.)

// Protocol limits (Redis's own values): a hostile "$99999999999" must be
// rejected while parsing, not honored with a giant allocation.
constexpr long long RESP_MAX_MULTIBULK = 1024 * 1024;          // elements per command
constexpr long long RESP_MAX_BULK = 512ll * 1024 * 1024;       // bytes per argument
constexpr size_t RESP_MAX_INLINE = 64 * 1024;                  // bytes per inline line

enum class ParseResult {
    Complete,    // args filled, `consumed` bytes of buf belong to this command
    Incomplete,  // buf holds only a prefix of a command — wait for more bytes
    Error,       // framing is broken; err says how. Connection must be closed:
                 // with framing lost there is no way to find the next command.
};

// Tries to parse ONE command from the front of buf. Stateless by design:
// on Incomplete we keep buf and simply re-parse when more bytes arrive —
// simpler than a resumable parser, and commands are small (re-scan cost
// is bounded by one command's size, not the whole stream).
ParseResult parse_resp_command(const std::string& buf, size_t& consumed,
                               std::vector<std::string>& args, std::string& err);

// Same as above, but parses the command that begins at buf[start] instead of
// buf[0]. `consumed` is set to the ABSOLUTE end index of the command (so the
// caller advances a cursor to it), not the number of bytes past `start`. This
// lets the event loop walk many pipelined commands with a moving cursor and
// erase the consumed prefix once, instead of erase(0, n) per command (which is
// O(remaining) each time — quadratic over a big pipeline). The zero-argument
// front-of-buffer form above is just this with start = 0.
ParseResult parse_resp_command(const std::string& buf, size_t start, size_t& consumed,
                               std::vector<std::string>& args, std::string& err);

// Reply serializers — the five RESP2 reply types we use.
std::string resp_simple(const std::string& s);   // +OK\r\n       (status)
std::string resp_error(const std::string& msg);  // -ERR ...\r\n  (error)
std::string resp_integer(long long n);           // :2\r\n        (integer)
std::string resp_bulk(const std::string& s);     // $6\r\naayush\r\n (binary-safe string)
std::string resp_nil();                          // $-1\r\n       (key absent — distinct
                                                 //  from any real value, fixing Phase 2's
                                                 //  "(nil)" ambiguity)

// Encodes args as a RESP command (*N + bulk strings) — the client-side
// direction. Used by the AOF: the log stores commands in the exact bytes a
// client could have sent, so replay reuses the normal parser + executor.
std::string resp_command(const std::vector<std::string>& args);

// Reply-side array of bulk strings (KEYS). Identical encoding to
// resp_command — RESP reuses one framing both directions.
std::string resp_array(const std::vector<std::string>& items);
