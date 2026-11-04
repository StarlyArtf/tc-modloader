/* Hardware arithmetic must match the independent SoftFloat implementation,
   including flags, while leaving the caller's MXCSR unchanged. */
#include <cstdio>

#if !__has_include("../examples/float-ops/fp/hardware.hpp")
int main() {
    std::puts("FAIL: CPU floating-point backend is not implemented");
    return 1;
}
#else
#include "../examples/float-ops/fp/hardware.hpp"
#include "../examples/float-ops/fp/environment.hpp"
#include "../examples/float-ops/fp/format.hpp"
#include <xmmintrin.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {
using tcfp::hardware::Operation;
using tcfp::hardware::Result;
using tcfp::FPRounding;
int failures = 0;
uint64_t checks = 0, accepted = 0;

void require(bool condition, const char* message) {
    ++checks;
    if (!condition && failures++ < 12) std::printf("FAIL: %s\n", message);
}

Result reference(unsigned width, Operation op, uint64_t a, uint64_t b, FPRounding mode) {
    using namespace tcfp::softfloat_environment;
    SoftFloatOperation environment(mode);
    uint64_t bits = 0;
    if (width == 32) {
        const float32_t x{uint32_t(a)}, y{uint32_t(b)};
        switch (op) {
            case Operation::add: bits = f32_add(x,y).v; break;
            case Operation::subtract: bits = f32_sub(x,y).v; break;
            case Operation::multiply: bits = f32_mul(x,y).v; break;
            case Operation::divide: bits = f32_div(x,y).v; break;
            case Operation::square_root: bits = f32_sqrt(x).v; break;
        }
    } else {
        const float64_t x{a}, y{b};
        switch (op) {
            case Operation::add: bits = f64_add(x,y).v; break;
            case Operation::subtract: bits = f64_sub(x,y).v; break;
            case Operation::multiply: bits = f64_mul(x,y).v; break;
            case Operation::divide: bits = f64_div(x,y).v; break;
            case Operation::square_root: bits = f64_sqrt(x).v; break;
        }
    }
    const auto& layout = tcfp::layoutFor(width);
    if ((bits & layout.exponent_mask) == layout.exponent_mask &&
        (bits & layout.significand_mask)) bits = layout.canonical_nan;
    return {bits,environment.flags()};
}

bool evaluate(unsigned width, Operation op, uint64_t a, uint64_t b,
              FPRounding mode, Result* out) {
    return width == 32 ? tcfp::hardware::evaluate32(op,uint32_t(a),uint32_t(b),mode,out)
                       : tcfp::hardware::evaluate64(op,a,b,mode,out);
}

Result integrated(unsigned width, Operation op, uint64_t a, uint64_t b, FPRounding mode) {
    const auto& api = *tcfp::formatOps(width);
    tcfp::FormatResult result{};
    switch (op) {
        case Operation::add: result=api.add(a,b,mode); break;
        case Operation::subtract: result=api.subtract(a,b,mode); break;
        case Operation::multiply: result=api.multiply(a,b,mode); break;
        case Operation::divide: result=api.divide(a,b,mode); break;
        case Operation::square_root: result=api.square_root(a,mode); break;
    }
    return {result.bits,result.flags};
}

void compare(unsigned width, Operation op, uint64_t a, uint64_t b,
             FPRounding mode, unsigned csr) {
    const auto want=reference(width,op,a,b,mode);
    const unsigned saved=_mm_getcsr();
    _mm_setcsr(csr);
    Result got{};
    const bool used=evaluate(width,op,a,b,mode,&got);
    const unsigned after=_mm_getcsr();
    const auto publicResult=integrated(width,op,a,b,mode);
    const unsigned publicAfter=_mm_getcsr();
    _mm_setcsr(saved);
    require(after==csr && publicAfter==csr,"caller's MXCSR was changed");
    if (used) {
        ++accepted;
        require(got.bits==want.bits && got.flags==want.flags,"hardware bits/flags differ from SoftFloat");
    }
    require(publicResult.bits==want.bits && publicResult.flags==want.flags,
            "public arithmetic bits/flags differ from SoftFloat");
}

uint64_t randomBits(uint64_t& state) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return state;
}

void benchmark() {
    constexpr unsigned count=2000000;
    volatile uint64_t checksum=0;
    for (unsigned width : {32u,64u}) {
        const uint64_t base=width==32?UINT64_C(0x3f800000):UINT64_C(0x3ff0000000000000);
        for (Operation op : {Operation::add,Operation::multiply,Operation::divide,Operation::square_root}) {
            double seconds[2]{};
            for (unsigned path=0;path<2;++path) {
                const auto begin=std::chrono::steady_clock::now();
                for (unsigned i=0;i<count;++i) {
                    const uint64_t fraction=width==32?UINT64_C(0x007fffff):UINT64_C(0x000fffffffffffff);
                    const uint64_t a=base+((uint64_t(i)*UINT64_C(0x9e3779b97f4a7c15))&fraction);
                    const uint64_t b=base+((uint64_t(i+17)*UINT64_C(0xbf58476d1ce4e5b9))&fraction);
                    const auto r=path?integrated(width,op,a,b,FPRounding::nearest_even)
                                     :reference(width,op,a,b,FPRounding::nearest_even);
                    checksum+=r.bits+r.flags;
                }
                seconds[path]=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
            }
            std::printf("BENCH FP%u op=%u SoftFloat=%.1f ns CPU=%.1f ns ratio=%.2fx\n",width,
                        unsigned(op),seconds[0]*1e9/count,seconds[1]*1e9/count,seconds[0]/seconds[1]);
        }
    }
    std::printf("checksum=%llu\n",static_cast<unsigned long long>(checksum));
}
}

int main(int argc,char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--backend") == 0) {
        const bool wantCpu = std::strcmp(argv[2], "cpu") == 0;
        require(tcfp::hardware::enabled() == wantCpu, "unexpected arithmetic backend");
        std::printf("%s backend selection: %s\n", failures ? "FAIL" : "PASS",
                    tcfp::hardware::enabled() ? "CPU" : "SoftFloat");
        return failures ? 1 : 0;
    }
    // Enable the public dispatch before its first call, in this test only.
    _putenv_s("TC_FLOATOPS_CPU", "1");
    if (argc>1 && std::strcmp(argv[1],"--bench")==0) { benchmark(); return 0; }
    for (unsigned width : {32u,64u}) {
        const uint64_t one=width==32?UINT64_C(0x3f800000):UINT64_C(0x3ff0000000000000);
        const uint64_t two=width==32?UINT64_C(0x40000000):UINT64_C(0x4000000000000000);
        const uint64_t four=width==32?UINT64_C(0x40800000):UINT64_C(0x4010000000000000);
        for (Operation op : {Operation::add,Operation::subtract,Operation::multiply,
                             Operation::divide,Operation::square_root}) {
            Result r{};
            require(evaluate(width,op,op==Operation::square_root?four:two,one,
                             FPRounding::nearest_even,&r),"ordinary arithmetic did not use CPU backend");
            require(!evaluate(width,op,two,one,FPRounding::ties_away,&r),
                    "ties-away must use the software fallback");
        }
        const auto& layout=tcfp::layoutFor(width);
        const uint64_t edge[]={0,layout.sign_mask,1,layout.significand_mask,
            layout.significand_mask+1,layout.significand_mask+2,one,one+1,two,four,
            layout.infinity-1,layout.infinity,layout.sign_mask|layout.infinity,
            layout.canonical_nan,layout.infinity|1};
        for (auto a:edge) for (auto b:edge) for (unsigned mode=0;mode<5;++mode)
            for (unsigned op=0;op<5;++op)
                compare(width,Operation(op),a,b,FPRounding(mode),0xbfff);
        uint64_t state=UINT64_C(0x987654321badcafe);
        for (unsigned i=0;i<30000;++i) {
            uint64_t a=randomBits(state),b=randomBits(state);
            if (width==32) { a=uint32_t(a); b=uint32_t(b); }
            for (unsigned mode=0;mode<5;++mode) for (unsigned op=0;op<5;++op)
                compare(width,Operation(op),a,b,FPRounding(mode),(i&1)?0x1f80:0x0040);
        }
    }
    require(accepted>100000,"too few hardware results were exercised");
    std::printf("%s CPU floating-point: %llu checks, %llu hardware results, %d failures\n",
                failures?"FAIL":"PASS",static_cast<unsigned long long>(checks),
                static_cast<unsigned long long>(accepted),failures);
    return failures?1:0;
}
#endif
