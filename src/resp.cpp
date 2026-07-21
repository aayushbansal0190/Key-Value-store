#include "resp.h"

#include <cctype>
#include <climits>

// Locates the "\r\n" terminating the line that starts at `pos`.
// Returns false if it isn't in the buffer yet (line still in flight).
static bool find_crlf(const std::string& buf, size_t pos, size_t& crlf_at) {
    size_t nl = buf.find("\r\n", pos);
    if (nl == std::string::npos) return false;
    crlf_at = nl;
    return true;
}

// Strict integer parse of buf[start, end): digits with optional leading '-',
// overflow-checked. Rejects trailing junk by construction (whole range must
// be digits) — "$6x" must be a protocol error, not "6".
static bool parse_ll(const std::string& buf, size_t start, size_t end, long long& out) {
    if (start >= end) return false;
    bool neg = false;
    size_t i = start;
    if (buf[i] == '-') {
        neg = true;
        if (++i >= end) return false;
    }
    long long v = 0;
    for (; i < end; i++) {
        if (!isdigit((unsigned char)buf[i])) return false;
        int digit = buf[i] - '0';
        if (v > (LLONG_MAX - digit) / 10) return false;
        v = v * 10 + digit;
    }
    out = neg ? -v : v;
    return true;
}

ParseResult parse_resp_command(const std::string& buf, size_t& consumed,
                               std::vector<std::string>& args, std::string& err) {
    // Thin wrapper: parse from the very front. `consumed` then equals the
    // number of bytes the command occupied (start is 0, so absolute == count).
    return parse_resp_command(buf, 0, consumed, args, err);
}

ParseResult parse_resp_command(const std::string& buf, size_t start, size_t& consumed,
                               std::vector<std::string>& args, std::string& err) {
    args.clear();
    if (start >= buf.size() || buf[start] != '*') {
        err = "expected '*' as first byte";
        return ParseResult::Error;
    }

    // Header: *<count>\r\n
    size_t crlf;
    if (!find_crlf(buf, start + 1, crlf)) {
        // No terminator yet. A count line can't legitimately be this long —
        // don't let a client feed us an endless header. (Measure from `start`,
        // not the whole buffer, so earlier commands don't count toward it.)
        if (buf.size() - start > 32) {
            err = "invalid multibulk length";
            return ParseResult::Error;
        }
        return ParseResult::Incomplete;
    }
    long long count;
    if (!parse_ll(buf, start + 1, crlf, count) || count < 0 || count > RESP_MAX_MULTIBULK) {
        err = "invalid multibulk length";
        return ParseResult::Error;
    }
    size_t pos = crlf + 2;

    // count == 0 (*0\r\n) parses as an empty command; the caller skips it.
    for (long long i = 0; i < count; i++) {
        // Each element: $<len>\r\n<len bytes>\r\n
        if (pos >= buf.size()) return ParseResult::Incomplete;
        if (buf[pos] != '$') {
            err = std::string("expected '$', got '") + buf[pos] + "'";
            return ParseResult::Error;
        }
        if (!find_crlf(buf, pos + 1, crlf)) {
            if (buf.size() - pos > 32) {
                err = "invalid bulk length";
                return ParseResult::Error;
            }
            return ParseResult::Incomplete;
        }
        long long len;
        if (!parse_ll(buf, pos + 1, crlf, len) || len < 0 || len > RESP_MAX_BULK) {
            // Negative lengths ($-1) are legal in REPLIES (nil) but not in
            // commands — an argument always has a real byte count.
            err = "invalid bulk length";
            return ParseResult::Error;
        }

        size_t data_start = crlf + 2;
        // The data is length-known: we need len bytes + the trailing \r\n.
        // No scanning — this is what makes RESP binary-safe: a value
        // containing "\r\n" is jumped over, never searched through.
        if (buf.size() < data_start + (size_t)len + 2) return ParseResult::Incomplete;
        if (buf[data_start + len] != '\r' || buf[data_start + len + 1] != '\n') {
            err = "bulk data not terminated by CRLF";
            return ParseResult::Error;
        }
        args.emplace_back(buf, data_start, (size_t)len);
        pos = data_start + len + 2;
    }

    consumed = pos;
    return ParseResult::Complete;
}

std::string resp_simple(const std::string& s) {
    return "+" + s + "\r\n";
}

std::string resp_error(const std::string& msg) {
    // Error messages sometimes interpolate untrusted bytes (e.g. an unknown
    // command name, which the client fully controls and RESP lets contain any
    // byte including CR/LF). A raw \r\n inside the message would prematurely
    // terminate this error line and let the client inject fake reply lines into
    // its own stream. Neutralize line breaks to spaces — the ONE place every
    // error reply is built, so no caller can forget. (Redis sanitizes too.)
    std::string safe = msg;
    for (char& ch : safe) {
        if (ch == '\r' || ch == '\n') ch = ' ';
    }
    return "-" + safe + "\r\n";
}

std::string resp_integer(long long n) {
    return ":" + std::to_string(n) + "\r\n";
}

std::string resp_bulk(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

std::string resp_nil() {
    return "$-1\r\n";
}

std::string resp_command(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) {
        out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    }
    return out;
}

std::string resp_array(const std::vector<std::string>& items) {
    // Same bytes as a command: RESP has one encoding for "array of bulk
    // strings", whichever direction it travels.
    return resp_command(items);
}
