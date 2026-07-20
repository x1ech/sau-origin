`timescale 1ns/1ps

module tb_im2col_mikui_sau_pipeline;
    localparam int ROWS = 16;
    localparam int COLS = 16;
    localparam int SP_BANK_ENTRIES = 4096;
    localparam int MAX_OUTPUTS = 65536;
    localparam int WATCHDOG_CYCLES = 200000;

    logic clk = 1'b0;
    logic rst_n = 1'b0;
    logic cfg_valid = 1'b0;
    logic start = 1'b0;
    logic [15:0] cfg_n, cfg_c, cfg_h, cfg_w;
    logic [15:0] cfg_out_h, cfg_out_w;
    logic [3:0] cfg_kernel_h, cfg_kernel_w;
    logic [3:0] cfg_stride_h, cfg_stride_w;
    logic [3:0] cfg_dilation_h, cfg_dilation_w;
    logic [15:0] cfg_pad_top, cfg_pad_left, cfg_spad_base;
    logic [15:0] cfg_out_channels;
    logic [4:0] cfg_cutbit;
    logic [1:0] cfg_weight_generator;
    logic cfg_bias_zero;
    logic [31:0] cfg_expected_tiles;
    logic output_grant = 1'b0;

    logic [15:0] sram_req_valid;
    logic [15:0][11:0] sram_req_addr;
    logic [15:0] sram_resp_valid;
    logic [15:0][7:0] sram_resp_data;
    logic [7:0] spad_mem [0:15][0:SP_BANK_ENTRIES-1];

    logic [2:0] pipeline_state;
    logic [31:0] tile_index;
    logic [15:0] tile_buffer_count, collect_k, stream_k;
    logic im2col_feed_ready, output_request, drained;
    logic sa_ins_valid, sa_input_valid;
    logic [10:0] sa_calc_cycles;
    logic [4:0] sa_valid_rows, sa_valid_columns, sa_cutbit;
    logic [255:0] sa_biases;
    logic [127:0] sa_activations, sa_weights;
    logic [15:0] sa_row_mask, sa_column_mask;
    logic [255:0] pe_valid_mask;
    logic [255:0] pe_mac_commit_mask, pe_add_commit_mask;
    logic [2047:0] pe_activations, pe_weights;
    logic [6143:0] pe_accumulators;
    logic [15:0] row_ready_mask;
    logic [255:0] output_slots;
    logic row_score_valid;
    logic [4:0] row_sequence;
    logic storage_ready, pe_finish, cal_finish;

    string fixture_name;
    string trace_path;
    string output_path;
    string resolved_config_sha256;
    integer trace_fd;
    integer output_fd;
    integer output_ready_period = 1;
    integer output_ready_high_cycles = 1;
    integer expected_outputs;
    integer trace_cycle = 0;
    integer watchdog_cycle;
    integer output_elements;
    integer im2col_done_cycle;
    integer sau_last_result_cycle;
    integer output_values [0:MAX_OUTPUTS-1];
    bit output_written [0:MAX_OUTPUTS-1];

    im2col_mikui_sau_pipeline dut (
        .clk(clk),
        .rst_n(rst_n),
        .cfg_valid(cfg_valid),
        .start(start),
        .cfg_n(cfg_n),
        .cfg_c(cfg_c),
        .cfg_h(cfg_h),
        .cfg_w(cfg_w),
        .cfg_out_h(cfg_out_h),
        .cfg_out_w(cfg_out_w),
        .cfg_kernel_h(cfg_kernel_h),
        .cfg_kernel_w(cfg_kernel_w),
        .cfg_stride_h(cfg_stride_h),
        .cfg_stride_w(cfg_stride_w),
        .cfg_dilation_h(cfg_dilation_h),
        .cfg_dilation_w(cfg_dilation_w),
        .cfg_pad_top(cfg_pad_top),
        .cfg_pad_left(cfg_pad_left),
        .cfg_spad_base(cfg_spad_base),
        .cfg_out_channels(cfg_out_channels),
        .cfg_cutbit(cfg_cutbit),
        .cfg_weight_generator(cfg_weight_generator),
        .cfg_bias_zero(cfg_bias_zero),
        .cfg_expected_tiles(cfg_expected_tiles),
        .output_grant(output_grant),
        .sram_req_valid(sram_req_valid),
        .sram_req_addr(sram_req_addr),
        .sram_resp_valid(sram_resp_valid),
        .sram_resp_data(sram_resp_data),
        .pipeline_state(pipeline_state),
        .tile_index(tile_index),
        .tile_buffer_count(tile_buffer_count),
        .collect_k(collect_k),
        .stream_k(stream_k),
        .im2col_feed_ready(im2col_feed_ready),
        .output_request(output_request),
        .drained(drained),
        .sa_ins_valid(sa_ins_valid),
        .sa_input_valid(sa_input_valid),
        .sa_calc_cycles(sa_calc_cycles),
        .sa_valid_rows(sa_valid_rows),
        .sa_valid_columns(sa_valid_columns),
        .sa_cutbit(sa_cutbit),
        .sa_biases(sa_biases),
        .sa_activations(sa_activations),
        .sa_weights(sa_weights),
        .sa_row_mask(sa_row_mask),
        .sa_column_mask(sa_column_mask),
        .pe_valid_mask(pe_valid_mask),
        .pe_mac_commit_mask(pe_mac_commit_mask),
        .pe_add_commit_mask(pe_add_commit_mask),
        .pe_activations(pe_activations),
        .pe_weights(pe_weights),
        .pe_accumulators(pe_accumulators),
        .row_ready_mask(row_ready_mask),
        .output_slots(output_slots),
        .row_score_valid(row_score_valid),
        .row_sequence(row_sequence),
        .storage_ready(storage_ready),
        .pe_finish(pe_finish),
        .cal_finish(cal_finish)
    );

    always #5 clk = ~clk;

    always_comb begin
        for (int bank = 0; bank < ROWS; bank++) begin
            sram_resp_valid[bank] = sram_req_valid[bank];
            sram_resp_data[bank] = spad_mem[bank][sram_req_addr[bank]];
        end
    end

    function automatic int ceil_div(input int value, input int divisor);
        ceil_div = (value + divisor - 1) / divisor;
    endfunction

    function automatic int rows_per_word(input int width);
        if (width <= ROWS)
            rows_per_word = ROWS / width;
        else
            rows_per_word = 1;
    endfunction

    function automatic int spatial_words(input int height, input int width);
        if (width <= ROWS)
            spatial_words = ceil_div(height, rows_per_word(width));
        else
            spatial_words = height * ceil_div(width, ROWS);
    endfunction

    function automatic int chw_word_offset(
        input int n,
        input int channel,
        input int height,
        input int width
    );
        if (cfg_w <= ROWS) begin
            chw_word_offset =
                n * cfg_c * spatial_words(cfg_h, cfg_w) +
                channel * spatial_words(cfg_h, cfg_w) +
                height / rows_per_word(cfg_w);
        end else begin
            chw_word_offset =
                n * cfg_c * spatial_words(cfg_h, cfg_w) +
                channel * spatial_words(cfg_h, cfg_w) +
                height * ceil_div(cfg_w, ROWS) + width / ROWS;
        end
    endfunction

    function automatic int chw_lane(input int height, input int width);
        if (cfg_w <= ROWS)
            chw_lane = (height % rows_per_word(cfg_w)) * cfg_w + width;
        else
            chw_lane = width % ROWS;
    endfunction

    function automatic [7:0] activation_value(
        input int n,
        input int channel,
        input int height,
        input int width
    );
        activation_value = n * 97 + channel * 31 + height * 7 + width + 1;
    endfunction

    task automatic fill_scratchpad;
        integer word;
        integer lane;
        integer row;
        begin
            for (int bank = 0; bank < ROWS; bank++)
                for (int address = 0; address < SP_BANK_ENTRIES; address++)
                    spad_mem[bank][address] = '0;
            for (int n = 0; n < cfg_n; n++) begin
                for (int channel = 0; channel < cfg_c; channel++) begin
                    for (int height = 0; height < cfg_h; height++) begin
                        for (int width = 0; width < cfg_w; width++) begin
                            word = chw_word_offset(
                                n, channel, height, width);
                            lane = chw_lane(height, width);
                            row = (cfg_spad_base + word) &
                                (SP_BANK_ENTRIES - 1);
                            spad_mem[lane][row] = activation_value(
                                n, channel, height, width);
                        end
                    end
                end
            end
        end
    endtask

    task automatic require_plusargs;
        integer weight_generator;
        integer bias_zero;
        integer expected_tiles;
        begin
            if (!$value$plusargs("FIXTURE_NAME=%s", fixture_name))
                $fatal(1, "missing +FIXTURE_NAME");
            if (!$value$plusargs("TRACE_FILE=%s", trace_path))
                $fatal(1, "missing +TRACE_FILE");
            if (!$value$plusargs("OUTPUT_FILE=%s", output_path))
                $fatal(1, "missing +OUTPUT_FILE");
            if (!$value$plusargs(
                    "RESOLVED_CONFIG_SHA256=%s", resolved_config_sha256))
                $fatal(1, "missing +RESOLVED_CONFIG_SHA256");
            if (!$value$plusargs("CFG_N=%d", cfg_n) ||
                !$value$plusargs("CFG_C=%d", cfg_c) ||
                !$value$plusargs("CFG_H=%d", cfg_h) ||
                !$value$plusargs("CFG_W=%d", cfg_w) ||
                !$value$plusargs("CFG_OUT_H=%d", cfg_out_h) ||
                !$value$plusargs("CFG_OUT_W=%d", cfg_out_w) ||
                !$value$plusargs("CFG_KERNEL_H=%d", cfg_kernel_h) ||
                !$value$plusargs("CFG_KERNEL_W=%d", cfg_kernel_w) ||
                !$value$plusargs("CFG_STRIDE_H=%d", cfg_stride_h) ||
                !$value$plusargs("CFG_STRIDE_W=%d", cfg_stride_w) ||
                !$value$plusargs("CFG_DILATION_H=%d", cfg_dilation_h) ||
                !$value$plusargs("CFG_DILATION_W=%d", cfg_dilation_w) ||
                !$value$plusargs("CFG_PAD_TOP=%d", cfg_pad_top) ||
                !$value$plusargs("CFG_PAD_LEFT=%d", cfg_pad_left) ||
                !$value$plusargs("CFG_SPAD_BASE=%d", cfg_spad_base) ||
                !$value$plusargs("CFG_OUT_CHANNELS=%d", cfg_out_channels) ||
                !$value$plusargs("CFG_CUTBIT=%d", cfg_cutbit) ||
                !$value$plusargs("CFG_WEIGHT_GENERATOR=%d", weight_generator) ||
                !$value$plusargs("CFG_BIAS_ZERO=%d", bias_zero) ||
                !$value$plusargs("EXPECTED_TILES=%d", expected_tiles) ||
                !$value$plusargs("EXPECTED_OUTPUTS=%d", expected_outputs) ||
                !$value$plusargs(
                    "OUTPUT_READY_PERIOD=%d", output_ready_period) ||
                !$value$plusargs(
                    "OUTPUT_READY_HIGH_CYCLES=%d",
                    output_ready_high_cycles))
                $fatal(1, "missing pipeline configuration plusarg");
            cfg_weight_generator = weight_generator[1:0];
            cfg_bias_zero = bias_zero[0];
            cfg_expected_tiles = expected_tiles;
            if (expected_outputs < 1 || expected_outputs > MAX_OUTPUTS)
                $fatal(1, "EXPECTED_OUTPUTS is outside testbench capacity");
            if (output_ready_period < 1 ||
                output_ready_high_cycles < 1 ||
                output_ready_high_cycles > output_ready_period)
                $fatal(1, "invalid output-ready pattern");
        end
    endtask

    task automatic write_trace_header;
        $fwrite(trace_fd,
            "schema_version,resolved_config_sha256,cycle,pipeline_state,");
        $fwrite(trace_fd,
            "tile_index,tile_buffer_count,collect_k,stream_k,im2col_state,");
        $fwrite(trace_fd,
            "im2col_done,im2col_fifo_count,im2col_fifo_rptr,");
        $fwrite(trace_fd,
            "im2col_fifo_wptr,im2col_feed_valid,im2col_feed_ready,");
        $fwrite(trace_fd,
            "im2col_feed_handshake,im2col_feed_data,im2col_feed_mask,");
        $fwrite(trace_fd,
            "sa_ins_valid,sa_calc_cycles,sa_valid_rows,sa_valid_columns,");
        $fwrite(trace_fd,
            "sa_cutbit,sa_biases,sa_state,sa_datain_count,");
        $fwrite(trace_fd,
            "sa_output_counter,sa_input_valid,sa_activations,sa_weights,");
        $fwrite(trace_fd,
            "sa_row_mask,sa_column_mask,pe_valid_mask,");
        $fwrite(trace_fd,
            "pe_mac_commit_mask,pe_add_commit_mask,pe_activations,");
        $fwrite(trace_fd,
            "pe_weights,pe_accumulators,os_valid_mask,row_ready_mask,");
        $fwrite(trace_fd,
            "pe_finish,storage_ready,output_request,output_grant,");
        $fwrite(trace_fd,
            "internal_output_valid,engine_output_fire,row_score_valid,");
        $fwrite(trace_fd,
            "row_sequence,output_slots,cal_finish,output_collected,");
        $fwrite(trace_fd, "sau_last_result,drained\n");
    endtask

    task automatic write_trace_cycle;
        logic [127:0] feed_data_normalized;
        logic [15:0] feed_mask_normalized;
        logic [255:0] output_normalized;
        logic sau_last_result;
        begin
            feed_data_normalized = dut.im2col_feed_valid ?
                dut.im2col_feed_data : '0;
            feed_mask_normalized = dut.im2col_feed_valid ?
                dut.im2col_feed_mask : '0;
            output_normalized = row_score_valid ? output_slots : '0;
            sau_last_result = row_score_valid &&
                output_elements + cfg_out_channels == expected_outputs;
            $fwrite(trace_fd,
                "1,%s,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,",
                resolved_config_sha256, trace_cycle, pipeline_state,
                tile_index, tile_buffer_count, collect_k, stream_k,
                dut.im2col_dut.state, dut.im2col_done,
                dut.im2col_dut.fifo_count, dut.im2col_dut.fifo_rptr,
                dut.im2col_dut.fifo_wptr);
            $fwrite(trace_fd,
                "%0d,%0d,%0d,0x%032h,0x%04h,",
                dut.im2col_feed_valid, im2col_feed_ready,
                dut.im2col_feed_valid && im2col_feed_ready,
                feed_data_normalized, feed_mask_normalized);
            $fwrite(trace_fd,
                "%0d,%0d,%0d,%0d,%0d,0x%064h,%0d,%0d,%0d,%0d,",
                sa_ins_valid,
                sa_ins_valid ? sa_calc_cycles : 0,
                sa_ins_valid ? sa_valid_rows : 0,
                sa_ins_valid ? sa_valid_columns : 0,
                sa_ins_valid ? sa_cutbit : 0,
                sa_ins_valid ? sa_biases : 256'd0,
                dut.array_debug_state, dut.sa_datain_count,
                dut.sa_output_counter, sa_input_valid);
            $fwrite(trace_fd,
                "0x%032h,0x%032h,0x%04h,0x%04h,",
                sa_input_valid ? sa_activations : 128'd0,
                sa_input_valid ? sa_weights : 128'd0,
                sa_input_valid ? sa_row_mask : 16'd0,
                sa_input_valid ? sa_column_mask : 16'd0);
            $fwrite(trace_fd,
                "0x%064h,0x%064h,0x%064h,0x%0512h,0x%0512h,",
                pe_valid_mask, pe_mac_commit_mask, pe_add_commit_mask,
                pe_activations, pe_weights);
            $fwrite(trace_fd,
                "0x%01536h,0x%04h,0x%04h,%0d,%0d,%0d,%0d,",
                pe_accumulators, dut.sa_os_valid, row_ready_mask,
                pe_finish, storage_ready, output_request, output_grant);
            $fwrite(trace_fd,
                "%0d,%0d,%0d,%0d,0x%064h,%0d,%0d,%0d,%0d\n",
                dut.sa_internal_valid, dut.sa_internal_valid,
                row_score_valid, row_score_valid ? row_sequence : 0,
                output_normalized, cal_finish, row_score_valid,
                sau_last_result, drained);
        end
    endtask

    task automatic collect_output_row;
        integer tiles_per_batch;
        integer local_tile;
        integer h_group;
        integer w_group;
        integer n;
        integer oh;
        integer ow;
        integer index;
        integer signed slot;
        begin
            if (!row_score_valid)
                return;
            tiles_per_batch =
                ceil_div(cfg_out_h, rows_per_word(cfg_w)) *
                (cfg_w <= ROWS ? 1 : ceil_div(cfg_out_w, ROWS));
            n = tile_index / tiles_per_batch;
            local_tile = tile_index % tiles_per_batch;
            h_group = local_tile /
                (cfg_w <= ROWS ? 1 : ceil_div(cfg_out_w, ROWS));
            w_group = cfg_w <= ROWS ? 0 :
                local_tile % ceil_div(cfg_out_w, ROWS);
            if (cfg_w <= ROWS) begin
                oh = h_group * rows_per_word(cfg_w) +
                    row_sequence / cfg_w;
                ow = row_sequence % cfg_w;
            end else begin
                oh = h_group;
                ow = w_group * ROWS + row_sequence;
            end
            if (n >= cfg_n || oh >= cfg_out_h || ow >= cfg_out_w)
                $fatal(1, "registered output maps outside NCHW shape");
            for (int output_channel = 0;
                 output_channel < cfg_out_channels; output_channel++) begin
                index = (((n * cfg_out_channels + output_channel) *
                    cfg_out_h + oh) * cfg_out_w + ow);
                if (output_written[index])
                    $fatal(1, "duplicate NCHW output index %0d", index);
                slot = $signed(
                    output_slots[output_channel*16 +: 16]);
                if (slot < -128 || slot > 127)
                    $fatal(1, "non-INT8 output slot %0d", slot);
                output_written[index] = 1'b1;
                output_values[index] = slot;
                output_elements++;
            end
        end
    endtask

    task automatic write_output_file;
        integer index;
        begin
            output_fd = $fopen(output_path, "w");
            if (output_fd == 0)
                $fatal(1, "cannot open output file %s", output_path);
            $fwrite(output_fd, "n,oc,oh,ow,value\n");
            index = 0;
            for (int n = 0; n < cfg_n; n++) begin
                for (int output_channel = 0;
                     output_channel < cfg_out_channels; output_channel++) begin
                    for (int oh = 0; oh < cfg_out_h; oh++) begin
                        for (int ow = 0; ow < cfg_out_w; ow++) begin
                            if (!output_written[index])
                                $fatal(1,
                                    "missing NCHW output index %0d", index);
                            $fwrite(output_fd, "%0d,%0d,%0d,%0d,%0d\n",
                                n, output_channel, oh, ow,
                                output_values[index]);
                            index++;
                        end
                    end
                end
            end
            $fclose(output_fd);
        end
    endtask

    always @(posedge clk) begin
        if (rst_n) begin
            watchdog_cycle++;
            if (watchdog_cycle > WATCHDOG_CYCLES)
                $fatal(1, "pipeline watchdog expired");
        end
    end

    initial begin
        trace_fd = 0;
        output_fd = 0;
        trace_cycle = 0;
        watchdog_cycle = 0;
        output_elements = 0;
        im2col_done_cycle = -1;
        sau_last_result_cycle = -1;
        for (int index = 0; index < MAX_OUTPUTS; index++) begin
            output_written[index] = 1'b0;
            output_values[index] = 0;
        end
        require_plusargs();
        fill_scratchpad();
        trace_fd = $fopen(trace_path, "w");
        if (trace_fd == 0)
            $fatal(1, "cannot open trace file %s", trace_path);
        write_trace_header();

        repeat (5) @(posedge clk);
        rst_n = 1'b1;
        repeat (2) @(negedge clk);
        cfg_valid = 1'b1;
        @(negedge clk);
        cfg_valid = 1'b0;
        start = 1'b1;
        @(negedge clk);
        start = 1'b0;

        trace_cycle = 0;
        while (1) begin
            output_grant =
                (trace_cycle % output_ready_period) <
                output_ready_high_cycles;
            #1;
            write_trace_cycle();
            if (dut.im2col_done && im2col_done_cycle < 0)
                im2col_done_cycle = trace_cycle;
            if (row_score_valid &&
                output_elements + cfg_out_channels == expected_outputs)
                sau_last_result_cycle = trace_cycle;
            collect_output_row();
            if (drained)
                break;
            trace_cycle++;
            @(negedge clk);
        end
        $fflush(trace_fd);
        $fclose(trace_fd);
        if (output_elements != expected_outputs)
            $fatal(1, "output count %0d differs from expected %0d",
                output_elements, expected_outputs);
        write_output_file();
        $display("im2col_done_cycle=%0d", im2col_done_cycle);
        $display("sau_last_result_cycle=%0d", sau_last_result_cycle);
        $display("pipeline_drained_cycle=%0d", trace_cycle);
        $display("PASS pipeline fixture %s outputs=%0d",
            fixture_name, output_elements);
        $finish;
    end
endmodule
