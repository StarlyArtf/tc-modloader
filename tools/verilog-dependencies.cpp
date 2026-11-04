/* Print the exact per-output input dependency map of a TC-Verilog module.

   The bridge turns each entry into one private combinational cone inside the
   generated component, so this table is exactly what the game's own cycle
   check sees around the part.  A false "combinational loop" - an output that
   only reads `instr` being reported as depending on every input, including a
   register-file read - shows up here as a mask that names an input the
   expression never reads.

   Build (the SDK is header + one translation unit, no game needed):

     g++ -std=c++17 -O2 -Isdk tools/verilog-dependencies.cpp sdk/tc_verilog.cpp \
         -o build/verilog-dependencies.exe

   Run:

     build/verilog-dependencies.exe tests/fixtures/rv32i_single_cycle.v
*/
#include "tc_verilog.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: verilog-dependencies <file.v>\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "cannot read " << argv[1] << '\n';
        return 2;
    }
    std::string source((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    tc::verilog::Program program;
    std::vector<tc::verilog::Diagnostic> diagnostics;
    if (!program.compile(source, &diagnostics)) {
        for (const auto& diagnostic : diagnostics)
            std::cerr << argv[1] << ':' << diagnostic.line << ':' << diagnostic.column << ' '
                      << diagnostic.message << '\n';
        return 1;
    }

    std::vector<std::string> inputs, outputs;
    for (const tc::verilog::Port& port : program.ports())
        (port.direction == tc::verilog::PortDirection::Input ? inputs : outputs)
            .push_back(port.name + "[" + std::to_string(port.bits) + "]");
    const std::vector<uint32_t>& masks = program.outputDependencies();

    std::cout << "module " << program.moduleName() << "\n";
    std::cout << "inputs " << inputs.size() << ", outputs " << outputs.size()
              << ", state_words " << program.stateWords()
              << ", clocked " << (program.hasClockedState() ? "yes" : "no") << "\n";
    for (size_t output = 0; output < outputs.size(); ++output) {
        const uint32_t mask = output < masks.size() ? masks[output] : 0;
        std::cout << "  " << outputs[output] << " <-";
        if (!mask) {
            std::cout << " (no input; constant or state only)";
        } else {
            for (size_t bit = 0; bit < inputs.size(); ++bit)
                if (bit < 32 && (mask & (uint32_t(1) << bit)))
                    std::cout << ' ' << inputs[bit];
        }
        std::cout << "  mask=0x" << std::hex << mask << std::dec << "\n";
    }
    /* Clocked modules publish the same visible-output table.  Their state
       transition runs on a separate all-input sink with no public output, so
       it cannot add a combinational edge to this list. */
    if (program.hasClockedState())
        std::cout << "note: an edge-triggered block is present; the host adds a hidden "
                     "all-input tick sink while keeping these visible-output masks\n";
    return 0;
}
