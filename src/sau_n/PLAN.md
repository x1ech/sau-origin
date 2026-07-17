# Gem5 Im2Col Reference RTL 独立周期模型计划

## 1. 目标

为 `rtl/gemmini_im2col_chw_gather_readable.sv` 建立独立的 gem5
`ClockedObject` 周期模型，逐拍复刻该 reference RTL 的：

- 配置锁存和六态状态机；
- CHW/W-inner 地址与 lane 计算；
- 16-bank SRAM 请求和仲裁；
- 组合 SRAM 响应；
- FIFO push/pop 和 `feed_*` 握手；
- `busy`、`done` 和完成周期。

模型的周期只能由 RTL 逻辑、resolved workload 配置和预加载 scratchpad 内容
计算。不得读取 RTL trace 驱动模型，不得保存 fixture 对应的魔法周期数组，也
不得用 golden 拟合延迟。

RTL golden 只是后续逐拍验证 oracle。没有 golden 时可以完成模型和自测，但只能
声明：

```text
RTL model implementation complete, RTL validation pending
```

实际 RTL trace 逐拍比较通过后才能声明：

```text
RTL per-cycle validation passed
```

## 2. 最终交付形态

最终用户通过 JSON fixture 配置 workload，通过 gem5 配置脚本运行：

```bash
./build/RISCV/gem5.opt \
    configs/example/im2col_timing.py \
    --fixture tests/gem5/im2col/fixtures/w5.json \
    --trace m5out/im2col/trace.csv
```

默认运行环境与当前 testbench 一致：

```text
clock = 100 MHz
SRAM response = combinational
feed_ready = 1
```

为验证 DUT 原生 FIFO 反压，可以选择性配置周期性 `feed_ready`：

```text
--ready-period <N>
--ready-high-cycles <N>
```

这只是驱动 RTL 已有的 `feed_ready` 输入，不改变 DUT，不引入新的 SRAM 协议或
额外硬件模型。

输出至少包括：

```text
trace.csv
stats.txt
rtl_done_cycle
drained_cycle
post_done_drain_cycles
```

- `rtl_done_cycle`：模型中 DUT 单拍 `done` 所在周期；
- `drained_cycle`：从 `rtl_done_cycle` 开始，FIFO 首次为空且不会再产生新 feed 的
  周期；
- `post_done_drain_cycles = drained_cycle - rtl_done_cycle`；
- 仿真在 drained 后退出，避免反压运行中遗漏 done 之后仍在 FIFO 的数据。

## 3. 范围

### 3.1 第一版支持

- 普通卷积的 `N/C/H/W`；
- resolved `out_h/out_w`；
- kernel、stride、dilation、`pad_top/pad_left`；
- `W <= 16` 的多 H 行打包；
- `W > 16` 的宽度分段；
- 已预加载的 16-bank scratchpad；
- gather、FIFO、`feed_data/feed_mask`；
- 默认 `feed_ready=1`；
- 可选的周期性 `feed_ready`，用于验证 DUT 自身的反压行为。

### 3.2 第一版不支持

- DMA/DRAM 到 scratchpad 的预加载；
- gem5 `RequestPort`、SystemXBar 或系统内存；
- 固定或可变的多周期 SRAM 响应；
- response tag、pending request 或请求去重适配器；
- 脉动阵列、权重输入、MAC 或输出写回；
- CSR、指令解码或完整 NPU 数据流；
- `cfg_dw_mode=1`；
- 非全 1 的 `cfg_kernel_pattern`；
- `kernel_h * kernel_w > 16`；
- NPY、二进制文件或任意外部 tensor；
- `W <= 16 && out_w > W`；
- 时钟、FIFO 深度、bank 数或其他结构参数扫描。

`W <= 16 && out_w > W` 是 reference RTL 当前宽度迭代方式的限制。第一版直接
拒绝，不修改 RTL，也不在模型中补造额外宽度分块。

## 4. 固定参数和配置校验

硬件参数固定为：

```text
BLOCK_SIZE = 16
SP_BANKS = 16
ELEM_W = 8 bit
SP_BANK_ENTRIES = 4096
FIFO_DEPTH = 4
clock = 100 MHz
```

Python fixture loader 和 C++ 模型必须执行一致的校验：

```text
n/c/h/w/out_h/out_w:       1..65535
pad_top/pad_left:          0..65535
kernel_h/kernel_w:         1..15
stride_h/stride_w:         1..15
dilation_h/dilation_w:     1..15
kernel_h * kernel_w:       <= 16
spad_base:                 0..4095
cfg_dw_mode:               0
cfg_kernel_pattern:        0xffff
```

另外必须满足：

```text
W <= 16 时 out_w <= W
spad_base + total_spatial_words <= 4096
total_spatial_words = N * C * spatial_words_per_channel
```

所有乘法和输出尺寸计算先使用足够宽的整数完成，再检查能否安全转换到 RTL 的
固定位宽。第一版不开放 `cfg_spad_base` 的高 bank 位语义；物理 bank 始终由
lane 决定，`spad_base` 只作为每个 bank 内的 row base。

所有派生计数，包括 `expected_vectors`、cycle、push/pop/handshake 和统计累加值，
统一使用 `uint64_t` 或等价的 64-bit 无符号类型。所有连乘使用 checked
multiplication；如果结果不能用 `uint64_t` 表示，Python loader 和 C++ 模型都拒绝
配置，不能截断或回绕。

scratchpad footprint 校验只约束输入存储，不保证显式 `out_h/out_w` 对应的运行
规模适合快速仿真。第一版不因为运行时间长而改变 RTL 合法配置，也不在模型中
加入 watchdog。fixture loader 必须在启动前打印 `expected_vectors`；quick test 只
使用规模受控的 fixture，批量运行的超时由外部测试框架负责。用户提供的大规模
合法 workload 可能需要很长时间，这一点必须写入 README。

## 5. workload 和输出尺寸

### 5.1 JSON fixture

JSON fixture 是 Python logical feed sequence oracle、gem5 模型和 RTL fixture
runner 的唯一 workload 来源。例如：

```json
{
  "schema_version": 1,
  "name": "w5_pack3_pad1_stride1",
  "n": 1,
  "c": 2,
  "h": 4,
  "w": 5,
  "out_h": 4,
  "out_w": 5,
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
}
```

fixture 只描述 workload。周期性 ready 是运行环境参数，不写入 fixture。

### 5.2 输出尺寸解析

`out_h/out_w` 可以同时填写，也可以同时省略：

- 均未填写：Python loader 自动计算；
- 均填写：使用显式值；
- 只填写一个：拒绝 fixture；
- 显式值与自动计算结果不同：允许并打印提示；
- Python 最终生成 resolved config，明确保存实际传给 gem5 和 RTL 的
  `out_h/out_w`；
- C++ 模型只接收 resolved config，不包含第二套输出尺寸推导逻辑。

自动计算沿用当前 testbench 的对称 padding 假设：

```text
effective_kernel_h = dilation_h * (kernel_h - 1) + 1
effective_kernel_w = dilation_w * (kernel_w - 1) + 1
out_h = floor((H + 2 * pad_top - effective_kernel_h) / stride_h) + 1
out_w = floor((W + 2 * pad_left - effective_kernel_w) / stride_w) + 1
```

只有自动计算模式要求公式分子非负。显式输出模式只校验输出尺寸、固定位宽和
第一版支持边界，不因自动公式结果不同而拒绝。

### 5.3 确定性输入

第一版只支持：

```text
tb_act_value_v1 = (n * 97 + c * 31 + h * 7 + w + 1) mod 256
```

Python、C++ 和 SV 都使用低 8 bit 的无符号字节。解释为 int8 时只改变显示，
不能改变 packed bits。

SystemVerilog 不解析 JSON。Python runner 校验并解析 fixture 后，将 resolved
config 转换为明确的 plusargs。未提供 fixture plusargs 时，现有 testbench 的
四个功能案例和 PASS/FAIL 行为保持不变。

## 6. 逐拍 RTL 契约

### 6.1 状态机

```text
IDLE -> ISSUE -> COLLECT -> PUSH -> NEXT -> DONE
```

- `IDLE`：接受 start，清零迭代器；
- `ISSUE`：锁存 lane 请求，清空 `lane_done/interm_*`，padding lane 直接产生
  mask=1、data=0；
- `COLLECT`：按 bank 仲裁并收集组合 SRAM 响应；
- `PUSH`：FIFO 非满时写入向量；
- `NEXT`：按 `kw -> kh -> c -> ow/oh -> n` 推进；
- `DONE`：该稳定区间内 `state=ST_DONE, done=0, busy=1`；区间结束的 posedge
  执行 `done <= 1` 和 `state <= ST_IDLE`，因此下一稳定区间才是
  `state=ST_IDLE, done=1, busy=0`。done 不表示 FIFO 已空。

### 6.2 每周期执行顺序

cycle 表示两个相邻 posedge 之间的稳定区间。每个 cycle 固定执行：

```text
cycle t 开始：寄存器已经由前一个 posedge 提交
-> 根据已提交寄存器计算组合 req/resp/feed/push/pop
-> 在 negedge 记录 cycle t trace
-> 结束 cycle t 的 posedge 使用 cycle t 组合信号计算 next state
-> 所有寄存器更新统一提交
-> 进入 cycle t+1
```

实现必须读取 old state、计算 next state、最后统一 commit，不能用 C++ 语句顺序
代替 SystemVerilog nonblocking assignment 语义。

组合 SRAM 响应在请求所在 cycle 可见，但只在该 cycle 结束的 posedge 被
`ST_COLLECT` 消费。最后一次响应更新 `lane_done` 后，还需要下一个 COLLECT
判定周期才能看到 `all_done` 并进入 PUSH。

feed handshake 归属于 trace 中 `feed_valid && feed_ready` 同时为 1 的 cycle，
FIFO pop 在该 cycle 结束的 posedge 提交。

`done` 不能由 C++ 当前 state 组合生成。模型必须把它作为寄存器处理：每个 tick
默认 next `done=0`，只有 old state 为 `ST_DONE` 时提交 next `done=1` 和
`state=ST_IDLE`。逐拍测试必须同时检查 `ST_DONE/done=0/busy=1` 和紧随其后的
`ST_IDLE/done=1/busy=0`。

### 6.3 bank 仲裁

- 每个 bank 每周期最多一笔请求；
- 按 destination lane 升序选择每个 bank 的第一个未完成 row；
- 同 bank、同 row 的多个 lane 由一次响应同时完成；
- 同 bank、不同 row 的请求跨周期串行；
- 全 padding 或全无效向量不发请求，但仍经过一次 COLLECT 判定。

### 6.4 FIFO

- `feed_*` 来自当前 FIFO head；
- pop 条件为 `feed_valid && feed_ready`；
- push 条件为 `state == PUSH && fifo_count != FIFO_DEPTH`；
- FIFO 满且同拍 pop 时，push 仍读取旧 full 状态，因此不能同拍 push；
- FIFO 空且同拍 push 时，旧 `feed_valid` 为 0，不能旁路消费；
- `feed_mask=1` 包含 padding zero lane，不等同于 SRAM-read lane。

### 6.5 启动与周期编号

每个 fixture 只运行一个命令：

```text
reset
-> scratchpad preload
-> 在 negedge 拉高 cfg_valid，跨越一个 posedge，在下一个 negedge 拉低
-> 在后续 negedge 拉高 start，跨越一个 posedge，在下一个 negedge 拉低
-> start 被接受后的稳定区间定义为 cycle 0
```

reset、fixture 解析、scratchpad preload 和配置锁存不计入模型延迟。cycle 0
状态为 ISSUE。cycle 从 0 开始，因此：

```text
total_done_cycles = rtl_done_cycle + 1
total_drained_cycles = drained_cycle + 1
```

周期性 ready 定义为：

```text
feed_ready[t] = (t % ready_period) < ready_high_cycles
ready_period >= 1
1 <= ready_high_cycles <= ready_period
```

默认值为 `ready_period=1, ready_high_cycles=1`。

## 7. logical feed sequence oracle

Python oracle 只验证逻辑 feed 内容和顺序，不计算物理 bank、row、冲突、FIFO 或
周期，也不复用 C++ physical packing helper。

它按 reference RTL 的顺序输出 `feed_data/feed_mask`：

```text
kw -> kh -> c -> ow/oh group -> n
```

每个 lane 直接根据 NCHW 输入坐标、stride、dilation、padding 和输出边界计算：

- 正常输入坐标：mask=1，data=`tb_act_value_v1`；
- padding：mask=1，data=0；
- 尾部或打包空位：mask=0，data=0。

模型开始前还必须计算预期 feed 向量数：

```text
W <= 16:
    h_groups = ceil(out_h / rows_per_word)
    w_groups = 1
W > 16:
    h_groups = out_h
    w_groups = ceil(out_w / 16)

expected_vectors =
    N * C * kernel_h * kernel_w * h_groups * w_groups
```

运行中必须满足：

```text
fifo_count = push_count - pop_count
push_count <= expected_vectors
handshake_count <= push_count
```

drained 时必须满足：

```text
push_count = expected_vectors
handshake_count = expected_vectors
fifo_count = 0
```

## 8. trace 与 golden

### 8.1 trace schema

RTL 和 gem5 使用同一 CSV 字段：

```text
schema_version,resolved_config_sha256,cycle,state,busy,done,
fifo_count,fifo_rptr,fifo_wptr,
req_valid,req_addr_b00,...,req_addr_b15,
resp_valid,resp_data_b00,...,resp_data_b15,
feed_valid,feed_ready,feed_data,feed_mask
```

header 中的 `...` 只是本文缩写；实际 CSV 必须把 `b00` 到 `b15` 按 bank 升序全部
展开，不能包含省略号。

- CSV 使用 UTF-8、逗号分隔、单行 header、LF 换行，不允许额外注释行；
- `schema_version` 固定为十进制 `1`；
- `resolved_config_sha256` 是 64 个小写十六进制字符，不带 `0x`；
- `cycle` 使用无符号十进制，从 0 开始；
- `state` 使用十进制数值，冻结映射为
  `IDLE=0, ISSUE=1, COLLECT=2, PUSH=3, NEXT=4, DONE=5`；名称只用于比较器诊断，
  不写入 CSV state 字段；
- `busy/done/feed_valid/feed_ready` 使用十进制 `0/1`；
- `fifo_count/fifo_rptr/fifo_wptr` 使用无符号十进制；
- `req_valid/resp_valid/feed_mask` 分别使用补足到 4 hex digit 的 `0x` 十六进制；
- `req_addr_b00..b15` 使用 3 hex digit，`resp_data_b00..b15` 使用 2 hex digit，
  `feed_data` 使用 32 hex digit，均带 `0x` 且使用小写；
- bank 0 和 lane 0 固定为 packed 字段最低有效位置；
- `feed_valid=0` 时 trace 将 `feed_data/feed_mask` 规范化为 0；
- 某 bank 的 `req_valid=0` 时将对应 `req_addr` 规范化为 0；
- 某 bank 的 `resp_valid=0` 时将对应 `resp_data` 规范化为 0；
- 有效 payload、state、valid 和 FIFO 元数据不允许 X/Z。

所有模式统一从 cycle 0 记录到 drained cycle，包含首尾两行，因此数据行数必须为
`drained_cycle + 1`。默认 ready=1 时 FIFO 在 done 前已经排空，所以
`drained_cycle == rtl_done_cycle`；启用周期性 ready 时继续记录 done 后排空过程。

resolved config 的 SHA256 输入固定为 resolved config 对象的 canonical JSON，
等价于 Python
`json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)` 的 UTF-8
编码结果：整数使用无前导零十进制、无额外空白、末尾无换行。resolved config
schema 不允许浮点数。Python loader 生成 canonical bytes 和 hash，并把同一 hash
明确传给 gem5 和 RTL runner；C++ 模型不重新定义第二套 JSON canonicalization。

### 8.2 RTL fixture runner

只允许扩展 `rtl/tb_gemmini_im2col_chw_gather_readable.sv`，不修改 DUT。

- fixture 模式在 negedge 驱动 cfg/start，避免与 DUT posedge 采样产生 race；
- observer 在 negedge 采样 NBA 提交后的稳定信号；
- 未提供 fixture/trace plusargs 时保留现有四例行为；
- 每个 fixture 独立运行并生成独立 trace。

### 8.3 golden 的角色

golden 不是模型实现前置条件。没有 SV 仿真器时，可以先完成 Python、C++ 和
gem5 模型，不伪造 golden，也不阻塞开发。

golden 可用后保存：

- 原始 fixture 和 resolved config；
- resolved config SHA256；
- DUT source SHA256；
- testbench/observer source SHA256；
- 固定硬件参数；
- trace schema version；
- simulator 名称和版本；
- RTL CSV。

比较器逐周期逐字段比较，在首个差异处报告 fixture、cycle、字段和 bank/lane。

## 9. 统计

只统计与该 RTL 直接相关的指标：

- `rtlDoneCycle`、`drainedCycle`、`postDoneDrainCycles`；
- total done/drained cycles；
- feed vector 数、handshake 数；
- presented、SRAM-read、padding-zero 和 invalid lane 数；
- 每 bank 请求周期和利用率；
- 同 bank 不同 row 冲突及其增加的 COLLECT 周期；
- FIFO 平均/峰值占用、full stall；
- downstream backpressure cycles。

默认吞吐和 FIFO 平均值使用 cycle 0 到 drained 的区间，并额外保留 done 前周期
用于 RTL 诊断。

统计口径冻结如下：

- 统计区间包含 cycle 0 和 drained，分母为
  `total_drained_cycles = drained_cycle + 1`；
- presented、SRAM-read、padding-zero、invalid 按成功 push 的每个向量统计，四者
  中 SRAM-read、padding-zero 和 invalid 对每个向量必须相加为 16；presented
  等于 SRAM-read 加 padding-zero，invalid 是不进入 `feed_mask` 的 lane；
- bank `b` 利用率为 `request_cycles_b / total_drained_cycles`；
- 对一个向量，令 `rows_b` 为 bank `b` 的不同请求 row 数；同 bank 不同 row
  冲突数为 `sum_b max(rows_b - 1, 0)`，该向量增加的 COLLECT 请求周期为
  `max(max_b(rows_b) - 1, 0)`；最终的无请求 all-done 判定周期不计入冲突增加值；
- FIFO 平均占用对每个 trace cycle 开始时的稳定 `fifo_count` 取算术平均，峰值取
  同一采样点最大值；
- full stall cycle 是 `state==PUSH && fifo_count==FIFO_DEPTH`；
- downstream backpressure cycle 是 `feed_valid && !feed_ready`。

## 10. 实施步骤

### Step 0：baseline RTL 和工具检查

- 只读检查 DUT/testbench 当前状态、固定参数和已有四个案例；
- 检查目标 SystemVerilog simulator 是否可用并记录名称和版本；
- simulator 可用时先编译并运行未修改行为的四个 baseline 案例；
- simulator 不可用时明确记录 `RTL baseline compile/run pending`，不伪造结果，也
  不阻塞 Python/C++ 模型开发；
- 在实现模型前冻结本计划中的 done/state 周期锚点和 trace 字段格式。

### Step 1：冻结类型、校验和周期契约

- 定义 resolved config C++/Python 类型；
- 实现参数和 scratchpad footprint 校验；
- 实现派生计数的 checked multiplication 和 64-bit 计数策略；
- 固定 old-state/next-state/commit 和 trace schema；
- 单元测试覆盖全部拒绝路径、cycle 0 定义和 done/state/busy 周期锚点。

### Step 2：实现 fixture loader 和 logical oracle

- 实现 JSON schema、自动/显式输出尺寸和 resolved config；
- 实现 `tb_act_value_v1`；
- 实现 logical feed sequence oracle 和 expected vector count；
- 测试 `W=1/5/16/17/20`、padding、stride、dilation 和尾块。

### Step 3：实现地址和内部 scratchpad

- 新增 `im2col_types.hh` 和地址/packing helper；
- 新增 `banked_scratchpad.hh/.cc`；
- 实现 CHW 预加载、row/lane/bank 计算和组合响应；
- 测试同 bank 同 row、同 bank 不同 row 和 footprint 边界。

### Step 4：实现状态机和 FIFO

- 新增 `im2col_model.hh/.cc` 的纯 tick 核心；
- 实现六态状态机、仲裁、FIFO 和 feed handshake；
- 实现 done/drained 和向量守恒断言；
- 测试全 padding、全无效、FIFO 满空和周期性 ready。

### Step 5：注册 gem5 SimObject 和运行入口

- 修改 `SConscript` 属于构建系统变更，实施到本步骤前必须向用户说明影响并取得
  明确确认；
- 新增 `src/sau_n/SConscript`、`src/sau_n/Im2Col.py`；
- 新增 `configs/example/im2col_timing.py`；
- 读取 fixture 并实例化独立模型；
- 不创建内存总线、CPU 或完整 NPU 数据流；
- drained 后正常退出并写出 stats；
- 源码完成后暂停，由开发者手动执行增量构建；确认新 binary 构建成功后，才继续
  gem5 quick test 和端到端验收。

### Step 6：实现 trace、统计和比较器

- 实现 gem5 CSV writer；
- 实现 Python per-cycle 比较器及其单元测试；
- 注册本计划定义的最小统计集合；
- 检查 trace、stats 和模型内部计数守恒。

### Step 7：准备 RTL observer 和回归

- 扩展 testbench fixture runner 和 CSV observer；
- legacy 四例继续固定 `feed_ready=1`；fixture 模式才按 cycle 0 相位驱动周期性
  ready；
- 没有 SV 仿真器时记录待运行命令，不阻塞模型实现；
- golden 可用后运行本计划冻结的最小 golden 矩阵；
- 新增 `tests/gem5/im2col/test_im2col.py` quick test。

### Step 8：文档和最终验收

- 新增 `src/sau_n/README.md`；
- 记录运行命令、配置字段、输出尺寸规则、统计和不支持范围；
- 运行 Python/C++/gem5 最小相关测试；
- golden 可用时完成 RTL 逐拍验证；
- 不把该 reference RTL 模型声明为真实 Gemmini 或完整 NPU 性能模型。

## 11. 验收标准

没有 golden 时必须满足：

- fixture 和 resolved config 测试通过；
- logical feed oracle 测试通过；
- C++ 地址、scratchpad、仲裁、FIFO 和状态机测试通过；
- gem5 baseline 和周期性 ready 运行都到达 drained；
- feed 序列、expected vector count 和 FIFO 守恒成立；
- trace 与 stats 一致；
- 状态标记为 `RTL model implementation complete, RTL validation pending`。

golden 可用后还必须满足：

- RTL testbench 自检 PASS；
- resolved config、DUT/testbench hash、固定参数、trace schema 和 simulator 信息与
  manifest 一致；
- RTL feed 序列与 Python oracle 一致；
- gem5 与 RTL 的 state、bank 请求/响应、FIFO、done 和 feed 逐拍一致；
- `rtl_done_cycle` 一致；
- 以下最小 golden fixture 集合必须整体覆盖并全部逐拍通过，不要求各项做笛卡尔积：
  - `W=1/5/16/17/20`；
  - 至少一个非零 `spad_base`，以及 `N>1`、`C>1`；
  - padding、stride、dilation 和尾块；
  - 可由支持范围 workload 产生的同 bank 不同 row 串行；同 bank 同 row 合并使用
    scheduler 定向单元测试覆盖，因为正 stride/dilation 的支持范围不会自然产生
    重复物理输入坐标；
  - 全 padding 向量和包含 invalid 尾 lane 的向量；
  - 周期性 ready 下 FIFO full 且同拍 pop；
  - `done` 时 FIFO 尚未排空并继续运行到 drained；
- 状态标记为 `RTL per-cycle validation passed`。

## 12. 构建与交付约束

- 新实现只位于 `src/sau_n/`、`configs/example/`、`util/im2col/` 和
  `tests/gem5/im2col/`；
- 不复用或修改现有 `src/sau` GEMM timing model；
- 不修改 reference RTL DUT，只允许扩展 testbench；
- 不引入第三方依赖；
- 不增加 watchdog、多周期 SRAM、tag、pending adapter 或通用仿真基础设施；
- 测试超时由外部测试框架负责，不进入模型功能；
- gem5 编译只由开发者手动执行增量构建；agent 在 Step 5 后暂停并等待构建结果，
  然后使用成功构建的新 binary 完成相关验证；
- golden 交付前不得声称 RTL 逐拍验证完成；
- 不做无关重构、格式化、清理或删除。
