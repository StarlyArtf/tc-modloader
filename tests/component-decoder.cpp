/* Offline checks for the M4 example: the five-input (one-of-thirty-two) decoder
   built from the game's own parts (examples/composite-decoder/decoder.hpp).

   **Work in progress, and both halves are open.**  Two things came out of the
   2026-09-29 measurement (docs/HANDOFF-component-m4.md section 5.1):

     * the layout refusal this case used to print is the *builder's* rule being
       stricter than the game: a wire here has two endpoints, an endpoint placed
       on a wire splits it into two wires that share that endpoint (one net), and
       two wires that merely *cross* - with no endpoint at the crossing - are two
       nets.  With that rule relaxed the generated layout validates and encodes;
     * the truth table then runs for the first time and fails: 24 of the 32 input
       patterns light the wrong line.  `validate()` used to fail first, which is
       why the generator's line order (its newest bit lands in the line number's
       least significant position while the output pins run in `laneY` order) and
       the decoders' line polarity were never exercised.

   The case pins three things (the second and third are the open ones):

     * **the truth table** - all thirty-two input patterns have to light exactly
       their own output and nothing else, evaluated straight from the netlist
       (the model of the two parts is in the header, and the real-machine gate is
       what confirms the parts' polarity);
     * **the layout routes** - the builder has to accept the generated geometry
       and encode it into a circuit file.  It does today only because the
       clearance rule refuses crossings; the rule itself is what has to be
       measured and rewritten (step 1 and 2 of the handoff's table), so this case
       is the natural regression for it;
     * **the shipped clearance rule and this case agree** - a layout the rule
       refuses today prints the refusal, and the case prints it.

   It also checks the shape the game will be asked about: five inputs, thirty-two
   outputs, and a two-input AND count of 4 + 8 + 16 + 32 = 60. */
#include "../examples/composite-decoder/decoder.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
}

}  // namespace

int main() {
    using namespace composite_decoder;
    constexpr uint64_t kDecoderId = 0x44454335335F3031ull; /* "DEC53_01" */
    Definition decoder;
    const bool built = build(decoder, kDecoderId, "Decoder 5 to 32");
    check(built, "the generator produced a netlist");
    if (!built) return 1;

    check(decoder.inputCount() == 5, "five inputs");
    check(decoder.outputCount() == 32, "thirty-two outputs");
    check(decoder.nodes.size() == 65, "65 parts: five decoders and sixty ANDs");
    check(gateCount(decoder) == 60, "sixty two-input ANDs");

    std::string why;
    const tc::component_netlist::Error verdict = tc::component_netlist::validate(decoder, why);
    check(verdict == tc::component_netlist::Error::None,
          std::string("the generated layout routes: ") + tc::component_netlist::errorText(verdict) +
              (why.empty() ? std::string() : " (" + why + ")"));
    if (verdict != tc::component_netlist::Error::None) {
        std::printf("component decoder: %d check(s) failed\n", failures);
        return 1;
    }

    const std::vector<uint8_t> bytes = tc::component_netlist::encode(decoder);
    check(!bytes.empty(), "the netlist encodes into a circuit file");

    /* The truth table, straight from the netlist. */
    std::vector<uint8_t> outputs;
    for (uint32_t input = 0; input < 32; ++input) {
        check(evaluate(decoder, input, outputs), "the evaluator ran");
        if (outputs.size() != 32) break;
        bool hot = true;
        for (uint32_t line = 0; line < 32; ++line) {
            const uint8_t expected = line == input ? 1u : 0u;
            if (outputs[line] != expected) hot = false;
        }
        if (!hot) {
            ++failures;
            std::string table;
            for (uint32_t line = 0; line < 32; ++line) table += outputs[line] ? '1' : '0';
            std::printf("FAIL input %u gave %s\n", input, table.c_str());
        }
    }

    if (failures) {
        std::printf("component decoder: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS component decoder: 5 -> 32 one-hot for all thirty-two patterns, "
                "and the layout routes and encodes\n");
    return 0;
}
