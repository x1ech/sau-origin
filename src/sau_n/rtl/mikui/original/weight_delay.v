`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 2024/05/19 04:05:36
// Design Name: 
// Module Name: weight_delay
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////


module weight_delay#(
    parameter                           COL                       = 1     ,
    parameter                           ROW                       = 1     ,
    parameter                           DATA_WIDTH                = 8     
)
    (
    input  wire                          clk                        ,
    input  wire                          rst_n                      ,
    input  wire  signed [DATA_WIDTH-1: 0]weight_i                   ,
    input  wire                          EN                         ,
    output reg   signed [DATA_WIDTH-1: 0]weight_o                    
    );

    
    wire    signed     [DATA_WIDTH-1: 0]weight_o_tmp;
    generate if (COL==0) begin
        assign  weight_o_tmp                  = weight_i;
    end else begin
        reg                [(COL)*DATA_WIDTH-1: 0] weight_delay;
        wire               [(COL+1)*DATA_WIDTH-1: 0] weight_delay_result;
        assign  weight_o_tmp              = EN ? weight_delay[((COL)*DATA_WIDTH-1) -: DATA_WIDTH] : weight_i;
        assign  weight_delay_result       = {weight_delay[(COL)*DATA_WIDTH-1:0],weight_i};
        always @(posedge clk)begin
            if(EN && COL>=1)
                weight_delay<=weight_delay_result[COL*DATA_WIDTH-1:0];
            else
                weight_delay<='d0;
        end    
    end
    endgenerate
    always @(posedge clk)begin
            weight_o <= weight_o_tmp;
    end
endmodule
