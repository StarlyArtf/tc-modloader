/* How a multi-bit net picks between the rise and the fall delay.

   `assign #(5,9)` on a 1-bit net is unambiguous - 5 for a transition to 1, 9
   for a transition to 0 - but the standard has more to say about a vector, and
   simcore's engine picks the delay from the *top* bit of the new value while
   iverilog does not.  This probe sweeps all sixteen known values of a 4-bit
   net and prints when the source changed and when the net followed, so the
   choice can be read off instead of guessed.

   Icarus Verilog 13.0: for a value whose bits are all known the *lowest* bit
   decides - odd values take the 5, even values the 9 - and a value containing
   x or z follows no rule this probe can state (`1111 -> xxxx` and
   `0000 -> zzzz` arrive after 5, but `1111 -> 0z0z` after 9).  That is why
   every bus device is cross-checked with one delay for both edges (see
   simcore/tests/circuits.hpp, the mux_word fixture): until the game's own
   timing table says which of the two a vector really takes, the difference is
   recorded here rather than copied into the engine. */
`timescale 1ps/1ps
module tb;
  reg [3:0] src;
  wire [3:0] dst;
  integer index;
  assign #(5,9) dst = src;
  initial begin
    $dumpfile("probe4-vector-delays.vcd");
    $dumpvars(0, tb);
    $monitor("t=%0t src=%b dst=%b", $time, src, dst);
    src = 4'b0000; #20;
    for (index = 0; index < 16; index = index + 1) begin
      src = index[3:0];
      #20;
    end
    src = 4'bxxxx; #20;
    src = 4'b0000; #20;
    src = 4'bzzzz; #20;
    src = 4'b1111; #20;
    src = 4'b0z0z; #20;
    $finish;
  end
endmodule
