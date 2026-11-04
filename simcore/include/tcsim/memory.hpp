#pragma once
/* Game-compatible RAM storage and Load/Store port behavior.

   The game exposes memory through components placed in a RAM's socket.  The
   RAM itself has no ordinary pins; a Load port exposes Enable/Address/Output,
   a Store port exposes Enable/Address/Store value, and the remaining 8-bit
   pins are the compiler's private ordering chain.  This file models the byte
   store and the observable part of those ports.  board.hpp builds the private
   chain from the measured top-to-bottom placement order. */

#include "netlist.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace tcsim {

struct MemoryImage {
    uint64_t component_id = 0;
    uint8_t address_bits = 8;
    bool little_endian = false;
    std::unordered_map<uint64_t, uint8_t> bytes;

    uint64_t capacity() const {
        return address_bits >= 63 ? 0 : (uint64_t{1} << address_bits);
    }
    uint64_t maskAddress(uint64_t address) const {
        const uint64_t size = capacity();
        return size ? address & (size - 1) : address;
    }
    uint8_t readByte(uint64_t address) const {
        const uint64_t size = capacity();
        if (size && address >= size) return 0; /* the game keeps a zero guard past RAM */
        const auto found = bytes.find(address);
        return found == bytes.end() ? 0 : found->second;
    }
    void writeByte(uint64_t address, uint8_t value) {
        const uint64_t size = capacity();
        if (size && address >= size) return;
        if (value) bytes[address] = value;
        else bytes.erase(address);
    }
    uint64_t read(uint64_t address, uint16_t bits) const {
        const uint16_t width = bits > 64 ? 64 : bits;
        const uint16_t count = static_cast<uint16_t>((width + 7u) / 8u);
        const uint64_t base = maskAddress(address);
        uint64_t value = 0;
        if (little_endian) {
            for (uint16_t index = 0; index < count; ++index)
                value |= static_cast<uint64_t>(readByte(base + index)) << (index * 8u);
        } else {
            for (uint16_t index = 0; index < count; ++index)
                value = (value << 8u) | readByte(base + index);
        }
        if (width < 64) value &= (uint64_t{1} << width) - 1u;
        return value;
    }
    void write(uint64_t address, uint16_t bits, uint64_t value) {
        const uint16_t width = bits > 64 ? 64 : bits;
        const uint16_t count = static_cast<uint16_t>((width + 7u) / 8u);
        const uint64_t base = maskAddress(address);
        for (uint16_t index = 0; index < count; ++index) {
            const uint16_t shift = little_endian ? static_cast<uint16_t>(index * 8u)
                                                 : static_cast<uint16_t>((count - 1u - index) * 8u);
            writeByte(base + index, static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }
};

inline BitVector memoryVector(uint64_t value, uint16_t bits) {
    const uint16_t width = bits ? bits : 1;
    BitVector out(width, Logic::kZero);
    for (uint16_t bit = 0; bit < width; ++bit) {
        const uint16_t from_right = static_cast<uint16_t>(width - 1u - bit);
        out.at(bit) = from_right < 64 && ((value >> from_right) & 1u) ? Logic::kOne : Logic::kZero;
    }
    return out;
}

inline Logic memoryEnable(const BitVector& value) {
    if (value.empty()) return Logic::kX;
    return value[0];
}

/* Each returned spec belongs to one port.  The captured serial is the private
   ordering token: any evaluation changes it, so the next port evaluates after
   this one even when the visible address/data happen not to change. */
inline std::unique_ptr<DeviceSpec> memoryLoadPortSpec(const std::shared_ptr<MemoryImage>& memory,
                                                      uint16_t access_bits, ArcDelay delay = ArcDelay{}) {
    const uint16_t width = access_bits ? access_bits : 8;
    auto serial = std::make_shared<uint64_t>(0);
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "load_port_" + std::to_string(width);
    spec->inputs = {PinSpec{"Enable", 1}, PinSpec{"Address", 32}, PinSpec{"Order", 8}};
    spec->outputs = {PinSpec{"Output", width}, PinSpec{"Order", 8}};
    spec->default_delay = delay;
    spec->behavior = [memory, width, serial](const EvalContext&, const std::vector<BitVector>& inputs,
                                             std::vector<BitVector>& outputs) {
        outputs.assign(2, BitVector{});
        if (inputs.size() < 3 || !inputs[2].isKnown()) {
            outputs[0] = BitVector(width, Logic::kX);
            outputs[1] = BitVector(8, Logic::kX);
            return;
        }
        const Logic enable = inputs.size() > 0 ? memoryEnable(inputs[0]) : Logic::kX;
        if (enable == Logic::kZero) outputs[0] = BitVector(width, Logic::kZ);
        else if (enable != Logic::kOne || inputs.size() < 2 || !inputs[1].isKnown())
            outputs[0] = BitVector(width, Logic::kX);
        else
            outputs[0] = memoryVector(memory->read(inputs[1].toUint64(), width), width);
        outputs[1] = memoryVector(++*serial, 8);
    };
    return spec;
}

inline std::unique_ptr<DeviceSpec> memoryStorePortSpec(const std::shared_ptr<MemoryImage>& memory,
                                                       uint16_t access_bits, ArcDelay delay = ArcDelay{}) {
    const uint16_t width = access_bits ? access_bits : 8;
    auto serial = std::make_shared<uint64_t>(0);
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "store_port_" + std::to_string(width);
    spec->inputs = {PinSpec{"Enable", 1}, PinSpec{"Address", 32}, PinSpec{"Value", width},
                    PinSpec{"Order", 8}};
    spec->outputs = {PinSpec{"Order", 8}};
    spec->default_delay = delay;
    spec->behavior = [memory, width, serial](const EvalContext&, const std::vector<BitVector>& inputs,
                                             std::vector<BitVector>& outputs) {
        if (inputs.size() < 4 || !inputs[3].isKnown()) {
            outputs.assign(1, BitVector(8, Logic::kX));
            return;
        }
        if (memoryEnable(inputs[0]) == Logic::kOne && inputs[1].isKnown() &&
            inputs[2].isKnown()) {
            memory->write(inputs[1].toUint64(), width, inputs[2].toUint64());
        }
        outputs.assign(1, memoryVector(++*serial, 8));
    };
    return spec;
}

}  // namespace tcsim
