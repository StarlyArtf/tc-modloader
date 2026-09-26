/* Offline checks for TC_SERVICE_IO_VALUE's number handling.

   The service's game-facing calls (read/write an input, write a constant) can
   only be exercised in the game, but the part a Mod leans on for masks - parse
   the expression a player would type into the game's own value field, keep the
   field's width - is pure arithmetic, and the loader's parser is what answers
   when the game's evaluator is not used.  This pins that behaviour, plus the
   service table's shape, so a change to either is visible without a playtest. */
#include "../src/expression.hpp"
#include "../sdk/tc_io_value.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
}

void checkValue(const char* expression, uint64_t expected) {
    uint64_t value = 0;
    const int status = tc::expr::evaluate(expression, &value);
    if (status != TC_IO_VALUE_OK) {
        ++failures;
        std::printf("FAIL \"%s\" did not parse (status %d)\n", expression, status);
        return;
    }
    if (value != expected) {
        ++failures;
        std::printf("FAIL \"%s\" = 0x%llx, expected 0x%llx\n", expression,
                    static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(expected));
    }
}

void checkSyntax(const char* expression) {
    uint64_t value = 0;
    if (tc::expr::evaluate(expression, &value) == TC_IO_VALUE_OK) {
        ++failures;
        std::printf("FAIL \"%s\" was accepted\n", expression);
    }
}

}  // namespace

int main() {
    /* What the native value field accepts, and what the mask feature asks for. */
    checkValue("0", 0);
    checkValue("20", 20);
    checkValue("0xFFFFFFFF", 0xFFFFFFFFull);
    checkValue("0xFFFFFFFF^(1<<23)", 0xFFFFFFFFull ^ (1ull << 23));
    checkValue("~(1<<23)", ~(1ull << 23));
    checkValue("(0xF<<8)|0x3", 0xF03ull);
    checkValue("1_000_000", 1000000ull);
    checkValue("0b1010_1010", 0xAAull);
    checkValue("0o17", 0xFull);
    checkValue("1<<40", 1ull << 40);
    checkValue("0x100>>4", 0x10ull);
    checkValue("7%4", 3ull);
    checkValue("2+3*4", 14ull);
    checkValue("(2+3)*4", 20ull);
    checkValue("-1", ~0ull);
    checkValue("~0", ~0ull);
    checkValue("0-1", ~0ull);            /* wraps, like the game's unsigned field */
    checkValue("1<<64", 0ull);           /* a shift past the width is zero */

    checkSyntax("");
    checkSyntax("0x");
    checkSyntax("1+");
    checkSyntax("(1");
    checkSyntax("1)");
    checkSyntax("0x1 2");
    checkSyntax("1/0");

    {
        uint64_t value = 0;
        const int status = tc::expr::evaluate("0x1FFFFFFFFFFFFFFFFF", &value);
        check(status == TC_IO_VALUE_ERR_RANGE,
              "a literal wider than 64 bits reports a range error");
    }

    /* The truncation every value field applies. */
    check(tc::expr::truncate(0x1234, 8) == 0x34, "truncate keeps the low byte");
    check(tc::expr::truncate(0x1234, 1) == 0, "one bit keeps bit 0");
    check(tc::expr::truncate(0xFFFFFFFFFFFFFFFFull, 64) == ~0ull,
          "64 bits keep everything");
    check(tc::expr::truncate(0xFFFFFFFFFFFFFFFFull, 0) == 0, "zero bits keep nothing");
    check(tc::io_value::truncate(0x1234, 8) == tc::expr::truncate(0x1234, 8),
          "the SDK's truncate matches the loader's");

    /* The service table the loader hands out, checked without a loader: the
       shape a Mod compiles against, and the guard the SDK wrapper applies. */
    {
        TCIoValueApiV1 api{};
        api.size = sizeof(api);
        api.version = TC_IO_VALUE_API_VERSION_1;
        api.evaluate = [](void*, const char*, uint64_t*) { return TC_IO_VALUE_OK; };
        api.read_input = [](void*, const TCGameHandle*, uint64_t, uint64_t*) { return TC_IO_VALUE_OK; };
        api.write_input = [](void*, const TCGameHandle*, uint64_t, uint64_t) { return TC_IO_VALUE_OK; };
        api.flip_input = [](void*, const TCGameHandle*, uint64_t, uint64_t) { return TC_IO_VALUE_OK; };
        api.input_width = [](void*, const TCGameHandle*, uint64_t, uint32_t*) { return TC_IO_VALUE_OK; };
        api.write_constant = [](void*, const TCGameHandle*, uint64_t, uint64_t) { return TC_IO_VALUE_OK; };
        check(tc::io_value::ready(api), "a complete table is ready");
        api.write_constant = nullptr;
        check(!tc::io_value::ready(api), "a table without the constant write is not");
        check(std::strcmp(TC_SERVICE_IO_VALUE, "tc.io_value") == 0,
              "the service id is the agreed one");
        check(sizeof(TCIoValueApiV1) == 8 * sizeof(void*) + 16,
              "the table stays context + eight entry points");
    }

    if (failures) {
        std::printf("%d io value check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS io value service: expressions parse, values truncate, table shape holds\n");
    return 0;
}
