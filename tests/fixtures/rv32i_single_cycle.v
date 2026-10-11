// RV32I 单核单周期核心，兼容 TC-Verilog 可综合子集。
//
// 外部模块负责：
//   1. PC 寄存器：pc_we 时在时钟沿写入 pc_next。
//   2. 32×32 寄存器堆：组合读、同步写，并保证 x0 恒为零。
//   3. 指令存储器：根据 imem_addr 组合输出 instr。
//   4. 数据存储器：根据对齐后的 dmem_addr 组合输出 dmem_rdata，
//      在时钟沿按照 dmem_wstrb 写入 dmem_wdata。
//   5. 周期及退休指令计数器：retire=1 时递增 instret。
//
// 数据存储器采用小端、32 位对齐字接口。
// FENCE 在无缓存实现中视为 NOP。
// ECALL、EBREAK、非法指令及未对齐访问通过 trap 接口交给外部处理。
// Zicsr、M、A、C 等扩展不在本模块内。

module rv32i_single_cycle(
    input  wire [31:0] pc,
    input  wire [31:0] instr,
    input  wire [31:0] rs1_data,
    input  wire [31:0] rs2_data,
    input  wire [31:0] dmem_rdata,

    output wire [31:0] imem_addr,

    output wire [4:0]  rs1_addr,
    output wire [4:0]  rs2_addr,
    output wire [4:0]  rd_addr,
    output reg          rf_we,
    output reg  [31:0] rd_wdata,

    output reg  [31:0] dmem_addr,
    output reg          dmem_re,
    output reg          dmem_we,
    output reg  [31:0] dmem_wdata,
    output reg  [3:0]  dmem_wstrb,

    output reg  [31:0] pc_next,
    output reg          pc_we,

    output reg          retire,

    output reg          trap,
    output reg  [3:0]  trap_cause,
    output reg  [31:0] trap_value
);

wire [6:0] opcode;
wire [2:0] funct3;
wire [6:0] funct7;

wire [31:0] imm_i;
wire [31:0] imm_s;
wire [31:0] imm_b;
wire [31:0] imm_u;
wire [31:0] imm_j;

reg illegal;
reg take_branch;
reg [31:0] eff_addr;
reg [31:0] target;
reg [31:0] load_shifted;

assign opcode = instr[6:0];
assign funct3 = instr[14:12];
assign funct7 = instr[31:25];

assign rs1_addr = instr[19:15];
assign rs2_addr = instr[24:20];
assign rd_addr  = instr[11:7];

assign imem_addr = pc;

assign imm_i = {
    {20{instr[31]}},
    instr[31:20]
};

assign imm_s = {
    {20{instr[31]}},
    instr[31:25],
    instr[11:7]
};

assign imm_b = {
    {19{instr[31]}},
    instr[31],
    instr[7],
    instr[30:25],
    instr[11:8],
    1'b0
};

assign imm_u = {
    instr[31:12],
    12'b0
};

assign imm_j = {
    {11{instr[31]}},
    instr[31],
    instr[19:12],
    instr[20],
    instr[30:21],
    1'b0
};

always_comb begin
    rf_we = 1'b0;
    rd_wdata = 32'b0;

    dmem_addr = 32'b0;
    dmem_re = 1'b0;
    dmem_we = 1'b0;
    dmem_wdata = 32'b0;
    dmem_wstrb = 4'b0000;

    pc_next = pc + 32'd4;
    pc_we = 1'b1;

    retire = 1'b1;

    trap = 1'b0;
    trap_cause = 4'd0;
    trap_value = 32'b0;

    illegal = 1'b0;
    take_branch = 1'b0;
    eff_addr = 32'b0;
    target = 32'b0;
    load_shifted = 32'b0;

    // RV32I 的 IALIGN 为 32 位。
    if (pc[1:0] != 2'b00) begin
        trap = 1'b1;
        trap_cause = 4'd0;
        trap_value = pc;
    end else begin
        case (opcode)

            // LUI
            7'b0110111: begin
                rf_we = 1'b1;
                rd_wdata = imm_u;
            end

            // AUIPC
            7'b0010111: begin
                rf_we = 1'b1;
                rd_wdata = pc + imm_u;
            end

            // JAL
            7'b1101111: begin
                target = pc + imm_j;

                if (target[1:0] != 2'b00) begin
                    trap = 1'b1;
                    trap_cause = 4'd0;
                    trap_value = target;
                end else begin
                    rf_we = 1'b1;
                    rd_wdata = pc + 32'd4;
                    pc_next = target;
                end
            end

            // JALR
            7'b1100111: begin
                if (funct3 != 3'b000) begin
                    illegal = 1'b1;
                end else begin
                    target = (rs1_data + imm_i) & 32'hfffffffe;

                    if (target[1:0] != 2'b00) begin
                        trap = 1'b1;
                        trap_cause = 4'd0;
                        trap_value = target;
                    end else begin
                        rf_we = 1'b1;
                        rd_wdata = pc + 32'd4;
                        pc_next = target;
                    end
                end
            end

            // 条件分支
            7'b1100011: begin
                case (funct3)
                    // BEQ
                    3'b000:
                        take_branch = (rs1_data == rs2_data);

                    // BNE
                    3'b001:
                        take_branch = (rs1_data != rs2_data);

                    // BLT
                    3'b100:
                        take_branch =
                            ($signed(rs1_data) < $signed(rs2_data));

                    // BGE
                    3'b101:
                        take_branch =
                            ($signed(rs1_data) >= $signed(rs2_data));

                    // BLTU
                    3'b110:
                        take_branch = (rs1_data < rs2_data);

                    // BGEU
                    3'b111:
                        take_branch = (rs1_data >= rs2_data);

                    default:
                        illegal = 1'b1;
                endcase

                if (take_branch && !illegal) begin
                    target = pc + imm_b;

                    if (target[1:0] != 2'b00) begin
                        trap = 1'b1;
                        trap_cause = 4'd0;
                        trap_value = target;
                    end else begin
                        pc_next = target;
                    end
                end
            end

            // LOAD
            7'b0000011: begin
                eff_addr = rs1_data + imm_i;

                // 外部存储器读取包含目标字节的对齐 32 位字。
                dmem_addr = {eff_addr[31:2], 2'b00};
                dmem_re = 1'b1;

                load_shifted =
                    dmem_rdata >> {eff_addr[1:0], 3'b000};

                case (funct3)
                    // LB
                    3'b000: begin
                        rf_we = 1'b1;
                        rd_wdata = {
                            {24{load_shifted[7]}},
                            load_shifted[7:0]
                        };
                    end

                    // LH
                    3'b001: begin
                        if (eff_addr[0] != 1'b0) begin
                            trap = 1'b1;
                            trap_cause = 4'd4;
                            trap_value = eff_addr;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata = {
                                {16{load_shifted[15]}},
                                load_shifted[15:0]
                            };
                        end
                    end

                    // LW
                    3'b010: begin
                        if (eff_addr[1:0] != 2'b00) begin
                            trap = 1'b1;
                            trap_cause = 4'd4;
                            trap_value = eff_addr;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata = load_shifted;
                        end
                    end

                    // LBU
                    3'b100: begin
                        rf_we = 1'b1;
                        rd_wdata = {
                            24'b0,
                            load_shifted[7:0]
                        };
                    end

                    // LHU
                    3'b101: begin
                        if (eff_addr[0] != 1'b0) begin
                            trap = 1'b1;
                            trap_cause = 4'd4;
                            trap_value = eff_addr;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata = {
                                16'b0,
                                load_shifted[15:0]
                            };
                        end
                    end

                    default:
                        illegal = 1'b1;
                endcase
            end

            // STORE
            7'b0100011: begin
                eff_addr = rs1_data + imm_s;
                dmem_addr = {eff_addr[31:2], 2'b00};

                case (funct3)
                    // SB
                    3'b000: begin
                        dmem_we = 1'b1;

                        dmem_wdata =
                            (rs2_data & 32'h000000ff)
                            << {eff_addr[1:0], 3'b000};

                        case (eff_addr[1:0])
                            2'b00:
                                dmem_wstrb = 4'b0001;

                            2'b01:
                                dmem_wstrb = 4'b0010;

                            2'b10:
                                dmem_wstrb = 4'b0100;

                            default:
                                dmem_wstrb = 4'b1000;
                        endcase
                    end

                    // SH
                    3'b001: begin
                        if (eff_addr[0] != 1'b0) begin
                            trap = 1'b1;
                            trap_cause = 4'd6;
                            trap_value = eff_addr;
                        end else begin
                            dmem_we = 1'b1;

                            if (eff_addr[1] == 1'b0) begin
                                dmem_wstrb = 4'b0011;
                                dmem_wdata =
                                    rs2_data & 32'h0000ffff;
                            end else begin
                                dmem_wstrb = 4'b1100;
                                dmem_wdata =
                                    (rs2_data & 32'h0000ffff) << 16;
                            end
                        end
                    end

                    // SW
                    3'b010: begin
                        if (eff_addr[1:0] != 2'b00) begin
                            trap = 1'b1;
                            trap_cause = 4'd6;
                            trap_value = eff_addr;
                        end else begin
                            dmem_we = 1'b1;
                            dmem_wstrb = 4'b1111;
                            dmem_wdata = rs2_data;
                        end
                    end

                    default:
                        illegal = 1'b1;
                endcase
            end

            // OP-IMM
            7'b0010011: begin
                case (funct3)
                    // ADDI
                    3'b000: begin
                        rf_we = 1'b1;
                        rd_wdata = rs1_data + imm_i;
                    end

                    // SLTI
                    3'b010: begin
                        rf_we = 1'b1;
                        rd_wdata = {
                            31'b0,
                            ($signed(rs1_data) < $signed(imm_i))
                        };
                    end

                    // SLTIU
                    3'b011: begin
                        rf_we = 1'b1;
                        rd_wdata = {
                            31'b0,
                            (rs1_data < imm_i)
                        };
                    end

                    // XORI
                    3'b100: begin
                        rf_we = 1'b1;
                        rd_wdata = rs1_data ^ imm_i;
                    end

                    // ORI
                    3'b110: begin
                        rf_we = 1'b1;
                        rd_wdata = rs1_data | imm_i;
                    end

                    // ANDI
                    3'b111: begin
                        rf_we = 1'b1;
                        rd_wdata = rs1_data & imm_i;
                    end

                    // SLLI
                    3'b001: begin
                        if (funct7 != 7'b0000000) begin
                            illegal = 1'b1;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata =
                                rs1_data << instr[24:20];
                        end
                    end

                    // SRLI / SRAI
                    3'b101: begin
                        if (funct7 == 7'b0000000) begin
                            rf_we = 1'b1;
                            rd_wdata =
                                rs1_data >> instr[24:20];
                        end else if (funct7 == 7'b0100000) begin
                            rf_we = 1'b1;
                            rd_wdata =
                                $signed(rs1_data)
                                >>> instr[24:20];
                        end else begin
                            illegal = 1'b1;
                        end
                    end

                    default:
                        illegal = 1'b1;
                endcase
            end

            // OP
            7'b0110011: begin
                case (funct3)
                    // ADD / SUB
                    3'b000: begin
                        if (funct7 == 7'b0000000) begin
                            rf_we = 1'b1;
                            rd_wdata =
                                rs1_data + rs2_data;
                        end else if (funct7 == 7'b0100000) begin
                            rf_we = 1'b1;
                            rd_wdata =
                                rs1_data - rs2_data;
                        end else begin
                            illegal = 1'b1;
                        end
                    end

                    // SLL
                    3'b001: begin
                        if (funct7 != 7'b0000000) begin
                            illegal = 1'b1;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata =
                                rs1_data << rs2_data[4:0];
                        end
                    end

                    // SLT
                    3'b010: begin
                        if (funct7 != 7'b0000000) begin
                            illegal = 1'b1;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata = {
                                31'b0,
                                ($signed(rs1_data)
                                    < $signed(rs2_data))
                            };
                        end
                    end

                    // SLTU
                    3'b011: begin
                        if (funct7 != 7'b0000000) begin
                            illegal = 1'b1;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata = {
                                31'b0,
                                (rs1_data < rs2_data)
                            };
                        end
                    end

                    // XOR
                    3'b100: begin
                        if (funct7 != 7'b0000000) begin
                            illegal = 1'b1;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata =
                                rs1_data ^ rs2_data;
                        end
                    end

                    // SRL / SRA
                    3'b101: begin
                        if (funct7 == 7'b0000000) begin
                            rf_we = 1'b1;
                            rd_wdata =
                                rs1_data >> rs2_data[4:0];
                        end else if (funct7 == 7'b0100000) begin
                            rf_we = 1'b1;
                            rd_wdata =
                                $signed(rs1_data)
                                >>> rs2_data[4:0];
                        end else begin
                            illegal = 1'b1;
                        end
                    end

                    // OR
                    3'b110: begin
                        if (funct7 != 7'b0000000) begin
                            illegal = 1'b1;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata =
                                rs1_data | rs2_data;
                        end
                    end

                    // AND
                    3'b111: begin
                        if (funct7 != 7'b0000000) begin
                            illegal = 1'b1;
                        end else begin
                            rf_we = 1'b1;
                            rd_wdata =
                                rs1_data & rs2_data;
                        end
                    end

                    default:
                        illegal = 1'b1;
                endcase
            end

            // FENCE
            // 无缓存、顺序执行的单核中作为 NOP。
            7'b0001111: begin
                if (funct3 != 3'b000)
                    illegal = 1'b1;
            end

            // SYSTEM
            7'b1110011: begin
                // ECALL
                if (instr == 32'h00000073) begin
                    trap = 1'b1;
                    trap_cause = 4'd11;
                    trap_value = 32'b0;

                // EBREAK
                end else if (instr == 32'h00100073) begin
                    trap = 1'b1;
                    trap_cause = 4'd3;
                    trap_value = 32'b0;

                // CSR 属于 Zicsr 扩展，不在 RV32I 核心中。
                end else begin
                    illegal = 1'b1;
                end
            end

            default:
                illegal = 1'b1;
        endcase
    end

    // 非法指令异常。
    if (illegal) begin
        trap = 1'b1;
        trap_cause = 4'd2;
        trap_value = instr;
    end

    // trap 由外部异常控制器接管。
    // 当前指令不退休，不产生寄存器或存储器副作用。
    if (trap) begin
        pc_next = pc;
        pc_we = 1'b0;
        retire = 1'b0;

        rf_we = 1'b0;

        dmem_re = 1'b0;
        dmem_we = 1'b0;
        dmem_wstrb = 4'b0000;
    end else begin
        // 即使外部寄存器堆未特殊处理，也不允许写 x0。
        if (rd_addr == 5'b00000)
            rf_we = 1'b0;
    end
end

endmodule