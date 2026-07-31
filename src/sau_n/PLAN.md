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

## 13. 当前活动阶段：共享 Scratchpad 到 16x16 SA 的双缓冲数据流

本节是原 Im2Col reference model 和已经完成的融合 pipeline 之后的新活动计划。
前面的独立 Im2Col 计划仍作为历史交付记录保留；本节只规划下一阶段的共享
scratchpad、权重预取、SA 计算和 D 区写回，不修改 CPU、DRAM 或 RTL DUT。
第 12 节中“不增加多周期 SRAM、tag、pending adapter”的限制只约束已经完成的
历史 Im2Col/reference 阶段；第 13 节允许且只允许本阶段明确列出的固定一拍
scratchpad in-flight metadata、B0/B1 buffer 和有限 D pending queue，不扩展为
通用仿真基础设施。

### 13.1 目标

在现有 gem5 `StreamingConvPipelineModel` 基础上，尽可能复现 Mikui/OpenGeMM
风格的片上数据通路：

```text
A 区 activation ──> Im2Col/A FIFO ───────────────┐
                                                 ├─> 16x16 SA
B 区 weight ────> B double buffer / prefetch FIFO ┘
C 区 int16 bias ────────────────────────────────> bias input

SA wide accumulator ──> quantize/saturate ──> D 区 int8 output
```

本阶段需要回答并量化以下问题：

- A、B 共用 16-bank 单操作 scratchpad 时，bank conflict 对 SA 输入吞吐的影响；
- B 双缓冲能否隐藏短时预取延迟，以及在 A 占满 bank 带宽时何时必然产生 bubble；
- 权重在不同 spatial tile 之间复用时，B 读取量和总周期能减少多少；
- D 写回与 A/B 访问竞争时，输出暂存是否会产生额外反压。

本阶段所说的 Mikui/OpenGeMM 风格是数据复用方式相似，不宣称端口和状态机逐拍
等价。目标计算数据流固定为：A 从 shared scratchpad 读取并经过 Im2Col/A FIFO，
B 从 scratchpad 外的本地 active buffer 读取，两者配对后送入 SA。完整 K 权重已经
驻留且 A FIFO 能连续供数的配置，必须能够连续 K 拍产生 SA input fire；小容量
buffer 或 scratchpad 冲突导致数据未就绪时，使用现有 elastic-bubble 协议显式停顿。

### 13.2 已确认的设计选择

- 保留项目自有 `sau_array_16x16.sv` 及其当前 gem5 周期模型，不切换为 Mikui
  `SA_ENGINE` RTL；
- 以 `StreamingConvPipelineModel` 作为主改造对象，已有 Im2Col-only、tile buffer
  和 pipeline model 继续作为回归基线；
- 暂时不建模 CPU、DRAM、DMA、总线和外部内存请求；A/B/C 的初始内容由模型启动时
  预加载到共享 scratchpad；
- 共享 scratchpad 固定为 16 banks、每 bank 8-bit element；每个 bank 每周期只能
  接受一次读或一次写，不能同周期读写；
- scratchpad 读采用一拍请求/一拍响应的周期模型；bank 仲裁、响应返回和 FIFO
  消费必须显式记录；
- A 区存 activation，B 区存 weight，C 区存 bias，D 区存 output；不再使用旧的
  C/D 语义；
- A、B 使用 int8；乘积使用 int16；bias 使用 int16；SA 累加保持当前 signed
  24-bit 数值契约，并在 C++ 中使用 int32 容器承载；
- 中间累加结果不得提前截断为 int8；完成 bias、cutbit、量化和饱和后，最终 int8
  结果才写入 D 区；
- B 权重在多个 spatial tile 之间保持有效并复用，不因每个 activation tile 完成
  而重新生成或无条件清空；
- 不在同一个 16-bank scratchpad 中新增一份重复的 `B_reuse` 地址区；B 的复用区
  建模为 scratchpad 外的本地 B0/B1 buffer。若后续要研究复制 B 区的收益，必须作为
  单独实验并显式计入复制读写带宽；
- D pending queue 是容量有限、以一个 SA output row 为 entry 的本地队列；其深度
  作为 resolved config 参数显式建模，不能使用无限 host-side 容器隐藏写回反压；
- streaming fixture 使用可选的 `shared_spad` 子对象承载本阶段新参数；
  `streaming_fixture` 在调用现有 common pipeline resolver 前单独提取该子对象，
  不改变 common conv fixture 的严格字段集合和已有 golden hash；
- 未提供 `shared_spad` 时，默认使用自动连续区域布局、每个 B buffer 深度为 `K`
  vector、启用 weight reuse、D pending 深度为 1 row、仲裁策略为
  `A > D > B`。现有 schema version 1 streaming fixture 必须继续可加载；
  streaming canonical resolved config 和 SHA256 必须包含补全后的所有默认值。

### 13.3 当前模型缺口

当前 `StreamingConvPipelineModel::buildSauInputs()` 直接调用
`weightValue()` 生成 B，权重没有经过 scratchpad、bank 仲裁或任何预取 buffer；
输出通过 host-side `collectedOutputs` 收集，也没有写入 D 区。当前
`BankedScratchpad` 只有独立的读写 API 和组合响应，没有共享 A/B/C/D requester、
单操作端口仲裁、读延迟或周期性写回状态。

因此本阶段不能只替换 `weightValue()`。需要把 A、B、C、D 的地址、读写请求、
buffer 状态和 SA 握手纳入同一个周期状态机。

### 13.4 Scratchpad 区域和数据布局

所有区域都使用 resolved config 中的独立 bank-local row base/limit，启动时检查
区域不重叠和每 bank row footprint。现有 fixture 未提供新字段时，默认按
`A -> B -> C -> D` 顺序从 A 的现有 `spad_base` 连续推导；显式配置可以覆盖这些
默认值，但不能绕过重叠和总容量检查。第一版使用以下逻辑布局：

```text
A 区：现有 CHW activation layout，由 Im2Col 地址生成器读取
B 区：按 k = c * kernel_h * kernel_w + kh * kernel_w + kw 排列
     每个 k 对应一个 16-lane weight vector，lane 是 output channel
C 区：每个 output-channel bias 使用两个连续 8-bit row
     low byte 在前，high byte 在后，重新拼成 signed int16
D 区：每个输出元素写一个最终 int8，按 output tile 的 spatial row 和 channel lane
     计算唯一 bank/row 地址
```

在当前 output-channel 不超过 16 的配置下，B 的一个 weight vector 可以映射为
16 个 bank 的同一逻辑 row；这会有意暴露 A/B 都访问全部 bank 时的冲突。D 的
具体 row 编码必须由独立地址 helper 统一生成，不能在 writer 和测试中各写一套
公式。

C bias 的读取是首次 launch 前的两次 16-bank byte sweep，读完后在当前
output-channel tile 的 bias register 中保存；同一 output-channel tile 的后续
spatial tile 不重复读取 C。只有 bias 全部返回后才允许发出 SA instruction。
首次 C 读取作为 launch-critical 初始化阶段执行，在 C 完成前暂停 A producer、
B prefetch 和 SA launch，因此不需要把 C 隐式塞入正常 `A > D > B` 仲裁优先级。
D 写回使用 byte write，不能把 int32/int24 中间值直接写入 D。

### 13.5 A/B bank 仲裁和 SA 输入契约

每周期按以下顺序处理 old state、上拍响应、working state、请求生成、仲裁、
SA input fire 和 next-state：

1. 使用 old in-flight tag 消费上拍获准的 read response，并清除已消费的 old tag；
2. 把 response 应用到 old A collect state、B buffer 或 C bias register，得到本拍
   working/collected state；
3. Im2Col 按 working S1 的完成状态选择本拍 A request context：
   - working old S1 尚未完成：为其中仍未完成的 lane 产生下一轮请求；
   - working old S1 已完成、可以退休到 S2，且 old S0 可以前移：同拍将 old S0
     转成 incoming S1，并直接为 incoming S1 产生第一轮请求；
   - working old S1 已完成但不能退休：保持 old S0/S1，不为下一 vector 发请求；
   任何分支都不能从未更新的 old S1 重发已经完成的 lane；
4. B prefetch engine 从 working B state 为仍未完成的 entry/lane 产生 read request；
5. D writer 从 old D queue head 的 pending-write mask 产生 byte write request；
6. 共享 bank arbiter 对每个 bank 只选择一项操作，并把本拍获准 read 的新
   in-flight tag 锁存到 next-state；
7. 只有 old-state A FIFO head 和 old-state active B buffer 中相同 K index 的 entry
   同时 ready，才向 SA
   发起一次 `inputValid`；否则保留 A/B 状态并产生真实的 SA input bubble；
8. SA 输出只进入 next-state D pending queue；D writer 本拍不处理新产生的输出。

每个获准的 bank read 必须锁存至少以下返回信息：

```text
requester = A / B / C
destination = A vector/lane，或 B0/B1 + entry/lane，或 bias byte/lane
request identity = tile identity + K index（适用时）
```

每个 bank 最多保存一个等待下一拍返回的 read。没有对应 in-flight tag 的 response
不得被消费；部分 bank 返回只能更新对应 lane，不能提前完成整个 A/B vector。
working state 只用于避免重复发出已经完成的 A/B/C lane 请求；本拍 response 新完成
的 A/B vector 仍要到下一拍成为 old-state ready entry 后才能被 SA 消费。本拍
SA output 同样只能在下一拍成为 D writer 的 old head，不实现 response-to-SA 或
SA-output-to-D-write 的组合 bypass。

shared A 模式固定采用单 S1 context 的 turnover 流水，而不是完整 A tile buffer：

```text
cycle t：
    为 current S1 发出 read request

cycle t+1（current S1 的 response 已使其完整且 S2 可接收）：
    current S1 -> S2
    old S0 -> incoming S1
    同拍为 incoming S1 发出第一轮 read request
    next in-flight tag 指向 incoming S1 identity
```

因此 conflict-free、S2/FIFO 无反压且 A request 获准时，首次 request 后每拍都可以
完成一个 A vector，稳态 A initiation interval 为 1。若 current S1 需要多个
bank round、S2/FIFO 反压或 A grant 被拒绝，则 turnover 自然暂停并产生可观测
stall；不得用额外隐藏端口维持 II=1。

默认仲裁优先级为：

```text
A critical read > D pending write > B background prefetch
```

但优先级必须做成统计可见的策略参数；不能默默把 B prefetch 当成额外端口。
如果 A 当前向全部 16 个 bank 发读请求，B refill 只能等待；双缓冲可以吸收已经
发生的 burst latency，但不能突破共享 scratchpad 的长期带宽上限。

在本阶段保留现有 SA 的 elastic-bubble 协议。`inputValid` 只在 A/B 配对完成时
置 1，SA 不消费不完整的 A/B pair。每个 SA input fire 仍代表一个 K 维 MAC step，
不能因为出现 bubble 而重复或跳过 K index。

### 13.6 B0/B1 双缓冲协议

每个 B buffer 保存若干个 16-lane weight vector，并维护：

```text
valid/ready entry mask
chunk_base_k
chunk_length
fill pointer
consume pointer
number of complete vectors
weight tile identity
```

运行规则如下：

```text
active buffer：提供 nextExpectedK 对应的 weight vector
inactive buffer：仅在需要后续 chunk 或下一 weight tile 时，通过 B 区和 arbiter 填充
active 消费到 chunk 边界且 inactive 的下一 chunk 完整：交换 active/inactive
下一 chunk 未就绪：暂停 SA input fire，记录 b_buffer_stall
```

读返回采用 entry mask：一个 weight vector 的 16 个 byte 全部返回后，entry 才
能标记为 ready，不能因为部分 bank 返回就提前送入 SA。

B buffer depth 是设计空间参数，当前不冻结为单一最终值。实现必须支持至少两个
buffer，并允许扫描不同的每 buffer vector 数量。每个 entry 除数据和 ready mask
外，还必须保存 weight tile identity 和全局 K index，防止交换后错配。

每个 buffer 只描述当前 weight tile 的一个连续 K chunk：

```text
local_slot in [0, chunk_length)
global_k = chunk_base_k + local_slot
chunk_base_k + chunk_length <= K
chunk_length <= configured_buffer_depth
```

同一 weight tile 的 B0/B1 ready chunk 不得包含重复 global K；超出
`chunk_length` 的物理 slot 必须保持 invalid。`nextExpectedK` 是唯一消费游标，
active buffer 必须是 identity 匹配且区间覆盖 `nextExpectedK` 的 buffer；SA 只能
读取 `local_slot = nextExpectedK - chunk_base_k`，不能仅根据 consume pointer
猜测全局 K。

容量和复用语义固定如下：

```text
B0/B1 中 identity 匹配的有效 entry 覆盖完整 K index 区间 [0, K)：
    允许保留完整 weight tile，在下一个 spatial tile 重置消费序列并直接复用。
    两个 buffer 的 ready chunk 必须是不重叠的连续分区；未使用 slot 保持 invalid。

B0/B1 未覆盖完整 K index 区间：
    buffer 只作为分块预取/解耦存储；被后续 refill 覆盖的 K entry 必须在下一个
    spatial tile 从 B 区重新读取，不得计为 weight reuse hit。
```

第一版功能测试使用最小深度验证交换和 refill；性能测试比较小块、半 tile 和完整 K
驻留容量。必须至少有一个完整 K 驻留 profile，用于验证计算阶段由 A FIFO 和 active
B buffer 每拍向 SA 提供一组数据。

首次 B 填充从 pipeline 启动后开始，允许与 Im2Col/A FIFO 生产重叠；不能把全部 B
预先搬完作为隐藏的初始化步骤。只有当前配置实际保留了完整 K 权重序列，才能在
跨 spatial tile 时保留内容并重置消费位置；小容量配置按上述规则重新读取缺失
entry。只有 weight tile 或 output-channel tile 改变时，完整驻留配置才必须启动
新的 B fill/交换。

B active 选择、边界交换和覆盖规则固定为：

```text
tile 开始或跨 spatial tile 重置：
    nextExpectedK = 0
    active = identity 匹配且 chunk 覆盖 K0 的 buffer

普通 input fire：
    读取 active[nextExpectedK - active.chunk_base_k]
    nextExpectedK++

input fire 消费 active chunk 最后一个 K，且 nextExpectedK < K：
    若另一 buffer 已 ready 且 chunk 覆盖新的 nextExpectedK：
        周期末交换 active，下一拍继续供数，不插入边界 bubble
    否则：
        保持 nextExpectedK，暂停后续 input fire，等待另一 buffer ready

完整 K 驻留期间：
    从首次 launch 到最后一个 spatial tile 完成，不得 refill 或覆盖共同覆盖
    [0, K) 的 B0/B1 entry；跨 spatial tile 只重置 nextExpectedK 和 active。

小容量分块模式：
    buffer 变为 inactive 后，才允许改写其 chunk_base_k/chunk_length 并 refill
    后续尚未消费的连续 K chunk；进入下一个 spatial tile 时，已被覆盖的早期
    chunk 必须重新从 B 区填充。
```

active 交换只在周期末更新 next-state；当前拍仍从 old active 读取，下一拍从新的
active 读取，所以不需要同拍双 buffer 读取端口。若另一 buffer 已完整 ready，交换
本身不产生 SA bubble。

首次 SA launch 门槛按配置冻结为：

```text
完整 K 驻留配置：
    C ready
    && A FIFO head 是当前 tile 的 tile-first / K0
    && 当前 weight tile 的所有 K index [0, K) 已经 ready

小容量或关闭 reuse 的配置：
    C ready
    && A FIFO head 是当前 tile 的 tile-first / K0
    && active B chunk 中 K0 已经 ready
```

只有门槛满足才能从 consumer `Idle` 进入 `Launch`。完整 K 的首次 fill 仍通过运行时
scratchpad 请求、仲裁和一拍响应完成并计入总周期，不能作为隐藏初始化；因此等待
全部 B ready 不等于启动前免费预加载。

### 13.7 数值和写回契约

SA 数值路径冻结为：

```text
int8 A × int8 B -> int16 product
int16 product + wide accumulator -> signed 24-bit saturated accumulator
final MAC/drain -> bias phase adds signed int16 C bias exactly once
final accumulator -> cutbit/quantize/saturate -> int8 D
```

C bias 在首次 launch 前从 C 区读入并由 SA instruction config 锁存，但不在首个 K
step 提前加入。保持现有 `SauCycleModel` 时序：最后一个 MAC 输入完成、阵列 drain
后进入独立 bias phase，每个有效 PE 只加入一次 bias。

D writer 只接收最终 int8。D pending queue 的每个 row entry 保存 output
coordinate、valid-column mask、每 lane byte 和 pending-write mask。D writer 只
处理 old head；若本拍所有剩余 lane 都获准写回，则 `headWillRetire=1`。queue 的
push-ready 和 SA grant 固定为：

```text
dPushReady = !dQueueFull || headWillRetire
outputGrant = periodicOutputReady && dPushReady
```

因此允许同拍 dequeue old head 并 enqueue 一个新 SA output row，包括默认 depth 1
的队列；新 row 本拍不能直接写 D，下一拍才成为 old head。若 old head 不能完整
retire 且队列已满，必须反压 SA，不能覆盖或丢弃输出。

运行结束时，输出文件和 `outputs()` 必须由已完成写回的 D 区重建，或者对 D 区做
等价的 post-drain readback；host-side 收集值只能作为检查元数据，不能绕过 D 写回
成为最终结果来源。post-drain readback 不计入运行时 scratchpad 带宽统计。

测试必须同时检查：

- 中间 accumulator 没有提前截断为 int8；
- C 的 low/high byte 拼接和负数符号扩展正确；
- bias 只对每个 output tile 正确加入一次；
- D 地址唯一，没有 duplicate write 或漏写；
- 输出量化结果与现有 SA 数值模型一致。

### 13.8 预计修改范围

第一阶段预计只影响以下已有文件及其直接测试：

- `banked_scratchpad.hh/.cc`：区域布局、周期读写请求、读延迟、单操作 bank
  约束和统计接口；
- `pipelined_im2col_model.hh/.cc`：把当前内部私有 scratchpad 访问拆成可由
  shared arbiter 驱动的 A request/grant/response 接口，同时保留 standalone
  Im2Col 的现有组合响应路径和逐拍 timing；
- `streaming_conv_pipeline_model.hh/.cc`：共享 scratchpad、A/B/C/D requester、
  B0/B1 状态机、D 写回和 SA 输入配对；
- `streaming_pipeline_contract.hh/.cc` 或等价配置类型：A/B/C/D base、buffer
  depth、D pending depth、weight reuse 和 bank arbitration 参数；
- `sau_types.hh/.cc`：必要的周期观察字段和统计字段；
- `StreamingConvPipeline.py`、`streaming_conv_pipeline_timing.hh/.cc` 和
  `streaming_conv_pipeline_io.hh/.cc`：SimObject 参数、gem5 stats 和逐周期 trace；
- `banked_scratchpad.test.cc`、`pipelined_im2col_model.test.cc`、
  `streaming_conv_pipeline_model.test.cc` 以及必要的新增最小单元测试；
- `configs/example/streaming_conv_pipeline_timing.py`、
  `util/conv_pipeline/` 中直接相关的 fixture/config/verifier 及其测试，以及
  `tests/gem5/streaming_conv_pipeline/` 中的直接回归；
- `SConscript` 仅在确有新增源文件或测试目标时登记，修改前先确认构建影响。

本阶段不修改 `src/sau/`、reference RTL、CPU/DRAM 配置、外部依赖或无关的 gem5
组件。

### 13.9 实施步骤

#### Step A：冻结共享数据结构和地址布局

- 扩展 resolved pipeline config，加入 A/B/C/D base、区域大小、B buffer depth、
  D pending row depth、是否保持 weight reuse 和 bank arbitration 策略；
- 在 streaming fixture 中单独解析可选 `shared_spad` 子对象，保持 common conv
  fixture 和既有 golden hash 不变；streaming resolved config 输出及 SHA256 包含
  补全后的实际值；
- 实现统一 A/B/C/D 地址 helper；
- 预加载 A activation、B weight 和 C bias，初始化 D；
- 为 C int16、D int8 和区域不重叠增加边界测试。

#### Step B：把 A 路径迁移到共享 scratchpad

- 为 `PipelinedIm2ColModel` 拆出 A request、grant 和下一拍 response 接口；
- standalone Im2Col 保留现有构造、组合响应、S1/request/completed-read-round
  逐拍 timing 和既有测试观察；只有新 shared 模式使用外部一拍 response，且不再
  持有私有 activation scratchpad；
- shared 模式先把 response 应用到 working S1，再从 working S1 生成下一轮请求；
- working S1 完整并可退休时，实现 old S0 -> incoming S1 的同拍 turnover request，
  使 conflict-free shared A 在无下游反压时达到稳态 II=1；
- 保持现有 Im2Col logical sequence 和 FIFO 行为；
- 将 A 请求接入共享 bank arbiter；
- 保证原有 Im2Col 输出内容和坐标顺序不变。

#### Step C：实现 B source、B0/B1 buffer 和预取

- 删除 `buildSauInputs()` 中直接的 `weightValue()` SA 输入路径；
- 将确定性权重先写入 B 区；
- 实现 B vector 地址生成、逐 bank read、读响应收集和完整 entry ready；
- 实现 `chunk_base_k + local_slot -> global_k`、按 `nextExpectedK` 选择 active、
  周期末零气泡交换、跨 spatial tile 重置到 K0、容量相关的 reuse/refill 和 stall；
- 完整 K 驻留期间禁止覆盖共同覆盖 `[0,K)` 的 B0/B1 entry；小容量模式只允许
  inactive buffer 改写为后续连续 chunk；
- 按完整 K 与小容量两类规则把 B ready 条件接入 consumer begin-launch 门槛；
- 所有 refill 请求必须经过真实 bank 仲裁。

#### Step D：实现 C bias 和 D output

- 从 C 区按两个 byte 读取并拼接 int16 bias，送入现有 SA config；
- 保持现有 SA 最终 MAC/drain 后的独立 bias phase，不在首 K step 提前加 bias；
- 将 SA 的最终 int8 output row 写入容量有限的 D pending queue；
- 实现 D queue old-head 完整写回时的同拍 dequeue/enqueue，并以
  `periodicOutputReady && (!full || headWillRetire)` 驱动 SA `outputGrant`；
- D writer 按单 bank 单操作规则写回 D 区；
- 维护 D 区输出索引、tail mask、duplicate/missing write 检查，并从 D 区重建最终
  NCHW 输出。

#### Step E：整合周期状态机和统计

- 保持 old-state/next-state/commit 语义；
- 为每个 bank 锁存获准 read 的 requester/destination in-flight tag；
- 明确 SRAM request、下一拍 response、B buffer fill、SA input fire、SA output
  和 D write 的相对周期；
- trace 至少记录 D queue cycle-start occupancy、head pending-write mask、
  D request/grant mask、enqueue/dequeue，以及 B entry hit 与跨 spatial tile
  weight reuse hit；
- 增加 A/B/C/D 请求数、bank 冲突、B buffer occupancy、B refill stall、SA bubble、
  D pending occupancy 和写回 stall 统计；
- 扩展 drained 条件：producer 和 A FIFO 已空、SA idle、无 in-flight scratchpad
  read、B fill 已停止、D pending queue 已空且所有 D write 已完成；
- 维持现有 output-ready/backpressure 的外部语义。

#### Step F：验证和性能对照

- 先做单 vector、单 spatial tile、无 bank conflict 的功能测试；
- 再做 A/B 都需要从 scratchpad 读取且同时占用 16 个 bank 的定向 conflict
  baseline，确认无复用时稳态每两个 bank access slot 才能形成一个 A/B pair；
- 完整 K 驻留测试必须确认首次填充完成且 A 无冲突时，连续 K 拍 SA input fire，
  并在下一个 spatial tile 复用同一 B tile 而不重复读取 B；
- launch 定向测试必须确认完整 K 配置等待 `[0,K)` 全部 ready，小容量配置只等待
  包含 K0 的 active chunk ready；
- 多轮 A read 测试必须确认 response 完成的 lane 不会在下一拍被重复请求；
- shared A turnover 测试使用 `C>1` 且 K 大于 S0/S1/S2/A FIFO 的总暂存容量，
  确认 conflict-free、无下游反压时首次 response 后每拍完成一个 A vector，稳态
  II=1；测试不能只使用可能被启动前积压掩盖的最小 K；
- 小容量 B0/B1 测试必须确认被覆盖的 K entry 会重新从 B 区读取，不能误报 reuse；
- B chunk 测试必须逐次检查每个 tile 消费的 weight identity/global K 严格为
  `0..K-1`，覆盖半 K 分区的零气泡交换、跨 spatial tile 重置到包含 K0 的 buffer、
  完整驻留禁止覆盖，以及另一 chunk 未 ready 时的真实 bubble；
- “无 B reuse”对照使用 depth 1 且关闭 reuse，只有一个不可复用的 response
  staging entry，每个 K、每个 spatial tile 都重新读取 B；比较该 baseline、
  B0/B1 分块预取和完整 K 复用；
- 覆盖 C 负 bias、D int8 饱和、K 尾部、output-channel 尾部、D 写回竞争和周期性
  output backpressure；
- depth 1 D queue 测试必须同时覆盖 old head 完整 retire 时的同拍 dequeue/enqueue，
  以及 old head 部分写回时对 SA 的反压；
- 不主动执行 gem5 编译；完成源码和单元测试后，提供符合本目录约束的增量构建命令，
  等待开发者返回构建结果。

### 13.10 必须记录的统计

```text
spad_read_requests_a/b/c
spad_read_grants_a/b/c
spad_read_responses_a/b/c
spad_write_requests_d
spad_write_grants_d
per_bank_read_cycles
per_bank_write_cycles
per_bank_read_write_conflicts
b_buffer_fill_vectors
b_buffer_consumed_vectors
b_buffer_hit_vectors
b_buffer_empty_cycles
b_buffer_switches
b_prefetch_stall_cycles
sa_input_fire_cycles
sa_input_bubble_cycles
d_pending_peak
d_write_stall_cycles
weight_reuse_hits
```

`request` 统计 requester 提出的逐 bank 操作，`grant` 统计 arbiter 实际接受的逐
bank 操作，`response` 统计下一拍实际返回的逐 bank read；vector fill/consume 只在
对应完整 vector 条件满足时计数。`b_buffer_hit_vectors` 表示当前 K 在 buffer 中
命中，`weight_reuse_hits` 只表示相同 weight tile 的 entry 被后续 spatial tile
再次消费，两者不能混计。所有统计都必须能由逐周期 trace 中的 requester mask、
grant owner、response owner、B identity/K index、D queue cycle-start occupancy、
D head pending mask 和 enqueue/dequeue 重算，不能只输出一个无法解释的总周期。

### 13.11 风险和暂不冻结的参数

- 如果 A 每周期占满 16 个 bank，B0/B1 refill 没有剩余带宽；双缓冲不能保证 SA
  每拍输入，模型必须保留 bubble，而不是假设存在隐藏的第二读端口；
- B buffer 每 buffer 的 vector 数量尚未最终确定，必须保持参数化；只有实际保留
  完整 K 序列的配置才能声明跨 spatial tile 完整复用；
- D pending row depth 保持参数化，但必须是有限正数；不同深度的结果不能混为同一
  性能配置；
- A/B/C/D 的具体 row base 和 D 输出 row 编码在 Step A 统一冻结前不得分散写入
  各模块；
- 当前 SA 模型是项目自有 16x16 阵列的周期模型，不等价于完整 Gemmini/OpenGeMM
  端口结构；OpenGeMM 风格的独立 A/B 读端口或 Gemmini WS 的 PE 内部权重驻留，
  作为后续对照模式，不在本双缓冲基线中隐式加入；
- 当前 shared scratchpad 仍是 gem5 architectural timing model，不宣称已经复现
  真实 SRAM macro 的物理端口、仲裁延迟或功耗。

### 13.12 本阶段完成标准

只有同时满足以下条件，才能声明本双缓冲阶段完成：

- A、B、C、D 均通过共享 scratchpad 地址和周期端口访问；
- `weightValue()` 不再直接作为 SA 的运行时 B 输入；
- B0/B1 能正确填充、消费、交换，并在未就绪时产生可观测 SA bubble；
- conflict-free shared A 在 S2/FIFO 无反压时通过 S1 turnover 达到稳态 II=1，
  且长 K 测试证明结果不是有限启动积压造成；
- 完整 K 配置只在 C、A tile-first 和 B `[0,K)` 全部 ready 后 launch；小容量配置
  只在 C、A tile-first 和包含 K0 的 active chunk ready 后 launch；
- 完整 K 驻留配置在 A 无冲突时连续 K 拍产生 SA input fire，并在多个 spatial tile
  间复用同一 weight tile 且不重复读取 B；
- 每个 tile 的 B identity/global K 消费序列严格为 `0..K-1`；ready chunk 边界交换
  不产生 bubble，跨 spatial tile 正确重新选择覆盖 K0 的 buffer，完整驻留 entry
  在复用期间不被 refill 覆盖；
- 小容量配置按实际保留 entry 复用，并对被覆盖 entry 重新读取 B；
- C bias 以 int16 正确加入，D 只保存最终 int8；
- D pending queue 满时正确反压 SA；D 区无重复写、漏写，由 D 区重建的输出与现有
  数值 oracle 一致；
- depth 1 D queue 在 old head 完整 retire 时支持同拍 dequeue/enqueue，部分写回
  时不错误释放 slot；
- drained 时不存在 A/B/C read response、B fill、D pending entry 或 D write
  等未完成事务；
- 16-bank 单读/写端口约束、A/B 冲突和 D 写回冲突均有定向测试；
- 现有 Im2Col、SA、output-ready 和 drained 回归不受破坏；
- gem5 增量构建和最小相关测试由开发者手动执行并返回结果；
- 未验证内容和仍未冻结的 B buffer 参数在交付说明中明确列出。
