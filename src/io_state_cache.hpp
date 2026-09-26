#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace tc::io_state_cache {

/* build_io_state_view reads three Nim sequences from these offsets in the
   presenter context.  Each sequence is represented by a length and a payload
   pointer, so get_io_states returns six machine words in total. */
inline constexpr std::size_t input_offset=0xda30;
inline constexpr std::size_t output_offset=0xda40;
inline constexpr std::size_t other_offset=0xda50;

struct Groups {
 std::array<std::uintptr_t,6> words{};
};

/* Keep ownership operations in the game: sequence elements contain managed
   Nim strings, so a byte copy would either leak them or leave dangling names.
   The supplied copy/destroy functions are the game's generated operations. */
template<class Generate,class Copy,class Destroy>
bool refresh(void* board,void* context,Generate&& generate,Copy&& copy,Destroy&& destroy){
 if(!board||!context)return false;
 Groups fresh{};
 std::forward<Generate>(generate)(board,&fresh);
 auto* cache=static_cast<unsigned char*>(context);
 std::forward<Copy>(copy)(cache+input_offset,fresh.words.data());
 std::forward<Copy>(copy)(cache+output_offset,fresh.words.data()+2);
 std::forward<Copy>(copy)(cache+other_offset,fresh.words.data()+4);
 std::forward<Destroy>(destroy)(&fresh);
 return true;
}

} // namespace tc::io_state_cache
