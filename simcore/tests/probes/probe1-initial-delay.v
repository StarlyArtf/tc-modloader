/* Does the first assignment take the delay?

   `a` is 0 from t=0 and `y` is `a` through an inertial #5 assignment.  The
   answer decides whether the engine may skip the delay for a device's first
   evaluation: it may not - y stays x until t=5.  Also shows the narrow-pulse
   case at t=106: `a` returns low 3 ticks later, so the 1 never arrives. */
`timescale 1ps/1ps
module tb;
  reg a;
  wire y;
  assign #5 y = a;
  initial begin
    $dumpfile("probe1.vcd"); $dumpvars(0, tb);
    a = 0;
    $display("t=%0t y=%b", $time, y);
    #1  $display("t=%0t y=%b", $time, y);
    #4  $display("t=%0t y=%b", $time, y);
    #1  $display("t=%0t y=%b", $time, y);
    #100 a = 1; #3 a = 0;      /* pulse narrower than the delay */
    #20 $display("t=%0t y=%b after a 3 tick pulse", $time, y);
    #20 a = 1; #20 a = 0;      /* pulse wider than the delay */
    #20 $display("t=%0t y=%b after a 20 tick pulse", $time, y);
    $finish;
  end
endmodule
