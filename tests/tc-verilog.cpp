#include "../sdk/tc_verilog.hpp"

#include <cassert>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using tc::verilog::Diagnostic;
using tc::verilog::Program;

struct FakeTypesHost {
    int result = TC_COMPONENT_TYPES_OK;
    unsigned queries = 0, registrations = 0;
    const TCComponentTypeDefinitionV3* seen = nullptr;

    static int registerV2(void*, const TCComponentTypeDefinitionV2*) { return TC_COMPONENT_TYPES_ERR_UNSUPPORTED; }
    static int registerV3(void* context, const TCComponentTypeDefinitionV3* definition) {
        auto& self = *static_cast<FakeTypesHost*>(context);
        ++self.registrations; self.seen = definition;
        return self.result;
    }
    static int query(void* context, const char* id, uint32_t version, void* out, uint32_t bytes) {
        auto& self = *static_cast<FakeTypesHost*>(context);
        ++self.queries;
        if (!id || std::strcmp(id, TC_SERVICE_COMPONENT_TYPES) != 0 ||
            version != TC_COMPONENT_TYPES_API_VERSION_2 || bytes < sizeof(TCComponentTypesApiV2))
            return TC_SERVICE_ERR_UNAVAILABLE;
        TCComponentTypesApiV2 api{};
        api.size = sizeof(api); api.version = TC_COMPONENT_TYPES_API_VERSION_2;
        api.context = context; api.register_definition = &registerV2; api.register_definition_v3 = &registerV3;
        *static_cast<TCComponentTypesApiV2*>(out) = api;
        return TC_SERVICE_OK;
    }
    TCHost host() {
        TCHost value{}; value.size = sizeof(value); value.api_version = TC_MOD_API_VERSION;
        value.context = this; value.query_service = &query; return value;
    }
};

struct Fixture {
    Program program;
    std::vector<TCLogicValueV3> inputs, outputs;
    std::vector<uint64_t> state;
    TCLogicIOV3 io{};

    explicit Fixture(const char* source) {
        std::vector<Diagnostic> diagnostics;
        if (!program.compile(source, &diagnostics)) {
            for (const auto& d : diagnostics)
                std::cerr << d.line << ':' << d.column << ": " << d.message << '\n';
            assert(false);
        }
        inputs.resize(program.inputCount());
        outputs.resize(program.outputCount());
        state.assign(program.stateWords(), 0);
        io.size = sizeof(io); io.version = TC_LOGIC_IO_V3_VERSION_1;
        io.input_count = static_cast<uint32_t>(inputs.size());
        io.output_count = static_cast<uint32_t>(outputs.size());
        io.inputs = inputs.data(); io.outputs = outputs.data();
        io.state = state.data(); io.state_words = static_cast<uint32_t>(state.size());
        io.user = &program;
    }

    void input(size_t index, uint64_t value, uint64_t drive = ~uint64_t(0)) {
        inputs[index] = {value, drive};
    }
    void run(uint32_t phase = TC_LOGIC_CYCLE) {
        io.phase = phase;
        Program::callback(&io);
    }
};

int main() {
    /* Per-output dependency masks are what let a combinational CPU front end
       feed a built-in register file without the generated wrapper inventing a
       read-data -> read-address loop. */
    {
        Fixture f(R"(
            module dependencies(
                input wire instr,
                input wire rs1_data,
                input wire rs2_data,
                output wire rs1_addr,
                output wire alu_result
            );
            assign rs1_addr = instr;
            assign alu_result = rs1_data ^ rs2_data;
            endmodule
        )");
        assert(!f.program.hasClockedState());
        assert(f.program.outputDependencies().size() == 2);
        assert(f.program.outputDependencies()[0] == 0x1);
        assert(f.program.outputDependencies()[1] == 0x6);

        tc::verilog::Component component;
        tc::verilog::ComponentMetadata metadata; metadata.custom_id = 0x4450454e44454e43ULL;
        std::vector<Diagnostic> diagnostics;
        assert(component.compile(R"(
            module dependencies(
                input wire instr, rs1_data, rs2_data,
                output wire rs1_addr, alu_result
            );
            assign rs1_addr = instr;
            assign alu_result = rs1_data ^ rs2_data;
            endmodule
        )", metadata, &diagnostics));
        const auto* definition=component.definition();
        assert((definition->flags&TC_COMPONENT_TYPE_OUTPUT_DEPENDENCIES)!=0);
        assert((definition->flags&TC_COMPONENT_TYPE_INLINE_EXPRESSIONS)!=0);
        assert((definition->flags&TC_COMPONENT_TYPE_GROUPED_OUTPUTS)==0);
        assert(definition->inline_expression_count==2);
        assert(definition->inline_expressions&&definition->inline_expressions[0]);
        assert(definition->output_dependency_count==2);
        assert(definition->output_dependencies[0]==0x1);
        assert(definition->output_dependencies[1]==0x6);
    }
    /* Inline lowering keeps literals immediate, aliases a full-width wire, and
       lowers a conditional without the old pair of multiplications. */
    {
        Program program;
        std::vector<Diagnostic> diagnostics;
        assert(program.compile(R"(
            module inline_peepholes(
                input wire [7:0] a, b,
                input wire select,
                output wire [7:0] y,
                output wire one
            );
            wire [7:0] alias_a;
            assign alias_a = a;
            assign y = select ? alias_a : b;
            assign one = 1'b1;
            endmodule
        )", &diagnostics));
        const auto& prefixes = program.inlinePrefixes();
        const auto& expressions = program.inlineExpressions();
        assert(prefixes.size() == 2 && expressions.size() == 2);
        assert(prefixes[1].empty() && expressions[1] == "U64 1");
        const std::string lowered = prefixes[0] + expressions[0];
        assert(lowered.find(" * ") == std::string::npos);
        assert(lowered.find("if (") != std::string::npos);
        size_t declarations = 0;
        for (size_t at = prefixes[0].find("var "); at != std::string::npos;
             at = prefixes[0].find("var ", at + 4))
            ++declarations;
        assert(declarations == 4); /* a, b, select, and the selected result */
    }
    /* Different dependency groups are slices of one canonical graph.  The
       wider output can therefore reuse the narrower output's common node when
       the host encounters that declaration first. */
    {
        Program program;
        std::vector<Diagnostic> diagnostics;
        assert(program.compile(R"(
            module cross_group_common(
                input wire [7:0] a, b, c,
                output wire [7:0] ab,
                output wire [7:0] abc
            );
            wire [7:0] common;
            assign common = a ^ b;
            assign ab = common;
            assign abc = common ^ c;
            endmodule
        )", &diagnostics));
        assert(program.outputDependencies()[0] == 0x3);
        assert(program.outputDependencies()[1] == 0x7);
        assert(program.inlineExpressions().size() == 2);
        const std::string& common = program.inlineExpressions()[0];
        assert(common.find("tc_j@T@_g0_") == 0);
        assert(program.inlinePrefixes()[1].find(common) != std::string::npos);
    }
    /* The single-cycle RV32I front end is the case that motivated the map: the
       register addresses are instruction fields, so they must not be reported
       as reading the register-file outputs, while the write-back value does.
       Keeping the real source here means a future parser change that widens a
       mask (and re-creates the false loop) fails in the fast suite. */
    {
        std::ifstream fixture("tests/fixtures/rv32i_single_cycle.v", std::ios::binary);
        assert(fixture);
        std::string source((std::istreambuf_iterator<char>(fixture)),
                           std::istreambuf_iterator<char>());
        Program program;
        std::vector<Diagnostic> diagnostics;
        assert(program.compile(source, &diagnostics));
        assert(!program.hasClockedState());
        assert(program.inputCount() == 5);
        assert(program.outputCount() == 17);
        assert(program.inlineExpressions().empty());  /* signed SRA stays on callback fallback */
        const std::vector<uint32_t>& masks = program.outputDependencies();
        assert(masks.size() == 17);
        assert(masks[0] == 0x1);  /* imem_addr <- pc */
        assert(masks[1] == 0x2);  /* rs1_addr <- instr */
        assert(masks[2] == 0x2);  /* rs2_addr <- instr */
        assert(masks[3] == 0x2);  /* rd_addr <- instr */
        assert(masks[5] == 0x1f); /* rd_wdata <- everything, including dmem_rdata */
        tc::verilog::Component component;
        tc::verilog::ComponentMetadata metadata;
        metadata.custom_id = 0x2927d7c021863f27ULL;
        assert(component.compile(source, metadata, &diagnostics));
        const auto* definition = component.definition();
        assert((definition->flags & TC_COMPONENT_TYPE_OUTPUT_DEPENDENCIES) != 0);
        assert((definition->flags & TC_COMPONENT_TYPE_INLINE_EXPRESSIONS) == 0);
        assert((definition->flags & TC_COMPONENT_TYPE_GROUPED_OUTPUTS) != 0);
        assert(definition->output_dependency_count == 17);
        assert(definition->output_dependencies[1] == 0x2);
        assert(definition->output_dependencies[5] == 0x1f);
    }
    /* The component bridge accepts the game's full 32-port shape.  Thirty-two
       64-bit inputs are the 2048-bit boundary; a thirty-third port is refused. */
    {
        auto source=[](uint32_t count) {
            std::string text="module wide_inputs(";
            for(uint32_t i=0;i<count;++i) {
                if(i)text+=", ";
                text+="input wire [63:0] i"+std::to_string(i);
            }
            text+=", output wire y); assign y = i0[0]; endmodule";
            return text;
        };
        Program program;
        std::vector<Diagnostic> diagnostics;
        assert(program.compile(source(32),&diagnostics));
        assert(program.inputCount()==32);
        diagnostics.clear();
        assert(!program.compile(source(33),&diagnostics));
    }
    {
        Fixture f(R"(
            module tri_add(
                input wire [7:0] a, b,
                input wire oe,
                output wire [7:0] y
            );
            assign y = oe ? a + b : 8'bz;
            endmodule
        )");
        assert(f.program.moduleName() == "tri_add");
        assert(f.program.inputCount() == 3 && f.program.outputCount() == 1);
        assert(f.program.stateWords() == 0);
        f.input(0, 0x81, 0xff); f.input(1, 0x02, 0xff); f.input(2, 1, 1);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0x83 && f.outputs[0].drive_mask == 0xff);
        f.input(2, 0, 1); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0 && f.outputs[0].drive_mask == 0);
    }

    {
        Fixture f(R"(
            module counter(
                input wire clk,
                input wire reset,
                output reg [7:0] q
            );
            always @(posedge clk) begin
                if (reset) q <= 0;
                else q <= q + 1;
            end
            endmodule
        )");
        assert(f.program.stateWords() == 2); // q + previous clk
        f.input(0, 0, 1); f.input(1, 0, 1); f.run();
        assert(f.outputs[0].value == 0);
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 1 && f.outputs[0].drive_mask == 0xff);
        f.run(); // held-high is not another edge
        assert(f.outputs[0].value == 1);
        f.input(0, 0, 1); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 2);

        const auto before = f.state;
        f.input(1, 1, 1); f.input(0, 0, 1); f.run(TC_LOGIC_REFRESH);
        f.input(0, 1, 1); f.run(TC_LOGIC_REFRESH);
        assert(f.state == before); // REFRESH is combinational in the kernel too
    }

    {
        Fixture f(R"(
            module ram16x8(
                input wire clk,
                input wire we,
                input wire re,
                input wire [3:0] addr,
                input wire [7:0] wdata,
                output wire [7:0] rdata
            );
            reg [7:0] mem [0:15];
            always @(posedge clk) begin
                if (we) mem[addr] <= wdata;
            end
            assign rdata = re ? mem[addr] : 8'bz;
            endmodule
        )");
        assert(f.program.stateWords() == 17); // 16 bytes + previous clk
        f.input(0, 0, 1); f.input(1, 1, 1); f.input(2, 1, 1);
        f.input(3, 3, 0xf); f.input(4, 0xa5, 0xff); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0xa5 && f.outputs[0].drive_mask == 0xff);
        f.input(2, 0, 1); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].drive_mask == 0);
        f.input(2, 1, 1); f.input(3, 4, 0xf); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0 && f.outputs[0].drive_mask == 0xff);
    }

    /* Four-state folding: Z & 0 is a known zero; an unresolved arithmetic
       result is X internally and therefore crosses TCLogicIOV3 as Z. */
    {
        Fixture f(R"(
            module z_rules(input wire a, output wire y0, output wire y1, output wire y2);
            assign y0 = a & 1'b0;
            assign y1 = a + 1'b1;
            assign y2 = a && 1'b0;
            endmodule
        )");
        f.input(0, 0, 0); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0 && f.outputs[0].drive_mask == 1);
        assert(f.outputs[1].drive_mask == 0);
        assert(f.outputs[2].value == 0 && f.outputs[2].drive_mask == 1);
    }

    {
        Fixture f(R"(
            module shift_width(input wire [7:0] a, output wire [15:0] y);
            assign y = a << 8;
            endmodule
        )");
        f.input(0, 1, 0xff); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0 && f.outputs[0].drive_mask == 0xffff);
    }

    {
        Fixture f(R"(
            module literal_padding(output wire [7:0] one, output wire [7:0] ones);
            assign one = 8'b1;
            assign ones = '1;
            endmodule
        )");
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 1 && f.outputs[0].drive_mask == 0xff);
        assert(f.outputs[1].value == 0xff && f.outputs[1].drive_mask == 0xff);
    }

    /* Concatenation, replication, bit and part selects on both sides of an
       assignment.  The three non-blocking writes to q overlap on purpose: the
       selected bits compose in the order they were written. */
    {
        Fixture f(R"(
            module selects(
                input wire clk,
                input wire [7:0] a,
                output reg [7:0] q,
                output wire [15:0] joined,
                output wire [15:0] repeated,
                output wire [7:0] swapped
            );
            always @(posedge clk) begin
                q[0] <= a[7];
                q[7:4] <= a[3:0];
                q[5] <= 1'b1;
            end
            assign joined = {a, a};
            assign repeated = {2{8'h5a}};
            assign swapped = {a[3:0], a[7:4]};
            endmodule
        )");
        f.input(0, 0, 1);            /* clk low */
        f.input(1, 0x81, 0xff);
        f.run();
        assert(f.outputs[1].value == 0x8181 && f.outputs[1].drive_mask == 0xffff);
        assert(f.outputs[2].value == 0x5a5a && f.outputs[2].drive_mask == 0xffff);
        assert(f.outputs[3].value == 0x18 && f.outputs[3].drive_mask == 0xff);
        f.input(0, 1, 1); f.run();
        /* q[0] = a[7] = 1, q[7:4] = a[3:0] = 1 then q[5] = 1 -> 0x31 */
        assert(f.outputs[0].value == 0x31 && f.outputs[0].drive_mask == 0xff);
        f.run();                     /* held high: not a second edge */
        assert(f.outputs[0].value == 0x31);
    }

    /* Indexed part selects, a memory word's own bit select, and a part select
       that reaches past the end of the value (those bits are X). */
    {
        Fixture f(R"(
            module slices(
                input wire clk,
                input wire [7:0] a,
                input wire [2:0] pos,
                input wire [3:0] addr,
                input wire d,
                output wire [3:0] up,
                output wire [3:0] down,
                output wire [7:0] word
            );
            reg [7:0] mem [0:15];
            always @(posedge clk) mem[addr][0] <= d;
            assign up = a[pos +: 4];
            assign down = a[pos -: 4];
            assign word = mem[addr];
            endmodule
        )");
        f.input(0, 0, 1);
        f.input(1, 0x5a, 0xff);      /* 0101_1010 */
        f.input(2, 4, 7);
        f.input(3, 3, 0xf);
        f.input(4, 1, 1);
        f.run();
        assert(f.outputs[0].value == 0x5 && f.outputs[0].drive_mask == 0xf);  /* a[7:4] */
        assert(f.outputs[1].value == 0xd && f.outputs[1].drive_mask == 0xf);  /* a[4:1] */
        f.input(0, 1, 1); f.run();
        assert(f.outputs[2].value == 0x01 && f.outputs[2].drive_mask == 0xff);
        f.input(2, 7, 7);
        f.run(TC_LOGIC_REFRESH);
        /* a[10:7]: only bit 7 is inside the value, so bit 0 is known and the
           three bits above it are X (and cross the bridge as Z). */
        assert(f.outputs[0].value == 0 && f.outputs[0].drive_mask == 0x1);
        assert(f.outputs[1].value == 0x5 && f.outputs[1].drive_mask == 0xf);  /* a[7:4] */
    }

    /* A wire may be driven by several continuous assignments as long as their
       bit ranges do not overlap. */
    {
        Fixture f(R"(
            module halves(input wire [3:0] a, input wire [3:0] b, output wire [7:0] y);
            assign y[3:0] = a;
            assign y[7:4] = b;
            endmodule
        )");
        f.input(0, 0x3, 0xf); f.input(1, 0xc, 0xf);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0xc3 && f.outputs[0].drive_mask == 0xff);
    }

    /* case: comma-separated labels, a default, and a body that is a block. */
    {
        Fixture f(R"(
            module dec(
                input wire clk,
                input wire [2:0] op,
                output reg [7:0] q
            );
            always @(posedge clk)
                case (op)
                    3'd0, 3'd1: q <= 8'h01;
                    3'd2: begin
                        q <= 8'h02;
                    end
                    default: q <= 8'hff;
                endcase
            endmodule
        )");
        const auto edge = [&](uint64_t op) {
            f.input(0, 0, 1); f.input(1, op, 7); f.run();
            f.input(0, 1, 1); f.run();
        };
        edge(0); assert(f.outputs[0].value == 0x01);
        edge(1); assert(f.outputs[0].value == 0x01);
        edge(2); assert(f.outputs[0].value == 0x02);
        edge(7); assert(f.outputs[0].value == 0xff);
    }

    /* casez: `?` (stored as X by the literal parser) and Z are don't-cares, and
       a default written first still runs only when nothing matched. */
    {
        Fixture f(R"(
            module decode(
                input wire clk,
                input wire [3:0] addr,
                input wire [1:0] sel,
                output reg [3:0] q,
                output reg [3:0] r
            );
            always @(posedge clk) begin
                casez (addr)
                    4'b1???: q <= 4'h8;
                    4'b01?0: q <= 4'h4;
                    default: q <= 4'h0;
                endcase
                case (sel)
                    default: r <= 4'h0;
                    2'b00:   r <= 4'h1;
                    2'b01:   r <= 4'h2;
                endcase
            end
            endmodule
        )");
        const auto edge = [&](uint64_t addr, uint64_t sel, uint64_t drive = 3) {
            f.input(0, 0, 1); f.input(1, addr, 0xf); f.input(2, sel, drive); f.run();
            f.input(0, 1, 1); f.run();
        };
        edge(0b1000, 0); assert(f.outputs[0].value == 0x8 && f.outputs[1].value == 0x1);
        edge(0b0100, 1); assert(f.outputs[0].value == 0x4 && f.outputs[1].value == 0x2);
        edge(0b0110, 2); assert(f.outputs[0].value == 0x4 && f.outputs[1].value == 0x0);
        edge(0b0010, 3); assert(f.outputs[0].value == 0x0 && f.outputs[1].value == 0x0);
        /* An undriven selector is Z in a plain case: no label matches, so the
           default arm runs. */
        f.input(0, 0, 1); f.input(1, 0, 0); f.input(2, 0, 0); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[1].value == 0x0);
    }

    /* casex additionally treats X and Z in the *case expression* as don't
       cares - which is why it is the one style that accepts a fully unknown
       selector. */
    {
        Fixture f(R"(
            module wildx(
                input wire clk,
                input wire [3:0] a,
                output reg [3:0] q
            );
            always @(posedge clk)
                casex (a)
                    4'b1x00: q <= 4'h5;
                    default: q <= 4'h0;
                endcase
            endmodule
        )");
        f.input(0, 0, 1); f.input(1, 0b1000, 0xf); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0x5);
        f.input(0, 0, 1); f.input(1, 0, 0); f.run();   /* every bit Z */
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0x5);
        f.input(0, 0, 1); f.input(1, 0b0000, 0xf); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0x0);
    }

    /* Asynchronous reset: the block triggers on the clock's rising edge *or*
       the reset's falling edge, so asserting the reset clears the register
       without a clock edge.  One state word per sensitivity entry. */
    {
        Fixture f(R"(
            module ared(
                input wire clk,
                input wire rst_n,
                input wire en,
                output reg [3:0] q
            );
            always @(posedge clk or negedge rst_n)
                if (!rst_n) q <= 4'd0;
                else if (en) q <= q + 4'd1;
            endmodule
        )");
        assert(f.program.stateWords() == 3);   /* q + clk level + rst_n level */
        /* The kernel samples the level on each host cycle, so a low run has to
           happen between two rising edges for the next edge to be an edge. */
        const auto clockEdge = [&]() {
            f.input(0, 0, 1); f.run();
            f.input(0, 1, 1); f.run();
        };
        f.input(0, 0, 1); f.input(1, 1, 1); f.input(2, 1, 1);
        f.run();                                /* first sample, no edge yet */
        clockEdge(); assert(f.outputs[0].value == 1);
        clockEdge(); assert(f.outputs[0].value == 2);
        clockEdge(); assert(f.outputs[0].value == 3);
        /* The falling reset edge alone clears it; no clock edge is involved. */
        f.input(1, 0, 1); f.run();
        assert(f.outputs[0].value == 0);
        /* Held low, clock edges keep taking the reset branch. */
        clockEdge(); assert(f.outputs[0].value == 0);
        clockEdge(); assert(f.outputs[0].value == 0);
        /* Releasing the reset is not an edge for this list, so q stays put. */
        f.input(1, 1, 1); f.run();
        assert(f.outputs[0].value == 0);
        clockEdge(); assert(f.outputs[0].value == 1);
        f.input(2, 0, 1); clockEdge(); assert(f.outputs[0].value == 1);
    }

    /* A negedge-only block, and a sensitivity list that uses the comma form. */
    {
        Fixture f(R"(
            module edges(
                input wire clk,
                input wire a,
                input wire b,
                output reg [3:0] down,
                output reg toggle
            );
            always @(negedge clk) down <= down + 4'd1;
            always @(posedge a, posedge b) toggle <= ~toggle;
            endmodule
        )");
        assert(f.program.stateWords() == 5);   /* down + toggle + three levels */
        f.input(0, 1, 1); f.input(1, 0, 1); f.input(2, 0, 1);
        f.run();                                /* sample: both low, clk high */
        f.input(0, 0, 1); f.run();              /* falling clk */
        assert(f.outputs[0].value == 1 && f.outputs[1].value == 0);
        f.input(0, 1, 1); f.run();              /* rising clk: not in the list */
        assert(f.outputs[0].value == 1);
        f.input(0, 0, 1); f.run();
        assert(f.outputs[0].value == 2);
        f.input(1, 1, 1); f.run();              /* posedge a toggles */
        assert(f.outputs[1].value == 1);
        f.input(2, 1, 1); f.run();              /* posedge b toggles */
        assert(f.outputs[1].value == 0);
    }

    /* for loops are unrolled while compiling, with the loop variable folded to
       a literal: `q[i]` and `a[7 - i]` both come out of the substitution. */
    {
        Fixture f(R"(
            module reverse(
                input wire clk,
                input wire [7:0] a,
                output reg [7:0] q
            );
            integer i;
            always @(posedge clk)
                for (i = 0; i < 8; i = i + 1) q[i] <= a[7 - i];
            endmodule
        )");
        assert(f.program.stateWords() == 2);   /* q + clock; the loop variable is free */
        f.input(0, 0, 1); f.input(1, 0xc0, 0xff); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0x03 && f.outputs[0].drive_mask == 0xff);
    }

    /* A module-level for (with or without `generate`) unrolls assign items, and
       an indexed part select can be built from the loop variable. */
    {
        Fixture f(R"(
            module nibbles(
                input wire [7:0] a,
                output wire [7:0] y
            );
            integer i;
            for (i = 0; i < 2; i = i + 1)
                assign y[i*4 +: 4] = a[(1 - i)*4 +: 4];
            endmodule
        )");
        f.input(0, 0x12, 0xff);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0x21 && f.outputs[0].drive_mask == 0xff);
    }

    /* A generate loop that emits one always block per bit: the classic
       "vector of registers" idiom.  Every unrolled block gets its own clock
       state word. */
    {
        Fixture f(R"(
            module regs(
                input wire clk,
                input wire [3:0] d,
                output reg [3:0] q
            );
            genvar g;
            generate
                for (g = 0; g < 4; g = g + 1)
                    always @(posedge clk) q[g] <= d[g];
            endgenerate
            endmodule
        )");
        assert(f.program.stateWords() == 5);   /* q + four unrolled clock words */
        f.input(0, 0, 1); f.input(1, 0b1010, 0xf); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0b1010 && f.outputs[0].drive_mask == 0xf);
        f.input(0, 0, 1); f.input(1, 0b0101, 0xf); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0b0101);
    }

    /* Nested statement loops, with the loop variables also used as arithmetic
       operands. */
    {
        Fixture f(R"(
            module pairs(
                input wire clk,
                input wire [7:0] a,
                output reg [7:0] q
            );
            integer i, j;
            always @(posedge clk)
                for (i = 0; i < 2; i = i + 1)
                    for (j = 0; j < 2; j = j + 1)
                        q[i*2 + j] <= a[3 - (i*2 + j)];
            endmodule
        )");
        f.input(0, 0, 1); f.input(1, 0x0c, 0xff); f.run();
        f.input(0, 1, 1); f.run();
        /* a[3..0] = 1100 -> q[3..0] = 0011 */
        assert(f.outputs[0].value == 0x03 && f.outputs[0].drive_mask == 0xff);
    }

    /* Parameters: the `#(...)` list is bound before the ports are parsed, so a
       port width and an array bound can both come from it. */
    {
        Fixture f(R"(
            module pmem #(parameter W = 8, D = 4)(
                input wire clk,
                input wire we,
                input wire [1:0] addr,
                input wire [W-1:0] wdata,
                output wire [W-1:0] rdata
            );
            reg [W-1:0] mem [0:D-1];
            always @(posedge clk) if (we) mem[addr] <= wdata;
            assign rdata = mem[addr];
            endmodule
        )");
        assert(f.program.ports()[3].bits == 8);   /* [W-1:0] */
        assert(f.program.ports()[4].bits == 8);
        assert(f.program.stateWords() == 5);      /* D words + the clock word */
        f.input(0, 0, 1); f.input(1, 1, 1); f.input(2, 2, 3); f.input(3, 0xa5, 0xff); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0xa5 && f.outputs[0].drive_mask == 0xff);
        f.input(2, 3, 3); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0 && f.outputs[0].drive_mask == 0xff);
    }

    /* A localparam built from a parameter, used as a loop bound and as an
       operand; two parameters drive replication counts. */
    {
        Fixture f(R"(
            module rev #(parameter W = 4)(
                input wire clk,
                input wire [W-1:0] a,
                output reg [W-1:0] q,
                output wire [2*W-1:0] doubled
            );
            localparam TOP = W - 1;
            integer i;
            always @(posedge clk)
                for (i = 0; i < W; i = i + 1) q[i] <= a[TOP - i];
            assign doubled = {a, a};
            endmodule
        )");
        assert(f.program.ports()[2].bits == 4);   /* W */
        assert(f.program.ports()[3].bits == 8);   /* 2*W */
        assert(f.program.stateWords() == 2);
        f.input(0, 0, 1); f.input(1, 0b1100, 0xf); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0x3 && f.outputs[0].drive_mask == 0xf);
        assert(f.outputs[1].value == 0xcc && f.outputs[1].drive_mask == 0xff);
    }

    {
        Fixture f(R"(
            module ones #(parameter N = 4)(
                output wire [N-1:0] y,
                output wire [2*N-1:0] z
            );
            localparam M = 2 * N;
            assign y = {N{1'b1}};
            assign z = {M{1'b1}};
            endmodule
        )");
        assert(f.program.stateWords() == 0);      /* constants own no state */
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0xf && f.outputs[0].drive_mask == 0xf);
        assert(f.outputs[1].value == 0xff && f.outputs[1].drive_mask == 0xff);
    }

    /* A parameter declared in the module body is evaluated at its own line, so
       a later localparam can build on it. */
    {
        Fixture f(R"(
            module offset(input wire [7:0] a, output wire [7:0] y);
            parameter BASE = 8'd16;
            localparam STEP = BASE / 2;
            assign y = a + BASE + STEP;
            endmodule
        )");
        f.input(0, 1, 0xff);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 25 && f.outputs[0].drive_mask == 0xff);
    }

    /* Signed and unsigned comparison: the same bits order differently. */
    {
        Fixture f(R"(
            module cmp(
                input wire signed [3:0] a,
                input wire signed [3:0] b,
                input wire [3:0] u,
                output wire lt_signed,
                output wire lt_unsigned
            );
            assign lt_signed = a < b;
            assign lt_unsigned = u < b;
            endmodule
        )");
        f.input(0, 0b1000, 0xf);   /* -8 signed, 8 unsigned */
        f.input(1, 0b0001, 0xf);
        f.input(2, 0b1000, 0xf);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 1 && f.outputs[0].drive_mask == 1);   /* -8 < 1 */
        assert(f.outputs[1].value == 0 && f.outputs[1].drive_mask == 1);   /* 8 < 1 is false */
    }

    /* Widening a signed value sign-extends it; >>> fills with the sign bit while
       >> fills with zero; $unsigned(a) makes the same bits widen with zeros. */
    {
        Fixture f(R"(
            module sext(
                input wire signed [3:0] a,
                output wire signed [7:0] ext,
                output wire [7:0] zext,
                output wire signed [3:0] asr,
                output wire [3:0] lsr,
                output wire [3:0] shl
            );
            assign ext = a;
            assign zext = $unsigned(a);
            assign asr = a >>> 1;
            assign lsr = a >> 1;
            assign shl = a <<< 1;
            endmodule
        )");
        f.input(0, 0b1110, 0xf);   /* -2 */
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0xfe && f.outputs[0].drive_mask == 0xff);
        assert(f.outputs[1].value == 0x0e && f.outputs[1].drive_mask == 0xff);
        assert(f.outputs[2].value == 0b1111 && f.outputs[2].drive_mask == 0xf);   /* -1 */
        assert(f.outputs[3].value == 0b0111 && f.outputs[3].drive_mask == 0xf);
        assert(f.outputs[4].value == 0b1100 && f.outputs[4].drive_mask == 0xf);
    }

    /* Signed literals and signed division / negation. */
    {
        Fixture f(R"(
            module sdiv(
                input wire signed [3:0] a,
                output wire signed [3:0] half,
                output wire signed [3:0] neg,
                output wire [3:0] lit
            );
            assign half = a / 4'sd2;
            assign neg  = -a;
            assign lit  = 4'sh8;
            endmodule
        )");
        f.input(0, 0b1110, 0xf);   /* -2 */
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0b1111 && f.outputs[0].drive_mask == 0xf);   /* -2/2 = -1 */
        assert(f.outputs[1].value == 0b0010 && f.outputs[1].drive_mask == 0xf);   /* -(-2) = 2 */
        assert(f.outputs[2].value == 0b1000 && f.outputs[2].drive_mask == 0xf);   /* 4'sh8 */
    }

    /* $signed on an unsigned port, and a mixed-type expression staying
       unsigned. */
    {
        Fixture f(R"(
            module casts(
                input wire [3:0] u,
                output wire lt_zero,
                output wire mixed_lt
            );
            assign lt_zero = $signed(u) < 4'sd0;
            assign mixed_lt = ($signed(u) + 4'sd1) > 4'sd0;
            endmodule
        )");
        f.input(0, 0b1000, 0xf);   /* -8 when read as signed */
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 1 && f.outputs[0].drive_mask == 1);
        /* 4'sd0 is signed while $signed(u)+4'sd1 is signed, so this stays
           signed: -7 > 0 is false. */
        assert(f.outputs[1].value == 0 && f.outputs[1].drive_mask == 1);
    }

    /* Two modules in one source: the top instantiates the other, and the
       instance is flattened into it (its output port drives the parent's net). */
    {
        Fixture f(R"(
            module mux2(
                input wire [7:0] d0,
                input wire [7:0] d1,
                input wire sel,
                output wire [7:0] y
            );
            assign y = sel ? d1 : d0;
            endmodule

            module top(
                input wire [7:0] a,
                input wire [7:0] b,
                input wire s,
                output wire [7:0] y
            );
            mux2 u0(.d0(a), .d1(b), .sel(s), .y(y));
            endmodule
        )");
        assert(f.program.moduleName() == "top");
        assert(f.program.inputCount() == 3 && f.program.outputCount() == 1);
        assert(f.program.stateWords() == 0);
        f.input(0, 0x12, 0xff); f.input(1, 0x34, 0xff); f.input(2, 0, 1);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0x12 && f.outputs[0].drive_mask == 0xff);
        f.input(2, 1, 1); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0x34 && f.outputs[0].drive_mask == 0xff);
    }

    /* Positional connections, and a child whose state (a register and its clock
       word) becomes part of the parent's state array. */
    {
        Fixture f(R"(
            module counter(
                input wire clk,
                input wire en,
                output reg [3:0] q
            );
            always @(posedge clk) if (en) q <= q + 4'd1;
            endmodule

            module top(input wire clk, input wire en, output wire [3:0] count);
            counter u0(clk, en, count);
            endmodule
        )");
        assert(f.program.stateWords() == 2);   /* the child's q + its clock word */
        f.input(0, 0, 1); f.input(1, 1, 1); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 1 && f.outputs[0].drive_mask == 0xf);
        f.input(0, 0, 1); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 2);
        f.input(1, 0, 1);
        f.input(0, 0, 1); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 2);       /* enable low: held */
    }

    /* The same module instantiated twice with different parameter overrides. */
    {
        Fixture f(R"(
            module ones #(parameter N = 4)(output wire [N-1:0] y);
            assign y = {N{1'b1}};
            endmodule

            module top(output wire [7:0] wide, output wire [1:0] narrow);
            ones #(.N(8)) u0(.y(wide));
            ones #(.N(2)) u1(.y(narrow));
            endmodule
        )");
        assert(f.program.outputCount() == 2);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0xff && f.outputs[0].drive_mask == 0xff);
        assert(f.outputs[1].value == 0b11 && f.outputs[1].drive_mask == 0b11);
    }

    /* A generate loop that instantiates one module per bit: the connection is a
       bit select on both sides. */
    {
        Fixture f(R"(
            module inv(input wire a, output wire y);
            assign y = ~a;
            endmodule

            module top(input wire [3:0] a, output wire [3:0] y);
            genvar g;
            generate
                for (g = 0; g < 4; g = g + 1) inv u(.a(a[g]), .y(y[g]));
            endgenerate
            endmodule
        )");
        f.input(0, 0b1010, 0xf);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0b0101 && f.outputs[0].drive_mask == 0xf);
    }

    /* `.a` connects the same-named net; an unconnected input leaves that port
       undriven (Z), so `0 & Z` is still a known 0 while `1 & Z` is not. */
    {
        Fixture f(R"(
            module drop(input wire a, input wire b, output wire y);
            assign y = a & b;
            endmodule

            module top(input wire a, output wire y);
            drop u(.a, .b(), .y(y));
            endmodule
        )");
        f.input(0, 0, 1); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0 && f.outputs[0].drive_mask == 1);
        f.input(0, 1, 1); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].drive_mask == 0);
    }

    /* Combinational blocks: always_comb assigns immediately, owns no edge
       state, and is re-evaluated on REFRESH alone (no clock edge involved). */
    {
        Fixture f(R"(
            module comb(
                input wire [7:0] d0,
                input wire [7:0] d1,
                input wire sel,
                output reg [7:0] y
            );
            always_comb begin
                if (sel) y = d1;
                else     y = d0;
            end
            endmodule
        )");
        assert(f.program.stateWords() == 0);   /* fully assigned y is invocation-local */
        f.input(0, 0x12, 0xff); f.input(1, 0x34, 0xff); f.input(2, 0, 1);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0x12 && f.outputs[0].drive_mask == 0xff);
        f.input(2, 1, 1); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0x34 && f.outputs[0].drive_mask == 0xff);
    }

    /* Blocking assignments inside one block chain, and two blocks settle no
       matter what order they are declared in: `y`'s block is written first and
       reads `t`, which the second block computes. */
    {
        Fixture f(R"(
            module cross(
                input wire [7:0] a,
                output reg [7:0] t,
                output reg [7:0] y
            );
            always @(*) y = t & 8'h0f;
            always @(a) begin
                t = ~a;
            end
            endmodule
        )");
        assert(f.program.stateWords() == 0);   /* topologically ordered scratch values */
        f.input(0, 0xf0, 0xff);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0x0f && f.outputs[0].drive_mask == 0xff);
        assert(f.outputs[1].value == 0x0f && f.outputs[1].drive_mask == 0xff);
    }

    /* A selected-output callback executes only that output's reverse slice.
       `held` is deliberately an incomplete combinational assignment (a latch),
       so touching the unrelated y1 slice would be observable in persistent
       state.  reserved=N+1 is the private native-bridge selector. */
    {
        Fixture f(R"(
            module sliced(
                input wire a,
                input wire b,
                output wire y0,
                output wire y1
            );
            reg held;
            always @(*) if (b) held = 1'b1;
            assign y0 = a;
            assign y1 = held;
            endmodule
        )");
        assert(f.program.stateWords() == 1);  /* inferred latch remains persistent */
        f.input(0, 1, 1); f.input(1, 1, 1);
        f.io.reserved = 1;                    /* output 0 only */
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 1 && f.outputs[0].drive_mask == 1);
        assert(f.state[0] == 0);              /* unrelated latch block did not run */
        f.io.reserved = 2;                    /* output 1 */
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[1].value == 1 && f.state[0] == 1);
        f.io.reserved = 0;
    }

    /* The hidden clock sink uses UINT32_MAX: it commits edge-triggered state
       but intentionally skips every post-edge output cone.  The next visible
       output callback observes the committed value. */
    {
        Fixture f(R"(
            module tick_only(input wire clk, output reg q, output reg qbar);
            always @(posedge clk) q <= 1'b1;
            always_comb qbar = ~q;
            endmodule
        )");
        f.input(0, 0, 1); f.run();
        f.input(0, 1, 1);
        f.io.reserved = ~uint32_t(0);
        f.run();
        assert(f.state[0] == 1);
        assert(f.outputs[0].drive_mask == 0 && f.outputs[1].drive_mask == 0);
        f.io.reserved = 2;
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[1].value == 0 && f.outputs[1].drive_mask == 1);
        f.io.reserved = 0;
    }

    /* The bridge's high-bit selector evaluates every output with the same
       dependency mask in one callback, while leaving other cones untouched. */
    {
        Fixture f(R"(
            module grouped(input wire a, input wire b,
                           output reg y0, output reg y1, output wire y2);
            always_comb begin y0 = a; y1 = ~a; end
            assign y2 = b;
            endmodule
        )");
        f.input(0, 1, 1); f.input(1, 1, 1);
        f.io.reserved = (uint32_t(1) << 31) | 1;
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 1 && f.outputs[0].drive_mask == 1);
        assert(f.outputs[1].value == 0 && f.outputs[1].drive_mask == 1);
        assert(f.outputs[2].drive_mask == 0);
        f.io.reserved = 0;
    }

    /* Pure scratch blocks shared by selected-output slices may be reused, but
       an input change must invalidate the transient cache immediately. */
    {
        Fixture f(R"(
            module cached(input wire [7:0] a, output reg [7:0] t,
                          output reg [7:0] y);
            always_comb t = a + 8'd1;
            always_comb y = t ^ 8'hff;
            endmodule
        )");
        f.input(0, 3, 0xff); f.io.reserved = 1; f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 4);
        f.io.reserved = 2; f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[1].value == 0xfb);
        f.input(0, 9, 0xff); f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[1].value == 0xf5);
        f.io.reserved = 0;
    }

    /* A real combinational cycle cannot be topologically scheduled and keeps
       the bounded fixed-point/persistent starting-value path. */
    {
        Fixture f(R"(
            module ring(output reg a, output reg b);
            always @(*) a = b;
            always @(*) b = a;
            endmodule
        )");
        assert(f.program.stateWords() == 2);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].drive_mask == 1 && f.outputs[1].drive_mask == 1);
    }

    /* A combinational block that reads a register: after a clock edge it must
       see the value the clocked block just committed. */
    {
        Fixture f(R"(
            module mix(
                input wire clk,
                input wire en,
                output reg [3:0] q,
                output reg [3:0] qbar
            );
            always @(posedge clk) if (en) q <= q + 4'd1;
            always_comb qbar = ~q;
            endmodule
        )");
        /* q and the clock word persist; the fully assigned qbar is scratch. */
        assert(f.program.stateWords() == 2);
        f.input(0, 0, 1); f.input(1, 1, 1); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 1 && f.outputs[1].value == 0xe);
        f.input(0, 0, 1); f.run();
        /* REFRESH (no edge) still recomputes the combinational half. */
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 1 && f.outputs[1].value == 0xe);
    }

    /* Every unrolled iteration is its own scope: the same signal name may be
       declared in each one, and an instantiation inside the body wires through
       that iteration's own net. */
    {
        Fixture f(R"(
            module inv(input wire a, output wire y);
            assign y = ~a;
            endmodule

            module top(input wire [3:0] a, output wire [3:0] y);
            genvar g;
            generate
                for (g = 0; g < 4; g = g + 1) begin
                    wire t;
                    inv u(.a(a[g]), .y(t));
                    assign y[g] = t;
                end
            endgenerate
            endmodule
        )");
        f.input(0, 0b0101, 0xf);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0b1010 && f.outputs[0].drive_mask == 0xf);
    }

    /* ...which is what makes "one register per bit" writable: four separate
       registers, each with its own clock word. */
    {
        Fixture f(R"(
            module top(
                input wire clk,
                input wire en,
                input wire [3:0] d,
                output wire [3:0] q
            );
            genvar g;
            generate
                for (g = 0; g < 4; g = g + 1) begin : bit
                    reg r;
                    always @(posedge clk) if (en) r <= d[g];
                    assign q[g] = r;
                end
            endgenerate
            endmodule
        )");
        assert(f.program.stateWords() == 8);   /* four registers + four clock words */
        f.input(0, 0, 1); f.input(1, 1, 1); f.input(2, 0b1010, 0xf); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0b1010 && f.outputs[0].drive_mask == 0xf);
        f.input(1, 0, 1);
        f.input(0, 0, 1); f.input(2, 0b0101, 0xf); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 0b1010);    /* enable low: held */
    }

    /* A declaration inside a generate block shadows an outer name of the same
       spelling for the block's own statements only. */
    {
        Fixture f(R"(
            module top(input wire a, output wire [1:0] y);
            wire t;
            assign t = a;
            generate
                begin
                    wire t;
                    assign t = ~a;
                    assign y[0] = t;
                end
            endgenerate
            assign y[1] = t;
            endmodule
        )");
        f.input(0, 1, 1);
        f.run(TC_LOGIC_REFRESH);
        /* y[0] sees the block's own t (= ~a = 0), y[1] the module-level t (= a = 1). */
        assert(f.outputs[0].value == 0b10 && f.outputs[0].drive_mask == 0b11);
    }

    /* generate if / else: the branch is chosen while compiling, and an instance
       parameter override reaches the condition. */
    {
        Fixture f(R"(
            module leaf(input wire a, output wire y);
            assign y = ~a;
            endmodule

            module wide #(parameter WIDE = 1)(
                input wire [1:0] a,
                output wire [1:0] y
            );
            generate
                if (WIDE) begin
                    leaf u0(.a(a[0]), .y(y[0]));
                    assign y[1] = a[1];
                end else begin
                    assign y[0] = a[0];
                    assign y[1] = ~a[1];
                end
            endgenerate
            endmodule

            module narrow(input wire [1:0] a, output wire [1:0] y);
            wide #(.WIDE(0)) u(.a(a), .y(y));
            endmodule
        )");
        /* The top is `narrow`, which instantiates wide with WIDE=0. */
        assert(f.program.moduleName() == "narrow");
        f.input(0, 0b01, 0b11);
        f.run(TC_LOGIC_REFRESH);
        /* WIDE=0: y[0] = a[0] = 1, y[1] = ~a[1] = 1. */
        assert(f.outputs[0].value == 0b11 && f.outputs[0].drive_mask == 0b11);
    }

    {
        Fixture f(R"(
            module wide #(parameter WIDE = 1)(
                input wire [1:0] a,
                output wire [1:0] y
            );
            generate
                if (WIDE) begin
                    assign y[0] = ~a[0];
                    assign y[1] = a[1];
                end else begin
                    assign y = a;
                end
            endgenerate
            endmodule
        )");
        f.input(0, 0b10, 0b11);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0b11 && f.outputs[0].drive_mask == 0b11);
    }

    /* generate case, with a default that is not the last item. */
    {
        Fixture f(R"(
            module pick #(parameter MODE = 2)(
                input wire [1:0] a,
                output wire [1:0] y
            );
            generate
                case (MODE)
                    default: begin assign y = ~a; end
                    0: begin assign y = a; end
                    1: begin assign y = {a[1], ~a[0]}; end
                endcase
            endgenerate
            endmodule
        )");
        f.input(0, 0b10, 0b11);
        f.run(TC_LOGIC_REFRESH);
        assert(f.outputs[0].value == 0b01 && f.outputs[0].drive_mask == 0b11);   /* MODE=2 -> default */
    }

    /* Hierarchical names read an inlined instance's own signals, including ones
       that are not ports. */
    {
        Fixture f(R"(
            module counter(
                input wire clk,
                input wire en,
                output reg [3:0] q
            );
            reg [3:0] shadow;
            always @(posedge clk) begin
                if (en) q <= q + 4'd1;
                shadow <= q;
            end
            endmodule

            module top(
                input wire clk,
                input wire en,
                output wire [3:0] count,
                output wire [3:0] inner,
                output wire [3:0] again
            );
            counter u0(.clk(clk), .en(en), .q(count));
            assign inner = u0.shadow;
            assign again = u0.q;
            endmodule
        )");
        assert(f.program.stateWords() == 3);   /* q + shadow + one clock word */
        f.input(0, 0, 1); f.input(1, 1, 1); f.run();
        f.input(0, 1, 1); f.run();
        /* q is 1 now; shadow took the value q had before the edge. */
        assert(f.outputs[0].value == 1 && f.outputs[1].value == 0);
        assert(f.outputs[2].value == 1);
        f.input(0, 0, 1); f.run();
        f.input(0, 1, 1); f.run();
        assert(f.outputs[0].value == 2 && f.outputs[1].value == 1);
    }

    {
        Program p;
        std::vector<Diagnostic> diagnostics;
        assert(!p.compile("module bad(input a, output y); always @(*) y = a; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].line == 1 && diagnostics[0].column != 0);
        assert(!p.valid());
        assert(p.compile("module good(input a, output y); assign y = a; endmodule", &diagnostics));
        assert(!p.compile("module broken(input a, output y); assign y = nope; endmodule", &diagnostics));
        assert(p.valid() && p.moduleName() == "good"); // transactional failure

        tc::verilog::CompileOptions noState{}; noState.max_state_words = 0;
        assert(!p.compile("module x(output y); assign y = 0; endmodule", &diagnostics, noState));
        assert(p.valid() && p.moduleName() == "good");

        /* The new select syntax keeps its own refusals: a part select whose
           bounds are not constant, a replication wider than the engine's 64
           bits, and two continuous drivers on the same bits. */
        assert(!p.compile("module ps(input [7:0] a, input [2:0] i, output [3:0] y);"
                          " assign y = a[i:0]; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("constant") != std::string::npos);
        assert(!p.compile("module rp(output [63:0] y); assign y = {3{32'd0}}; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("replication") != std::string::npos);
        assert(!p.compile("module ov(input a, output y); assign y = a; assign y[0] = a; endmodule",
                          &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("continuous driver") != std::string::npos);
        assert(p.compile("module ok(input [7:0] a, output [7:0] y); assign y = {a[3:0], a[7:4]}; endmodule",
                         &diagnostics));

        /* case statements keep their own refusals: no endcase, two defaults,
           and a case item without a statement. */
        assert(!p.compile("module nc(input clk, input [1:0] s, output reg q);"
                          " always @(posedge clk) case (s) 2'd0: q <= 1; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("case") != std::string::npos);
        assert(!p.compile("module dd(input clk, input [1:0] s, output reg q);"
                          " always @(posedge clk) case (s) default: q <= 0; default: q <= 1; endcase endmodule",
                          &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("default") != std::string::npos);
        assert(!p.compile("module ns(input clk, input [1:0] s, output reg q);"
                          " always @(posedge clk) case (s) 2'd0: endcase endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("statement") != std::string::npos);

        /* Sensitivity lists keep their own refusals: an always with no list at
           all, a multi-bit edge signal, and a missing 'or'. */
        assert(!p.compile("module comb(input a, output reg q); always q <= a; endmodule",
                          &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("'@(...)'") != std::string::npos);
        assert(!p.compile("module wide(input clk, input [1:0] s, output reg q);"
                          " always @(posedge s) q <= clk; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("one bit wide") != std::string::npos);
        assert(!p.compile("module nosep(input clk, input rst, output reg q);"
                          " always @(posedge clk negedge rst) q <= clk; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("'or'") != std::string::npos);

        /* for loops are unrolled, so their bounds must be constant and the
           variable may not escape its own loop. */
        assert(!p.compile("module nb(input clk, input [7:0] a, output reg [7:0] q);"
                          " integer i; always @(posedge clk)"
                          " for (i = 0; i < a; i = i + 1) q[i] <= 1'b1; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("condition must be constant") != std::string::npos);
        assert(!p.compile("module esc(input [7:0] a, output [7:0] y);"
                          " integer i; assign y = a + i; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("inside its own for loop") != std::string::npos);
        assert(!p.compile("module sv(input clk, output reg q);"
                          " always @(posedge clk) for (genvar i = 0; i < 2; i = i + 1) q <= 1'b1; endmodule",
                          &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("declare the loop variable") != std::string::npos);
        assert(!p.compile("module zero(output [7:0] y);"
                          " genvar g; generate for (g = 0; g < 0; g = g + 1) assign y[g] = 1'b0;"
                          " endgenerate endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("at least once") != std::string::npos);
        assert(!p.compile("module spin(output reg q); integer i; always @(posedge q)"
                          " for (i = 0; i < 4; i = i) q <= 1'b1; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("4096") != std::string::npos);

        /* Parameters must be constant, must be declared before use, and cannot
           be assigned. */
        assert(!p.compile("module fw(output [7:0] y); localparam A = B; localparam B = 1;"
                          " assign y = A; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("unknown signal 'B'") != std::string::npos);
        assert(!p.compile("module nc(input [7:0] a, output [7:0] y); parameter P = a;"
                          " assign y = P; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("constant expression") != std::string::npos);
        assert(!p.compile("module dp(output [7:0] y); parameter P = 1; localparam P = 2;"
                          " assign y = P; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("duplicate") != std::string::npos);
        assert(!p.compile("module ap(input clk, output reg q); parameter P = 1;"
                          " always @(posedge clk) P <= 1'b1; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("procedural assignment target") != std::string::npos);

        /* The cast functions need their parentheses. */
        assert(!p.compile("module c1(input [3:0] a, output [3:0] y); assign y = $signed a; endmodule",
                          &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("$signed") != std::string::npos);

        /* Instantiation refusals: an unknown module, an unknown port, an output
           connected to something that is not a net, a positional parameter
           override, an override of a parameter that does not exist, and two
           drivers on the same bits. */
        assert(!p.compile("module t(input a, output y); nope u0(.a(a), .y(y)); endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("no module named") != std::string::npos);
        assert(!p.compile("module s(input a, output y); assign y = a; endmodule"
                          " module t(input a, output y); s u0(.a(a), .z(y)); endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("has no port named") != std::string::npos);
        assert(!p.compile("module s(input a, output y); assign y = a; endmodule"
                          " module t(input a, output reg y); s u0(.a(a), .y(y)); endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("output port") != std::string::npos);
        assert(!p.compile("module s #(parameter N = 2)(output [N-1:0] y); assign y = {N{1'b1}}; endmodule"
                          " module t(output [3:0] y); s #(4) u0(.y(y[1:0])); endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("named parameter override") != std::string::npos);
        assert(!p.compile("module s #(parameter N = 2)(output [N-1:0] y); assign y = {N{1'b1}}; endmodule"
                          " module t(output [1:0] y); s #(.W(4)) u0(.y(y)); endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("no parameter named") != std::string::npos);
        assert(!p.compile("module s(input a, output y); assign y = a; endmodule"
                          " module t(input a, output y); s u0(.a(a), .y(y)); s u1(.a(a), .y(y)); endmodule",
                          &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("already have a continuous driver") != std::string::npos);

        /* Combinational blocks keep their own refusals: a non-blocking
           assignment inside one, a variable written by both kinds of block, a
           clocked keyword with a level list, and always_comb with a list. */
        assert(!p.compile("module cb(input a, output reg q); always_comb q <= a; endmodule",
                          &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("use '='") != std::string::npos);
        assert(!p.compile("module both(input clk, input a, output reg q);"
                          " always @(posedge clk) q <= a; always_comb q = ~a; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("both a combinational") != std::string::npos);
        assert(!p.compile("module ff(input a, output reg q); always_ff @(a) q <= a; endmodule",
                          &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("always_ff") != std::string::npos);
        assert(!p.compile("module ac(input clk, output reg q);"
                          " always_comb @(posedge clk) q = clk; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("always_comb") != std::string::npos);

        /* A scoped declaration is local to its own block: it cannot be read
           after the block, and declaring the same name twice in one block is
           still a duplicate. */
        assert(!p.compile("module leak(input a, output y);"
                          " generate begin wire t; assign t = a; end endgenerate"
                          " assign y = t; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("unknown signal 't'") != std::string::npos);
        assert(!p.compile("module dup(input a, output [1:0] y);"
                          " generate begin wire t; wire t; assign t = a; assign y[0] = t; end"
                          " endgenerate assign y[1] = a; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("duplicate declaration") != std::string::npos);

        /* Generate conditions and labels must be constant, hierarchical names
           must exist and are read-only. */
        assert(!p.compile("module g(input a, output y);"
                          " generate if (a) begin assign y = a; end endgenerate endmodule",
                          &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("generate condition must be constant") != std::string::npos);
        assert(!p.compile("module s(input a, output y); assign y = a; endmodule"
                          " module t(input a, output y); wire missing; s u0(.a(a), .y(y));"
                          " assign missing = u0.nope; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("unknown hierarchical name") != std::string::npos);
        assert(!p.compile("module s(input clk, output reg q); always @(posedge clk) q <= ~q; endmodule"
                          " module t(input clk, output y); s u0(.clk(clk), .q(y));"
                          " always @(posedge clk) u0.q <= 1'b1; endmodule", &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("hierarchical") != std::string::npos);
    }

    /* Source-to-component adapter: port declarations become stable V3 pin
       records, state_words follows the inferred RAM/register layout, and the
       registered callback executes the same Program. */
    {
        tc::verilog::Component component;
        tc::verilog::ComponentMetadata metadata;
        metadata.custom_id = 0x5443564552544553ULL;
        metadata.type_id = "test/tc-verilog-counter";
        metadata.description = "generated from Verilog ports";
        metadata.gate_cost = 7; metadata.delay = 3; metadata.pin_lane = 3.0f;
        std::vector<Diagnostic> diagnostics;
        assert(component.compile(R"(
            module auto_counter(input clk, input enable, output reg [7:0] q);
            always @(posedge clk) if (enable) q <= q + 8'b1;
            endmodule
        )", metadata, &diagnostics));
        const TCComponentTypeDefinitionV3* definition = component.definition();
        assert(definition && definition->version == TC_COMPONENT_TYPES_VERSION_3);
        assert(std::string(definition->name) == "auto_counter");
        assert(std::string(definition->type_id) == metadata.type_id);
        assert(definition->input_count == 2 && definition->output_count == 1);
        assert(std::string(definition->inputs[0].pin_id) == "clk" && definition->inputs[0].bits == 1);
        assert(std::string(definition->inputs[1].name) == "enable" && definition->inputs[1].bits == 1);
        assert(std::string(definition->outputs[0].pin_id) == "q" && definition->outputs[0].bits == 8);
        assert(definition->state_words == 2 && definition->gate_cost == 7 && definition->delay == 3);
        assert(definition->pin_lane == 3.0f && definition->callback == &Program::callback);
        /* Sequential state is committed by a hidden all-input sink.  The
           public q pin is state-only, so it has no combinational input edge at
           all; in particular enable -> q is not invented by the wrapper. */
        assert((definition->flags&TC_COMPONENT_TYPE_OUTPUT_DEPENDENCIES)!=0);
        assert((definition->flags&TC_COMPONENT_TYPE_CLOCKED_TICK)!=0);
        assert((definition->flags&TC_COMPONENT_TYPE_GROUPED_OUTPUTS)!=0);
        assert(definition->output_dependency_count==1);
        assert(definition->output_dependencies[0]==0);

        /* A failed recompile is transactional at the component layer too. */
        assert(!component.compile("module nope(input a, output y); assign y = missing; endmodule",
                                  metadata, &diagnostics));
        assert(component.valid() && component.program().moduleName() == "auto_counter");

        FakeTypesHost fake;
        TCHost host = fake.host();
        assert(component.registerWith(&host) == TC_COMPONENT_TYPES_OK);
        assert(component.registered() && fake.queries == 1 && fake.registrations == 1);
        assert(fake.seen == component.definition() && fake.seen->user == &component.program());
        assert(component.registerWith(&host) == TC_COMPONENT_TYPES_ERR_DUPLICATE);
        assert(!component.compile("module later(input a, output y); assign y = a; endmodule",
                                  metadata, &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("immutable") != std::string::npos);

        TCLogicValueV3 in[2]{{0, 1}, {1, 1}}, out[1]{};
        uint64_t state[2]{};
        TCLogicIOV3 io{}; io.size = sizeof(io); io.version = TC_LOGIC_IO_V3_VERSION_1;
        io.phase = TC_LOGIC_CYCLE; io.input_count = 2; io.output_count = 1;
        io.inputs = in; io.outputs = out; io.state = state; io.state_words = 2;
        io.user = fake.seen->user;
        fake.seen->callback(&io);
        in[0] = {1, 1}; fake.seen->callback(&io);
        assert(out[0].value == 1 && out[0].drive_mask == 0xff);
    }

    {
        tc::verilog::Component component;
        tc::verilog::ComponentMetadata metadata; metadata.custom_id = 1;
        std::vector<Diagnostic> diagnostics;
        assert(component.compile("module sink(input [7:0] data); endmodule", metadata, &diagnostics));
        TCHost oldHost{}; oldHost.size = sizeof(oldHost);
        assert(component.registerWith(&oldHost) == TC_COMPONENT_TYPES_ERR_UNAVAILABLE);
        metadata.custom_id = 0;
        tc::verilog::Component invalid;
        assert(!invalid.compile("module x(output y); assign y = 1; endmodule", metadata, &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("custom_id") != std::string::npos);
    }

    {
        /* Hot reload: the program object keeps its address, so the callback user
           pointer the host stored at registration still names this component -
           and every instance runs the new logic from the next cycle on. */
        FakeTypesHost types;
        TCHost host = types.host();
        tc::verilog::Component component;
        tc::verilog::ComponentMetadata metadata;
        metadata.custom_id = 0x5443484f54303031ULL;   /* "THOT001" */
        metadata.name = "hot";
        std::vector<Diagnostic> diagnostics;
        const std::string first = R"(module hot(input wire [7:0] a, input wire en, output reg [7:0] y);
always @(posedge en) y <= a + 8'd1;
endmodule
)";
        const std::string second = R"(module hot(input wire [7:0] a, input wire en, output reg [7:0] y);
always @(posedge en) y <= a + 8'd2;
endmodule
)";
        const std::string moved = R"(module hot(input wire [7:0] a, input wire en, input wire rst, output reg [7:0] y);
always @(posedge en) y <= a + 8'd2;
endmodule
)";
        assert(component.compile(first, metadata, &diagnostics));
        assert(component.registerWith(&host) == TC_COMPONENT_TYPES_OK);
        assert(component.registered());

        const TCComponentTypeDefinitionV3* definition = component.definition();
        const void* user = definition->user;
        const uint64_t all = ~uint64_t(0);
        TCLogicValueV3 in[2]{{0, all}, {0, all}}, out[1]{};
        uint64_t state[4]{};
        TCLogicIOV3 io{};
        io.size = sizeof(io); io.version = TC_LOGIC_IO_V3_VERSION_1;
        io.phase = TC_LOGIC_CYCLE; io.input_count = 2; io.output_count = 1;
        io.inputs = in; io.outputs = out; io.state = state;
        io.state_words = static_cast<uint32_t>(definition->state_words);
        io.user = definition->user;
        auto pulse = [&]() {
            in[1] = {0, all}; definition->callback(&io);
            in[1] = {1, all}; definition->callback(&io);
        };
        in[0] = {10, all};
        pulse();
        assert((out[0].value & 0xff) == 11);

        assert(component.hotSwap(second, &diagnostics));
        assert(diagnostics.empty());
        assert(component.definition()->user == user);
        io.user = component.definition()->user;
        pulse();
        assert((out[0].value & 0xff) == 12);

        /* A moved port cannot be swapped into a live board. */
        assert(!component.hotSwap(moved, &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("same ports") != std::string::npos);
        assert(!component.checkHotSwap(moved, &diagnostics));
        /* A component that was never registered has nothing to reload. */
        tc::verilog::Component loose;
        assert(loose.compile(first, metadata, &diagnostics));
        assert(!loose.hotSwap(second, &diagnostics));
        assert(!diagnostics.empty() && diagnostics[0].message.find("registered") != std::string::npos);
    }

    std::cout << "TC-Verilog kernel tests passed\n";
    return 0;
}
