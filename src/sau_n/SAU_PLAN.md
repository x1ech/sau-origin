# Gem5 Im2Col -> Mikui SAU 标准卷积周期模型计划

## 1. 目标

在已经通过 RTL 逐拍验收的 `Im2ColModel` 下游建立 Mikui 16x16 SAU
周期模型，形成一个独立的标准卷积 pipeline：

```text
NCHW activation
  -> Im2Col
  -> single-tile activation buffer
  -> deterministic weight/bias provider
  -> Mikui 16x16 SA_ENGINE cycle model
  -> quantized output collector
  -> NCHW output
```

第一版必须同时满足两类正确性：

1. Python 独立卷积 oracle 验证最终输出数据、顺序和数量；
2. gem5 与 integration RTL wrapper 对 controller、接口、256 个 PE 的有效数据、
   累加器和输出逐拍严格一致。

模型只能根据 fixture、确定性 activation/weight/bias 和 RTL 状态转换计算结果。
不得读取 golden trace 驱动模型，不得保存 fixture 对应的魔法周期表，也不得通过
调整期望结果迁就实现。

在 RTL trace 尚未返回前，最多只能声明：

```text
SAU pipeline implementation complete, RTL validation pending
```

只有全部冻结 golden profile 逐拍通过后才能声明：

```text
Im2Col-to-SAU RTL per-cycle validation passed
```

RTL 仿真结果是最终逐拍验收依据，不是开始建模的前置输入。Step 0 本地完成来源、补丁、
testbench和候选周期契约冻结后，即使暂时拿不到 VCS 结果，也允许继续 Step 1 至 Step 7。
模型必须直接根据 RTL 源码的组合逻辑、寄存器、nonblocking assignment和流水结构实现，
不得等待或读取 golden trace 来决定行为。尚未由 RTL 仿真确认的首输入、MAC commit、
bias、首输出、反压和 `cal_finish` 周期锚点必须标记为 provisional，并用 directed contract
test固定当前源码推导。VCS 结果返回后先校准这些锚点；若有差异，只能修订受影响的周期
契约、模型和测试并重新回归，不能调整 RTL golden 迁就模型。Step 8 golden导入和最终
`RTL per-cycle validation passed`声明仍必须等待 VCS 验收。

## 2. RTL 来源和黄金边界

### 2.1 Mikui 来源

第一版参考 RTL 固定为：

```text
remote: http://git.acelab.net.cn/zbn/npu_lpnpu.git
branch: mikui_16x16
commit: 2ca8252ef1cac43ef843998e9e08023259ac17ee
description: SAU 16*16 脉动阵列，256macs版本 V_0.1
```

从该提交的 `hardware/src/sa_execute/` 复制最小只读 RTL 集合到
`src/sau_n/rtl/mikui/original/`：

```text
SA_pkg.sv
SA_ENGINE.sv
SA_ROW.sv
SA_PE.sv
active_delay.v
weight_delay.v
DW02_mult_2_stage.v
registers.svh
```

实际编译若证明还需要直接依赖文件，只加入最小缺失集合，并在 provenance 中记录
原因。每个复制文件必须记录原路径、commit 和 SHA256。`original/` 中的文件必须与
commit `2ca8252` 逐字节一致，后续不得修改。

集成验证使用单独的 patched variant：

```text
src/sau_n/rtl/mikui/original/SA_ENGINE.sv
src/sau_n/rtl/mikui/integration/SA_ENGINE.sv
src/sau_n/rtl/mikui/patches/0001-fix-finish-dimension-width.patch
```

除 `SA_ENGINE.sv` 的已批准位宽修复外，其他模块直接从 `original/` 编译。适配、
ready/valid、tile buffer 和 observer 仍全部放在独立 wrapper/testbench。manifest
必须分别记录 upstream commit、original SHA256、patch SHA256、patched SHA256 和最终
RTL filelist；后续 golden 的黄金对象明确是该 patched integration variant，而不是
未经修改的 `2ca8252`。

生产参考宏固定为：

```text
MODULE_TEST undefined
FULL_PRECISION undefined
MAX_USE undefined
```

### 2.2 已批准的 `FINISH_ROW/FINISH_COL` 集成补丁

冻结源码中以下写法把常量转换成 `$clog2(16)=4` bit，数值 16 会截断成 0：

```systemverilog
assign FINISH_ROW = (($clog2(COL_NUM))'(ROW_NUM)>=row_num_i)?row_num_i:ROW_NUM;
assign FINISH_COL = (($clog2(COL_NUM))'(COL_NUM)>=col_num_i)?col_num_i:COL_NUM;
```

因此对合法的 `row_num_i/col_num_i=1..16`，原式不能按预期选择尾部尺寸。integration
variant 只允许使用显式同宽常量修复这两条赋值：

```systemverilog
localparam logic [$clog2(ROW_NUM):0] ROW_NUM_VALUE = ROW_NUM;
localparam logic [$clog2(COL_NUM):0] COL_NUM_VALUE = COL_NUM;
assign FINISH_ROW = (ROW_NUM_VALUE >= row_num_i) ? row_num_i : ROW_NUM_VALUE;
assign FINISH_COL = (COL_NUM_VALUE >= col_num_i) ? col_num_i : COL_NUM_VALUE;
```

Step 0 VCS矩阵最终必须用 original 和 patched 两个 DUT 做 `1/15/16` 行列定向探针：
先证明
original 的实际行为，再证明 patched variant 的 `FINISH_ROW/FINISH_COL`、输出行数和
列 mask 正确。VCS结果暂缺不阻塞Step 1至Step 7；若返回后不接受上述写法或实测行为
不同，则必须停止受影响的周期验收和Step 8，修订计划后重跑，不能扩大补丁范围。
该补丁不是静默修复，所有文档和最终声明都必须带
“based on `2ca8252` with the documented finish-dimension patch”。

### 2.3 SAU 黄金边界

第一版以 `SA_ENGINE.sv` 及其 PE 层次为黄金核心，不包含 Mikui 的：

- `SA_CORE` CSR、scheduler 和指令执行外壳；
- `mem_addr`、`mem_ctrl` 和 SRAM 接口；
- 原生 `feeder`、`sa_feeder`、register file、shift register 和 transposer；
- DMA、crossbar、CPU 和完整 NPU。

原因是 Mikui feeder 从原始 SRAM 数据组织卷积输入，而现有 Im2Col 已经完成窗口
展开。把两者串联会重复生成和重排 activation。integration wrapper 直接将完整
tile 转换为 `SA_ENGINE` 需要的连续 activation/weight 流。

### 2.4 卷积模式编码确认点

当前 `SA_pkg.sv` 和当前 `software/autodata/ins/ins.py` 使用：

```text
SA_pkg::CONV = 2'b01
```

部分端口注释、旧 benchmark 和旧 Verilog 文件仍写 `2'b10`。建模阶段以可执行
`SA_pkg::CONV=2'b01`比较逻辑为源码契约，Step 0 VCS baseline返回后再做实测确认。
若实测与当前`SA_pkg.sv`不一致，停止受影响的周期验收和Step 8并报告源码矛盾，
不静默选择另一编码，也不丢弃已经独立完成的fixture/oracle等无关工作。

## 3. 第一版范围

### 3.1 支持范围

- INT8 activation 和 INT8 weight，均按二进制补码有符号数解释；
- 3x3 标准卷积；
- `N >= 1`；
- `C = 1..63`；
- `out_channels = 1..16`；
- 现有 Im2Col 支持的 H/W、padding、stride 和 dilation；
- 现有 Im2Col 的 `W <= 16` 多行 packing 和 `W > 16` 宽度 splitting；
- 24-bit 有符号逐次饱和累加；
- 每输出通道一个 INT16 bias；
- `cutbit = 0..23` 算术右移；
- 最终 INT8 有符号饱和，使用 16-bit RTL slot 输出；
- 单 tile activation buffer；
- 默认 output ready 和周期性 output backpressure；
- 最终 NCHW output、cycle trace 和专项 stats。

### 3.2 不支持范围

- 外部 NPY、binary activation、weight 或 bias；
- `out_channels > 16` 和 output-channel 分块重放；
- `C > 63`；
- 非 3x3 kernel；
- INT16 activation；
- GEMM、PW、DW、matrix add 和 transposer 模式；
- 双 tile buffer或 tile 收集/计算重叠；
- Mikui 原生 CSR、scheduler、SRAM地址生成和 feeder；
- DMA、DRAM、cache、crossbar 或完整 NPU；
- 多时钟、异步 FIFO 或 CDC；
- 综合、物理实现、功耗或 Mikui 完整 NPU 性能声明。

第一版中的 dilation 和通用 stride 由上游 Im2Col 完成。它们不表示 Mikui 原生
feeder 已支持同样模式。

## 4. 固定硬件和数值参数

```text
clock                         = 100 MHz
SA rows                       = 16
SA columns                    = 16
physical MACs                 = 256
activation width              = signed INT8
weight width                  = signed INT8
accumulator width             = signed 24 bit
bias width                    = signed INT16
RTL quantized output slot     = signed INT16
logical output                = signed INT8
kernel_h/kernel_w             = 3/3
K per spatial tile            = C * 9
C range                       = 1..63
K range                       = 9..567
out_channels                  = 1..16
tile spatial lanes            = 1..16
tile buffers                  = 1
```

100 MHz（10 ns周期）是本 Im2Col -> SA 集成模型的选定频率，用于与已经验收的
Im2Col保持同一时钟域。Mikui RTL端口旁的“200 MHz”仅作为原设计目标注释保留；
本计划不声称复现其物理时序或200 MHz性能。当前复制模块没有基于绝对时间的功能
逻辑，因此严格比较以cycle编号为准；工作站wrapper固定使用10 ns周期。如果baseline
发现频率相关延时、`#delay` 或其他绝对时间依赖，必须停止并重新评估，不能换算后
继续比较。

物理 weight 接口每拍始终是 16 个 lane，但 `out_channels` 不要求为 16 的倍数。以下
尾列行为以第 2.2 节 patched variant 为准：

```text
weight[0 .. out_channels-1]   = valid
weight[out_channels .. 15]    = 0
col_num_i                     = out_channels
```

无效列不得产生 useful MAC，不得写入最终输出。未来扩展 `out_channels > 16` 时才需要
按 16 列分块；该扩展不属于第一版。

## 5. Pipeline fixture

### 5.1 独立 schema

不修改已经冻结的 Im2Col fixture schema。新增严格的 pipeline fixture loader，示例：

```json
{
  "schema_version": 1,
  "name": "conv_n1_c3_h32_w32_oc16",
  "im2col": {
    "schema_version": 1,
    "name": "conv_n1_c3_h32_w32_oc16_im2col",
    "n": 1,
    "c": 3,
    "h": 32,
    "w": 32,
    "kernel_h": 3,
    "kernel_w": 3,
    "stride_h": 1,
    "stride_w": 1,
    "dilation_h": 1,
    "dilation_w": 1,
    "pad_top": 1,
    "pad_left": 1,
    "spad_base": 0,
    "input_generator": "tb_act_value_v1"
  },
  "out_channels": 16,
  "cutbit": 8,
  "weight_generator": "tb_weight_value_v1",
  "bias_generator": "tb_bias_value_v1"
}
```

pipeline loader 必须：

- 拒绝未知字段、重复字段、缺失字段和非整数 numeric 字段；
- 复用现有 Im2Col loader 解析嵌套对象，不复制另一套尺寸公式；
- 强制 `kernel_h=kernel_w=3`；
- 强制 `C=1..63`、`out_channels=1..16`、`cutbit=0..23`；
- 计算 `K=C*9`、spatial tile 数、output element 数和 useful MAC 数；
- 对所有连乘执行 uint64 checked arithmetic；
- 输出 canonical resolved pipeline config JSON 和 SHA256；
- 启动前打印 `expected_tiles`、`expected_outputs` 和 `expected_macs`。

output ready 是运行环境而不是 workload 内容，不允许写入 fixture：

```text
--output-ready-period N
--output-ready-high-cycles N
```

周期 t 的 ready 定义为：

```text
output_ready[t] = (t % period) < high_cycles
```

运行入口必须强制 `period >= 1` 且 `1 <= high_cycles <= period`；默认值为
`period=1, high_cycles=1`。

### 5.2 确定性数据生成器

activation 继续使用：

```text
tb_act_value_v1_raw =
    (n * 97 + c * 31 + h * 7 + w + 1) mod 256
```

进入 SA 前把 raw byte 按 signed INT8 解释，不改变 bit pattern。

通用 weight generator 固定为：

```text
tb_weight_value_v1(oc, c, kh, kw) =
    ((oc * 29 + c * 17 + kh * 5 + kw * 3 + 11) mod 255) - 127
```

通用 bias generator 固定为：

```text
tb_bias_value_v1(oc) = ((oc * 37 + 13) mod 257) - 128
```

允许的测试生成器：

```text
weight_generator = tb_weight_value_v1 | zero | ones
bias_generator   = tb_bias_value_v1   | zero
```

Python、C++ 和 RTL testbench 必须使用相同公式和 signed conversion。不得使用随机数。

## 6. 逻辑数据映射

### 6.1 activation tile

现有 Im2Col 对每个 output spatial group 依次产生：

```text
for n
  for output_group
    for c
      for kh
        for kw
          emit 16-lane activation vector
```

所以每个 tile 恰好包含连续 `K=C*9` 个 activation 向量。adapter 必须为每个条目
保存：

```text
feed_data
feed_mask
n/output-group metadata
c/kh/kw or canonical k index
```

tile 内 canonical k index：

```text
k = ((c * 3) + kh) * 3 + kw
```

同一个 tile 的所有条目必须具有相同 spatial shape mask。padding lane 的
`feed_mask=1,data=0`，仍是有效 SA 行；尾部无效 lane 的 `feed_mask=0`，不进入输出。

### 6.2 SA 行列

```text
SA row  = Im2Col lane = 一个 output spatial position
SA col  = output channel
```

每个 tile 的有效行数：

```text
valid_rows = popcount(spatial feed_mask)
row_num_i  = valid_rows
```

现有 Im2Col 的无效 lane 位于 tile 尾部。adapter 仍必须验证 mask 是 canonical
prefix shape；遇到非 canonical scattered invalid mask 时拒绝运行，而不是错误压缩行号。

有效列数：

```text
col_num_i = out_channels
```

每个 k 对应的 16-lane weight vector：

```text
lane oc < out_channels: weight(oc,c,kh,kw)
lane oc >= out_channels: 0
```

### 6.3 物理 bit/lane 映射

逻辑 row/column 编号不能直接等同于所有 RTL packed bus 的低位顺序。integration
wrapper、C++ 模型、SV observer 和 trace comparator 统一使用以下映射：

```text
logical activation row r
  -> data_active_left[(15-r)*8 +: 8]
  -> SA_ENGINE PE_row[r].active_left_i

logical weight column c
  -> in_weight_above[c*8 +: 8]

logical bias column c
  -> in_bias_above[c*16 +: 16]

logical quantized output column c
  <- out_sum_final_q[c*16 +: 16]
```

也就是 activation row 0 位于输入总线最高字节，而 weight、bias 和 output 的
column 0 均位于各自总线最低 element。Im2Col 的 lane 0 原本位于最低字节，因此
wrapper 必须在进入 SA 前反转 activation 的 16 个 byte；不得反转 weight、bias 或
output column。

统一的 packed PE trace 仍采用逻辑顺序：

```text
pe_index = row * 16 + col
PE[0][0] occupies the least-significant trace element
```

物理 bus pack/unpack 必须集中在有独立单元测试的 helper 中，不能在 wrapper、模型和
observer 中分别手写。测试至少覆盖 row/column `0/1/14/15`，使用彼此不同的 byte/
halfword pattern，防止对称数据掩盖端序错误。

### 6.4 输出布局

SA 每个有效输出 handshake 产生一个空间行和最多 16 个输出通道。collector 使用
tile metadata 和 `row_seq_o` 映射到：

```text
output[n][oc][oh][ow]
```

不写入无效 spatial lane 或 `oc >= out_channels` 的列。所有合法 NCHW output element
必须且只能写一次；重复写、缺失写和越界写立即报错。

## 7. 数值契约

### 7.1 MAC 和 24-bit 累加

每个 PE 按 canonical k 顺序执行：

```text
product = signed_int8(activation) * signed_int8(weight)
acc24   = saturating_add_signed_24(acc24, sign_extend(product))
```

饱和发生在每一次加法，不允许先用无限精度求总和后只在末尾饱和。

3x3 标准卷积最后一个 k 完成后，每个输出通道的 signed INT16 bias 加一次：

```text
acc24 = saturating_add_signed_24(acc24, sign_extend(bias))
```

oracle 和模型必须以 RTL 实测确认 bias 的精确加入周期和 keep/clear 时序。

24-bit 饱和不能只依赖通用 golden workload 偶然覆盖。除 7 个端到端 profile 外，
新增两个 standalone SA 定向 profile，均使用 `K=567`、row1、col1、bias0：

```text
positive saturation: activation=-128, weight=-128
  第 512 次 MAC 首次超过 +8388607，期望钳位为 0x7fffff

negative saturation: activation=-128, weight=127
  第 517 次 MAC 首次低于 -8388608，期望钳位为 0x800000
```

两例必须逐次检查 accumulator，覆盖首次饱和前一拍、首次饱和拍、后续继续同号累加
和最终 bias/output；Python oracle、纯 C++ SA model、original/patched RTL baseline
均使用同一组显式输入向量，但不能通过共享饱和实现互相验证。

### 7.2 cutbit 和输出饱和

```text
shifted = arithmetic_shift_right(acc24, cutbit)
logical_output = clamp(shifted, -128, 127)
rtl_slot = sign_extend_16(logical_output)
```

第一版不做 rounding。`output.csv` 写 logical signed INT8 十进制值；cycle trace 记录
16-bit RTL slot 的固定宽度二进制补码 hex。trace 还记录有效 PE 的 24-bit accumulator。

## 8. 单 tile buffer和周期状态机

### 8.1 为什么必须缓冲完整 tile

Im2Col 的 ISSUE/COLLECT/PUSH/NEXT 使 feed 存在周期气泡，而 Mikui SA execution
路径按连续 K 拍输入设计。integration adapter 先收集完整 tile，再连续 K 拍驱动
`SA_ENGINE`，避免把上游气泡解释为阵列计算周期。

### 8.2 Pipeline 状态

冻结顶层状态编码：

```text
IDLE          = 0
COLLECT_TILE  = 1
LAUNCH_SA     = 2
STREAM_K      = 3
WAIT_RESULT   = 4
DRAIN_OUTPUT  = 5
DONE          = 6
```

若后续 RTL baseline 表明 `LAUNCH_SA` 与首个 K 输入必须同拍，必须修订状态转换，
并同时更新计划、contract test和 trace schema，不能留下隐式差异。baseline返回前
按第8.4节候选协议实现并标记该周期锚点为provisional。

### 8.3 行为契约

- `COLLECT_TILE`：仅在 tile buffer 未满且尚需条目时允许 Im2Col handshake；
- 收到第 K 个条目后关闭 Im2Col ready，不在同拍旁路到 SA；
- `LAUNCH_SA`：锁存 SA config、bias、valid row/column count；
- `STREAM_K`：连续 K 拍提供 activation 和 weight，不能插入气泡；
- `WAIT_RESULT`：等待 SA 首个输出行；
- `DRAIN_OUTPUT`：按 output ready 接收有效行；
- 当前 tile 全部有效行写入 collector 后清空 tile buffer；
- 单 buffer设计下，SA执行和输出期间不收集下一 tile；
- 最后 tile 输出完成后进入 `DONE`，随后 pipeline drained。

Im2Col `feed_ready` 必须由当前旧状态和 tile buffer 空间计算。顶层每拍遵守：

```text
observe old state
-> compute output_ready and SA combinational inputs
-> compute Im2Col feed_ready
-> tick Im2Col
-> compute handshakes and next state
-> tick SA
-> collect output
-> commit all next registers
```

不允许同拍读取刚写入的 tile buffer 条目，不允许空 buffer旁路启动 SA。

### 8.4 `SA_ENGINE` 控制协议

第一版只使用单次、非 retain 的标准卷积模式，wrapper 的候选驱动协议冻结为：

```text
sa_calmode_i    = SA_pkg::CONV（当前 package 值 2'b01，后续由 baseline 确认）
sa_flowmode_i   = 2'b00（CNORMAL/clear，不保留上一 tile accumulator）
register_mode_i = 2'b00（standard convolution）
shift_mode_i    = 1'b0（INT8 output saturation）
shift_ctl_i     = 1'b0（signed INT8 activation，不做 16-bit half switching）
CALC_CYCLE_i    = K
row_num_i       = valid_rows
col_num_i       = out_channels
```

配置从 `LAUNCH_SA` 开始驱动，`ins_valid_i` 只拉高一拍；但由于 RTL 的
`ins_update_flag = (ins_valid_i & state != WORK) | OS_valid[0]` 会在 `OS_valid[0]`
再次锁存，以上所有配置、`cutbit`、row/col count 和 bias 必须从 `LAUNCH_SA` 一直
保持到 `cal_finish`，不能在 `ins_valid_i` 下降后改为下一 tile。

候选输入/输出时序冻结为：

1. `LAUNCH_SA` 驱动稳定配置并脉冲 `ins_valid_i`；该拍`EN_i=0`，因此只更新配置
   寄存器，`sa_cur_state`保持`IDLE`；
2. `STREAM_K` 中 `EN_i` 连续拉高恰好 K 拍，首拍同时提供 `k=0` activation/weight，
   并驱动`IDLE -> START`；末拍提供 `k=K-1`并驱动`START -> WORK`，中间不允许
   气泡；
3. K 拍后拉低 `EN_i`，保持数据总线为 canonical 0，等待 `OS_valid[0]` 和
   `storage_ready` 的实测锚点；
4. `Flag_o` 是“启动/继续输出序列的请求输入”，不是完成输出。观察到
   `storage_ready` 后拉高并保持，直到内部首个 `valid_o` 接受事件；之后拉低，后续
   行由 `cnt_o != 0` 维持；
5. `Flag_o_ready` 直接由周期性 engine output grant 驱动。内部接受事件定义为
   `engine_output_fire = valid_o`；注册输出 `row_score_valid/out_sum_final_q/row_seq_o`
   在下一稳定周期出现并无条件写入 collector，不再与下一周期 ready 二次握手；
6. `cal_finish` 表示最后一行输出序列完成，用于释放当前 tile；`Flag_o`、
   `storage_ready`、`pe_finish_o` 和 `cal_finish` 均不得互相替代。

以下状态因果关系可直接由当前RTL的组合状态逻辑、寄存器和nonblocking assignment
确定，不属于等待VCS选择的候选行为：

- 新注册的`OS_valid[0]`出现时，该稳定周期的`sa_cur_state`仍为`WORK`；它在下一
  posedge驱动`WORK -> STORAGE`，并同时使`storage_ready`置位；
- 注册的`row_score_valid`和`cal_finish`在最后一行内部`valid_o`接受事件的下一稳定
  周期出现；该周期`sa_cur_state`仍为`STORAGE`；
- `cal_finish`在随后一个posedge驱动`STORAGE -> IDLE`并清除`storage_ready`。

首MAC、末MAC、bias、`OS_valid`、`storage_ready`、首输出和`cal_finish`的绝对cycle
编号在VCS返回前仍保持provisional；provisional只表示边沿编号需要实测校准，不允许
改变上述寄存器因果关系。

Step 0 VCS结果最终必须逐拍确认上述候选协议，覆盖 ready=1 及首行、中间行、末行停顿。
baseline
必须记录 `EN_i/ins_valid_i/Flag_o/Flag_o_ready`、全部 mode、`CALC_CYCLE_i`、
`datain_cnt`、`OS_valid[0]`、`storage_ready`、内部 `valid_o/cnt_o`、注册输出和
`cal_finish`。结果返回前允许严格按RTL源码实现C++周期模型并以contract test固定当前
推导；任何一项实测不一致都要更新本节、受影响模型和contract test，再进入Step 8。

## 9. 统一 gem5 SimObject

保留现有 `Im2ColTiming` standalone 行为和接口不变。新增：

```text
ConvPipelineTiming : ClockedObject
```

内部组合：

```text
Im2ColModel
SauTileBuffer
SauModel
OutputCollector
ConvPipelineTraceWriter
PeriodicOutputReady
```

使用统一 SimObject，而不是 gem5 memory ports。各内部类保持纯 C++、可独立测试；
`ConvPipelineTiming` 只负责参数转换、clock event、stats、trace 和 exit event。

默认输出：

```text
<outdir>/conv_pipeline/trace.csv
<outdir>/conv_pipeline/output.csv
<outdir>/stats.txt
<outdir>/config.ini
<outdir>/config.json
```

成功退出原因固定为：

```text
conv pipeline drained
```

## 10. 完成和 drain 契约

分别记录：

```text
im2col_done_cycle
sau_last_result_cycle
pipeline_drained_cycle
post_im2col_drain_cycles = pipeline_drained_cycle - im2col_done_cycle
```

pipeline drained 必须同时满足：

```text
Im2Col 不再产生新 feed
Im2Col FIFO 为空
tile buffer 为空
SA 不在 STREAM/WAIT/OUTPUT 状态
所有 expected tiles 已完成
所有 expected output elements 已写入且只写一次
没有待 output handshake
```

`im2col_done` 不能作为端到端退出条件。

## 11. Cycle trace

### 11.1 周期定义

沿用 Im2Col trace 约定：一个 trace cycle 是相邻两个 posedge 之间的稳定区间。
cycle 0 从 pipeline start 被接受后的第一个稳定区间开始。RTL fixture 在 negedge
驱动配置/start并在 negedge observer 记录，避免 posedge race。

### 11.2 观察内容

每拍至少记录：

- schema version、resolved config SHA256 和 cycle；
- pipeline state、tile index、tile buffer count、collect k、stream k；
- Im2Col state/done/FIFO metadata和 feed handshake/data/mask；
- SA config、state、datain counter、row counter和 output counter；
- 送入 SA 的 16-lane activation、weight、row mask和 column mask；
- 256-bit PE valid mask；
- 256-bit PE MAC commit mask和256-bit PE add commit mask；
- 256x8-bit PE activation packed field；
- 256x8-bit PE weight packed field；
- 256x24-bit PE accumulator packed field；
- `OS_valid`、`PE_valid` 和 storage-ready相关 masks；
- `Flag_o`、`Flag_o_ready`、内部 `valid_o`、`engine_output_fire`；
- 注册 output valid/row/16x16-bit data；
- im2col done、SA last result和 pipeline drained。

packed ordering 固定为逻辑 PE 顺序，不沿用 activation 物理总线的反向 byte 顺序：

```text
PE[row=0][col=0] occupies the least-significant packed element
index = row * 16 + col
```

所有 hex 使用小写、`0x` 前缀和固定宽度。无效 payload 规范化为 0。valid mask 不得
包含 X/Z；valid PE 的 activation、weight和 accumulator 不得包含 X/Z。无效 PE 的
原始 X/Z 不进入 canonical trace。

比较器遇到 packed PE 字段差异时，必须解码并报告首个不同的 `PE[row][col]`、
expected 和 actual，不能只打印整段超长 hex。

## 12. Statistics

### 12.1 Pipeline 周期

```text
convPipeline.im2colDoneCycle
convPipeline.sauLastResultCycle
convPipeline.drainedCycle
convPipeline.totalCycles
convPipeline.postIm2colDrainCycles
```

### 12.2 Tile 和输入

```text
sau.tilesCollected
sau.tilesLaunched
sau.tilesCompleted
sau.tileBufferAverageOccupancy
sau.tileBufferPeakOccupancy
sau.activationHandshakes
sau.engineInputCycles
sau.im2colBackpressureCycles
```

### 12.3 阵列利用率

```text
sau.usefulMacs
sau.arrayActiveCycles
sau.activeCycleMacUtilization
sau.endToEndMacUtilization
```

```text
activeCycleMacUtilization = usefulMacs / (256 * arrayActiveCycles)
endToEndMacUtilization    = usefulMacs / (256 * totalCycles)
```

useful MAC 只统计有效 spatial row、有效 output channel和全部 K。padding zero 是有效
spatial row，物理上仍执行 MAC；尾部无效 row/column 不计 useful MAC。

### 12.4 输出

```text
sau.outputRows
sau.outputElements
sau.positiveSaturations
sau.negativeSaturations
sau.outputBackpressureCycles
```

drained 时强制检查：

```text
tilesCollected = tilesLaunched = tilesCompleted = expectedTiles
activationHandshakes = expectedTiles * K
engineInputCycles = expectedTiles * K
outputElements = N * out_channels * out_h * out_w
usefulMacs = outputElements * K
```

## 13. 独立 functional oracle

新增 Python convolution oracle，直接按 NCHW 坐标计算：

```text
for n, oc, oh, ow
  acc24 = 0
  for c, kh, kw
    activation = signed tb_act_value_v1 or padding zero
    weight = selected deterministic generator
    acc24 = saturating_add_24(acc24, activation * weight)
  acc24 = saturating_add_24(acc24, bias[oc])
  output = saturating_int8(acc24 >>> cutbit)
```

oracle 不调用：

- Im2Col bank/address helper；
- tile buffer helper；
- SA PE、pipeline或cycle helper；
- RTL trace或golden output。

它同时生成 expected NCHW output 和 expected output count。gem5 output和 RTL output
必须分别通过 oracle，避免两个周期实现共享同一个功能错误。

## 14. RTL integration wrapper 和工作站流程

新增 wrapper/testbench，保持 `original/` 不变，并只使用第 2.2 节记录的 patched
`SA_ENGINE.sv`。wrapper 负责：

- 接收 reference Im2Col 的 `feed_*`；
- 实现同一份单 tile buffer状态机；
- 提供确定性 weight/bias；
- 连续 K 拍驱动 `SA_ENGINE`；
- 按第 6.3 节完成 activation反向 byte、weight/bias/output低位优先的总线映射；
- 按第 8.4 节驱动 `EN_i`、`ins_valid_i`、全部 mode、`CALC_CYCLE_i`、`Flag_o` 和
  `Flag_o_ready`；
- 收集 NCHW output；
- 通过 hierarchy 观察 256 个 PE 的 valid payload；
- 输出 canonical trace、output和 done/drained anchors；
- 自检 input/output守恒和 functional oracle 可检查的元数据。

本地不具备 VCS 环境。实现阶段生成自包含工作站包，包含：

```text
copied RTL and SHA256 manifest
integration wrapper/testbench
fixture runner
golden matrix fixtures
compile/run scripts
README.md
expected source/config hashes
```

工作站返回：

```text
simulator name/version
compile and run logs
trace.csv
output.csv
manifest.json
done/drained anchors
```

只有返回包中的 source/config hash与当前树一致时才能导入 golden。

## 15. Golden regression matrix

| Profile | 核心配置 | 主要覆盖 |
|---|---|---|
| `01_c1_w1_oc1_ones` | N1 C1 H3 W1 OC1, pad1, cut0, ones/zero | K=9、手算、row/col tail |
| `02_c2_w5_oc3_pack` | N1 C2 H4 W5 OC3, pad1, cut8, v1/v1 | W<=16 packing、bias、partial rows/cols |
| `03_c3_w16_oc16_full` | N1 C3 H3 W16 OC16, pad1, cut8 | 完整 16x16 阵列 |
| `04_c3_w17_oc7_dil2` | N1 C3 H5 W17 OC7, dil2, pad2, cut4 | W>16 split、tail、dilation |
| `05_n2_c4_w20_oc15_s2` | N2 C4 H5 W20 OC15, stride2, pad1 | batch、多 tile、stride、tail |
| `06_c63_w1_oc16_maxk` | N1 C63 H3 W1 OC16, pad1 | 最大 flow/K、长累加、计数边界 |
| `07_c2_w5_oc3_outbp` | profile 02, output ready 1/11 | 输出反压、FIFO积压、post-done drain |

以上 7 项是端到端 pipeline golden。另有不经过 Im2Col 的 standalone SA directed
matrix：

| Profile | 核心配置 | 主要覆盖 |
|---|---|---|
| `sa_01_k567_pos_sat` | K567 row1 col1, A=-128, W=-128, bias0 | 24-bit正饱和首次发生在第512次MAC |
| `sa_02_k567_neg_sat` | K567 row1 col1, A=-128, W=127, bias0 | 24-bit负饱和首次发生在第517次MAC |

具体 fixture 数值必须由 loader推导并在 matrix manifest 中冻结，不手写 magic expected
cycle。7个端到端profile的quick regression检查：

1. gem5 正常 drained；
2. stats anchors和守恒；
3. output匹配 Python oracle；
4. trace严格匹配 RTL golden；
5. source/config/provenance hash匹配。

两个standalone profile另外逐拍检查PE accumulator及首次饱和MAC序号，不套用
pipeline tile/output守恒条件。

## 16. 预计文件

新增或修改范围：

```text
src/sau_n/SAU_PLAN.md
src/sau_n/STATUS.md
src/sau_n/SConscript
src/sau_n/ConvPipeline.py
src/sau_n/sau_types.hh/.cc
src/sau_n/sau_generators.hh/.cc
src/sau_n/sau_tile_buffer.hh/.cc
src/sau_n/sau_model.hh/.cc
src/sau_n/conv_pipeline_model.hh/.cc
src/sau_n/conv_pipeline_trace.hh/.cc
src/sau_n/conv_pipeline_timing.hh/.cc
src/sau_n/*.test.cc
src/sau_n/rtl/mikui/*
src/sau_n/rtl/tb_im2col_mikui_sau_pipeline.sv
configs/example/conv_pipeline_timing.py
util/conv_pipeline/*
tests/gem5/conv_pipeline/*
m5out/conv_pipeline/provenance/src_sau_baseline/*  # 本地只读证据，不纳入源码交付
```

现有 `src/sau/` 不属于本计划，不能修改。现有 Im2Col standalone 配置、trace和回归
必须保持兼容。

## 17. 实施步骤

### Pre-Step 0：保存并冻结 `src/sau/` 初始基线

- 在修改任何 SAU pipeline 实现文件前，记录
  `git status --porcelain=v1 -- src/sau`；
- 分别保存 working-tree 和 staged 的 `git diff --binary -- src/sau`；
- 保存 tracked/untracked 文件清单、文件类型、mode、size 和 SHA256；
- 将证据放在
  `m5out/conv_pipeline/provenance/src_sau_baseline/`，并把证据目录自身的 manifest
  SHA256 写入 `STATUS.md`；
- 最终验收重新生成同格式快照，要求所有 `src/sau/` 文件路径、mode、size、SHA256
  以及 git status逐项相同。不能只用“最终 diff 没有新增条目”作为未修改证明。

验收：初始证据完整可读，能同时覆盖当前已修改的 tracked 文件和全部 untracked
文件；后续任何不一致都必须停止并交由用户判断，不能回滚或覆盖用户修改。

### Step 0：冻结 RTL 来源、补丁和 baseline

- 复制最小 Mikui RTL到 `original/` 并记录 provenance/hash；
- 生成第 2.2 节唯一批准的 patch、patched `SA_ENGINE.sv` 和明确 RTL filelist；
- 静态核对 16x16、INT8、24-bit accumulator、16-bit quant slot和 multiplier delay；
- 编写最小 SA_ENGINE baseline wrapper，同时支持 original/patched DUT；
- 用 `row_num_i/col_num_i=1/15/16` 定向确认 original 截断行为和 patched tail行为；
- 用简单 K=9、OC1、row1 input确认 reset、CONV编码、第 8.4 节全部控制输入、首输入
  周期、连续输入要求、bias周期、首输出周期、row序和 output ready停顿；
- 运行 K567正/负逐次饱和 standalone profile；
- 生成工作站 baseline包，由用户在 VCS 环境编译运行；
- VCS结果暂缺时记录`Step 0 VCS validation pending`，不伪造结果；本地来源、补丁、
  baseline包和候选契约完成后允许继续Step 1至Step 7。

本地继续开发条件：original和patched来源可追溯，补丁只包含批准的位宽修改，baseline
包及source/trace verifier通过本地静态自检。最终Step 0验收仍要求original和patched均
可编译，tail、控制协议、总线映射、正负饱和和周期锚点均有VCS实测依据。

### Step 1：pipeline contract、fixture和生成器

- 实现 Python/C++ pipeline config和checked derivation；
- 实现严格 nested fixture loader和canonical SHA256；
- 实现 signed activation conversion、weight/bias generators；
- 冻结 state、packed ordering、ready和done/drained契约；
- 覆盖所有范围、溢出、未知/重复字段和 generator测试。

验收：Python/C++派生值一致，非法 fixture在启动前被拒绝。

### Step 2：独立 convolution oracle和tile mapping

- 实现逐 MAC 24-bit饱和的 Python oracle；
- 实现 Im2Col group/lane到 NCHW output坐标的独立映射；
- 验证 W=1/5/16/17/20、padding、stride、dilation、batch和tail；
- 验证 output count、useful MAC和bias/cutbit/saturation。

验收：oracle可独立生成全部 golden matrix 的最终 output。

### Step 3：SA数值核心

- 实现 16x16 PE寄存器和数据传播结构；
- 实现 signed INT8 multiplier pipeline；
- 实现每次加法的 24-bit饱和；
- 实现 bias、cutbit和INT8饱和；
- 覆盖正负乘法、累加边界、列tail和reset/clear。

验收：纯 C++单元测试与独立数值向量一致，不依赖 RTL trace；K567正负 profile逐次
检查首次饱和拍和后续保持行为。

### Step 4：SA周期状态和256 PE阵列

- 逐拍复刻 SA_ENGINE/SA_ROW/SA_PE old-state/next-state行为；
- 实现 input/weight skew、valid传播、MAC delay、acc finish和row output；
- 复刻 patched `row_num_i/col_num_i`、第 8.4 节完整控制协议、output grant和内部
  停顿；
- 暴露 canonical packed PE snapshot。

验收：standalone SA directed cycle tests覆盖 K=9、K=567、row/col tail和backpressure；
VCS返回前周期锚点状态为provisional，不能声称RTL逐拍通过。

### Step 5：单 tile buffer和端到端 pipeline

- 实现 K-entry单 tile buffer和metadata守恒；
- 将 Im2Col feed ready连接到 COLLECT_TILE状态；
- 连续 K 拍驱动 SA，禁止泡和旁路；
- 实现 output collector和NCHW写入检查；
- 实现三个完成周期锚点和pipeline drained。

验收：纯 C++端到端 output匹配 Python oracle，反压下无丢失/重复。

### Step 6：gem5 SimObject、stats和运行入口

- 注册 `ConvPipelineTiming` 和新增 C++ sources/GTests；
- 新增 `conv_pipeline_timing.py`；
- 写出 output、trace和stats；
- 支持默认/周期性 output ready；
- 保持 `Im2ColTiming` standalone不变。

构建由用户手动执行：

```bash
scons build/RISCV/gem5.opt -j4 \
    --ignore-style --limit-ld-memory-usage
```

验收：最小 fixture通过标准 gem5 exit event drained，统计守恒。

### Step 7：canonical trace和比较器

- 实现固定 schema trace writer/loader；
- 实现 packed PE字段格式、X/Z规则和invalid normalization；
- 比较器报告首个 cycle/field，PE字段继续报告首个 row/col；
- 用合成差异测试所有诊断路径。

验收：合法 trace可 round-trip，所有错误格式被拒绝。

### Step 8：RTL wrapper、工作站包和golden导入

- 完成 patched integration wrapper/testbench和fixture runner；
- 先通过 legacy Im2Col回归和 SA baseline；
- 生成7个端到端profile及2个standalone饱和profile的工作站包；
- 校验返回 source/config/simulator manifest；
- 分别验证 RTL output与Python oracle；
- 对7个profile执行 gem5/RTL strict per-cycle comparison；
- 对2个standalone profile执行C++ SA/RTL accumulator strict per-cycle comparison；
- 导入 validated golden和testlib quick tests。

验收：7个端到端profile完成gem5/RTL逐拍比较，2个standalone profile完成C++ SA/RTL
accumulator逐拍比较，才能声明 RTL per-cycle validation passed。

### Step 9：文档和最终验收

- 更新 README，说明架构、fixture、运行、output、trace、stats和限制；
- 更新 STATUS，记录实际命令、结果、hash和未验证项；
- 复跑原 Im2Col standalone regression；
- 复跑 pipeline Python/C++/gem5/RTL golden regression；
- 按 Pre-Step 0格式重建 `src/sau/` 快照并与初始manifest逐项比较。

验收：Definition of Done全部满足，文档命令可以复现。

## 18. Definition of Done

只有同时满足以下条件才算完成：

1. 原 Im2Col standalone Python、C++、gem5和7-profile RTL golden回归继续通过；
2. SA和pipeline Python/C++单元测试通过；
3. Python convolution oracle验证所有最终 NCHW output；
4. Mikui `original/` 与 `2ca8252` 逐字节一致；integration variant只包含已记录的
   `FINISH_ROW/FINISH_COL` 位宽补丁；
5. integration wrapper的7个 RTL profile全部运行通过；
6. 7个端到端profile的gem5/RTL逐拍、逐字段一致，2个standalone profile的C++
   SA/RTL accumulator逐拍一致；
7. 256个PE的valid activation/weight/accumulator逐拍一致；
8. output数据、数量、顺序和done/drained守恒；
9. 默认和output backpressure场景均通过；
10. README、运行命令、限制和RTL provenance完整；
11. `src/sau/` 最终路径/mode/size/SHA256和git状态与Pre-Step 0基线逐项一致；
12. 最终声明严格限定为 Im2Col -> Mikui SAU INT8 3x3标准卷积周期模型；
13. 两个K567 standalone profile分别证明24-bit正、负逐次饱和的首次发生拍及后续
    行为与RTL一致。

## 19. 已知风险和确认点

- 当前 RTL存在 CONV编码的新旧注释矛盾；建模先遵循`SA_pkg`可执行逻辑，最终必须由
  Step 0实测确认；
- 原始 `2ca8252` 的 `FINISH_ROW/FINISH_COL` 常量位宽会把16截断为0；第一版黄金对象
  是带有第2.2节显式补丁的integration variant，不得混用original trace；
- `SA_ENGINE` 输出不是普通的同拍ready/valid接口；`Flag_o`请求、内部`valid_o`接受
  和下一拍注册输出必须按第8.4节分别建模；
- activation物理byte顺序与weight/bias/output相反，所有映射必须经过集中helper；
- 通用卷积profile不保证触发24-bit正负逐次饱和，standalone K567两例是强制验收项；
- RTL中的200 MHz只是原模块目标注释；本集成模型明确选择100 MHz（10 ns周期）以与
  已验收Im2Col统一。纯同步逐拍结果不依赖频率，任何200 MHz性能/时序收敛结论均不
  属于本计划；
- 当前worktree在本计划开始前已有`src/sau/`修改，必须用Pre-Step 0完整manifest证明
  最终未触碰该目录，不能依赖肉眼检查git diff；
- Mikui部分流水寄存器无reset，observer必须按valid规范化无效X/Z；
- hierarchy观察256个PE依赖稳定的generate名称和VCS行为，runner必须记录版本；
- Im2Col输出有气泡，单 tile buffer是正确性适配，不是可选性能优化；
- 单 buffer禁止收集/计算重叠，性能统计不代表优化后的硬件上限；
- K=567和packed PE trace可能产生较大文件，quick fixtures仍需外部超时策略；
- copied RTL来自独立仓库，任何源commit变化都必须重新生成provenance和golden；
- 若baseline证明 SA_ENGINE无法在不包含sa_feeder的情况下保持目标语义，应停止并
  重新审查边界，不能在模型中臆造时序。
