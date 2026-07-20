# PLAN2：CSR 驱动的 Int8 GEMM RTL 对齐模型（固定 ATB + reuse-A 路径）

## 状态

**Ready for implementation.**

八组 RTL golden package 已交付并完成 package 完整性、SHA256、CSR/snapshot
一致性与命令事件闭合检查：五组 coverage fixture 和三组 hold-out fixture。
hold-out 已可用，但仍必须在 coverage 的通用公式冻结后才可用于验收，不能参与
调参。

本计划是 Task 11 后的独立阶段。它不替换 `PLAN.md` 中的首里程碑记录；
实施时以本文件为准。

## 目标与范围

将 SAU 的输入边界扩展为：

```text
真实 CSR 写入 -> SauCsrConfig -> SauCommand + TimingPolicy
              -> SauScheduleState -> token / buffer / timing-memory 数据通路
```

本计划只覆盖真实 Int8 GEMM 使用的固定控制组合：

```text
trans_mode = 2'b01  // ATB
reuse_mode = 2'b01  // reuse Operand-A
```

该组合的目标支持域是：在固定 RTL elaboration 参数下，所有由真实 software/CSR
产生、且被 RTL 接受的 Int8 GEMM 配置与 M/K/N 尺寸。模型必须从 CSR snapshot、
RTL 结构参数和实际握手推导执行次数、地址和周期，不能按矩阵尺寸、test ID 或
fixture 名称选择 timing profile。现有五组 coverage 与三组独立 hold-out 只构成
跨 M/K/N 的泛化验证证据；它们不能在逻辑上证明所有未采样、但 RTL 可接受的尺寸。

已采集五组 coverage fixture：最小 workload、分别改变 M/K/N 的三组 dimension
sweep，以及多 flow/基准 workload。公式冻结后，再使用三组未参与调参的 M/K/N
hold-out fixture 验证泛化性。其他 `transpose` 或 `reuse` 原始值不在本阶段建模
范围内；CSR decoder 必须显式拒绝，并在错误中打印两个字段值。后续只有在真实
算子使用新组合且提供对应 RTL package 时，才单独扩展范围。

不包括：RISC-V `msetins1..7` decode、CPU/中断集成、INT16、卷积和数值计算。
RISC-V 组后续应把指令转换为同一 CSR 写入接口，而不直接操纵 SAU 数据通路。

## 建模原则

### CSR 与命令边界

- `SauCsrWrite` 表示 RTL 实际观察到的一次 CSR 写入：cycle、raw address、
  operation、64-bit data、accepted。
- `SauCsrConfig` 保存按 `csr.sv` 语义更新的寄存器状态，包括保持行为和 start。
- `SauCommand` 仍是已经解码的工作描述；它不保存 CSR 打包格式。
- `TimingPolicy` 保存由 CSR、RTL 结构参数和状态/握手规则导出的时序策略。
- direct-command API 必须保留，仅用于组件单测、DSE 和 legacy baseline 回归。

本阶段的 CSR decoder 在接受命令前必须验证
`trans_mode == 2'b01 && reuse_mode == 2'b01`。它仍保留 raw CSR 字段，不能把
该约束隐含为未检查的默认值。

### 泛化与支持域

- 支持的是 RTL 实际接受的 CSR 配置，不是任意数学上的 M/K/N 三元组；不满足
  对齐、tile、burst、register-depth 或 firmware 约束的尺寸必须显式拒绝或记录为
  RTL unsupported，不能由 gem5 猜测补齐。
- `SauCommand`、地址程序和 `TimingPolicy` 必须由 CSR 字段和结构参数构造。禁止
  以 `M/K/N`、fixture 路径、test ID、command ID、固定 cycle offset 或已知 trace
  行号作为分支或查表键。
- 固定流水级延迟可以使用具名 RTL 参数或可定位的 RTL 派生公式；循环次数、状态
  时长和地址序列必须随 burst/step/loop 与实际 token/port 握手变化。
- 五组 coverage fixture 用于覆盖不同维度和控制边界；三组 hold-out fixture 只能
  在通用公式冻结后运行。hold-out 不允许新增 fixture 专用补拍或 profile。

### 语义状态，不复制 RTL FSM

使用 `SauScheduleState`：

```text
Idle
ResidentLoad
TransposeSetup
FlowExecute
FlowBoundary
DrainAndWriteback
Complete
```

每个状态关联 RTL `REGISTER_LOAD`、`TRANSPOSE_LOAD`、`REUSE_LOAD`、
`TRANSPOSE_CLIP`、`FIRST_LOAD`、`D_OUT`、`REGISTER_UNLOAD` 的名称和 guard
来源，但不复制 RTL 状态编码、每个寄存器或每条组合逻辑。

本阶段只为固定 ATB + reuse-A 路径建立映射和测试；不得为了预留其他八种
组合而加入未由 fixture 证明的 mode-specific 分支。

状态只控制 SAU 本地操作的 eligibility 和优先级：是否允许读 A/B、填充
resident storage、消费 token、注入阵列、释放结果、发起写回。memory port、
response、retry、buffer、outstanding 和 token 是实际进度的唯一事实来源；
状态不得以“固定等待 N 拍”强行推进。

状态转换必须同时满足：

1. 可追溯的 RTL guard，例如 `data_last`、`register_load_done`、
   `execute_finished`、`update_finished`、`write_finished`；
2. gem5 的实际资源与握手条件，例如 resident-ready、B token、array/result
   token、write accepted/finished。

RTL 只要不改变这些可观察 guard、资源行为和 trace，即使内部重构，gem5 不应
修改；这些外部行为改变时才更新模型。

### 非魔法延迟规则

strict fixture 的任何周期只能来自以下来源之一：

1. RTL 结构参数：`SA_SIZE`、`SRAM_DELAY`、`ADDR_DELAY`、register depth、
   数据宽度和 PE/array pipeline 参数；
2. CSR 字段：x/y/flow/ins step、burst、cycle、loop、mode；
3. 语义状态 guard 或实际 timing-memory handshake。

每条 command 必须输出 timing ledger，逐项说明 resident load、transpose、
每个 flow、drain、writeback、complete 的周期和来源。若差异不能由以上来源
拆解，必须新增 RTL instrumentation/trace；禁止添加 residual delay、裸数字
默认值或事件专用补拍。

timing ledger 还必须记录每项的推导输入（CSR 字段、RTL 参数、guard 或 runtime
handshake）。同一 RTL 参数下，ledger 不得因为 fixture 名称或 M/K/N 之外的隐式
profile 而改变规则。

`--rtl-profile` 最终改为选择 CSR fixture，不能继续在 Python 中硬编码
269/343/234 等 profile 常数。DSE override 允许保留，但必须显式标为
non-strict，不能用于 strict 比较。

## RTL Golden Package 契约

每组 package 必须包含：

```text
csr_writes.csv
csr_snapshot.json
architecture.csv
diagnostic.csv
manifest.json
SHA256SUMS
README.md
```

### `csr_writes.csv`

表头固定：

```text
cycle,csr_addr,csr_operation,csr_wdata,accepted
```

记录有序的真实 SAU CSR 写入。`csr_addr` 和 `csr_operation` 必须保留 RTL
原值；不得只输出已解码字段。

### `csr_snapshot.json`

按 command 记录 start 时最终 CSR 字段。`start_write_cycle` 与
`start_write_row` 是正式字段，分别表示使该 command 生效的、已接受 start 写入的
cycle 与 `csr_writes.csv` 数据行号；`architecture.csv` 中对应的
`command_accepted` 必须处于同一 cycle。snapshot 是 command 到 CSR 配置的权威
映射来源。每个对象至少包括：地址、
`flow_loop_times`、`reuse_mode`、`trans_mode`、`pe_work_mode`、
`sa_flow_mode`、`register_mode`、stride/shift/cutbit，以及所有 x/y/flow/ins
step 与 burst 字段。

### `manifest.json`

记录 RTL commit、SA_SIZE、SRAM/ADDR delay、数据宽度、case、固定的
`trans_mode/reuse_mode`、command 映射及 workload 档位（small、M/K/N sweep 或
baseline/hold-out）。

本阶段只有 `trans_mode=01, reuse_mode=01` 的真实 RTL package 才是 supported
fixture。其他组合不要求生成 RTL package；gem5 必须在 decode 阶段显式拒绝，
而不能静默回退到默认路径。

已交付的 `int8_gemm_64x256x256_baseline` 含 CSR write/snapshot，既是 PLAN2
正式 coverage fixture，也继续作为 legacy direct-command strict 回归。

## 实施步骤

### 1. CSR frontend 与 decode

预计新增 `csr_config.{hh,cc}` 及 C++ test，并修改 command/types/SConscript。

- [x] 定义 `SauCsrWrite`、`SauCsrConfig` 和 fixture write replayer。
- [x] 以 `hardware/src/sa_element/csr.sv` 为唯一位域来源，实现 apply、hold
  和 start 语义；不从 synthetic 参数反推 CSR。
- [x] 定义 decode 输出 `SauCommand + TimingPolicy`，保持两者和 CSR config
  的职责分离。
- [x] 只支持 `trans_mode=01, reuse_mode=01` 的 fixture 已验证 Int8 GEMM
  mode；错误信息必须包含实际字段值和拒绝原因。
- [x] 验证 decode 不依赖 fixture 名称、test ID、command ID 或预置 M/K/N profile；
  所有执行形状均从 CSR snapshot 构造。

**验收：** 覆盖每个 CSR index 位域、write order、accepted=false、hold、start、
snapshot 等价性、固定模式接受、其他 mode 显式拒绝，以及不同有效 loop/burst
配置产生不同通用命令形状的 C++ 单测。

### 2. 语义调度状态接入

预计新增 `schedule_state.{hh,cc}` 及 test，并修改 `sau_model`。

- [x] 为固定 ATB + reuse-A 路径建立 `SauScheduleState` 到 RTL
  state/guard 的映射表。
- [x] 将 read A/B、resident load、array admission、result release 和 writeback
  的 eligibility 改由语义状态控制。
- [x] 以实际 token/port 条件作为状态转换的第二 guard，保持 memory retry 与
  backpressure 的动态影响。

**验收：** 固定路径的合法/非法转换、token/backpressure 阻止转换，以及 legacy
direct-command 不变性的 C++ 测试。

### 3. 可追溯 timing policy 与 ledger

预计新增 `timing_policy.{hh,cc}`，并修改 scheduler/model/CLI。

- [x] 用结构参数、CSR 字段和 guard 替换 strict 路径的 profile 裸延迟。
- [x] 输出每 command timing ledger；各项必须解释 command cycles。
- [x] strict fixture 禁止 timing override；DSE override 标记为 non-strict。
- [x] `--rtl-profile` 改为选择 CSR fixture。
- [x] 禁止以矩阵尺寸、fixture、test ID 或 command ID 选择 strict timing 参数；
  所有 strict fixture 共享同一套 RTL 参数与 CSR 推导规则。

**验收：** timing-policy 单测验证每项来源及 loop/burst 边界；未解释的差异或
fixture-specific 分支阻塞 strict promotion。

### 4. 状态/debug trace 与比较器

保持公开 `architecture.csv` 七列 schema 不变；新增独立状态 trace：

```text
cycle,command_id,schedule_state,rtl_state,input_switch,transition_cause
```

- [x] 从 RTL `diagnostic.csv` 归一化状态转换、input switch 和状态持续时间。
- [x] 新 comparator 严格比较 gem5 state trace；原 architecture comparator 继续
  比较地址、beat、phase 与因果依赖。
- [x] fixture schema validator 检查 package 文件、manifest/snapshot、固定 mode
  值、五组 coverage 以及三组 hold-out workload 覆盖率，并校验 hold-out 标记。

**2026-07-15 状态：** 独立 state trace、RTL diagnostic 归一化、首差异定位和
fixture schema/SHA256 validator 已实现。baseline architecture trace 仍 strict 通过。
state trace 暴露的首段投影已按通用 RTL 结构修正：`REGISTER_LOAD ->
TRANSPOSE_LOAD` 使用 `register_load_done` cause，`TRANSPOSE_LOAD -> REUSE_LOAD`
使用 `scheduler.SA_SIZE`，input switch 使用 scheduler 寄存器、feeder
`STATE_DELAY`、`input_switch_d`、`REGISTER_DELAY` 和输出寄存器的具名和。该首段
已在 baseline strict run 中对齐；随后定位到 flow boundary 仍错误地由 gem5 array
admission 推进。trace 投影现已与执行 eligibility 分离，按 scheduler 的
`flow_times/data_last` 窗口、`TRANSPOSE_CLIP` 的 `SA_SIZE` 行和 `D_OUT` edge
推进。该 flow 序列在 baseline 中已对齐；最终 `REGISTER_UNLOAD` 的 input-switch
复位从 `sa_feeder.result_last/update_finished`、scheduler edge 和 feeder pipeline
推导。2026-07-15 baseline strict 验证通过：architecture comparator 与 state
comparator 均无差异，分别产生 18,446 条 architecture 数据行和 56 条 state
转换；不得添加 fixture 专用补拍。

**验收：** 人工构造的 state、guard、input-switch 错误必须定位到 command、cycle
和 transition cause。

### 4.5. 固化 RTL 存储时序契约

在扩展到五组 coverage 前，先把 SAU 可观察到的存储行为从当前 calibration 默认值
提升为显式、可测试的 RTL 契约。本步骤只建模影响 SAU 周期和请求顺序的边界，
不复制 SRAM bitcell、bank 内部实现或数值内容。

RTL 当前支持路径的存储契约为：

- 外部数据 SRAM 接口宽度为 256 bit，即每 beat 32 bytes；SAU 只有一个逻辑请求
  端口，每 cycle 最多接受一个外部请求。
- Operand-A 在 `REGISTER_LOAD` 阶段从外部 SRAM 读入内部
  `register_file_in`；执行阶段由该 resident storage 提供 A。Operand-B 在执行阶段
  从同一个外部 SRAM 端口流入 feeder。A 和 B 物理上可以位于同一 SRAM，但在
  固定 ATB + reuse-A 状态序列中不要求同拍读取。
- 外部读写共享接口；存在读请求时读优先，只有无读请求时才允许写。响应保持请求
  顺序。
- SAU SRAM 边界没有 ready/retry 信号。strict fixed-memory 路径不得注入 retry、
  bank contention 或可变响应延迟；这些只属于后续 non-strict timing-memory/DSE。
- 可见读响应延迟必须由 RTL 参数推导：
  `read_visible_latency = SRAM_DELAY + 1`。当前 manifest 的
  `SRAM_DELAY=3`，因此得到 4 cycles；4 不能继续作为与 RTL 参数脱离的默认裸值。

预计扩展 `TimingPolicy` 的 RTL 参数/存储子结构，并修改 fixture load、strict 参数
校验、`SauModel` 请求调度与 focused tests：

- [x] 定义具名存储契约字段：`beat_bytes=32`、`issue_width=1`、共享读写端口、
  read priority、in-order response，以及由 `SRAM_DELAY + 1` 得到的响应延迟。
- [x] strict fixture 只从 manifest/RTL 参数构造该契约，禁止 Python 默认值或 CLI
  override 改变它；non-strict DSE 可以显式覆盖延迟或启用 backpressure，但必须在
  ledger/config 中标记来源。
- [x] 为请求侧添加不变量检查：同一 cycle 最多一个外部请求，A preload、B stream
  与 writeback 共用一个 issue slot，读写竞争时读优先。
- [x] 明确两层存储边界：外部 SRAM 事件负责 A preload/B stream/writeback；
  `ARegisterFileIn` 只保存已装载的 resident A；B buffer 只表示 feeder/pipeline
  staging，不得形成 RTL 中不存在的无限预取窗口。
- [x] 增加 focused tests：`SRAM_DELAY=3 -> 4 cycles`、改变参数后延迟同步变化、
  连续请求仍保持单发射和有序响应、resident A 未完成时禁止执行、读写竞争优先级、
  strict 拒绝 override、non-strict override 仍可用于 DSE。

**2026-07-15 验证状态：** `RtlStorageTiming`、strict 参数边界、共享 issue-slot
不变量、固定响应调度和有界 B staging 已接入。strict 保持 RTL 无 ready 的固定
请求节奏，只检查已返回 B token 的 staging 上限；non-strict 才为在途响应预留
buffer 并反压请求。fixture validator 对八组 package
的 20,352 个 read 逐笔验证 4-cycle 可见延迟、单发射、有序响应及 A-before-B，
Python tests 18/18 通过；`timing_policy.test` 4/4、`csr_config.test` 7/7 通过。
baseline gem5 strict run 正常完成，architecture comparator 对 18,446 条数据行、
state comparator 对 56 条转换均无差异。strict 显式拒绝 read-latency override；
non-strict 以 5-cycle override 完成并逐笔观察到 5-cycle 延迟。Step 4.5 verified。

**验收：** timing ledger 以 `storage_read_visible` 记录所有外部 read 共用的延迟
公式，architecture trace 与运行时不变量逐笔检查 accepted/visible 关系；在固定
契约下所有响应满足 `visible_cycle = accepted_cycle + SRAM_DELAY + 1`，任何 cycle
不超过一个外部请求，所有 A preload 请求先于 B stream，resident A ready 后才允许
array execution。baseline 的 architecture/state strict 回归继续通过，且 strict
路径中不再存在独立于 RTL 参数的“4-cycle calibration”来源。

### 5. 五组 coverage 与三组 hold-out fixture 分级收敛

当前已验收的 fixture 清单如下：

| 分组 | Fixture | M × K × N |
| --- | --- | --- |
| coverage | `int8_gemm_32x32x32_single_flow` | 32 × 32 × 32 |
| coverage | `int8_gemm_32x256x256_m_sweep` | 32 × 256 × 256 |
| coverage | `int8_gemm_64x32x256_k_sweep` | 64 × 32 × 256 |
| coverage | `int8_gemm_64x256x32_n_sweep` | 64 × 256 × 32 |
| coverage | `int8_gemm_64x256x256_baseline` | 64 × 256 × 256 |
| hold-out | `int8_gemm_96x256x256_m_holdout` | 96 × 256 × 256 |
| hold-out | `int8_gemm_64x128x256_k_holdout` | 64 × 128 × 256 |
| hold-out | `int8_gemm_64x256x128_n_holdout` | 64 × 256 × 128 |

五组 coverage 按以下顺序接入，前一组通过并回归后再进入下一组：

1. `int8_gemm_32x32x32_single_flow`：建立最小单 flow 边界；
2. `int8_gemm_64x32x256_k_sweep`：优先验证 K/stream 深度变化；
3. `int8_gemm_64x256x32_n_sweep`：验证 N/输出与 flow 形状变化；
4. `int8_gemm_32x256x256_m_sweep`：验证 M/command 与 resident-A 装载变化；
5. `int8_gemm_64x256x256_baseline`：回归已对齐的多 flow 基准。

每个 coverage fixture 必须依次通过以下门槛，不能跳级宣称“已对齐”：

1. CSR replay 与 start snapshot decode 等价；
2. 外部 SRAM 的 A preload、B stream、writeback 地址、beat、请求顺序和
   accepted/visible 周期符合 Step 4.5；
3. resident A 的装载/ready 边界正确，B feeder/staging 不越过 RTL 允许的窗口；
4. state sequence、input switch、flow 边界与事件数量一致；
5. timing ledger 锚点、architecture trace 和 state trace 在 fixed-memory 下全行
   strict 一致；
6. 重新运行此前已通过的所有 coverage fixture，防止通用公式回归。

- [x] 接入固定 `trans_mode=01, reuse_mode=01` 的 small、M/K/N sweep 与 baseline
  五组 coverage package。
- [x] 每新增 coverage package 先完成上述门槛，才加入 strict suite；发现问题时
  只能修复通用 CSR/结构/guard 推导并重新跑全部 coverage fixture。
- [x] coverage 全部 fixed-memory strict 通过后冻结公式与参数来源，再接入三组改变
  M/K/N 的 RTL accepted hold-out package。
- [x] hold-out 只能验证泛化，禁止加入 fixture 专用 timing override；若失败，修复
  通用规则并回归全部八组。所有通过后才可声明该支持域具有泛化验证证据。
- [ ] Step 5.5 timing skeleton 完成且八组 fixed-memory strict 继续通过后，再为
  所有 supported fixture 运行
  timing-memory causal、stall/token 守恒和 DSE 单调性验证。retry、outstanding
  limit、buffer full 与 bank contention 只在该阶段评估，不参与 strict 周期校准。
- [ ] legacy 64x256x256 direct-command 与 CSR fixture trace 持续回归。

**2026-07-15 验收状态：** 五组 coverage 已按 small → K → N → M → baseline
顺序完成 fixed-memory architecture/state strict 验收，通用公式随后冻结；三组
M96/K128/N128 hold-out 未参与参数拟合，并在冻结后完成相同的 strict 验收。八组
fixture 共 16 个最终 comparator 均返回成功，且已加入 gem5 quick strict suite。
当前实现将
`scheduler.flow_times_i = flow_loop_times` 与
`scheduler.ins_times_i = vertical.ins_cycle` 分别保留为两个独立维度；B 地址按
`mem_addr.sv` 的 x/y/flow/instruction 嵌套计数器生成；resident extent 恰为
`SA_SIZE` 时，A 阵列
输入保留 `register_file_in`/feeder 的额外一行窗口。短 `flow_times_i == 1` 路径的
首个、第二个和稳态 gap/D_OUT 时长均由 `SA_SIZE`、SRAM/地址/控制延迟和寄存器
边沿组合得出。最终 input-switch reset 与 writeback 启动根据
`REGISTER_UNLOAD` 是否已覆盖结果尾部选择结构路径，不使用 fixture 名称或
workload profile。代码中的 instruction extent 命名为 `scheduleInstructions`，避免
与 RTL `flow_times_i` 混淆；`arrayFillCycles` 继续依据 feeder 的
`flow_loop_times_i` 推导。`REGISTER_UNLOAD` 提前覆盖结果尾部的 guard 对应
FIRST_INS/INS_LOOP 的单计数器边界：仅当 `flow_times_i` 与 `ins_times_i` 中恰有
一个等于 1 时成立；其余组合等待 `result_last/update_finished`。

K128 hold-out 首次验收时 architecture strict 已通过，但内部状态每个 instruction
累计晚 4 拍。RTL `scheduler.sv` 的 `flow_times_cnt`/`data_last` 关系表明通用
`REUSE_LOAD` 时长应为
`inputBeatsPerInstruction - SA_SIZE + flow_times_i`；旧式
`inputBeatsPerInstruction - BReadAhead` 只因 baseline 的
`flow_times_i == SA_SIZE - BReadAhead == 8` 而巧合成立。该结构修复加入
`TimingPolicy` ledger 与 K128 单测后，重新运行全部八组仍全部 strict 通过；没有
引入 fixture 名称、矩阵尺寸分支或 hold-out timing override。

### 5.5 从聚合延迟公式收敛到逐拍 RTL timing skeleton

Step 5 的八组 strict 通过证明当前实现具有多点一致性，但不能单独证明所有合法
CSR 都能被预测。删除 golden 后仍可运行也只证明运行时没有读取答案，不能证明
`arrayFillCycles`、short-path gap/drain 或 result gap 等聚合公式已经完整表达 RTL。
本步骤优先于 timing-memory causal/DSE，目标是：给定算子产生的一组 CSR 配置与
地址，在不需要对应 RTL trace 的情况下，由 RTL 时序骨架逐拍产生各阶段拍数与
总拍数；golden 只用于最终验收，不能参与运行、查表或 residual 拟合。

不新增 CPU、M/K/N 到 CSR 的生成器或新的仿真模式。模型边界仍是现有
`csr_writes.csv + manifest.json`；CPU/算子负责产生 CSR，SAU 只负责 replay、解码
和执行。`architecture.csv`、`diagnostic.csv` 是可选 golden，不是运行输入。

#### RTL 时序执行链

按以下链路审计 timing-relevant 状态、counter、valid/last 和寄存器边沿：

```text
csr.sv start/config
  -> SA_CORE command registers
  -> mem_addr x/y/flow/ins counters
  -> mem_ctrl + external SRAM request/visible response
  -> register_file_in resident load / transpose counters
  -> scheduler flow_times_cnt / ins_times_cnt / core_state
  -> feeder input_switch / valid delay chain
  -> SA token pipeline + sa_feeder result_valid/result_last
  -> register_file_out / output address counters
  -> writeback accepted/write_finished
  -> command_done
```

只复制会影响可观察拍数的时序骨架，不复制 RTL 状态编码、PE 数值数据通路或实际
乘加。阵列并行计算继续用 token、固定结构流水和 initiation interval 表达；每个
tick 依据旧状态计算 next state，并在拍末统一提交，以保持 RTL nonblocking
assignment 的边沿语义。

#### 周期来源表

开始修改前，先为每个 strict timing 项建立并维护下表；注释或“通过 golden”不能
代替 RTL 来源证明：

| 项目 | RTL 模块/信号 | 起始事件 | 结束事件/guard | CSR/参数依赖 | 当前状态 |
| --- | --- | --- | --- | --- | --- |
| command start | `csr.start_reg`/`scheduler.ins_valid`/`mem_addr.start` | accepted start CSR write | scheduler/address state activated | start register edge | 源码链已审计，待逐拍实现 |
| resident load | `register_addr`/`mem_ctrl`/`register_file_in` | first registered A request | `register_rdaddr_last` and delayed RF write | register-input CSR、SRAM delay | counter/流水已审计，末拍待波形确认 |
| transpose | `scheduler.transload_state_cnt` | `TRANSPOSE_LOAD` | counter clear/`data_last` | `SA_SIZE` | 源码 guard 已审计，待 counter 实现 |
| flow execute | `flow_times_cnt`/`ins_times_cnt` | `REUSE_LOAD` | `data_last` + counter clear | flow/ins CSR | 源码 guard 已审计，待 counter 实现 |
| flow boundary | `TRANSPOSE_CLIP`/`FIRST_LOAD` | flow clear | current-RTL `D_OUT_cond`/next execute guard | `SA_SIZE`、mode、`update_finished` | 旧 short 公式失效，待 result 边沿确认 |
| array/result | feeder/SA/`sa_feeder` valid pipeline | delayed A/B valid | transposer result valid/last | array结构参数 | counter/流水已审计，首末边沿待波形确认 |
| unload/writeback | `REGISTER_UNLOAD`/`register_file_out` | `result_accum_done` | output address counters + 2/4-stage pipe | output CSR、port contract | 源码链已审计，边沿待波形确认 |
| command complete | `sram_wr_last_o`/scheduler | four-stage write-finished pulse | `flow_end_o` | 固定寄存器边沿 | 源码链已审计，待逐拍实现 |

每一行最终必须标记为“RTL 源码已证明、逐拍实现、golden 已验证”；若源码存在跨
模块边沿歧义，先标记 unresolved。只有 unresolved 项允许请求少量定向 RTL
instrumentation，优先观察 `flow_times_cnt`、`ins_times_cnt`、
`transload_state_cnt`、`data_last`、`result_last`、`update_finished`、
`write_finished` 和 SRAM valid/response；不得先盲目新增完整 M/K/N package。

#### 实现与验收

- [x] 审计 `RtlTimingParameters` 的每个固定值，记录准确 RTL parameter、localparam
  或寄存器来源；manifest 未携带且源码无法证明的默认值视为未解释魔法数字。
- [ ] 将 strict 状态推进改为实际 counter/guard 驱动：至少覆盖 transpose、
  flow/ins、D_OUT/unload 和 command-done；禁止按预先计算的结束周期空转跳转。
- [ ] 用延迟队列或等价 token pipeline 表达 mem_ctrl、feeder、input-switch、
  result-last 和 output/writeback 的固定寄存器链。
- [ ] 逐步移除 strict 对 `arrayFillCycles`、`resultFlowGapCycles`、
  `first/second/steadyShort*Cycles` 和
  `finalDrainToInputSwitchResetCycles` 的运行时依赖；静态公式只允许保留位宽、
  counter 上限和经 RTL 证明的固定流水级数。
- [ ] 每个 command 从实际状态转换生成阶段汇总：stage instance、start cycle、
  end cycle、cycles，并以 `command_accepted -> command_complete` 给出总拍数。
- [ ] 每完成一段替换，重新运行 small、K、N、M、baseline 五组 coverage；冻结后
  再运行 M96/K128/N128 三组 hold-out。architecture/state strict 必须继续全行
  一致，且不得为回归添加 fixture/profile 分支。
- [ ] 用仅含 `manifest.json + csr_writes.csv` 的输入做独立预测 smoke test；该测试
  只验证输入边界。模型正确性仍由 RTL 来源表、逐拍实现和独立 golden 回归共同
  证明。
- [ ] Step 5.5 完成后，才继续八组 timing-memory causal、stall/token 守恒、DSE
  单调性和 legacy direct-command 回归。

**Definition of Done：** strict 路径的阶段与总周期由 CSR、具名 RTL 参数、实际
counter/valid/last/handshake 自然推进得到；不存在按 workload 预先预约的聚合结束
周期、裸延迟或 fixture 特判。对于尚未逐拍建模的 RTL 行为必须显式拒绝对应 CSR
支持域，不能以当前八组通过推断为任意尺寸均已对齐。

**2026-07-15 新 RTL 第一阶段审计：** 当前
`/home/xch/workspace/npu_lpnpu` 源码已完成 CSR start、resident、scheduler、feeder、
SA/result、output RF 和 write-finished 的静态链路审计，完整文件哈希、结构参数、
guard 与待观察信号见 `RTL_TIMING_PROVENANCE.md`。审计发现当前
`scheduler.D_OUT_cond` 已改由 `update_finished/update_finished_q` 驱动，且输出写回
增加 `result_accum_done` guard、两级地址/数据流水和四级完成流水；因此 Step 5 的
short/early-unload/completion 聚合公式不能继续作为运行时推进机制。RTL 负责人已
确认八组 package 来自这套最新 RTL，因此它们继续作为 strict oracle；其中
`diagnostic.csv` 已保存 start/state、memory last、result last、write 和 done 的当前
RTL 边沿。源码 guard 与这些导出边沿足以开始逐拍实现，不要求重新运行 RTL；若原始
FSDB 仍可取得，再用 `npi_fsdb_probe` 补查未导出的内部 counter/update 信号。

Phase B 已冻结最小组件边界：`schedule_state.{hh,cc}` 中新增纯逐拍
`RtlSchedulerSkeleton`，只复制 scheduler timing-relevant 状态、counter、guard 和
寄存器边沿，不接收 workload 尺寸、golden cycle 或预计算结束时间。该组件暂未接入
`SauModel`，所以不会改变现有 strict 输出；先通过 focused test 后，再依次接入
memory-last、result/update 和 write-finished 三类实际脉冲。

Phase C 的第一个 producer 已以隔离组件加入：`RtlResidentLoadSkeleton` 逐拍执行
`register_addr.sv` 的 x/y/channel counter，并产生注册后的 request valid/last。
baseline 的 `8*32*1` CSR extent 自然得到 256 个请求拍，scheduler 在相对第 258 拍
进入 `TRANSPOSE_LOAD`，对应当前 RTL 波形 28914→29172；没有使用 resident 聚合延迟。

Phase C 的第二个 producer 已以隔离组件加入：`RtlStreamLoadSkeleton` 逐拍执行
`mem_addr.sv` 的 `WAIT_TRIG/RUNNING/DONE`、x/y/flow/instruction counter、
`vertical_cnt_valid/last`、`rdaddr_last_d[1]`、`last_flow_time_d[1]` 和 9 拍
fallback counter，并从这些当前寄存器组合产生 `load_done_flag`。八组当前 CSR
snapshot 的 vertical shape 均为 `x_burst=1, y_cycle=32`，所以首个受支持契约只接受
该 shape，flow/ins extent 仍分别由 CSR 驱动；未验证 shape 显式拒绝。focused test
要求每个 `load_done` 对应 32 个注册 read-valid 拍，连续 flow 由延迟后的 last 自然以
33 拍间隔重触发；这 3 个测试连同已有 10 个测试已经由开发者构建并全部通过。
新增的耦合测试让 scheduler、resident 和 stream producer 在每拍共享同一份旧状态
snapshot，预期 baseline 第一条 instruction 的相对状态边沿为
`REGISTER_LOAD=2`、`TRANSPOSE_LOAD=258`、`REUSE_LOAD=290`、
`TRANSPOSE_CLIP=521`、`D_OUT=554`，分别对应 diagnostic 的
28916/29172/29204/29435/29468。开发者增量构建后，14 个测试全部通过，至首次
`D_OUT` 的逐拍控制链已闭合。该 producer 尚未接入 `SauModel`；下一小步开始
result/update producer，暂不替换 strict 聚合调度。

result/update 的首个隔离增量新增 `RtlExecuteUpdateSkeleton`：当前只接受已验证的
normal-int8 GEMM、non-retain 控制域，逐拍复制 `SA_ENGINE.calc_cnt`、
`internal_finish_pulse -> delay_finish_flag[0] -> acc_finish_flag_d1/PE_valid_o`
以及 `sa_feeder.update_state/update_finished`。计数器只在实际 `sa_en_i` 为真时推进，
所以输入 bubble 会自然停住计数，不会被折算成聚合 elapsed delay。非最终
instruction 从 `execute_done_flag_o` 产生注册后的 update pulse；最终 instruction
必须等待 `result_last_o`。开发者增量构建后 17 个测试全部通过；feeder enable 和
result serializer 尚未耦合。

耦合前的 diagnostic 复核发现一个不能用常数掩盖的观测缺口：按导出的
`input_switch_f` 与上一拍 `data_A_valid || data_B_valid` 重建 `sa_en_i`，baseline
command 1 首次进入/退出 `D_OUT` 时只累计 246/247 个 enable，第 256 个 enable 在
退出后 9 拍；但源码的 `calc_cnt` limit 明确为 `32*8=256`。single-flow 同样在
`D_OUT` 入口表现为少 10 个可见 enable，但退出受 result serialization 影响，不能
据此补一个“10 拍修正”。因此暂停 execute producer 的耦合，先为既有 baseline
定向导出 `sa_en_i/calc_cnt/internal_finish/execute_done/update_finished/update_q`
从第一段 stream 到首次 D_OUT 的内部边沿；这不是采集新尺寸或重新拟合。

### 6. 最终验证与交付

- [x] C++：CSR、decode、状态 guard、timing derivation。
- [x] Python：package schema、schedule comparator、architecture comparator、
  coverage/hold-out 分组与 M/K/N 覆盖验证。
- [ ] gem5：每 fixture strict/causal，DSE 单调性和 legacy baseline 回归。
- [ ] 更新 README、STATUS 和本文件，列出固定 supported control contract、
  被拒绝的 control 值、已验证的 CSR/尺寸支持域、hold-out 结果、RTL commit 与
  未解释阻塞项。
- [ ] 开发者手动完成增量编译、所有 supported fixture 通过后再提交并推送。

## 编译约束

agent 不主动编译 gem5。实现后仅建议最小增量命令：

```bash
scons --ignore-style build/RISCV/gem5.opt -j4
```

开发者确认编译完成后，agent 才运行 fixture、strict/causal 和脚本验证。
