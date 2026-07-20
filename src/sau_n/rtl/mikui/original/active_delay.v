`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 2024/09/27 14:55:57
// Design Name: 
// Module Name: active_delay
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


module active_delay#
(   parameter                           ROW                       = 1 ,
    parameter                           INPUTDW                   = 8

)
(
    input  wire                         clk                        ,
    input  wire                         rst_n                      ,
    input  wire signed [INPUTDW-1: 0]   active_i                   ,
    input  wire                         EN                         ,
    output reg  signed [INPUTDW-1: 0]   active_o
    );
    wire        signed [INPUTDW-1: 0]        active_o_tmp          ;//left data case
    always @(posedge clk)begin
        active_o <= active_o_tmp;
    end
    generate if (ROW==0) begin
        assign  active_o_tmp              = active_i;
    end else begin
        reg                [ROW*INPUTDW-1: 0]active_delay          ;//left data delay
        wire               [(ROW+1)*INPUTDW-1: 0]active_delay_result   ;//left data delay
        assign  active_o_tmp              = EN ? active_delay[((ROW)*INPUTDW-1) -: INPUTDW] : active_i;
        assign  active_delay_result       = {active_delay[(ROW)*INPUTDW-1:0],active_i};
        always @(posedge clk)
        begin
            if(EN)
                active_delay<=active_delay_result[ROW*INPUTDW-1:0];
            else
                active_delay<='d0;
        end
    end
    endgenerate
endmodule
