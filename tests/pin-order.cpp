/* Offline checks for the pin-order store (src/pin_order.hpp).

   The store is what turns "move this pin" into a permutation of the panel's
   cached IO records.  Only a playtest has the real cache, so this file builds
   one: the same three (count, payload) descriptors at the same context offsets,
   the same payload header and the same 32-byte entries, with names that look
   like the ones a level carries.  Everything a Mod can ask for is exercised
   against it, plus the two rules that are easy to get wrong - a stored order
   survives a rebuild with the same pins, and is dropped when the pins change. */
#include "../src/pin_order.hpp"
#include "../src/pin_order_bounds.hpp"
#include "../examples/pin-order/layout.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    ++failures;
    std::cout << "FAIL " << what << "\n";
}

struct Pin {
    uint64_t key;
    uint64_t width;
    std::string name;
};

/* One fake panel: the context block the panel keeps its caches in, plus the
   payload blocks those caches point at. */
class FakePanel {
public:
    FakePanel() : context_(0xdb00) {
        const std::vector<Pin> inputs{{2, 1, "Carry in"}, {3, 8, "A"}, {4, 8, "B"}};
        const std::vector<Pin> outputs{{1, 8, "Output"}, {5, 1, "Carry out"}};
        inputs_ = makePayload(inputs);
        outputs_ = makePayload(outputs);
        writeDescriptor(tc::io_state_cache::input_offset, inputs_);
        writeDescriptor(tc::io_state_cache::output_offset, outputs_);
        writeDescriptor(tc::io_state_cache::other_offset, empty_);
    }

    void* context() { return context_.data(); }

    /* The panel rebuilding its cache, with the same pins in another order or a
       different set of pins altogether. */
    void rebuildInputs(const std::vector<Pin>& pins) {
        inputs_ = makePayload(pins);
        writeDescriptor(tc::io_state_cache::input_offset, inputs_);
    }

    /* The same, with the block's own word set to the list's capacity. */
    void rebuildInputsReserved(const std::vector<Pin>& pins, uint64_t reserved) {
        inputs_ = makePayload(pins, reserved);
        /* The descriptor carries the number of entries the panel draws; the
           block word is the list's capacity, which is the larger of the two. */
        writeDescriptor(tc::io_state_cache::input_offset, inputs_, pins.size());
    }

    std::vector<uint64_t> keys(uint64_t group) const {
        const auto* descriptor = context_.data() + tc::pin_order::groupOffset(group);
        const auto count = *reinterpret_cast<const uint64_t*>(descriptor);
        const auto* payload = *reinterpret_cast<const uint8_t* const*>(descriptor + 8);
        std::vector<uint64_t> result;
        for (uint64_t index = 0; index < count; ++index)
            result.push_back(*reinterpret_cast<const uint64_t*>(
                payload + tc::pin_order::kEntriesOffset + index * tc::pin_order::kEntryBytes));
        return result;
    }

private:
    /* `reserved` is the block's own first word.  A list the game allocated at its
       final size has `reserved == count`; one that grew one entry at a time
       (the component workshop's, and a board whose input devices were added by
       hand) has a capacity that doubles - 1, 2, 4, 8, 16 - so the two differ
       for every count in between.  Both are the same structure. */
    std::vector<uint8_t> makePayload(const std::vector<Pin>& pins, uint64_t reserved = 0) {
        std::vector<uint8_t> payload(tc::pin_order::kEntriesOffset + pins.size() * 0x20 + 0x40);
        *reinterpret_cast<uint64_t*>(payload.data() + tc::pin_order::kPayloadCountOffset) =
            reserved ? reserved : pins.size();
        for (size_t index = 0; index < pins.size(); ++index) {
            /* The pointer names a block that starts with the string's own
               capacity, exactly like the game's strings: the characters are
               eight bytes in. */
            auto block = std::make_unique<char[]>(tc::pin_order::kNameHeaderBytes +
                                                  pins[index].name.size() + 1);
            std::memset(block.get(), 0, tc::pin_order::kNameHeaderBytes);
            std::memcpy(block.get() + tc::pin_order::kNameHeaderBytes, pins[index].name.c_str(),
                        pins[index].name.size() + 1);
            const char* text = block.get() + tc::pin_order::kNameHeaderBytes;
            uint8_t* record = payload.data() + tc::pin_order::kEntriesOffset +
                              index * tc::pin_order::kEntryBytes;
            *reinterpret_cast<uint64_t*>(record) = pins[index].key;
            *reinterpret_cast<uint64_t*>(record + 8) = pins[index].width;
            *reinterpret_cast<uint64_t*>(record + 16) = pins[index].name.size();
            *reinterpret_cast<const char**>(record + 24) = text - tc::pin_order::kNameHeaderBytes;
            blocks_.push_back(std::move(block));
        }
        return payload;
    }

    void writeDescriptor(std::size_t offset, const std::vector<uint8_t>& payload,
                         uint64_t count = UINT64_MAX) {
        /* The empty group is a (0, nullptr) pair, which is what the panel leaves
           for a group a level does not use. */
        const uint64_t stored = count == UINT64_MAX
                                    ? (payload.empty()
                                           ? 0
                                           : *reinterpret_cast<const uint64_t*>(
                                                 payload.data() +
                                                 tc::pin_order::kPayloadCountOffset))
                                    : count;
        const bool empty = payload.empty() || stored == 0;
        *reinterpret_cast<uint64_t*>(context_.data() + offset) = empty ? 0 : stored;
        *reinterpret_cast<const uint8_t**>(context_.data() + offset + 8) =
            empty ? nullptr : payload.data();
    }

    std::vector<uint8_t> context_;
    std::vector<uint8_t> inputs_;
    std::vector<uint8_t> outputs_;
    std::vector<uint8_t> empty_ = makePayload({});
    /* The blocks the records' name pointers name; they stay put for the life of
       the panel. */
    std::vector<std::unique_ptr<char[]>> blocks_;
};

std::string join(const std::vector<uint64_t>& keys) {
    std::string text;
    for (uint64_t key : keys) {
        if (!text.empty()) text += ",";
        text += std::to_string(key);
    }
    return text;
}

}  // namespace

int main() {
    using tc::pin_order::store;

    FakePanel panel;
    store().apply(panel.context());

    /* What the panel is showing. */
    check(store().count(0) == 3, "inputs are counted");
    check(store().count(1) == 2, "outputs are counted");
    tc::pin_order::Entry entry{};
    check(store().entry(0, 0, &entry) == 0, "the first input is readable");
    check(entry.key == 2 && entry.width == 1 && entry.name == "Carry in",
          "the first input keeps its key, width and name");
    check(store().entry(1, 1, &entry) == 0 && entry.name == "Carry out",
          "the second output keeps its name");
    check(store().entry(0, 3, &entry) != 0, "an entry past the end is refused");

    /* Moving an entry: the panel's own order is what the positions mean. */
    check(store().move(0, 0, 2) == 0, "moving the first input to the last slot works");
    store().apply(panel.context());
    check(join(panel.keys(0)) == "3,4,2", "the cache is permuted: " + join(panel.keys(0)));
    std::vector<uint64_t> order(8);
    uint32_t length = 0;
    check(store().order(0, order.data(), 8, &length) == 0 && length == 3,
          "the order is reported");
    check(join(std::vector<uint64_t>(order.begin(), order.begin() + 3)) == "3,4,2",
          "the reported order is the new one");
    check(join(panel.keys(1)) == "1,5", "the other group is untouched");

    /* Applying twice must not permute twice: the order is expressed in keys. */
    store().apply(panel.context());
    check(join(panel.keys(0)) == "3,4,2", "applying again keeps the same order");

    /* A rebuild with the same pins keeps the order; a different set drops it. */
    panel.rebuildInputs({{2, 1, "Carry in"}, {3, 8, "A"}, {4, 8, "B"}});
    store().apply(panel.context());
    check(join(panel.keys(0)) == "3,4,2", "the order survives a rebuild with the same pins");
    panel.rebuildInputs({{7, 8, "X"}, {8, 8, "Y"}});
    store().apply(panel.context());
    check(join(panel.keys(0)) == "7,8", "a different set of pins keeps the panel's order");
    check(store().move(0, 0, 1) == 0, "moving still works after the set changed");
    store().apply(panel.context());
    check(join(panel.keys(0)) == "8,7", "the new order is applied: " + join(panel.keys(0)));

    /* Reset: the panel's own order comes back. */
    check(store().clear(0) == 0, "reset works");
    check(join(panel.keys(0)) == "7,8", "reset restores the panel's order: " + join(panel.keys(0)));

    /* An order that names a pin the panel is not showing is refused. */
    const uint64_t strangers[] = {99, 98};
    check(store().setOrder(0, strangers, 2) != 0, "an order of unknown pins is refused");
    const uint64_t same[] = {8, 7};
    check(store().setOrder(0, same, 2) == 0, "an order of the panel's own pins is accepted");
    store().apply(panel.context());
    check(join(panel.keys(0)) == "8,7", "the installed order is applied");

    /* A panel that is not there answers nothing rather than guessing. */
    check(store().move(2, 0, 0) != 0, "a group the panel is not showing is refused");
    check(store().count(0) == 2, "the last context is remembered");

    /* Entry layout is a loader-owned union, not a decorator guessing another
       Mod's controls from the next cursor anchor.  It also deliberately keeps
       the previous completed frame because frames are painted one tick late. */
    auto& bounds = tc::pin_order_bounds::store();
    bounds.clear();
    check(bounds.include({40, 0, 7, 20.f, 100.f, 200.f, 160.f}) == 0,
          "first entry bounds accepted");
    check(bounds.include({40, 0, 7, 10.f, 120.f, 230.f, 210.f}) == 0,
          "second producer bounds accepted");
    tc::pin_order_bounds::Bounds united{};
    check(bounds.get(40, 0, 7, &united) == 0 && united.min_x == 10.f &&
              united.min_y == 100.f && united.max_x == 230.f && united.max_y == 210.f,
          "entry controls are unioned");
    check(bounds.include({41, 0, 7, 20.f, 100.f, 200.f, 170.f}) == 0 &&
              bounds.get(40, 0, 7, &united) == 0,
          "the previous completed frame remains queryable");
    check(bounds.include({43, 0, 7, 20.f, 100.f, 200.f, 180.f}) == 0 &&
              bounds.get(40, 0, 7, &united) != 0,
          "stale layout frames are retired");
    check(bounds.include({44, 3, 7, 0.f, 0.f, 1.f, 1.f}) != 0,
          "an unknown layout group is refused");

    /* A one-bit widget is deliberately large.  Its square anchor is the
       top-left corner, so using half of the line spacing as its lower half
       cuts the frame through the button.  The measured item height must win. */
    const float oneBitBottom = tc_pin_order_layout::contentBelowLabel(
        true, 35.f, 44.f, 1, 1, 120.f, 0.29f, 0.62f, 4.f);
    check(oneBitBottom == 83.f,
          "one-bit frame includes the measured 44 px control plus padding");
    const float autoBitBottom = tc_pin_order_layout::contentBelowLabel(
        true, 35.f, 44.f, 0, 1, 120.f, 0.29f, 0.62f, 4.f);
    check(autoBitBottom == oneBitBottom,
          "raw auto width 0 uses the same measured one-bit extent");
    const float eightBitBottom = tc_pin_order_layout::contentBelowLabel(
        true, 35.f, 10.f, 8, 0, 120.f, 0.29f, 0.62f, 4.f);
    check(eightBitBottom == 87.5f,
          "multi-bit value line keeps the larger native estimate");

    /* A list the panel grew one entry at a time: the block's own word is the
       capacity (4 here) while the panel draws three entries.  Requiring the two
       to be equal switched the service off for exactly the counts that were not
       powers of two - three pins, five, six, seven - which is the report this
       case exists for. */
    {
        FakePanel grown;
        /* Three entries in a list whose capacity is four: the panel grew it one
           pin at a time, exactly the shape that used to switch the service off. */
        grown.rebuildInputsReserved({{2, 1, "Carry in"}, {3, 8, "A"}, {4, 8, "B"}}, 4);
        store().apply(grown.context());
        check(store().count(0) == 3, "a list that grew past its previous capacity still reads");
        tc::pin_order::Entry entry{};
        check(store().entry(0, 2, &entry) == 0 && std::string(entry.name) == "B",
              "the third entry of a grown list is the panel's third entry");
        check(store().move(0, 0, 2) == 0, "moving inside a grown list is accepted");
        store().apply(grown.context());
        check(join(grown.keys(0)) == "3,4,2", "the grown list is permuted whole");
        /* A block word smaller than the count is not this structure. */
        grown.rebuildInputsReserved({{2, 1, "Carry in"}, {3, 8, "A"}}, 1);
        store().apply(grown.context());
        check(store().count(0) == 0, "a block word below the count is refused");
    }

    /* Readability is asked of the whole presenter context, which is large
       enough to span more than one VirtualQuery region.  Asking a single
       region to cover the range answers "no" for such an object, and the
       service then looks like "the panel is not on screen" for every group at
       once - no order to read and no handle to draw.  Two pages with the same
       state but different protection are two regions, so this is the shape the
       check has to accept. */
    {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const std::size_t page = system.dwPageSize ? system.dwPageSize : 4096;
        auto* two = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        check(two != nullptr, "the readability fixture allocates two pages");
        if (two) {
            DWORD previous = 0;
            check(VirtualProtect(two + page, page, PAGE_READONLY, &previous) != 0,
                  "the second page is re-protected, so the two are separate regions");
            check(tc::pin_order::readable(two, page * 2),
                  "a range split across two committed regions is readable");
            check(tc::pin_order::readable(two + page, page),
                  "the second region on its own is readable");
            check(!tc::pin_order::readable(two - page, page * 2),
                  "an uncommitted page in front of the range is refused");
            check(!tc::pin_order::readable(two, page * 3),
                  "the uncommitted page behind the range is refused");
            VirtualFree(two, 0, MEM_RELEASE);
        }
    }

    if (failures == 0)
        std::cout << "PASS pin order: cached order, unioned bounds, and measured bit height\n";
    return failures == 0 ? 0 : 1;
}
