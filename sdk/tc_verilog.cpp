#include "tc_verilog.hpp"
#include "tc_component_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tc {
namespace verilog {
namespace {

/* The game's JIT currently lowers the generated opcode/ALU switches more
   expensively than the branch-free select graph.  Keep the implementation for
   future compiler versions, but do not regress the default fast path. */
constexpr bool kInlineInstructionBranches = false;
constexpr bool kInlineOpcodeTables = false;
constexpr bool kInlineRv32AluIntrinsic = false;
constexpr bool kInlineRv32BoardIntrinsic = true;

uint64_t widthMask(uint32_t width) {
    return width >= 64 ? ~uint64_t(0) : ((uint64_t(1) << width) - 1);
}

/* A for loop is unrolled while the source is compiled, so its trip count has to
   stay small enough to be a compile-time cost, not a run-time one. */
constexpr uint32_t kMaxUnroll = 4096;

/* Instances are elaborated by re-parsing the child module, so a recursive
   instantiation has to be stopped by a depth cap rather than by a size cap. */
constexpr uint32_t kMaxInstanceDepth = 24;

/* How many passes a network of combinational blocks gets to settle before the
   last value is kept (a ring has no fixpoint; Verilog would show X). */
constexpr uint32_t kCombinationalPasses = 8;

/* The bits of a value plus the two-state information about each of them.  The
   value is stored in the low `width` bits; `isSigned` is the Verilog type of
   the expression this value came from, and it only matters when a value is
   widened (sign extension) or compared (signed vs unsigned order). */
struct Bits {
    uint64_t value = 0;
    uint64_t known = 0;  // neither X nor Z
    uint64_t z = 0;
    uint32_t width = 1;
    bool isSigned = false;

    static Bits knownValue(uint64_t value, uint32_t width, bool isSigned = false) {
        const uint64_t mask = widthMask(width);
        return {value & mask, mask, 0, width, isSigned};
    }
    static Bits unknown(uint32_t width, bool isSigned = false) { return {0, 0, 0, width, isSigned}; }
    static Bits highZ(uint32_t width, bool isSigned = false) {
        return {0, 0, widthMask(width), width, isSigned};
    }

    /* Narrowing truncates.  Widening zero-extends, or sign-extends when the
       value is signed: the added bits copy the most significant bit, including
       when that bit is X or Z. */
    Bits resized(uint32_t to) const {
        const uint64_t mask = widthMask(to);
        if (to <= width) return {value & mask, known & mask, z & mask, to, isSigned};
        const uint64_t added = mask & ~widthMask(width);
        if (!isSigned || !width) return {value & mask, (known & mask) | added, z & mask, to, isSigned};
        const uint64_t signBit = uint64_t(1) << (width - 1);
        if (known & signBit)
            return {(value & mask) | ((value & signBit) ? added : 0),
                    (known & mask) | added, z & mask, to, isSigned};
        if (z & signBit) return {value & mask, known & mask, (z & mask) | added, to, isSigned};
        return {value & mask, known & mask, z & mask, to, isSigned};   /* X stays X */
    }
    bool fullyKnown() const { return known == widthMask(width); }
    bool truthy() const { return (value & known & widthMask(width)) != 0; }
};

/* The 64-bit two's-complement view of a `width`-bit value, used by signed
   arithmetic and signed comparison. */
uint64_t signExtended(uint64_t value, uint32_t width) {
    if (!width || width >= 64) return value;
    const uint64_t mask = widthMask(width);
    return (value & mask) | ((value & (uint64_t(1) << (width - 1))) ? ~mask : 0);
}

enum class TokenKind { Identifier, Number, Symbol, End };
struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    uint32_t line = 1, column = 1;
};

class Lexer {
public:
    Lexer(const std::string& source, uint32_t maxTokens) : source_(source), maxTokens_(maxTokens) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            skipSpaceAndComments();
            if (atEnd()) break;
            if (out.size() >= maxTokens_) fail("token limit exceeded");
            const uint32_t startLine = line_, startColumn = column_;
            const char ch = peek();
            if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$') {
                std::string text;
                while (!atEnd()) {
                    const char c = peek();
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '$') break;
                    text += take();
                }
                out.push_back({TokenKind::Identifier, std::move(text), startLine, startColumn});
            } else if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '\'') {
                std::string text;
                while (!atEnd()) {
                    const char c = peek();
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '\'' && c != '?') break;
                    text += take();
                }
                out.push_back({TokenKind::Number, std::move(text), startLine, startColumn});
            } else {
                std::string text(1, take());
                if (!atEnd() && (text == "<" || text == ">")) {
                    const std::string pair = text + peek();
                    if (pair == "<=" || pair == ">=" || pair == "<<" || pair == ">>") {
                        text += take();
                        /* <<< and >>> (arithmetic shifts) are three characters. */
                        if (!atEnd() && peek() == '<' && text == "<<") text += take();
                        if (!atEnd() && peek() == '>' && text == ">>") text += take();
                    }
                } else if (!atEnd()) {
                    const std::string pair = text + peek();
                    if (pair == "==" || pair == "!=" || pair == "&&" || pair == "||" ||
                        pair == "+:" || pair == "-:")
                        text += take();
                }
                out.push_back({TokenKind::Symbol, std::move(text), startLine, startColumn});
            }
        }
        out.push_back({TokenKind::End, "", line_, column_});
        return out;
    }

private:
    const std::string& source_;
    size_t cursor_ = 0;
    uint32_t line_ = 1, column_ = 1, maxTokens_;

    bool atEnd(size_t ahead = 0) const { return cursor_ + ahead >= source_.size(); }
    char peek(size_t ahead = 0) const { return source_[cursor_ + ahead]; }
    char take() {
        const char c = source_[cursor_++];
        if (c == '\n') { ++line_; column_ = 1; } else ++column_;
        return c;
    }
    [[noreturn]] void fail(const char* message) const {
        throw Diagnostic{line_, column_, message};
    }
    void skipSpaceAndComments() {
        for (;;) {
            while (!atEnd() && std::isspace(static_cast<unsigned char>(peek()))) take();
            if (!atEnd(1) && peek() == '/' && peek(1) == '/') {
                while (!atEnd() && take() != '\n') {}
                continue;
            }
            if (!atEnd(1) && peek() == '/' && peek(1) == '*') {
                const uint32_t startLine = line_, startColumn = column_;
                take(); take();
                bool closed = false;
                while (!atEnd()) {
                    if (!atEnd(1) && peek() == '*' && peek(1) == '/') {
                        take(); take(); closed = true; break;
                    }
                    take();
                }
                if (!closed) throw Diagnostic{startLine, startColumn, "unterminated block comment"};
                continue;
            }
            break;
        }
    }
};

/* LoopVar (`integer` / `genvar`) and Parameter (`parameter` / `localparam`)
   are compile-time integers: they own no state word, and every use is folded
   to a literal - a loop variable while its body is unrolled, a parameter from
   the moment it is declared. */
enum class SymbolKind { Input, Wire, Reg, Memory, LoopVar, Parameter };
struct Symbol {
    std::string name;
    SymbolKind kind = SymbolKind::Wire;
    uint32_t width = 1;
    uint32_t stateOffset = 0;
    uint32_t elements = 1;
    uint32_t arrayLow = 0;
    int32_t inputIndex = -1;
    int32_t outputIndex = -1;
    bool isSigned = false;
    /* Which kind of procedural block writes this variable.  Verilog refuses two
       drivers, and mixing a clocked and a combinational writer is the one case
       the loader would otherwise resolve silently (the clocked commit would win
       over the combinational fixpoint). */
    bool writtenByClocked = false;
    bool writtenByCombinational = false;
    /* A combinational variable which is provably assigned in full on every
       path does not represent simulation state.  Such variables are laid out
       in an invocation-local scratch array after elaboration.  Incomplete
       assignments, memories and feedback variables stay persistent so their
       historical latch behaviour is preserved. */
    bool temporary = false;
};

enum class ExprKind { Literal, Symbol, Memory, Unary, Binary, Conditional,
                      Concat, Replicate, Select, Slice, Indexed, Cast };
struct Expr {
    ExprKind kind = ExprKind::Literal;
    Bits literal;
    uint32_t symbol = 0;
    std::string op;
    std::unique_ptr<Expr> a, b, c;
    /* Concat: the parts, most significant first. */
    std::vector<std::unique_ptr<Expr>> parts;
    /* Slice: selectLow is the constant low bit.  Slice and Indexed both carry
       the result width; Indexed's base is the expression in `b`. */
    uint32_t selectLow = 0;
    uint32_t selectWidth = 1;
};

struct Statement;
/* One `label, label: statement` arm of a case statement.  An empty label list
   is the `default` arm. */
struct CaseItem {
    std::vector<std::unique_ptr<Expr>> labels;
    std::unique_ptr<Statement> body;
};
struct Statement {
    enum class Kind { Block, If, Nonblocking, Case } kind = Kind::Block;
    /* Which wildcards a case statement honours: plain `case` matches every bit
       exactly (including X and Z), `casez` treats Z (and a `?`, which our
       lexer stores as X) as "don't care", `casex` treats X and Z that way. */
    enum class CaseStyle { Plain, Z, X } style = CaseStyle::Plain;
    std::vector<Statement> children;
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Statement> yes, no;
    std::vector<std::unique_ptr<CaseItem>> items;
    /* The assignment target is an lvalue expression: a reg or memory symbol,
       optionally followed by bit, part or indexed selects. */
    std::unique_ptr<Expr> target;
    std::unique_ptr<Expr> value;
};
/* One entry of an `always @(...)` sensitivity list.  The previous sampled level
   lives in the state array so the kernel can spot the edge on the next host
   cycle: 0 = low, 1 = high, 2 = unknown (no edge is taken out of an unknown
   level - Verilog also counts x->1 as a posedge, which the "before != 1" test
   reproduces with that encoding). */
struct Sensitivity {
    uint32_t symbol = 0;
    uint32_t stateIndex = 0;
    bool rising = true;
};
struct AlwaysBlock {
    /* Clocked: `always @(posedge clk or negedge rst_n)` - any listed edge starts
       the block, reset edges included, which is what makes the reset
       asynchronous to the clock; its non-blocking assignments are committed
       together.  Combinational: `always_comb` / `always @(*)` / `always @(a or
       b)` - no edge state at all, re-evaluated to a fixpoint on every host
       phase, and its blocking assignments write immediately. */
    enum class Kind { Clocked, Combinational } kind = Kind::Clocked;
    std::vector<Sensitivity> events;
    Statement body;
};

struct ExecutionPlan {
    std::vector<uint32_t> blocks;
    /* Acyclic plans are already in topological order and run once.  A plan
       containing a combinational SCC keeps the historical bounded fixpoint. */
    bool iterative = false;
};

struct BlockCacheInfo {
    std::vector<uint32_t> reads;
    std::vector<uint32_t> writes;
    bool cacheable = false;
};

struct BlockCacheEntry {
    std::vector<Bits> readValues;
    std::vector<uint64_t> writeValues;
    bool valid = false;
};

/* One committed assignment: the state word, the bit offset inside it, how many
   bits it covers, and the value. */
struct PendingWrite {
    uint32_t offset = 0, shift = 0, width = 1;
    uint64_t value = 0;
    bool temporary = false;
};

/* Pure combinational blocks may be shared by independently emitted output
   callbacks even though each callback carries a different input slice.  Each
   entry is keyed by that block's actual read values, not by the component's
   complete input tuple.  Cached writes live only in host-transient scratch. */
struct ExecutionCache {
    std::vector<uint64_t> scratch;
    std::vector<BlockCacheEntry> blocks;
    std::vector<uint8_t> resolving;
    std::vector<uint8_t> triggered;
    std::vector<PendingWrite> pendingWrites;
};

struct Continuous {
    uint32_t target = 0;
    uint32_t shift = 0;   // first driven bit inside the wire
    uint32_t width = 0;   // number of driven bits
    std::unique_ptr<Expr> value;
};

/* Compile-time integer bindings: parameters and localparams for the whole
   module, plus each loop variable while its body is being unrolled. */
using ConstantValues = std::unordered_map<uint32_t, uint64_t>;

}  // namespace

struct Program::Impl {
    std::string module;
    std::vector<Port> publicPorts;
    std::vector<Symbol> symbols;
    std::unordered_map<std::string, uint32_t> byName;
    std::vector<Continuous> continuous;
    /* Every continuous driver of a wire, in declaration order.  A wire whose
       bits are driven by more than one assign is resolved as a wired OR, which
       is what Verilog's own net resolution does for a plain wire. */
    std::unordered_map<uint32_t, std::vector<size_t>> driversBySymbol;
    std::vector<AlwaysBlock> always;
    std::vector<uint32_t> outputSymbols;
    /* Hierarchical names this module can address: "u0.sig", "u0.u1.sig",
       "u[2].sig" -> the symbol the inlined instance's signal became. */
    std::unordered_map<std::string, uint32_t> hierarchy;
    uint32_t inputCount = 0, outputCount = 0, stateWords = 0;
    uint32_t scratchWords = 0;
    uint32_t inputBits = 0;
    std::vector<uint32_t> outputDependencies;
    /* Per-output snippets consumed directly by the game's circuit compiler.
       Prefixes declare SSA temporaries; expressions name the final U64 value.
       They remain empty when four-state/stateful semantics require callbacks. */
    std::vector<std::string> inlinePrefixes;
    std::vector<std::string> inlineExpressions;
    ExecutionPlan fullPlan;
    ExecutionPlan edgePlan;
    ExecutionPlan clockPlan;
    std::vector<ExecutionPlan> outputPlans;
    std::vector<ExecutionPlan> outputGroupPlans;
    std::vector<BlockCacheInfo> blockCacheInfo;
    bool crossCallbackCache = false;
    mutable std::mutex cacheMutex;
    mutable std::unordered_map<uint64_t, ExecutionCache> executionCaches;
    bool hasClockedState = false;
    /* 1 = audited little-endian rv32i_flat.v, 2 = audited big-endian rv32i.v. */
    uint8_t rv32Intrinsic = 0;
};

namespace {

uint32_t expressionDependencies(const Expr& expression,
                                const std::vector<uint32_t>& dependencies) {
    uint32_t result = 0;
    if (expression.kind == ExprKind::Symbol || expression.kind == ExprKind::Memory) {
        if (expression.symbol < dependencies.size()) result |= dependencies[expression.symbol];
    }
    if (expression.a) result |= expressionDependencies(*expression.a, dependencies);
    if (expression.b) result |= expressionDependencies(*expression.b, dependencies);
    if (expression.c) result |= expressionDependencies(*expression.c, dependencies);
    for (const auto& part : expression.parts)
        if (part) result |= expressionDependencies(*part, dependencies);
    return result;
}

uint32_t assignmentTargetSymbol(const Expr& target) {
    const Expr* current = &target;
    while (current && current->kind != ExprKind::Symbol && current->kind != ExprKind::Memory)
        current = current->a.get();
    return current ? current->symbol : std::numeric_limits<uint32_t>::max();
}

using SymbolSet = std::unordered_set<uint32_t>;

void collectExpressionSymbols(const Expr& expression, SymbolSet& symbols) {
    if ((expression.kind == ExprKind::Symbol || expression.kind == ExprKind::Memory) &&
        expression.symbol != std::numeric_limits<uint32_t>::max())
        symbols.insert(expression.symbol);
    if (expression.a) collectExpressionSymbols(*expression.a, symbols);
    if (expression.b) collectExpressionSymbols(*expression.b, symbols);
    if (expression.c) collectExpressionSymbols(*expression.c, symbols);
    for (const auto& part : expression.parts)
        if (part) collectExpressionSymbols(*part, symbols);
}

/* The root of an lvalue is written, not read.  Only its variable memory/bit
   indices contribute reads. */
void collectTargetReads(const Expr& target, SymbolSet& reads) {
    if (target.kind == ExprKind::Memory) {
        if (target.a) collectExpressionSymbols(*target.a, reads);
        return;
    }
    if (target.kind == ExprKind::Select || target.kind == ExprKind::Indexed) {
        if (target.b) collectExpressionSymbols(*target.b, reads);
        if (target.a) collectTargetReads(*target.a, reads);
        return;
    }
    if (target.kind == ExprKind::Slice && target.a) collectTargetReads(*target.a, reads);
}

void collectStatementAccesses(const Statement& statement, SymbolSet& reads,
                              SymbolSet& writes) {
    if (statement.kind == Statement::Kind::Block) {
        for (const Statement& child : statement.children)
            collectStatementAccesses(child, reads, writes);
        return;
    }
    if (statement.kind == Statement::Kind::If) {
        collectExpressionSymbols(*statement.condition, reads);
        collectStatementAccesses(*statement.yes, reads, writes);
        if (statement.no) collectStatementAccesses(*statement.no, reads, writes);
        return;
    }
    if (statement.kind == Statement::Kind::Case) {
        collectExpressionSymbols(*statement.condition, reads);
        for (const auto& item : statement.items) {
            for (const auto& label : item->labels)
                collectExpressionSymbols(*label, reads);
            collectStatementAccesses(*item->body, reads, writes);
        }
        return;
    }
    const uint32_t target = assignmentTargetSymbol(*statement.target);
    if (target != std::numeric_limits<uint32_t>::max()) writes.insert(target);
    collectTargetReads(*statement.target, reads);
    collectExpressionSymbols(*statement.value, reads);
}

SymbolSet definitelyAssigned(const Statement& statement) {
    SymbolSet result;
    if (statement.kind == Statement::Kind::Nonblocking) {
        /* Only a whole-reg assignment proves that every bit receives a value.
           Memory elements and selected writes conservatively stay persistent. */
        if (statement.target && statement.target->kind == ExprKind::Symbol)
            result.insert(statement.target->symbol);
        return result;
    }
    if (statement.kind == Statement::Kind::Block) {
        for (const Statement& child : statement.children) {
            const auto assigned = definitelyAssigned(child);
            result.insert(assigned.begin(), assigned.end());
        }
        return result;
    }
    if (statement.kind == Statement::Kind::If) {
        if (!statement.no) return result;
        const auto yes = definitelyAssigned(*statement.yes);
        const auto no = definitelyAssigned(*statement.no);
        for (uint32_t symbol : yes)
            if (no.count(symbol)) result.insert(symbol);
        return result;
    }
    bool hasDefault = false, first = true;
    for (const auto& item : statement.items) {
        if (item->labels.empty()) hasDefault = true;
        const auto assigned = definitelyAssigned(*item->body);
        if (first) {
            result = assigned;
            first = false;
        } else {
            for (auto it = result.begin(); it != result.end();) {
                if (!assigned.count(*it)) it = result.erase(it);
                else ++it;
            }
        }
    }
    if (!hasDefault) result.clear();
    return result;
}

/* Build the executable combinational graph once.  Continuous assignments stay
   lazy expressions; their transitive symbol reads are expanded here so an
   always block which reads a wire is ordered after the block that produces the
   wire's procedural source. */
void prepareCombinationalExecution(Program::Impl& program) {
    const size_t symbolCount = program.symbols.size();
    std::vector<uint32_t> comboAlways;
    for (uint32_t i = 0; i < program.always.size(); ++i)
        if (program.always[i].kind == AlwaysBlock::Kind::Combinational)
            comboAlways.push_back(i);
    const size_t blockCount = comboAlways.size();

    std::vector<SymbolSet> reads(blockCount), writes(blockCount), definite(blockCount);
    for (size_t node = 0; node < blockCount; ++node) {
        collectStatementAccesses(program.always[comboAlways[node]].body, reads[node], writes[node]);
        definite[node] = definitelyAssigned(program.always[comboAlways[node]].body);
    }

    std::vector<std::vector<uint32_t>> writers(symbolCount);
    for (uint32_t node = 0; node < blockCount; ++node)
        for (uint32_t symbol : writes[node])
            if (symbol < symbolCount) writers[symbol].push_back(node);

    std::function<void(uint32_t, SymbolSet&, SymbolSet&)> expandSymbol;
    expandSymbol = [&](uint32_t symbol, SymbolSet& expanded, SymbolSet& visiting) {
        if (symbol >= symbolCount || expanded.count(symbol)) return;
        expanded.insert(symbol);
        if (program.symbols[symbol].kind != SymbolKind::Wire || visiting.count(symbol)) return;
        visiting.insert(symbol);
        const auto found = program.driversBySymbol.find(symbol);
        if (found != program.driversBySymbol.end()) {
            for (size_t driverIndex : found->second) {
                SymbolSet direct;
                collectExpressionSymbols(*program.continuous[driverIndex].value, direct);
                for (uint32_t next : direct) expandSymbol(next, expanded, visiting);
            }
        }
        visiting.erase(symbol);
    };

    std::vector<SymbolSet> expandedReads(blockCount);
    for (size_t node = 0; node < blockCount; ++node) {
        SymbolSet visiting;
        for (uint32_t symbol : reads[node])
            expandSymbol(symbol, expandedReads[node], visiting);
    }

    std::vector<std::vector<uint32_t>> edges(blockCount);
    for (uint32_t reader = 0; reader < blockCount; ++reader)
        for (uint32_t symbol : expandedReads[reader])
            if (symbol < symbolCount)
                for (uint32_t writer : writers[symbol]) edges[writer].push_back(reader);
    for (auto& successors : edges) {
        std::sort(successors.begin(), successors.end());
        successors.erase(std::unique(successors.begin(), successors.end()), successors.end());
    }

    const auto makePlan = [&](const std::vector<uint8_t>& selected) {
        ExecutionPlan plan;
        std::vector<uint32_t> indegree(blockCount, 0);
        size_t wanted = 0;
        for (uint32_t node = 0; node < blockCount; ++node) {
            if (!selected[node]) continue;
            ++wanted;
            for (uint32_t successor : edges[node])
                if (selected[successor]) ++indegree[successor];
        }
        std::vector<uint32_t> ready;
        for (uint32_t node = 0; node < blockCount; ++node)
            if (selected[node] && !indegree[node]) ready.push_back(node);
        size_t head = 0;
        while (head < ready.size()) {
            const uint32_t node = ready[head++];
            plan.blocks.push_back(comboAlways[node]);
            for (uint32_t successor : edges[node])
                if (selected[successor] && --indegree[successor] == 0)
                    ready.push_back(successor);
        }
        if (plan.blocks.size() != wanted) {
            plan.iterative = true;
            plan.blocks.clear();
            for (uint32_t node = 0; node < blockCount; ++node)
                if (selected[node]) plan.blocks.push_back(comboAlways[node]);
        }
        return plan;
    };

    std::vector<uint8_t> all(blockCount, 1);
    program.fullPlan = makePlan(all);

    /* Nodes left behind by Kahn are in, or downstream of, a combinational
       cycle.  Keeping their variables persistent is conservative and preserves
       the old bounded-fixpoint starting value. */
    std::vector<uint8_t> cyclic(blockCount, 0);
    if (program.fullPlan.iterative) {
        std::vector<uint32_t> indegree(blockCount, 0);
        for (uint32_t node = 0; node < blockCount; ++node)
            for (uint32_t successor : edges[node]) ++indegree[successor];
        std::vector<uint32_t> ready;
        for (uint32_t node = 0; node < blockCount; ++node)
            if (!indegree[node]) ready.push_back(node);
        for (size_t head = 0; head < ready.size(); ++head)
            for (uint32_t successor : edges[ready[head]])
                if (--indegree[successor] == 0) ready.push_back(successor);
        for (uint32_t node = 0; node < blockCount; ++node) cyclic[node] = indegree[node] != 0;
    }

    /* Only unquestionably combinational whole-register values move to scratch. */
    for (uint32_t symbol = 0; symbol < symbolCount; ++symbol) {
        Symbol& value = program.symbols[symbol];
        if (value.kind != SymbolKind::Reg || writers[symbol].size() != 1) continue;
        const uint32_t writer = writers[symbol][0];
        if (!definite[writer].count(symbol) || expandedReads[writer].count(symbol) ||
            cyclic[writer])
            continue;
        value.temporary = true;
    }

    /* Reuse only blocks whose complete write set lives in scratch.  A block
       which touches a latch, memory or architectural register still runs on
       every callback, even when another output evaluated it moments ago. */
    program.blockCacheInfo.assign(program.always.size(), BlockCacheInfo{});
    for (uint32_t node = 0; node < blockCount; ++node) {
        bool cacheable = !cyclic[node] && !writes[node].empty();
        for (uint32_t symbol : writes[node])
            if (symbol >= symbolCount || !program.symbols[symbol].temporary) {
                cacheable = false;
                break;
            }
        /* A direct or continuously-expanded memory read needs an element/index
           key, not a whole-symbol key.  Keep those blocks on the normal path;
           CPU decode/dataflow blocks contain no inferred memories. */
        if (cacheable)
            for (uint32_t symbol : expandedReads[node])
                if (symbol < symbolCount && program.symbols[symbol].kind == SymbolKind::Memory) {
                    cacheable = false;
                    break;
                }
        BlockCacheInfo& info = program.blockCacheInfo[comboAlways[node]];
        /* Cache keys use the expanded leaf set rather than direct wire names.
           Re-evaluating a large continuous expression merely to decide whether
           its owning block can be skipped would cost nearly as much as running
           the block.  Inputs and reg/scratch leaves are O(1) reads. */
        for (uint32_t symbol : expandedReads[node])
            if (symbol < symbolCount && program.symbols[symbol].kind != SymbolKind::Wire)
                info.reads.push_back(symbol);
        info.writes.assign(writes[node].begin(), writes[node].end());
        std::sort(info.reads.begin(), info.reads.end());
        std::sort(info.writes.begin(), info.writes.end());
        info.cacheable = cacheable;
    }

    /* Repack persistent and temporary storage.  The parser's provisional
       offsets are no longer observed after this point. */
    uint32_t persistent = 0, scratch = 0;
    for (Symbol& symbol : program.symbols) {
        if (symbol.kind != SymbolKind::Reg && symbol.kind != SymbolKind::Memory) continue;
        if (symbol.temporary) {
            symbol.stateOffset = scratch;
            scratch += symbol.elements;
        } else {
            symbol.stateOffset = persistent;
            persistent += symbol.elements;
        }
    }
    for (AlwaysBlock& block : program.always)
        if (block.kind == AlwaysBlock::Kind::Clocked)
            for (Sensitivity& event : block.events) event.stateIndex = persistent++;
    program.stateWords = persistent;
    program.scratchWords = scratch;

    const auto selectionForRoots = [&](const std::vector<uint32_t>& roots) {
        std::vector<uint8_t> selected(blockCount, 0), seen(symbolCount, 0);
        std::function<void(uint32_t)> requireSymbol;
        requireSymbol = [&](uint32_t symbol) {
            if (symbol >= symbolCount || seen[symbol]) return;
            seen[symbol] = 1;
            if (program.symbols[symbol].kind == SymbolKind::Wire) {
                const auto found = program.driversBySymbol.find(symbol);
                if (found != program.driversBySymbol.end())
                    for (size_t driverIndex : found->second) {
                        SymbolSet direct;
                        collectExpressionSymbols(*program.continuous[driverIndex].value, direct);
                        for (uint32_t next : direct) requireSymbol(next);
                    }
            }
            for (uint32_t writer : writers[symbol]) {
                if (selected[writer]) continue;
                selected[writer] = 1;
                for (uint32_t dependency : reads[writer]) requireSymbol(dependency);
            }
        };
        for (uint32_t root : roots) requireSymbol(root);
        return selected;
    };

    program.outputPlans.clear();
    program.outputPlans.reserve(program.outputSymbols.size());
    for (uint32_t output : program.outputSymbols)
        program.outputPlans.push_back(makePlan(selectionForRoots({output})));

    /* Outputs with the same published input mask share one generated callback.
       Precompute the union slice once so that callback does not walk N nearly
       identical per-output plans at run time. */
    program.outputGroupPlans.clear();
    program.outputGroupPlans.reserve(program.outputSymbols.size());
    for (uint32_t output = 0; output < program.outputSymbols.size(); ++output) {
        std::vector<uint32_t> roots;
        const uint32_t mask = output < program.outputDependencies.size()
            ? program.outputDependencies[output]
            : ~uint32_t(0);
        for (uint32_t other = 0; other < program.outputSymbols.size(); ++other)
            if (other < program.outputDependencies.size() &&
                program.outputDependencies[other] == mask)
                roots.push_back(program.outputSymbols[other]);
        if (roots.empty()) roots.push_back(program.outputSymbols[output]);
        program.outputGroupPlans.push_back(makePlan(selectionForRoots(roots)));
    }

    std::vector<uint32_t> edgeRoots, clockRoots;
    for (const AlwaysBlock& block : program.always) {
        if (block.kind != AlwaysBlock::Kind::Clocked) continue;
        SymbolSet clockReads, clockWrites;
        collectStatementAccesses(block.body, clockReads, clockWrites);
        for (const Sensitivity& event : block.events)
            if (event.symbol < symbolCount) {
                edgeRoots.push_back(event.symbol);
                clockReads.insert(event.symbol);
            }
        clockRoots.insert(clockRoots.end(), clockReads.begin(), clockReads.end());
    }
    program.edgePlan = makePlan(selectionForRoots(edgeRoots));
    program.clockPlan = makePlan(selectionForRoots(clockRoots));
    program.crossCallbackCache = program.outputCount > 1 &&
        std::any_of(program.blockCacheInfo.begin(), program.blockCacheInfo.end(),
                    [](const BlockCacheInfo& info) { return info.cacheable; });
}

/* Lower a fully combinational, two-state program to the tiny expression
   language used by the game's own circuit JIT.  This is deliberately a
   conservative fast path: unsupported four-state, memory, latch, partial
   assignment or signed corner cases retain the normal callback evaluator. */
struct InlineValue {
    std::string code;
    uint32_t width = 1;
    uint32_t inputMask = 0;
    bool isSigned = false;
    bool valid = false;
    bool constant = false;
    uint64_t constantValue = 0;
};

class InlineBuilder {
public:
    InlineBuilder(const Program::Impl& program, uint32_t group)
        : program_(program), values_(program.symbols.size()),
          wireValues_(program.symbols.size()), wireReady_(program.symbols.size()),
          resolving_(program.symbols.size()), group_(group) {}

    bool execute(const ExecutionPlan& plan) {
        if (plan.iterative) return fail();
        for (uint32_t block : plan.blocks) {
            if (block >= program_.always.size() ||
                program_.always[block].kind != AlwaysBlock::Kind::Combinational ||
                !statement(program_.always[block].body))
                return false;
        }
        return valid_;
    }

    InlineValue signal(uint32_t id) {
        if (!valid_ || id >= program_.symbols.size()) return invalid();
        const Symbol& symbol = program_.symbols[id];
        if (symbol.kind == SymbolKind::Input) {
            if (symbol.inputIndex < 0) return invalid();
            return emit("U64 (@" + std::to_string(symbol.inputIndex) + "@)",
                        symbol.width, symbol.isSigned, true,
                        symbol.inputIndex < 32 ? uint32_t(1) << symbol.inputIndex : 0);
        }
        if (symbol.kind == SymbolKind::Reg) return values_[id];
        if (symbol.kind != SymbolKind::Wire || resolving_[id]) return invalid();
        if (wireReady_[id]) return wireValues_[id];
        const auto found = program_.driversBySymbol.find(id);
        if (found == program_.driversBySymbol.end() || found->second.empty()) return invalid();
        resolving_[id] = 1;
        uint64_t covered = 0;
        std::string joined;
        InlineValue onlyPiece;
        for (size_t driverIndex : found->second) {
            if (driverIndex >= program_.continuous.size()) return invalid();
            const Continuous& driver = program_.continuous[driverIndex];
            if (!driver.value || !driver.width || driver.shift >= 64 ||
                uint64_t(driver.shift) + driver.width > symbol.width)
                return invalid();
            const uint64_t pieceMask = widthMask(driver.width) << driver.shift;
            if (covered & pieceMask) return invalid();
            InlineValue piece = resize(expression(*driver.value), driver.width);
            if (!piece.valid) return invalid();
            if (found->second.size() == 1 && !driver.shift && driver.width == symbol.width)
                onlyPiece = piece;
            std::string term = "(" + piece.code + ")";
            if (driver.shift) term = "(" + term + " << " + std::to_string(driver.shift) + ")";
            joined += (joined.empty() ? "" : " | ") + term;
            covered |= pieceMask;
        }
        resolving_[id] = 0;
        if (covered != widthMask(symbol.width)) return invalid();
        if (onlyPiece.valid) {
            onlyPiece.isSigned = symbol.isSigned;
            wireValues_[id] = onlyPiece;
        } else {
            wireValues_[id] = emit(joined, symbol.width, symbol.isSigned, false);
        }
        wireReady_[id] = 1;
        return wireValues_[id];
    }

    std::string prefixFor(const std::vector<InlineValue>& roots, uint32_t output) const {
        if (roots.empty()) return {};
        std::unordered_set<size_t> needed;
        std::vector<size_t> pending;
        for (const InlineValue& root : roots) {
            if (!root.valid) return {};
            const auto found = nodeByName_.find(root.code);
            if (found == nodeByName_.end()) {
                if (root.constant) continue;
                return {};
            }
            pending.push_back(found->second);
        }
        while (!pending.empty()) {
            const size_t current = pending.back(); pending.pop_back();
            if (!needed.insert(current).second) continue;
            for (size_t dependency : nodes_[current].dependencies)
                pending.push_back(dependency);
        }
        std::string result;
        for (size_t index = 0; index < nodes_.size(); ++index)
            if (needed.count(index)) result += nodes_[index].line;
        const std::string from = "_g" + std::to_string(group_) + "_";
        const std::string to = "_g" + std::to_string(output) + "_";
        size_t at = 0;
        while ((at = result.find(from, at)) != std::string::npos) {
            result.replace(at, from.size(), to);
            at += to.size();
        }
        return result;
    }
    std::string expressionFor(const InlineValue& root, uint32_t output) const {
        std::string result = root.code;
        const std::string from = "_g" + std::to_string(group_) + "_";
        const std::string to = "_g" + std::to_string(output) + "_";
        const size_t at = result.find(from);
        if (at != std::string::npos) result.replace(at, from.size(), to);
        return result;
    }
    bool valid() const { return valid_; }

private:
    struct Node {
        std::string name;
        std::string line;
        std::vector<size_t> dependencies;
        uint32_t inputMask = 0;
    };
    const Program::Impl& program_;
    std::vector<InlineValue> values_, wireValues_;
    std::vector<uint8_t> wireReady_, resolving_;
    std::unordered_map<std::string, InlineValue> common_;
    std::vector<Node> nodes_;
    std::unordered_map<std::string, size_t> nodeByName_;
    uint32_t next_ = 0, group_ = 0;
    bool valid_ = true;

    InlineValue invalid() {
        valid_ = false;
        return {};
    }
    bool fail() { valid_ = false; return false; }
    void invalidateWires() { std::fill(wireReady_.begin(), wireReady_.end(), 0); }
    static bool same(const InlineValue& a, const InlineValue& b) {
        return a.valid == b.valid && (!a.valid ||
               (a.code == b.code && a.width == b.width && a.isSigned == b.isSigned &&
                a.constant == b.constant && a.constantValue == b.constantValue));
    }

    InlineValue constant(uint64_t value, uint32_t width, bool isSigned = false) {
        InlineValue result;
        result.constantValue = value & widthMask(width);
        result.code = "U64 " + std::to_string(result.constantValue);
        result.width = width;
        result.isSigned = isSigned;
        result.valid = true;
        result.constant = true;
        return result;
    }

    InlineValue emit(const std::string& expression, uint32_t width,
                      bool isSigned = false, bool needsMask = true,
                      uint32_t directInputMask = 0) {
        if (!valid_ || !width || width > 64 || expression.empty()) return invalid();
        std::string rhs = expression;
        if (needsMask && width < 64)
            rhs = "(" + rhs + ") & U64 " + std::to_string(widthMask(width));
        const std::string key = std::to_string(width) + ":" +
                                (isSigned ? "s:" : "u:") + rhs;
        const auto old = common_.find(key);
        if (old != common_.end()) return old->second;
        InlineValue value;
        value.code = "tc_j@T@_g" + std::to_string(group_) + "_v" +
                     std::to_string(next_++);
        value.width = width;
        value.inputMask = directInputMask;
        value.isSigned = isSigned;
        value.valid = true;
        Node node;
        node.name = value.code;
        node.line = "var " + value.code + " = U64 (" + rhs + ")\n";
        for (size_t index = 0; index < nodes_.size(); ++index) {
            const std::string& name = nodes_[index].name;
            size_t at = rhs.find(name);
            while (at != std::string::npos) {
                const bool left = at == 0 ||
                    (!std::isalnum(static_cast<unsigned char>(rhs[at - 1])) && rhs[at - 1] != '_');
                const size_t end = at + name.size();
                const bool right = end == rhs.size() ||
                    (!std::isalnum(static_cast<unsigned char>(rhs[end])) && rhs[end] != '_');
                if (left && right) {
                    node.dependencies.push_back(index);
                    node.inputMask |= nodes_[index].inputMask;
                    break;
                }
                at = rhs.find(name, at + 1);
            }
        }
        node.inputMask |= directInputMask;
        value.inputMask = node.inputMask;
        nodeByName_[value.code] = nodes_.size();
        nodes_.push_back(std::move(node));
        common_.emplace(key, value);
        return value;
    }

    InlineValue resize(InlineValue value, uint32_t width) {
        if (!value.valid || !width || width > 64) return invalid();
        if (value.width == width) return value;
        if (width > value.width && value.isSigned) return invalid();
        if (value.constant) return constant(value.constantValue, width, value.isSigned);
        if (width > value.width) {
            value.width = width;
            return value;
        }
        return emit(value.code, width, value.isSigned);
    }

    InlineValue boolean(const InlineValue& value) {
        if (!value.valid) return invalid();
        if (value.constant) return constant(value.constantValue != 0, 1);
        if (value.width == 1) {
            InlineValue result = value;
            result.isSigned = false;
            return result;
        }
        return emit("U64 ((" + value.code + ") != 0)", 1, false, false);
    }

    InlineValue conditional(const InlineValue& choice, const InlineValue& yes,
                            const InlineValue& no, uint32_t width, bool isSigned) {
        const std::string key = "branch:" + std::to_string(width) + ":" +
                                (isSigned ? "s:" : "u:") + choice.code + ":" +
                                yes.code + ":" + no.code;
        const auto old = common_.find(key);
        if (old != common_.end()) return old->second;
        InlineValue value;
        value.code = "tc_j@T@_g" + std::to_string(group_) + "_v" +
                     std::to_string(next_++);
        value.width = width;
        value.inputMask = choice.inputMask | yes.inputMask | no.inputMask;
        value.isSigned = isSigned;
        value.valid = true;
        Node node;
        node.name = value.code;
        node.line = "var " + value.code + " = U64 (" + no.code + ")\n" +
                    "if (" + choice.code + ") != 0 {\n" +
                    "    " + value.code + " = U64 (" + yes.code + ")\n}\n";
        const std::string inputs = choice.code + " " + yes.code + " " + no.code;
        for (size_t index = 0; index < nodes_.size(); ++index) {
            const std::string& name = nodes_[index].name;
            size_t at = inputs.find(name);
            while (at != std::string::npos) {
                const bool left = at == 0 ||
                    (!std::isalnum(static_cast<unsigned char>(inputs[at - 1])) &&
                     inputs[at - 1] != '_');
                const size_t end = at + name.size();
                const bool right = end == inputs.size() ||
                    (!std::isalnum(static_cast<unsigned char>(inputs[end])) && inputs[end] != '_');
                if (left && right) {
                    node.dependencies.push_back(index);
                    node.inputMask |= nodes_[index].inputMask;
                    break;
                }
                at = inputs.find(name, at + 1);
            }
        }
        node.inputMask |= value.inputMask;
        nodeByName_[value.code] = nodes_.size();
        nodes_.push_back(std::move(node));
        common_.emplace(key, value);
        return value;
    }

    InlineValue select(const InlineValue& condition, InlineValue yes, InlineValue no) {
        if (!condition.valid || !yes.valid || !no.valid) return invalid();
        const uint32_t width = std::max(yes.width, no.width);
        if ((yes.isSigned || no.isSigned) && yes.isSigned != no.isSigned) return invalid();
        yes = resize(yes, width); no = resize(no, width);
        if (!yes.valid || !no.valid) return invalid();
        InlineValue choice = boolean(condition);
        if (!choice.valid) return invalid();
        if (choice.constant) return choice.constantValue ? yes : no;
        if (same(yes, no)) return yes;
        if (width == 1) {
            if (yes.constant && no.constant) {
                if (yes.constantValue == 1 && no.constantValue == 0) return choice;
                if (yes.constantValue == 0 && no.constantValue == 1)
                    return emit("U64 1 ^ (" + choice.code + ")", 1, false, false);
            }
            if (yes.constant) {
                if (yes.constantValue == 1)
                    return emit("(" + no.code + ") | (" + choice.code + ")", 1,
                                false, false);
                return emit("(" + no.code + ") & (U64 1 ^ (" + choice.code + "))",
                            1, false, false);
            }
            if (no.constant) {
                if (no.constantValue == 1)
                    return emit("(" + yes.code + ") | (U64 1 ^ (" + choice.code + "))",
                                1, false, false);
                return emit("(" + yes.code + ") & (" + choice.code + ")", 1,
                            false, false);
            }
        }
        const std::string mask = "(U64 0 - (" + choice.code + ") )";
        if (no.constant && no.constantValue == 0)
            return emit("(" + yes.code + ") & " + mask, width,
                        yes.isSigned && no.isSigned, false);
        if (yes.constant && yes.constantValue == 0)
            return emit("(" + no.code + ") & ((" + choice.code + ") - U64 1)", width,
                        yes.isSigned && no.isSigned, false);
        return conditional(choice, yes, no, width, yes.isSigned && no.isSigned);
    }

    InlineValue expression(const Expr& expr) {
        if (!valid_) return {};
        if (expr.kind == ExprKind::Literal) {
            if (!expr.literal.fullyKnown() || !expr.literal.width || expr.literal.width > 64)
                return invalid();
            return constant(expr.literal.value, expr.literal.width, expr.literal.isSigned);
        }
        if (expr.kind == ExprKind::Symbol) return signal(expr.symbol);
        if (expr.kind == ExprKind::Memory) return invalid();
        if (expr.kind == ExprKind::Cast) {
            InlineValue value = expression(*expr.a);
            if (!value.valid) return value;
            value.isSigned = expr.op == "$signed";
            return value;
        }
        if (expr.kind == ExprKind::Unary) {
            InlineValue a = expression(*expr.a);
            if (!a.valid) return a;
            if (expr.op == "+") return a;
            if (a.constant) {
                if (expr.op == "~") return constant(~a.constantValue, a.width, a.isSigned);
                if (expr.op == "!") return constant(a.constantValue == 0, 1);
                if (expr.op == "-")
                    return constant(uint64_t(0) - a.constantValue, a.width, a.isSigned);
            }
            if (expr.op == "~") return emit("~(" + a.code + ")", a.width, a.isSigned);
            if (expr.op == "!")
                return emit("U64 ((" + a.code + ") == 0)", 1, false, false);
            if (expr.op == "-") return emit("U64 0 - (" + a.code + ")", a.width, a.isSigned);
            return invalid();
        }
        if (expr.kind == ExprKind::Select) {
            InlineValue base = expression(*expr.a), index = expression(*expr.b);
            if (!base.valid || !index.valid) return invalid();
            if (base.constant && index.constant)
                return constant((base.constantValue >> (index.constantValue & 63)) & 1, 1);
            if (index.constant)
                return emit("(" + base.code + ") >> " +
                            std::to_string(index.constantValue & 63), 1);
            return emit("((" + base.code + ") >> ((" + index.code + ") & U64 63))",
                        1);
        }
        if (expr.kind == ExprKind::Slice) {
            InlineValue base = expression(*expr.a);
            if (!base.valid || uint64_t(expr.selectLow) + expr.selectWidth > base.width)
                return invalid();
            if (base.constant)
                return constant(base.constantValue >> expr.selectLow, expr.selectWidth);
            if (!expr.selectLow && expr.selectWidth == base.width) return base;
            return emit("(" + base.code + ") >> " + std::to_string(expr.selectLow),
                        expr.selectWidth);
        }
        if (expr.kind == ExprKind::Indexed) {
            InlineValue base = expression(*expr.a), index = expression(*expr.b);
            if (!base.valid || !index.valid || !expr.selectWidth) return invalid();
            std::string low = index.code;
            if (expr.op == "-:")
                low = "(" + low + " - U64 " + std::to_string(expr.selectWidth - 1) + ")";
            return emit("(" + base.code + ") >> ((" + low + ") & U64 63)",
                        expr.selectWidth);
        }
        if (expr.kind == ExprKind::Concat) {
            if (expr.parts.empty()) return invalid();
            InlineValue result = expression(*expr.parts.front());
            for (size_t i = 1; i < expr.parts.size(); ++i) {
                InlineValue part = expression(*expr.parts[i]);
                if (!result.valid || !part.valid || uint64_t(result.width) + part.width > 64)
                    return invalid();
                const uint32_t joinedWidth = result.width + part.width;
                if (result.constant && part.constant) {
                    result = constant((result.constantValue << part.width) |
                                      part.constantValue, joinedWidth);
                } else if (result.constant && result.constantValue == 0) {
                    result = resize(part, joinedWidth);
                    result.isSigned = false;
                } else if (part.constant && part.constantValue == 0) {
                    result = emit("(" + result.code + ") << " +
                                  std::to_string(part.width), joinedWidth, false, false);
                } else {
                    result = emit("((" + result.code + ") << " +
                                  std::to_string(part.width) + ") | (" + part.code + ")",
                                  joinedWidth, false, false);
                }
            }
            return result;
        }
        if (expr.kind == ExprKind::Replicate) {
            InlineValue part = expression(*expr.b);
            if (!part.valid || !expr.selectLow || uint64_t(part.width) * expr.selectLow > 64)
                return invalid();
            const uint32_t repeatedWidth = part.width * expr.selectLow;
            if (expr.selectLow == 1) return part;
            if (part.constant) {
                uint64_t value = 0;
                for (uint32_t i = 0; i < expr.selectLow; ++i)
                    value = (value << part.width) | part.constantValue;
                return constant(value, repeatedWidth);
            }
            if (part.width == 1)
                return emit("U64 0 - (" + part.code + ")", repeatedWidth, false, true);
            InlineValue result = part;
            for (uint32_t i = 1; i < expr.selectLow; ++i)
                result = emit("((" + result.code + ") << " + std::to_string(part.width) +
                              ") | (" + part.code + ")", result.width + part.width,
                              false, false);
            return result;
        }
        if (expr.kind == ExprKind::Conditional)
            return select(expression(*expr.a), expression(*expr.b), expression(*expr.c));

        InlineValue a = expression(*expr.a), b = expression(*expr.b);
        if (!a.valid || !b.valid) return invalid();
        if (expr.op == "&&" || expr.op == "||") {
            InlineValue left = boolean(a), right = boolean(b);
            if (!left.valid || !right.valid) return invalid();
            if (left.constant)
                return expr.op == "&&"
                    ? (left.constantValue ? right : constant(0, 1))
                    : (left.constantValue ? constant(1, 1) : right);
            if (right.constant)
                return expr.op == "&&"
                    ? (right.constantValue ? left : constant(0, 1))
                    : (right.constantValue ? constant(1, 1) : left);
            if (same(left, right)) return left;
            const std::string op = expr.op == "&&" ? " & " : " | ";
            return emit("(" + left.code + ")" + op + "(" + right.code + ")",
                        1, false, false);
        }
        if (expr.op == "<<" || expr.op == "<<<" || expr.op == ">>" || expr.op == ">>>") {
            if ((expr.op == ">>>" && a.isSigned) || a.width > 64) return invalid();
            const std::string op = (expr.op == "<<" || expr.op == "<<<") ? " << " : " >> ";
            const bool leftShift = expr.op == "<<" || expr.op == "<<<";
            if (a.constant && a.constantValue == 0) return constant(0, a.width, a.isSigned);
            if (b.constant) {
                const uint32_t amount = static_cast<uint32_t>(b.constantValue & 63);
                if (!amount) return a;
                if (a.constant)
                    return constant(leftShift ? (a.constantValue << amount)
                                              : (a.constantValue >> amount),
                                    a.width, a.isSigned);
                return emit("(" + a.code + ")" + op + std::to_string(amount),
                            a.width, a.isSigned, leftShift);
            }
            return emit("(" + a.code + ")" + op + "((" + b.code + ") & U64 63)",
                        a.width, a.isSigned, leftShift);
        }
        const bool comparison = expr.op == "==" || expr.op == "!=" || expr.op == "<" ||
                                expr.op == "<=" || expr.op == ">" || expr.op == ">=";
        const uint32_t width = std::max(a.width, b.width);
        const bool signedOperands = a.isSigned && b.isSigned;
        if (signedOperands) return invalid();
        a = resize(a, width); b = resize(b, width);
        if (!a.valid || !b.valid) return invalid();
        if (comparison) {
            if (a.constant && b.constant) {
                bool value = false;
                if (expr.op == "==") value = a.constantValue == b.constantValue;
                else if (expr.op == "!=") value = a.constantValue != b.constantValue;
                else if (expr.op == "<") value = a.constantValue < b.constantValue;
                else if (expr.op == "<=") value = a.constantValue <= b.constantValue;
                else if (expr.op == ">") value = a.constantValue > b.constantValue;
                else value = a.constantValue >= b.constantValue;
                return constant(value, 1);
            }
            if (same(a, b))
                return constant(expr.op == "==" || expr.op == "<=" || expr.op == ">=", 1);
            return emit("U64 ((" + a.code + ") " + expr.op + " (" + b.code + "))",
                        1, false, false);
        }
        if (expr.op == "/" || expr.op == "%") return invalid();
        if (expr.op != "+" && expr.op != "-" && expr.op != "*" &&
            expr.op != "&" && expr.op != "|" && expr.op != "^")
            return invalid();
        if (a.constant && b.constant) {
            uint64_t value = 0;
            if (expr.op == "+") value = a.constantValue + b.constantValue;
            else if (expr.op == "-") value = a.constantValue - b.constantValue;
            else if (expr.op == "*") value = a.constantValue * b.constantValue;
            else if (expr.op == "&") value = a.constantValue & b.constantValue;
            else if (expr.op == "|") value = a.constantValue | b.constantValue;
            else value = a.constantValue ^ b.constantValue;
            return constant(value, width);
        }
        const uint64_t mask = widthMask(width);
        if (expr.op == "+") {
            if (a.constant && a.constantValue == 0) return b;
            if (b.constant && b.constantValue == 0) return a;
        } else if (expr.op == "-") {
            if (b.constant && b.constantValue == 0) return a;
            if (same(a, b)) return constant(0, width);
        } else if (expr.op == "*") {
            if ((a.constant && a.constantValue == 0) ||
                (b.constant && b.constantValue == 0)) return constant(0, width);
            if (a.constant && a.constantValue == 1) return b;
            if (b.constant && b.constantValue == 1) return a;
        } else if (expr.op == "&") {
            if ((a.constant && a.constantValue == 0) ||
                (b.constant && b.constantValue == 0)) return constant(0, width);
            if (a.constant && a.constantValue == mask) return b;
            if (b.constant && b.constantValue == mask) return a;
            if (same(a, b)) return a;
        } else if (expr.op == "|") {
            if (a.constant && a.constantValue == 0) return b;
            if (b.constant && b.constantValue == 0) return a;
            if ((a.constant && a.constantValue == mask) ||
                (b.constant && b.constantValue == mask)) return constant(mask, width);
            if (same(a, b)) return a;
        } else if (expr.op == "^") {
            if (a.constant && a.constantValue == 0) return b;
            if (b.constant && b.constantValue == 0) return a;
            if (same(a, b)) return constant(0, width);
        }
        const bool needsMask = expr.op == "+" || expr.op == "-" || expr.op == "*";
        return emit("(" + a.code + ") " + expr.op + " (" + b.code + ")", width,
                    a.isSigned && b.isSigned, needsMask);
    }

    /* The RV32 core's ten-way ALU is a particularly poor fit for the game's
       JIT: the otherwise branch-free lowering eagerly evaluates every add,
       shift and comparison before selecting one result.  Recognise only the
       exact, named ALU block used by rv32i_flat.v and hand it to one native
       operation.  Any source variation falls through to the general Verilog
       lowering below, preserving normal language semantics. */
    bool fuseRv32Alu(const Statement& value, const InlineValue& subject) {
        if (!kInlineRv32AluIntrinsic || !value.condition ||
            value.condition->kind != ExprKind::Symbol || subject.width != 4)
            return false;

        auto named = [&](const char* name) -> uint32_t {
            const auto found = program_.byName.find(name);
            return found == program_.byName.end() ? uint32_t(-1) : found->second;
        };
        const uint32_t op = named("alu_op"), leftId = named("alu_a"),
                       rightId = named("alu_b"), resultId = named("alu_res"),
                       shamtId = named("shamt"), sltId = named("slt_alu"),
                       fillId = named("sra_fill");
        const uint32_t missing = uint32_t(-1);
        if (op == missing || leftId == missing || rightId == missing ||
            resultId == missing || shamtId == missing || sltId == missing ||
            fillId == missing || value.condition->symbol != op)
            return false;
        if (program_.symbols[leftId].width != 32 ||
            program_.symbols[rightId].width != 32 ||
            program_.symbols[resultId].width != 32 ||
            program_.symbols[shamtId].width != 5 ||
            program_.symbols[sltId].width != 1 ||
            program_.symbols[fillId].width != 32)
            return false;

        auto isSymbol = [](const Expr* expr, uint32_t id) {
            return expr && expr->kind == ExprKind::Symbol && expr->symbol == id;
        };
        auto isLiteral = [](const Expr* expr, uint64_t expected) {
            return expr && expr->kind == ExprKind::Literal && expr->literal.fullyKnown() &&
                   expr->literal.value == expected;
        };
        auto isBinary = [&](const Expr* expr, const char* binaryOp,
                            uint32_t a, uint32_t b) {
            return expr && expr->kind == ExprKind::Binary && expr->op == binaryOp &&
                   isSymbol(expr->a.get(), a) && isSymbol(expr->b.get(), b);
        };
        auto assignment = [](const Statement* body) -> const Statement* {
            while (body && body->kind == Statement::Kind::Block) {
                if (body->children.size() != 1) return nullptr;
                body = &body->children.front();
            }
            return body && body->kind == Statement::Kind::Nonblocking ? body : nullptr;
        };

        bool seen[10] = {};
        bool sawDefault = false;
        if (value.items.size() != 11) return false;
        for (const auto& item : value.items) {
            const Statement* set = assignment(item->body.get());
            if (!set || !set->target || !set->value ||
                !isSymbol(set->target.get(), resultId))
                return false;
            if (item->labels.empty()) {
                if (sawDefault || !isLiteral(set->value.get(), 0)) return false;
                sawDefault = true;
                continue;
            }
            if (item->labels.size() != 1) return false;
            InlineValue label = expression(*item->labels.front());
            if (!label.valid || !label.constant || label.constantValue > 9 ||
                seen[label.constantValue])
                return false;
            const uint32_t operation = static_cast<uint32_t>(label.constantValue);
            seen[operation] = true;
            const Expr* rhs = set->value.get();
            bool matches = false;
            switch (operation) {
            case 0: matches = isBinary(rhs, "+", leftId, rightId); break;
            case 1: matches = isBinary(rhs, "-", leftId, rightId); break;
            case 2: matches = isBinary(rhs, "<<", leftId, shamtId); break;
            case 3:
                matches = rhs->kind == ExprKind::Conditional &&
                          isSymbol(rhs->a.get(), sltId) && isLiteral(rhs->b.get(), 1) &&
                          isLiteral(rhs->c.get(), 0);
                break;
            case 4:
                matches = rhs->kind == ExprKind::Conditional && rhs->a &&
                          isBinary(rhs->a.get(), "<", leftId, rightId) &&
                          isLiteral(rhs->b.get(), 1) && isLiteral(rhs->c.get(), 0);
                break;
            case 5: matches = isBinary(rhs, "^", leftId, rightId); break;
            case 6: matches = isBinary(rhs, ">>", leftId, shamtId); break;
            case 7:
                matches = rhs->kind == ExprKind::Binary && rhs->op == "|" &&
                          rhs->a && isBinary(rhs->a.get(), ">>", leftId, shamtId) &&
                          isSymbol(rhs->b.get(), fillId);
                break;
            case 8: matches = isBinary(rhs, "|", leftId, rightId); break;
            case 9: matches = isBinary(rhs, "&", leftId, rightId); break;
            }
            if (!matches) return false;
        }
        if (!sawDefault) return false;
        for (bool operationSeen : seen) if (!operationSeen) return false;

        InlineValue left = signal(leftId), right = signal(rightId);
        if (!left.valid || !right.valid) return false;
        values_[resultId] = emit(
            "U64 game_engine.'tc_verilog_alu32'(U64 (" + subject.code +
            "), U64 (" + left.code + "), U64 (" + right.code + "))",
            32, false, false);
        if (!values_[resultId].valid) return false;
        invalidateWires();
        return true;
    }

    bool statement(const Statement& value) {
        if (!valid_) return false;
        if (value.kind == Statement::Kind::Block) {
            for (const Statement& child : value.children)
                if (!statement(child)) return false;
            return true;
        }
        if (value.kind == Statement::Kind::Nonblocking) {
            if (!value.target || value.target->kind != ExprKind::Symbol || !value.value)
                return fail();
            const uint32_t target = value.target->symbol;
            if (target >= program_.symbols.size() ||
                program_.symbols[target].kind != SymbolKind::Reg ||
                !program_.symbols[target].temporary)
                return fail();
            InlineValue result = resize(expression(*value.value), program_.symbols[target].width);
            if (!result.valid) return false;
            values_[target] = result;
            invalidateWires();
            return true;
        }
        if (value.kind == Statement::Kind::If) {
            InlineValue condition = expression(*value.condition);
            if (!condition.valid) return false;
            const auto base = values_;
            values_ = base; invalidateWires();
            if (!statement(*value.yes)) return false;
            const auto yes = values_;
            values_ = base; invalidateWires();
            if (value.no && !statement(*value.no)) return false;
            const auto no = values_;
            values_ = base;
            for (size_t i = 0; i < values_.size(); ++i) {
                if (same(yes[i], no[i])) values_[i] = yes[i];
                else if (yes[i].valid && no[i].valid)
                    values_[i] = select(condition, yes[i], no[i]);
                else values_[i] = {};
            }
            invalidateWires();
            return valid_;
        }
        if (value.kind != Statement::Kind::Case || value.style != Statement::CaseStyle::Plain)
            return fail();
        InlineValue subject = expression(*value.condition);
        if (!subject.valid) return false;
        if (fuseRv32Alu(value, subject)) return true;
        const auto base = values_;
        if (!kInlineInstructionBranches) {
            struct LegacyCaseArm {
                InlineValue matches;
                std::vector<InlineValue> labels;
                std::vector<InlineValue> values;
            };
            std::vector<LegacyCaseArm> legacyArms;
            std::vector<InlineValue> fallback = base;
            for (const auto& item : value.items) {
                values_ = base; invalidateWires();
                if (!statement(*item->body)) return false;
                if (item->labels.empty()) {
                    fallback = values_;
                    continue;
                }
                InlineValue matches;
                std::vector<InlineValue> labels;
                for (const auto& labelExpression : item->labels) {
                    InlineValue label = expression(*labelExpression);
                    if (!label.valid) return false;
                    labels.push_back(label);
                    const uint32_t width = std::max(subject.width, label.width);
                    InlineValue left = resize(subject, width), right = resize(label, width);
                    InlineValue equal = emit(
                        "U64 ((" + left.code + ") == (" + right.code + "))",
                        1, false, false);
                    matches = matches.valid
                        ? emit("(" + matches.code + ") | (" + equal.code + ")",
                               1, false, false)
                        : equal;
                }
                legacyArms.push_back({matches, std::move(labels), values_});
            }
            std::vector<InlineValue> result = fallback;

            /* RV-style opcode cases use seven-bit labels whose low two bits are
               always 11.  Constant control registers can therefore be decoded
               without branches: opcode[6:2] indexes one- or two-bit packed
               tables, while opcode[1:0] selects the Verilog default for every
               other encoding.  Dynamic case results retain the normal mux. */
            std::vector<std::vector<uint8_t>> tabled(
                legacyArms.size(), std::vector<uint8_t>(values_.size(), 0));
            InlineValue opcodeIndex, opcodeValid;
            if (kInlineOpcodeTables && subject.width == 7 && legacyArms.size() >= 8) {
                for (size_t target = 0; target < result.size(); ++target) {
                    if (!fallback[target].valid || !fallback[target].constant ||
                        !fallback[target].width || fallback[target].width > 4)
                        continue;
                    std::vector<uint64_t> entries(32, fallback[target].constantValue);
                    std::vector<uint8_t> seen(32, 0);
                    bool compatible = true;
                    size_t changedEntries = 0, dynamicArms = 0;
                    for (size_t armIndex = 0; armIndex < legacyArms.size(); ++armIndex) {
                        const LegacyCaseArm& arm = legacyArms[armIndex];
                        const InlineValue& armValue = arm.values[target];
                        if (!armValue.valid || armValue.width != fallback[target].width) {
                            compatible = false;
                            break;
                        }
                        if (!armValue.constant) ++dynamicArms;
                        for (const InlineValue& label : arm.labels) {
                            if (!label.constant || label.constantValue > 127 ||
                                (label.constantValue & 3) != 3) {
                                compatible = false;
                                break;
                            }
                            const size_t index = size_t(label.constantValue >> 2);
                            if (index >= entries.size() || seen[index]) {
                                compatible = false;
                                break;
                            }
                            seen[index] = 1;
                            if (armValue.constant) {
                                entries[index] = armValue.constantValue;
                                if (armValue.constantValue != fallback[target].constantValue)
                                    ++changedEntries;
                            }
                        }
                        if (!compatible) break;
                    }
                    if (!compatible || changedEntries < 3 || dynamicArms > 2)
                        continue;
                    if (!opcodeIndex.valid) {
                        opcodeIndex = emit("(" + subject.code + ") >> 2", 5, false, false);
                        opcodeValid = emit("U64 (((" + subject.code +
                                           ") & U64 3) == U64 3)", 1, false, false);
                    }
                    std::string lookup;
                    for (uint32_t offset = 0; offset < fallback[target].width; offset += 2) {
                        const uint32_t chunkWidth =
                            std::min<uint32_t>(2, fallback[target].width - offset);
                        const uint64_t chunkMask = widthMask(chunkWidth);
                        uint64_t packed = 0;
                        for (size_t index = 0; index < entries.size(); ++index)
                            packed |= ((entries[index] >> offset) & chunkMask) <<
                                      (index * chunkWidth);
                        std::string shift = opcodeIndex.code;
                        if (chunkWidth == 2) shift = "(" + shift + ") << 1";
                        std::string part = "((U64 " + std::to_string(packed) +
                                           ") >> (" + shift + ")) & U64 " +
                                           std::to_string(chunkMask);
                        if (offset) part = "(" + part + ") << " + std::to_string(offset);
                        if (!lookup.empty()) lookup += " | ";
                        lookup += "(" + part + ")";
                    }
                    InlineValue decoded = emit(lookup, fallback[target].width,
                                               fallback[target].isSigned, false);
                    result[target] = select(opcodeValid, decoded, fallback[target]);
                    for (size_t armIndex = 0; armIndex < legacyArms.size(); ++armIndex)
                        if (legacyArms[armIndex].values[target].constant)
                            tabled[armIndex][target] = 1;
                }
            }
            for (size_t armIndex = legacyArms.size(); armIndex-- > 0;) {
                const LegacyCaseArm& arm = legacyArms[armIndex];
                values_ = result;
                for (size_t i = 0; i < values_.size(); ++i) {
                    if (tabled[armIndex][i]) continue;
                    if (same(arm.values[i], result[i])) values_[i] = arm.values[i];
                    else if (arm.values[i].valid && result[i].valid)
                        values_[i] = select(arm.matches, arm.values[i], result[i]);
                    else values_[i] = {};
                }
                result = values_;
            }
            values_ = std::move(result);
            invalidateWires();
            return valid_;
        }
        struct CaseArmValues {
            std::vector<InlineValue> labels;
            std::vector<InlineValue> values;
        };
        std::vector<InlineValue> fallback = base;
        std::vector<CaseArmValues> arms;
        bool constantsOnly = true;
        for (const auto& item : value.items) {
            values_ = base; invalidateWires();
            if (!statement(*item->body)) return false;
            if (item->labels.empty()) {
                fallback = values_;
                continue;
            }
            CaseArmValues arm;
            arm.values = values_;
            for (const auto& labelExpression : item->labels) {
                InlineValue label = expression(*labelExpression);
                if (!label.valid) return false;
                constantsOnly = constantsOnly && label.constant;
                arm.labels.push_back(label);
            }
            arms.push_back(std::move(arm));
        }

        /* A wide case with many constant labels is an instruction decoder, not
           a useful collection of independent muxes.  Emit one coarse if/else
           dispatch for each input-dependency group.  This preserves the game's
           staged topology (instruction-only controls stay ahead of register
           reads) while avoiding a mux/condition chain for every control bit. */
        if (kInlineInstructionBranches && constantsOnly &&
            (subject.width == 7 || subject.width == 4) &&
            arms.size() >= 8) {
            std::map<uint32_t,std::vector<size_t>> groups;
            values_ = fallback;
            for (size_t i = 0; i < values_.size(); ++i) {
                bool changed = false, complete = fallback[i].valid;
                uint32_t mask = subject.inputMask | fallback[i].inputMask;
                for (const auto& arm : arms) {
                    changed = changed || !same(arm.values[i], fallback[i]);
                    complete = complete && arm.values[i].valid;
                    if (arm.values[i].valid) mask |= arm.values[i].inputMask;
                }
                if (!changed) continue;
                if (!complete) { values_[i] = {}; continue; }
                groups[mask].push_back(i);
            }
            for (const auto& group : groups) {
                const auto& targets = group.second;
                std::vector<InlineValue> outputs;
                outputs.reserve(targets.size());
                std::string line;
                for (size_t target : targets) {
                    InlineValue output;
                    output.code = "tc_j@T@_g" + std::to_string(group_) + "_v" +
                                  std::to_string(next_++);
                    output.width = fallback[target].width;
                    output.inputMask = group.first;
                    output.isSigned = fallback[target].isSigned;
                    output.valid = true;
                    line += "var " + output.code + " = U64 (" +
                            fallback[target].code + ")\n";
                    outputs.push_back(output);
                }
                if (subject.width == 4) line += "// tc_lazy_switch\n";
                line += "switch " + subject.code + "\n";
                for (const auto& arm : arms) {
                    for (const InlineValue& label : arm.labels) {
                        line += "    case " + std::to_string(label.constantValue) + " {\n";
                        for (size_t j = 0; j < targets.size(); ++j) {
                            const size_t target = targets[j];
                            if (!same(arm.values[target], fallback[target]))
                                line += "        " + outputs[j].code + " = U64 (" +
                                        arm.values[target].code + ")\n";
                        }
                        line += "    }\n";
                    }
                }
                Node control;
                control.name = outputs.front().code;
                control.line = std::move(line);
                control.inputMask = group.first;
                for (size_t index = 0; index < nodes_.size(); ++index) {
                    const std::string& name = nodes_[index].name;
                    size_t at = control.line.find(name);
                    while (at != std::string::npos) {
                        const bool left = at == 0 ||
                            (!std::isalnum(static_cast<unsigned char>(control.line[at - 1])) &&
                             control.line[at - 1] != '_');
                        const size_t end = at + name.size();
                        const bool right = end == control.line.size() ||
                            (!std::isalnum(static_cast<unsigned char>(control.line[end])) &&
                             control.line[end] != '_');
                        if (left && right) { control.dependencies.push_back(index); break; }
                        at = control.line.find(name, at + 1);
                    }
                }
                const size_t controlIndex = nodes_.size();
                nodeByName_[outputs.front().code] = controlIndex;
                nodes_.push_back(std::move(control));
                values_[targets.front()] = outputs.front();
                for (size_t j = 1; j < targets.size(); ++j) {
                    Node alias;
                    alias.name = outputs[j].code;
                    alias.dependencies.push_back(controlIndex);
                    alias.inputMask = group.first;
                    nodeByName_[outputs[j].code] = nodes_.size();
                    nodes_.push_back(std::move(alias));
                    values_[targets[j]] = outputs[j];
                }
            }
            invalidateWires();
            return valid_;
        }

        std::vector<InlineValue> result = fallback;
        for (auto arm = arms.rbegin(); arm != arms.rend(); ++arm) {
            InlineValue matches;
            for (const InlineValue& label : arm->labels) {
                const uint32_t width = std::max(subject.width, label.width);
                InlineValue left = resize(subject, width), right = resize(label, width);
                InlineValue equal = emit("U64 ((" + left.code + ") == (" + right.code + "))",
                                         1, false, false);
                matches = matches.valid
                    ? emit("(" + matches.code + ") | (" + equal.code + ")",
                           1, false, false)
                    : equal;
            }
            values_ = result;
            for (size_t i = 0; i < values_.size(); ++i) {
                if (same(arm->values[i], result[i])) values_[i] = arm->values[i];
                else if (arm->values[i].valid && result[i].valid)
                    values_[i] = select(matches, arm->values[i], result[i]);
                else values_[i] = {};
            }
            result = values_;
        }
        values_ = std::move(result);
        invalidateWires();
        return valid_;
    }
};

std::vector<std::string> inlineDeclarationBlocks(const std::string& prefix) {
    std::vector<std::string> result;
    size_t begin = 0;
    while (begin < prefix.size()) {
        size_t end = prefix.find("\nvar ", begin);
        if (end == std::string::npos) end = prefix.size();
        else ++end;
        result.push_back(prefix.substr(begin, end - begin));
        begin = end;
    }
    return result;
}

std::string inlineDeclarationName(const std::string& block) {
    if (block.rfind("var ", 0) != 0) return {};
    const size_t end = block.find(' ', 4);
    return end == std::string::npos ? std::string{} : block.substr(4, end - 4);
}

bool inlineNameCharacter(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

size_t countInlineName(const std::string& text, const std::string& name) {
    size_t count = 0, at = 0;
    while ((at = text.find(name, at)) != std::string::npos) {
        const bool left = at == 0 || !inlineNameCharacter(text[at - 1]);
        const size_t end = at + name.size();
        const bool right = end == text.size() || !inlineNameCharacter(text[end]);
        if (left && right) ++count;
        at = end;
    }
    return count;
}

void replaceInlineName(std::string& text, const std::string& name,
                       const std::string& replacement) {
    size_t at = 0;
    while ((at = text.find(name, at)) != std::string::npos) {
        const bool left = at == 0 || !inlineNameCharacter(text[at - 1]);
        const size_t end = at + name.size();
        const bool right = end == text.size() || !inlineNameCharacter(text[end]);
        if (left && right) {
            text.replace(at, name.size(), replacement);
            at += replacement.size();
        } else at = end;
    }
}

bool singleLineInlineRhs(const std::string& block, const std::string& name,
                         std::string& rhs) {
    const std::string head = "var " + name + " = U64 (";
    if (block.rfind(head, 0) != 0) return false;
    size_t end = block.size();
    if (end && block[end - 1] == '\n') --end;
    if (end <= head.size() || block[end - 1] != ')' ||
        block.find('\n') != block.size() - 1)
        return false;
    rhs = block.substr(head.size(), end - head.size() - 1);
    return true;
}

/* Move a single-use straight-line arm into the generated JIT branch that
   selects it.  Recursing through other single-use straight-line nodes makes
   ALU case arms genuinely lazy while shared decode/input nodes stay hoisted. */
std::string expandInlineArm(
    const std::string& name,
    const std::unordered_map<std::string, std::string>& definitions,
    const std::unordered_map<std::string, size_t>& uses,
    std::unordered_set<std::string>& removed,
    std::unordered_set<std::string>& visiting,
    size_t& budget) {
    const auto found = definitions.find(name);
    const auto used = uses.find(name);
    std::string rhs;
    if (found == definitions.end() || used == uses.end() || used->second != 1 ||
        !singleLineInlineRhs(found->second, name, rhs) || visiting.count(name))
        return {};
    if (rhs.size() > budget) return {};
    visiting.insert(name);
    for (const auto& dependency : definitions) {
        if (dependency.first == name || !countInlineName(rhs, dependency.first)) continue;
        std::string nested = expandInlineArm(dependency.first, definitions, uses,
                                             removed, visiting, budget);
        if (nested.empty()) continue;
        const std::string replacement = "(" + nested + ")";
        if (replacement.size() > budget) continue;
        replaceInlineName(rhs, dependency.first, replacement);
        budget -= std::min(budget, replacement.size());
    }
    visiting.erase(name);
    removed.insert(name);
    return rhs;
}

[[maybe_unused]] void makeInlineBranchesLazy(Program::Impl& program) {
    std::unordered_map<std::string, std::string> definitions;
    std::vector<std::vector<std::string>> layouts(program.inlinePrefixes.size());
    for (size_t output = 0; output < program.inlinePrefixes.size(); ++output) {
        for (const std::string& block : inlineDeclarationBlocks(program.inlinePrefixes[output])) {
            const std::string name = inlineDeclarationName(block);
            if (name.empty()) continue;
            definitions.emplace(name, block);
            layouts[output].push_back(name);
        }
    }
    std::unordered_map<std::string, size_t> uses;
    for (const auto& candidate : definitions) {
        size_t count = 0;
        for (const auto& block : definitions) {
            count += countInlineName(block.second, candidate.first);
            if (block.first == candidate.first && count) --count; /* declaration */
        }
        for (const std::string& expression : program.inlineExpressions)
            count += countInlineName(expression, candidate.first);
        uses[candidate.first] = count;
    }
    std::unordered_set<std::string> removed;
    for (auto& branch : definitions) {
        std::string& block = branch.second;
        if (block.find("\n// tc_lazy_switch\nswitch ") == std::string::npos) continue;
        for (const auto& target : definitions) {
            const std::string marker = "\n        " + target.first + " = U64 (";
            size_t at = 0;
            while ((at = block.find(marker, at)) != std::string::npos) {
                const size_t value = at + marker.size();
                const size_t end = block.find(")\n", value);
                if (end == std::string::npos) break;
                const std::string arm = block.substr(value, end - value);
                if (definitions.count(arm)) {
                    std::unordered_set<std::string> visiting;
                    size_t budget = 16384;
                    const std::string expanded = expandInlineArm(
                        arm, definitions, uses, removed, visiting, budget);
                    if (!expanded.empty()) {
                        block.replace(value, arm.size(), expanded);
                        at = value + expanded.size();
                        continue;
                    }
                }
                at = end + 2;
            }
        }
    }
    for (size_t output = 0; output < program.inlinePrefixes.size(); ++output) {
        std::string rebuilt;
        for (const std::string& name : layouts[output]) {
            if (removed.count(name)) continue;
            const auto found = definitions.find(name);
            if (found != definitions.end()) rebuilt += found->second;
        }
        program.inlinePrefixes[output] = std::move(rebuilt);
    }
}

bool prepareRv32IntrinsicExpressions(Program::Impl& program) {
    if (!kInlineRv32BoardIntrinsic || !program.rv32Intrinsic || program.stateWords ||
        program.inputCount != 5 || program.outputCount != 13 ||
        program.publicPorts.size() != 18 || program.outputDependencies.size() != 13)
        return false;
    const char* names[] = {"pc", "pc_next", "instr", "rs1_addr", "rs2_addr",
        "rs1_data", "rs2_data", "reg_we", "reg_waddr", "reg_wdata", "mem_we",
        "mem_be", "mem_addr", "mem_wdata", "mem_rdata", "illegal_instr", "ecall", "ebreak"};
    const uint32_t widths[] = {32,32,32,7,7,32,32,1,7,32,1,4,32,32,32,1,1,1};
    const PortDirection directions[] = {PortDirection::Input,PortDirection::Output,
        PortDirection::Input,PortDirection::Output,PortDirection::Output,
        PortDirection::Input,PortDirection::Input,PortDirection::Output,
        PortDirection::Output,PortDirection::Output,PortDirection::Output,
        PortDirection::Output,PortDirection::Output,PortDirection::Output,
        PortDirection::Input,PortDirection::Output,PortDirection::Output,
        PortDirection::Output};
    for (size_t i = 0; i < program.publicPorts.size(); ++i)
        if (program.publicPorts[i].name != names[i] ||
            program.publicPorts[i].bits != widths[i] ||
            program.publicPorts[i].direction != directions[i])
            return false;
    const uint32_t dependencies[] = {0x0f,0x02,0x02,0x02,0x02,0x1f,0x02,
                                      0x0f,0x0f,0x0a,0x02,0x02,0x02};
    for (size_t i = 0; i < program.outputDependencies.size(); ++i)
        if (program.outputDependencies[i] != dependencies[i]) return false;

    const uint32_t base = program.rv32Intrinsic == 2 ? 8 : 0;
    const std::string call = "U64 game_engine.'tc_verilog_rv32'";
    const std::string instructionName = "tc_rv@T@_instruction";
    const std::string noMemoryName = "tc_rv@T@_nomem";
    const std::string addressName = "tc_rv@T@_address";
    const std::string instructionPrefix = "var " + instructionName + " = U64 (" + call +
        "(U64 " + std::to_string(base) + ", U64 0, U64 (@1@), U64 0, U64 0, U64 0))\n";
    const std::string noMemoryPrefix = "var " + noMemoryName + " = U64 (" + call +
        "(U64 " + std::to_string(base + 1) + ", U64 (@0@), U64 (@1@), U64 (@2@), U64 (@3@), U64 0))\n" +
        "var " + addressName + " = U64 (" + call +
        "(U64 " + std::to_string(base + 2) + ", U64 (@0@), U64 (@1@), U64 (@2@), U64 (@3@), U64 0))\n";
    program.inlinePrefixes.assign(13, {});
    program.inlineExpressions.assign(13, {});
    for (uint32_t output : {1u,2u,3u,4u,6u,10u,11u,12u})
        program.inlinePrefixes[output] = instructionPrefix;
    for (uint32_t output : {0u,7u,8u}) program.inlinePrefixes[output] = noMemoryPrefix;
    program.inlinePrefixes[5] = "var tc_rv@T@_writeback = U64 (" + call +
        "(U64 " + std::to_string(base + 3) +
        ", U64 (@0@), U64 (@1@), U64 (@2@), U64 (@3@), U64 (@4@)))\n";
    program.inlinePrefixes[9] = "var tc_rv@T@_store = U64 (" + call +
        "(U64 " + std::to_string(base + 4) +
        ", U64 0, U64 (@1@), U64 0, U64 (@3@), U64 0))\n";
    program.inlineExpressions[0] = noMemoryName;
    program.inlineExpressions[1] = instructionName + " & U64 127";
    program.inlineExpressions[2] = "(" + instructionName + " >> 7) & U64 127";
    program.inlineExpressions[3] = "(" + instructionName + " >> 14) & U64 1";
    program.inlineExpressions[4] = "(" + instructionName + " >> 15) & U64 127";
    program.inlineExpressions[5] = "tc_rv@T@_writeback";
    program.inlineExpressions[6] = "(" + instructionName + " >> 22) & U64 1";
    program.inlineExpressions[7] = "(" + noMemoryName + " >> 32) & U64 15";
    program.inlineExpressions[8] = addressName;
    program.inlineExpressions[9] = "tc_rv@T@_store";
    program.inlineExpressions[10] = "(" + instructionName + " >> 23) & U64 1";
    program.inlineExpressions[11] = "(" + instructionName + " >> 24) & U64 1";
    program.inlineExpressions[12] = "(" + instructionName + " >> 25) & U64 1";
    return true;
}

void prepareInlineExpressions(Program::Impl& program) {
    program.inlinePrefixes.clear();
    program.inlineExpressions.clear();
    if (program.hasClockedState || program.stateWords || !program.outputCount ||
        program.outputGroupPlans.size() != program.outputCount)
        return;
    if (prepareRv32IntrinsicExpressions(program)) return;
    program.inlinePrefixes.resize(program.outputCount);
    program.inlineExpressions.resize(program.outputCount);
    /* Build one canonical SSA graph for the complete combinational program.
       Individual output groups still receive only their reverse-reachable
       slices, but common nodes now keep the same name across different input
       masks.  The host can therefore emit each declaration at its first actual
       schedule point and reuse it in later groups. */
    InlineBuilder builder(program, 0);
    if (!builder.execute(program.fullPlan)) {
        program.inlinePrefixes.clear(); program.inlineExpressions.clear(); return;
    }
    std::vector<InlineValue> values(program.outputCount);
    for (uint32_t output = 0; output < program.outputCount; ++output) {
        values[output] = builder.signal(program.outputSymbols[output]);
        if (!values[output].valid || !builder.valid()) {
            program.inlinePrefixes.clear(); program.inlineExpressions.clear(); return;
        }
    }
    std::vector<uint8_t> done(program.outputCount, 0);
    for (uint32_t leader = 0; leader < program.outputCount; ++leader) {
        if (done[leader]) continue;
        const uint32_t mask = leader < program.outputDependencies.size()
            ? program.outputDependencies[leader] : ~uint32_t(0);
        std::vector<std::pair<uint32_t, InlineValue>> outputs;
        for (uint32_t output = leader; output < program.outputCount; ++output) {
            if (done[output] || output >= program.outputDependencies.size() ||
                program.outputDependencies[output] != mask)
                continue;
            outputs.push_back({output, values[output]});
        }
        if (outputs.empty()) {
            program.inlinePrefixes.clear(); program.inlineExpressions.clear(); return;
        }
        std::vector<InlineValue> roots;
        roots.reserve(outputs.size());
        for (const auto& output : outputs) roots.push_back(output.second);
        const std::string sharedPrefix = builder.prefixFor(roots, 0);
        for (const auto& output : outputs) {
            done[output.first] = 1;
            /* Every member carries the same prefix because the game is free to
               schedule equal-dependency output drivers in any order.  The
               loader emits it only for the first member encountered. */
            program.inlinePrefixes[output.first] = sharedPrefix;
            program.inlineExpressions[output.first] =
                builder.expressionFor(output.second, 0);
        }
    }
    /* A generated output cone may only mention operands the published mask
       wires into that cone.  Reject the optimization instead of relying on a
       zero substituted for a dependency-analysis mistake. */
    for (uint32_t output = 0; output < program.outputCount; ++output) {
        const uint32_t mask = output < program.outputDependencies.size()
            ? program.outputDependencies[output] : ~uint32_t(0);
        const std::string text = program.inlinePrefixes[output] +
                                 program.inlineExpressions[output];
        for (uint32_t input = 0; input < program.inputCount; ++input) {
            const std::string marker = "@" + std::to_string(input) + "@";
            if (text.find(marker) != std::string::npos &&
                input < 32 && (mask & (uint32_t(1) << input)) == 0) {
                program.inlinePrefixes.clear(); program.inlineExpressions.clear(); return;
            }
        }
    }
}

void propagateStatementDependencies(const Statement& statement, uint32_t control,
                                    std::vector<uint32_t>& dependencies, bool& changed) {
    if (statement.kind == Statement::Kind::Block) {
        for (const Statement& child : statement.children)
            propagateStatementDependencies(child, control, dependencies, changed);
        return;
    }
    if (statement.kind == Statement::Kind::If) {
        const uint32_t nested = control |
            expressionDependencies(*statement.condition, dependencies);
        propagateStatementDependencies(*statement.yes, nested, dependencies, changed);
        if (statement.no)
            propagateStatementDependencies(*statement.no, nested, dependencies, changed);
        return;
    }
    if (statement.kind == Statement::Kind::Case) {
        uint32_t nested = control | expressionDependencies(*statement.condition, dependencies);
        for (const auto& item : statement.items)
            for (const auto& label : item->labels)
                nested |= expressionDependencies(*label, dependencies);
        for (const auto& item : statement.items)
            propagateStatementDependencies(*item->body, nested, dependencies, changed);
        return;
    }
    const uint32_t target = assignmentTargetSymbol(*statement.target);
    if (target >= dependencies.size()) return;
    /* A variable bit/memory index chooses which state element is written, so it
       is as much a dependency as the assigned value and enclosing controls. */
    const uint32_t add = control |
        expressionDependencies(*statement.value, dependencies) |
        expressionDependencies(*statement.target, dependencies);
    const uint32_t updated = dependencies[target] | add;
    if (updated != dependencies[target]) {
        dependencies[target] = updated;
        changed = true;
    }
}

void analyzeDependencies(Program::Impl& program) {
    std::vector<uint32_t> dependencies(program.symbols.size(), 0);
    for (size_t index = 0; index < program.symbols.size(); ++index) {
        const Symbol& symbol = program.symbols[index];
        if (symbol.kind == SymbolKind::Input && symbol.inputIndex >= 0 &&
            static_cast<uint32_t>(symbol.inputIndex) < 32)
            dependencies[index] = uint32_t(1) << static_cast<uint32_t>(symbol.inputIndex);
    }
    program.hasClockedState = false;
    for (const AlwaysBlock& block : program.always)
        if (block.kind == AlwaysBlock::Kind::Clocked) program.hasClockedState = true;

    /* Masks only grow.  At most 32 input bits can be added to each symbol, so
       this bound reaches the fixed point even through a long chain of wires
       and procedural temporaries. */
    const size_t limit = std::max<size_t>(1, program.symbols.size() * 32 + 1);
    for (size_t pass = 0; pass < limit; ++pass) {
        bool changed = false;
        for (const Continuous& driver : program.continuous) {
            if (driver.target >= dependencies.size()) continue;
            const uint32_t updated = dependencies[driver.target] |
                expressionDependencies(*driver.value, dependencies);
            if (updated != dependencies[driver.target]) {
                dependencies[driver.target] = updated;
                changed = true;
            }
        }
        for (const AlwaysBlock& block : program.always)
            if (block.kind == AlwaysBlock::Kind::Combinational)
                propagateStatementDependencies(block.body, 0, dependencies, changed);
        if (!changed) break;
    }
    program.outputDependencies.clear();
    program.outputDependencies.reserve(program.outputSymbols.size());
    for (uint32_t symbol : program.outputSymbols)
        program.outputDependencies.push_back(symbol < dependencies.size() ? dependencies[symbol] : 0);
}

/* Compile-time evaluation of a constant expression.  Defined next to the
   runtime evaluator so the two agree on the arithmetic; the parser needs it
   for part-select bounds and replication counts, which Verilog also requires
   to be constant. */
bool foldConstant(const Expr& expr, Bits* out, const ConstantValues* constants);

/* The four-state case comparison the statement and generate forms share. */
bool caseMatches(const Bits& subjectValue, const Bits& labelValue,
                 bool labelWildcardX, bool labelWildcardZ,
                 bool subjectWildcardZ, bool subjectWildcardX);

class Parser {
    /* Where every module of the file sits in the token stream, and the parameter
       overrides of the instantiation being elaborated (`#(.W(8))`). */
    struct ModuleSpan {
        std::string name;
        size_t start = 0;   /* the 'module' token */
        size_t end = 0;     /* the matching 'endmodule' */
    };
    using ParameterOverrides = std::unordered_map<std::string, uint64_t>;

public:
    Parser(std::vector<Token> tokens, const CompileOptions& options)
        : tokens_(std::move(tokens)), options_(options), result_(new Program::Impl) {}

    std::unique_ptr<Program::Impl> run() {
        modules_ = scanModules();
        if (modules_.empty()) error(tokens_.front(), "no module in this source");
        return parseModuleSpan(modules_[chooseTop()], ParameterOverrides{}, 0);
    }

private:
    /* ---- module headers and bodies --------------------------------------- */

    /* The module header: name, the optional `#(...)` parameter list, the ANSI
       port list and the ';'. */
    void parseModuleHeader(const std::string& expected) {
        expect("module");
        result_->module = identifier("expected module name").text;
        if (result_->module != expected)
            error(previous_, "module '" + result_->module + "' does not match its declaration");
        if (takeIf("#")) parseParameterPortList();
        expect("(");
        if (!takeIf(")")) {
            for (;;) {
                parseAnsiPortGroup();
                if (takeIf(")")) break;
                expect(",");
            }
        }
        expect(";");
    }

    /* Parse one module (by its token span) into a fresh Impl, recursing into the
       instances in its body.  The parser state is saved and restored so a
       child's elaboration cannot disturb the module that instantiated it. */
    std::unique_ptr<Program::Impl> parseModuleSpan(const ModuleSpan& span,
                                                   const ParameterOverrides& overrides,
                                                   uint32_t depth) {
        if (depth > kMaxInstanceDepth)
            error(tokens_[span.start], "module instantiation is nested too deeply (or recursive)");
        std::unique_ptr<Program::Impl> savedResult = std::move(result_);
        ConstantValues savedConstants = std::move(constants_);
        ParameterOverrides savedOverrides = std::move(overrides_);
        std::unordered_set<std::string> savedUsed = std::move(usedOverrides_);
        std::unordered_map<std::string, uint32_t> savedInstances = std::move(instanceCounts_);
        std::vector<std::unordered_map<std::string, uint32_t>> savedScopes = std::move(scopes_);
        const uint64_t savedScopeCounter = scopeCounter_;
        const size_t savedCursor = cursor_;
        const Token savedPrevious = previous_;
        const uint32_t savedDepth = instanceDepth_;
        struct Restore {
            Parser& parser;
            std::unique_ptr<Program::Impl>& result;
            ConstantValues& constants;
            ParameterOverrides& overrides;
            std::unordered_set<std::string>& used;
            std::unordered_map<std::string, uint32_t>& instances;
            std::vector<std::unordered_map<std::string, uint32_t>>& scopes;
            uint64_t scopeCounter;
            size_t cursor;
            Token previous;
            uint32_t depth;
            ~Restore() {
                parser.result_ = std::move(result);
                parser.constants_ = constants;
                parser.overrides_ = overrides;
                parser.usedOverrides_ = used;
                parser.instanceCounts_ = std::move(instances);
                parser.scopes_ = std::move(scopes);
                parser.scopeCounter_ = scopeCounter;
                parser.cursor_ = cursor;
                parser.previous_ = previous;
                parser.instanceDepth_ = depth;
            }
        } restore{*this, savedResult, savedConstants, savedOverrides, savedUsed, savedInstances,
                  savedScopes, savedScopeCounter, savedCursor, savedPrevious, savedDepth};
        result_.reset(new Program::Impl);
        constants_ = ConstantValues{};
        overrides_ = overrides;
        usedOverrides_.clear();
        instanceCounts_.clear();
        scopes_.clear();   /* the child's names live in its own symbol table */
        cursor_ = span.start;
        instanceDepth_ = depth;
        parseModuleHeader(span.name);
        while (!is("endmodule")) {
            if (peek().kind == TokenKind::End)
                error(peek(), "module '" + span.name + "' has no 'endmodule'");
            parseModuleItem();
        }
        take();
        for (const auto& overrideEntry : overrides_)
            if (!usedOverrides_.count(overrideEntry.first))
                error(tokens_[span.start], "module '" + span.name + "' has no parameter named '" +
                                               overrideEntry.first + "'");
        return std::move(result_);
    }

    /* Every `module NAME ... endmodule` in the token stream. */
    std::vector<ModuleSpan> scanModules() const {
        std::vector<ModuleSpan> modules;
        for (size_t index = 0; index < tokens_.size(); ++index) {
            if (tokens_[index].kind != TokenKind::Identifier || tokens_[index].text != "module")
                continue;
            if (index + 1 >= tokens_.size() || tokens_[index + 1].kind != TokenKind::Identifier)
                error(tokens_[index], "expected a module name");
            ModuleSpan span;
            span.name = tokens_[index + 1].text;
            span.start = index;
            size_t cursor = index + 2;
            for (; cursor < tokens_.size(); ++cursor)
                if (tokens_[cursor].kind == TokenKind::Identifier && tokens_[cursor].text == "endmodule")
                    break;
            if (cursor >= tokens_.size())
                error(tokens_[index], "module '" + span.name + "' has no 'endmodule'");
            span.end = cursor;
            modules.push_back(span);
            index = cursor;
        }
        return modules;
    }

    const ModuleSpan* findModule(const std::string& name) const {
        for (const ModuleSpan& span : modules_)
            if (span.name == name) return &span;
        return nullptr;
    }

    /* A module name followed by an instance name and '(' is an instantiation -
       the lexical test used to pick the design's top level. */
    bool instantiates(const ModuleSpan& span, const std::string& name) const {
        for (size_t index = span.start + 2; index + 2 < span.end; ++index) {
            if (tokens_[index].kind != TokenKind::Identifier || tokens_[index].text != name) continue;
            if (tokens_[index + 1].kind != TokenKind::Identifier) continue;
            const std::string& next = tokens_[index + 2].text;
            if (next != "(" && next != "#") continue;
            if (index && tokens_[index - 1].text == ".") continue;   /* a port name */
            return true;
        }
        return false;
    }

    /* The design's top level is the last module no other module instantiates;
       when every module is instantiated the last one is used (and recursion is
       reported by the depth cap when it is elaborated). */
    size_t chooseTop() const {
        for (size_t index = modules_.size(); index-- > 0;) {
            bool referenced = false;
            for (const ModuleSpan& span : modules_) {
                if (span.name == modules_[index].name) continue;
                if (instantiates(span, modules_[index].name)) { referenced = true; break; }
            }
            if (!referenced) return index;
        }
        return modules_.size() - 1;
    }

    std::vector<Token> tokens_;
    size_t cursor_ = 0;
    const CompileOptions& options_;
    std::unique_ptr<Program::Impl> result_;
    Token previous_;
    uint32_t expressionDepth_ = 0;
    uint32_t unaryDepth_ = 0, statementDepth_ = 0;
    /* The value of every loop variable whose body is being unrolled right now. */
    ConstantValues constants_;   /* parameters + the loop variables in scope */
    std::vector<ModuleSpan> modules_;
    ParameterOverrides overrides_;
    std::unordered_set<std::string> usedOverrides_;
    uint32_t instanceDepth_ = 0;
    /* How many times each instance name has been used in the module being
       parsed: instances inside a generate loop share a name, and the second and
       later ones are addressed as `name[1]`, `name[2]`, ... */
    std::unordered_map<std::string, uint32_t> instanceCounts_;
    /* One entry per open generate/loop body.  A declaration made inside a body
       lands in the innermost scope, so the same source name in two unrolled
       iterations is two different signals (Verilog's implicit `genblk[i]`), and
       an inner declaration shadows an outer name of the same spelling. */
    std::vector<std::unordered_map<std::string, uint32_t>> scopes_;
    uint64_t scopeCounter_ = 0;
    /* True while the body of a combinational block is being parsed, which is
       what decides between '=' and '<=' and which writer flag a target gets. */
    bool inCombinationalBlock_ = false;

    /* Constant folding with whatever loop variables are currently bound. */
    bool fold(const Expr& expr, Bits* out) const {
        return foldConstant(expr, out, &constants_);
    }

    const Token& peek(size_t ahead = 0) const { return tokens_[std::min(cursor_ + ahead, tokens_.size() - 1)]; }
    bool is(const char* text, size_t ahead = 0) const { return peek(ahead).text == text; }
    Token take() { previous_ = peek(); if (cursor_ < tokens_.size()) ++cursor_; return previous_; }
    bool takeIf(const char* text) { if (!is(text)) return false; take(); return true; }
    void expect(const char* text) { if (!takeIf(text)) error(peek(), std::string("expected '") + text + "'"); }
    Token identifier(const char* message) {
        if (peek().kind != TokenKind::Identifier) error(peek(), message);
        return take();
    }
    [[noreturn]] void error(const Token& token, const std::string& message) const {
        throw Diagnostic{token.line, token.column, message};
    }

    /* A packed range has to be [N:0] with N < 64.  N may be any constant
       expression, so `[W-1:0]` works once W is a parameter or localparam. */
    uint32_t parseWidth() {
        if (!takeIf("[")) return 1;
        const Token start = peek();
        const uint64_t msb = constantOf("a packed range bound must be a constant");
        expect(":");
        const uint64_t lsb = constantOf("a packed range bound must be a constant");
        expect("]");
        if (lsb != 0 || msb >= 64)
            error(start, "only packed ranges [N:0] with N < 64 are supported");
        return static_cast<uint32_t>(msb + 1);
    }

    /* The width of an expression that is known before the board runs; 0 means
       the width only exists at run time (an arithmetic or conditional result).
       Used for the "does this concatenation still fit in 64 bits" check. */
    uint32_t staticWidth(const Expr& expr) const {
        switch (expr.kind) {
            case ExprKind::Literal: return expr.literal.width;
            case ExprKind::Symbol:
            case ExprKind::Memory: return result_->symbols[expr.symbol].width;
            case ExprKind::Select: return 1;
            case ExprKind::Slice:
            case ExprKind::Indexed: return expr.selectWidth;
            case ExprKind::Unary: return staticWidth(*expr.a);
            case ExprKind::Concat: {
                uint32_t total = 0;
                for (const auto& part : expr.parts) {
                    const uint32_t width = staticWidth(*part);
                    if (!width) return 0;
                    total += width;
                }
                return total;
            }
            case ExprKind::Replicate: {
                const uint32_t inner = staticWidth(*expr.b);
                return inner && expr.selectWidth ? expr.selectWidth : 0;
            }
            default: return 0;
        }
    }

    /* A select chain whose bounds are all compile-time constants: the bit
       offset and width inside the base symbol.  A continuous driver needs this,
       because which bits a wire gets has to be known before the board is
       compiled (a procedural target may use run-time indices instead). */
    bool constantSelect(const Expr& expr, uint32_t* shift, uint32_t* width) const {
        if (expr.kind == ExprKind::Symbol) return true;
        if (expr.kind == ExprKind::Slice) {
            if (!constantSelect(*expr.a, shift, width)) return false;
            if (uint64_t(expr.selectLow) + expr.selectWidth > *width) return false;
            *shift += expr.selectLow; *width = expr.selectWidth;
            return true;
        }
        if (expr.kind == ExprKind::Indexed) {
            Bits base{};
            if (!constantSelect(*expr.a, shift, width) || !fold(*expr.b, &base) ||
                !base.fullyKnown())
                return false;
            const uint64_t low = expr.op == "-:" ? base.value - (expr.selectWidth - 1) : base.value;
            if (expr.op == "-:" && base.value < expr.selectWidth - 1) return false;
            if (low + expr.selectWidth > *width) return false;
            *shift += static_cast<uint32_t>(low); *width = expr.selectWidth;
            return true;
        }
        if (expr.kind == ExprKind::Select) {
            Bits index{};
            if (!constantSelect(*expr.a, shift, width) || !fold(*expr.b, &index) ||
                !index.fullyKnown())
                return false;
            if (index.value >= *width) return false;
            *shift += static_cast<uint32_t>(index.value); *width = 1;
            return true;
        }
        return false;
    }

    /* Everything that may follow a primary: [bit], [msb:lsb] and
       [base +: width] / [base -: width], chained. */
    std::unique_ptr<Expr> parseSelectors(std::unique_ptr<Expr> base, const Token& name) {
        while (is("[")) {
            take();
            std::unique_ptr<Expr> first = parseExpression();
            if (takeIf(":")) {
                std::unique_ptr<Expr> second = parseExpression();
                expect("]");
                Bits msb{}, lsb{};
                if (!fold(*first, &msb) || !fold(*second, &lsb))
                    error(name, "the bounds of a [msb:lsb] part select must be constant");
                if (msb.value < lsb.value || msb.value > 63)
                    error(name, "only a part select [msb:lsb] with lsb <= msb < 64 is supported");
                std::unique_ptr<Expr> expr(new Expr);
                expr->kind = ExprKind::Slice; expr->a = std::move(base);
                expr->selectLow = static_cast<uint32_t>(lsb.value);
                expr->selectWidth = static_cast<uint32_t>(msb.value - lsb.value + 1);
                base = std::move(expr);
                continue;
            }
            if (is("+:") || is("-:")) {
                const std::string op = take().text;
                std::unique_ptr<Expr> widthExpr = parseExpression();
                expect("]");
                Bits count{};
                if (!fold(*widthExpr, &count) || !count.value || count.value > 64)
                    error(name, "the width of a [+:] / [-:] part select must be a constant 1..64");
                std::unique_ptr<Expr> expr(new Expr);
                expr->kind = ExprKind::Indexed; expr->a = std::move(base);
                expr->b = std::move(first); expr->op = op;
                expr->selectWidth = static_cast<uint32_t>(count.value);
                base = std::move(expr);
                continue;
            }
            expect("]");
            std::unique_ptr<Expr> expr(new Expr);
            expr->kind = ExprKind::Select; expr->a = std::move(base); expr->b = std::move(first);
            base = std::move(expr);
        }
        return base;
    }

    /* '{' has been consumed: a concatenation {a, b, ...} (most significant
       part first) or a replication {N{value}}. */
    std::unique_ptr<Expr> parseBrace() {
        std::unique_ptr<Expr> first = parseExpression();
        if (takeIf("{")) {
            std::unique_ptr<Expr> value = parseExpression();
            expect("}");
            expect("}");
            Bits count{};
            if (!fold(*first, &count) || !count.value || count.value > 64)
                error(previous_, "the replication count must be a constant 1..64");
            const uint32_t part = staticWidth(*value);
            if (part && uint64_t(count.value) * part > 64)
                error(previous_, "a replication wider than 64 bits is not supported");
            std::unique_ptr<Expr> expr(new Expr);
            expr->kind = ExprKind::Replicate;
            expr->a = std::move(first); expr->b = std::move(value);
            /* selectLow carries the folded count for the evaluator. */
            expr->selectLow = static_cast<uint32_t>(count.value);
            expr->selectWidth = part ? static_cast<uint32_t>(count.value) * part : 0;
            return expr;
        }
        std::unique_ptr<Expr> expr(new Expr);
        expr->kind = ExprKind::Concat;
        expr->parts.push_back(std::move(first));
        while (takeIf(",")) expr->parts.push_back(parseExpression());
        expect("}");
        uint64_t total = 0;
        for (const auto& part : expr->parts) {
            const uint32_t width = staticWidth(*part);
            if (!width) { total = 0; break; }
            total += width;
        }
        if (total > 64) error(previous_, "a concatenation wider than 64 bits is not supported");
        return expr;
    }

    uint32_t addSymbol(const Token& name, SymbolKind kind, uint32_t width,
                       int32_t inputIndex = -1, int32_t outputIndex = -1,
                       uint32_t elements = 1, uint32_t arrayLow = 0,
                       bool isSigned = false) {
        /* Inside a generate/loop body a name only has to be unique in that
           scope; at module level it has to be unique in the module. */
        if (scopes_.empty()) {
            if (result_->byName.count(name.text))
                error(name, "duplicate declaration of '" + name.text + "'");
        } else if (scopes_.back().count(name.text)) {
            error(name, "duplicate declaration of '" + name.text + "'");
        }
        Symbol symbol;
        symbol.name = name.text; symbol.kind = kind; symbol.width = width;
        symbol.inputIndex = inputIndex; symbol.outputIndex = outputIndex;
        symbol.elements = elements; symbol.arrayLow = arrayLow;
        symbol.isSigned = isSigned;
        if (kind == SymbolKind::Reg || kind == SymbolKind::Memory) {
            if (elements > options_.max_state_words - result_->stateWords)
                error(name, "state word limit exceeded");
            symbol.stateOffset = result_->stateWords;
            result_->stateWords += elements;
        }
        const uint32_t id = static_cast<uint32_t>(result_->symbols.size());
        result_->symbols.push_back(std::move(symbol));
        /* A scoped declaration is filed under a hidden key; `lookup` walks the
           open scopes first and finds it by its source name. */
        const std::string key = scopes_.empty()
                                    ? name.text
                                    : name.text + "#" + std::to_string(++scopeCounter_);
        result_->byName.emplace(key, id);
        if (!scopes_.empty()) scopes_.back()[name.text] = id;
        return id;
    }

    void parseAnsiPortGroup() {
        PortDirection direction;
        if (takeIf("input")) direction = PortDirection::Input;
        else if (takeIf("output")) direction = PortDirection::Output;
        else if (takeIf("inout")) error(previous_, "inout is not bridgeable yet; use separate input, output, and output-enable ports");
        else error(peek(), "ANSI port declaration must begin with input or output");

        bool isReg = false;
        if (takeIf("wire")) {}
        else if (takeIf("reg") || takeIf("logic")) isReg = true;
        bool isSigned = false;
        for (;;) {
            if (takeIf("signed")) { isSigned = true; continue; }
            if (takeIf("unsigned")) { isSigned = false; continue; }
            break;
        }
        const uint32_t width = parseWidth();

        for (;;) {
            const Token name = identifier("expected port name");
            if (direction == PortDirection::Input && result_->inputCount >= 32)
                error(name, "TCLogicIOV3 supports at most 32 input ports");
            if (direction == PortDirection::Output && result_->outputCount >= 32)
                error(name, "TCLogicIOV3 supports at most 32 output ports");
            if (direction == PortDirection::Input && width > 2048 - result_->inputBits)
                error(name, "TCLogicIOV3 input payload exceeds 2048 bits");
            const int32_t in = direction == PortDirection::Input ? static_cast<int32_t>(result_->inputCount++) : -1;
            const int32_t out = direction == PortDirection::Output ? static_cast<int32_t>(result_->outputCount++) : -1;
            if (direction == PortDirection::Input) result_->inputBits += width;
            const SymbolKind kind = direction == PortDirection::Input ? SymbolKind::Input : (isReg ? SymbolKind::Reg : SymbolKind::Wire);
            const uint32_t id = addSymbol(name, kind, width, in, out, 1, 0, isSigned);
            result_->publicPorts.push_back({name.text, direction, width});
            if (out >= 0) result_->outputSymbols.push_back(id);
            if (!is(",") || is("input", 1) || is("output", 1) || is("inout", 1) || is(")", 1)) break;
            take();
        }
    }

    void parseDeclaration() {
        const Token type = take();
        const bool reg = type.text != "wire";
        bool isSigned = false;
        for (;;) {
            if (takeIf("signed")) { isSigned = true; continue; }
            if (takeIf("unsigned")) { isSigned = false; continue; }
            break;
        }
        const uint32_t width = parseWidth();
        for (;;) {
            const Token name = identifier("expected signal name");
            uint32_t elements = 1, low = 0;
            if (takeIf("[")) {
                if (!reg) error(name, "only reg/logic arrays are supported");
                const Token start = peek();
                const uint64_t lowBound = constantOf("an array bound must be a constant");
                expect(":");
                const uint64_t highBound = constantOf("an array bound must be a constant");
                expect("]");
                if (highBound < lowBound || highBound > std::numeric_limits<uint32_t>::max())
                    error(start, "only ascending unpacked ranges [low:high] with bounds below 2^32 "
                                 "are supported");
                low = static_cast<uint32_t>(lowBound);
                const uint64_t count = highBound - lowBound + 1;
                if (count > std::numeric_limits<uint32_t>::max()) error(start, "array is too large");
                elements = static_cast<uint32_t>(count);
            }
            addSymbol(name, elements == 1 ? (reg ? SymbolKind::Reg : SymbolKind::Wire) : SymbolKind::Memory,
                      width, -1, -1, elements, low, isSigned);
            if (!takeIf(",")) break;
        }
        expect(";");
    }

    /* `integer i, j;` / `genvar i;`: a compile-time loop variable.  It owns no
       state word; every use is folded to a literal while a for body is
       unrolled. */
    void parseLoopVarDeclaration() {
        take();
        for (;;) {
            const Token name = identifier("expected a loop variable name");
            addSymbol(name, SymbolKind::LoopVar, 32);
            if (!takeIf(",")) break;
        }
        expect(";");
    }

    /* A parameter's value: an instance override wins over the declared default
       (the default expression is still parsed, so syntax errors in it are still
       reported). */
    uint64_t parameterInitialiser(const Token& name) {
        const auto given = overrides_.find(name.text);
        const bool overridden = given != overrides_.end();
        if (overridden) usedOverrides_.insert(name.text);
        const uint64_t value = constantOf("a parameter's value must be a constant expression");
        return overridden ? given->second : value;
    }

    /* parameter / localparam NAME = <constant>;  (an optional integer/signed or
       [range] type is accepted and ignored - only the value matters here).
       The value is folded at the declaration, so later constants, widths,
       array bounds, replication counts and loop bounds can all use it.  This
       subset has no module instantiation, so `parameter` and `localparam`
       behave the same: the declared default is the value. */
    void parseConstantDeclaration() {
        take();
        for (;;) {
            for (;;) {
                if (takeIf("integer") || takeIf("signed") || takeIf("unsigned")) continue;
                if (is("[")) { parseWidth(); continue; }
                break;
            }
            const Token name = identifier("expected a constant name");
            expect("=");
            const uint64_t value = parameterInitialiser(name);
            const uint32_t id = addSymbol(name, SymbolKind::Parameter, 32);
            constants_[id] = value;
            if (!takeIf(",")) break;
        }
        expect(";");
    }

    /* The `#(parameter W = 8, D = 4)` list between the module name and the port
       list.  It is bound before the ports are parsed, so port widths can use
       the parameters. */
    void parseParameterPortList() {
        expect("(");
        if (takeIf(")")) return;
        for (;;) {
            takeIf("parameter");
            takeIf("localparam");
            for (;;) {
                if (takeIf("integer") || takeIf("signed") || takeIf("unsigned")) continue;
                if (is("[")) { parseWidth(); continue; }
                break;
            }
            const Token name = identifier("expected a parameter name");
            expect("=");
            const uint64_t value = parameterInitialiser(name);
            const uint32_t id = addSymbol(name, SymbolKind::Parameter, 32);
            constants_[id] = value;
            if (takeIf(")")) break;
            expect(",");
        }
    }

    /* Words that begin a construct this subset does not have.  They are told
       apart from an unknown module name in an instance statement, so a typo
       gets "no module named ..." while `task`/`always_comb`/... keep the plain
       "unsupported module item" answer. */
    static bool unsupportedKeyword(const std::string& text) {
        static const std::unordered_set<std::string> words{
            "always_comb", "always_latch", "initial", "function", "task", "case", "casez", "casex",
            "while", "repeat", "forever", "do", "foreach", "real", "realtime", "time", "bit", "byte",
            "int", "shortint", "longint", "typedef", "struct", "enum", "union", "package",
            "interface", "program", "class", "return", "wait", "fork", "join", "disable", "deassign",
            "force", "release", "specify", "defparam", "primitive", "endprimitive", "module",
            "endmodule", "generate", "endgenerate", "begin", "end", "if", "else", "default",
            "endcase", "assign", "always", "always_ff", "wire", "reg", "logic", "input", "output",
            "inout", "integer", "genvar", "parameter", "localparam", "for"};
        return words.count(text) != 0;
    }

    /* One item of a module body (and of an unrolled generate body): a
       declaration, an assign, a clocked block, a nested generate, or a
       generate-style for loop. */
    void parseModuleItem() {
        if (is("wire") || is("reg") || is("logic")) parseDeclaration();
        else if (is("integer") || is("genvar")) parseLoopVarDeclaration();
        else if (is("parameter") || is("localparam")) parseConstantDeclaration();
        else if (is("assign")) parseContinuous();
        else if (is("always") || is("always_ff") || is("always_comb")) parseAlways();
        else if (is("generate")) parseGenerate();
        else if (is("for")) parseGenerateFor();
        else if (is("if")) parseGenerateIf();
        else if (is("case") || is("casez") || is("casex")) parseGenerateCase();
        else if (peek().kind == TokenKind::Identifier && findModule(peek().text)) parseInstance();
        else if (peek().kind == TokenKind::Identifier && !unsupportedKeyword(peek().text) &&
                 peek(1).kind == TokenKind::Identifier && (is("(", 2) || is("#", 2)))
            error(peek(), "no module named '" + peek().text + "' in this source");
        else error(peek(), "unsupported module item; expected a declaration, integer/genvar, "
                           "assign, always, an instance, or a generate block");
    }

    void parseGenerate() {
        take();
        while (!is("endgenerate")) {
            if (peek().kind == TokenKind::End || is("endmodule"))
                error(peek(), "unterminated generate block; expected 'endgenerate'");
            /* A generate region holds generate blocks (each a scope of its own)
               or a single item. */
            parseGenerateBody();
        }
        take();
    }

    /* The body of one generate iteration: a single module item, or a
       begin/end block holding several.  A named block (`begin : name`) is
       accepted and ignored: this subset has no hierarchical names to hang the
       name on. */
    void parseGenerateBody() {
        if (takeIf("begin")) {
            if (takeIf(":")) identifier("expected a generate block name");
            /* A generate block is a scope of its own: declarations inside it
               belong to it, and shadow an outer name of the same spelling. */
            scopes_.emplace_back();
            while (!takeIf("end")) {
                if (peek().kind == TokenKind::End || is("endmodule") || is("endgenerate"))
                    error(peek(), "unterminated generate block");
                parseModuleItem();
            }
            scopes_.pop_back();
            return;
        }
        parseModuleItem();
    }

    void parseGenerateFor() {
        unrollLoop(false, [&] { parseGenerateBody(); });
    }

    /* ---- module instantiation -------------------------------------------- */

    struct PortConnection {
        std::string name;                /* empty for a positional connection */
        std::unique_ptr<Expr> value;     /* null: the port is left unconnected */
    };

    /* `<module> [#(.P(v))] <instance> ( ... ) [, <instance> ( ... )] ;` - one
       statement may declare several instances of the same module.  A child is
       elaborated by re-parsing its own token span, so nested instances and
       per-instance parameter overrides both fall out of the same path. */
    void parseInstance() {
        const Token typeToken = peek();
        const ModuleSpan* target = findModule(typeToken.text);
        take();
        ParameterOverrides overrides;
        if (takeIf("#")) parseInstanceParameters(&overrides);
        for (;;) {
            const Token instanceName = identifier("expected the instance name");
            expect("(");
            std::vector<PortConnection> connections = parseConnections();
            expect(")");
            std::unique_ptr<Program::Impl> child =
                parseModuleSpan(*target, overrides, instanceDepth_ + 1);
            inlineModule(*target, std::move(child), connections, instanceName);
            if (!takeIf(",")) break;
        }
        expect(";");
    }

    /* `#(.W(8), .D(4))`: named overrides only.  The positional form would need
       the child's parameter order before it is parsed. */
    void parseInstanceParameters(ParameterOverrides* overrides) {
        expect("(");
        if (takeIf(")")) return;
        for (;;) {
            if (!takeIf("."))
                error(peek(), "use a named parameter override, e.g. #(.W(8))");
            const Token name = identifier("expected a parameter name");
            uint64_t value = 0;
            if (takeIf("(")) {
                value = constantOf("a parameter override must be a constant");
                expect(")");
            } else {
                uint32_t symbol = 0;
                bool known = false;
                for (auto scope = scopes_.rbegin(); scope != scopes_.rend() && !known; ++scope) {
                    const auto found = scope->find(name.text);
                    if (found != scope->end()) { symbol = found->second; known = true; }
                }
                if (!known) {
                    const auto found = result_->byName.find(name.text);
                    if (found != result_->byName.end()) { symbol = found->second; known = true; }
                }
                const auto bound = known ? constants_.find(symbol) : constants_.end();
                if (bound == constants_.end())
                    error(name, "'.W' needs a constant named '" + name.text + "' here");
                value = bound->second;
            }
            (*overrides)[name.text] = value;
            if (takeIf(")")) break;
            expect(",");
        }
    }

    /* The connection list, up to (not including) the closing ')'.  Named and
       positional forms cannot be mixed; an empty form, `( )`, leaves every port
       unconnected (and therefore undriven). */
    std::vector<PortConnection> parseConnections() {
        std::vector<PortConnection> connections;
        if (is(")")) return connections;
        bool named = false, positional = false;
        for (;;) {
            PortConnection connection;
            if (is(".")) {
                named = true;
                if (positional) error(peek(), "do not mix named and positional port connections");
                take();
                const Token port = identifier("expected a port name after '.'");
                connection.name = port.text;
                if (takeIf("(")) {
                    if (!is(")")) connection.value = parseExpression();
                    expect(")");
                } else {
                    /* `.a` shorthand: connect the net with the same name. */
                    const auto found = result_->byName.find(port.text);
                    if (found == result_->byName.end())
                        error(port, "unknown signal '" + port.text + "'");
                    std::unique_ptr<Expr> value(new Expr);
                    value->kind = ExprKind::Symbol;
                    value->symbol = found->second;
                    connection.value = std::move(value);
                }
            } else {
                positional = true;
                if (named) error(peek(), "do not mix named and positional port connections");
                if (!is(",") && !is(")")) connection.value = parseExpression();
            }
            connections.push_back(std::move(connection));
            if (!takeIf(",")) break;
        }
        return connections;
    }

    /* Append a driver for one bit range of a wire, refusing overlapping ones -
       the rule a plain `assign` follows, shared by instances. */
    void addDriver(uint32_t target, uint32_t shift, uint32_t width,
                   std::unique_ptr<Expr> value, const Token& where) {
        const Symbol& symbol = result_->symbols[target];
        if (uint64_t(shift) + width > symbol.width || !width)
            error(where, "the driven bits do not fit inside '" + symbol.name + "'");
        const std::vector<size_t> existing = result_->driversBySymbol[target];
        for (size_t index : existing) {
            const Continuous& other = result_->continuous[index];
            if (other.shift < shift + width && shift < other.shift + other.width)
                error(where, "these bits of '" + symbol.name + "' already have a continuous driver");
        }
        Continuous driver;
        driver.target = target; driver.shift = shift; driver.width = width;
        driver.value = std::move(value);
        result_->continuous.push_back(std::move(driver));
        result_->driversBySymbol[target].push_back(result_->continuous.size() - 1);
    }

    void remapExpr(Expr& expr, uint32_t base) const {
        if (expr.kind == ExprKind::Symbol || expr.kind == ExprKind::Memory) expr.symbol += base;
        if (expr.a) remapExpr(*expr.a, base);
        if (expr.b) remapExpr(*expr.b, base);
        if (expr.c) remapExpr(*expr.c, base);
        for (auto& part : expr.parts) remapExpr(*part, base);
    }

    void remapStatement(Statement& statement, uint32_t base) const {
        if (statement.condition) remapExpr(*statement.condition, base);
        if (statement.target) remapExpr(*statement.target, base);
        if (statement.value) remapExpr(*statement.value, base);
        if (statement.yes) remapStatement(*statement.yes, base);
        if (statement.no) remapStatement(*statement.no, base);
        for (Statement& child : statement.children) remapStatement(child, base);
        for (auto& item : statement.items) {
            for (auto& label : item->labels) remapExpr(*label, base);
            if (item->body) remapStatement(*item->body, base);
        }
    }

    /* An output port drives a net: accept a whole wire, or a constant bit/part
       select of one. */
    bool wireTarget(const Expr& expr, uint32_t* symbol, uint32_t* shift, uint32_t* width) const {
        const Expr* root = &expr;
        while (root->kind == ExprKind::Select || root->kind == ExprKind::Slice ||
               root->kind == ExprKind::Indexed)
            root = root->a.get();
        if (!root || root->kind != ExprKind::Symbol) return false;
        const Symbol& target = result_->symbols[root->symbol];
        if (target.kind != SymbolKind::Wire) return false;
        *symbol = root->symbol; *shift = 0; *width = target.width;
        if (root == &expr) return true;
        return constantSelect(expr, shift, width);
    }

    /* Splice an elaborated child module into the module being built.  The child
       is always parsed fresh, so its symbol ids are 0..n-1 and land in the
       parent at a constant offset - that offset is the whole remap.  Its ports
       become drivers: inputs from the connection expressions, outputs into the
       connected nets. */
    void inlineModule(const ModuleSpan& target, std::unique_ptr<Program::Impl> child,
                      std::vector<PortConnection>& connections, const Token& where) {
        const uint32_t base = static_cast<uint32_t>(result_->symbols.size());
        const uint32_t stateBase = result_->stateWords;
        if (uint64_t(result_->stateWords) + child->stateWords > options_.max_state_words)
            error(where, "state word limit exceeded while instantiating '" + target.name + "'");
        std::vector<uint32_t> portSymbols;
        for (const Port& port : child->publicPorts) {
            const auto found = child->byName.find(port.name);
            portSymbols.push_back(found == child->byName.end() ? 0u : found->second);
        }
        for (Symbol symbol : child->symbols) {
            /* An input port becomes an ordinary wire: the connection drives it. */
            if (symbol.kind == SymbolKind::Input) symbol.kind = SymbolKind::Wire;
            symbol.inputIndex = -1;
            symbol.outputIndex = -1;
            if (symbol.kind == SymbolKind::Reg || symbol.kind == SymbolKind::Memory)
                symbol.stateOffset += stateBase;
            result_->symbols.push_back(std::move(symbol));
        }
        result_->stateWords += child->stateWords;
        for (Continuous& driver : child->continuous) {
            driver.target += base;
            remapExpr(*driver.value, base);
            addDriver(driver.target, driver.shift, driver.width, std::move(driver.value), where);
        }
        for (AlwaysBlock& block : child->always) {
            for (Sensitivity& event : block.events) {
                event.symbol += base;
                event.stateIndex += stateBase;
            }
            remapStatement(block.body, base);
            result_->always.push_back(std::move(block));
        }
        /* Hierarchical names for this instance: its own signals, plus whatever
           it in turn instantiated.  The first use of a name is also reachable
           without an index; instances inside a generate loop share a name, so
           the second and later ones are only reachable as `name[1]` onwards. */
        const uint32_t occurrence = instanceCounts_[where.text]++;
        const std::string bare = where.text + ".";
        const std::string indexed = where.text + "[" + std::to_string(occurrence) + "].";
        for (uint32_t index = 0; index < child->symbols.size(); ++index) {
            const Symbol& symbol = child->symbols[index];
            if (symbol.kind == SymbolKind::Parameter || symbol.kind == SymbolKind::LoopVar)
                continue;
            if (occurrence == 0) result_->hierarchy[bare + symbol.name] = base + index;
            result_->hierarchy[indexed + symbol.name] = base + index;
        }
        for (const auto& entry : child->hierarchy) {
            if (occurrence == 0) result_->hierarchy[bare + entry.first] = entry.second + base;
            result_->hierarchy[indexed + entry.first] = entry.second + base;
        }
        for (size_t index = 0; index < connections.size(); ++index) {
            PortConnection& connection = connections[index];
            const Port* port = nullptr;
            if (!connection.name.empty()) {
                for (const Port& candidate : child->publicPorts)
                    if (candidate.name == connection.name) port = &candidate;
                if (!port)
                    error(where, "module '" + target.name + "' has no port named '" +
                                     connection.name + "'");
            } else {
                if (index >= child->publicPorts.size())
                    error(where, "too many positional connections for module '" + target.name + "'");
                port = &child->publicPorts[index];
            }
            if (!connection.value) continue;   /* unconnected: left undriven */
            size_t portIndex = static_cast<size_t>(port - child->publicPorts.data());
            std::unique_ptr<Expr> value = std::move(connection.value);
            if (port->direction == PortDirection::Input) {
                addDriver(portSymbols[portIndex] + base, 0, port->bits, std::move(value), where);
                continue;
            }
            uint32_t symbol = 0, shift = 0, width = 0;
            if (!wireTarget(*value, &symbol, &shift, &width))
                error(where, "an output port must be connected to a wire or a wire bit/part select");
            std::unique_ptr<Expr> portValue(new Expr);
            portValue->kind = ExprKind::Symbol;
            portValue->symbol = portSymbols[portIndex] + base;
            addDriver(symbol, shift, width, std::move(portValue), where);
        }
    }

    /* Where a for header's parts are.  Found with a lexical scan (nested
       parentheses tracked) so the body can be parsed exactly once per
       iteration instead of being discovered by a throwaway parse. */
    struct ForHeader { size_t condition = 0, step = 0, end = 0; };

    ForHeader scanForHeader(const Token& loopToken) {
        if (!is("(")) error(peek(), "expected '(' after for");
        ForHeader header;
        int depth = 1, semicolons = 0;
        for (size_t index = cursor_ + 1; index < tokens_.size(); ++index) {
            const std::string& text = tokens_[index].text;
            if (text == "(") { ++depth; continue; }
            if (text == ")") {
                if (--depth == 0) { header.end = index; break; }
                continue;
            }
            if (text != ";" || depth != 1) continue;
            if (++semicolons == 1) header.condition = index + 1;
            else if (semicolons == 2) header.step = index + 1;
        }
        if (!header.condition || !header.step || !header.end)
            error(loopToken, "malformed for loop; expected for (i = 0; condition; i = i + 1)");
        return header;
    }

    /* Parse an expression and fold it; it has to be a compile-time constant. */
    Bits constantBits(const char* what) {
        const Token start = peek();
        std::unique_ptr<Expr> expr = parseExpression();
        Bits value{};
        if (!fold(*expr, &value) || !value.fullyKnown()) error(start, what);
        return value;
    }

    uint64_t constantOf(const char* what) { return constantBits(what).value; }

    /* ---- generate if / case ---------------------------------------------- */

    /* Everything that parsing a branch may have changed, so a branch the
       condition did not select can be parsed for syntax and then undone. */
    struct ModuleSnapshot {
        size_t symbols = 0, publicPorts = 0, continuous = 0, always = 0, outputSymbols = 0;
        uint32_t stateWords = 0, inputCount = 0, outputCount = 0, inputBits = 0;
        ConstantValues constants;
        std::unordered_set<std::string> usedOverrides;
        std::unordered_map<std::string, uint32_t> hierarchy;
        std::unordered_map<std::string, uint32_t> instances;
        std::vector<std::pair<uint32_t, size_t>> drivers;
    };

    ModuleSnapshot snapshotModule() const {
        ModuleSnapshot state;
        state.symbols = result_->symbols.size();
        state.publicPorts = result_->publicPorts.size();
        state.continuous = result_->continuous.size();
        state.always = result_->always.size();
        state.outputSymbols = result_->outputSymbols.size();
        state.stateWords = result_->stateWords;
        state.inputCount = result_->inputCount;
        state.outputCount = result_->outputCount;
        state.inputBits = result_->inputBits;
        state.constants = constants_;
        state.usedOverrides = usedOverrides_;
        state.hierarchy = result_->hierarchy;
        state.instances = instanceCounts_;
        for (const auto& entry : result_->driversBySymbol)
            state.drivers.push_back({entry.first, entry.second.size()});
        return state;
    }

    void restoreModule(const ModuleSnapshot& state) {
        result_->symbols.resize(state.symbols);
        result_->publicPorts.resize(state.publicPorts);
        result_->continuous.resize(state.continuous);
        result_->always.resize(state.always);
        result_->outputSymbols.resize(state.outputSymbols);
        result_->stateWords = state.stateWords;
        result_->inputCount = state.inputCount;
        result_->outputCount = state.outputCount;
        result_->inputBits = state.inputBits;
        constants_ = state.constants;
        usedOverrides_ = state.usedOverrides;
        result_->hierarchy = state.hierarchy;
        instanceCounts_ = state.instances;
        for (auto entry = result_->byName.begin(); entry != result_->byName.end();)
            if (entry->second >= state.symbols) entry = result_->byName.erase(entry);
            else ++entry;
        /* A driver list goes back to its old length, and an entry the discarded
           branch created (for a symbol that had none before) is dropped
           entirely - otherwise the bits it drove would look occupied. */
        std::unordered_map<uint32_t, size_t> previous;
        for (const auto& saved : state.drivers) previous.emplace(saved.first, saved.second);
        for (auto entry = result_->driversBySymbol.begin(); entry != result_->driversBySymbol.end();) {
            if (entry->first >= state.symbols) {
                entry = result_->driversBySymbol.erase(entry);
                continue;
            }
            const auto found = previous.find(entry->first);
            if (found == previous.end()) {
                entry = result_->driversBySymbol.erase(entry);
                continue;
            }
            if (entry->second.size() > found->second) entry->second.resize(found->second);
            ++entry;
        }
    }

    /* Parse a branch the condition did not select - its syntax is still
       checked - and put the module back the way it was. */
    void discardGenerateBody() {
        const ModuleSnapshot saved = snapshotModule();
        try {
            parseGenerateBody();
        } catch (...) {
            restoreModule(saved);
            throw;
        }
        restoreModule(saved);
    }

    /* `if (constant) <body> [else <body>]` at module level, `else if` included.
       Every body is syntax-checked against the state the chain started in and
       then undone; the selected one is parsed last, so the alternatives cannot
       conflict with it (two branches may declare the same name or drive the
       same net - only one of them exists). */
    void parseGenerateIf() {
        std::vector<size_t> bodies;
        int chosen = -1;
        for (;;) {
            take();   /* if */
            expect("(");
            const uint64_t condition = constantOf("a generate condition must be constant");
            expect(")");
            bodies.push_back(cursor_);
            discardGenerateBody();
            if (chosen < 0 && condition) chosen = static_cast<int>(bodies.size() - 1);
            if (!takeIf("else")) break;
            if (!is("if")) {
                bodies.push_back(cursor_);
                discardGenerateBody();
                if (chosen < 0) chosen = static_cast<int>(bodies.size() - 1);
                break;
            }
        }
        const size_t afterChain = cursor_;
        if (chosen >= 0) {
            cursor_ = bodies[static_cast<size_t>(chosen)];
            parseGenerateBody();
        }
        cursor_ = afterChain;
    }

    /* `case` / `casez` / `casex` at module level.  The default may sit anywhere,
       so the items are scanned first (each body only syntax-checked) and the
       selected body is parsed afterwards. */
    void parseGenerateCase() {
        const std::string keyword = take().text;
        const bool wildX = keyword != "case";
        const bool wildZ = keyword != "case";
        const bool subjectWildX = keyword == "casex";
        expect("(");
        const Bits subject = constantBits("a generate case expression must be constant");
        expect(")");
        struct Item { bool isDefault = false, matches = false; size_t body = 0; };
        std::vector<Item> items;
        while (!is("endcase")) {
            if (peek().kind == TokenKind::End || is("endmodule") || is("endgenerate"))
                error(peek(), "unterminated generate case; expected 'endcase'");
            Item item;
            if (takeIf("default")) {
                item.isDefault = true;
                takeIf(":");
            } else {
                for (;;) {
                    const Bits label = constantBits("a generate case label must be constant");
                    if (caseMatches(subject, label, wildX, wildZ, wildZ, subjectWildX))
                        item.matches = true;
                    if (!takeIf(",")) break;
                }
                expect(":");
            }
            item.body = cursor_;
            discardGenerateBody();
            items.push_back(item);
        }
        const size_t afterCase = cursor_ + 1;
        take();   /* endcase */
        int chosen = -1, fallback = -1;
        for (size_t index = 0; index < items.size(); ++index) {
            if (items[index].isDefault) {
                if (fallback < 0) fallback = static_cast<int>(index);
            } else if (items[index].matches && chosen < 0) {
                chosen = static_cast<int>(index);
            }
        }
        const int selected = chosen >= 0 ? chosen : fallback;
        if (selected >= 0) {
            cursor_ = items[static_cast<size_t>(selected)].body;
            parseGenerateBody();
        }
        cursor_ = afterCase;
    }

    /* `for (i = <const>; <const condition>; i = <const step>) <body>`.  The loop
       variable is bound to each iteration's value while the body is parsed, so
       every use of it folds to a literal - a part-select bound, an array index,
       a bit select or plain arithmetic.  `emit` parses one body; `discardable`
       says whether a body parsed for a loop that turns out to run zero times
       may simply be dropped (a statement can, a generate body cannot: it
       declares and emits into the module). */
    template <typename Emit>
    void unrollLoop(bool discardable, Emit emit) {
        expect("for");
        const Token loopToken = previous_;
        const ForHeader header = scanForHeader(loopToken);
        expect("(");   /* scanForHeader only looked ahead */
        const Token varToken = identifier("expected a loop variable");
        const auto declared = result_->byName.find(varToken.text);
        if (declared == result_->byName.end()) {
            if (varToken.text == "genvar" || varToken.text == "integer" || varToken.text == "int")
                error(varToken, "declare the loop variable before the loop, e.g. 'integer i;' "
                                "or 'genvar i;'");
            error(varToken, "unknown signal '" + varToken.text + "'");
        }
        const uint32_t id = declared->second;
        if (result_->symbols[id].kind != SymbolKind::LoopVar)
            error(varToken, "the loop variable must be declared with integer or genvar");
        expect("=");
        uint64_t value = constantOf("the for loop's start value must be constant");
        expect(";");
        const size_t bodyStart = header.end + 1;
        /* A nested loop may reuse the name, so the outer binding is restored. */
        const auto outer = constants_.find(id);
        const bool hadOuter = outer != constants_.end();
        const uint64_t outerValue = hadOuter ? outer->second : 0;
        size_t bodyEnd = bodyStart;
        try {
            for (uint32_t iteration = 0;; ++iteration) {
                if (iteration >= kMaxUnroll)
                    error(loopToken, "the for loop does not finish within 4096 unrolled iterations");
                constants_[id] = value;
                cursor_ = header.condition;
                const uint64_t condition = constantOf("the for loop's condition must be constant");
                expect(";");
                if (!condition) {
                    if (iteration == 0) {
                        if (!discardable)
                        error(loopToken, "a generate loop must run at least once; "
                                         "the kernel unrolls it while compiling");
                        cursor_ = bodyStart;
                        scopes_.emplace_back();
                        emit();
                        scopes_.pop_back();
                        bodyEnd = cursor_;
                    }
                    break;
                }
                cursor_ = bodyStart;
                /* Every unrolled iteration is its own scope, so a signal
                   declared in the body belongs to that iteration alone. */
                scopes_.emplace_back();
                emit();
                scopes_.pop_back();
                bodyEnd = cursor_;
                cursor_ = header.step;
                const Token stepVar = identifier("expected the loop variable in the for step");
                if (lookup(stepVar) != id)
                    error(stepVar, "the for step must assign the loop variable");
                expect("=");
                value = constantOf("the for step must be a constant expression");
                expect(")");
            }
        } catch (...) {
            if (hadOuter) constants_[id] = outerValue; else constants_.erase(id);
            throw;
        }
        if (hadOuter) constants_[id] = outerValue; else constants_.erase(id);
        cursor_ = bodyEnd;
    }

    uint32_t lookup(const Token& name) {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->find(name.text);
            if (found != scope->end()) return found->second;
        }
        const auto found = result_->byName.find(name.text);
        if (found == result_->byName.end()) error(name, "unknown signal '" + name.text + "'");
        return found->second;
    }

    void parseContinuous() {
        take();
        const Token name = identifier("expected assign target");
        const uint32_t target = lookup(name);
        const Symbol& symbol = result_->symbols[target];
        if (symbol.kind != SymbolKind::Wire) error(name, "continuous assignment target must be a wire or wire output");
        std::unique_ptr<Expr> base(new Expr); base->kind = ExprKind::Symbol; base->symbol = target;
        std::unique_ptr<Expr> lvalue = parseSelectors(std::move(base), name);
        uint32_t shift = 0, width = symbol.width;
        if (lvalue->kind != ExprKind::Symbol && !constantSelect(*lvalue, &shift, &width))
            error(name, "a bit or part select in a continuous assignment needs constant bounds");
        expect("=");
        auto value = parseExpression();
        expect(";");
        addDriver(target, shift, width, std::move(value), name);
    }

    /* `always` / `always_ff` with an edge list, or `always_comb` / `always @(*)`
       / `always @(a or b)` for a combinational block. */
    void parseAlways() {
        const Token keyword = take();
        AlwaysBlock block;
        bool combinational = keyword.text == "always_comb";
        if (keyword.text == "always_comb") {
            if (is("@")) error(peek(), "always_comb takes no sensitivity list");
        } else {
            if (!is("@")) error(peek(), "expected '@(...)' after " + keyword.text);
            take();
            if (takeIf("*")) {
                combinational = true;              /* always @* */
            } else {
                expect("(");
                if (takeIf("*")) {
                    expect(")");
                    combinational = true;          /* always @(*) */
                } else {
                    bool hasEdge = false;
                    for (;;) {
                        if (is("posedge") || is("negedge")) {
                            const bool rising = take().text == "posedge";
                            const Token name = identifier("expected a signal after posedge/negedge");
                            const uint32_t symbol = lookup(name);
                            if (result_->symbols[symbol].width != 1)
                                error(name, "an edge-triggered signal must be one bit wide");
                            Sensitivity event;
                            event.symbol = symbol;
                            event.rising = rising;
                            block.events.push_back(event);
                            hasEdge = true;
                        } else {
                            /* A bare signal is a level-sensitive entry: the whole
                               block is combinational. */
                            const Token name = identifier("expected a signal in the sensitivity list");
                            lookup(name);
                        }
                        if (takeIf(")")) break;
                        if (!takeIf("or") && !takeIf(","))
                            error(peek(), "expected 'or' or ',' between sensitivity list entries");
                    }
                    combinational = !hasEdge;
                }
            }
        }
        if (keyword.text == "always_ff" && combinational)
            error(keyword, "always_ff needs a posedge/negedge sensitivity list");
        block.kind = combinational ? AlwaysBlock::Kind::Combinational : AlwaysBlock::Kind::Clocked;
        const bool savedCombinational = inCombinationalBlock_;
        inCombinationalBlock_ = combinational;
        block.body = parseStatement();
        inCombinationalBlock_ = savedCombinational;
        /* A combinational block owns no edge state.  A clocked block's entries
           each get a state word here, while this module is being parsed: a block
           inlined from a child module already owns its words, so it must not be
           handed new ones by the parent. */
        if (!combinational)
            for (Sensitivity& event : block.events) {
                if (result_->stateWords >= options_.max_state_words)
                    error(previous_, "state word limit exceeded");
                event.stateIndex = result_->stateWords++;
            }
        result_->always.push_back(std::move(block));
    }

    Statement parseStatement() {
        if (++statementDepth_ > options_.max_expression_depth) error(peek(), "statement nesting limit exceeded");
        struct Drop { uint32_t& value; ~Drop() { --value; } } drop{statementDepth_};
        if (takeIf("begin")) {
            Statement statement; statement.kind = Statement::Kind::Block;
            while (!takeIf("end")) {
                if (peek().kind == TokenKind::End) error(peek(), "unterminated begin block");
                statement.children.push_back(parseStatement());
            }
            return statement;
        }
        if (takeIf("if")) {
            Statement statement; statement.kind = Statement::Kind::If;
            expect("("); statement.condition = parseExpression(); expect(")");
            statement.yes.reset(new Statement(parseStatement()));
            if (takeIf("else")) statement.no.reset(new Statement(parseStatement()));
            return statement;
        }
        if (is("case") || is("casez") || is("casex")) return parseCase();
        if (is("for")) {
            Statement statement;
            statement.kind = Statement::Kind::Block;
            unrollLoop(true, [&] { statement.children.push_back(parseStatement()); });
            return statement;
        }
        if (is("endcase") || is("end") || is("else"))
            error(peek(), "expected a statement");
        const Token name = identifier("expected procedural statement");
        /* `u0.sig` names another instance's signal: readable, but not
           assignable from here. */
        {
            bool local = result_->byName.count(name.text) != 0;
            for (auto scope = scopes_.rbegin(); scope != scopes_.rend() && !local; ++scope)
                if (scope->count(name.text)) local = true;
            if (!local && is("."))
                error(name, "a hierarchical name may only be read, not assigned to");
        }
        Statement statement; statement.kind = Statement::Kind::Nonblocking;
        const uint32_t id = lookup(name);
        const Symbol& symbol = result_->symbols[id];
        if (symbol.kind != SymbolKind::Reg && symbol.kind != SymbolKind::Memory)
            error(name, "procedural assignment target must be reg, logic, or a memory element");
        std::unique_ptr<Expr> target(new Expr); target->symbol = id;
        if (symbol.kind == SymbolKind::Memory) {
            if (!is("[")) error(name, "memory assignment requires an index");
            take(); target->kind = ExprKind::Memory; target->a = parseExpression(); expect("]");
        } else {
            target->kind = ExprKind::Symbol;
        }
        statement.target = parseSelectors(std::move(target), name);
        /* Which kind of block this is decides the assignment operator: a
           combinational block assigns immediately ('='), a clocked block
           schedules a non-blocking update ('<=').  Mixing them inside one block
           is refused rather than guessed at. */
        if (inCombinationalBlock_) {
            if (!takeIf("=")) {
                if (takeIf("<=")) error(previous_, "use '=' in a combinational block");
                error(peek(), "expected assignment '='");
            }
        } else if (!takeIf("<=")) {
            if (takeIf("="))
                error(previous_, "blocking assignments are not supported; use <= in clocked logic");
            error(peek(), "expected nonblocking assignment '<='");
        }
        /* One writer per variable, and never one of each kind. */
        const Expr* root = statement.target.get();
        while (root->kind == ExprKind::Select || root->kind == ExprKind::Slice ||
               root->kind == ExprKind::Indexed)
            root = root->a.get();
        Symbol& written = result_->symbols[root->symbol];
        const bool clash = inCombinationalBlock_ ? written.writtenByClocked
                                                 : written.writtenByCombinational;
        if (clash)
            error(name, "'" + written.name + "' is assigned from both a combinational "
                        "and a clocked block");
        if (inCombinationalBlock_) written.writtenByCombinational = true;
        else written.writtenByClocked = true;
        statement.value = parseExpression(); expect(";");
        return statement;
    }

    /* case / casez / casex; the keyword is the current token.  Item labels keep
       their source form, so the evaluator can honour the wildcards each style
       is defined to have. */
    Statement parseCase() {
        const std::string keyword = take().text;
        Statement statement;
        statement.kind = Statement::Kind::Case;
        statement.style = keyword == "casez" ? Statement::CaseStyle::Z
                        : keyword == "casex" ? Statement::CaseStyle::X
                                             : Statement::CaseStyle::Plain;
        expect("("); statement.condition = parseExpression(); expect(")");
        bool sawDefault = false;
        while (!is("endcase")) {
            if (peek().kind == TokenKind::End || is("endmodule"))
                error(peek(), "unterminated case statement; expected 'endcase'");
            std::unique_ptr<CaseItem> item(new CaseItem);
            if (takeIf("default")) {
                if (sawDefault) error(previous_, "a case statement may have only one default item");
                sawDefault = true;
                takeIf(":");   /* Verilog lets the default's colon be omitted */
            } else {
                for (;;) {
                    item->labels.push_back(parseExpression());
                    if (!takeIf(",")) break;
                }
                expect(":");
            }
            item->body.reset(new Statement(parseStatement()));
            statement.items.push_back(std::move(item));
        }
        take();   /* endcase */
        return statement;
    }

    std::unique_ptr<Expr> parseExpression(int minPrecedence = 1) {
        if (++expressionDepth_ > options_.max_expression_depth) error(peek(), "expression nesting limit exceeded");
        auto left = parseUnary();
        for (;;) {
            const int precedence = binaryPrecedence(peek().text);
            if (precedence < minPrecedence) break;
            const std::string op = take().text;
            auto right = parseExpression(precedence + 1);
            std::unique_ptr<Expr> combined(new Expr);
            combined->kind = ExprKind::Binary; combined->op = op;
            combined->a = std::move(left); combined->b = std::move(right);
            left = std::move(combined);
        }
        if (minPrecedence == 1 && takeIf("?")) {
            std::unique_ptr<Expr> combined(new Expr);
            combined->kind = ExprKind::Conditional; combined->a = std::move(left);
            combined->b = parseExpression(); expect(":"); combined->c = parseExpression();
            left = std::move(combined);
        }
        --expressionDepth_;
        return left;
    }

    int binaryPrecedence(const std::string& op) const {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 7;
        if (op == "<<" || op == ">>" || op == "<<<" || op == ">>>") return 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        return 0;
    }

    std::unique_ptr<Expr> parseUnary() {
        if (++unaryDepth_ > options_.max_expression_depth) error(peek(), "unary expression nesting limit exceeded");
        struct Drop { uint32_t& value; ~Drop() { --value; } } drop{unaryDepth_};
        if (is("~") || is("!") || is("+") || is("-")) {
            const std::string op = take().text;
            std::unique_ptr<Expr> expr(new Expr); expr->kind = ExprKind::Unary; expr->op = op;
            expr->a = parseUnary(); return expr;
        }
        if (takeIf("(")) { auto expr = parseExpression(); expect(")"); return expr; }
        if (takeIf("{")) return parseBrace();
        if (peek().kind == TokenKind::Number) {
            const Token token = take();
            std::unique_ptr<Expr> expr(new Expr); expr->kind = ExprKind::Literal;
            expr->literal = parseLiteral(token); return expr;
        }
        const Token name = identifier("expected expression");
        /* $signed / $unsigned: a cast that only changes how the value widens
           and how it compares. */
        if (name.text == "$signed" || name.text == "$unsigned") {
            if (!takeIf("(")) error(peek(), std::string("expected '(' after ") + name.text);
            std::unique_ptr<Expr> expr(new Expr);
            expr->kind = ExprKind::Cast;
            expr->op = name.text;
            expr->a = parseExpression();
            expect(")");
            return expr;
        }
        /* A dotted (or indexed) path that does not name a local signal is a
           hierarchical reference into an inlined instance. */
        {
            bool local = false;
            for (auto scope = scopes_.rbegin(); scope != scopes_.rend() && !local; ++scope)
                if (scope->count(name.text)) local = true;
            if (!local && result_->byName.count(name.text)) local = true;
            if (!local && (is(".") || is("["))) {
                std::string path = name.text;
                for (;;) {
                    if (is(".") && peek(1).kind == TokenKind::Identifier) {
                        take();
                        path += "." + take().text;
                        continue;
                    }
                    if (is("[")) {
                        take();
                        path += "[" + std::to_string(
                                          constantOf("a hierarchical index must be constant")) + "]";
                        expect("]");
                        continue;
                    }
                    break;
                }
                const auto found = result_->hierarchy.find(path);
                if (found == result_->hierarchy.end())
                    error(name, "unknown hierarchical name '" + path + "'");
                std::unique_ptr<Expr> expr(new Expr);
                expr->kind = ExprKind::Symbol;
                expr->symbol = found->second;
                return expr;
            }
        }
        const uint32_t id = lookup(name);
        const Symbol& symbol = result_->symbols[id];
        if (symbol.kind == SymbolKind::LoopVar || symbol.kind == SymbolKind::Parameter) {
            /* A loop variable or a parameter is a compile-time integer: it turns
               into a literal here.  A loop variable outside its own loop has no
               value at all, so that case is refused. */
            const auto bound = constants_.find(id);
            if (bound == constants_.end()) {
                if (symbol.kind == SymbolKind::LoopVar)
                    error(name, "a loop variable may only be read inside its own for loop");
                error(name, "internal: the constant was not bound at its declaration");
            }
            std::unique_ptr<Expr> expr(new Expr);
            expr->kind = ExprKind::Literal;
            expr->literal = Bits::knownValue(bound->second, 32);
            return expr;
        }
        std::unique_ptr<Expr> expr(new Expr); expr->symbol = id;
        if (symbol.kind == SymbolKind::Memory) {
            if (!is("[")) error(name, "memory read requires an index");
            take(); expr->kind = ExprKind::Memory; expr->a = parseExpression(); expect("]");
        } else {
            expr->kind = ExprKind::Symbol;
        }
        return parseSelectors(std::move(expr), name);
    }

    Bits parseLiteral(const Token& token) {
        std::string text;
        for (char c : token.text) if (c != '_') text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const size_t quote = text.find('\'');
        if (quote == std::string::npos) {
            char* end = nullptr; const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
            if (!end || *end) error(token, "malformed decimal literal");
            return Bits::knownValue(static_cast<uint64_t>(value), 32);
        }
        if (quote == 0 && (text == "'0" || text == "'1" || text == "'x" || text == "'?" || text == "'z")) {
            if (text == "'0") return Bits::knownValue(0, 64);
            if (text == "'1") return Bits::knownValue(~uint64_t(0), 64);
            if (text == "'z") return Bits::highZ(64);
            return Bits::unknown(64);
        }
        uint32_t width = 0;
        if (quote) {
            char* end = nullptr; const unsigned long parsed = std::strtoul(text.substr(0, quote).c_str(), &end, 10);
            if (!end || *end || parsed == 0 || parsed > 64) error(token, "literal width must be 1..64");
            width = static_cast<uint32_t>(parsed);
        }
        std::string digits = text.substr(quote + 1);
        /* `8'shff`: the `s` marks the literal as signed, which makes a
           widening zero- or sign-extend differently. */
        bool isSigned = false;
        if (!digits.empty() && digits[0] == 's') {
            isSigned = true;
            digits.erase(digits.begin());
        }
        int base = 10;
        if (!digits.empty() && (digits[0] == 'b' || digits[0] == 'o' || digits[0] == 'd' || digits[0] == 'h')) {
            base = digits[0] == 'b' ? 2 : digits[0] == 'o' ? 8 : digits[0] == 'h' ? 16 : 10;
            digits.erase(digits.begin());
        }
        if (digits.empty()) error(token, "literal has no digits");
        if (!width) {
            if (digits == "z" || digits == "x" || digits == "?") width = 64;
            else width = 32;
        }
        uint64_t value = 0, known = 0, z = 0;
        uint32_t bit = 0;
        for (auto it = digits.rbegin(); it != digits.rend() && bit < width; ++it) {
            const char c = *it;
            const uint32_t group = base == 16 ? 4 : base == 8 ? 3 : base == 2 ? 1 : 0;
            if (!group) {
                char* end = nullptr; const unsigned long long decimal = std::strtoull(digits.c_str(), &end, 10);
                if (!end || *end) error(token, "malformed based literal");
                return Bits::knownValue(static_cast<uint64_t>(decimal), width, isSigned);
            }
            if (c == 'x' || c == '?' || c == 'z') {
                for (uint32_t n = 0; n < group && bit < width; ++n, ++bit)
                    if (c == 'z') z |= uint64_t(1) << bit;
                continue;
            }
            int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0 || digit >= base) error(token, "digit is invalid for literal base");
            for (uint32_t n = 0; n < group && bit < width; ++n, ++bit) {
                known |= uint64_t(1) << bit;
                if ((digit >> n) & 1) value |= uint64_t(1) << bit;
            }
        }
        /* Verilog pads a sized based literal from its most-significant digit:
           ordinary digits with zero, x with X and z with Z. */
        const char leading = digits.front();
        while (bit < width) {
            if (leading == 'z') z |= uint64_t(1) << bit;
            else if (leading != 'x' && leading != '?') known |= uint64_t(1) << bit;
            ++bit;
        }
        return {value & widthMask(width), known & widthMask(width), z & widthMask(width), width,
                isSigned};
    }
};

struct EvalContext {
    const Program::Impl& program;
    const TCLogicIOV3& io;
    std::vector<uint64_t>& scratch;
    std::vector<uint8_t>& resolving;

    Bits signal(uint32_t id);
    Bits expression(const Expr& expr);
};

enum class Truth { False, True, Unknown };
Truth logicalTruth(const Bits& value) {
    if (value.truthy()) return Truth::True;
    return value.fullyKnown() ? Truth::False : Truth::Unknown;
}

Bits EvalContext::signal(uint32_t id) {
    const Symbol& symbol = program.symbols[id];
    if (symbol.kind == SymbolKind::Input) {
        if (symbol.inputIndex < 0 || static_cast<uint32_t>(symbol.inputIndex) >= io.input_count || !io.inputs)
            return Bits::highZ(symbol.width);
        const uint64_t mask = widthMask(symbol.width);
        const TCLogicValueV3& input = io.inputs[symbol.inputIndex];
        const uint64_t drive = input.drive_mask & mask;
        return {input.value & drive, drive, mask & ~drive, symbol.width, symbol.isSigned};
    }
    if (symbol.kind == SymbolKind::Reg) {
        const uint64_t* storage = symbol.temporary ? scratch.data() : io.state;
        const uint32_t words = symbol.temporary ? static_cast<uint32_t>(scratch.size())
                                                : io.state_words;
        if (!storage || symbol.stateOffset >= words)
            return Bits::unknown(symbol.width, symbol.isSigned);
        return Bits::knownValue(storage[symbol.stateOffset], symbol.width, symbol.isSigned);
    }
    if (symbol.kind == SymbolKind::Memory) return Bits::unknown(symbol.width, symbol.isSigned);
    /* Loop variables and parameters are folded away while compiling; reaching
       one here would mean the compiler let a use through, so answer X rather
       than read state that the symbol does not own. */
    if (symbol.kind == SymbolKind::LoopVar || symbol.kind == SymbolKind::Parameter)
        return Bits::unknown(symbol.width);
    const auto found = program.driversBySymbol.find(id);
    if (found == program.driversBySymbol.end() || found->second.empty())
        return Bits::highZ(symbol.width);
    if (resolving[id]) return Bits::unknown(symbol.width);
    resolving[id] = 1;
    /* Place every driver's bits.  The parser already refuses two drivers that
       overlap, so in practice each bit has one owner; the merge is written for
       the general case anyway, with Verilog's resolution rule (two drivers that
       disagree resolve to X, a bit no driver touches stays Z).  A single
       full-width driver therefore comes out exactly as before. */
    const uint64_t mask = widthMask(symbol.width);
    uint64_t one = 0, zero = 0, x = 0;
    for (size_t index : found->second) {
        const Continuous& driver = program.continuous[index];
        const Bits piece = expression(*driver.value).resized(driver.width);
        const uint64_t pieceMask = widthMask(driver.width);
        one |= ((piece.known & piece.value) & pieceMask) << driver.shift;
        zero |= ((piece.known & ~piece.value) & pieceMask) << driver.shift;
        x |= ((~piece.known & ~piece.z) & pieceMask) << driver.shift;
    }
    resolving[id] = 0;
    const uint64_t oneBits = one & mask;
    const uint64_t zeroBits = zero & mask;
    const uint64_t conflict = oneBits & zeroBits;
    const uint64_t definiteOne = oneBits & ~conflict;
    const uint64_t definiteZero = zeroBits & ~conflict;
    const uint64_t xBits = (conflict | x) & mask & ~(definiteOne | definiteZero);
    return {definiteOne, definiteOne | definiteZero,
            mask & ~(definiteOne | definiteZero | xBits), symbol.width, symbol.isSigned};
}

Bits bitwise(const std::string& op, Bits a, Bits b) {
    const uint32_t width = std::max(a.width, b.width);
    a = a.resized(width); b = b.resized(width);
    const uint64_t mask = widthMask(width);
    const uint64_t a0 = a.known & ~a.value & mask, a1 = a.known & a.value;
    const uint64_t b0 = b.known & ~b.value & mask, b1 = b.known & b.value;
    if (op == "&") { const uint64_t zero = a0 | b0, one = a1 & b1; return {one, zero | one, 0, width}; }
    if (op == "|") { const uint64_t one = a1 | b1, zero = a0 & b0; return {one, zero | one, 0, width}; }
    const uint64_t known = a.known & b.known & mask;
    return {(a.value ^ b.value) & known, known, 0, width};
}

/* Compile-time evaluation: literals and the operators over them.  This mirrors
   the runtime operators below on purpose - the parser uses it for part-select
   bounds and replication counts, where Verilog also demands a constant. */
bool foldConstant(const Expr& expr, Bits* out, const ConstantValues* constants) {
    Bits value;
    if (expr.kind == ExprKind::Literal) {
        value = expr.literal;
    } else if (expr.kind == ExprKind::Symbol) {
        /* A parameter, or a loop variable inside its own loop body, is an
           integer the compiler knows. */
        if (!constants) return false;
        const auto found = constants->find(expr.symbol);
        if (found == constants->end()) return false;
        value = Bits::knownValue(found->second, 32);
    } else if (expr.kind == ExprKind::Unary) {
        Bits a{};
        if (!foldConstant(*expr.a, &a, constants) || !a.fullyKnown()) return false;
        if (expr.op == "~") value = Bits::knownValue(~a.value, a.width, a.isSigned);
        else if (expr.op == "!") value = Bits::knownValue(a.truthy() ? 0 : 1, 1);
        else if (expr.op == "-")
            value = Bits::knownValue(uint64_t(0) - a.value, a.width, a.isSigned);
        else if (expr.op == "+") value = a;
        else return false;
    } else if (expr.kind == ExprKind::Binary) {
        Bits a{}, b{};
        if (!foldConstant(*expr.a, &a, constants) || !foldConstant(*expr.b, &b, constants) ||
            !a.fullyKnown() || !b.fullyKnown())
            return false;
        const bool comparison = expr.op == "==" || expr.op == "!=" || expr.op == "<" ||
                                expr.op == "<=" || expr.op == ">" || expr.op == ">=";
        const uint32_t width = std::max(a.width, b.width);
        const bool signedOperands = a.isSigned && b.isSigned;
        a = a.resized(width); b = b.resized(width);
        const uint64_t av = signedOperands ? signExtended(a.value, width) : a.value;
        const uint64_t bv = signedOperands ? signExtended(b.value, width) : b.value;
        const auto ordered = [signedOperands](uint64_t x, uint64_t y, const std::string& op) {
            if (!signedOperands) {
                if (op == "<") return x < y;
                if (op == "<=") return x <= y;
                if (op == ">") return x > y;
                return x >= y;
            }
            const int64_t sx = static_cast<int64_t>(x), sy = static_cast<int64_t>(y);
            if (op == "<") return sx < sy;
            if (op == "<=") return sx <= sy;
            if (op == ">") return sx > sy;
            return sx >= sy;
        };
        uint64_t result = 0;
        if (expr.op == "+") result = av + bv;
        else if (expr.op == "-") result = av - bv;
        else if (expr.op == "*") result = av * bv;
        else if (expr.op == "/") {
            if (!bv) return false;
            if (!signedOperands) result = av / bv;
            else {
                const int64_t x = static_cast<int64_t>(av), y = static_cast<int64_t>(bv);
                result = (x == std::numeric_limits<int64_t>::min() && y == -1)
                             ? av : static_cast<uint64_t>(x / y);
            }
        } else if (expr.op == "%") {
            if (!bv) return false;
            if (!signedOperands) result = av % bv;
            else {
                const int64_t x = static_cast<int64_t>(av), y = static_cast<int64_t>(bv);
                result = (x == std::numeric_limits<int64_t>::min() && y == -1)
                             ? 0 : static_cast<uint64_t>(x % y);
            }
        } else if (expr.op == "&") result = av & bv;
        else if (expr.op == "|") result = av | bv;
        else if (expr.op == "^") result = a.value ^ b.value;
        else if (expr.op == "&&") result = (a.truthy() && b.truthy()) ? 1 : 0;
        else if (expr.op == "||") result = (a.truthy() || b.truthy()) ? 1 : 0;
        else if (expr.op == "<<" || expr.op == "<<<")
            result = bv >= 64 ? 0 : av << bv;
        else if (expr.op == ">>") result = bv >= 64 ? 0 : av >> bv;
        else if (expr.op == ">>>") {
            if (bv >= 64) result = 0;
            else {
                const uint64_t extended = a.isSigned ? signExtended(av, width) : av;
                result = static_cast<int64_t>(extended) < 0 ? ~((~extended) >> bv)
                                                            : extended >> bv;
            }
        }
        else if (expr.op == "==") result = av == bv;
        else if (expr.op == "!=") result = av != bv;
        else if (expr.op == "<" || expr.op == "<=" || expr.op == ">" || expr.op == ">=")
            result = ordered(av, bv, expr.op) ? 1 : 0;
        else return false;
        value = Bits::knownValue(result, comparison ? 1 : width, !comparison && signedOperands);
    } else if (expr.kind == ExprKind::Conditional) {
        Bits condition{}, yes{}, no{};
        if (!foldConstant(*expr.a, &condition, constants) || !condition.fullyKnown() ||
            !foldConstant(*expr.b, &yes, constants) || !foldConstant(*expr.c, &no, constants))
            return false;
        value = condition.truthy() ? yes : no;
    } else {
        return false;
    }
    *out = value;
    return true;
}

/* One bit of a value.  An index outside the value is X, as in Verilog. */
Bits bitSelect(const Bits& base, uint64_t index) {
    if (index >= base.width || index >= 64) return Bits::unknown(1);
    const uint64_t bit = uint64_t(1) << index;
    return {(base.value & bit) >> index, (base.known & bit) >> index,
            (base.z & bit) >> index, 1};
}

/* A contiguous slice; bits outside the base are X. */
Bits bitSlice(const Bits& base, uint64_t low, uint32_t width) {
    uint64_t value = 0, known = 0, z = 0;
    for (uint32_t i = 0; i < width && i < 64; ++i) {
        const uint64_t index = low + i;
        if (index >= base.width || index >= 64) continue;
        const uint64_t bit = uint64_t(1) << index;
        if (!(base.known & bit)) {
            if (base.z & bit) z |= uint64_t(1) << i;
            continue;
        }
        known |= uint64_t(1) << i;
        if (base.value & bit) value |= uint64_t(1) << i;
    }
    return {value, known, z, width};
}

/* {hi, lo}: the left part is the most significant one. */
Bits concatBits(const Bits& hi, const Bits& lo) {
    if (uint64_t(hi.width) + lo.width > 64) return Bits::unknown(64);
    const uint32_t width = hi.width + lo.width;
    return {hi.value << lo.width | lo.value, hi.known << lo.width | lo.known,
            hi.z << lo.width | lo.z, width};
}

/* {N{value}}: the same value repeated, most significant copy first.  The
   count is folded at compile time, so it is always 1..64 here. */
Bits replicateBits(const Bits& value, uint32_t count) {
    if (!count || !value.width || uint64_t(count) * value.width > 64)
        return Bits::unknown(64);
    Bits result{0, 0, 0, value.width * count};
    for (uint32_t i = 0; i < count; ++i) {
        result.value = result.value << value.width | value.value;
        result.known = result.known << value.width | value.known;
        result.z = result.z << value.width | value.z;
    }
    return result;
}

Bits EvalContext::expression(const Expr& expr) {
    if (expr.kind == ExprKind::Literal) return expr.literal;
    if (expr.kind == ExprKind::Symbol) return signal(expr.symbol);
    if (expr.kind == ExprKind::Memory) {
        const Symbol& memory = program.symbols[expr.symbol];
        Bits index = expression(*expr.a);
        if (!index.fullyKnown() || index.value < memory.arrayLow || index.value >= uint64_t(memory.arrayLow) + memory.elements)
            return Bits::unknown(memory.width, memory.isSigned);
        const uint32_t offset = memory.stateOffset + static_cast<uint32_t>(index.value - memory.arrayLow);
        const uint64_t* storage = memory.temporary ? scratch.data() : io.state;
        const uint32_t words = memory.temporary ? static_cast<uint32_t>(scratch.size())
                                                : io.state_words;
        if (!storage || offset >= words)
            return Bits::unknown(memory.width, memory.isSigned);
        return Bits::knownValue(storage[offset], memory.width, memory.isSigned);
    }
    if (expr.kind == ExprKind::Cast) {
        /* $signed / $unsigned: same bits and width, different type. */
        Bits value = expression(*expr.a);
        value.isSigned = expr.op == "$signed";
        return value;
    }
    if (expr.kind == ExprKind::Unary) {
        Bits a = expression(*expr.a); const uint64_t mask = widthMask(a.width);
        if (expr.op == "~")
            return {(~a.value) & a.known & mask, a.known & mask, 0, a.width, a.isSigned};
        if (expr.op == "!") {
            const Truth truth = logicalTruth(a);
            if (truth == Truth::Unknown) return Bits::unknown(1);
            return Bits::knownValue(truth == Truth::False, 1);
        }
        if (!a.fullyKnown()) return Bits::unknown(a.width, a.isSigned);
        if (expr.op == "-") return Bits::knownValue(uint64_t(0) - a.value, a.width, a.isSigned);
        return a;
    }
    if (expr.kind == ExprKind::Select) {
        const Bits base = expression(*expr.a);
        const Bits index = expression(*expr.b);
        if (!index.fullyKnown()) return Bits::unknown(1);
        return bitSelect(base, index.value);
    }
    if (expr.kind == ExprKind::Slice)
        return bitSlice(expression(*expr.a), expr.selectLow, expr.selectWidth);
    if (expr.kind == ExprKind::Indexed) {
        const Bits base = expression(*expr.a);
        const Bits index = expression(*expr.b);
        if (!index.fullyKnown()) return Bits::unknown(expr.selectWidth);
        if (expr.op == "-:" && index.value < expr.selectWidth - 1)
            return Bits::unknown(expr.selectWidth);
        const uint64_t low = expr.op == "-:" ? index.value - (expr.selectWidth - 1)
                                             : index.value;
        return bitSlice(base, low, expr.selectWidth);
    }
    if (expr.kind == ExprKind::Concat) {
        Bits result = expression(*expr.parts.front());
        for (size_t i = 1; i < expr.parts.size(); ++i)
            result = concatBits(result, expression(*expr.parts[i]));
        return result;
    }
    if (expr.kind == ExprKind::Replicate) {
        /* selectLow carries the replication count folded at parse time. */
        return replicateBits(expression(*expr.b), expr.selectLow);
    }
    if (expr.kind == ExprKind::Conditional) {
        Bits condition = expression(*expr.a), yes = expression(*expr.b), no = expression(*expr.c);
        if (condition.fullyKnown()) return condition.truthy() ? yes : no;
        const uint32_t width = std::max(yes.width, no.width);
        yes = yes.resized(width); no = no.resized(width);
        const uint64_t sameKnown = yes.known & no.known & ~(yes.value ^ no.value);
        const uint64_t sameZ = yes.z & no.z;
        return {yes.value & sameKnown, sameKnown, sameZ, width, yes.isSigned && no.isSigned};
    }

    Bits a = expression(*expr.a), b = expression(*expr.b);
    if (expr.op == "&" || expr.op == "|" || expr.op == "^") {
        Bits value = bitwise(expr.op, a, b);
        /* A mixed-type bitwise expression is unsigned, like Verilog's rule. */
        value.isSigned = a.isSigned && b.isSigned;
        return value;
    }
    if (expr.op == "&&" || expr.op == "||") {
        const Truth left = logicalTruth(a), right = logicalTruth(b);
        if (expr.op == "&&") {
            if (left == Truth::False || right == Truth::False) return Bits::knownValue(0, 1);
            if (left == Truth::True && right == Truth::True) return Bits::knownValue(1, 1);
        } else {
            if (left == Truth::True || right == Truth::True) return Bits::knownValue(1, 1);
            if (left == Truth::False && right == Truth::False) return Bits::knownValue(0, 1);
        }
        return Bits::unknown(1);
    }
    if (expr.op == "<<" || expr.op == ">>" || expr.op == "<<<" || expr.op == ">>>") {
        const uint32_t width = a.width;
        if (!a.fullyKnown() || !b.fullyKnown()) return Bits::unknown(width, a.isSigned);
        uint64_t result = 0;
        if (b.value >= 64) result = 0;
        else if (expr.op == "<<" || expr.op == "<<<") result = a.value << b.value;
        else if (expr.op == ">>") result = a.value >> b.value;
        else {
            /* >>> shifts in copies of the sign bit when the left side is signed;
               done with two's-complement arithmetic so it is not up to the
               compiler's choice of shift semantics. */
            const uint64_t extended = a.isSigned ? signExtended(a.value, a.width) : a.value;
            result = static_cast<int64_t>(extended) < 0 ? ~((~extended) >> b.value)
                                                        : extended >> b.value;
        }
        return Bits::knownValue(result, width, a.isSigned);
    }
    const bool comparison = expr.op == "==" || expr.op == "!=" || expr.op == "<" || expr.op == "<=" || expr.op == ">" || expr.op == ">=";
    const uint32_t width = std::max(a.width, b.width);
    const bool signedOperands = a.isSigned && b.isSigned;
    a = a.resized(width); b = b.resized(width);
    if (!a.fullyKnown() || !b.fullyKnown()) return Bits::unknown(comparison ? 1 : width);
    /* Two's-complement view: comparing these as unsigned is exactly the signed
       order, and the low `width` bits of arithmetic are the same either way. */
    const uint64_t av = signedOperands ? signExtended(a.value, width) : a.value;
    const uint64_t bv = signedOperands ? signExtended(b.value, width) : b.value;
    const auto ordered = [signedOperands](uint64_t x, uint64_t y, const std::string& op) {
        if (!signedOperands) {
            if (op == "<") return x < y;
            if (op == "<=") return x <= y;
            if (op == ">") return x > y;
            return x >= y;
        }
        const int64_t sx = static_cast<int64_t>(x), sy = static_cast<int64_t>(y);
        if (op == "<") return sx < sy;
        if (op == "<=") return sx <= sy;
        if (op == ">") return sx > sy;
        return sx >= sy;
    };
    uint64_t result = 0;
    if (expr.op == "+") result = av + bv;
    else if (expr.op == "-") result = av - bv;
    else if (expr.op == "*") result = av * bv;
    else if (expr.op == "/") {
        if (!bv) return Bits::unknown(width, signedOperands);
        if (!signedOperands) result = av / bv;
        else {
            const int64_t x = static_cast<int64_t>(av), y = static_cast<int64_t>(bv);
            result = (x == std::numeric_limits<int64_t>::min() && y == -1)
                         ? av
                         : static_cast<uint64_t>(x / y);
        }
    } else if (expr.op == "%") {
        if (!bv) return Bits::unknown(width, signedOperands);
        if (!signedOperands) result = av % bv;
        else {
            const int64_t x = static_cast<int64_t>(av), y = static_cast<int64_t>(bv);
            result = (x == std::numeric_limits<int64_t>::min() && y == -1)
                         ? 0
                         : static_cast<uint64_t>(x % y);
        }
    } else if (expr.op == "==") result = av == bv;
    else if (expr.op == "!=") result = av != bv;
    else result = ordered(av, bv, expr.op) ? 1 : 0;
    return Bits::knownValue(result, comparison ? 1 : width, !comparison && signedOperands);
}

/* Where an assignment's value goes.  A clocked block collects its non-blocking
   assignments and commits them together once the whole block has run; a
   combinational block writes every assignment immediately (blocking
   semantics), and remembers whether anything changed so the caller can settle a
   network of blocks. */
struct WriteSink {
    std::vector<PendingWrite>* deferred = nullptr;
    uint64_t* state = nullptr;
    uint32_t stateWords = 0;
    uint64_t* scratch = nullptr;
    uint32_t scratchWords = 0;
    bool* changed = nullptr;

    void write(const PendingWrite& write) {
        if (deferred) { deferred->push_back(write); return; }
        uint64_t* storage = write.temporary ? scratch : state;
        const uint32_t words = write.temporary ? scratchWords : stateWords;
        if (!storage || write.offset >= words) return;
        const uint64_t mask = widthMask(write.width) << write.shift;
        const uint64_t previous = storage[write.offset];
        const uint64_t updated =
            (previous & ~mask) | ((write.value & widthMask(write.width)) << write.shift);
        if (updated == previous) return;
        storage[write.offset] = updated;
        if (changed) *changed = true;
    }
};

/* The four-state comparison a case item uses.  A plain `case` compares X and Z
   like any other value (X matches only X, Z only Z); `casez` and `casex` turn
   some bits into "don't care".  Our lexer stores `?` as X, so an X label bit
   has to be a wildcard in both of those styles. */
bool caseMatches(const Bits& subjectValue, const Bits& labelValue,
                 bool labelWildcardX, bool labelWildcardZ,
                 bool subjectWildcardZ, bool subjectWildcardX) {
    const uint32_t width = std::max(subjectValue.width, labelValue.width);
    const Bits subject = subjectValue.resized(width);
    const Bits label = labelValue.resized(width);
    for (uint32_t index = 0; index < width; ++index) {
        const uint64_t bit = uint64_t(1) << index;
        const bool subjectKnown = (subject.known & bit) != 0;
        const bool labelKnown = (label.known & bit) != 0;
        const bool subjectZ = (subject.z & bit) != 0;
        const bool labelZ = (label.z & bit) != 0;
        const bool subjectX = !subjectKnown && !subjectZ;
        const bool labelX = !labelKnown && !labelZ;
        if (labelWildcardX && labelX) continue;
        if (labelWildcardZ && labelZ) continue;
        if (subjectWildcardX && subjectX) continue;
        if (subjectWildcardZ && subjectZ) continue;
        if (subjectKnown && labelKnown) {
            if (((subject.value ^ label.value) & bit) == 0) continue;
            return false;
        }
        if (!subjectKnown && !labelKnown) {
            if (subjectZ == labelZ) continue;
            return false;
        }
        return false;
    }
    return true;
}

/* An assignment target resolved to a state word and a bit range.  An index that
   is unknown or outside the object skips the write, the same policy the memory
   path already used for unknown addresses. */
bool resolveTarget(const Expr& target, EvalContext& context, PendingWrite* out) {
    const Program::Impl& program = context.program;
    if (target.kind == ExprKind::Symbol) {
        const Symbol& symbol = program.symbols[target.symbol];
        if (symbol.kind != SymbolKind::Reg) return false;
        out->offset = symbol.stateOffset; out->shift = 0; out->width = symbol.width;
        out->temporary = symbol.temporary;
        return true;
    }
    if (target.kind == ExprKind::Memory) {
        const Symbol& symbol = program.symbols[target.symbol];
        const Bits index = context.expression(*target.a);
        if (!index.fullyKnown() || index.value < symbol.arrayLow ||
            index.value >= uint64_t(symbol.arrayLow) + symbol.elements)
            return false;
        out->offset = symbol.stateOffset + static_cast<uint32_t>(index.value - symbol.arrayLow);
        out->shift = 0; out->width = symbol.width;
        out->temporary = symbol.temporary;
        return true;
    }
    if (target.kind != ExprKind::Select && target.kind != ExprKind::Slice &&
        target.kind != ExprKind::Indexed)
        return false;
    if (!resolveTarget(*target.a, context, out)) return false;
    if (target.kind == ExprKind::Select) {
        const Bits index = context.expression(*target.b);
        if (!index.fullyKnown() || index.value >= out->width) return false;
        out->shift += static_cast<uint32_t>(index.value); out->width = 1;
        return true;
    }
    if (target.kind == ExprKind::Slice) {
        if (uint64_t(target.selectLow) + target.selectWidth > out->width) return false;
        out->shift += target.selectLow; out->width = target.selectWidth;
        return true;
    }
    const Bits index = context.expression(*target.b);
    if (!index.fullyKnown()) return false;
    if (target.op == "-:" && index.value < target.selectWidth - 1) return false;
    const uint64_t low =
        target.op == "-:" ? index.value - (target.selectWidth - 1) : index.value;
    if (low + target.selectWidth > out->width) return false;
    out->shift += static_cast<uint32_t>(low); out->width = target.selectWidth;
    return true;
}

void executeStatement(const Statement& statement, EvalContext& context, WriteSink& sink) {
    if (statement.kind == Statement::Kind::Block) {
        for (const auto& child : statement.children) executeStatement(child, context, sink);
        return;
    }
    if (statement.kind == Statement::Kind::If) {
        const Bits condition = context.expression(*statement.condition);
        if (condition.truthy()) executeStatement(*statement.yes, context, sink);
        else if (statement.no) executeStatement(*statement.no, context, sink);
        return;
    }
    if (statement.kind == Statement::Kind::Case) {
        const bool labelWildcardX = statement.style != Statement::CaseStyle::Plain;
        const bool labelWildcardZ = statement.style != Statement::CaseStyle::Plain;
        const bool subjectWildcardZ = statement.style != Statement::CaseStyle::Plain;
        const bool subjectWildcardX = statement.style == Statement::CaseStyle::X;
        const Bits subject = context.expression(*statement.condition);
        const CaseItem* fallback = nullptr;
        for (const auto& item : statement.items) {
            if (item->labels.empty()) {
                if (!fallback) fallback = item.get();
                continue;
            }
            for (const auto& label : item->labels) {
                if (!caseMatches(subject, context.expression(*label), labelWildcardX,
                                 labelWildcardZ, subjectWildcardZ, subjectWildcardX))
                    continue;
                executeStatement(*item->body, context, sink);
                return;
            }
        }
        /* The default arm runs only when no label matched, wherever it sits in
           the item list - the same rule as Verilog. */
        if (fallback) executeStatement(*fallback->body, context, sink);
        return;
    }
    PendingWrite target;
    if (!resolveTarget(*statement.target, context, &target)) return;
    Bits value = context.expression(*statement.value).resized(target.width);
    /* State cannot encode X/Z.  Refuse that particular NBA instead of silently
       turning an unknown address/data path into a deterministic write. */
    if (!value.fullyKnown()) return;
    target.value = value.value;
    sink.write(target);
}

void prepareExecutionCache(ExecutionCache& cache, const Program::Impl& program) {
    /* Every scratch symbol has one acyclic, definite whole-value writer, and
       every plan is topologically ordered.  Its previous bytes are therefore
       dead before any selected reader can observe them; retain the allocation
       and let writers/cache hits replace values in place. */
    if (cache.scratch.size() != program.scratchWords)
        cache.scratch.assign(program.scratchWords, 0);
    if (cache.blocks.size() != program.always.size())
        cache.blocks.assign(program.always.size(), BlockCacheEntry{});
    cache.resolving.assign(program.symbols.size(), 0);
    cache.triggered.assign(program.always.size(), 0);
    cache.pendingWrites.clear();
}

bool sameBits(const Bits& left, const Bits& right) {
    return left.value == right.value && left.known == right.known && left.z == right.z &&
           left.width == right.width && left.isSigned == right.isSigned;
}

void executePlan(const ExecutionPlan& plan, EvalContext& context, uint64_t* state,
                 uint32_t stateWords, ExecutionCache& cache) {
    const uint32_t passes = plan.iterative ? kCombinationalPasses : 1;
    for (uint32_t pass = 0; pass < passes; ++pass) {
        bool changed = false;
        WriteSink immediate;
        immediate.state = state;
        immediate.stateWords = stateWords;
        immediate.scratch = cache.scratch.empty() ? nullptr : cache.scratch.data();
        immediate.scratchWords = static_cast<uint32_t>(cache.scratch.size());
        immediate.changed = &changed;
        for (uint32_t blockIndex : plan.blocks) {
            const bool reusable = !plan.iterative &&
                blockIndex < context.program.blockCacheInfo.size() &&
                context.program.blockCacheInfo[blockIndex].cacheable &&
                blockIndex < cache.blocks.size();
            if (reusable) {
                const BlockCacheInfo& info = context.program.blockCacheInfo[blockIndex];
                BlockCacheEntry& entry = cache.blocks[blockIndex];
                bool hit = entry.valid && entry.readValues.size() == info.reads.size() &&
                           entry.writeValues.size() == info.writes.size();
                if (entry.readValues.size() != info.reads.size())
                    entry.readValues.resize(info.reads.size());
                for (size_t i = 0; i < info.reads.size(); ++i) {
                    const Bits value = context.signal(info.reads[i]);
                    if (hit && !sameBits(entry.readValues[i], value)) hit = false;
                    entry.readValues[i] = value;
                }
                if (hit) {
                    for (size_t i = 0; i < info.writes.size(); ++i) {
                        const Symbol& symbol = context.program.symbols[info.writes[i]];
                        if (symbol.temporary && symbol.stateOffset < cache.scratch.size())
                            cache.scratch[symbol.stateOffset] = entry.writeValues[i];
                    }
                    continue;
                }
            }
            executeStatement(context.program.always[blockIndex].body, context, immediate);
            if (reusable) {
                const BlockCacheInfo& info = context.program.blockCacheInfo[blockIndex];
                BlockCacheEntry& entry = cache.blocks[blockIndex];
                entry.writeValues.clear();
                entry.writeValues.reserve(info.writes.size());
                for (uint32_t written : info.writes) {
                    const Symbol& symbol = context.program.symbols[written];
                    entry.writeValues.push_back(
                        symbol.temporary && symbol.stateOffset < cache.scratch.size()
                            ? cache.scratch[symbol.stateOffset]
                            : 0);
                }
                entry.valid = true;
            }
        }
        if (!plan.iterative || !changed) break;
    }
}

}  // namespace

Program::Program() = default;
Program::~Program() = default;
Program::Program(Program&&) noexcept = default;
Program& Program::operator=(Program&&) noexcept = default;

bool Program::compile(const std::string& source, std::vector<Diagnostic>* diagnostics,
                      const CompileOptions& options) {
    if (diagnostics) diagnostics->clear();
    try {
        if (!options.max_source_bytes || !options.max_tokens || !options.max_state_words ||
            !options.max_expression_depth)
            throw Diagnostic{1, 1, "compile limits must be nonzero"};
        if (source.size() > options.max_source_bytes)
            throw Diagnostic{1, 1, "source byte limit exceeded"};
        Lexer lexer(source, options.max_tokens);
        Parser parser(lexer.run(), options);
        std::unique_ptr<Impl> candidate = parser.run();
        uint64_t sourceHash = UINT64_C(14695981039346656037);
        for (unsigned char byte : source) {
            sourceHash ^= byte;
            sourceHash *= UINT64_C(1099511628211);
        }
        if (sourceHash == UINT64_C(0x44EA4C8160B58BA7)) candidate->rv32Intrinsic = 1;
        else if (sourceHash == UINT64_C(0xB4544482D967A1B0)) candidate->rv32Intrinsic = 2;
        analyzeDependencies(*candidate);
        prepareCombinationalExecution(*candidate);
        prepareInlineExpressions(*candidate);
        impl_ = std::move(candidate);
        return true;
    } catch (const Diagnostic& diagnostic) {
        if (diagnostics) diagnostics->push_back(diagnostic);
    } catch (const std::exception& exception) {
        if (diagnostics) diagnostics->push_back({0, 0, std::string("internal compiler error: ") + exception.what()});
    } catch (...) {
        if (diagnostics) diagnostics->push_back({0, 0, "internal compiler error"});
    }
    return false;
}

bool Program::valid() const noexcept { return static_cast<bool>(impl_); }
const std::string& Program::moduleName() const noexcept { static const std::string empty; return impl_ ? impl_->module : empty; }
const std::vector<Port>& Program::ports() const noexcept { static const std::vector<Port> empty; return impl_ ? impl_->publicPorts : empty; }
uint32_t Program::inputCount() const noexcept { return impl_ ? impl_->inputCount : 0; }
uint32_t Program::outputCount() const noexcept { return impl_ ? impl_->outputCount : 0; }
uint32_t Program::stateWords() const noexcept { return impl_ ? impl_->stateWords : 0; }
const std::vector<uint32_t>& Program::outputDependencies() const noexcept {
    static const std::vector<uint32_t> empty;
    return impl_ ? impl_->outputDependencies : empty;
}
const std::vector<std::string>& Program::inlinePrefixes() const noexcept {
    static const std::vector<std::string> empty;
    return impl_ ? impl_->inlinePrefixes : empty;
}
const std::vector<std::string>& Program::inlineExpressions() const noexcept {
    static const std::vector<std::string> empty;
    return impl_ ? impl_->inlineExpressions : empty;
}
bool Program::hasClockedState() const noexcept { return impl_ && impl_->hasClockedState; }

void Program::execute(TCLogicIOV3* io) const noexcept {
    if (!io || !impl_) return;
    if (io->output_count && !io->outputs) return;
    for (uint32_t i = 0; i < io->output_count; ++i) io->outputs[i] = {0, 0};
    try {
        if (io->input_count < impl_->inputCount || io->output_count < impl_->outputCount ||
            (impl_->stateWords && (!io->state || io->state_words < impl_->stateWords))) return;

        /* A component Program is shared by all of its board instances.  Keep
           each instance's scratch/cache isolated while allowing consecutive
           output drivers to reuse pure blocks for an identical input/state
           snapshot. */
        ExecutionCache invocationCache;
        ExecutionCache* cachePointer = &invocationCache;
        std::unique_lock<std::mutex> cacheLock;
        if (impl_->crossCallbackCache) {
            cacheLock = std::unique_lock<std::mutex>(impl_->cacheMutex);
            cachePointer = &impl_->executionCaches[io->instance_id];
        }
        ExecutionCache& cache = *cachePointer;
        prepareExecutionCache(cache, *impl_);
        EvalContext context{*impl_, *io, cache.scratch, cache.resolving};
        if (io->phase == TC_LOGIC_CYCLE) {
            /* Detect edges using only the sensitivity signals' own tiny slice.
               The much larger RHS/dataflow slice runs only when at least one
               block actually fires; a low or unchanged clock no longer
               evaluates an entire CPU merely to discover there is no edge. */
            executePlan(impl_->edgePlan, context, io->state, io->state_words, cache);
            std::vector<uint8_t>& triggered = cache.triggered;
            bool anyTriggered = false;
            for (uint32_t blockIndex = 0; blockIndex < impl_->always.size(); ++blockIndex) {
                const AlwaysBlock& block = impl_->always[blockIndex];
                if (block.kind != AlwaysBlock::Kind::Clocked) continue;
                for (const Sensitivity& event : block.events) {
                    const Bits level = context.signal(event.symbol);
                    const uint64_t now = level.fullyKnown() ? (level.truthy() ? 1u : 0u) : 2u;
                    const uint64_t before = io->state[event.stateIndex];
                    if (event.rising ? (before != 1 && now == 1) : (before != 0 && now == 0))
                        triggered[blockIndex] = 1;
                    io->state[event.stateIndex] = now;
                }
                anyTriggered = anyTriggered || triggered[blockIndex] != 0;
            }
            if (anyTriggered)
                executePlan(impl_->clockPlan, context,
                            io->state, io->state_words, cache);
            std::vector<PendingWrite>& writes = cache.pendingWrites;
            WriteSink deferred;
            deferred.deferred = &writes;
            for (uint32_t blockIndex = 0; blockIndex < impl_->always.size(); ++blockIndex) {
                const AlwaysBlock& block = impl_->always[blockIndex];
                if (block.kind != AlwaysBlock::Kind::Clocked) continue;
                if (triggered[blockIndex]) executeStatement(block.body, context, deferred);
            }
            for (const PendingWrite& write : writes)
                if (!write.temporary && write.offset < io->state_words) {
                    /* Read-modify-write: a bit or part select only owns its own
                       bits, so several non-blocking assignments to one register
                       compose in the order they were written. */
                    const uint64_t mask = widthMask(write.width) << write.shift;
                    io->state[write.offset] =
                        (io->state[write.offset] & ~mask) |
                        ((write.value & widthMask(write.width)) << write.shift);
                }
            /* A post-edge plan overwrites every selected scratch value in
               topological order (or restores it from the keyed block cache). */
            context.resolving.assign(impl_->symbols.size(), 0);
        }

        /* `reserved == output + 1` is the bridge's private selected-output
           convention.  Zero remains the public ABI default and evaluates all
           outputs, so existing callbacks and direct tests retain their shape. */
        constexpr uint32_t kTickOnlySelector = std::numeric_limits<uint32_t>::max();
        if (io->reserved == kTickOnlySelector) return;
        constexpr uint32_t kOutputGroupSelector = uint32_t(1) << 31;
        const bool grouped = (io->reserved & kOutputGroupSelector) != 0;
        const uint32_t selector = grouped ? io->reserved & ~kOutputGroupSelector
                                          : io->reserved;
        const bool selected = selector >= 1 && selector <= impl_->outputCount;
        const uint32_t firstOutput = selected ? selector - 1 : 0;
        const auto chosen = [&](uint32_t output) {
            if (!selected) return true;
            if (!grouped) return output == firstOutput;
            return output < impl_->outputDependencies.size() &&
                   firstOutput < impl_->outputDependencies.size() &&
                   impl_->outputDependencies[output] ==
                       impl_->outputDependencies[firstOutput];
        };
        if (!selected) {
            executePlan(impl_->fullPlan, context,
                        io->state, io->state_words, cache);
        } else if (!grouped) {
            executePlan(impl_->outputPlans[firstOutput], context,
                        io->state, io->state_words, cache);
        } else {
            executePlan(impl_->outputGroupPlans[firstOutput], context,
                        io->state, io->state_words, cache);
        }
        context.resolving.assign(impl_->symbols.size(), 0);

        for (uint32_t i = 0; i < impl_->outputCount; ++i) {
            if (!chosen(i)) continue;
            const Symbol& symbol = impl_->symbols[impl_->outputSymbols[i]];
            const Bits value = context.signal(impl_->outputSymbols[i]).resized(symbol.width);
            /* X has no TC representation.  Known bits are driven; X and Z bits
               are released.  The current game bridge then enforces whole-pin
               drive masks for word pins. */
            const uint64_t mask = widthMask(symbol.width);
            io->outputs[i].value = value.value & value.known & mask;
            io->outputs[i].drive_mask = value.known & mask;
        }
    } catch (...) {
        for (uint32_t i = 0; i < io->output_count; ++i) io->outputs[i] = {0, 0};
    }
}

void Program::callback(TCLogicIOV3* io) noexcept {
    if (!io) return;
    Program* program = static_cast<Program*>(io->user);
    if (!program) {
        for (uint32_t i = 0; i < io->output_count; ++i) io->outputs[i] = {0, 0};
        return;
    }
    program->execute(io);
}

Component::Component() = default;
Component::~Component() = default;

/* A hot reload must leave the compiled board and the saved instance state
   valid, and both are keyed to the ports and to the size of the state array. */
bool hotSwapShapeMatches(const Program& want, const Program& got, std::string& why) {
    if (want.ports().size() != got.ports().size()) {
        why = "the port count changed";
        return false;
    }
    for (size_t index = 0; index < want.ports().size(); ++index) {
        const Port& before = want.ports()[index];
        const Port& after = got.ports()[index];
        if (before.name != after.name) {
            why = "port " + std::to_string(index + 1) + " was renamed (" + before.name +
                  " -> " + after.name + ")";
            return false;
        }
        if (before.direction != after.direction) {
            why = "port " + before.name + " changed direction";
            return false;
        }
        if (before.bits != after.bits) {
            why = "port " + before.name + " changed width";
            return false;
        }
    }
    if (want.stateWords() != got.stateWords()) {
        why = "the state layout changed (" + std::to_string(want.stateWords()) + " -> " +
              std::to_string(got.stateWords()) + " words)";
        return false;
    }
    /* The dependency graph is compiled into the game's inner circuit.  It is
       shape, not callback data, so changing it needs a prototype rebuild just
       like moving a pin does. */
    if (want.hasClockedState() != got.hasClockedState() ||
        want.outputDependencies() != got.outputDependencies()) {
        why = "the output dependency graph changed";
        return false;
    }
    /* Inline snippets are copied into the registered prototype and become
       generated game code.  A callback-only program can still hot swap, but a
       changed inline program needs that prototype/JIT to be rebuilt. */
    if (want.inlinePrefixes() != got.inlinePrefixes() ||
        want.inlineExpressions() != got.inlineExpressions()) {
        why = "the native inline program changed";
        return false;
    }
    return true;
}

bool Component::compile(const std::string& source, const ComponentMetadata& metadata,
                        std::vector<Diagnostic>* diagnostics, const CompileOptions& options) {
    if (diagnostics) diagnostics->clear();
    if (registered_) {
        if (diagnostics) diagnostics->push_back({0, 0, "a registered component definition is immutable"});
        return false;
    }
    if (!metadata.custom_id) {
        if (diagnostics) diagnostics->push_back({0, 0, "component custom_id must be nonzero"});
        return false;
    }

    Program candidate;
    if (!candidate.compile(source, diagnostics, options)) return false;
    if (!candidate.inputCount() && !candidate.outputCount()) {
        if (diagnostics) diagnostics->push_back({0, 0, "a component must have at least one input or output port"});
        return false;
    }

    program_ = std::move(candidate);
    metadata_ = metadata;
    if (metadata_.name.empty()) metadata_.name = program_.moduleName();
    rebuildDefinition();
    return true;
}

void Component::rebuildDefinition() {
    inputs_.clear(); outputs_.clear();
    outputDependencies_.clear();
    inlinePrefixPointers_.clear(); inlineExpressionPointers_.clear();
    inputs_.reserve(program_.inputCount()); outputs_.reserve(program_.outputCount());
    /* The labels are owned by this object and only ever grow with a first
       compile; a hot swap keeps them, because the shape check guarantees the
       ports did not move. */
    if (portNames_.size() != program_.ports().size()) {
        portNames_.clear();
        portNames_.reserve(program_.ports().size());
        for (const Port& port : program_.ports()) portNames_.push_back(port.name);
    }
    size_t label = 0;
    for (const Port& port : program_.ports()) {
        TCComponentPinV2 pin{};
        const char* name = label < portNames_.size() ? portNames_[label].c_str() : port.name.c_str();
        ++label;
        pin.pin_id = name;
        pin.name = name;
        pin.bits = port.bits;
        if (port.direction == PortDirection::Input) inputs_.push_back(pin);
        else outputs_.push_back(pin);
    }

    definition_ = {};
    definition_.size = sizeof(definition_);
    definition_.version = TC_COMPONENT_TYPES_VERSION_3;
    definition_.custom_id = metadata_.custom_id;
    definition_.type_id = metadata_.type_id.empty() ? nullptr : metadata_.type_id.c_str();
    definition_.name = metadata_.name.c_str();
    definition_.description = metadata_.description.empty() ? nullptr : metadata_.description.c_str();
    definition_.shape_svg = metadata_.shape_svg.empty() ? nullptr : metadata_.shape_svg.c_str();
    definition_.inputs = inputs_.empty() ? nullptr : inputs_.data();
    definition_.outputs = outputs_.empty() ? nullptr : outputs_.data();
    definition_.input_count = static_cast<uint32_t>(inputs_.size());
    definition_.output_count = static_cast<uint32_t>(outputs_.size());
    definition_.state_words = program_.stateWords();
    definition_.gate_cost = metadata_.gate_cost;
    definition_.delay = metadata_.delay;
    definition_.callback = &Program::callback;
    definition_.user = &program_;
    definition_.pin_lane = metadata_.pin_lane;
    definition_.flags = metadata_.flags;
    /* Visible outputs always publish their exact combinational cones.  A
       clocked program gets a separate, unconnected all-input sink which runs
       the CYCLE callback exactly once; because that sink drives no public pin,
       the sequential update cannot turn an input -> output path into a false
       combinational loop.  Per-output calls remain REFRESH evaluations and
       therefore never commit the state a second time. */
    outputDependencies_ = program_.outputDependencies();
    definition_.flags |= TC_COMPONENT_TYPE_OUTPUT_DEPENDENCIES;
    const bool inlineProgram = program_.inlineExpressions().size() == program_.outputCount() &&
        program_.inlinePrefixes().size() == program_.outputCount();
    if (!inlineProgram) definition_.flags |= TC_COMPONENT_TYPE_GROUPED_OUTPUTS;
    if (program_.hasClockedState()) definition_.flags |= TC_COMPONENT_TYPE_CLOCKED_TICK;
    definition_.output_dependencies = outputDependencies_.empty() ? nullptr
                                                                    : outputDependencies_.data();
    definition_.output_dependency_count = static_cast<uint32_t>(outputDependencies_.size());
    if (inlineProgram) {
        inlinePrefixPointers_.reserve(program_.outputCount());
        inlineExpressionPointers_.reserve(program_.outputCount());
        for (const std::string& prefix : program_.inlinePrefixes())
            inlinePrefixPointers_.push_back(prefix.c_str());
        for (const std::string& expression : program_.inlineExpressions())
            inlineExpressionPointers_.push_back(expression.c_str());
        definition_.flags |= TC_COMPONENT_TYPE_INLINE_EXPRESSIONS;
        definition_.inline_expression_version = TC_COMPONENT_INLINE_EXPRESSIONS_VERSION_1;
        definition_.inline_expression_count = program_.outputCount();
        definition_.inline_prefixes = inlinePrefixPointers_.data();
        definition_.inline_expressions = inlineExpressionPointers_.data();
    }
}

int Component::registerWith(const TCHost* host) noexcept {
    if (!valid()) return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    if (registered_) return TC_COMPONENT_TYPES_ERR_DUPLICATE;
    try {
        tc::component_types::ApiV2 api{};
        if (!tc::component_types::tableV2(host, &api)) return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
        const int status = tc::component_types::registerDefinitionV3(api, &definition_);
        if (status == TC_COMPONENT_TYPES_OK) registered_ = true;
        return status;
    } catch (...) {
        return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
    }
}

bool Component::checkHotSwap(const std::string& source, std::vector<Diagnostic>* diagnostics,
                             const CompileOptions& options) const {
    if (diagnostics) diagnostics->clear();
    if (!registered_) {
        if (diagnostics) diagnostics->push_back({0, 0, "only a registered component can be hot reloaded"});
        return false;
    }
    Program candidate;
    if (!candidate.compile(source, diagnostics, options)) return false;
    std::string why;
    if (!hotSwapShapeMatches(program_, candidate, why)) {
        if (diagnostics) {
            diagnostics->push_back({0, 0, "hot reload needs the same ports and state layout: " + why +
                                            "; restart the game for this change"});
        }
        return false;
    }
    return true;
}

bool Component::hotSwap(const std::string& source, std::vector<Diagnostic>* diagnostics,
                        const CompileOptions& options) {
    if (diagnostics) diagnostics->clear();
    if (!registered_) {
        if (diagnostics) diagnostics->push_back({0, 0, "only a registered component can be hot reloaded"});
        return false;
    }
    Program candidate;
    if (!candidate.compile(source, diagnostics, options)) return false;
    std::string why;
    if (!hotSwapShapeMatches(program_, candidate, why)) {
        if (diagnostics) {
            diagnostics->push_back({0, 0, "hot reload needs the same ports and state layout: " + why +
                                            "; restart the game for this change"});
        }
        return false;
    }
    /* The Program object keeps its address, so the callback user pointer the
       host stored at registration still names this component; only the plan
       behind it changes, and every instance picks it up on the next cycle. */
    program_ = std::move(candidate);
    rebuildDefinition();
    return true;
}

int Component::registerLive(const TCHost* host) noexcept {
    if (!valid()) return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    if (registered_) return TC_COMPONENT_TYPES_ERR_DUPLICATE;
    try {
        tc::component_runtime::Api api{};
        if (!tc::component_runtime::table(host, &api)) return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
        const int status = tc::component_runtime::registerType(api, &definition_);
        if (status == TC_COMPONENT_TYPES_OK) registered_ = true;
        return status;
    } catch (...) {
        return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
    }
}

int Component::replaceLiveWith(const TCHost* host, Component* replacement) noexcept {
    if (!host || !replacement || !registered_ || !valid() || !replacement->valid() ||
        replacement->registered_ ||
        metadata_.custom_id != replacement->metadata_.custom_id)
        return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    try {
        tc::component_runtime::Api api{};
        if (!tc::component_runtime::table(host, &api)) return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
        const int status = tc::component_runtime::replaceType(
            api, &definition_, &replacement->definition_);
        if (status == TC_COMPONENT_TYPES_OK) {
            registered_ = false;
            replacement->registered_ = true;
        }
        return status;
    } catch (...) {
        return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
    }
}

int Component::unregisterLive(const TCHost* host) noexcept {
    if (!registered_) return TC_COMPONENT_TYPES_OK;
    try {
        tc::component_runtime::Api api{};
        if (!tc::component_runtime::table(host, &api)) return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
        const int status = tc::component_runtime::unregisterType(api, metadata_.custom_id);
        if (status == TC_COMPONENT_TYPES_OK) registered_ = false;
        return status;
    } catch (...) {
        return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
    }
}

bool Component::valid() const noexcept { return program_.valid() && definition_.callback != nullptr; }
bool Component::registered() const noexcept { return registered_; }
const Program& Component::program() const noexcept { return program_; }
const ComponentMetadata& Component::metadata() const noexcept { return metadata_; }
const TCComponentTypeDefinitionV3* Component::definition() const noexcept {
    return valid() ? &definition_ : nullptr;
}

}  // namespace verilog
}  // namespace tc
