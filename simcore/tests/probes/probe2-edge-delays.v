/* Per-edge delays on a gate primitive, and a tristate driver.

   `nand #(8,12)` is 8 ticks to rise and 12 to fall, which is why a timing arc
   carries two numbers instead of one: with both inputs high from t=0 the
   output rises at t=8, then falls 12 ticks after `a` rises at t=20 and rises
   again 8 ticks after `a` falls.  `y4` shows a driver that turns off. */
`timescale 1ps/1ps
module tb;
  reg a, b, en;
  wire y1, y2, y4;
  assign #10 y1 = a;
  nand #(8,12) g0 (y2, a, b);
  assign y4 = en ? 1'bz : 1'b0;
  initial begin
    $dumpfile("probe2.vcd"); $dumpvars(0, tb);
    a = 1'b0; b = 1'b1; en = 1'b0;
    $display("t=%0t y1=%b y2=%b y4=%b", $time, y1, y2, y4);
    #1 $display("t=%0t y1=%b y2=%b y4=%b", $time, y1, y2, y4);
    #19 a = 1'b1;   /* both inputs high: the nand has to fall */
    #14 $display("t=%0t y2=%b (fell 12 ticks after a rose)", $time, y2);
    a = 1'b0;
    #9 $display("t=%0t y2=%b (rose 8 ticks after a fell)", $time, y2);
    en = 1'b1; #5 $display("t=%0t y4=%b (driver off)", $time, y4);
    $finish;
  end
endmodule
