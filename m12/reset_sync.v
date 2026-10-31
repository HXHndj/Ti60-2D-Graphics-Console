// Only the assertion is asynchronous. Release requires two destination clocks.
module reset_sync(input wire clk, input wire arst_n, output wire rst_n);
    (* async_reg = "true" *) reg [1:0] ff;
    always @(posedge clk or negedge arst_n)
        if (!arst_n) ff <= 0; else ff <= {ff[0], 1'b1};
    assign rst_n = ff[1];
endmodule
