# PLAN3：CSR 驱动的 Int8 GEMM 功能与数据通路 RTL 对齐

## 状态

**Steps 0–5 已在冻结的 Reuse-A 支持域内完成；Step 6 increment 4 已开始，
Flow3 继续 explicit fail-fast。Step 5 increment 4 已将 native unload
payload 接入 strict memory write submission；cutbit-8 与 cutbit-1 strict
final-memory checkpoint 均 1024/1024 bytes 匹配。Increment 5 已启用 Flow1
strict per-tick runtime 和 multi-flow/multi-instruction array tile 生命周期；
64x160x64 两条 command 各自 690 cycles，最终 4096/4096 bytes 匹配。
Flow2 K512 `[flow2, flow0]` strict runtime 也已闭环：resident A 外部读取复用
`register_addr.sv` 的 x/y/channel 地址程序，两条 command 分别精确为 569/685
cycles，间隔 93 cycles，第一条无 result/write，第二条各 32 beats，最终
1024/1024 bytes 匹配。quick 回归扩展为 25 suites、69/69 checks。
K768 `[flow2, flow2, flow0]` 进一步验证连续两次 retain：三条 command 精确为
569/569/685 cycles，间隔 82/89 cycles，只有最终 command 产生 32 个
result/write beats，最终 1024/1024 bytes 匹配。真实 payload 读写分别由
`payloadReadBeats`/`payloadWriteBeats` 统计，并由 functional verifier 对照
architecture trace；Step 6 chain fixture 加入后 quick 回归为 27 suites、
75/75 checks。
`[flow2, flow1]` diagnostic 时序有效，但输出为
32x32 tile 内顺时针旋转 90°而非纯转置；无留存的 64x160x64 case 复现相同
tile-local RTL 行为。2026-07-28 用户确认 gem5 以当前 RTL 语义为对齐目标，
不额外要求数学上的整矩阵纯转置。64x160x64 fixture 已打包并成为 permanent
byte-exact functional regression。**

本计划承接 `PLAN2.md` 已完成的 CSR 解码、逐拍控制、地址请求和时序对齐工作。
`PLAN2` 的完成结果继续作为时序回归基线。2026-07-27 用户根据仍在更新的 RTL
工程明确冻结当前 reuse 支持域为 `reuse_mode=01`（Reuse-A）；其他 raw reuse
值继续无损解码并保留已有接口/实现，但在 RTL 稳定前 deferred，不属于当前阶段
功能验收范围。

## 1. 目标

在暂不接入 CPU 的前提下，通过一个 workload JSON 提供：

- CSR 配置来源；
- 初始 SRAM/memory image；
- 一个或多个顺序执行的 SAU command；
- 可选的 RTL golden 结果。

SAU 必须按 CSR 配置复现 RTL 的以下完整链路：

```text
CSR start
  -> RTL 地址发生器决定每拍读取的地址
  -> 真实 256-bit 数据从 SRAM 边界返回
  -> register_file_in / padding / feeder
  -> transposer
  -> 32x32 systolic array PE 逐拍乘加
  -> result serializer
  -> register_file_out 累加、截断和饱和
  -> 真实 256-bit 数据写回 SRAM
```

最终结果以写回后的 memory bytes 为准，必须与相同 RTL testcase 的最终 memory
逐字节一致；不能用直接计算 `C=A*B` 的抽象 GEMM 捷径替代上述硬件数据通路。

## 2. 已确认的范围

### 2.1 本阶段支持

- operation：仅 GEMM / MATMUL。
- precision：仅 signed int8 输入路径，即 `shift_flag=0`。
- 除 reuse 的临时范围冻结外，其他参与 int8 GEMM 的 CSR 字段都必须按 raw 配置
  产生真实行为，不能固定为当前 fixture 的数值。`reuse_mode=00/10/11` 仍按 raw
  值解码，但完整 workload 必须在产生结果或可信性能统计前以
  `rtl_legal_unimplemented` fail-fast；不得回退到 Reuse-A。
- 启动方式：JSON 引用或直接包含 raw CSR writes；两种形式都先规范化为同一种
  `SauCsrWrite` 序列，再由 `SauCsrConfig` 按 `csr.sv` 位域 replay/decode。
  `csr_snapshot.json` 只作为解码结果的交叉检查，不直接启动 command。
- command：允许多个顺序 command，但同一时刻只运行一个。只有按 RTL
  ready/busy/start 条件实际 accepted 的 start 才能形成 command；gem5 不得把
  busy 期间未接受的 start 私自排队并在以后补执行。
- memory：所有 command 共享一个持久 memory image。前一 command 的写回对后续
  command 可见。
- 时序分为两种合同：
  - **strict fixed-SRAM：** 在相同 raw CSR、memory image、clock 和 memory
    request-accept/response schedule 下，所选资源边界事件及 command-done
    总拍数必须与 RTL 一致；
  - **timing-memory：** 总拍数允许因 gem5 memory/xbar 的 latency、retry、排队和
    outstanding 限制不同于 fixed-SRAM RTL，但额外周期必须能完全归因到可观测的
    memory stall/backpressure；相同 request-accept/response schedule 下仍应回到
    strict 拍数。
- 数据 trace：默认关闭以优先保证仿真速度。本阶段只预留接口，不把完整内部数据
  trace 作为完成条件。

RTL 合法性与模型验证成熟度必须分开：

| 状态 | 含义 |
| --- | --- |
| `rtl_illegal` | 权威接口明确非法或切换到本阶段外算子，拒绝 |
| `decoded` | raw CSR 已按 RTL 位域解码并完整保留 |
| `rtl_legal_unimplemented` | RTL 合法，但命中的资源路径尚未实现到可执行级别 |
| `resource_timed` | 相关资源的容量、transaction 和周期已与 RTL 边界对齐 |
| `data_functional` | 相关模块的输入/输出 payload 已 bit-exact 对齐 |
| `end_to_end_validated` | 最终 write payload 和 memory 已与 RTL 对齐 |

合法的 int8 GEMM 配置不能仅因为当前 golden 尚未闭合就被当成
`rtl_illegal`。实施和测试可以按模块逐级提升验证成熟度；仿真输出必须明确当前
配置已通过的最高级别，不能把部分实现的结果标成 end-to-end correct。

完整 workload 命中 `rtl_legal_unimplemented` 时必须在产生任何结果或可信性能统计
前 fail-fast，并报告缺失的 RTL 路径/资源；不能回退默认 mode 或继续输出错误的
final memory。模块级 bring-up 可以只运行已实现边界，但必须明确其最高成熟度，
不得冒充完整 command。

模型不得判断或修正用户“本来想配置什么”。对当前支持域内的 raw CSR，即使其
`trans_mode/sa_flow_mode` 与输入数据布局不匹配，gem5 也必须忠实执行该 raw
配置。deferred reuse 配置必须显式 fail-fast，不能被修正或映射到其他模式。
正确性定义为：

```text
gem5(same raw CSR, same memory, same memory handshake/response schedule)
    == RTL(same raw CSR, same memory, same memory handshake/response schedule)
```

不要求错误配置与用户原本期望配置产生相同结果或相同拍数，只要求它们分别与相同
配置的 RTL 对齐。

### 2.2 Int8 GEMM 内必须由 CSR 选择的 RTL 路径

`trans_mode` 必须支持 `SA_pkg.sv` 定义的全部四种模式：

| raw 值 | RTL 名称 | 数据语义 |
| --- | --- | --- |
| `00` | `ABD` | `A * B = D` |
| `01` | `ATBD` | `A^T * B = D^T` |
| `10` | `ABTD` | `A * B^T = D^T` |
| `11` | `ABDT` | `A * B = D^T` |

当前功能支持域只包含 Reuse-A；其余 raw 值为 RTL 更新期间的 deferred 接口：

| raw 值 | 接口语义 | 当前状态 |
| --- | --- | --- |
| `00` | 不复用 | deferred，保留解码和已有实现 |
| `01` | 复用 Operand-A | supported |
| `10` | 复用 Operand-B | deferred，RTL 仅有未稳定接口 |
| `11` | 两个 reuse bit 均置位 | deferred，保留历史 probe 与已有实现 |

deferred 不等于 `rtl_illegal`：模型继续完整保存这些 raw 值和已有资源接口，但在
完整 workload 执行前报告 `rtl_legal_unimplemented`。RTL 稳定后必须重新调查
`00/10/11` 的最终语义，再由用户决定恢复支持或显式拒绝；当前历史 probe 不能
替代届时的最终 RTL 合同。

`sa_flow_mode` 专门控制输出：`0` 正常顺序，`1` 转置顺序，`2` 保留 output
SRAM 数据供下一次计算累加。`trans_mode` 只控制 Operand-A 或 Operand-B 的输入
转置，不参与输出顺序选择。raw `3` 继续无损解码，但其功能语义需单独确认。

`cutbit` 是 5-bit CSR 字段，int8 GEMM 必须支持 RTL 可表示的完整 `0..31` 范围，
包括 `cutbit=1`、现有 fixture 使用的 `cutbit=8`，以及其他合法值。当前 RTL 的
实际行为不是选择一个预设量化配置，而是：

```text
signed 24-bit MAC result
  -> arithmetic right shift by cutbit
  -> detect whether shifted value exceeds signed int8 range
  -> saturate to [-128, 127]
```

模型必须逐项复现 `SA_pkg::sat_truncate_func` 的 `>>>`、符号扩展、overflow 检查和
返回位布局，不能把 cutbit 固定为 8，也不能用浮点缩放或最终统一量化替代。

以下字段同样不能固定为现有 golden 的数值，但必须先区分“GEMM 内自由配置”
和“会把数据通路切换到其他算子”的值：

- `register_mode`、`sa_flow_mode`、`conv_kernal` 和 `stride_flag` 在
  `pe_work_mode=MATMUL, shift_flag=0` 下仍可到达的控制效果；
- `flow_loop_times`；
- padding 与 register valid x/y window；
- input、vertical、register-input、output 的全部
  x/y/flow/instruction step、burst 和 cycle。

实施前必须形成逐字段支持域表，至少记录 raw 位宽、当前 RTL 消费位置、int8 GEMM
合法域、保留/非法条件和对应 golden。特别是 `register_mode=10` 会影响
single-column 路径，`conv_kernal>=3` 会使当前 `sa_feeder` 的 `conv_mode`
成立，不能未经调查就把它们当作普通 GEMM 参数，也不能因为当前 fixture 为 0
就硬编码为 0。

合法 GEMM 值必须按 RTL 工作；会切换到 CONV/DW/INT16 等本阶段外数据通路的组合
必须根据权威接口约束显式拒绝。`pe_work_mode` 必须是 MATMUL，`shift_flag`
必须为 0；卷积、单独转置、矩阵加法和 int16 均明确不在本阶段。

### 2.3 不在本阶段

- CPU 指令 decode、CSR 总线接入、中断和操作系统集成；
- INT16、CONV、独立 TRANSPOSE、ADD；
- SRAM bitcell、模拟电路和物理实现细节；但任何会影响 transaction 接受、端口
  竞争、吞吐、stall 或可见周期的 bank/port 冲突属于本阶段，必须建模；
- 为未实现算子添加通用计算图；
- 默认开启逐 PE、逐 accumulator 的大体积数据 trace。

## 3. 权威来源与正确性原则

功能行为以 `/home/xch/work/npu_lpnpu` 当前 RTL 为权威，主要对应：

- `hardware/src/sa_element/csr.sv`
- `mem_addr.sv`、`mem_ctrl.sv`、`register_addr.sv`
- `register_file_in.sv`、`padding_shifter.sv`、`feeder.sv`
- `hardware/src/sa_execute/sa_feeder.sv`
- `transposer_tiny.v`
- `SA_ENGINE.sv`、`SA_ROW.sv`、`SA_PE_array.sv`、`SA_PE.sv`
- `SA_pkg.sv`
- `hardware/src/sa_element/register_file_out.sv`

当前 elaborated hierarchy 固定为：

```text
SA_CORE
  -> sa_element/feeder + register_file_in
  -> sa_execute/sa_feeder
  -> transposer_tiny
  -> SA_ENGINE
  -> SA_ROW
  -> SA_PE_array
  -> SA_PE
  -> register_file_out
```

`trans2sa_top.v`、`SA_TOP.v`、`SA_row_unit.v`、`SA_pe.v` 属于仓库中仍保留的旧
数据通路文件，不作为本阶段实现依据。只有证明当前 elaboration 实例化了某文件，
才能把它加入权威路径。

### 3.1 建模层次

本阶段实现的是 **CSR 驱动、资源约束感知、数据 bit-exact、边界时序准确的逐拍
高层模型**，不是 SystemVerilog 的逐模块或逐寄存器 C++ 翻译。

必须与 RTL 等价的内容包括：

- CSR 对控制路径和数据路径的实际语义；
- memory request/response 的地址、payload、顺序、accepted/retry 和 outstanding；
- input/output buffer 的有限容量、端口数、占用、RAW 和 backpressure；
- transpose/reuse 资源的 bank ownership、吞吐、阻塞和有效数据顺序；
- systolic array 的并行规模、接受速率、流水延迟、有效列和结果产生顺序；
- 每个有架构影响的定点位宽边界、clear/retain 和 command 间状态；
- writeback 可见性以及 command done 的资源完成条件。

允许抽象的内容包括：

- 不改变功能结果、资源冲突、吞吐、阻塞传播或可见周期的组合 wire；
- 仅服务 RTL 时序收敛、但可以被等价总延迟表示的内部寄存器；
- generate 层级、always block、门控时钟和无架构影响的临时信号；
- 具体的 SystemVerilog 模块/对象数量。实现可用连续数组和统一逐拍更新表示
  32×32 PE 资源，不要求创建 1024 个逐信号 RTL 对象。

若合并某段 RTL 内部状态，必须证明合并后在相关资源边界上的 transaction 顺序、
容量、吞吐、阻塞、payload 和可见周期不变。不得用直接 `C=A*B`、无限容量队列或
预先写死的完成延迟代替资源模型。

### 3.2 资源状态与逐拍推进

“唯一权威状态”指同一个架构事实不能由两套独立进度重复决定，不要求每个 RTL
寄存器都在 C++ 中有一一对应对象。资源职责划分如下：

| 状态/行为 | 唯一权威资源 |
| --- | --- |
| command 阶段和完成判定 | command controller/scheduler |
| read/write 地址循环 | address generator |
| request 接受、retry、response 和 outstanding | memory port |
| input 数据、valid label、容量和端口占用 | input buffer/register-file |
| transpose/reuse 数据、bank 和可用性 | transpose/reuse resource |
| operand token、流水占用、accumulator 和 result token | systolic-array resource |
| output 数据、累加、容量和 unload | output datapath |
| 外部数据的最终可见内容 | 当前运行所选的唯一 memory authority |

控制器可以向资源发出 request/enable，但只有资源实际接受 transaction 时，相关
地址、token 和计数才能共同推进。每拍统一遵守：

```text
读取当前状态
  -> controller/address generator 提出候选 transaction
  -> 各有限资源根据容量、端口、ready/retry 判断是否接受
  -> accepted transaction 同时携带控制元数据和真实 payload
  -> 计算各资源 next state
  -> 在拍末统一 commit
```

`RtlCommandDriverSkeleton` 保留为现有 `01/01` 控制路径的回归 oracle，不与新模型
并行维护另一套功能执行进度。bring-up 阶段用它逐拍检查关键资源边界事件；最终执行
由上述资源模型负责。模型不要求内部状态与 skeleton 或 RTL 逐寄存器相同，只要求
被选为 strict 边界的 transaction、资源占用、payload 和周期等价。

### 3.3 正确性原则

实现遵守以下原则：

1. 地址和取数范围来自 CSR counter/step/burst/valid/last 逻辑，不从 M/K/N
   重新推导运行时行为。
2. 每个 accepted 数据 token 同时携带控制元数据和真实 payload；同一资源只维护
   一套 transaction 进度，避免控制模型和功能模型各自决定数据是否已经移动。
3. 固定位宽运算必须在每个 RTL 截断点执行 signed extension、saturation、wrap
   和 cutbit；不能先用无限精度完成 GEMM 后统一量化。当前 `SA_PE` 的 24-bit
   accumulator 使用 `saturate_add_signed`，不是 wrap；`register_file_out`
   的 16-bit lane 加法、随后 int8 saturation 必须作为另一个独立位宽边界实现。
4. transpose、reuse、array pipeline、serializer 和 output accumulation 分别保留
   会影响容量、竞争、吞吐、阻塞或结果的资源状态；允许合并对这些行为不可见的
   RTL 临时状态。
5. fixture/test ID、M/K/N 或 golden 值不得出现在功能分支中。

## 4. Workload JSON 合同

新增 `--workload-json FILE`。JSON 只负责组合已有事实来源，不复制 CSR 位域：

```json
{
  "schema_version": 1,
  "rtl_contract": {
    "commit": "<git-sha>",
    "row_num": 32,
    "col_num": 32,
    "output_dw": 24,
    "sram_delay": 3,
    "clock_period_ps": 1667
  },
  "manifest": "manifest.json",
  "csr_source": {
    "type": "writes_csv",
    "path": "csr_writes.csv"
  },
  "memory": {
    "format": "rtl_memory_hex",
    "path": "firmware/memory.hex",
    "base_address": "0x0",
    "size_bytes": 1073741824
  },
  "golden": {
    "final_memory": "firmware/memory_expected.hex",
    "compare_ranges": [
      {"address": "0x...", "size_bytes": 1024}
    ]
  }
}
```

等价的 inline raw-write 形式复用同一记录字段：

```json
{
  "csr_source": {
    "type": "raw_writes",
    "writes": [
      {
        "cycle": 0,
        "csr_addr": "0x...",
        "csr_operation": "0x...",
        "csr_wdata": "0x...",
        "accepted": true
      }
    ]
  }
}
```

要求：

- 相对路径相对于 workload JSON 所在目录解析。
- `rtl_contract` 必须绑定生成 golden 的 RTL commit 和所有影响功能/周期的
  elaboration 参数，至少包含 `ROW_NUM/COL_NUM/OUTPUTDW/SRAM_DELAY` 与 clock
  period；若后续路径表发现其他有影响参数，也必须纳入。
- `manifest` 必须明确指向对应 RTL golden package 的 `manifest.json`，相对路径按
  workload JSON 所在目录解析。
- 仿真开始前必须将 `rtl_contract` 与 gem5 resource config、golden manifest
  逐项校验；缺失或不一致立即报错，不能进入 command。
- JSON 由 `configs/example/sau_timing.py` 使用 Python 标准库解析并完成 schema
  校验；C++ SimObject 只接收规范化后的 CSR source、memory image、dump/compare
  路径和范围，不引入新的 JSON 依赖。
- `csr_source.type` 支持 `writes_csv` 和可选的 `raw_writes`。`raw_writes` 直接
  包含与 CSV 相同的 `cycle/csr_addr/csr_operation/csr_wdata/accepted` 记录；
  Python 必须把两种形式规范化为完全相同的 `SauCsrWrite` 序列，C++ 只保留一条
  replay/decode/start 路径。
- `csr_snapshot.json` 是非执行性 artifact，只用于检查 raw replay 后的
  `SauCsrConfig`；不得绕过 CSR 位域、write order 或 accepted-start 语义直接构造
  command。
- memory loader 必须明确 byte order、每行宽度、地址单位和空洞填充值，并用现有
  RTL `memory.hex`/`memory_mod_*.hex` 做交叉测试。
- `size_bytes` 表示可访问地址范围，不表示必须等量分配 backing store。strict
  `FunctionalMemory` 使用稀疏 page/range 存储，不在启动时分配、清零或最终扫描
  完整 1 GiB；未加载空洞按合同填充值，dump/compare 只访问显式 range。
- JSON 可包含比较范围，但不能包含直接作为运行输入的期望矩阵结果。
- 配置不完整、地址越界、重叠格式冲突或不支持的 CSR mode 在仿真开始前报错。
- 旧 `--rtl-profile` timing-only 入口继续可用；功能 workload 是新增入口，不能
  破坏现有 23 个 quick suite。

功能数据只能有一个权威存储，不能让本地 backing store 与 `SimpleMemory` 在同一
运行中分别演化：

- **strict fixed-SRAM 运行：** `FunctionalMemory` 是唯一数据真源。固定延迟响应
  从其中取 256-bit payload，strict write 在 RTL 可见的本地提交边沿更新它。
- **timing-memory 运行：** 下游 gem5 memory 是唯一数据真源。仿真开始命令前，
  使用同一 request port 的 functional packet 将 memory image 装入下游 memory；
  运行时 read payload 只取 timing response packet，write payload 只通过真实
  timing write packet 提交。
- timing-memory 的下一 command 和最终 compare 必须等待全部 write response，
  或证明目标 memory 在 request accepted 时已经具有同等可见性；不能在未定义的
  accepted 时刻额外更新一份镜像。
- timing-memory 最终结果使用 functional read packet 从下游 memory 读回；
  strict 结果直接从 `FunctionalMemory` dump。两条路径使用同一个 range comparator
  和 byte-order 合同。

`csr_snapshot.json` 只能复用现有 snapshot schema，并且必须能按 command id/order
与 raw replay 结果对应。snapshot 不表达的 raw CSR operation、write order 和
accepted start 语义始终以 `SauCsrWrite` 序列为准，不能补造第三套启动规则。

预计修改：

- `configs/example/sau_timing.py`
- `src/sau/Sau.py`
- 新增 `src/sau/functional_memory.{hh,cc}` 及 focused test
- `src/sau/SConscript`

## 5. 实施步骤

### Step 0：冻结当前 RTL、CSR 支持域和功能 golden

这是实现前置条件，不允许边写模型边猜测。

- [x] 从当前 elaboration/仿真日志确认实际 hierarchy 和参数：
  `SA_CORE -> sa_element/feeder + register_file_in ->
  sa_execute/sa_feeder -> transposer_tiny -> SA_ENGINE -> SA_ROW ->
  SA_PE_array -> SA_PE -> register_file_out`。
- [x] 冻结资源抽象合同：为 controller、address generator、memory port、
  input buffer、transpose/reuse、systolic array、output datapath 分别记录容量、
  端口、吞吐、延迟、accepted 条件、backpressure 路径、stall 原因/归因优先级和
  strict comparison 边界。
- [x] 建立状态生命周期表，明确 input buffer、transposer bank、array pipeline/
  accumulator、output SRAM、地址计数和 valid/last 在 reset、accepted start、
  command done 及四种 `sa_flow_mode` 下的清理或保留条件。
- [x] 从当前 RTL 条件分支建立 mode/资源行为等价类表，覆盖
  `trans_mode × reuse_mode × sa_flow_mode` 对 input-switch、bank ownership、
  result transpose、retain/clear、serializer 和 completion guard 的交互；测试
  可以按等价类收敛，不要求无依据地机械穷举所有笛卡尔积。
- [x] 等价类必须从当前 elaborated RTL 的 mux/select、case、counter guard、
  enable/ready、clear/retain 和 completion 条件静态推导，形成路径表：

  ```text
  path_id,rtl_guard,csr_fields,selected_resources,
  capacity_or_port_effect,boundary_events,representative_fixture
  ```

  每个合法 CSR 组合都必须能映射到某个已知 RTL 路径；测试负责验证每条结构路径、
  每个字段边界和关键交互，不枚举数值不同但走向相同的组合。
- [x] 路径表记录 boundary golden 的来源和 readiness。实现某条路径前，该路径所需
  的 read/input/resource/output 边界 golden 必须已取得并绑定 RTL contract；
  不要求等待尚未进入实现的其他路径全部采集完成。
- [x] 建立逐字段 CSR 支持域表：

  ```text
  field,raw_width,rtl_consumers,int8_gemm_legal_values,
  reserved_or_operator_switch,validation_fixture
  ```

  至少覆盖 `trans_mode`、`reuse_mode`、`sa_flow_mode`、`register_mode`、
  `conv_kernal`、`stride_flag`、`cutbit`、flow/loop、padding/valid window 和
  全部 step/burst/cycle。
- [x] 对 `reuse_mode=11` 做真实 RTL/软件契约判定；没有证据前不归类。
- [x] 为当前 `32x32x32, trans=01, reuse=01, cutbit=8` 采集：
  initial memory、CSR、完整 256-bit read/write address+payload+valid+last、
  output address range 和最终 output memory dump。
- [x] 为小规模 `trans_mode=2 (ABTD)` testcase 采集 B memory read payload、
  transposer input/output payload、bank/ready/valid/last 边界和对应周期，作为
  Step 3 的第一个非默认模块级 golden。
- [x] 额外采集同一路径 `cutbit=1` 的最终 output/write payload，证明 cutbit
  不是 fixture 常量。
- [x] 确认现有 `memory.hex` 是初始 image，而不是仿真结束后的 memory；所有文件
  记录 RTL commit、仿真命令和 SHA-256。

若需要读取 FSDB，必须按 `/home/xch/work/npi_fsdb_probe/README.md` 使用工具。

验收：

- 当前实例化层级、资源抽象合同、状态生命周期、RTL 路径/等价类表、CSR 支持域表、
  ABTD transposer golden 和两组 cutbit golden 均已落盘；
- 当前准备实现的每条路径均已有可复现 boundary golden 和匹配的 RTL contract；
- golden 能独立确定 byte order、输出范围及每个 write beat 的 256-bit 数据；
- 不再依赖不存在的 `memory_expected.hex` 或仅凭 `matmul_compare.csv` 猜测 packing。

### Step 1：建立唯一的功能 memory 与 payload 合同

- [x] 定义明确的定点数据类型/视图和转换边界：

  ```text
  MemoryBeat256          外部 32-byte read payload
  OperandVector32x8      32-lane signed-int8 operand
  AccumulatorVector32x24 32-lane signed 24-bit PE/snapshot state
  OutputVector32x16      32-lane signed 16-bit output-RF state
  WriteBeat256           int8 saturation 后的外部 32-byte write payload
  ```

  类型可以共享底层连续存储，但不得省略位宽、signed 语义、小端 lane 映射及发生
  sign-extension、wrap、saturation 的转换位置。
- [x] loader 读取 RTL memory image，建立有边界检查、稀疏 page/range backing 的
  byte-addressable `FunctionalMemory`；地址范围可以是 1 GiB，但物理分配只覆盖
  已加载或已写入的页。
- [ ] strict fixed-SRAM 只从 `FunctionalMemory` 取 read payload，并在 RTL
  对应提交边沿写入；固定 read-visible 周期保持不变。
- [ ] timing-memory 初始化时用 functional packet 把同一 image 写入下游 memory；
  扩展 `SauMemoryPort` 保存 read response data，write packet 不再填零。
- [x] timing request 被拒绝后，blocked packet 的 address/payload/last/command
  metadata 保持不变，address generator 只有在 request accepted 时推进。
- [x] response 延迟或 outstanding 满不能无条件冻结整个 SAU：已有 buffered token
  且不依赖该 response 的资源继续推进；只有缺少输入、端口被占用或下游满的相关
  资源停止，并通过真实 ready/backpressure 链传播。
- [x] 每拍可保留多个原始 stall 原因用于诊断，但总 stall cycle 按 Step 0 冻结的
  primary-cause 优先级只归因一次；至少区分 memory retry、response latency/
  starvation、outstanding limit、input/transposer/array/output backpressure。
- [x] timing-memory 的 read/write/final compare 只访问下游 memory，不维护第二份
  运行时镜像；下一 command 前处理 write 可见性屏障。
- [x] 提供统一的 range dump/compare，报告首个不同地址、expected/actual byte
  以及所属 256-bit beat/lane。

验收：

- memory image round-trip 与 RTL byte order 单测；
- 稀疏 memory 的空洞填充值、跨 page 访问、边界错误及 range-only dump/compare
  单测；不得因 1 GiB 地址范围产生 1 GiB eager allocation；
- strict 和 timing-memory 各自的真实 payload 读写单测；
- blocked/retry packet 保持 payload 不变；
- retry、延迟 response 和 outstanding-limit 测试证明独立资源可继续推进、相关资源
  正确冻结，且 stall 主因不重复计数；
- 同地址 RAW、跨 beat 边界和 write visibility barrier 单测；
- 原 timing-only 测试的地址、事件和统计不变。

### Step 2：建立全合法 Int8 GEMM CSR 驱动与资源分发框架

代码结构从本步骤起不得以 `01/01` 作为实现限制；`01/01` 仅是已有 regression。

- [x] `SauCsrConfig::decode()` 按 Step 0 支持域表解码所有 RTL 合法的 int8 GEMM
  配置，不使用“等于当前 fixture 默认值”作为合法性条件。
- [x] `SauCommand` 保存 raw/type-safe 的 `trans_mode`、`reuse_mode`、
  `sa_flow_mode`、`register_mode`、`cutbit`、flow/loop、padding/valid window
  和全部 step/burst/cycle，不再只保存压扁后的 beat 数。
- [x] 为 controller、address generator、input、transpose/reuse、array、output
  和 writeback 定义共享的 typed resource config；每个资源只消费 RTL 中实际连接
  到它的字段，禁止在模块内部重新解释 raw CSR。
- [x] 地址 generator 从 raw x/y/flow/instruction counter、step、burst、
  padding/valid window 推进，不使用 M/K/N 或 fixture 分支。
- [x] 建立验证成熟度记录，区分 decoded/resource-timed/data-functional/
  end-to-end；合法配置不能因为尚未达到 end-to-end 就伪装成 RTL illegal。
- [x] 保留现有 `RtlSchedulerSkeleton/RtlCommandDriverSkeleton` 的 `01/01`
  regression；新 mode 不在 skeleton 中复制控制状态机，而是通过通用
  boundary-trace comparator 与 RTL golden 对比。
- [x] `reuse=11`、`register_mode`、`conv_kernal`、`stride_flag` 按 Step 0 分类；
  只有权威接口判定非法或切换到阶段外算子的组合才在 decode 阶段拒绝。

验收：

- 四种 `trans_mode`、三种明确 `reuse_mode`、四种 `sa_flow_mode` 以及
  `cutbit=0..31` 均能无损通过 raw CSR -> config -> command -> resource config；
- 改变任一合法字段时，对应 resource config 必须改变，不能回退到 `01/01`；
- 每个合法配置都能依据 CSR 字段和 RTL guard 映射到 Step 0 路径表中的确定资源
  走向；相同路径的数值组合共享实现，不按 fixture 或具体 raw 数值分支；
- 地址循环在边界、last、padding 和回绕处与 RTL focused sequence 一致；
- 非法配置打印完整 raw CSR 和权威拒绝原因；
- 原 `01/01` skeleton 与 timing regression 不退化。

### Step 3：实现配置驱动的 input、transpose 与 reuse 资源（已完成）

- [x] `register_file_in` 建立与 RTL 容量、端口、地址、valid label 一致的真实
  payload 存储。
- [x] 实现 padding/valid window、读写 pointer、flow/instruction 地址更新及会影响
  accepted、backpressure 或边界周期的 pipeline 行为。
- [x] controller 和 feeder 通过 accepted transaction 推进 A/B token；每个 token
  携带真实 32-lane signed-int8 payload。
- [x] 实现 `ABD/ATBD/ABTD/ABDT` 对应的 input-switch、`transposer_tiny` 有限
  bank、ownership、ready/rden/valid/last、flush/preflush 和 lane 顺序。
- [x] 实现当前支持域 Reuse-A 的外部读取、内部存储/重放和 valid/last；
  `reuse=00/10/11` 的接口与已有代码保留，但按 2026-07-27 用户范围决定 deferred。
- [x] 为 transposer/reuse 增加 input/output token、busy cycle、bank occupancy、
  input/output stall 和首入到首出的 latency stats。

第一个非默认里程碑固定为 `trans_mode=2 (ABTD)`：

```text
真实 B memory payload
  -> input/register resource
  -> transposer bank
  -> 按 RTL 顺序输出 B^T token
```

即使 array/output 尚未达到 end-to-end，mode 2 也必须能独立达到
`data_functional`：B 输入/输出 payload 与 RTL 一致，转置资源消耗的周期、占用和
backpressure 被计入仿真。

验收：

- 每个外部 read address 对应的 256-bit payload 与 RTL 一致；
- `trans_mode=2` 的 B/B^T lane、token、valid/last、bank 占用和周期逐边界匹配 RTL；
- 四种 transpose 的 input/transposer 结构由 focused test 覆盖；当前支持的
  Reuse-A runtime 具有 focused payload/resource test；
- ATBD Reuse-A runtime 和首个非默认 ABTD B -> B^T 模块边界均有 RTL golden；
  deferred reuse 路径不作为当前 Step 3 验收阻塞项；
- fixture ID、矩阵尺寸或 golden 值不进入资源实现分支。

### Step 4：实现配置驱动的 systolic-array 与定点计算资源

- [x] 建立等价于当前
  `SA_ENGINE -> SA_ROW -> SA_PE_array -> SA_PE` 的 32×32 有限计算资源；使用固定
  大小连续状态和统一逐拍更新，不要求创建逐 RTL 实例对象，也不调用矩阵乘法库。
- [x] 每个 PE 实现 signed int8 乘法、乘法流水、wstrb/enable、clear 条件以及
  当前 RTL 的 24-bit `saturate_add_signed` accumulator。
- [x] 保留影响接受速率、wavefront、有效列、结果顺序和周期的 macro-row/column
  staircase、snapshot 与 row streaming；不可见寄存器可用等价流水延迟表示。
- [x] `cutbit=0..31` 全范围直接来自 CSR；对 24-bit signed MAC 逐 lane 执行
  `SA_pkg::sat_truncate_func` 的算术右移、符号扩展检查和 int8 饱和。
- [x] `register_mode`、`conv_kernal`、`stride_flag` 只实现 Step 0 证明仍属于
  int8 GEMM 的控制效果；切换到阶段外算子的组合明确拒绝。

验收：

- 单 PE 覆盖正负乘积、24-bit 正/负饱和、clear 和连续累加；
- `cutbit=0/1/8/15/23/31` 覆盖正常右移、负数算术右移及正/负饱和；
- 不同 transpose/reuse token 顺序进入 array 后，admission、wavefront、result
  token 和周期与对应 RTL boundary golden 一致；
- 所有 Step 0 判定为 GEMM 内合法的 array 控制分支都有 focused test。

### Step 5：实现配置驱动的 serializer、output RF 与真实写回

当前进度：increment 1 已实现 `register_file_out` accepted-update 侧；increment
2 新增有限 32-row T2 result serializer、冻结 RTL 的 CTRANS 顺序、registered
output-RF unload、`register_addr` 外部地址序列和 valid/address/data d1/d2 流水；
increment 3 将 array row、driver result-valid、output RF 和 native unload FIFO
接入 strict runtime；increment 4 以该 FIFO 为 strict write payload 权威源，
校验 token/address/last，并向 strict FunctionalMemory 提交真实 bytes。
timing-memory/DSE 接口保持原行为；首个 cutbit-8 final-memory golden 已逐字节
匹配。cutbit-1 的权威 CSR replay 已从 frozen FSDB 提取并同样逐字节匹配。
flow mode 2 的刷新 K512 RTL capture 已在第一 command done 后 93 cycles 发出
第二 start，按顺序接受 `[flow2, flow0]`，并在第二 command 后一次性写回匹配
1024/1024 bytes。flow mode 1 的 K512 diagnostic 同样按顺序接受 `[flow2,
flow1]`，且 `actual[r][c] == expected[31-c][r]`。无 flow2 留存的
64x160x64 diagnostic 进一步证明 2x2 tile 位置不变，每个 32x32 tile 独立
顺时针旋转。按 2026-07-28 用户范围决定，这一观测到的 tile-local 顺序就是 gem5
需要复现的 RTL flow1 语义。fixture 已包含两条 command 的 CSR、initial/final
memory、完整 boundary、日志及 Hash。Increment 5 已使模型按同一 tile-local
顺序执行：每条 command 接受 320 个阵列输入、产生并写回 64 beats，在 RTL 的
690-cycle command extent 完成；两条 command 的最终 4096 bytes 与 RTL oracle
一致。K512 与 K768 的 flow2 retain runtime、golden、统计及性能基线均已闭环，
Step 5 在冻结的 Reuse-A 支持域内完成。

- [x] 按 `sa_flow_mode` 实现结果 serializer/transpose 顺序；`trans_mode`
  不参与输出选择。
- [x] 实现 `register_file_out` 的 x/y/flow/instruction pointer、两个 SRAM half、
  read-after-write forwarding 和有限端口行为。
- [x] 实现 flow mode `0/1/2` 的 output SRAM 正常/转置/保留行为、16-bit
  two's-complement lane wrap accumulation及 unload/completion guard；raw `3`
  在语义确认前不宣称功能支持。
- [x] 按 RTL `sat_signed8` 对每个 16-bit lane 饱和，组装真实 256-bit write
  payload；strict/timing-memory 分别按 Step 1 的唯一数据真源提交。
- [x] command complete 必须等待相应 result、output、writeback 和 memory
  visibility 条件，不使用预设完成周期。

验收：

- output SRAM 覆盖 16-bit lane 溢出、同址 RAW forwarding 和
  `-129/-128/127/128` int8 饱和边界；
- flow mode `0/1/2` 各有真实 golden，并覆盖它们与 operand transpose/reuse
  的结构交互；raw `3` 确认语义后再加入验收；
- `32x32x32, trans=01, reuse=01, cutbit=8` 与 `cutbit=1` 作为首个完整
  end-to-end checkpoint，write payload 和最终 memory 逐字节匹配 RTL；
- 随后每个 Step 0 mode 结构等价类至少有一个真实 RTL end-to-end golden。

### Step 6：合法配置集成覆盖、多 command 与持久状态

当前进度：increment 1 已将 strict raw fixture 的 admission 变为构造期
preflight。模型用同一 `RtlCommandDriverSkeleton` 预测每条 command-done edge，
下一 start 必须严格晚于上一条完成/写回可见边沿；否则在产生 command stats 或
trace event 前拒绝。569/685-cycle 预测、恰好同边沿拒绝和合法间隔均已验证。
Increment 2 显式区分 `raw` 与 `sequential` fixture admission：timing-memory
raw replay 在 start 到达当拍仍 busy 时立即停止，不缓存该 command；sequential
把 fixture 作为 command template，只在前一 command 完成且写回可见后的下一拍
提交。100ns/单 outstanding 定向测试中 raw 在 cycle 662 拒绝，sequential 在
56859 complete 后于 56860 接收 command 2 并完成 2/2。Increment 3 增加真实
write→read 链式 fixture：`[flow2, flow2, flow0]` 的最终 command 写 32 beats，
随后第四条 Flow0 command 把同一范围作为 Operand-A 读取；所有 producer write
均早于 consumer 首次 read，最终 1024/1024 bytes 匹配 RTL。可选
`memory_dependencies` 只复用既有 trace 和 final-memory comparator，不引入第二套
功能模型；quick 回归 75/75。Increment 4 用真实 Yinglong 固件路径尝试省略首条
command 的 completion poll，但 crossbar 在 SAU active 期间阻塞 CPU 后续 CSR，
三次 start 仍全部在 IDLE 且分别晚于前一 done 110/105 cycles。该证据只冻结
Yinglong 软件可见的串行 admission；不使用非权威 UVM，也不推断强制直连
`SA_CORE` 的 start 行为。

- [x] 默认 sequential workload 必须保证下一 start 在前一 command 完成且写回可见
  后出现；静态可判定时在仿真开始前拒绝，受 timing-memory 动态 stall 影响而无法
  预判时，在冲突 start 到达的当拍报错并停止，不能缓存该 start。
- [x] Yinglong 软件路径已验证 crossbar 会把后续 CSR/start 阻塞到当前 SAU done
  之后，因此集成边界不存在可达的 busy start。raw fixture 的重叠 start 保持
  fail-fast，不作为强制直连 `SA_CORE` accepted/ignored 行为的 RTL oracle。
- [x] strict functional memory 在 command 间持久；timing-memory 在 write
  visibility barrier 后再启动下一 command。
- [ ] 内部 input/transposer/array/output 状态严格跟随 RTL reset、start、
  `sa_flow_mode` 和 clear/retain 条件。
- [x] 验证 command 2 可以读取 command 1 写回，且没有残留 token/valid 或错误
  accumulator 污染。
- [ ] 四种 transpose × 当前支持的 Reuse-A 都有集成 focused test；端到端测试按
  当前支持域的 `trans/reuse/flow` 结构等价类覆盖，不以当前 fixture 组合替代
  合法域。deferred reuse 路径待 RTL 稳定后另行恢复。
- [ ] 增加“合法但不符合用户意图”的成对测试：保持 memory 和其他 CSR 相同，仅将
  `trans_mode=1` 改为 `trans_mode=2`。gem5 不修正输入布局或回退 mode；两个 case
  的 result、write payload、资源边界周期和 command-done 分别匹配同配置 RTL，
  但不要求两个 case 彼此相同。

验收：

- 两 command 独立地址、链式依赖、retain 与 non-retain 测试通过；
- sequential workload 保持完成后提交；Yinglong 软件路径的 start/done 顺序与
  RTL 一致，重叠 raw fixture 在进入模型前明确拒绝；
- 每个 command 的 timing、payload、memory visibility 和完成顺序分别守恒；
- 实现支持全部合法 CSR 到 RTL 路径表的映射；验证覆盖每条结构路径、每个字段的
  最小/最大/回绕等边界、关键 guard 交互和未参与开发的合法 hold-out，不要求枚举
  所有 raw CSR 笛卡尔积；
- 所有 Step 0 结构路径达到 end-to-end validated，成对错误配置测试分别与 RTL
  的结果和拍数一致；
- 新 mode 直接使用通用 comparator 对比 RTL boundary golden，不依赖第二套
  command-driver 状态机。

### Step 7：规模回归、性能与文档

功能正确性按以下顺序收敛：

1. 小规模 `trans_mode=2`：B -> B^T transposer 模块 payload/资源时序闭环；
2. `32x32x32, trans=01, reuse=01`：首个完整读算写闭环；
3. 同路径 `cutbit=1`：动态量化验证；
4. 其余合法 transpose/reuse/flow 的 focused 和结构等价类覆盖；
5. `64x256x256` baseline：多 command/multi-flow；
6. `64x128x256`：不同 K 累加深度。

每个功能 fixture 必须保存或引用：

- 原始 CSR writes，以及 raw replay 后的 snapshot 交叉检查；
- 初始 memory image；
- RTL commit、仿真命令和 checksum；
- 完整 output memory range；
- 256-bit write address/payload/valid/last；
- mode bring-up 所需的最小中间边界 payload。

golden 调试顺序固定为：

```text
raw CSR writes
 -> replay/decode + snapshot cross-check
 -> read address + read payload
 -> register/transposer A/B payload
 -> PE/macro result payload
 -> output SRAM/unload payload
 -> write address + write payload
 -> final memory bytes
```

- [x] 原 timing quick suite 全部继续通过；当前连同功能回归为 27 suites、
  75/75 checks。
- [x] 新增 focused functional tests 和 Flow0/Flow1/Flow2 workload
  end-to-end tests。
- [x] 默认不输出逐拍 payload boundary trace；strict architecture/state/ledger
  保持开启作为验证合同，debug boundary trace 由显式文件参数启用。
- [x] 在 payload trace 关闭时按固定命令记录 `32x32x32` 和 `64x256x256` 的 wall-clock、
  peak RSS、simulated cycles 和 host memory allocation 热点，形成可重复性能基线；
  后续若出现明显退化必须定位原因。结果见
  `docs/reports/plan3-step5-performance-baseline.md`。
- [x] 统计增加 payload beat；功能 compare 由 regression verifier 报告并对照
  trace/final memory，不改变已有 stats 名称语义或向模型注入 RTL oracle。
- [x] 更新 `README.md` 和 `STATUS.md`：说明 workload、支持域、运行命令、
  验证结果和明确未支持配置。

## 6. 预计修改文件

现有文件：

- `src/sau/types.hh`
- `src/sau/csr_config.{hh,cc}` 及 test
- `src/sau/address_generator.{hh,cc}` 及 test
- `src/sau/a_register_file.{hh,cc}` 及 test
- `src/sau/memory_port.{hh,cc}` 及 test
- `src/sau/schedule_state.{hh,cc}` 及 test
- `src/sau/sau_model.{hh,cc}`
- `src/sau/Sau.py`
- `src/sau/SConscript`
- `configs/example/sau_timing.py`
- `tests/gem5/sau/test_sau.py`
- `src/sau/README.md`
- `src/sau/STATUS.md`

预计新增的模块（实施时可按职责小步拆分，但不得合并成直接 GEMM 计算器）：

- `functional_memory.{hh,cc}`：RTL memory image、持久 backing store 与
  dump/compare；
- `data_beat.hh`：外部 beat、operand、24-bit accumulator、16-bit output 和 lane
  转换边界；
- `input_datapath.{hh,cc}`：input RF、padding、feeder 数据状态；
- `transposer.{hh,cc}`：RTL 转置存储与输出；
- `systolic_array.{hh,cc}`：PE array 固定位宽逐拍状态；
- `output_datapath.{hh,cc}`：serializer、output RF、累加和饱和。

每个新增模块必须有 focused test，并加入 `SConscript`。

## 7. 验证命令原则

Codex 不主动编译 gem5。每个实现增量先运行不需要重链接的 Python/static 检查，
再给开发者最小增量构建命令。建议构建形式遵守：

```bash
cd /home/xch/work/sau_n_gem5/sau_origin_feature_sau_command_types_b9fbc18_20260720
scons build/RISCV/gem5.opt \
    --ignore-style --limit-ld-memory-usage -j32
```

实现期间优先构建对应 focused test target，最终才运行完整
`tests/main.py run --skip-build gem5/sau` 和功能 fixture。

## 8. Definition of Done

本阶段只有同时满足以下条件才算完成：

1. JSON 中引用或包含的 raw CSR writes 统一经过
   `SauCsrWrite` -> `SauCsrConfig` replay/decode，可以启动一个或多个顺序 int8
   GEMM command，无需 CPU；snapshot 不构成第二条执行路径。
2. workload/golden 绑定并校验 RTL commit、`ROW_NUM/COL_NUM/OUTPUTDW/SRAM_DELAY`、
   clock 和其他有影响 elaboration 参数；当前实际 RTL hierarchy、逐字段 CSR
   支持域和功能 golden 均有可复现证据，旧/未实例化 datapath 不作为实现来源。
3. 资源抽象合同、状态生命周期和 mode 等价类已冻结；模型反映有限容量、端口竞争、
   吞吐、延迟、backpressure、retry、outstanding 和 command 间依赖，但不要求
   逐寄存器翻译 RTL。
4. 所有权威接口判定为合法的 int8 GEMM 配置均可进入同一配置驱动资源框架；
   合法性与 decoded/resource-timed/data-functional/end-to-end 验证成熟度分离，
   不因当前 golden 未闭合而伪报 RTL illegal；完整 workload 命中
   `rtl_legal_unimplemented` 时 fail-fast，不输出错误 final memory 或可信性能。
5. 支持四种 RTL `trans_mode` 与当前冻结的 Reuse-A 路径，且 CSR 改变会真实改变
   逐拍控制、取数、transpose/reuse 数据路径和结果；`trans_mode=2` 的真实
   B -> B^T payload 与资源周期通过独立边界验收。`reuse_mode=00/10/11` 保留 raw
   解码和接口，但在 RTL 稳定前作为 deferred 配置 fail-fast，不计入当前 DoD。
6. `cutbit=0..31` 由 CSR 动态驱动 RTL 等价的 signed arithmetic shift 和 int8
   saturation；其他合法 GEMM CSR 参数也不能固定成现有 fixture 数值。
7. 地址、valid/last、input-switch 和完成条件由资源状态与 accepted transaction
   决定；每个资源/mode 都通过通用 comparator 与对应 RTL boundary golden
   比较，不依赖第二套 command-driver 状态机。
8. 对相同 raw CSR、memory 和 memory request-accept/response schedule，strict
   边界事件、command-done 总拍数、write payload 和最终 memory 与 RTL 一致；
   合法但不符合用户意图的配置不被修正，并分别匹配相同错误配置的 RTL。
9. timing-memory 相对 fixed-SRAM RTL 的额外周期均可由 memory latency、retry、
   queue/outstanding stall 和 backpressure 解释；给定相同
   request-accept/response schedule 时恢复 strict 拍数。blocked packet 保持不变，
   独立资源继续推进，相关资源按真实依赖冻结，stall 主因不重复计数。
10. strict 与 timing-memory 每次运行都只有一个数据真源；每次 read 返回真实
   payload，每次 write 携带真实 payload，写回不再为零。
11. 当前 `SA_PE` 的 24-bit saturating accumulator、cutbit、
   `register_file_out` 16-bit lane 运算、RAW forwarding、int8 saturation 和
   byte layout 与 RTL 一致。
12. `32x32x32` 的 `cutbit=8` 与 `cutbit=1`、`64x256x256` baseline、
   `64x128x256` K hold-out 的最终 memory 指定范围逐字节匹配 RTL。
13. 全部合法 CSR 可映射到由 RTL guard 静态推导的已知资源路径；测试覆盖每条结构
    路径、字段边界、关键交互和合法 hold-out，不要求穷举相同路径的数值组合；
    每条路径在实现前已有绑定 RTL contract 的对应 boundary golden。
14. transpose/reuse/flow 的 focused test 和结构交互等价类 golden 覆盖通过；
    非法或 reserved 配置依据支持域表明确拒绝。
15. 默认 workload 只接受顺序 start，静态可判定时 preflight 拒绝，动态冲突在
    start 到达当拍报错；专用重叠-start testcase 严格复现 RTL 的
    accepted/ignored 行为，任何未接受 start 都不由 gem5 延后补执行。多 command
    顺序、共享 memory 可见性和内部状态边界通过测试。
16. 原 timing quick suite 与新增 functional suite 全部通过，且无 fixture、
    尺寸或 golden 值硬编码；trace 关闭时已有规模运行性能基线。

## 9. 主要风险与控制

- **RTL 注释与实际行为可能不一致**：以当前 RTL source 和真实 testcase 波形为准；
  需要看 FSDB 时按 `/home/xch/work/npi_fsdb_probe/README.md` 使用专用工具。
- **仓库同时保留新旧 datapath**：每个实现来源都必须能沿当前 elaborated hierarchy
  从 `SA_CORE` 追到实例，禁止依据未实例化的同名旧模块实现。
- **mode 组合数量较多**：接口和状态结构从一开始由完整合法 CSR 驱动，验证按资源
  模块和 Step 0 RTL 路径表逐步收敛；所有合法组合必须映射到已知路径，但测试只需
  覆盖结构路径、字段边界、关键交互和 hold-out。`01/01` 只是首个完整
  end-to-end golden，不能成为实现分支或合法域。
- **CSR raw 值不等于合法算子配置**：`reuse=11`、`register_mode`、
  `conv_kernal` 等先由 Step 0 支持域表分类，不能猜测接受或拒绝。
- **位宽错误可能只在极值暴露**：每个截断/溢出位置都要有正负极值 focused test，
  最终值比较不能替代这些测试。
- **大规模逐 PE 仿真速度**：数据 trace 默认关闭；PE 使用固定大小连续存储和
  预分配状态，不在每拍分配对象。允许合并不可见内部状态，但必须保持资源容量、
  竞争、吞吐、阻塞、payload 和 strict 边界周期。
- **抽象不足或过度**：不逐信号翻译 RTL，也不使用直接 GEMM 或无限资源捷径；
  每个抽象都以 Step 0 的资源合同和边界 comparison 证明其保真范围。
- **oracle 演变为第二套模型**：skeleton 只保留已有 `01/01` 回归；新 mode 使用
  通用 boundary-trace comparator 直接对比 RTL golden，不在 oracle 中复制控制
  状态机。
- **大地址空间导致 eager allocation**：memory address range 与物理 backing
  分离，strict memory 使用稀疏 page/range，最终只比较显式范围。
- **strict/timing-memory 数据分叉**：两类运行分别使用明确的唯一数据真源，并共享
  image parser、byte-order 和 comparator；不得在一个运行中维护两份可写镜像。
- **现有 artifact 没有完整 final-memory 文件**：Step 0 未采集 write payload 和
  output memory dump 前，不开始 datapath 实现，也不宣称 byte-exact 验收。
