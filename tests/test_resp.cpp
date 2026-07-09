#include "../src/resp.h"
#include "test.h"

#include <string>
#include <vector>

static const std::string SET_CMD =
    "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\naayush\r\n";

static void test_serializers() {
    CHECK(resp_simple("OK") == "+OK\r\n");
    CHECK(resp_error("ERR boom") == "-ERR boom\r\n");
    CHECK(resp_integer(42) == ":42\r\n");
    CHECK(resp_integer(0) == ":0\r\n");
    CHECK(resp_bulk("aayush") == "$6\r\naayush\r\n");
    CHECK(resp_bulk("") == "$0\r\n\r\n");           // empty string is NOT nil
    CHECK(resp_nil() == "$-1\r\n");

    std::string binary("a\r\nb\0c", 6);              // CRLF and NUL inside data
    CHECK(resp_bulk(binary) == std::string("$6\r\na\r\nb\0c\r\n", 12));
}

static void test_parse_complete_command() {
    size_t consumed = 0;
    std::vector<std::string> args;
    std::string err;

    CHECK(parse_resp_command(SET_CMD, consumed, args, err) == ParseResult::Complete);
    CHECK_EQ(consumed, SET_CMD.size());
    CHECK_EQ(args.size(), 3u);
    CHECK(args[0] == "SET" && args[1] == "name" && args[2] == "aayush");
}

// THE test for stream protocols: every proper prefix of a valid command
// must parse as Incomplete — never Error, never a truncated Complete.
static void test_every_split_point_is_incomplete() {
    size_t consumed = 0;
    std::vector<std::string> args;
    std::string err;

    for (size_t cut = 1; cut < SET_CMD.size(); cut++) {
        std::string prefix = SET_CMD.substr(0, cut);
        ParseResult r = parse_resp_command(prefix, consumed, args, err);
        CHECK(r == ParseResult::Incomplete);
    }
}

static void test_pipelined_commands_consume_exactly() {
    std::string two = SET_CMD + "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n";
    size_t consumed = 0;
    std::vector<std::string> args;
    std::string err;

    CHECK(parse_resp_command(two, consumed, args, err) == ParseResult::Complete);
    CHECK_EQ(consumed, SET_CMD.size());          // exactly one command consumed

    std::string rest = two.substr(consumed);
    CHECK(parse_resp_command(rest, consumed, args, err) == ParseResult::Complete);
    CHECK_EQ(args.size(), 2u);
    CHECK(args[0] == "GET" && args[1] == "name");
}

static void test_binary_safe_arguments() {
    // A value containing \r\n and \0 — impossible in inline mode, routine
    // in RESP: the parser jumps by length, never scans inside data.
    std::string val("line1\r\nline2\0end", 16);
    std::string cmd = "*3\r\n$3\r\nSET\r\n$3\r\nbin\r\n$16\r\n" + val + "\r\n";

    size_t consumed = 0;
    std::vector<std::string> args;
    std::string err;
    CHECK(parse_resp_command(cmd, consumed, args, err) == ParseResult::Complete);
    CHECK_EQ(args.size(), 3u);
    CHECK(args[2] == val);
    CHECK_EQ(args[2].size(), 16u);
}

static void test_empty_array_is_a_skippable_command() {
    size_t consumed = 0;
    std::vector<std::string> args;
    std::string err;
    CHECK(parse_resp_command("*0\r\n", consumed, args, err) == ParseResult::Complete);
    CHECK(args.empty());
    CHECK_EQ(consumed, 4u);
}

static void test_malformed_input_is_error() {
    size_t consumed = 0;
    std::vector<std::string> args;
    std::string err;

    auto is_err = [&](const std::string& s) {
        return parse_resp_command(s, consumed, args, err) == ParseResult::Error;
    };

    CHECK(is_err("*abc\r\n"));                          // non-numeric count
    CHECK(is_err("*-2\r\n"));                           // negative count
    CHECK(is_err("*2000000\r\n"));                      // count over the cap
    CHECK(is_err("*1\r\n#3\r\nfoo\r\n"));               // '$' expected, '#' found
    CHECK(is_err("*1\r\n$-1\r\n"));                     // nil bulk invalid in a command
    CHECK(is_err("*1\r\n$6x\r\nfoobar\r\n"));           // junk inside a length
    CHECK(is_err("*1\r\n$999999999999999\r\n"));        // bulk length over the cap
    CHECK(is_err("*1\r\n$3\r\nfooXY"));                 // data not CRLF-terminated
    CHECK(is_err(std::string(64, '9').insert(0, "*"))); // endless header line
}

int main() {
    test_serializers();
    test_parse_complete_command();
    test_every_split_point_is_incomplete();
    test_pipelined_commands_consume_exactly();
    test_binary_safe_arguments();
    test_empty_array_is_a_skippable_command();
    test_malformed_input_is_error();
    return test_summary("test_resp");
}
