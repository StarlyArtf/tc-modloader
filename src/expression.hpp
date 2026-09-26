#pragma once
#include "../sdk/tc_service_api.h"
#include <cstdint>

/* The loader's own side of TC_SERVICE_IO_VALUE's number handling.

   The service prefers the game's evaluator, because that is what the native
   value field runs and it is the only way to match it exactly.  This fallback
   exists for the day that entry point is missing or renamed, and it keeps the
   same shape a player already writes into the game's field: decimal, 0x/0b/0o
   literals, `~` and unary minus, `* / %`, `+ -`, `<< >>`, `& ^ |`, parentheses
   and `_` between digits.  Values are unsigned 64-bit and wrap like the game's;
   the caller truncates to the field's width.

   Kept header-only and free of game state so it can be checked offline:
   tests/io-value-service.cpp pins the parse results and the wraparound. */

namespace tc::expr {

namespace detail {

class Parser {
 public:
    explicit Parser(const char* text) : cursor_(text ? text : "") {}

    int parse(uint64_t* out) {
        if (!out) return TC_IO_VALUE_ERR_ARGUMENT;
        const uint64_t value = conditional();
        if (failed_) return error_;
        skip();
        if (*cursor_) return TC_IO_VALUE_ERR_SYNTAX;
        *out = value;
        return TC_IO_VALUE_OK;
    }

 private:
    void skip() {
        while (*cursor_ == ' ' || *cursor_ == '\t' || *cursor_ == '\r' || *cursor_ == '\n')
            ++cursor_;
    }
    bool take(char c) {
        skip();
        if (*cursor_ != c) return false;
        ++cursor_;
        return true;
    }
    bool failed(int code) {
        if (!failed_) {
            failed_ = true;
            error_ = code;
        }
        return false;
    }
    bool ok(uint64_t) { return true; }

    uint64_t literal() {
        skip();
        if (*cursor_ == '(') {
            ++cursor_;
            const uint64_t value = conditional();
            if (!take(')')) failed(TC_IO_VALUE_ERR_SYNTAX);
            return value;
        }
        if (*cursor_ == '~') {
            ++cursor_;
            return ~unary();
        }
        if (*cursor_ == '-') {
            ++cursor_;
            return 0ull - unary();
        }
        if (*cursor_ == '+') {
            ++cursor_;
            return unary();
        }
        unsigned base = 10;
        const char* start = cursor_;
        if (cursor_[0] == '0' && (cursor_[1] == 'x' || cursor_[1] == 'X')) {
            base = 16;
            cursor_ += 2;
        } else if (cursor_[0] == '0' && (cursor_[1] == 'b' || cursor_[1] == 'B')) {
            base = 2;
            cursor_ += 2;
        } else if (cursor_[0] == '0' && (cursor_[1] == 'o' || cursor_[1] == 'O')) {
            base = 8;
            cursor_ += 2;
        }
        uint64_t value = 0;
        unsigned digits = 0;
        for (;;) {
            const char c = *cursor_;
            if (c == '_') {
                ++cursor_;
                continue;
            }
            unsigned d = 0;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A') + 10;
            else break;
            if (d >= base) break;
            if (value > (~0ull - d) / base) failed(TC_IO_VALUE_ERR_RANGE);
            value = value * base + d;
            ++digits;
            ++cursor_;
        }
        if (!digits || cursor_ == start) failed(TC_IO_VALUE_ERR_SYNTAX);
        return value;
    }

    uint64_t unary() { return literal(); }

    uint64_t product() {
        uint64_t value = unary();
        for (;;) {
            skip();
            if (take('*')) {
                value *= unary();
            } else if (take('/')) {
                const uint64_t divisor = unary();
                if (!divisor) {
                    failed(TC_IO_VALUE_ERR_RANGE);
                    return value;
                }
                value /= divisor;
            } else if (take('%')) {
                const uint64_t divisor = unary();
                if (!divisor) {
                    failed(TC_IO_VALUE_ERR_RANGE);
                    return value;
                }
                value %= divisor;
            } else {
                return value;
            }
        }
    }

    uint64_t sum() {
        uint64_t value = product();
        for (;;) {
            if (take('+')) value += product();
            else if (take('-')) value -= product();
            else return value;
        }
    }

    uint64_t shift() {
        uint64_t value = sum();
        for (;;) {
            skip();
            if (cursor_[0] == '<' && cursor_[1] == '<') {
                cursor_ += 2;
                const uint64_t amount = sum();
                value = amount >= 64 ? 0ull : value << amount;
            } else if (cursor_[0] == '>' && cursor_[1] == '>') {
                cursor_ += 2;
                const uint64_t amount = sum();
                value = amount >= 64 ? 0ull : value >> amount;
            } else {
                return value;
            }
        }
    }

    uint64_t bitAnd() {
        uint64_t value = shift();
        while (!failed_ && take('&')) value &= shift();
        return value;
    }

    uint64_t bitXor() {
        uint64_t value = bitAnd();
        while (!failed_ && take('^')) value ^= bitAnd();
        return value;
    }

    uint64_t conditional() {
        uint64_t value = bitXor();
        while (!failed_ && take('|')) value |= bitXor();
        return value;
    }

    const char* cursor_;
    bool failed_ = false;
    int error_ = TC_IO_VALUE_ERR_SYNTAX;
};

}  // namespace detail

/* Parses one expression.  Returns TC_IO_VALUE_OK / _ERR_SYNTAX / _ERR_RANGE. */
inline int evaluate(const char* text, uint64_t* out) {
    detail::Parser parser(text);
    return parser.parse(out);
}

/* Keeps the low `width` bits, the truncation every game value field applies. */
inline uint64_t truncate(uint64_t value, uint32_t width) {
    if (width >= 64) return value;
    if (width == 0) return 0;
    return value & ((1ull << width) - 1ull);
}

}  // namespace tc::expr
