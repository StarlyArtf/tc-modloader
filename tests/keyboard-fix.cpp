/* The built-in Keyboard fix is a rewrite of the program the game generates, so
   the input of the unit test is the shape that program really has.  The two
   blocks below are copied verbatim from captures of the pinned build
   (TC_MODLOADER_DUMP_SOURCE=1, a board with one 0x5d Keyboard):

   * `undriven` is what the game emitted when nothing drove the read strobe -
     the case the fix exists for;
   * `wired` is the same block with the strobe wired to a Constant, whose value
     the generator resolved into a real operand that has to be left alone.

   The per-frame refresh block is included as well: it publishes the outputs
   from the same cache and must not be touched by the rewrite. */
#include "../src/keyboard_fix.hpp"
#include <cassert>
#include <iostream>

namespace {

const char* refreshBlock =
    "    // 2 com_keyboard 8 2 \n"
    "    let value_id258 = load(<U32>, #SIMULATION_STATE + 272) >> 16\n"
    "    store(#SIMULATION_STATE + 258, U1 (value_id258))\n"
    "    store(#SIMULATION_STATE + 259, U1 (value_id258))\n"
    "    let value_id260 = load(<U1>, #SIMULATION_STATE + 272)\n"
    "    store(#SIMULATION_STATE + 260, U1 (value_id260))\n"
    "    store(#SIMULATION_STATE + 261, U1 (value_id260))\n"
    "    let value_id262 = load(<U16>, #SIMULATION_STATE + 272) >> 8\n"
    "    store(#SIMULATION_STATE + 262, U8 (value_id262))\n"
    "    store(#SIMULATION_STATE + 263, U8 (value_id262))\n";

/* Nothing drives the read pin, so the generator resolved it to `(U1 0x0)`. */
const char* undrivenBlock =
    "            // 3 com_keyboard 8 3 \n"
    "            var vid256 = U1 0\n"
    "            var vid258 = U1 0\n"
    "            var vid260 = U8 0\n"
    "            let read_index_3 = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4096) & 0xfff\n"
    "            let write_index_3 = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4098) & 0xfff\n"
    "            let is_new_value_3 = read_index_3 != write_index_3\n"
    "            if is_new_value_3 && (U1 0x0) != 0 {\n"
    "                let value = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + Int read_index_3)\n"
    "                vid256 = U1 is_new_value_3\n"
    "                vid258 = U1 value\n"
    "                vid260 = U8 (value >> 8)\n"
    "                store(#SIMULATION_KEYBOARD_CHARACTER + 4096, U16 (read_index_3 + 8))\n"
    "                store(#SIMULATION_STATE + 270, ((U32 is_new_value_3) << 16) | U32 value) // Output cache\n"
    "            } else {\n"
    "                vid256 = U1 (read_index_3 != load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4098))\n"
    "                store(#SIMULATION_STATE + 270, U32 is_new_value_3 << 16)\n"
    "            }\n";

/* The same block with the strobe driven; the generator kept the operand. */
const char* wiredBlock =
    "            // 2 com_keyboard 8 2 \n"
    "            var vid258 = U1 0\n"
    "            var vid260 = U1 0\n"
    "            var vid262 = U8 0\n"
    "            let read_index_2 = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4096) & 0xfff\n"
    "            let write_index_2 = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4098) & 0xfff\n"
    "            let is_new_value_2 = read_index_2 != write_index_2\n"
    "            if is_new_value_2 && (U1 ((U1 vid256))) != 0 {\n"
    "                let value = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + Int read_index_2)\n"
    "                vid258 = U1 is_new_value_2\n"
    "                vid260 = U1 value\n"
    "                vid262 = U8 (value >> 8)\n"
    "                store(#SIMULATION_KEYBOARD_CHARACTER + 4096, U16 (read_index_2 + 8))\n"
    "                store(#SIMULATION_STATE + 272, ((U32 is_new_value_2) << 16) | U32 value) // Output cache\n"
    "            } else {\n"
    "                vid258 = U1 (read_index_2 != load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4098))\n"
    "                store(#SIMULATION_STATE + 272, U32 is_new_value_2 << 16)\n"
    "            }\n";

/* The block from the board that reported "it will not write into memory": the
   strobe is held high by a constant, and a Store port with its write enable
   held on stores `(U8 vid276)` - the key code local - into a RAM every cycle.
   Before the third part of the rewrite that local is 0 on every cycle except
   the one that read the event, so the RAM ends up holding 0. */
const char* storePortBlock =
    "            // 14 com_keyboard 8 8859308081459214077 \n"
    "            var vid514 = U1 0\n"
    "            var vid274 = U1 0\n"
    "            var vid276 = U8 0\n"
    "            let read_index_14 = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4096) & 0xfff\n"
    "            let write_index_14 = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4098) & 0xfff\n"
    "            let is_new_value_14 = read_index_14 != write_index_14\n"
    "            if is_new_value_14 && (U1 vid272) != 0 {\n"
    "                let value = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + Int read_index_14)\n"
    "                vid514 = U1 is_new_value_14\n"
    "                vid274 = U1 value\n"
    "                vid276 = U8 (value >> 8)\n"
    "                store(#SIMULATION_KEYBOARD_CHARACTER + 4096, U16 (read_index_14 + 8))\n"
    "                store(#SIMULATION_STATE + 506, ((U32 is_new_value_14) << 16) | U32 value) // Output cache\n"
    "            } else {\n"
    "                vid514 = U1 (read_index_14 != load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4098))\n"
    "                store(#SIMULATION_STATE + 506, U32 is_new_value_14 << 16)\n"
    "            }\n"
    "\n"
    "            // 19 com_store_port 8 6666045368899583624 \n"
    "            if Bool (U1 vid278) {\n"
    "                store(#DATA_7566143460790582214 + Int (65535 - ((U32 vid256) & 65535)), (U8 vid276))\n"
    "\n"
    "            }\n";

bool has(const std::string& text,const char* needle) {
    return text.find(needle)!=std::string::npos;
}

}  // namespace

int main() {
    using namespace tc::keyboard_fix;

    /* The zero-literal strobe is what an unwired pin compiles to, and only a
       zero literal may be dropped: a real operand, or a constant the player
       wired to 1, keeps gating the read. */
    assert(zeroLiteralStrobe("(U1 0x0) != 0"));
    assert(zeroLiteralStrobe("(U1 0) != 0"));
    assert(zeroLiteralStrobe("(U1 0b0) != 0"));
    assert(!zeroLiteralStrobe("(U1 0x1) != 0"));
    assert(!zeroLiteralStrobe("(U1 ((U1 vid256))) != 0"));
    assert(!zeroLiteralStrobe("load(<U1>, #SIMULATION_STATE + 256)"));

    /* Undriven: the block is read every time an event waits, and the else
       branch now keeps state+key code instead of zeroing them. */
    {
        std::string text=std::string(refreshBlock)+undrivenBlock;
        const std::string beforeRefresh=text.substr(0,std::string(refreshBlock).size());
        assert(apply(text)==1);
        assert(has(text,"if is_new_value_3 {\n"));
        assert(!has(text,"(U1 0x0) != 0"));
        assert(has(text,"store(#SIMULATION_STATE + 270, ((U32 is_new_value_3) << 16) | "
                        "U32 (load(<U16>, #SIMULATION_STATE + 270)))"));
        /* The latch has to reach the block's own per-cycle locals, or the parts
           that read the Keyboard inside a cycle keep seeing 0. */
        assert(has(text,"vid258 = U1 (load(<U1>, #SIMULATION_STATE + 270))"));
        assert(has(text,"vid260 = U8 ((load(<U16>, #SIMULATION_STATE + 270)) >> 8)"));
        /* The per-frame publish block and the pop branch's own cache write are
           left exactly as the game emitted them. */
        assert(text.substr(0,std::string(refreshBlock).size())==beforeRefresh);
        assert(has(text,"store(#SIMULATION_STATE + 270, ((U32 is_new_value_3) << 16) | U32 value)"));
        /* Idempotent. */
        const std::string fixed=text;
        assert(apply(text)==0);
        assert(text==fixed);
    }

    /* Wired: the read stays gated by the player's signal, but the outputs must
       still hold the last event instead of collapsing to 0. */
    {
        std::string text=wiredBlock;
        assert(apply(text)==1);
        assert(has(text,"if is_new_value_2 && (U1 ((U1 vid256))) != 0 {\n"));
        assert(has(text,"store(#SIMULATION_STATE + 272, ((U32 is_new_value_2) << 16) | "
                        "U32 (load(<U16>, #SIMULATION_STATE + 272)))"));
        /* This block's own numbering: waiting=vid258, state=vid260, key=vid262. */
        assert(has(text,"vid260 = U1 (load(<U1>, #SIMULATION_STATE + 272))"));
        assert(has(text,"vid262 = U8 ((load(<U16>, #SIMULATION_STATE + 272)) >> 8)"));
    }

    /* The reported board: the Store port reads the key code local, so the two
       restored lines are what makes it store a key instead of 0. */
    {
        std::string text=storePortBlock;
        assert(apply(text)==1);
        assert(has(text,"if is_new_value_14 && (U1 vid272) != 0 {\n"));
        assert(has(text,"vid274 = U1 (load(<U1>, #SIMULATION_STATE + 506))"));
        assert(has(text,"vid276 = U8 ((load(<U16>, #SIMULATION_STATE + 506)) >> 8)"));
        assert(has(text,"store(#SIMULATION_STATE + 506, ((U32 is_new_value_14) << 16) | "
                        "U32 (load(<U16>, #SIMULATION_STATE + 506)))"));
        assert(has(text,"(U8 vid276)"));
        const std::string fixed=text;
        assert(apply(text)==0);
        assert(text==fixed);
    }
    /* A strobe the player wired to a constant keeps gating the read; only the
       outputs are made to hold the last event. */
    {
        std::string text=
            "            // 2 com_keyboard 8 2 \n"
            "            if is_new_value_2 && (U1 0x1) != 0 {\n"
            "            } else {\n"
            "                store(#SIMULATION_STATE + 272, U32 is_new_value_2 << 16)\n"
            "            }\n";
        assert(apply(text)==1);
        assert(has(text,"if is_new_value_2 && (U1 0x1) != 0 {\n"));
        assert(!has(text,"U32 is_new_value_2 << 16)"));
    }
    /* A program with no Keyboard at all is left alone. */
    {
        std::string text=
            "            // 7 com_and_bit 2 6482963286758231035 \n"
            "            let value_id256 = load(<U1>, #SIMULATION_STATE + 258) & (U1 0x1)\n"
            "            store(#SIMULATION_STATE + 256, U1 (value_id256))\n";
        const std::string untouched=text;
        assert(apply(text)==0);
        assert(text==untouched);
    }

    std::cout<<"keyboard fix tests passed\n";
    return 0;
}
