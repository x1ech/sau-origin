`timescale 1ns/1ps

module sau_array_16x16 #(
    parameter int SIZE = 16,
    parameter int INPUT_W = 8,
    parameter int ACC_W = 24,
    parameter int BIAS_W = 16,
    parameter int OUTPUT_W = 16,
    parameter int K_W = 11
) (
    input  logic                         clk,
    input  logic                         rst_n,

    input  logic                         start,
    output logic                         start_ready,
    input  logic [K_W-1:0]               cfg_k,
    input  logic [$clog2(SIZE):0]        cfg_rows,
    input  logic [$clog2(SIZE):0]        cfg_cols,
    input  logic [4:0]                   cfg_cutbit,
    input  logic [SIZE*BIAS_W-1:0]       cfg_bias,

    input  logic                         input_valid,
    output logic                         input_ready,
    input  logic [SIZE*INPUT_W-1:0]      activation_data,
    input  logic [SIZE*INPUT_W-1:0]      weight_data,

    output logic                         output_valid,
    input  logic                         output_ready,
    output logic [$clog2(SIZE):0]        output_row,
    output logic [SIZE*OUTPUT_W-1:0]     output_data,

    output logic                         busy,
    output logic                         done,

    output logic [2:0]                   debug_state,
    output logic [SIZE*SIZE-1:0]         debug_mac_commit_mask,
    output logic [SIZE*SIZE-1:0]         debug_add_commit_mask,
    output logic [SIZE*SIZE*INPUT_W-1:0] debug_activation_packed,
    output logic [SIZE*SIZE*INPUT_W-1:0] debug_weight_packed,
    output logic [SIZE*SIZE*ACC_W-1:0]   debug_acc_packed
);

    localparam int DRAIN_CYCLES = 2 * (SIZE - 1) + 1;
    localparam logic signed [ACC_W-1:0] ACC_MAX =
        {1'b0, {(ACC_W-1){1'b1}}};
    localparam logic signed [ACC_W-1:0] ACC_MIN =
        {1'b1, {(ACC_W-1){1'b0}}};

    typedef enum logic [2:0] {
        ST_IDLE,
        ST_STREAM,
        ST_DRAIN,
        ST_BIAS,
        ST_OUTPUT
    } state_e;

    state_e state;
    logic [K_W-1:0] k_q;
    logic [$clog2(SIZE):0] rows_q;
    logic [$clog2(SIZE):0] cols_q;
    logic [4:0] cutbit_q;
    logic [SIZE*BIAS_W-1:0] bias_q;
    logic [K_W-1:0] input_count;
    logic [$clog2(DRAIN_CYCLES+1)-1:0] drain_count;
    logic [$clog2(SIZE):0] output_row_q;

    logic signed [INPUT_W-1:0] activation_skew [0:SIZE-1][0:SIZE-1];
    logic signed [INPUT_W-1:0] weight_skew [0:SIZE-1][0:SIZE-1];
    logic activation_skew_valid [0:SIZE-1][0:SIZE-1];
    logic weight_skew_valid [0:SIZE-1][0:SIZE-1];

    logic signed [INPUT_W-1:0] activation_pipe [0:SIZE-1][0:SIZE-1];
    logic signed [INPUT_W-1:0] weight_pipe [0:SIZE-1][0:SIZE-1];
    logic activation_pipe_valid [0:SIZE-1][0:SIZE-1];
    logic weight_pipe_valid [0:SIZE-1][0:SIZE-1];
    logic signed [ACC_W-1:0] accumulator [0:SIZE-1][0:SIZE-1];
    logic mac_commit_q [0:SIZE-1][0:SIZE-1];
    logic add_commit_q [0:SIZE-1][0:SIZE-1];
    logic signed [INPUT_W-1:0] mac_activation_q [0:SIZE-1][0:SIZE-1];
    logic signed [INPUT_W-1:0] mac_weight_q [0:SIZE-1][0:SIZE-1];

    wire start_accept = start && start_ready;
    wire input_fire = input_valid && input_ready;
    wire output_fire = output_valid && output_ready;

    function automatic logic signed [ACC_W-1:0] saturating_add(
        input logic signed [ACC_W-1:0] lhs,
        input logic signed [ACC_W-1:0] rhs
    );
        logic signed [ACC_W:0] extended_sum;
        begin
            extended_sum = {lhs[ACC_W-1], lhs} + {rhs[ACC_W-1], rhs};
            if (extended_sum > $signed({1'b0, ACC_MAX}))
                saturating_add = ACC_MAX;
            else if (extended_sum < $signed({1'b1, ACC_MIN}))
                saturating_add = ACC_MIN;
            else
                saturating_add = extended_sum[ACC_W-1:0];
        end
    endfunction

    function automatic logic signed [OUTPUT_W-1:0] quantize_int8(
        input logic signed [ACC_W-1:0] value,
        input logic [4:0] shift
    );
        logic signed [ACC_W-1:0] shifted;
        begin
            shifted = value >>> shift;
            if (shifted > 127)
                quantize_int8 = $signed(16'sd127);
            else if (shifted < -128)
                quantize_int8 = $signed(-16'sd128);
            else
                quantize_int8 = {{(OUTPUT_W-8){shifted[7]}}, shifted[7:0]};
        end
    endfunction

    assign start_ready = (state == ST_IDLE);
    assign input_ready = (state == ST_STREAM);
    assign output_valid = (state == ST_OUTPUT);
    assign output_row = output_row_q;
    assign busy = (state != ST_IDLE);
    assign debug_state = state;

    always_comb begin
        output_data = '0;
        if (state == ST_OUTPUT && output_row_q < rows_q) begin
            for (int c = 0; c < SIZE; c++) begin
                if (c < cols_q)
                    output_data[c*OUTPUT_W +: OUTPUT_W] =
                        quantize_int8(accumulator[output_row_q][c], cutbit_q);
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= ST_IDLE;
            k_q <= '0;
            rows_q <= '0;
            cols_q <= '0;
            cutbit_q <= '0;
            bias_q <= '0;
            input_count <= '0;
            drain_count <= '0;
            output_row_q <= '0;
            done <= 1'b0;
        end else begin
            done <= 1'b0;
            case (state)
                ST_IDLE: begin
                    if (start_accept) begin
                        k_q <= cfg_k;
                        rows_q <= cfg_rows;
                        cols_q <= cfg_cols;
                        cutbit_q <= cfg_cutbit;
                        bias_q <= cfg_bias;
                        input_count <= '0;
                        drain_count <= '0;
                        output_row_q <= '0;
                        state <= ST_STREAM;
                    end
                end
                ST_STREAM: begin
                    if (input_fire) begin
                        if (input_count == k_q - 1'b1) begin
                            drain_count <= DRAIN_CYCLES;
                            state <= ST_DRAIN;
                        end else begin
                            input_count <= input_count + 1'b1;
                        end
                    end
                end
                ST_DRAIN: begin
                    if (drain_count == 1) begin
                        drain_count <= '0;
                        state <= ST_BIAS;
                    end else begin
                        drain_count <= drain_count - 1'b1;
                    end
                end
                ST_BIAS: begin
                    output_row_q <= '0;
                    state <= ST_OUTPUT;
                end
                ST_OUTPUT: begin
                    if (output_fire) begin
                        if (output_row_q == rows_q - 1'b1) begin
                            done <= 1'b1;
                            state <= ST_IDLE;
                        end else begin
                            output_row_q <= output_row_q + 1'b1;
                        end
                    end
                end
                default: state <= ST_IDLE;
            endcase
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int lane = 0; lane < SIZE; lane++) begin
                for (int stage = 0; stage < SIZE; stage++) begin
                    activation_skew[lane][stage] <= '0;
                    weight_skew[lane][stage] <= '0;
                    activation_skew_valid[lane][stage] <= 1'b0;
                    weight_skew_valid[lane][stage] <= 1'b0;
                end
            end
        end else if (start_accept) begin
            for (int lane = 0; lane < SIZE; lane++) begin
                for (int stage = 0; stage < SIZE; stage++) begin
                    activation_skew[lane][stage] <= '0;
                    weight_skew[lane][stage] <= '0;
                    activation_skew_valid[lane][stage] <= 1'b0;
                    weight_skew_valid[lane][stage] <= 1'b0;
                end
            end
        end else if (state == ST_STREAM || state == ST_DRAIN) begin
            for (int lane = 0; lane < SIZE; lane++) begin
                activation_skew[lane][0] <= input_fire ?
                    $signed(activation_data[lane*INPUT_W +: INPUT_W]) : '0;
                weight_skew[lane][0] <= input_fire ?
                    $signed(weight_data[lane*INPUT_W +: INPUT_W]) : '0;
                activation_skew_valid[lane][0] <= input_fire;
                weight_skew_valid[lane][0] <= input_fire;
                for (int stage = 1; stage < SIZE; stage++) begin
                    activation_skew[lane][stage] <=
                        activation_skew[lane][stage-1];
                    weight_skew[lane][stage] <= weight_skew[lane][stage-1];
                    activation_skew_valid[lane][stage] <=
                        activation_skew_valid[lane][stage-1];
                    weight_skew_valid[lane][stage] <=
                        weight_skew_valid[lane][stage-1];
                end
            end
        end else begin
            for (int lane = 0; lane < SIZE; lane++) begin
                for (int stage = 0; stage < SIZE; stage++) begin
                    activation_skew_valid[lane][stage] <= 1'b0;
                    weight_skew_valid[lane][stage] <= 1'b0;
                end
            end
        end
    end

    generate
        for (genvar r = 0; r < SIZE; r++) begin : GEN_ROW
            for (genvar c = 0; c < SIZE; c++) begin : GEN_COL
                localparam int PE_INDEX = r * SIZE + c;
                wire signed [INPUT_W-1:0] pe_activation;
                wire signed [INPUT_W-1:0] pe_weight;
                wire pe_activation_valid;
                wire pe_weight_valid;

                if (c == 0) begin : GEN_ACTIVATION_BOUNDARY
                    assign pe_activation = activation_skew[r][r];
                    assign pe_activation_valid = activation_skew_valid[r][r];
                end else begin : GEN_ACTIVATION_NEIGHBOR
                    assign pe_activation = activation_pipe[r][c-1];
                    assign pe_activation_valid = activation_pipe_valid[r][c-1];
                end
                if (r == 0) begin : GEN_WEIGHT_BOUNDARY
                    assign pe_weight = weight_skew[c][c];
                    assign pe_weight_valid = weight_skew_valid[c][c];
                end else begin : GEN_WEIGHT_NEIGHBOR
                    assign pe_weight = weight_pipe[r-1][c];
                    assign pe_weight_valid = weight_pipe_valid[r-1][c];
                end
                wire signed [2*INPUT_W-1:0] pe_product =
                    pe_activation * pe_weight;
                wire signed [ACC_W-1:0] pe_product_extended =
                    {{(ACC_W-2*INPUT_W){pe_product[2*INPUT_W-1]}}, pe_product};
                wire signed [BIAS_W-1:0] pe_bias =
                    bias_q[c*BIAS_W +: BIAS_W];
                wire signed [ACC_W-1:0] pe_bias_extended =
                    {{(ACC_W-BIAS_W){pe_bias[BIAS_W-1]}}, pe_bias};
                wire pe_mac_fire =
                    (state == ST_STREAM || state == ST_DRAIN) &&
                    pe_activation_valid && pe_weight_valid &&
                    (r < rows_q) && (c < cols_q);

                assign debug_mac_commit_mask[PE_INDEX] = mac_commit_q[r][c];
                assign debug_add_commit_mask[PE_INDEX] = add_commit_q[r][c];
                assign debug_activation_packed[PE_INDEX*INPUT_W +: INPUT_W] =
                    mac_commit_q[r][c] ? mac_activation_q[r][c] : '0;
                assign debug_weight_packed[PE_INDEX*INPUT_W +: INPUT_W] =
                    mac_commit_q[r][c] ? mac_weight_q[r][c] : '0;
                assign debug_acc_packed[PE_INDEX*ACC_W +: ACC_W] =
                    accumulator[r][c];

                always_ff @(posedge clk or negedge rst_n) begin
                    if (!rst_n) begin
                        activation_pipe[r][c] <= '0;
                        weight_pipe[r][c] <= '0;
                        activation_pipe_valid[r][c] <= 1'b0;
                        weight_pipe_valid[r][c] <= 1'b0;
                        accumulator[r][c] <= '0;
                        mac_commit_q[r][c] <= 1'b0;
                        add_commit_q[r][c] <= 1'b0;
                        mac_activation_q[r][c] <= '0;
                        mac_weight_q[r][c] <= '0;
                    end else if (start_accept) begin
                        activation_pipe[r][c] <= '0;
                        weight_pipe[r][c] <= '0;
                        activation_pipe_valid[r][c] <= 1'b0;
                        weight_pipe_valid[r][c] <= 1'b0;
                        accumulator[r][c] <= '0;
                        mac_commit_q[r][c] <= 1'b0;
                        add_commit_q[r][c] <= 1'b0;
                        mac_activation_q[r][c] <= '0;
                        mac_weight_q[r][c] <= '0;
                    end else if (state == ST_STREAM || state == ST_DRAIN) begin
                        activation_pipe[r][c] <= pe_activation;
                        weight_pipe[r][c] <= pe_weight;
                        activation_pipe_valid[r][c] <= pe_activation_valid;
                        weight_pipe_valid[r][c] <= pe_weight_valid;
                        mac_commit_q[r][c] <= pe_mac_fire;
                        add_commit_q[r][c] <= 1'b0;
                        if (pe_mac_fire) begin
                            mac_activation_q[r][c] <= pe_activation;
                            mac_weight_q[r][c] <= pe_weight;
                            accumulator[r][c] <= saturating_add(
                                accumulator[r][c], pe_product_extended);
                        end
                    end else begin
                        activation_pipe_valid[r][c] <= 1'b0;
                        weight_pipe_valid[r][c] <= 1'b0;
                        mac_commit_q[r][c] <= 1'b0;
                        add_commit_q[r][c] <=
                            state == ST_BIAS && r < rows_q && c < cols_q;
                        if (state == ST_BIAS && r < rows_q && c < cols_q)
                            accumulator[r][c] <= saturating_add(
                                accumulator[r][c], pe_bias_extended);
                    end
                end
            end
        end
    endgenerate

endmodule
