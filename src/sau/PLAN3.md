# PLAN3：CSR 驱动的 Int8 GEMM 功能与数据通路 RTL 对齐

## 状态

**需求已确认，尚未开始实现。**

本计划承接 `PLAN2.md` 已完成的 CSR 解码、逐拍控制、地址请求和时序对齐工作。
`PLAN2` 的完成结果继续作为时序回归基线，但其“固定
`trans_mode=01 + reuse_mode=01`”限制不再是本阶段最终支持域。

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
- 上述两项是本阶段仅有的固定算子/精度边界。除 RTL 明确判定非法的组合外，
  其他参与 int8 GEMM 的 CSR 字段都必须按 raw 配置产生真实行为，不能固定为当前
  fixture 的数值。
- 启动方式：JSON 引用 CSR 写入或 CSR snapshot；所有配置仍由
  `SauCsrConfig` 按 `csr.sv` 位域解码，JSON 不定义第三套独立字段编码。
- command：允许多个 command，但同一时刻只运行一个；前一 command 完成并写回
  后才启动下一 command。
- memory：所有 command 共享一个持久 memory image。前一 command 的写回对后续
  command 可见。
- 时序：保留现有 strict RTL 时序行为；加入数据后不能破坏已通过的
  architecture/state trace。
- 数据 trace：默认关闭以优先保证仿真速度。本阶段只预留接口，不把完整内部数据
  trace 作为完成条件。

### 2.2 Int8 GEMM 内必须由 CSR 选择的 RTL 路径

`trans_mode` 必须支持 `SA_pkg.sv` 定义的全部四种模式：

| raw 值 | RTL 名称 | 数据语义 |
| --- | --- | --- |
| `00` | `ABD` | `A * B = D` |
| `01` | `ATBD` | `A^T * B = D^T` |
| `10` | `ABTD` | `A * B^T = D^T` |
| `11` | `ABDT` | `A * B = D^T` |

`reuse_mode` 必须覆盖 `scheduler.sv` 明确列出的三种 GEMM 复用策略：

| raw 值 | 本阶段语义 |
| --- | --- |
| `00` | 不复用 |
| `01` | 复用 Operand-A |
| `10` | 复用 Operand-B |

`reuse_mode=11` 在当前 RTL 中没有明确的第四种复用语义，本阶段必须显式拒绝，
不得猜测或映射到其他模式。若后续获得明确 RTL 定义和 testcase，再单独确认范围。

`sa_flow_mode` 按 RTL 的 `CNORMAL/CTRANS/RETAIN/TRETAIN` 行为影响结果顺序和
output SRAM 是否累加，不能在功能模型中固定成当前 fixture 的值。

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

以下字段同样不能固定为现有 golden 的数值：

- `register_mode`、`sa_flow_mode`、`conv_kernal` 和 `stride_flag` 在
  `pe_work_mode=MATMUL, shift_flag=0` 下仍可到达的控制效果；
- `flow_loop_times`；
- padding 与 register valid x/y window；
- input、vertical、register-input、output 的全部
  x/y/flow/instruction step、burst 和 cycle。

它们必须覆盖 RTL 对 int8 GEMM 定义的合法取值；若某个 raw 值在 RTL 中会形成
零长度、越界或未定义组合，模型应根据同一条 RTL 约束明确拒绝，而不是因为现有
fixture 没用过就拒绝。`pe_work_mode` 必须是 MATMUL，`shift_flag` 必须为 0；
卷积、单独转置、矩阵加法和 int16 均明确拒绝。

### 2.3 不在本阶段

- CPU 指令 decode、CSR 总线接入、中断和操作系统集成；
- INT16、CONV、独立 TRANSPOSE、ADD；
- SRAM bitcell、bank 冲突或模拟电路级行为；
- 为未实现算子添加通用计算图；
- 默认开启逐 PE、逐 accumulator 的大体积数据 trace。

## 3. 权威来源与正确性原则

功能行为以 `/home/xch/work/npu_lpnpu` 当前 RTL 为权威，主要对应：

- `hardware/src/sa_element/csr.sv`
- `mem_addr.sv`、`mem_ctrl.sv`、`register_addr.sv`
- `register_file_in.sv`、`padding_shifter.sv`、`feeder.sv`
- `hardware/src/sa_execute/transposer.v`
- `trans2sa_top.v`、`sa_feeder.sv`、`SA_ENGINE.sv`、`SA_TOP.v`
- `SA_row_unit.v`、`SA_pe.v`
- `hardware/src/sa_element/register_file_out.sv`

实现遵守以下原则：

1. 地址和取数范围来自 CSR counter/step/burst/valid/last 逻辑，不从 M/K/N
   重新推导运行时行为。
2. 每个数据 token 携带真实 payload；控制 valid/last 与 payload 使用同一个逐拍
   状态推进，避免时序模型和功能模型各自维护一套进度。
3. 固定位宽运算必须在每个 RTL 截断点执行 signed extension、wrap、cutbit 和
   saturation；不能先用无限精度完成 GEMM 后统一量化。
4. transpose、reuse、serializer 和 output accumulation 都保留独立状态存储，
   其边界与 RTL 模块一致；不要求复制无架构影响的临时 wire 或门级网表。
5. fixture/test ID、M/K/N 或 golden 值不得出现在功能分支中。

## 4. Workload JSON 合同

新增 `--workload-json FILE`。JSON 只负责组合已有事实来源，不复制 CSR 位域：

```json
{
  "schema_version": 1,
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

要求：

- 相对路径相对于 workload JSON 所在目录解析。
- JSON 由 `configs/example/sau_timing.py` 使用 Python 标准库解析并完成 schema
  校验；C++ SimObject 只接收规范化后的 CSR source、memory image、dump/compare
  路径和范围，不引入新的 JSON 依赖。
- `csr_source.type` 首先支持 `writes_csv`；可选支持已有
  `csr_snapshot.json`，但 snapshot 仍必须转换成 `SauCsrConfig` 状态再启动。
- memory loader 必须明确 byte order、每行宽度、地址单位和空洞填充值，并用现有
  RTL `memory.hex`/`memory_mod_*.hex` 做交叉测试。
- JSON 可包含比较范围，但不能包含直接作为运行输入的期望矩阵结果。
- 配置不完整、地址越界、重叠格式冲突或不支持的 CSR mode 在仿真开始前报错。
- 旧 `--rtl-profile` timing-only 入口继续可用；功能 workload 是新增入口，不能
  破坏现有 23 个 quick suite。

预计修改：

- `configs/example/sau_timing.py`
- `src/sau/Sau.py`
- 新增 `src/sau/functional_memory.{hh,cc}` 及 focused test
- `src/sau/SConscript`

## 5. 实施步骤

### Step 1：建立功能 memory 与 payload 合同

- [ ] 定义固定 32-byte beat payload 类型，规定地址与 lane 的小端映射。
- [ ] loader 读取 RTL memory image，建立可检查边界的持久 byte-addressable backing
  store。
- [ ] strict fixed-SRAM 路径的读响应返回真实 256-bit payload，写请求携带真实
  256-bit payload 并更新 backing store。
- [ ] timing-memory 路径扩展 `SauMemoryPort`：保存读响应 data，写 packet 不再
  `memset(0)`；payload 与原 `Beat` 元数据一同返回。
- [ ] 提供仿真结束后的 memory range dump/compare，错误报告首个不同地址、
  expected byte 和 actual byte。

验收：

- memory image round-trip 与 RTL byte order 单测；
- fixed-memory 和 timing-memory 的读写 payload 单测；
- 同地址 RAW、跨 beat 边界、多个 command 共享写回结果单测；
- 原 timing-only 测试的地址、事件和统计不变。

### Step 2：将 CSR 解码从“固定路径”扩展为 Int8 GEMM mode dispatch

- [ ] `SauCommand` 保存原始且类型化的 `trans_mode`、`reuse_mode`、
  `sa_flow_mode`、`register_mode`、cutbit、padding、valid window 及所有地址循环
  配置；不得只保存压扁后的 beat 数。
- [ ] `SauCsrConfig::decode()` 仅接受 MATMUL + int8，支持四种
  `trans_mode`、三种 `reuse_mode` 和 RTL 定义的四种 output flow mode。
- [ ] 建立 mode dispatch/table，使 scheduler、feeder、transposer、array 和
  output path 都读取相同的 decoded control。
- [ ] 对 `reuse_mode=11`、非 MATMUL、int16 和字段组合不变量提供包含 raw CSR
  值的明确错误。
- [ ] 不使用“与当前 fixture 默认值相同”作为合法性条件；为 cutbit、
  flow/loop、padding/valid window 和各级 step/burst/cycle 建立源自 RTL 位宽及
  counter guard 的合法域。
- [ ] 地址 generator 从 CSR 的 x/y/flow/instruction counter、step、burst 和
  padding/valid window 推进，不依赖 M/K/N。

验收：

- 四种 transpose × 三种 reuse 的 decode/dispatch 单测；
- `cutbit=0/1/8/15/23/31` 均能通过 decode 并保持 raw 值；
- 合法的非默认 flow/loop、padding、valid window 和 step/burst/cycle 配置不会
  因不等于现有 fixture 而被拒绝；
- 每种地址循环在边界、last、padding 和回绕处与 RTL 小规模序列一致；
- 不支持配置必须失败，不能回退到 `01/01`。

### Step 3：实现带数据的输入前端

- [ ] `register_file_in` 建立与 RTL 深度、地址和有效位一致的真实 payload 存储。
- [ ] 实现 padding shifter、valid window、读写 pointer、flow/instruction 地址
  更新以及 RTL 对应的 pipeline register。
- [ ] feeder 按 scheduler 的 `input_switch` 和 reuse mode 选择 resident/stream
  operand；每个 A/B valid token 同时携带 32 个 signed int8 lane。
- [ ] transposer 建立 RTL 相同的行存储、ready/valid/last 和四种
  `trans_mode` 数据排列。
- [ ] 将现有 `RtlCommandDriverSkeleton` 的控制脉冲与新的 payload datapath
  连接，删除只按 index 生成虚拟 A/B token 的 strict 功能路径。

验收：

- 用小型确定数据分别验证 `ABD/ATBD/ABTD/ABDT` 的 lane 顺序；
- 验证不复用、复用 A、复用 B 时外部读取次数、内部重放内容和 valid/last；
- 输入前端加入 payload 后，现有 strict architecture/state trace 仍逐拍通过。

### Step 4：实现 32×32 systolic array 的逐拍定点数据通路

- [ ] 按 RTL PE 行列连接建立 32×32 PE state，不调用矩阵乘法库。
- [ ] 每个 PE 实现 signed int8 乘法、RTL accumulator 位宽、寄存器更新顺序、
  clear/retain 条件和溢出 wrap。
- [ ] 按 `SA_ENGINE/SA_TOP/SA_row_unit/SA_pe` 的 enable、Flag、row/column
  sequence 和 pipeline latency 推进数据。
- [ ] 对 24-bit signed MAC 结果逐 lane 执行
  `SA_pkg::sat_truncate_func`：算术右移动态 `cutbit`，检查移位后的高位是否为
  正确符号扩展，并按 int8 边界饱和；cutbit 在 RTL 对应层级和周期生效。
- [ ] result token 同时携带 RTL 输出 payload、row sequence、valid 和 last。

验收：

- 单 PE 的正负数、最大/最小值、乘积溢出、累加溢出和 cutbit 边界测试；
- 对同一组非零 MAC 数据分别运行 `cutbit=0/1/8/15/23/31`，逐位对比 RTL
  `sat_truncate_func`；至少包含右移后正常值、正饱和、负饱和和负数算术右移；
- 2～3 个可手算的小 tile 验证 wavefront、skew、clear 和 retain；
- 32×32×32 fixture 的 array 输出 token 数、周期及 payload 与 RTL 中间/最终
  golden 一致。

### Step 5：实现结果序列化、output SRAM 累加与真实写回

- [ ] 按 `trans2sa_top` 的 result transpose/serializer 顺序生成输出。
- [ ] 实现 `register_file_out` 的 local write pointer 及
  x/y/flow/instruction 地址循环。
- [ ] 精确实现 output SRAM 的读后写、相邻周期 RAW forwarding 和两个 SRAM
  half 的路由。
- [ ] `CNORMAL/CTRANS` 不读取旧结果做累加；
  `RETAIN/TRETAIN` 按 `sa_flow_mode[1]` 与旧 output SRAM 值做 16-bit lane
  累加。
- [ ] int8 写回按 RTL `sat_signed8` 对每个 signed 16-bit lane 饱和，再按 RTL
  unload 顺序组装真实 256-bit write payload。
- [ ] write accepted 后更新共享 backing memory；command complete 仍等待 RTL
  write-finished 链和 token/payload conservation。

验收：

- `-129/-128/127/128` 饱和边界；
- output SRAM 同址连续写的 RAW forwarding；
- 四种 `sa_flow_mode` 的顺序/累加差异；
- 每个写地址、256-bit write data、last 和最终 memory bytes 与 RTL 一致。

### Step 6：多 command 顺序执行与持久状态

- [ ] workload 中多个 accepted start 按 CSR cycle/order 排队。
- [ ] 活跃 command 未完成时不得启动下一 command；fixture 若重叠 start，给出
  明确错误。
- [ ] 外部 memory 在 command 间持久；内部 register/output SRAM 的保留或清除
  严格跟随 RTL reset、start、flow mode 和 clear 信号。
- [ ] 验证 command 2 可以读取 command 1 的写回，同时不存在 command 间残留
  token、valid 或错误 accumulator 污染。

验收：

- 两 command 独立地址测试；
- command 2 消费 command 1 输出的链式测试；
- retain 与 non-retain 跨 command 边界测试；
- 每个 command 的 timing、payload 和完成顺序分别守恒。

### Step 7：RTL golden 分级收敛

功能正确性分三层验收，不能只比较软件 GEMM：

1. **Bring-up**：`32×32×32`、`trans_mode=01`、`reuse_mode=01`，使用现有
   `memory.hex` 和 `matmul_compare.csv`，先闭合完整读算写。
2. **现有形状回归**：`64×256×256` baseline，验证多 command/multi-flow；
   `64×128×256` 验证不同 K 累加深度。
3. **mode coverage**：为四种 transpose 和三种 reuse 至少各准备一个真实 RTL
   golden；组合测试集必须覆盖 scheduler 注释中的不同 input-switch 序列。
   `CNORMAL/CTRANS/RETAIN/TRETAIN` 各至少一个真实 golden。

每个功能 fixture 至少保存或引用：

- 原始 CSR writes/snapshot；
- 初始 memory image；
- RTL commit、仿真命令和 checksum；
- 最终 output memory range 或完整 expected memory image；
- 可选的 read/write payload trace，用于定位首个分歧。

golden 收敛顺序：

```text
CSR snapshot
 -> read address + read payload
 -> A/B token payload
 -> array result payload
 -> output SRAM/unload payload
 -> write address + write payload
 -> final memory bytes
```

若最终值不同，必须定位第一个不同的模块边界/周期；不得修改 expected 结果或绕过
中间数据通路。

### Step 8：回归、性能与文档

- [ ] 原 23 个 timing quick suite、63 个检查全部继续通过。
- [ ] 新增 focused functional tests 和 workload end-to-end tests。
- [ ] 默认不输出逐拍数据 trace；只保留按 command/地址范围过滤的可选 debug
  开关作为后续诊断接口。
- [ ] 统计增加实际读/写 payload beat 数和功能比较结果，但不改变已有 stats 名称
  语义。
- [ ] 更新 `README.md` 和 `STATUS.md`：说明 workload 格式、支持 mode、运行命令、
  验证结果和未支持配置。

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
- `data_beat.hh`：固定宽度 payload 与 lane 操作；
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

1. JSON + CSR 可以启动一个或多个顺序 int8 GEMM command，无需 CPU。
2. 支持四种 RTL `trans_mode`、三种已定义 `reuse_mode`，且 CSR 改变会真实改变
   取数、transpose/reuse 数据路径和结果。
3. `cutbit=0..31` 由 CSR 动态驱动 RTL 等价的 signed arithmetic shift 和 int8
   saturation；其他合法 GEMM CSR 参数也不能被固定成现有 fixture 数值。
4. 地址、valid/last、状态和周期来自 RTL 结构；现有 strict timing 回归不退化。
5. 每次 memory read 返回真实 payload，每次 write 携带真实 payload；写回不再为零。
6. PE、accumulator、cutbit、output accumulate、RAW forwarding、signed saturation
   和 byte layout 与 RTL 一致。
7. `32×32×32` bring-up、`64×256×256` baseline、`64×128×256` K hold-out 的最终
   memory 指定范围逐字节匹配 RTL。
8. 新增的 transpose/reuse/flow/cutbit mode golden 覆盖通过；未支持配置明确拒绝。
9. 多 command 顺序、共享 memory 可见性和内部状态边界通过测试。
10. 原 timing quick suite 与新增 functional suite 全部通过，且无 fixture、
   尺寸或 golden 值硬编码。

## 9. 主要风险与控制

- **RTL 注释与实际行为可能不一致**：以当前 RTL source 和真实 testcase 波形为准；
  需要看 FSDB 时按 `/home/xch/work/npi_fsdb_probe/README.md` 使用专用工具。
- **mode 组合数量较多**：先闭合 `01/01` 的全数据链路，再按同一模块边界扩展；
  但最终 DoD 不允许只支持 `01/01`。
- **位宽错误可能只在极值暴露**：每个截断/溢出位置都要有正负极值 focused test，
  最终值比较不能替代这些测试。
- **大规模逐 PE 仿真速度**：数据 trace 默认关闭；PE 使用固定大小连续存储和
  预分配状态，不在每拍分配对象。只有证明不改变逐拍寄存器语义时才允许机械优化。
- **旧 calibration memory 没有真实数据**：功能 memory 接入必须同时保留 strict
  固定延迟和真实 payload，不得为取数据而改用另一套时序。
