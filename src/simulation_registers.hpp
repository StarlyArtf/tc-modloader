#pragma once
#include <cstdint>
#include "../vendor/minhook/src/hde/hde64.h"
namespace tc::simulation_jit {
// Conservative physical-register effects. Unknown encodings are barriers;
// byte/word partial writes never count as overwriting a full register.
struct RegisterEffect {
    enum Value {Unknown,Fixed,Copy,AndImmediate,AndRegisters,OrRegisters,ByteWrite,ShiftLeft,ShiftRight,ShiftArithmetic,SignExtend};
    bool known=false,constant=false;
    uint32_t reads=0,writes=0,fullWrites=0;
    int dst=-1,src=-1;
    uint64_t mask=UINT64_MAX,byteMask=0;
    Value value=Unknown;
    unsigned count=0,sourceShift=0;
};
inline RegisterEffect registerEffect(const hde64s& h) {
    RegisterEffect e;
    if(h.flags&(F_ERROR|F_RELATIVE|F_PREFIX_REP|F_PREFIX_66|F_PREFIX_67|F_PREFIX_LOCK|F_PREFIX_SEG))return e;
    const auto bit=[](int reg)->uint32_t{return uint32_t(1)<<reg;};
    const auto byteReg=[&](int reg)->int{return !(h.flags&F_PREFIX_REX) && reg>=4 && reg<8?reg-4:reg;};
    const auto byteShift=[&](int reg)->unsigned{return !(h.flags&F_PREFIX_REX) && reg>=4 && reg<8?8:0;};
    const auto address=[&]()->uint32_t {
        if(h.modrm_mod==3)return 0;
        uint32_t reads=0;
        if(h.flags&F_SIB) {
            if(!(h.modrm_mod==0 && h.sib_base==5))reads|=bit(h.sib_base+8*h.rex_b);
            if(h.sib_index!=4 || h.rex_x)reads|=bit(h.sib_index+8*h.rex_x);
        } else if(!(h.modrm_mod==0 && h.modrm_rm==5))reads|=bit(h.modrm_rm+8*h.rex_b);
        return reads;
    };
    const int reg=h.modrm_reg+8*h.rex_r,rm=h.modrm_rm+8*h.rex_b;
    const uint64_t width=h.rex_w?UINT64_MAX:UINT32_MAX;
    const auto write=[&](int dst,bool byte=false) {
        e.dst=byte?byteReg(dst):dst;e.writes|=bit(e.dst);
        if(byte){e.reads|=bit(e.dst);e.byteMask=uint64_t(0xff)<<byteShift(dst);}
        else e.fullWrites|=bit(dst);
    };
    if(h.opcode>=0xb8 && h.opcode<=0xbf) {
        write((h.opcode&7)+8*h.rex_b);e.constant=true;e.value=RegisterEffect::Fixed;
        e.mask=h.rex_w?h.imm.imm64:h.imm.imm32;e.known=true;return e;
    }
    if(h.opcode>=0xb0 && h.opcode<=0xb7) {
        const int dst=(h.opcode&7)+8*h.rex_b;write(dst,true);
        e.mask=uint64_t(h.imm.imm8)<<byteShift(dst);e.value=RegisterEffect::ByteWrite;e.known=true;return e;
    }
    if(h.opcode>=0x88 && h.opcode<=0x8b) {
        const bool byte=(h.opcode&1)==0,load=(h.opcode&2)!=0;
        e.reads=address();
        if(load) {
            write(reg,byte);
            if(h.modrm_mod==3)e.reads|=bit(byte?byteReg(rm):rm);
            if(!byte){e.src=h.modrm_mod==3?rm:-1;e.mask=width;e.value=e.src>=0?RegisterEffect::Copy:RegisterEffect::Fixed;}
        } else {
            e.reads|=bit(byte?byteReg(reg):reg);
            if(h.modrm_mod==3) {
                write(rm,byte);
                if(!byte){e.src=reg;e.mask=width;e.value=RegisterEffect::Copy;}
            }
        }
        e.known=true;return e;
    }
    if(h.opcode==0x0f && (h.opcode2==0xb6 || h.opcode2==0xb7)) {
        e.reads=address();write(reg);e.mask=h.opcode2==0xb6?0xff:0xffff;
        if(h.modrm_mod==3) {
            e.src=h.opcode2==0xb6?byteReg(rm):rm;e.reads|=bit(e.src);
            // A high byte is shifted before extension; do not propagate its
            // low-byte mask as though AH/CH/DH/BH were AL/CL/DL/BL.
            if(h.opcode2==0xb6 && byteShift(rm)!=0)e.src=-1;
        }
        e.value=e.src>=0?RegisterEffect::Copy:RegisterEffect::Fixed;e.known=true;return e;
    }
    if(h.opcode==0x8d && h.modrm_mod!=3) {
        e.reads=address();write(reg);e.mask=width;e.value=RegisterEffect::Fixed;e.known=true;return e;
    }
    if((h.opcode==0x0f && (h.opcode2==0xbe || h.opcode2==0xbf)) || (h.opcode==0x63 && h.rex_w)) {
        e.reads=address();write(reg);e.mask=width;
        e.count=h.opcode==0x63?32:(h.opcode2==0xbe?8:16);
        if(h.modrm_mod==3) {
            e.src=e.count==8?byteReg(rm):rm;e.reads|=bit(e.src);
            if(e.count==8)e.sourceShift=byteShift(rm);
        }
        e.value=RegisterEffect::SignExtend;e.known=true;return e;
    }
    if(h.opcode==0xc7 && h.modrm_reg==0) {
        e.reads=address();
        if(h.modrm_mod==3){write(rm);e.constant=true;e.mask=h.rex_w?uint64_t(int64_t(int32_t(h.imm.imm32))):h.imm.imm32;e.value=RegisterEffect::Fixed;}
        e.known=true;return e;
    }
    if(h.opcode==0x0f && h.opcode2>=0x90 && h.opcode2<=0x9f) {
        e.reads=address();
        if(h.modrm_mod==3){write(rm,true);e.mask=uint64_t(1)<<byteShift(rm);e.value=RegisterEffect::ByteWrite;}
        e.known=true;return e;
    }
    if(h.opcode==0x84 || h.opcode==0x85) {
        const bool byte=h.opcode==0x84;e.reads=address()|bit(byte?byteReg(reg):reg);
        if(h.modrm_mod==3)e.reads|=bit(byte?byteReg(rm):rm);
        e.known=true;return e;
    }
    if(h.opcode==0xa8 || h.opcode==0xa9){e.reads=bit(0);e.known=true;return e;}
    if(h.opcode==0xc1 || h.opcode==0xd1 || h.opcode==0xd3) {
        if(h.modrm_reg!=4 && h.modrm_reg!=5 && h.modrm_reg!=7)return e;
        e.reads=address();if(h.opcode==0xd3)e.reads|=bit(1);
        if(h.modrm_mod==3){
            write(rm);e.reads|=bit(rm);e.mask=width;
            if(h.opcode!=0xd3) {
                e.count=(h.opcode==0xd1?1:h.imm.imm8)&(h.rex_w?63:31);
                if(e.count)e.value=h.modrm_reg==4?RegisterEffect::ShiftLeft:
                    h.modrm_reg==5?RegisterEffect::ShiftRight:RegisterEffect::ShiftArithmetic;
            }
        }
        e.known=true;return e;
    }
    const bool group=h.opcode==0x80 || h.opcode==0x81 || h.opcode==0x83;
    const bool arithmetic=h.opcode<=0x3d && (h.opcode&7)<=5;
    if(group || arithmetic) {
        const unsigned operation=group?h.modrm_reg:h.opcode>>3;
        const bool byte=group?h.opcode==0x80:(h.opcode&1)==0;
        const bool compare=operation==7,accumulator=!group && (h.opcode&7)>=4;
        bool immediate=group || accumulator;
        e.reads=accumulator?0:address();
        int dst=-1,src=-1;
        if(accumulator)dst=0;
        else if(group){if(h.modrm_mod==3)dst=rm;}
        else if(h.opcode&2){dst=reg;if(h.modrm_mod==3)src=rm;}
        else {src=reg;if(h.modrm_mod==3)dst=rm;}
        if(dst>=0)e.reads|=bit(byte?byteReg(dst):dst);
        if(src>=0)e.reads|=bit(byte?byteReg(src):src);
        if(!compare && dst>=0) {
            write(dst,byte);e.mask=width;
            if(!byte && immediate && operation==4) {
                e.mask=h.opcode==0x83?uint64_t(int64_t(int8_t(h.imm.imm8))):uint64_t(int64_t(int32_t(h.imm.imm32)));
                e.mask&=width;e.value=RegisterEffect::AndImmediate;
            } else if(!byte && src>=0 && (operation==1 || operation==4 || operation==6)) {
                e.src=src;e.value=operation==4?RegisterEffect::AndRegisters:RegisterEffect::OrRegisters;
                if(operation==6 && src==dst){e.reads=0;e.mask=0;e.value=RegisterEffect::Fixed;}
            } else if(!byte)e.value=RegisterEffect::Fixed;
        }
        e.known=true;return e;
    }
    if(h.opcode==0x90 && !h.rex_b){e.known=true;return e;}
    return e;
}
}
