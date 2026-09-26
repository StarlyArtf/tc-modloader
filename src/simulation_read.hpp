#pragma once
#include <cstdint>

namespace tc::sim {
/* The simulator allocates its state buffer once at init; the size comes from
   that allocation site and is the same number the sim-state probe documents
   (docs/sdk/simulation.md, tests/sim-state-probe.cpp).  Reads are a plain u64
   load through the game's own sim_state_read_u64, so the only bounds check the
   service can honestly make is against this buffer. */
inline constexpr uint64_t kStateBufferSize=0x9c4000;
inline constexpr uint32_t kMaxBits=64;

inline uint64_t bitMask(uint32_t bits){
    return bits>=kMaxBits?~uint64_t{0}:((uint64_t{1}<<bits)-1);
}
/* A read touches eight bytes starting at the offset. */
inline bool offsetInRange(uint64_t byteOffset){
    return byteOffset<=kStateBufferSize-8;
}
/* Mirrors the game's sim_state_read_bits: the low `bits` of the word. */
inline uint64_t lowBits(uint64_t word,uint32_t bits){
    return word&bitMask(bits);
}
}
