# SAU int8 GEMM 周期级模型实施计划（中文版）

> **面向执行本计划的 agent：** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans`，按任务逐项实施。所有步骤使用 `- [ ]` 复选框跟踪。

**目标：** 实现并通过 RTL 标定的首个 gem5 SAU 周期级模型，支持 int8 GEMM、真实 256-bit timing 内存请求、retry/backpressure、token 流水、事件轨迹和可解释的性能统计。

**架构：** 使用 `ClockedObject` 驱动元数据 token 在操作数加载、阵列执行、排空和写回阶段之间流动。自定义 `RequestPort` 在明确的 SAU 时钟边沿逐 beat 发送请求；RTL 与 gem5 输出相同格式的 CSV，用于固定延迟环境下逐周期差分。

**技术栈：** C++17、gem5 SimObject/Python 配置、gem5 timing port 与事件队列、gem5 statistics、GoogleTest、Python 3 `unittest`、SystemVerilog/VCS。

---

## 1. 范围与执行规则

本计划只实现设计文档
`docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md`
定义的首个里程碑：

- 由测试配置直接注入已解码的 SAU 命令；
- 支持走 RTL reuse 路径的 int8 GEMM；这里 reuse 指操作数是否经过
  `register_file_in`，首里程碑建模观察到的 Operand-A preload 与 resident
  array-input 行为，但不做 CSR decode；
- 真实发送 timing read/write；
- 与固定延迟 SRAM 下的 RTL 逐周期标定；
- 验证可变延迟、retry 和 buffer backpressure。

本计划不实现：

- RISC-V `msetins1..7`；
- CSR、完成中断；
- 数值计算；
- int16、泛化 transpose engine、CSR reuse 字段 decode 以及所有算子专用的
  `register_file_in`/transpose 时序变体；
- PWConv、普通卷积、DWConv、padding；
- RTL 寄存器级等价。

工作跨两个仓库，必须分别提交：

- `/home/xch/workspace/npu_lpnpu`：只增加 RTL 轨迹采集。
- `/home/xch/workspace/gem5`：模型、测试、比较器、golden trace 和文档。

每项任务开始前运行 `git status --short`。不得覆盖用户已有修改，不得
clean/reset，不得删除目录或批量删除文件，不得格式化无关代码。

## 2. 文件结构

### npu_lpnpu

- 修改 `sim/testbench/tb/top_sau_regress_tb.sv`：输出 SAU 周期事件 CSV。
- 修改 `sim/vcs/script/case_sau_regress/Makefile`：增加确定性 trace 目标。

### gem5

- `src/sau/SConscript`：构建、SimObject、debug flag 和单元测试。
- `src/sau/Sau.py`：端口、时序、容量、trace 和测试命令参数。
- `src/sau/types.hh`：命令、数据流、beat、token 和状态类型。
- `src/sau/command.{hh,cc}`：命令构造及接收校验。
- `src/sau/address_generator.{hh,cc}`：外部 256-bit read beat 生成：
  Operand-A 每个 instruction preload 一次，Operand-B 按 flow streaming。
- `src/sau/a_register_file.{hh,cc}`：体系结构层面的 `register_file_in`
  抽象，用于记录 resident Operand-A，并生成虚拟 A 阵列输入 beat。
- `src/sau/token_pipeline.{hh,cc}`：buffer 与阵列流水。
- `src/sau/memory_port.{hh,cc}`：timing packet、retry 和 outstanding。
- `src/sau/trace_writer.{hh,cc}`：统一 CSV 输出。
- `src/sau/sau_model.{hh,cc}`：时钟推进、调度、统计和完成。
- `src/sau/*.test.cc`：C++ 单元测试。
- `configs/example/sau_timing.py`：独立仿真入口。
- `tests/gem5/sau/test_sau.py`：系统级 quick test。
- `tests/gem5/sau/ref/int8_gemm.csv`：RTL 基准轨迹。
- `util/sau/compare_trace.py`：strict/causal 比较器。
- `util/sau/compare_trace_test.py`：比较器单元测试。
- `src/sau/README.md`：使用说明与限制。

## 3. 跨任务稳定接口

中英文计划必须使用同一组接口名：

```cpp
namespace gem5::sau
{

enum class Operation : uint8_t { Gemm };
enum class Precision : uint8_t { Int8 };
enum class StreamKind : uint8_t { OperandA, OperandB, Output };
enum class Phase : uint8_t {
    Idle, OperandLoad, ArrayActive, ArrayDrain, Writeback, Complete
};
enum class EventKind : uint8_t {
    CommandAccepted, ReadAccepted, ReadResponseVisible,
    ArrayInputAccepted, ResultProduced, WriteAccepted,
    PhaseChanged, CommandComplete
};

struct StreamDesc
{
    Addr base = 0;
    uint32_t beats = 0;
    uint32_t strideBytes = 32;
    uint32_t flowStrideBytes = 0;
    uint32_t instructionStrideBytes = 0;
};

struct SauCommand
{
    uint64_t id = 0;
    Operation operation = Operation::Gemm;
    Precision precision = Precision::Int8;
    StreamDesc operandA;
    StreamDesc operandB;
    StreamDesc output;
    uint32_t flowLoops = 1;
    uint32_t instructionLoops = 1;
    uint32_t workItems = 0;
};

struct Beat
{
    StreamKind stream;
    Addr address;
    uint32_t index;
    bool last;
};

struct PipelineToken
{
    uint64_t commandId;
    uint32_t index;
    Cycles readyCycle;
    bool last;
};

} // namespace gem5::sau
```

首阶段采用 RTL 观察到的 matmul reuse 数据路径。这里的 “reuse” 按 RTL 语义
理解为是否使用 `register_file_in`；本计划在周期/时序层面建模这个边界，而不是
把它当成额外的功能模型：

由于当前目标算子的 CSR 配置都会走 `register_file_in`，direct-command 阶段先
直接假定该路径开启；后续 CSR 集成阶段再补 decode 和不同算子的时序差异。

```text
外部 SRAM read：
  对每个 instruction：
    先把 Operand-A 全部 preload 到 register_file_in 一次
    对每个 flow：
      从 SRAM response stream Operand-B

阵列输入：
  当当前 instruction 的 A 已 resident，且有一个 B beat 可见时：
    从 B response queue 接收 B
    从 resident register_file_in 生成一个虚拟 A beat
```

重复出现的 A array input 不代表重复发起外部 A SRAM read，因为它由模型中的
`register_file_in` resident 状态提供。这就是 RTL reuse 路径；但它仍不是 RTL
寄存器端口、bank、存储数据值或 transpose 行为的寄存器级建模。具体一拍偏移和
phase 边界以任务 1 产生的 RTL trace 为准。

---

## 任务 1：生成确定性的 RTL 周期基线

**文件：**

- 修改 `/home/xch/workspace/npu_lpnpu/sim/testbench/tb/top_sau_regress_tb.sv`
- 修改 `/home/xch/workspace/npu_lpnpu/sim/vcs/script/case_sau_regress/Makefile`

- [ ] **步骤 1：增加可选 CSV 采集器**

通过 `+sau_trace=<path>` 启用，未提供 plusarg 时不影响原回归。CSV 表头固定为：

```text
cycle,event,command_id,stream,address,beat,phase
```

监视层级必须使用：

```text
u_dut.u_dut_kui.SAU_1_inst
```

记录以下事件：

- `command_accepted`
- `read_accepted`
- `read_response_visible`
- `array_input_accepted`
- `result_produced`
- `write_accepted`
- `phase_changed`
- `command_complete`

stream 必须根据 `input_switch_s` 判断，不能根据地址猜测。command ID 按命令接收
顺序从 1 开始稳定分配。corrected Task 1 package 中
`INT8_SAU_MATMUL_TEST_ID_0` 含两个 command，不能再假设基准 trace 只有一个
command。

- [ ] **步骤 2：建立抽象 phase 映射**

不要直接输出 RTL 8 态编码。测试平台维护：

```text
idle
→ operand_load     （start）
→ array_active     （首个 array input）
→ array_drain      （最后一个 A 输入）
→ writeback        （之后首个输出写）
→ complete         （sau_crossbar_done）
```

同周期发生 phase 切换和锚点事件时，先输出 `phase_changed`，再输出锚点事件。

- [ ] **步骤 3：增加 `timing_trace` Make 目标**

核心命令：

```make
TRACE_CSV ?= $(LPNPU_HOME)/sim/vcs/build/sau_regress/int8_gemm_timing.csv

timing_trace: compile
	cd $(BUILD_DIR) && ./simv $(SIM_FLAG) \
	    +firmware=$(REGRESS_DIR)/INT8_SAU_MATMUL_TEST_ID_0 \
	    +sau_trace=$(TRACE_CSV) \
	    +TIMEOUT_NS=5000000
	@test -s $(TRACE_CSV)
	@grep -q "command_accepted" $(TRACE_CSV)
	@grep -q "command_complete" $(TRACE_CSV)
```

- [ ] **步骤 4：运行 RTL 基线**

```bash
make -f sim/vcs/script/case_sau_regress/Makefile timing_trace
```

预期：仿真出现 `TEST PASSED`，CSV 非空，至少有一个命令开始，且
`command_accepted` 与 `command_complete` 数量一致。
若缺少 VCS，必须记录命令与缺失工具，不得伪造 golden trace。

- [ ] **步骤 5：提交 RTL instrumentation**

```bash
git add sim/testbench/tb/top_sau_regress_tb.sv \
        sim/vcs/script/case_sau_regress/Makefile
git commit -m "test: emit SAU cycle timing trace"
```

---

## 任务 2：实现公共 trace 比较器

**文件：**

- 新建 `util/sau/compare_trace.py`
- 新建 `util/sau/compare_trace_test.py`
- 新建 `tests/gem5/sau/ref/int8_gemm.csv`

- [ ] **步骤 1：复制真实 RTL trace**

```bash
mkdir -p tests/gem5/sau/ref
cp ../npu_lpnpu/sim/vcs/build/sau_regress/int8_gemm_timing.csv \
   tests/gem5/sau/ref/int8_gemm.csv
```

- [ ] **步骤 2：先写失败测试**

测试必须覆盖：

- strict 模式接受完全一致的 trace；
- strict 模式报告一拍偏差；
- causal 模式允许整体延迟；
- causal 模式拒绝事件重排；
- 非法表头和未知事件报错。

- [ ] **步骤 3：确认测试先失败**

```bash
python3 -m unittest util.sau.compare_trace_test -v
```

预期：因 `compare_rows` 尚不存在而失败。

- [ ] **步骤 4：实现比较器**

`read_trace()` 校验七列表头，要求至少一个 `command_accepted`，并以第一个
`command_accepted` 为 cycle 0 归一化。多 command trace 合法，通过
`command_id` 区分。strict 比较全部字段和行数；causal 比较事件顺序、stream、
address、beat、phase，并要求实际 cycle 单调不减。

CLI：

```bash
python3 util/sau/compare_trace.py \
    --mode strict EXPECTED.csv ACTUAL.csv
```

- [ ] **步骤 5：验证测试通过**

```bash
python3 -m unittest util.sau.compare_trace_test -v
```

- [ ] **步骤 6：提交**

```bash
git add util/sau tests/gem5/sau/ref/int8_gemm.csv
git commit -m "test: add SAU timing trace comparator"
```

---

## 任务 3：定义命令类型和接收校验

**文件：**

- 新建 `src/sau/SConscript`
- 新建 `src/sau/types.hh`
- 新建 `src/sau/command.{hh,cc}`
- 新建 `src/sau/command.test.cc`

- [ ] **步骤 1：注册 GoogleTest**

```python
Import("*")
Source("command.cc")
GTest("command.test", "command.test.cc", "command.cc")
```

- [ ] **步骤 2：先写失败测试**

覆盖合法命令，以及：

- beat 数为 0；
- 地址不按 32 byte 对齐；
- loop 或 stride 为 0；
- `workItems` 与 A beat/loop 不一致；
- output beat 总数与 work item 不一致；
- 非 int8 GEMM。

- [ ] **步骤 3：确认编译失败**

```bash
scons build/ALL/sau/command.test.opt -j4
```

- [ ] **步骤 4：实现校验**

公开接口：

```cpp
void validateCommand(const SauCommand &command, unsigned beatBytes);
```

校验必须要求 `beatBytes == 32`，并按该粒度检查三个 stream 的
base 地址对齐。

所有乘法先用 64-bit 做溢出检查，再缩窄。

- [ ] **步骤 5：运行测试**

```bash
scons build/ALL/sau/command.test.opt -j4
./build/ALL/sau/command.test.opt
```

- [ ] **步骤 6：提交**

```bash
git add src/sau
git commit -m "feat: define SAU timing command"
```

---

## 任务 4：实现确定性的外部 beat 生成

**文件：**

- 新建 `src/sau/address_generator.{hh,cc}`
- 新建 `src/sau/address_generator.test.cc`
- 修改 `src/sau/SConscript`

- [ ] **步骤 1：先写 A preload → B streaming 顺序测试**

基准输入：

```text
A: 0x1000, 0x1020, 0x1040, 0x1060
B: 0x2000, 0x2020, 0x2040, 0x2060
```

测试两次 flow 与两次 instruction 的地址公式。Operand-A 外部 preload 不随 flow
重复，因为 flow 内的 A 复用发生在 `register_file_in` 内部：

```text
A: base + instruction * instructionStrideBytes + beat * strideBytes
B: base + instruction * instructionStrideBytes
   + flow * flowStrideBytes + beat * strideBytes
```

- [ ] **步骤 2：确认测试失败**

```bash
scons build/ALL/sau/address_generator.test.opt -j4
```

- [ ] **步骤 3：实现游标式 generator**

接口：

```cpp
class AddressGenerator
{
  public:
    explicit AddressGenerator(const SauCommand &command);
    bool empty() const;
    const Beat &front() const;
    void pop();
    uint64_t totalReadBeats() const;
};
```

只保存当前 stream/beat/flow/instruction 游标，不预生成全部 beat vector。
`front()` 先返回当前 instruction 的 Operand-A preload beat，然后返回每个 flow
的 Operand-B streaming beat。`last` 标记每次外部 stream occurrence 的最后
一个 beat。

- [ ] **步骤 4：运行测试**

```bash
./build/ALL/sau/address_generator.test.opt
```

- [ ] **步骤 5：提交**

```bash
git add src/sau
git commit -m "feat: generate SAU operand beats"
```

---

## 任务 4.5：建模 Operand-A `register_file_in` reuse 路径

**文件：**

- 新建 `src/sau/a_register_file.{hh,cc}`
- 新建 `src/sau/a_register_file.test.cc`
- 修改 `src/sau/SConscript`

- [ ] **步骤 1：注册并先写失败测试**

加入：

```python
Source("a_register_file.cc")
GTest("a_register_file.test", "a_register_file.test.cc",
      "a_register_file.cc", "command.cc")
```

测试覆盖：

```cpp
TEST(ARegisterFileIn, TracksAReadPreloadPerInstruction);
TEST(ARegisterFileIn, ReportsArrayReuseMoreThanExternalReads);
TEST(ARegisterFileIn, ProducesVirtualAArrayInputBeatsAfterPreload);
TEST(ARegisterFileIn, RejectsNonAExternalLoad);
```

关键不变量：

```cpp
EXPECT_EQ(registerFile.totalExternalLoadBeats(), command.operandA.beats);
EXPECT_EQ(registerFile.totalArrayInputBeats(),
          command.operandA.beats * command.flowLoops *
          command.instructionLoops);
```

- [ ] **步骤 2：确认失败**

```bash
scons build/ALL/sau/a_register_file.test.opt -j4
```

预期：因 `ARegisterFileIn` 尚未定义而失败。

- [ ] **步骤 3：实现抽象接口**

```cpp
class ARegisterFileIn
{
  public:
    explicit ARegisterFileIn(const SauCommand &command);

    void load(const Beat &beat);

    bool instructionReady(uint32_t instruction) const;
    uint32_t loadedBeats(uint32_t instruction) const;

    uint64_t totalExternalLoadBeats() const;
    uint64_t totalArrayInputBeats() const;

    Beat arrayInputBeat(uint32_t instruction, uint32_t flow, uint32_t beat,
                        uint32_t arrayIndex) const;
};
```

`load()` 只接受 `StreamKind::OperandA`。`arrayInputBeat()` 返回 address 为 0
的虚拟 Operand-A beat，因为 array input trace 是 SAU 内部事件，不是外部 SRAM
访问。

- [ ] **步骤 4：运行测试**

```bash
scons build/ALL/sau/a_register_file.test.opt -j4
./build/ALL/sau/a_register_file.test.opt
```

- [ ] **步骤 5：提交**

```bash
git add src/sau/a_register_file.hh src/sau/a_register_file.cc \
        src/sau/a_register_file.test.cc src/sau/SConscript
git commit -m "feat: model SAU A register file input"
```

---

## 任务 5：实现 token buffer 与阵列流水

**文件：**

- 新建 `src/sau/token_pipeline.{hh,cc}`
- 新建 `src/sau/token_pipeline.test.cc`
- 修改 `src/sau/SConscript`

- [ ] **步骤 1：先写失败测试**

覆盖：

- buffer 满时拒绝 push；
- fill latency；
- initiation interval；
- 最大 in-flight；
- token 守恒。

具体时序：fill=3、II=1，cycle 0/1 接收 token 0/1，cycle 3/4 产生结果。

- [ ] **步骤 2：确认失败**

```bash
scons build/ALL/sau/token_pipeline.test.opt -j4
```

- [ ] **步骤 3：实现接口**

```cpp
class TokenBuffer
{
  public:
    explicit TokenBuffer(size_t capacity);
    bool canPush() const;
    void push(PipelineToken token);
    const PipelineToken &front() const;
    void pop();
    size_t size() const;
};

class ArrayPipeline
{
  public:
    ArrayPipeline(Cycles fillLatency, Cycles initiationInterval,
                  size_t maxInFlight);
    bool canAccept(Cycles now) const;
    void accept(uint64_t commandId, uint32_t index, bool last, Cycles now);
    bool hasReady(Cycles now) const;
    PipelineToken takeReady(Cycles now);
    size_t inFlight() const;
};
```

- [ ] **步骤 4：运行测试**

```bash
./build/ALL/sau/token_pipeline.test.opt
```

- [ ] **步骤 5：提交**

```bash
git add src/sau
git commit -m "feat: model SAU token pipeline"
```

---

## 任务 6：建立 SimObject、参数、trace 和统计骨架

**文件：**

- 新建 `src/sau/Sau.py`
- 新建 `src/sau/trace_writer.{hh,cc}`
- 新建 `src/sau/sau_model.{hh,cc}`
- 修改 `src/sau/SConscript`

- [ ] **步骤 1：声明 `SauModel`**

必须包含：

- `system` 与 `memory` port；
- 32-byte beat；
- read/write issue width；
- read/write outstanding 上限；
- input/output buffer 容量；
- array fill/II/capacity；
- command-start latency；
- trace 文件与 `exit_on_done`；
- synthetic A/B/output 地址、beat、flow 和 instruction 参数。

- [ ] **步骤 2：注册构建项**

```python
SimObject("Sau.py", sim_objects=["SauModel"])
Source("trace_writer.cc")
Source("sau_model.cc")
DebugFlag("SAU")
```

- [ ] **步骤 3：实现 `TraceWriter`**

```cpp
void emit(uint64_t cycle, EventKind event, uint64_t commandId,
          std::string_view stream, Addr address, uint32_t beat,
          Phase phase);
```

文件为空字符串时禁用。`CommandComplete` 后 flush。

- [ ] **步骤 4：实现可构建的 idle model**

`SauModel` 继承 `ClockedObject`，实现：

- `getPort("memory")`
- `startup()`
- `tick()`
- `submitCommand()`
- `drain()`
- `statistics::Group`

startup 构造 synthetic command，计算 `workItems`，校验后接收。

- [ ] **步骤 5：构建**

```bash
scons build/ALL/gem5.opt -j4
```

- [ ] **步骤 6：提交**

```bash
git add src/sau
git commit -m "feat: add SAU timing SimObject"
```

---

## 任务 7：实现 timing memory port

**文件：**

- 新建 `src/sau/memory_port.{hh,cc}`
- 修改 `src/sau/sau_model.{hh,cc}`
- 修改 `src/sau/SConscript`

- [ ] **步骤 1：先加入 outstanding/blocked invariant**

要求：

- blocked packet 存在时不得发送新 packet；
- read/write outstanding 不得越界；
- outstanding 只在请求被接受后增加。

- [ ] **步骤 2：实现 `SauMemoryPort`**

```cpp
class SauMemoryPort : public RequestPort
{
  public:
    bool trySend(const Beat &beat, bool write);
    std::vector<Beat> takeVisibleResponses();
    bool canIssue() const;
    bool hasBlockedPacket() const;
    unsigned outstandingReads() const;
    unsigned outstandingWrites() const;

  protected:
    bool recvTimingResp(PacketPtr packet) override;
    void recvReqRetry() override;
};
```

每个 packet 使用准确的 32-byte 大小。write payload 清零。发送失败时仅保留一个
blocked packet；retry 成功后才发出 accepted 回调。

- [ ] **步骤 3：保证响应只在下一 SAU 边沿可见**

`recvTimingResp()` 只入队并在需要时调度 `nextCycle()`，不得直接修改
`InputBuffer`。

- [ ] **步骤 4：构建全部目标**

```bash
scons build/ALL/sau/command.test.opt \
      build/ALL/sau/address_generator.test.opt \
      build/ALL/sau/token_pipeline.test.opt \
      build/ALL/gem5.opt -j4
```

- [ ] **步骤 5：提交**

```bash
git add src/sau
git commit -m "feat: issue SAU timing memory requests"
```

---

## 任务 8：集成调度、阵列、写回和 drain

**文件：**

- 修改 `src/sau/sau_model.{hh,cc}`

- [ ] **步骤 1：增加调度所需成员状态**

在 `src/sau/sau_model.hh` 中加入：

```cpp
#include <deque>

#include "sau/a_register_file.hh"
#include "sau/address_generator.hh"
#include "sau/token_pipeline.hh"
```

在运行时状态中加入：

```cpp
std::optional<AddressGenerator> readGenerator;
std::optional<ARegisterFileIn> aRegisterFile;
std::deque<Beat> availableB;
TokenBuffer outputBuffer;
ArrayPipeline arrayPipeline;

uint32_t currentInstruction = 0;
uint32_t currentFlow = 0;
uint32_t currentArrayBeat = 0;
uint32_t nextArrayIndex = 0;
uint32_t nextResultIndex = 0;
uint32_t nextWriteIndex = 0;

uint64_t acceptedReadBeats = 0;
uint64_t visibleReadBeats = 0;
uint64_t arrayAdmissions = 0;
uint64_t resultsProduced = 0;
uint64_t writesAccepted = 0;
```

构造函数初始化：

```cpp
outputBuffer(outputBufferEntries),
arrayPipeline(arrayFillCycles, arrayIiCycles, arrayCapacity),
```

`submitCommand()` 接受命令后初始化：

```cpp
readGenerator.emplace(command);
aRegisterFile.emplace(command);
availableB.clear();
visibleMemoryResponses.clear();
currentInstruction = 0;
currentFlow = 0;
currentArrayBeat = 0;
nextArrayIndex = 0;
nextResultIndex = 0;
nextWriteIndex = 0;
acceptedReadBeats = 0;
visibleReadBeats = 0;
arrayAdmissions = 0;
resultsProduced = 0;
writesAccepted = 0;
```

这里必须保留 A 外部 preload 与 A 阵列输入复用的分层，不要再把每个
A array input 都建模成一次外部 SRAM read。

- [ ] **步骤 2：固定每个时钟边沿的执行顺序**

```cpp
consumeResponses();
advanceArray();
produceResults();
issueWrites();
issueReads();
updatePhase();
accountCycle();
```

- [ ] **步骤 3：实现 read response 消费**

`consumeResponses()` 将 `visibleMemoryResponses` 分流到对应资源：

```cpp
void
SauModel::consumeResponses()
{
    for (const auto &beat : visibleMemoryResponses) {
        ++visibleReadBeats;
        if (beat.stream == StreamKind::OperandA) {
            aRegisterFile->load(beat);
        } else if (beat.stream == StreamKind::OperandB) {
            availableB.push_back(beat);
        } else {
            panic("unexpected SAU read response stream");
        }
    }
    visibleMemoryResponses.clear();
}
```

旧计划里的 “A response 进入 inputBuffer” 已经过时。现在正确模型是：

```text
A response -> ARegisterFileIn preload
B response -> availableB queue
A array input -> 从已 resident 的 ARegisterFileIn 虚拟生成
```

- [ ] **步骤 4：实现 A 复用 + B streaming 的阵列接收**

`advanceArray()` 只有在以下条件同时满足时才接收一个 work token：

- 当前 instruction 的 A 已经 `aRegisterFile->instructionReady()`；
- `availableB` 非空；
- `arrayPipeline.canAccept(Cycles(sauCycle))`；
- output buffer/headroom 不会溢出；
- `nextArrayIndex < activeCommand->workItems`。

接收时先消费一个 B token，再生成虚拟 A token：

```cpp
const auto bBeat = availableB.front();
availableB.pop_front();
const Beat aBeat = aRegisterFile->arrayInputBeat(
    currentInstruction, currentFlow, currentArrayBeat, nextArrayIndex);
```

如果这是第一笔阵列输入，先切 phase：

```cpp
if (phase == Phase::OperandLoad) {
    phase = Phase::ArrayActive;
    traceWriter.emit(sauCycle, EventKind::PhaseChanged,
                     activeCommand->id, "none", 0, 0, phase);
}
```

随后按 RTL trace 顺序输出 B 再 A：

```cpp
traceWriter.emit(sauCycle, EventKind::ArrayInputAccepted,
                 activeCommand->id, "operand_b", 0, bBeat.index, phase);
traceWriter.emit(sauCycle, EventKind::ArrayInputAccepted,
                 activeCommand->id, "operand_a", 0, aBeat.index, phase);
```

然后送入阵列：

```cpp
arrayPipeline.accept(activeCommand->id, nextArrayIndex,
                     nextArrayIndex + 1 == activeCommand->workItems,
                     Cycles(sauCycle));
```

阵列输入游标顺序：

```text
beat within flow -> next flow -> next instruction
```

- [ ] **步骤 5：实现结果和写回**

每个完成的 work token 产生一个 output token。写地址为：

```text
output.base + beatIndex * output.strideBytes
```

`write_accepted` 必须在 `SauMemoryPort` 的 accepted 回调里输出，而不是在
response 时输出。如果 `trySend()` 返回 false，port 已经接管一个 blocked
packet 等待 retry；scheduler 不能继续从 `outputBuffer` 重发同一个 output
token。

- [ ] **步骤 6：实现外部 read/write issue**

`issueReads()` 只从 `readGenerator` 发送外部 SRAM read：

```cpp
unsigned issued = 0;
while (issued < readIssueWidth && readGenerator &&
       !readGenerator->empty() &&
       memoryPort.outstandingReads() < maxOutstandingReads &&
       memoryPort.canIssue()) {
    const Beat beat = readGenerator->front();
    const bool acceptedOrBlocked = memoryPort.trySend(beat, false);
    readGenerator->pop();
    ++issued;
    if (!acceptedOrBlocked) {
        ++stats.stallRequestRetry;
        break;
    }
}
```

`issueWrites()` 从 `outputBuffer` 发写请求。写 beat：

```cpp
Beat writeBeat{
    StreamKind::Output,
    activeCommand->output.base +
        static_cast<Addr>(nextWriteIndex) *
            activeCommand->output.strideBytes,
    nextWriteIndex,
    nextWriteIndex + 1 == activeCommand->output.beats *
        activeCommand->instructionLoops,
};
```

`memoryPort.trySend(writeBeat, true)` 返回后就 pop output token，因为 port
要么已经立即接受，要么已经把它保存为唯一 blocked packet 等待 retry。
`writesAccepted` 只能在 `requestAccepted()` 中递增，因为 retry 接受可能发生在
之后。

- [ ] **步骤 7：实现 phase 与完成**

```text
OperandLoad → ArrayActive → ArrayDrain → Writeback → Complete
```

phase 规则：

- `OperandLoad`：从 command accepted 到第一笔 array input；
- `ArrayActive`：仍可能接收新的 work token；
- `ArrayDrain`：最后一个 work token 已接收，但最后一个 result 还没出来；
- `Writeback`：第一笔 output write 已接受后，仍有 output token、blocked write
  packet 或 outstanding write；
- `Complete`：所有期望 write 已接受，且命令本地 packet 状态清空。

`phase_changed` 必须只发一次；与 anchor event 同拍时，先发 `phase_changed`。
`command_complete` 与切到 `Complete` 同拍发出。若 `exit_on_done=true`，调用
`exitSimLoop("SAU command complete")`。

- [ ] **步骤 8：实现 drain**

仅当无 blocked packet、无 outstanding request 且模型 idle/complete 时返回
`Drained`。首阶段不序列化执行中的命令。

- [ ] **步骤 9：加入守恒断言**

每拍检查：

```cpp
assert(readResponses <= acceptedReads);
assert(aRegisterFile->totalExternalLoadBeats() <= acceptedReads);
assert(arrayAdmissions <= aRegisterFile->totalArrayInputBeats());
assert(resultsProduced <= arrayAdmissions);
assert(writesAccepted <= resultsProduced);
assert(availableB.size() <= visibleReadBeats);
```

完成时要求 buffer 和 array pipeline 为空，命令期望计数全部相等。

- [ ] **步骤 10：如可行，增加 focused SauModel scheduler 单测**

如果不需要复制大量 gem5 Python config，就加一个小测试覆盖：

- A response 进入 `ARegisterFileIn`；
- B response 进入 `availableB`；
- `array_input_accepted` 按 B 再 A 输出；
- result token 与 write accepted 能前进。

如果这个测试在当前结构下太重，就在 `STATUS.md` 说明原因，并把端到端验证放到
任务 9 standalone simulation。

- [ ] **步骤 11：运行全部 C++/Python 单测**

```bash
scons build/RISCV/sau/address_generator.test.opt \
      build/RISCV/sau/a_register_file.test.opt \
      build/RISCV/sau/command.test.opt \
      build/RISCV/sau/memory_port.test.opt \
      build/RISCV/sau/token_pipeline.test.opt \
      build/RISCV/sau/trace_writer.test.opt \
      build/RISCV/gem5.opt -j4

./build/RISCV/sau/address_generator.test.opt
./build/RISCV/sau/a_register_file.test.opt
./build/RISCV/sau/command.test.opt
./build/RISCV/sau/memory_port.test.opt
./build/RISCV/sau/token_pipeline.test.opt
./build/RISCV/sau/trace_writer.test.opt
python3 -m unittest util.sau.compare_trace_test -v
```

- [ ] **步骤 12：提交**

```bash
git add src/sau/sau_model.hh src/sau/sau_model.cc
git commit -m "feat: run SAU GEMM timing pipeline"
```

---

## 任务 8.5：校准并建模 matmul 的 `TRANSPOSE_LOAD/CLIP` 时序路径

任务 9 不能在任务 8.5 之前开始，除非明确记录 standalone simulation 会与 corrected
RTL baseline 存在预期差异。

**文件：**

- 修改 `src/sau/Sau.py`
- 修改 `src/sau/sau_model.{hh,cc}`
- 修改 `src/sau/STATUS.md`
- 复用现有 SAU C++ 测试，并在任务 9 用 standalone trace 验证。

- [ ] **步骤 1：记录 corrected baseline 的 RTL 状态分布**

使用 corrected diagnostic trace：

```bash
python3 - <<'PY'
import csv, collections
p = "tests/gem5/sau/ref/int8_gemm_64x256x256/diagnostic.csv"
counts = collections.Counter()
with open(p) as f:
    for row in csv.DictReader(f):
        cycle = int(row["cycle"])
        if cycle <= 5897:
            counts[row["core_state"]] += 1
for state, count in counts.most_common():
    print(state, count)
PY
```

corrected package 中每个 command 的预期状态 span：

```text
REGISTER_LOAD    256 cycles
TRANSPOSE_LOAD    32 cycles
REUSE_LOAD      1854 cycles
TRANSPOSE_CLIP   231 cycles
D_OUT             99 cycles
FIRST_LOAD        33 cycles
REGISTER_UNLOAD  265 cycles
```

这些状态不是当前 matmul baseline 的可选边角路径。`TRANSPOSE_LOAD` 每个 command
出现一次，`TRANSPOSE_CLIP` 在多个 flow boundary 高频出现。

- [ ] **步骤 2：替换 “A/B 同拍成对 array input” 假设**

corrected public trace 显示 A/B array-input window 有 32-cycle skew：

```text
command 1: A array_input cycles 269..2392, B array_input cycles 301..2425
command 2: A array_input cycles 3394..5517, B array_input cycles 3426..5550
```

因此 gem5 不应要求每个 A/B array input 必须同周期接收。需要在周期级建模
transpose/reuse 路径引入的 32-cycle skew；当同周期同时出现 A/B 时，仍保持事件
顺序稳定。

- [ ] **步骤 3：增加显式时序参数**

默认值先匹配 corrected baseline：

```python
register_load_cycles = Param.Cycles(256, "A register_file_in preload span")
transpose_load_cycles = Param.Cycles(32, "matmul transpose-load span")
transpose_clip_cycles = Param.Cycles(33, "per-flow transpose-clip span")
final_first_load_cycles = Param.Cycles(33, "final first-load tail span")
register_unload_cycles = Param.Cycles(265, "writeback/unload completion span")
```

这些是 timing-policy 参数，不是 RTL 寄存器级状态复制。

- [ ] **步骤 4：更新 `SauModel` 调度**

调度器至少要能表达 corrected trace 形状：

- 外部 A read 在第一段 B stream 前完成；
- A array input 在 `REGISTER_LOAD + TRANSPOSE_LOAD` 后开始；
- B array input 相对 A array input 晚 32 cycles；
- 每个 flow boundary 有类似 `TRANSPOSE_CLIP` 的 gap；
- 末尾使用 `FIRST_LOAD/D_OUT` tail，再进入 register unload/writeback completion。

继续保留 token/request 守恒断言。

- [ ] **步骤 5：用任务 9 生成的 trace 验证形状**

任务 9 能生成 gem5 trace 后运行：

```bash
python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-fixed/sau.csv
```

完整标定前，至少要求 event order 与 stream/beat metadata 能对齐；strict cycle
accuracy 留到任务 10。

- [ ] **步骤 6：提交**

```bash
git add src/sau docs/superpowers/plans src/sau/STATUS.md
git commit -m "feat: model SAU matmul transpose timing path"
```

---

## 任务 9：增加固定与受限内存仿真

**文件：**

- 新建 `configs/example/sau_timing.py`
- 新建 `tests/gem5/sau/test_sau.py`

- [ ] **步骤 1：建立独立配置**

系统包含：

- 1GHz 可配置 SAU clock；
- `SystemXBar(width=32)`；
- `SimpleMemory`；
- `SauModel`；
- timing mode；
- synthetic command；
- trace 输出。

CLI 必须暴露 memory latency/variance/bandwidth、beat/flow、buffer、array
capacity 和 outstanding 参数。

- [ ] **步骤 2：运行固定内存**

```bash
./build/ALL/gem5.opt \
    --outdir=m5out/sau-fixed \
    configs/example/sau_timing.py \
    --memory-latency=3ns \
    --memory-latency-var=0ns \
    --memory-bandwidth=256GiB/s \
    --trace=m5out/sau-fixed/sau.csv
```

预期：退出原因是 `SAU command complete`，trace 包含完整事件。

- [ ] **步骤 3：运行受限内存**

```bash
./build/ALL/gem5.opt \
    --outdir=m5out/sau-constrained \
    configs/example/sau_timing.py \
    --memory-latency=20ns \
    --memory-latency-var=5ns \
    --memory-bandwidth=1GiB/s \
    --trace=m5out/sau-constrained/sau.csv
```

预期：仍能完成，总周期更长，且至少一个 memory/backpressure stall 非零。

- [ ] **步骤 4：注册两个 quick test**

使用 `gem5_verify_config` 注册 fixed 和 constrained 两个配置，验证退出码和
`SAU command complete`。

- [ ] **步骤 5：运行系统测试**

```bash
cd tests
./main.py run --skip-build gem5/sau
```

- [ ] **步骤 6：提交**

```bash
git add configs/example/sau_timing.py tests/gem5/sau
git commit -m "test: run SAU timing model with memory backpressure"
```

---

## 任务 10：与 RTL 基线逐周期标定

**文件：**

- 修改 `configs/example/sau_timing.py`
- 修改 `src/sau/Sau.py`
- 修改 `src/sau/sau_model.cc`
- 修改设计文档

- [ ] **步骤 1：生成未标定 gem5 trace**

```bash
./build/ALL/gem5.opt \
    --outdir=m5out/sau-rtl-match \
    configs/example/sau_timing.py \
    --rtl-profile \
    --trace=m5out/sau-rtl-match/sau.csv
```

- [ ] **步骤 2：确认 strict 首次失败**

```bash
python3 util/sau/compare_trace.py --mode strict \
    tests/gem5/sau/ref/int8_gemm.csv \
    m5out/sau-rtl-match/sau.csv
```

- [ ] **步骤 3：只校准有物理含义的命名参数**

允许调整：

- command start；
- response 到 token 可见；
- feeder；
- array fill/II/drain；
- result 到 write；
- 最后 write 到 complete。

禁止加入 command-ID 特判或某个事件专用的临时补拍。

- [ ] **步骤 4：要求 strict 完全一致**

行数、cycle、event、stream、address、beat、phase 必须完全相等。

- [ ] **步骤 5：受限内存使用 causal 比较**

```bash
python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm.csv \
    m5out/sau-constrained/sau.csv
```

- [ ] **步骤 6：提交标定结果**

```bash
git add src/sau/Sau.py src/sau/sau_model.cc \
        configs/example/sau_timing.py \
        docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md
git commit -m "test: calibrate SAU GEMM timing to RTL"
```

---

## 任务 11：最终回归、统计审计和文档

**文件：**

- 修改 `src/sau/sau_model.{hh,cc}`
- 修改系统测试和配置
- 新建 `src/sau/README.md`

- [ ] **步骤 1：审计统计项**

至少包含：

- command 数和总/分阶段周期；
- read/write 请求与字节数；
- outstanding 平均值和最大值；
- input/output buffer 平均/最大占用；
- array active/utilization；
- read/write retry 与 outstanding-limit stall；
- input starvation 与 output-full stall；
- first read/input/result、last result/write、complete offset。

- [ ] **步骤 2：增加 DSE 单调性测试**

比较：

- `array_capacity=1` 与 `16`；
- `output_buffer_entries=1` 与 `8`。

资源增加不得无解释地增加 `commandCycles`；较慢配置必须出现对应 stall。

- [ ] **步骤 3：编写 README**

说明构建、固定/受限内存命令、RTL strict 比较、参数、统计以及：

> 首阶段写回数据固定为 0，输出内容不具备功能正确性。

- [ ] **步骤 4：运行全部聚焦验证**

```bash
scons build/ALL/sau/command.test.opt \
      build/ALL/sau/address_generator.test.opt \
      build/ALL/sau/token_pipeline.test.opt \
      build/ALL/gem5.opt -j4
./build/ALL/sau/command.test.opt
./build/ALL/sau/address_generator.test.opt
./build/ALL/sau/token_pipeline.test.opt
python3 -m unittest util.sau.compare_trace_test -v
cd tests
./main.py run --skip-build gem5/sau
```

- [ ] **步骤 5：运行格式和 whitespace 检查**

```bash
git diff --check
pre-commit run --files src/sau configs/example/sau_timing.py \
    tests/gem5/sau util/sau
```

若环境没有 `pre-commit`，明确记录，不能擅自安装依赖。

- [ ] **步骤 6：提交首里程碑**

```bash
git add src/sau configs/example/sau_timing.py tests/gem5/sau \
        util/sau \
        docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md
git commit -m "docs: complete SAU timing model milestone"
```

## 4. 首里程碑完成标准

只有满足以下全部条件才算完成：

- 无 ISA 支持也能注入 int8 GEMM；
- 所有输入和输出 beat 都经过 gem5 timing memory；
- retry、outstanding 与 buffer 容量能真实阻塞流水；
- 固定延迟 SRAM 下与 RTL trace 逐周期一致；
- 可变延迟环境保持事件因果与 token 守恒；
- 统计能够解释命令全部延迟；
- unsupported mode 明确失败；
- 不声明或测试数值正确性。

## 5. 后续独立计划

首里程碑通过后，按以下顺序分别设计和实施：

1. CSR decode 后的 `register_file_in`/reuse 控制、transpose、int16；
2. PWConv、普通卷积、DWConv、padding；
3. RISC-V `msetins1..7`、CSR、完成中断；
4. 可选 Functional Backend。

每个扩展阶段都必须增加对应 RTL reference trace，并保留 int8 GEMM 作为回归。
