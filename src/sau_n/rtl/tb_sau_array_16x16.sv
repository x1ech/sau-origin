`timescale 1ns/1ps

module tb_sau_array_16x16;
    localparam int SIZE = 16;
    localparam int INPUT_W = 8;
    localparam int ACC_W = 24;
    localparam int OUTPUT_W = 16;
    localparam int WATCHDOG = 4000;

    logic clk = 1'b0;
    logic rst_n = 1'b0;
    logic start = 1'b0;
    logic start_ready;
    logic [10:0] cfg_k;
    logic [4:0] cfg_rows;
    logic [4:0] cfg_cols;
    logic [4:0] cfg_cutbit;
    logic [SIZE*16-1:0] cfg_bias;
    logic input_valid = 1'b0;
    logic input_ready;
    logic [SIZE*INPUT_W-1:0] activation_data = '0;
    logic [SIZE*INPUT_W-1:0] weight_data = '0;
    logic output_valid;
    logic output_ready = 1'b1;
    logic [4:0] output_row;
    logic [SIZE*OUTPUT_W-1:0] output_data;
    logic busy;
    logic done;
    logic [2:0] debug_state;
    logic [SIZE*SIZE-1:0] debug_mac_commit_mask;
    logic [SIZE*SIZE-1:0] debug_add_commit_mask;
    logic [SIZE*SIZE*INPUT_W-1:0] debug_activation_packed;
    logic [SIZE*SIZE*INPUT_W-1:0] debug_weight_packed;
    logic [SIZE*SIZE*ACC_W-1:0] debug_acc_packed;

    string case_name;
    string trace_path;
    integer trace_fd;
    integer trace_cycle = 0;
    integer watchdog = 0;
    integer k_cycles;
    integer requested_rows;
    integer requested_cols;
    integer drive_backpressure;

    wire input_fire = input_valid && input_ready;
    wire output_fire = output_valid && output_ready;

    always #5 clk = ~clk;

    sau_array_16x16 dut (
        .clk(clk),
        .rst_n(rst_n),
        .start(start),
        .start_ready(start_ready),
        .cfg_k(cfg_k),
        .cfg_rows(cfg_rows),
        .cfg_cols(cfg_cols),
        .cfg_cutbit(cfg_cutbit),
        .cfg_bias(cfg_bias),
        .input_valid(input_valid),
        .input_ready(input_ready),
        .activation_data(activation_data),
        .weight_data(weight_data),
        .output_valid(output_valid),
        .output_ready(output_ready),
        .output_row(output_row),
        .output_data(output_data),
        .busy(busy),
        .done(done),
        .debug_state(debug_state),
        .debug_mac_commit_mask(debug_mac_commit_mask),
        .debug_add_commit_mask(debug_add_commit_mask),
        .debug_activation_packed(debug_activation_packed),
        .debug_weight_packed(debug_weight_packed),
        .debug_acc_packed(debug_acc_packed)
    );

    task automatic fail(input string message);
        begin
            $display("FAIL tb_sau_array_16x16 case=%s: %s", case_name, message);
            if (trace_fd != 0)
                $fclose(trace_fd);
            $fatal(1);
        end
    endtask

    task automatic configure_case;
        integer c;
        integer bias_value;
        begin
            requested_rows = 1;
            requested_cols = 1;
            k_cycles = 9;
            cfg_cutbit = 0;
            drive_backpressure = 0;

            if (case_name == "tail_r1_c1_k9") begin
                requested_rows = 1;
                requested_cols = 1;
            end else if (case_name == "tail_r15_c15_k9") begin
                requested_rows = 15;
                requested_cols = 15;
                cfg_cutbit = 5;
            end else if (case_name == "full_r16_c16_k9") begin
                requested_rows = 16;
                requested_cols = 16;
                cfg_cutbit = 5;
            end else if (case_name == "backpressure_r3_c3_k9") begin
                requested_rows = 3;
                requested_cols = 3;
                cfg_cutbit = 2;
                drive_backpressure = 1;
            end else if (case_name == "sat_pos_r1_c1_k567") begin
                k_cycles = 567;
                cfg_cutbit = 16;
            end else if (case_name == "sat_neg_r1_c1_k567") begin
                k_cycles = 567;
                cfg_cutbit = 16;
            end else begin
                fail($sformatf("unknown case %s", case_name));
            end

            cfg_k = k_cycles;
            cfg_rows = requested_rows;
            cfg_cols = requested_cols;
            cfg_bias = '0;
            for (c = 0; c < SIZE; c = c + 1) begin
                if (case_name == "sat_pos_r1_c1_k567" ||
                    case_name == "sat_neg_r1_c1_k567")
                    bias_value = 0;
                else
                    bias_value = c - 8;
                cfg_bias[c*16 +: 16] = bias_value[15:0];
            end
        end
    endtask

    task automatic drive_payload;
        integer r;
        integer c;
        integer activation_value;
        integer weight_value;
        begin
            activation_data = '0;
            weight_data = '0;
            for (r = 0; r < SIZE; r = r + 1) begin
                if (case_name == "sat_pos_r1_c1_k567" ||
                    case_name == "sat_neg_r1_c1_k567")
                    activation_value = (r == 0) ? -128 : 0;
                else
                    activation_value = r + 1;
                activation_data[r*INPUT_W +: INPUT_W] =
                    activation_value[INPUT_W-1:0];
            end
            for (c = 0; c < SIZE; c = c + 1) begin
                if (case_name == "sat_pos_r1_c1_k567")
                    weight_value = (c == 0) ? -128 : 0;
                else if (case_name == "sat_neg_r1_c1_k567")
                    weight_value = (c == 0) ? 127 : 0;
                else
                    weight_value = c + 1;
                weight_data[c*INPUT_W +: INPUT_W] =
                    weight_value[INPUT_W-1:0];
            end
        end
    endtask

    always @(negedge clk) begin
        #1;
        if (rst_n) begin
            $fwrite(trace_fd,
                "%0d,%s,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%032h,%032h,%0d,%0d,%0d,%064h,%064h,%01536h\n",
                trace_cycle, case_name, debug_state, start, busy,
                input_valid, input_ready, input_fire, output_valid,
                output_ready, activation_data, weight_data, output_fire,
                output_row, done, output_data, debug_mac_commit_mask,
                debug_acc_packed);
            trace_cycle = trace_cycle + 1;
        end

        if (drive_backpressure && output_valid)
            output_ready = ((trace_cycle % 5) == 0);
        else
            output_ready = 1'b1;
    end

    always @(posedge clk) begin
        if (rst_n) begin
            watchdog = watchdog + 1;
            if (watchdog > WATCHDOG)
                fail("watchdog expired");
        end
    end

    initial begin
        trace_fd = 0;
        if (!$value$plusargs("CASE=%s", case_name))
            case_name = "tail_r1_c1_k9";
        if (!$value$plusargs("TRACE=%s", trace_path))
            trace_path = "sau_array_trace.csv";

        trace_fd = $fopen(trace_path, "w");
        if (trace_fd == 0)
            fail($sformatf("cannot open trace %s", trace_path));
        $fwrite(trace_fd,
            "cycle,case_name,state,start,busy,input_valid,input_ready,input_fire,output_valid,output_ready,activation_bus,weight_bus,output_fire,output_row,done,output_bus,mac_commit_mask,acc_packed\n");

        configure_case();
        repeat (5) @(negedge clk);
        rst_n = 1'b1;
        @(negedge clk);
        if (!start_ready)
            fail("start_ready low after reset");
        start = 1'b1;
        @(negedge clk);
        start = 1'b0;

        for (integer k = 0; k < k_cycles; k = k + 1) begin
            drive_payload();
            input_valid = 1'b1;
            @(negedge clk);
            if (k + 1 < k_cycles && !input_ready)
                fail($sformatf("input_ready dropped before item %0d", k + 1));
        end
        input_valid = 1'b0;
        activation_data = '0;
        weight_data = '0;

        while (!done)
            @(negedge clk);
        repeat (2) @(negedge clk);

        $fclose(trace_fd);
        trace_fd = 0;
        $display("PASS tb_sau_array_16x16 case=%s trace=%s",
                 case_name, trace_path);
        $finish;
    end
endmodule
