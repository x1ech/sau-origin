# SAU int8 GEMM 周期级模型实施计划

> **面向执行本计划的 agent：** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans`，按任务逐项实施。所有步骤使用 `- [ ]` 复选框跟踪。

**目标：** 实现并通过 RTL 标定的首个 gem5 SAU 周期级模型，支持 int8 GEMM、真实 256-bit timing 内存请求、retry/backpressure、token 流水、事件轨迹和可解释的性能统计。

**架构：** 使用 `ClockedObject` 驱动元数据 token 在操作数加载、阵列执行、排空和写回阶段之间流动。自定义 `RequestPort` 在明确的 SAU 时钟边沿逐 beat 发送请求；RTL 与 gem5 输出相同格式的 CSV，用于固定延迟环境下逐周期差分。

**技术栈：** C++17、gem5 SimObject/Python 配置、gem5 timing port 与事件队列、gem5 statistics、GoogleTest、Python 3 `unittest`、SystemVerilog/VCS。

---

## 1. 范围与执行规则

本计划只实现设计文档
`gem5/.worktrees/sau-command-types/src/sau/PLAN.md`
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

状态：当前 worktree 已实现。这个任务把 corrected 64x256x256 RTL baseline
转换成 gem5 SAU 的可配置周期级 timing policy。它不复制 RTL 寄存器或算术
datapath 内部细节；它建模的是任务 9 standalone simulation 之前必须具备的
architectural trace 形状。

**文件：**

- 修改 `src/sau/Sau.py`
- 修改 `src/sau/sau_model.{hh,cc}`
- 修改 `src/sau/SConscript`
- 修改 `src/sau/command.{cc,test.cc}`
- 新建 `src/sau/array_input_scheduler.{hh,cc,test.cc}`
- 新建 `src/sau/result_scheduler.{hh,cc,test.cc}`
- 修改 `src/sau/STATUS.md`
- 复用 SAU C++ 单元测试、trace comparator 测试、style check 和
  `build/RISCV/gem5.opt` 编译验证。任务 9 仍负责生成 standalone trace 并与
  RTL baseline 端到端比较。

- [x] **步骤 1：记录 corrected baseline 的 RTL 状态分布**

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

- [x] **步骤 2：替换 “A/B 同拍成对 array input” 假设**

corrected public trace 显示 A/B array-input window 有 32-cycle skew：

```text
command 1: A array_input cycles 269..2392, B array_input cycles 301..2425
command 2: A array_input cycles 3394..5517, B array_input cycles 3426..5550
```

因此 gem5 不应要求每个 A/B array input 必须同周期接收。需要在周期级建模
transpose/reuse 路径引入的 32-cycle skew；当同周期同时出现 A/B 时，仍保持事件
顺序稳定。

- [x] **步骤 3：增加显式时序参数**

已实现的默认参数匹配 corrected baseline：

```python
array_input_start_delay_cycles = Param.Cycles(
    269, "Command acceptance to first A array-input eligibility")
array_input_skew_cycles = Param.Cycles(
    32, "Cycles by which B array input lags A array input")
array_input_burst_beats = Param.Unsigned(
    32, "Array-input beats issued per burst before inserting a gap")
array_input_burst_gap_cycles = Param.Cycles(
    1, "Empty cycles between adjacent array-input bursts")
array_input_flow_gap_cycles = Param.Cycles(
    3, "Empty cycles between array-input flow/tile groups")
array_fill_cycles = Param.Cycles(
    343, "Cycles from first array input to first result")
result_flow_gap_cycles = Param.Cycles(
    234, "Empty cycles between result flow groups")
writeback_start_delay_cycles = Param.Cycles(
    8, "Cycles from last result to first writeback eligibility")
completion_delay_cycles = Param.Cycles(
    4, "Cycles from last accepted write to command completion")
```

这些是 timing-policy 参数，不是 RTL 寄存器级状态复制。旧设想中的
`register_load_cycles`、`transpose_load_cycles`、`transpose_clip_cycles`、
`final_first_load_cycles`、`register_unload_cycles` 没有加入代码，因为它们把
RTL state label 和 gem5 architectural scheduling boundary 混在了一起。

- [x] **步骤 4：更新 `SauModel` 调度**

当前实现通过以下方式表达 corrected trace 形状：

- `ArrayInputScheduler`：支持 A/B 独立 issue、32-beat burst、1-cycle burst
  gap、3-cycle flow gap，以及 B 相对 A 的 32-cycle skew；
- first A array input 对 corrected baseline 锚定在 command acceptance 后第
  269 cycle；
- `ResultScheduler`：把大量 array-input work token 转换成较少的 result/write
  beat，并建模 343-cycle fill latency 与 234-cycle result-flow gap；
- command validation 允许 `expectedOutputBeats < workItems`，匹配 GEMM reduction
  路径；
- writeback start 与 local command completion 都显式延迟。

继续保留 token/request 守恒断言。

- [x] **步骤 5：验证组件级 timing 行为**

运行：

```bash
scons build/RISCV/sau/array_input_scheduler.test.opt \
      build/RISCV/sau/result_scheduler.test.opt \
      build/RISCV/sau/command.test.opt -j4
./build/RISCV/sau/array_input_scheduler.test.opt
./build/RISCV/sau/result_scheduler.test.opt
./build/RISCV/sau/command.test.opt
python3 -m unittest util.sau.compare_trace_test -v
scons build/RISCV/gem5.opt -j4
git diff --check
```

预期：所有测试 PASS，`build/RISCV/gem5.opt` 编译通过。如果 host 没有可选的
Capstone/HDF5 库，对应 warning 可以接受。

- [ ] **步骤 6：在任务 9 中比较 gem5 standalone trace**

任务 9 能生成 gem5 trace 后运行：

```bash
python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-fixed/sau.csv
```

完整标定前，至少要求 event order 与 stream/beat metadata 能对齐；strict cycle
accuracy 留到任务 10。

- [ ] **步骤 7：提交**

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

状态：先执行前半段 profile 对齐。Task 9 已能生成 standalone gem5 trace，但它
仍只发一个 synthetic command，且地址是 synthetic 地址；corrected RTL baseline
包含两个 command 和 RTL 实际地址。因此本任务先把 gem5 standalone 的命令数量、
地址和流形状对齐到 `tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv`，
再进入 strict 周期标定。不要在 profile 尚未对齐时调 cycle 参数。

**文件：**

- 修改 `configs/example/sau_timing.py`
- 修改 `src/sau/Sau.py`
- 修改 `src/sau/sau_model.cc`
- 修改设计文档

- [x] **步骤 0：提取并记录 RTL profile**

从 imported RTL reference 提取：

- command 数量；
- 每个 command 的 A/B/output base 地址；
- A/B/output beat 数；
- flow/instruction 形状；
- read、array input、result、write、complete 的事件数量。

如果 reference 和当前 direct-command 参数不能一一表达，先增加通用命名参数，
不要写 command-ID 特判。

- [x] **步骤 1：生成 profile 对齐但未标定的 gem5 trace**

```bash
./build/RISCV/gem5.opt \
    --outdir=m5out/sau-rtl-match \
    configs/example/sau_timing.py \
    --rtl-profile \
    --trace=m5out/sau-rtl-match/sau.csv
```

- [ ] **步骤 2：记录当前 causal profile 差异**

```bash
python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-rtl-match/sau.csv
```

预期：profile 对齐后，事件/stream/address/beat/phase 序列应匹配；cycle 可不同。
如果仍失败，优先修 command 数量、地址、beat 编号和 phase/event 顺序。

当前状态：2026-07-08 的 `--rtl-profile` trace 已匹配 RTL 的 command 数量、
行数、事件计数和 A/B/output 地址范围；causal compare 仍在事件顺序上失败，
首个 mismatch 为第 6 行，RTL 期待 Operand-A beat 4 的 `read_accepted`，
gem5 当前提前出现 Operand-A beat 0 的 `read_response_visible`。后半段从这里继续。

已定位的 mismatch 原因不是矩阵计算或尺寸错误，而是事件序列仍未对齐：

1. 同一拍或相邻拍内的 read issue/response 可见顺序不同。RTL trace 在发出
   Operand-A beat 4 的 `read_accepted` 后，才记录 Operand-A beat 0 的
   `read_response_visible`；旧的 gem5 `tick()` 先 emit visible responses，
   后 `issueReads()`，因此同一调度窗口里 response 会排到后续 read 前面。
   2026-07-09 已通过把 `issueReads()` 移到 visible response 处理前修正这
   一层顺序；新的首个 causal mismatch 推进到 row 510。
2. 固定 profile 下的 timing memory 节奏仍不是 RTL SRAM profile。RTL command 1
   的 A read 基本是 cycle 3 起每拍一个，response 从 cycle 7 起每拍一个；
   当前 gem5 经 `SystemXBar + SimpleMemory` 后首批 read/response 为
   0,2,2,4,14.../14,16,18...，会把后续 B 输入窗口拖长。
3. writeback 与 array input 的相对边界仍不符合 RTL。RTL command 1 的最后
   array input 在 cycle 2425，first write 在 cycle 2513；当前 gem5 first write
   在 cycle 3170，但最后 B array input 到 cycle 8134 才结束，说明模型仍允许
   writeback 与未完成的 B array input 长时间交叠。

后续步骤先处理最早可见的序列问题：让固定 RTL profile 下同一 SAU tick 内的
`read_accepted` trace 顺序与 RTL 一致，再决定是用更贴近 RTL 的固定 timing
memory profile，还是继续通过现有 timing port 参数逼近。

2026-07-09 更新：read accepted/response 的局部顺序已对齐，row 6 mismatch
消失。新的 row 510 mismatch 是 gem5 在最后几个 Operand-A response visible
之前已经开始发 Operand-B read；RTL 则先完成 Operand-A response visible，
进入 `array_active`，发出约 24 个 Operand-A array-input token 后才开始
Operand-B external read。下一步应增加 A preload 完成到 B streaming 启动之间的
RTL-observed barrier/offset，而不是继续改 command/profile 形状。

2026-07-09 追加更新：已增加 `b_read_start_ahead_beats`，`--rtl-profile`
设置为 24，使 Operand-B external read 等到 A preload response 全部可见并且
A array-input 领先 24 个 token 后才启动；row 510 mismatch 消失。同时增加
`b_stride_bytes`，`--rtl-profile` 设置 `b_stride_bytes=0x100` 和
`b_flow_stride=0x20`，使 Operand-B read 地址序列匹配 RTL：
`0x29124000, 0x29124100, ...`。新的首个 causal mismatch 为 row 541：
RTL 每拍接受一个 B read，当前 gem5 仍受 `SystemXBar + SimpleMemory` timing
节奏影响，B read accepted cadence 会跳拍或同拍接受多笔。下一步应处理 RTL
calibration run 的固定 memory cadence，而不是继续调整地址/profile。

- [x] **步骤 3：拆分 calibration-only 与 system/backpressure 模式**

Task 10 的 RTL 逐周期标定应先使用 calibration-only 固定内存节奏：

- read accepted 固定为每个 SAU cycle 最多 1 beat；
- read response 使用固定可见延迟，并按 RTL profile 每拍最多可见 1 beat；
- write accepted 固定为每个 SAU cycle 最多 1 beat；
- 不经过 `SystemXBar + SimpleMemory` 的 retry、带宽仲裁、同拍聚合或额外排队。

`SystemXBar + SimpleMemory` 路径继续保留给 Task 9 和后续 system/backpressure
实验。它用于观察真实 gem5 memory hierarchy 对 SAU 的反压影响，不作为 strict
RTL 对齐的依据。row 541 mismatch 正是当前两类目标混在一起后的表现：profile
形状和地址已经对齐，但 memory accepted cadence 仍是系统仿真节奏，不是 RTL
SRAM profile。

- [x] **步骤 4：实现 calibration-only fixed memory cadence**

新增显式开关，例如 `--calibration-memory` 或 `--rtl-memory-cadence`，只在
RTL 标定运行中启用。实现时仍复用现有命令、beat、scheduler 和 trace 结构，
只替换外部 memory accepted/response 的节奏来源；不要加入 command-ID 特判，
也不要为 row 541 写事件专用补丁。

建议最小实现目标：

- fixed read accept：若 SAU 本地有待发 read，则每拍接受 1 个；
- fixed read response：accepted read 经过固定延迟后，每拍可见 1 个 response；
- fixed write accept：若 writeback 有待发 write，则每拍接受 1 个；
- trace 仍输出同一套 `read_accepted`、`read_response_visible`、
  `write_accepted` 和 phase 事件，便于继续用 comparator。

2026-07-09 更新：已实现 `--calibration-memory` 和
`--calibration-read-latency-cycles`。该模式绕过 timing port 的 request/retry
路径，只在 `SauModel` 内部按固定节奏记录 accepted 和 visible response；
默认关闭，因此 Task 9 的 `SystemXBar + SimpleMemory` system/backpressure
路径保持不变。

- [x] **步骤 5：在 calibration-only 模式下重新要求 causal 对齐**

```bash
./build/RISCV/gem5.opt \
    --outdir=m5out/sau-rtl-match \
    configs/example/sau_timing.py \
    --rtl-profile \
    --calibration-memory \
    --trace=m5out/sau-rtl-match/sau.csv

python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-rtl-match/sau.csv
```

预期：row 541 这类由 `SystemXBar + SimpleMemory` cadence 引入的 mismatch
应消失。若 causal 仍失败，下一处 mismatch 才更可能是 SAU 内部 scheduler、
array/writeback overlap 或 phase 边界问题。

2026-07-09 结果：row 541 的 B read accepted cadence mismatch 已消失，B read
accepted 在 calibration-only trace 中变为 293、294、295... 每拍一笔，且 trace
行数仍与 RTL 相同。新的首个 causal mismatch 推进到 row 562：RTL 在 cycle 301
同一拍内记录 Operand-B array-input beat 0 后继续记录 Operand-A array-input
beat 32；当前 gem5 把 Operand-A beat 32 推迟到 cycle 302。下一步应检查 array
input scheduler 与 array pipeline 的同拍 A/B admission 规则。

2026-07-09 追加结果：已修复 row 562 及后续 B-read burst gap/result ordering
问题。`--rtl-profile --calibration-memory` 生成的 trace 与 RTL reference 在
causal mode 下完全一致；行数保持 18447。关键修正包括：

- 同拍 B/A array input 中，B 作为 work admission 消耗 array pipeline II，
  A 作为同拍 additional input 进入 pipeline 但不推进 II；
- calibration B read issue 复用 32-beat tile gap 和 flow gap；
- calibration result trace 由 `ResultScheduler` 直接驱动，避免 trace-only
  pipeline token 排列影响已标定的 result schedule。

- [x] **步骤 6：确认 strict 首次失败**

```bash
python3 util/sau/compare_trace.py --mode strict \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-rtl-match/sau.csv
```

2026-07-09 当前 strict 首次失败：事件序列和元数据已经一致，但 cycle 字段仍
不一致。command 1 的 `command_accepted` / `command_complete` 已与 RTL 同为
cycle 0 / 2772，但首个 `read_accepted` 比 RTL 早 3 拍；command 2 的
`command_accepted` 已与 RTL 同为 cycle 3125，但后半段周期仍被拉长。下一步
应校准 command-local start offset，并检查跨 command 的 scheduler/pipeline
状态复位或 command-local timing 边界。

- [x] **步骤 7：只校准有物理含义的命名参数**

允许调整：

- command start；
- response 到 token 可见；
- feeder；
- array fill/II/drain；
- result 到 write；
- 最后 write 到 complete。

禁止加入 command-ID 特判或某个事件专用的临时补拍。

- [x] **步骤 8：要求 strict 完全一致**

行数、cycle、event、stream、address、beat、phase 必须完全相等。

2026-07-10 结果：以 `scons --ignore-style` 重建后，
`--rtl-profile --calibration-memory` 生成的 trace 通过 strict comparison。
两份 trace 都有 18446 条数据行；command 1 的首读/完成周期为 3/2772，
command 2 为 3128/5897。修复使用命名的 `command_start_cycles=3` feeder
启动边界，以及每命令重置的 `ArrayPipeline` timing epoch，没有 command-ID
特判或事件专用补拍。

- [ ] **步骤 9：受限内存使用 causal/system 比较**

```bash
python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-constrained/sau.csv
```

该步骤不要求 constrained/system trace 与 RTL strict 对齐；它只检查在带反压的
系统路径中，事件 profile、地址、beat 编号和 phase progression 没有退化。

- [ ] **步骤 10：提交标定结果**

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

1. CSR decode 和 mode-derived command generation；
2. CSR decode 后的 `register_file_in`/reuse 控制、transpose、int16；
3. PWConv、普通卷积、DWConv、padding；
4. RISC-V `msetins1..7`、CSR、完成中断；
5. 可选 Functional Backend。

每个扩展阶段都必须增加对应 RTL reference trace，并保留 int8 GEMM 作为回归。

### 任务 12：SAU CSR decode 和 mode-derived command generation

**定位：** 这是首里程碑之后的独立任务，不阻塞 Task 10 的 profile alignment、
calibration-only memory cadence 或 strict cycle calibration。当前 direct-command
路径仍然是合法的抽象命令入口；后续 CSR decode 的目标是把 RTL/软件侧寄存器配置
稳定翻译成同一个 `SauCommand` 和 timing policy 边界。

**目标：**

- 保留当前 `SauCommand` 作为已解码的架构意图；
- 新增一层显式的 CSR/register 配置输入，记录 RTL 侧 `work_mode`、
  `register_ystep_i`、x/y step、reuse、transpose、precision、输出形状等字段；
- 实现 `CSR/register config -> SauCommand + TimingPolicy` 的 decode；
- 对当前 int8 GEMM baseline，CSR decode 后生成的 command/profile 必须等价于
  现有 `--rtl-profile` direct-command baseline；
- unsupported mode 必须明确失败，不能静默走错 timing path。

**文件：**

- 可能修改 `src/sau/types.hh`
- 可能新增或修改 `src/sau/command.{hh,cc}` / CSR decode helper
- 可能修改 `src/sau/sau_model.{hh,cc}`
- 可能修改 `configs/example/sau_timing.py`
- 增加对应 `src/sau/*.test.cc`
- 更新 `src/sau/README.md` 和 `src/sau/STATUS.md`

- [ ] **步骤 1：整理 RTL/软件侧 CSR 字段定义**

从 RTL 和软件启动流程中记录当前 baseline 真实依赖的寄存器字段，至少包括：

- `work_mode`；
- `register_ystep_i`；
- x/y step 和 stride 类字段；
- `register_file_in` / reuse 控制；
- transpose 或输入重排相关控制；
- precision、输出 shape、loop/work item 相关字段。

字段语义必须来自 RTL/软件上下文，不能只根据 gem5 当前 command 反推。

- [ ] **步骤 2：定义 CSR config 与 command 的边界**

新增独立结构表达原始或已规整的 SAU CSR 配置，例如 `SauCsrConfig` 或同等命名。
它应与 `SauCommand` 分离：

- CSR config 表示“软件/RTL 写了什么寄存器”；
- `SauCommand` 表示“gem5 SAU 已解码后要执行什么抽象工作”；
- timing policy 表示“该工作在当前 RTL profile 下如何排拍”。

direct-command 注入路径继续保留，用于 Task 10 标定、单元测试和未来回归。

- [ ] **步骤 3：实现 baseline decode**

先只支持当前 int8 GEMM baseline。decode 后必须生成与现有 `--rtl-profile`
一致的：

- command 数量；
- A/B/output base、stride、loop 和 beat 数；
- `register_file_in` reuse 路径选择；
- calibrated timing policy 参数；
- unsupported CSR/mode 的显式错误。

不在本步骤实现数值计算，也不把 transpose 做成完整功能模型；只把它作为
mode-derived timing/dataflow 选择的一部分。

- [ ] **步骤 4：增加 decode 单元测试**

至少覆盖：

- 当前 baseline CSR 配置 decode 后等价于现有两条 direct command；
- unsupported `work_mode` 或未建模 transpose/reuse 组合会失败；
- decode 不改变 Task 10 现有 `--rtl-profile --calibration-memory` trace 形状。

- [ ] **步骤 5：再接入软件/CSR 写入路径**

只有在 decode 边界和 baseline 测试稳定后，再设计 RISC-V `msetins1..7`、
CSR 写入、完成中断和真实软件驱动路径。该步骤应作为后续独立任务继续展开，
不要混入 Task 10 的 strict cycle calibration。
