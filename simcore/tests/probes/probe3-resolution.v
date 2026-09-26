/* Multi-driver resolution, unknown inputs, and the ring's period.

   m1 carries two drivers that disagree and then agree; m2 puts a weak driver
   against a strong one; gz/g0x/g1x are gates fed with z, 0 and x; and the
   ring is three inverting stages of 10 ticks each, released by `rst` at
   t=120, so its period is 6 * 10 = 60. */
`timescale 1ps/1ps
module tb;
  reg a, b, w0, w1, rst;
  wire m1, m2, gz, g0x, g1x, r1, r2, r3;
  assign #0 m1 = a;
  assign #0 m1 = b;
  assign (weak1, weak0) m2 = w0;
  assign (strong1, strong0) m2 = w1;
  nand #(5,5) nz (gz, 1'bz, 1'b1);
  and  #(5,5) a0 (g0x, 1'b0, 1'bx);
  and  #(5,5) a1 (g1x, 1'b1, 1'bx);
  nand #(10,10) n1 (r1, r3, rst);
  not  #(10,10) n2 (r2, r1);
  not  #(10,10) n3 (r3, r2);
  initial begin
    $dumpfile("probe3.vcd"); $dumpvars(0, tb);
    a=1'b0; b=1'b1; w0=1'b0; w1=1'b1; rst=1'b0;
    #60; $display("t=%0t m1=%b (two drivers disagreeing) m2=%b (weak 0 vs strong 1) gz=%b g0x=%b g1x=%b r1=%b r2=%b r3=%b",
                  $time, m1, m2, gz, g0x, g1x, r1, r2, r3);
    b=1'b0;
    #60; $display("t=%0t m1=%b (the drivers now agree)", $time, m1);
    rst=1'b1;   /* release the ring */
    #11; $display("t=%0t r1=%b r2=%b r3=%b", $time, r1, r2, r3);
    #49; $display("t=%0t r1=%b r2=%b r3=%b", $time, r1, r2, r3);
    #60; $display("t=%0t r1=%b r2=%b r3=%b (one period later)", $time, r1, r2, r3);
    $finish;
  end
endmodule
