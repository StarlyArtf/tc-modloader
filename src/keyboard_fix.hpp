#pragma once
/* The built-in Keyboard component (kind 0x5d) is fixed here, in the program
   the game generates for a board, instead of being reimplemented as a Mod
   component.  The kind, the pins, the sprite and the palette entry stay the
   game's own, so every board that carries a Keyboard keeps the game's
   component and only the emitted behaviour changes.

   What the game emits for one Keyboard (captured with
   `TC_MODLOADER_DUMP_SOURCE=1`; see docs/reference/diagnostics.md) is:

       let read_index_3  = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4096) & 0xfff
       let write_index_3 = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4098) & 0xfff
       let is_new_value_3 = read_index_3 != write_index_3
       if is_new_value_3 && (U1 0x0) != 0 {
           let value = load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + Int read_index_3)
           vid256 = U1 is_new_value_3
           vid258 = U1 value
           vid260 = U8 (value >> 8)
           store(#SIMULATION_KEYBOARD_CHARACTER + 4096, U16 (read_index_3 + 8))
           store(#SIMULATION_STATE + 270, ((U32 is_new_value_3) << 16) | U32 value) // Output cache
       } else {
           vid256 = U1 (read_index_3 != load(<U16>, #SIMULATION_KEYBOARD_CHARACTER + 4098))
           store(#SIMULATION_STATE + 270, U32 is_new_value_3 << 16)
       }

   The three outputs are out0 = "an event is waiting", out1 = the event's
   key-down flag and out2 = its 8-bit key code; the single input is the read
   strobe.  Two things in that block make the component look like it hears the
   keyboard (events do reach the buffer - the game records them while the
   simulation is not halted) but never reports a key:

   1. When the strobe pin is not driven, the game's code generator resolves it
      to the literal zero and emits `&& (U1 0x0) != 0`, so the pop branch can
      never run: out1/out2 stay 0 forever and only out0 moves.  A strobe that
      *is* wired keeps its own expression (`&& (U1 ((U1 vid256))) != 0` in the
      same capture), which is why this rewrite only touches the zero literal.
   2. The else branch stores `is_new << 16` over the output cache, throwing
      away the key-down flag and the key code of the event that was just read.
      Everything between two events therefore publishes out1 = out2 = 0, and
      because the game publishes these slots once per frame (mode_refresh) with
      the simulation running thousands of cycles per burst, a key that was
      genuinely read is almost never visible.

   The rewrite keeps the block's shape and both of its branches:

   * a zero-literal strobe becomes "read whenever an event is waiting", so an
     unwired Keyboard finally produces its key code and key state;
   * the else branch refreshes only the waiting flag and leaves the 16 bits
     that carry state+key code alone, so the outputs hold the last event until
     the next one is read - the behaviour a "keyboard" is expected to have.
   * the else branch also restores the block's two per-cycle local variables
     (state and key code) from that cache.  Without this the latch is invisible
     to the parts that read the Keyboard *within* a cycle: the game hands a
     consumer the producer's local variable, not the published slot, so a Store
     port wired to the key code with its write enable held on writes the key
     for the single cycle the event was read and overwrites it with 0 on every
     cycle after - a memory that never keeps a key.  Measured on the board that
     reported it: `store(#DATA_<ram> + ..., (U8 vid276))` with `vid276` assigned
     only inside the if branch.

   A strobe that the player actually drives still gates the read, so recorded
   sequences keep their deterministic, cycle-by-cycle replay.

   Set TC_MODLOADER_KEYBOARD_FIX=0 to leave the generated program untouched
   (one read per process, like the other loader switches). */
#include <cstddef>
#include <cstdlib>
#include <string>

namespace tc {
namespace keyboard_fix {

inline bool enabled() {
    static const bool on=[]{
        const char* value=std::getenv("TC_MODLOADER_KEYBOARD_FIX");
        /* Only an explicit leading zero disables it. */
        return !(value&&value[0]=='0');
    }();
    return on;
}

inline bool blank(char c) {
    return c==' '||c=='\t'||c=='\r'||c=='\n';
}

/* Whitespace-free copy, so a comparison does not depend on the generator's
   spacing. */
inline std::string squeeze(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for(char c:input) if(!blank(c)) out.push_back(c);
    return out;
}

/* Runs of whitespace become one space: the type token and the literal that
   follow it stay separate. */
inline std::string collapse(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool space=false;
    for(char c:input) {
        if(blank(c)) {space=true;continue;}
        if(space&&!out.empty()) out.push_back(' ');
        space=false;
        out.push_back(c);
    }
    return out;
}

/* The identifier that ends just before `at` - the variable name a line like
   "vid276 = U8 (value >> 8)" assigns to. */
inline std::string identifierBefore(const std::string& text,std::string::size_type at) {
    auto start=at;
    while(start>0) {
        const char c=text[start-1];
        const bool name=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
        if(!name) break;
        --start;
    }
    return text.substr(start,at-start);
}

/* True for a strobe operand the generator resolved to a constant zero, i.e.
   "(U1 0x0) != 0".  Any operand that names a value (a loaded pin, a local
   variable, a constant that is not zero) keeps the original semantics. */
inline bool zeroLiteralStrobe(const std::string& condition) {
    const std::string text=collapse(condition);
    const auto close=text.find(')');
    if(close==std::string::npos) return false;
    if(squeeze(text.substr(close))!=")!=0") return false;
    const std::string head=text.substr(1,close-1);   /* "U1 0x0" */
    const auto space=head.rfind(' ');
    if(space==std::string::npos||space+1>=head.size()) return false;
    std::string literal=head.substr(space+1);
    if(literal.size()>2&&literal[0]=='0'&&
       (literal[1]=='x'||literal[1]=='X'||literal[1]=='b'||literal[1]=='o')) {
        literal=literal.substr(2);
    }
    bool digit=false;
    for(char c:literal) {
        if(c=='_') continue;
        if(c!='0') return false;
        digit=true;
    }
    return digit;
}

/* Rewrites every Keyboard block in `text`.  Returns the number of blocks that
   changed; the transform is idempotent, so a second pass returns 0. */
inline int apply(std::string& text) {
    if(!enabled()) return 0;
    const std::string keyword="com_keyboard";
    std::string::size_type search=0;
    int changed=0;
    while(true) {
        const auto header=text.find(keyword,search);
        if(header==std::string::npos) break;
        search=header+keyword.size();
        /* The block header is the comment "<indent>// <node> com_keyboard
           <bits> <component>".  The node number in front of the keyword is the
           suffix every variable in the block carries. */
        auto before=header;
        while(before>0&&blank(text[before-1])) --before;
        auto digits=before;
        while(digits>0&&text[digits-1]>='0'&&text[digits-1]<='9') --digits;
        if(digits==before) continue;
        const auto line=text.rfind('\n',header);
        const auto lineStart=line==std::string::npos?std::string::size_type(0):line+1;
        if(digits<lineStart||text.find("//",lineStart)>=digits) continue;
        const std::string suffix=text.substr(digits,before-digits);
        bool block=false;

        /* The else branch's store both names the output cache and is the line
           the latch rewrite replaces.  Matched by its own shape, so the if
           branch (which writes the value) and the per-frame refresh block are
           never touched. */
        const std::string storeNeedle="U32 is_new_value_"+suffix+" << 16)";
        std::string cache;
        {
            const auto store=text.find(storeNeedle,header);
            static const char storePrefix[]="store(#SIMULATION_STATE + ";
            const auto stateAt=store==std::string::npos?std::string::npos:text.rfind(storePrefix,store);
            if(stateAt!=std::string::npos) {
                const auto digitsAt=stateAt+(sizeof(storePrefix)-1);
                auto end=digitsAt;
                while(end<store&&text[end]>='0'&&text[end]<='9') ++end;
                if(end>digitsAt&&text.compare(end,2,", ")==0)
                    cache=text.substr(digitsAt,end-digitsAt);
            }
        }

        /* The two per-cycle locals the if branch fills in; the else branch has
           to restore them from the cache or the latch stays invisible to every
           part that reads the Keyboard inside a cycle. */
        const auto stateAssign=text.find(" = U1 value\n",header);
        const auto keyAssign=text.find(" = U8 (value >> 8)\n",header);
        const std::string elseNeedle=" = U1 (read_index_"+suffix+" != load(<U16>, #SIMULATION_KEYBOARD_";
        const auto elseAssign=text.find(elseNeedle,header);
        std::string varsInsert;
        std::string::size_type varsAt=std::string::npos;
        if(!cache.empty()&&stateAssign!=std::string::npos&&keyAssign!=std::string::npos&&
           stateAssign<keyAssign&&elseAssign!=std::string::npos) {
            const auto stateName=identifierBefore(text,stateAssign);
            const auto keyName=identifierBefore(text,keyAssign);
            const auto elseEnd=text.find('\n',elseAssign);
            const auto elseLine=text.rfind('\n',elseAssign);
            if(!stateName.empty()&&!keyName.empty()&&elseEnd!=std::string::npos) {
                /* Idempotent: the restored line is already there after one pass. */
                const std::string restored=stateName+" = U1 (load(<U1>, #SIMULATION_STATE + "+cache+"))";
                if(text.find(restored,elseAssign)==std::string::npos) {
                    std::string indent;
                    for(auto at=elseLine==std::string::npos?std::string::size_type(0):elseLine+1;
                        at<elseAssign&&(text[at]==' '||text[at]=='\t');++at)
                        indent.push_back(text[at]);
                    varsInsert=indent+restored+"\n"+
                               indent+keyName+" = U8 ((load(<U16>, #SIMULATION_STATE + "+cache+")) >> 8)\n";
                    varsAt=elseEnd+1;
                }
            }
        }

        /* Applied back to front: the insert point sits before the store line,
           and the condition sits before both. */
        if(varsAt!=std::string::npos&&!varsInsert.empty()) {
            text.insert(varsAt,varsInsert);
            block=true;
        }
        {
            const auto store=text.find(storeNeedle,header);
            if(store!=std::string::npos&&!cache.empty()) {
                text.replace(store,storeNeedle.size(),
                             "((U32 is_new_value_"+suffix+") << 16) | U32 (load(<U16>, #SIMULATION_STATE + "+
                                 cache+"))"+")");
                block=true;
            }
        }
        /* Condition: drop a strobe the generator emitted as constant zero. */
        const std::string condNeedle="if is_new_value_"+suffix+" && ";
        const auto cond=text.find(condNeedle,header);
        if(cond!=std::string::npos) {
            const auto condStart=cond+condNeedle.size();
            const auto condEnd=text.find(" {",condStart);
            if(condEnd!=std::string::npos&&
               zeroLiteralStrobe(text.substr(condStart,condEnd-condStart))) {
                text.erase(cond+3+std::string("is_new_value_").size()+suffix.size(),
                           condEnd-(condStart-4));
                block=true;
            }
        }
        if(block) ++changed;
    }
    return changed;
}

}  // namespace keyboard_fix
}  // namespace tc
