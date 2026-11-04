#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <set>
#include <limits>
#include <array>
#include <algorithm>
#include "simulation_registers.hpp"
#include "../vendor/minhook/src/hde/hde64.h"
namespace tc::simulation_jit {
struct Result {std::vector<uint8_t> code;int64_t entry=0;unsigned removed=0,specialized=0,masked=0,deadConstants=0,deadCopies=0,redundantMasks=0,fallthroughJumps=0,stackForwards=0,equivalentCopies=0,signedExtensions=0,unusedExtensions=0,unusedMasks=0,flowCopies=0,redundantStores=0;};
inline Result optimize(const uint8_t* bytes,size_t size,int64_t entry) {
    if(!bytes || size==0 || size>16000000 || entry<0 || uint64_t(entry)>=size)return {};
    // HDE may read ahead while decoding. Padding prevents even a truncated
    // final encoding from reading outside the caller's byte sequence.
    std::vector<uint8_t> padded(size+16,0);
    std::memcpy(padded.data(),bytes,size);
    struct Instruction {
        size_t at=0,length=0,newAt=0;
        int dst=-1,src=-1;
        int shiftReg=-1,shiftDirection=0;
        unsigned relative=0;
        int64_t target=0;
        bool keep=true,zeroCount=false,shift=false,countKnown=false,immediate=false,fallthroughJump=false;
        bool flagsWrite=false,flagsNeutral=false;
        uint8_t count=0;
        std::vector<uint8_t> replacement;
        RegisterEffect registers;
        hde64s finalEncoding{};
    };
    std::vector<Instruction> instructions;
    std::set<size_t> boundaries,targets{size_t(entry)};
    for(size_t at=0;at<size;) {
        hde64s h{};hde64_disasm(padded.data()+at,&h);
        if((h.flags&F_ERROR) || h.len==0 || at+h.len>size)return {};
        // Vendored HDE does not reliably decode 32-bit address overrides in
        // long mode. Keep the entire original block rather than relocate it.
        if(h.p_67)return {};
        // This pass relocates only direct code branches. Unknown code-address
        // dependencies, inline data references and jump tables stay untouched.
        if((h.flags&F_MODRM) && h.modrm_mod==0 && h.modrm_rm==5)return {};
        if((h.opcode==0xff && (h.modrm_reg==4 || h.modrm_reg==5)) ||
           h.opcode==0xea || h.opcode==0x9a || (h.opcode>=0xd8 && h.opcode<=0xdf) ||
           (h.opcode==0x0f && (h.opcode2==5 || h.opcode2==7 || h.opcode2==0x34 || h.opcode2==0x35)))return {};
        Instruction instruction;instruction.at=at;instruction.length=h.len;
        if(h.flags&F_RELATIVE) {
            if(h.flags&F_IMM8) {instruction.relative=1;instruction.target=int64_t(at+h.len)+int8_t(h.imm.imm8);}
            else if(h.flags&F_IMM32) {instruction.relative=4;instruction.target=int64_t(at+h.len)+int32_t(h.imm.imm32);}
            else return {};
            if(h.opcode==0xe8 && instruction.target==int64_t(at+h.len))return {}; // call/pop PC materialization
            if(instruction.target<0 || uint64_t(instruction.target)>=size)return {};
            instruction.fallthroughJump=instruction.target==int64_t(at+h.len) &&
                ((h.len==5 && padded[at]==0xe9) || (h.len==2 && padded[at]==0xeb));
            // This edge has no join: execution already falls through here.
            // Other incoming branches and the explicit entry still add targets.
            if(!instruction.fallthroughJump)targets.insert(size_t(instruction.target));
        }
        // Full-width register moves only: 32-bit writes zero the high half.
        if(h.len==3 && h.rex_w && (h.opcode==0x89 || h.opcode==0x8b) && h.modrm_mod==3) {
            const int reg=h.modrm_reg+8*h.rex_r,rm=h.modrm_rm+8*h.rex_b;
            instruction.dst=h.opcode==0x89?rm:reg;instruction.src=h.opcode==0x89?reg:rm;
            if(instruction.dst==4)instruction.dst=instruction.src=-1; // stack pointer
        }
        // Match only canonical MOV ECX,imm32. HDE leaves h.rex unset; a
        // prefixed five-byte MOV R9W,imm16 must never be treated as defining CL.
        instruction.countKnown=h.len==5 && padded[at]==0xb9 && h.opcode==0xb9 && (h.flags&F_IMM32);
        instruction.zeroCount=instruction.countKnown && h.imm.imm32==0;
        instruction.count=uint8_t(h.imm.imm32);
        instruction.shift=h.len==3 && h.rex_w && h.opcode==0xd3 && h.modrm_mod==3 &&
            (h.modrm_reg==4 || h.modrm_reg==5 || h.modrm_reg==7) && h.modrm_rm+8*h.rex_b!=4;
        if(instruction.shift){instruction.shiftReg=h.modrm_rm+8*h.rex_b;instruction.shiftDirection=h.modrm_reg;}
        // ADD/SUB/CMP replace all arithmetic flags. Other recognized forms
        // below may write flags, but never consume the preceding flags.
        const bool group=h.opcode==0x80 || h.opcode==0x81 || h.opcode==0x83;
        instruction.flagsWrite=(h.opcode<=0x05) || (h.opcode>=0x28 && h.opcode<=0x2d) ||
             (h.opcode>=0x38 && h.opcode<=0x3d) ||
             (group && (h.modrm_reg==0 || h.modrm_reg==5 || h.modrm_reg==7));
        instruction.flagsNeutral=(h.opcode>=0x88 && h.opcode<=0x8b) || h.opcode==0x8d ||
            (h.opcode>=0xb0 && h.opcode<=0xbf) || h.opcode==0xc6 || h.opcode==0xc7 ||
            h.opcode==0x63 || h.opcode==0x90 ||
            (h.opcode==0x0f && (h.opcode2==0xb6 || h.opcode2==0xb7 || h.opcode2==0xbe || h.opcode2==0xbf)) ||
            ((h.opcode==0xc1 || h.opcode==0xd1 || h.opcode==0xd3) &&
             (h.modrm_reg==4 || h.modrm_reg==5 || h.modrm_reg==7)) ||
            (h.opcode>=0x08 && h.opcode<=0x0d) || (h.opcode>=0x20 && h.opcode<=0x25) ||
            (h.opcode>=0x30 && h.opcode<=0x35) || h.opcode==0x84 || h.opcode==0x85 ||
            h.opcode==0xa8 || h.opcode==0xa9 ||
            (group && (h.modrm_reg==1 || h.modrm_reg==4 || h.modrm_reg==6)) ||
            ((h.opcode==0xf6 || h.opcode==0xf7) && h.modrm_reg==0);
        boundaries.insert(at);instructions.push_back(instruction);at+=h.len;
    }
    for(auto target:targets)if(!boundaries.count(target))return {};
    unsigned removed=0,fallthroughJumps=0;
    for(size_t i=0;i<instructions.size();++i) {
        auto& current=instructions[i];
        if(current.fallthroughJump){current.keep=false;++fallthroughJumps;}
        if(current.dst>=0 && current.dst==current.src)current.keep=false;
        if(i>0 && !targets.count(current.at)) {
            const auto& previous=instructions[i-1];
            if(previous.dst>=0 && current.dst>=0 && previous.dst==current.src && previous.src==current.dst)
                current.keep=false; // A=B; B=A leaves the second destination unchanged
            if(previous.zeroCount && current.shift)current.keep=false; // shift by zero preserves flags
        }
        if(!current.keep)++removed;
    }
    const auto flagsAreOverwritten=[&](size_t i)->bool {
        for(size_t j=i+1;j<instructions.size() && j<=i+256;++j) {
            const auto& next=instructions[j];if(!next.keep)continue;
            if(next.flagsWrite)return true;
            if(!next.flagsNeutral)return false;
        }
        return false;
    };
    unsigned masked=0,signedExtensions=0;
    std::vector<size_t> kept;
    for(size_t i=0;i<instructions.size();++i)if(instructions[i].keep)kept.push_back(i);
    for(size_t i=0;i+4<kept.size();++i) {
        auto& first=instructions[kept[i]];auto& left=instructions[kept[i+1]];
        auto& copy=instructions[kept[i+2]];auto& last=instructions[kept[i+3]];auto& right=instructions[kept[i+4]];
        if(!first.keep || !left.keep || !copy.keep || !last.keep || !right.keep ||
           !first.countKnown || !last.countKnown || !left.shift || !right.shift ||
           left.shiftDirection!=4 || (right.shiftDirection!=5 && right.shiftDirection!=7) || left.shiftReg!=right.shiftReg ||
           left.shiftReg==1 || copy.dst!=1 || copy.src!=left.shiftReg)continue;
        const unsigned count=first.count&63;
        const bool signedExtension=right.shiftDirection==7;
        if(count<32 || (last.count&63)!=count || (signedExtension && count!=32 && count!=48 && count!=56))continue;
        bool incoming=false;
        // Include deleted no-ops in the target guard: a branch into one would
        // enter partway through the original normalization sequence.
        for(size_t j=kept[i]+1;j<=kept[i+4];++j)if(targets.count(instructions[j].at)){incoming=true;break;}
        if(incoming || !flagsAreOverwritten(kept[i+4]))continue;
        // (x << N) >> N retains at most 32 bits for N>=32. A 32-bit AND
        // also clears the upper half, including the N=32 / mask=UINT32_MAX
        // case where a sign-extended 64-bit immediate would be incorrect.
        if(signedExtension) {
            // (x << N) SAR N is sign extension of the low 64-N bits.
            // REX.W selects the real low byte even for BPL/SIL/DIL, and both
            // REX.R/B select the same extended source/destination register.
            const uint8_t reg=uint8_t(left.shiftReg&7);
            left.replacement.push_back(left.shiftReg>=8?0x4d:0x48);
            if(count==32)left.replacement.push_back(0x63);
            else {left.replacement.push_back(0x0f);left.replacement.push_back(count==56?0xbe:0xbf);}
            left.replacement.push_back(uint8_t(0xc0|(reg<<3)|reg));
            ++signedExtensions;
        } else {
            const uint32_t mask=uint32_t((uint64_t(1)<<(64-count))-1);
            if(left.shiftReg>=8)left.replacement.push_back(0x41);
            left.replacement.push_back(mask<=127?0x83:0x81);
            left.replacement.push_back(uint8_t(0xe0|(left.shiftReg&7)));
            if(mask<=127)left.replacement.push_back(uint8_t(mask));
            else for(unsigned byte=0;byte<4;++byte)left.replacement.push_back(uint8_t(mask>>(8*byte)));
            ++masked;
        }
        // The intervening RCX copy is overwritten by the final MOV ECX,
        // which remains so every other register retains its original value.
        first.keep=false;copy.keep=false;right.keep=false;removed+=3;
    }
    unsigned specialized=0;
    for(size_t i=1;i<instructions.size();++i) {
        auto& current=instructions[i];const auto& previous=instructions[i-1];
        if(!current.keep || !current.shift || !previous.keep || !previous.countKnown || targets.count(current.at))continue;
        // Variable and immediate shifts have the same defined flags. Require
        // a full flag overwrite anyway: undefined OF/AF must not be observed
        // through SETcc, a call, a return, or an unrecognized instruction.
        if(flagsAreOverwritten(i)){current.immediate=true;current.count=previous.count;++specialized;}
    }
    for(auto& instruction:instructions) {
        if(!instruction.keep)continue;
        uint8_t finalBytes[16]{};
        if(!instruction.replacement.empty())std::memcpy(finalBytes,instruction.replacement.data(),instruction.replacement.size());
        else {
            std::memcpy(finalBytes,bytes+instruction.at,instruction.length);
            if(instruction.immediate){finalBytes[1]=0xc1;finalBytes[3]=instruction.count;}
        }
        hde64_disasm(finalBytes,&instruction.finalEncoding);instruction.registers=registerEffect(instruction.finalEncoding);
    }
    // Reuse exact spills and zero-extended loads in a straight-line region.
    // A preceding load/store establishes the slot's value and accessibility; every possible
    // indirect write, stack-pointer change, unknown instruction and join
    // invalidates the proof. Register IDs describe actual values, not names.
    struct Spill {int64_t displacement;unsigned width;uint64_t value;bool written=false;};
    std::vector<Spill> spills;
    // Known writes may change a slot's contents without changing the fact
    // that these eight stack bytes were writable in this straight-line region.
    std::vector<int64_t> writableSlots;
    std::array<uint64_t,16> values;
    uint64_t nextValue=0;
    const auto resetValues=[&](){for(auto& value:values)value=++nextValue;spills.clear();writableSlots.clear();};
    resetValues();
    unsigned stackForwards=0,equivalentCopies=0,redundantStores=0;
    for(auto& instruction:instructions) {
        if(targets.count(instruction.at))resetValues();
        if(!instruction.keep)continue;
        auto& effect=instruction.registers;
        const auto& h=instruction.finalEncoding;
        if(!effect.known){resetValues();continue;}
        if(effect.writes&(uint32_t(1)<<4)){spills.clear();writableSlots.clear();}
        const bool memory=(h.flags&F_MODRM) && h.modrm_mod!=3;
        const bool stack=memory && (h.flags&F_SIB) && h.sib_base==4 && h.sib_index==4 &&
            !h.rex_b && !h.rex_x;
        const bool stackStore=stack && h.rex_w && h.opcode==0x89;
        const bool stackLoad=stack && (h.opcode==0x8b ||
            (h.opcode==0x0f && (h.opcode2==0xb6 || h.opcode2==0xb7)));
        const unsigned loadWidth=h.opcode==0x0f?(h.opcode2==0xb6?1:2):(h.rex_w?8:4);
        const int64_t displacement=h.modrm_mod==1?int64_t(int8_t(h.disp.disp8)):
            h.modrm_mod==2?int64_t(int32_t(h.disp.disp32)):0;
        const bool group=h.opcode==0x80 || h.opcode==0x81 || h.opcode==0x83;
        const bool arithmetic=h.opcode<=0x3d && (h.opcode&7)<=3;
        const bool memoryWrite=memory && (h.opcode==0x88 || h.opcode==0x89 || h.opcode==0xc7 ||
            (h.opcode==0x0f && h.opcode2>=0x90 && h.opcode2<=0x9f) ||
            h.opcode==0xc1 || h.opcode==0xd1 || h.opcode==0xd3 ||
            (group && h.modrm_reg!=7) || (arithmetic && !(h.opcode&2) && (h.opcode>>3)!=7));
        if(stackStore) {
            bool duplicate=false;
            for(const auto& spill:spills)if(spill.displacement==displacement && spill.width==8 &&
                spill.written && spill.value==values[h.modrm_reg+8*h.rex_r]){duplicate=true;break;}
            // A load alone proves readability, not write accessibility. Only
            // an earlier surviving store may establish this idempotent write.
            if(duplicate){instruction.keep=false;++removed;++redundantStores;continue;}
        }
        if(memoryWrite) {
            if(!stack)spills.clear();
            else {
                const bool byte=h.opcode==0x88 || h.opcode==0x80 || h.opcode==0x0f ||
                    (arithmetic && !(h.opcode&1));
                const unsigned width=byte?1:(h.rex_w?8:4);
                for(size_t i=0;i<spills.size();) {
                    if(spills[i].displacement<displacement+width && displacement<spills[i].displacement+spills[i].width)
                        spills.erase(spills.begin()+i);
                    else ++i;
                }
                if(stackStore) {
                    spills.push_back({displacement,8,values[h.modrm_reg+8*h.rex_r],true});
                    if(std::find(writableSlots.begin(),writableSlots.end(),displacement)==writableSlots.end())
                        writableSlots.push_back(displacement);
                }
            }
        } else if(stackLoad && effect.dst!=4) {
            int source=-1;
            for(const auto& spill:spills)if(spill.displacement==displacement && spill.width==loadWidth) {
                for(int reg=0;reg<16;++reg)if(reg!=4 && values[reg]==spill.value){source=reg;break;}
                break;
            }
            if(source>=0) {
                ++stackForwards;
                const int dst=effect.dst;
                instruction.dst=dst;instruction.src=source;
                if(source==dst){instruction.keep=false;++removed;continue;}
                instruction.replacement={uint8_t(0x48|(source>=8?4:0)|(dst>=8?1:0)),0x89,
                    uint8_t(0xc0|((source&7)<<3)|(dst&7))};
                uint8_t encoded[16]{};std::memcpy(encoded,instruction.replacement.data(),3);
                hde64_disasm(encoded,&instruction.finalEncoding);effect=registerEffect(instruction.finalEncoding);
            }
        }
        const bool fullCopy=effect.value==RegisterEffect::Copy && effect.mask==UINT64_MAX && effect.dst!=4;
        if(fullCopy && values[effect.dst]==values[effect.src]) {
            instruction.keep=false;++removed;++equivalentCopies;continue;
        }
        const uint64_t copied=fullCopy?values[effect.src]:0;
        for(unsigned reg=0;reg<16;++reg)if(effect.writes&(uint32_t(1)<<reg))values[reg]=++nextValue;
        if(fullCopy)values[effect.dst]=copied;
        if(stackLoad && effect.dst!=4) {
            // A full-register zero-extended load is represented by its exact
            // source width. No wider/narrower overlapping access is substituted.
            const bool written=loadWidth==8 &&
                std::find(writableSlots.begin(),writableSlots.end(),displacement)!=writableSlots.end();
            bool recorded=false;
            for(auto& spill:spills)if(spill.displacement==displacement && spill.width==loadWidth) {
                spill.value=values[effect.dst];spill.written=written;recorded=true;break;
            }
            if(!recorded)spills.push_back({displacement,loadWidth,values[effect.dst],written});
        }
    }
    unsigned deadConstants=0,deadCopies=0;
    for(size_t i=0;i<instructions.size();++i) {
        auto& current=instructions[i];const auto& effect=current.registers;
        const bool copy=current.dst>=0 && current.src>=0;
        if(!current.keep || !effect.known || (!effect.constant && !copy) || effect.dst<0 || effect.dst==4)continue;
        const uint32_t bit=uint32_t(1)<<effect.dst;
        for(size_t j=i+1;j<instructions.size() && j<=i+64;++j) {
            const auto& next=instructions[j];
            if(targets.count(next.at))break;
            if(!next.keep)continue;
            if(!next.registers.known || (next.registers.reads&bit))break;
            if(next.registers.fullWrites&bit){current.keep=false;++removed;if(copy)++deadCopies;else ++deadConstants;break;}
        }
    }
    unsigned redundantMasks=0;
    std::array<uint64_t,16> possibleBits;possibleBits.fill(UINT64_MAX);
    for(size_t i=0;i<instructions.size();++i) {
        auto& instruction=instructions[i];
        if(targets.count(instruction.at))possibleBits.fill(UINT64_MAX);
        if(!instruction.keep)continue;
        const auto& effect=instruction.registers;
        if(!effect.known){possibleBits.fill(UINT64_MAX);continue;}
        // A redundant data mask can still change flags. Original masks need
        // the same full flag-overwrite proof as the masks introduced by fusion.
        if(effect.value==RegisterEffect::AndImmediate && effect.dst>=0 &&
           (possibleBits[size_t(effect.dst)]&~effect.mask)==0 && flagsAreOverwritten(i)) {
            instruction.keep=false;++removed;++redundantMasks;continue;
        }
        uint64_t value=effect.mask;
        if(effect.dst>=0) {
            switch(effect.value) {
            case RegisterEffect::Copy:value=possibleBits[size_t(effect.src)]&effect.mask;break;
            case RegisterEffect::AndImmediate:value=possibleBits[size_t(effect.dst)]&effect.mask;break;
            case RegisterEffect::AndRegisters:value=possibleBits[size_t(effect.dst)]&possibleBits[size_t(effect.src)]&effect.mask;break;
            case RegisterEffect::OrRegisters:value=(possibleBits[size_t(effect.dst)]|possibleBits[size_t(effect.src)])&effect.mask;break;
            case RegisterEffect::ByteWrite:value=(possibleBits[size_t(effect.dst)]&~effect.byteMask)|effect.mask;break;
            case RegisterEffect::ShiftLeft:value=(possibleBits[size_t(effect.dst)]<<effect.count)&effect.mask;break;
            case RegisterEffect::ShiftRight:value=(possibleBits[size_t(effect.dst)]&effect.mask)>>effect.count;break;
            case RegisterEffect::ShiftArithmetic: {
                const uint64_t input=possibleBits[size_t(effect.dst)]&effect.mask;
                value=input>>effect.count;
                if(input&(effect.mask==UINT64_MAX?(uint64_t(1)<<63):(uint64_t(1)<<31)))
                    value|=effect.mask^(effect.mask>>effect.count);
                break;
            }
            case RegisterEffect::SignExtend: {
                const uint64_t input=(effect.src>=0?possibleBits[size_t(effect.src)]:UINT64_MAX)>>effect.sourceShift;
                const uint64_t lowMask=(uint64_t(1)<<effect.count)-1;
                value=input&lowMask;
                if(input&(uint64_t(1)<<(effect.count-1)))value|=~lowMask;
                value&=effect.mask;break;
            }
            case RegisterEffect::Unknown:value=UINT64_MAX;break;
            case RegisterEffect::Fixed:break;
            }
        }
        for(unsigned reg=0;reg<16;++reg)if(effect.writes&(uint32_t(1)<<reg))possibleBits[reg]=UINT64_MAX;
        if(effect.dst>=0)possibleBits[size_t(effect.dst)]=value;
    }
    // Bit demand follows both successors of conditional branches, including
    // backward edges. Unknown instructions conservatively observe every bit.
    // Only complete fixed-point results may remove instructions; large blocks
    // and slow-converging graphs retain all existing optimizations unchanged.
    unsigned unusedExtensions=0,unusedMasks=0,flowCopies=0;
    if(instructions.size()<=32768) {
        using Bits=std::array<uint64_t,16>;
        struct Flow {size_t next,target;bool external=false,branch=false;};
        const size_t end=instructions.size();
        std::vector<Flow> flows(end);
        std::vector<Bits> live(end);
        for(size_t i=0;i<end;++i) {
            auto& flow=flows[i];flow.next=i+1;flow.target=end;flow.external=i+1==end;
            const auto& instruction=instructions[i];const auto& h=instruction.finalEncoding;
            if(!instruction.keep || !instruction.relative ||
               (h.flags&(F_PREFIX_REP|F_PREFIX_66|F_PREFIX_67|F_PREFIX_LOCK|F_PREFIX_SEG)))continue;
            const bool jump=h.opcode==0xe9 || h.opcode==0xeb;
            const bool conditional=(h.opcode>=0x70 && h.opcode<=0x7f) ||
                (h.opcode==0x0f && h.opcode2>=0x80 && h.opcode2<=0x8f);
            if(!jump && !conditional)continue;
            const auto target=std::lower_bound(instructions.begin(),instructions.end(),size_t(instruction.target),
                [](const Instruction& item,size_t at){return item.at<at;});
            flow.branch=true;
            if(jump){flow.next=size_t(target-instructions.begin());flow.external=false;}
            else flow.target=size_t(target-instructions.begin());
        }
        const auto after=[&](size_t i) {
            Bits out{};const auto& flow=flows[i];
            if(flow.external)out.fill(UINT64_MAX);
            if(flow.next<end)for(unsigned reg=0;reg<16;++reg)out[reg]|=live[flow.next][reg];
            if(flow.target<end)for(unsigned reg=0;reg<16;++reg)out[reg]|=live[flow.target][reg];
            return out;
        };
        bool converged=false;
        for(unsigned iteration=0;iteration<64;++iteration) {
            bool changed=false;
            for(size_t i=end;i-- >0;) {
                const auto& instruction=instructions[i];const auto& effect=instruction.registers;
                const auto& h=instruction.finalEncoding;
                const auto out=after(i);Bits in=out;
                if(instruction.keep && !flows[i].branch) {
                    if(!effect.known)in.fill(UINT64_MAX);
                    else {
                        Bits reads{};
                        for(unsigned reg=0;reg<16;++reg)if(effect.reads&(uint32_t(1)<<reg))reads[reg]=UINT64_MAX;
                        if(effect.value==RegisterEffect::Copy && effect.src>=0)
                            reads[size_t(effect.src)]=out[size_t(effect.dst)]&effect.mask;
                        if(effect.value==RegisterEffect::AndImmediate)
                            reads[size_t(effect.dst)]=effect.mask;
                        if(effect.value==RegisterEffect::SignExtend && effect.src>=0)
                            reads[size_t(effect.src)]=((uint64_t(1)<<effect.count)-1)<<effect.sourceShift;
                        // A byte comparison/test or memory store consumes only its actual byte;
                        // memory base/index registers still consume all bits.
                        const bool groupByte=h.opcode==0x80 && h.modrm_reg==7;
                        const bool byteStore=h.opcode==0x88 && h.modrm_mod!=3;
                        if(h.opcode==0x38 || h.opcode==0x3a || h.opcode==0x3c || h.opcode==0x84 || h.opcode==0xa8 || groupByte || byteStore) {
                            reads.fill(0);
                            const auto byte=[&](int encoded) {
                                const bool high=!(h.flags&F_PREFIX_REX) && encoded>=4 && encoded<8;
                                reads[size_t(high?encoded-4:encoded)]|=uint64_t(0xff)<<(high?8:0);
                            };
                            if(h.opcode==0x3c || h.opcode==0xa8)reads[0]=0xff;
                            else {
                                if(!groupByte)byte(h.modrm_reg+8*h.rex_r);
                                if(h.modrm_mod==3)byte(h.modrm_rm+8*h.rex_b);
                                else if(h.flags&F_SIB) {
                                    if(!(h.modrm_mod==0 && h.sib_base==5))reads[h.sib_base+8*h.rex_b]=UINT64_MAX;
                                    if(h.sib_index!=4 || h.rex_x)reads[h.sib_index+8*h.rex_x]=UINT64_MAX;
                                } else if(!(h.modrm_mod==0 && h.modrm_rm==5))reads[h.modrm_rm+8*h.rex_b]=UINT64_MAX;
                            }
                        }
                        for(unsigned reg=0;reg<16;++reg) {
                            if(effect.fullWrites&(uint32_t(1)<<reg))in[reg]=0;
                            else if(effect.writes&(uint32_t(1)<<reg))in[reg]&=~effect.byteMask;
                            in[reg]|=reads[reg];
                        }
                    }
                }
                if(in!=live[i]){live[i]=in;changed=true;}
            }
            if(!changed){converged=true;break;}
        }
        if(converged)for(size_t i=0;i<end;++i) {
            auto& instruction=instructions[i];const auto& effect=instruction.registers;
            if(!instruction.keep || !effect.known || effect.dst<0 || effect.dst==4)continue;
            const uint64_t needed=after(i)[size_t(effect.dst)];
            if(effect.value==RegisterEffect::Copy && effect.mask==UINT64_MAX && needed==0) {
                instruction.keep=false;++removed;++flowCopies;
            } else if(effect.value==RegisterEffect::SignExtend && effect.dst==effect.src && effect.sourceShift==0 &&
               (needed&~((uint64_t(1)<<effect.count)-1))==0) {
                instruction.keep=false;++removed;++unusedExtensions;
            } else if(effect.value==RegisterEffect::AndImmediate && (needed&~effect.mask)==0 && flagsAreOverwritten(i)) {
                instruction.keep=false;++removed;++unusedMasks;
            }
        }
    }
    if(!removed && !specialized && !stackForwards)return {};
    std::vector<size_t> oldOffsets,newOffsets;
    size_t newSize=0;
    for(auto& instruction:instructions) {
        instruction.newAt=newSize;
        oldOffsets.push_back(instruction.at);newOffsets.push_back(newSize);
        if(instruction.keep)newSize+=instruction.replacement.empty()?instruction.length+(instruction.immediate?1:0):instruction.replacement.size();
    }
    // All targets were verified instruction starts. Deleted no-ops map to the
    // following instruction, so incoming branches retain their semantics.
    const auto relocate=[&](size_t old)->size_t {
        size_t lo=0,hi=oldOffsets.size();
        while(lo<hi){const size_t mid=lo+(hi-lo)/2;if(oldOffsets[mid]<old)lo=mid+1;else hi=mid;}
        return lo<newOffsets.size()?newOffsets[lo]:newSize;
    };
    Result result;result.code.reserve(newSize);result.entry=int64_t(relocate(size_t(entry)));result.removed=removed;result.specialized=specialized;result.masked=masked;result.deadConstants=deadConstants;result.deadCopies=deadCopies;result.redundantMasks=redundantMasks;result.fallthroughJumps=fallthroughJumps;
    result.stackForwards=stackForwards;result.equivalentCopies=equivalentCopies;
    result.signedExtensions=signedExtensions;
    result.unusedExtensions=unusedExtensions;result.unusedMasks=unusedMasks;
    result.flowCopies=flowCopies;
    result.redundantStores=redundantStores;
    for(const auto& instruction:instructions) {
        if(!instruction.keep)continue;
        if(!instruction.replacement.empty())result.code.insert(result.code.end(),instruction.replacement.begin(),instruction.replacement.end());
        else if(instruction.immediate) {
            result.code.push_back(bytes[instruction.at]);result.code.push_back(0xc1);
            result.code.push_back(bytes[instruction.at+2]);result.code.push_back(instruction.count);
        } else result.code.insert(result.code.end(),bytes+instruction.at,bytes+instruction.at+instruction.length);
        if(instruction.relative) {
            const int64_t delta=int64_t(relocate(size_t(instruction.target)))-int64_t(instruction.newAt+instruction.length);
            const size_t at=instruction.newAt+instruction.length-instruction.relative;
            if(instruction.relative==1) {
                if(delta<std::numeric_limits<int8_t>::min() || delta>std::numeric_limits<int8_t>::max())return {};
                result.code[at]=uint8_t(int8_t(delta));
            } else {
                if(delta<std::numeric_limits<int32_t>::min() || delta>std::numeric_limits<int32_t>::max())return {};
                const int32_t value=int32_t(delta);std::memcpy(result.code.data()+at,&value,4);
            }
        }
    }
    if(uint64_t(result.entry)>=result.code.size())return {};
    return result;
}
}
