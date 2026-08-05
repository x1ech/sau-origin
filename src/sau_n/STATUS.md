# Gem5 Im2Col Reference RTL 独立周期模型状态

最后更新：2026-08-05

## 2026-08-05 D 写回期间禁止新的 A/B/C 读 grant

已同步 `soc_gem5/gem5_cpu_veu/src/sau_n` 的 D 写回仲裁行为：当本拍存在 D
写请求时，不再为 A/B 发起新的 scratchpad 读 grant；上一拍已经发出的读请求仍
可在本拍正常接收响应。C 初始化阶段的独占读路径保持不变。

- 修改 `streaming_conv_pipeline_model.cc` 的共享 scratchpad 仲裁选择；
- 在 `streaming_conv_pipeline_model.test.cc` 增加 D-only 写回回归检查，并放宽
  A 的 request/grant 数量必须相等这一不再成立的断言；
- 已执行 `git diff --check`；按目录规则未主动编译或运行测试。

## 2026-07-30 双缓冲共享数据流计划

已在 `PLAN.md` 第 13 节新增下一阶段计划：以共享 16-bank scratchpad 为基础，建模
A/B/C/D 四个区域、B0/B1 权重双缓冲、真实 bank 仲裁、16x16 SA 消费以及 D 区
int8 写回。该阶段保持项目自有 SA，C 为 int16 bias，D 为最终 int8 output，并保留
跨 spatial tile 的权重复用。

本次仅更新计划和状态记录，未修改 RTL/C++，未执行 gem5 编译或测试。

随后完成了对该阶段计划的源码可行性复核，并修订以下必要契约：

- `PipelinedIm2ColModel` 需要拆出 shared A request/grant/response 接口，同时保留
  standalone Im2Col 兼容路径；
- B0/B1 只有覆盖完整 `[0, K)` 时才能声明跨 spatial tile 完整复用，小容量配置
  必须重新读取被覆盖的 K entry；
- scratchpad 一拍响应使用逐 bank in-flight requester/destination tag；
- streaming-only `shared_spad` 配置保持现有 common fixture 和 golden hash 不变；
- D pending queue 有限且参与 SA output backpressure，最终结果由 D 区重建；
- 完整 K 驻留 profile 必须验证 A 连续时 SA 连续 K 拍 input fire。

本次复核仍只修改 `PLAN.md` 和本状态记录，未修改 RTL/C++，未执行 gem5 编译或
测试。

第二轮修订已吸收独立 subagent 的首轮审核意见：

- 完整 K 配置等待 C、A tile-first 和 B `[0,K)` 全部 ready 后才 launch；小容量
  配置只等待包含 K0 的 active chunk；
- shared A response 先形成 working S1，再生成下一轮请求，避免重复读取已完成 lane；
- standalone Im2Col 明确保留现有组合响应和逐拍 timing；
- C bias 只在最终 MAC/drain 后的现有 bias phase 加入一次；
- 第 12 节历史限制不约束第 13 节明确需要的固定一拍 tag 和有限 pending queue；
- depth 1 D queue 支持 old head 完整 retire 时同拍 dequeue/enqueue，并补全可重算
  trace 字段。

本轮仍未修改 RTL/C++，也未执行 gem5 编译或测试；修订后的计划将再次交由独立
subagent 只读审核。

第二个独立 subagent 完成复审：首轮六项修订均通过，但发现计划仍有一个阻塞问题
和一个高风险缺口，尚不能进入实现：

- shared A 使用一拍 response 后，如果沿用单 S1 的串行 request/response/retire，
  conflict-free A 的稳态最小 II 会变为 2，无法长期支持完整 K profile 的连续
  SA input fire；计划需要冻结可流水的 S1 context 或等价的 S0-to-next-request
  turnover 机制；
- B0/B1 联合覆盖 `[0,K)` 时还需要冻结连续 `chunkBase + localSlot` 映射、active
  选择、零气泡边界交换、跨 spatial tile 重置到 K0，以及完整驻留期间禁止覆盖。

第二轮 subagent 未修改文件、未执行 gem5 编译。

随后已修订上述两个问题：

- shared A 采用单 S1 turnover 流水：current S1 response 完整且可退休时，同拍将
  old S0 转成 incoming S1 并发出第一轮请求；conflict-free 且无下游反压时稳态
  II=1，不增加完整 A tile buffer；
- B0/B1 使用连续 `chunk_base_k + local_slot -> global_k` 映射，由
  `nextExpectedK` 选择 active；冻结周期末零气泡交换、跨 spatial tile 重置到 K0、
  完整驻留期间禁止覆盖和小容量 inactive-buffer refill 规则；
- 验证增加长 K A turnover 测试，避免有限 FIFO 积压造成假阳性，并逐 tile 检查
  B identity/global K 消费序列严格为 `0..K-1`。

本次仍只修改计划和状态记录，未修改 RTL/C++，未执行 gem5 编译或测试。

### 共享数据流 Step A：配置和地址布局

已完成 `PLAN.md` 第 13.9 节 Step A 的源码实现：

- streaming fixture 可选解析严格的 `shared_spad` 子对象；未提供时按
  `A -> B -> C -> D` 自动连续布局，并补全 B buffer depth、D pending depth、
  weight reuse 和 `A > D > B` 仲裁策略；
- common conv fixture resolver 和 canonical hash 路径未修改；streaming resolved
  config/hash 包含全部补全后的 shared-spad 字段；
- Python/C++ 同步校验四个区域的最小 footprint、4096-row 边界、互不重叠、
  A base 与 `im2col.spad_base` 一致，以及 buffer/depth/仲裁参数；
- 冻结 B、C、D 统一地址 helper：B 为 `bank=oc,row=B_base+k`，C 为
  `bank=oc,row=C_base+byte`（low byte first），D 为
  `bank=oc,row=D_base+N/H/W spatial index`；
- `StreamingConvPipelineModel` 启动时建立共享 scratchpad 镜像，预加载 A activation、
  B weight、C signed-int16 bias bytes，并将 D 保持为零；该镜像将在 Step B 起接入
  周期请求/仲裁，当前既有 producer timing 保持不变；
- SimObject 参数路径已传递全部 resolved shared-spad 字段，未修改 `SConscript`。

验证结果：`python3 -m unittest discover -s util/conv_pipeline -p '*_test.py'`
共 66 项通过，`git diff --check` 通过。开发者随后完成
`build/RISCV/gem5.opt` 增量构建；使用
`01_c1_w1_oc1_ones.json` 的 Step A smoke run 在 cycle 50 drained，独立 streaming
verifier 通过 9 vectors、1 tile、3 outputs 及 reference output 检查。SCons 本次
未生成 `src/sau_n` GTest 可执行文件，因此新增 C++ contract/preload 单元测试仍待
单独构建运行。

### 共享数据流 Step B：A 路径迁移

已完成 `PLAN.md` 第 13.9 节 Step B 的源码实现：

- `PipelinedIm2ColModel` 现在显式区分 `StandaloneCombinational` 和
  `SharedOneCycle`；旧构造函数、caller-provided scratchpad 和 `tick()` 保持原
  standalone 组合响应路径；standalone scratchpad 改为只在 standalone 构造时
  分配，shared 实例不再保留第二份 activation storage；
- shared 模式通过 `tickShared()` 接收上拍 response，并以 callback 形式把本拍
  proposed request 交给外部 arbiter，返回的 grant 被锁存为下一拍 in-flight read；
- response 先更新 working S1，再生成剩余 lane 请求；当前 S1 完整且 S2 可接收时，
  同拍由 old S0 建立 incoming S1 并发出其第一轮请求，实现单 S1 turnover；
- grant 必须是 proposed request 的同地址子集，response valid mask 必须严格匹配
  上拍 grant；被拒绝的 bank 会重试，没有 grant 的 bank 不允许产生 phantom
  response；
- `StreamingConvPipelineModel` 的 producer 已切换到 shared one-cycle 模式，
  A response 来自 Step A 预加载的 shared scratchpad；模型和外部 A in-flight
  状态增加一致性检查；
- observation 增加 response 对应 request、本拍 proposed request 和实际 grant，
  为后续 B/D 共享仲裁和可重算 trace 保留明确边界；
- C++ 定向测试覆盖 standalone/shared 输出逐项一致、`C=8/K=72` 长序列 turnover
  稳态 II=1、response 后不重复请求已完成 row、单 bank grant 拒绝重试，以及原
  streaming model 确认使用 shared memory mode。

非编译验证结果：66 项 `util/conv_pipeline` Python 回归、相关 `py_compile` 和
`git diff --check` 均通过。开发者随后完成包含 Step B 的
`build/RISCV/gem5.opt` 增量构建；三项 gem5 smoke run 和独立 verifier 全部通过：

- `01_c1_w1_oc1_ones`：cycle 50 drained，9 vectors、1 tile、3 outputs，并与
  reference output 一致；
- `w6_stride2_scattered`：cycle 72 drained，18 vectors、1 tile、18 outputs，
  bank conflict、scattered compaction 和 input bubble 检查通过；
- `08_n1_c16_h16_w32_oc16`：cycle 6243 drained，4608 vectors、32 tiles、
  8192 outputs，FIFO full exchange 检查通过。

长 K profile 的 `conflictFreeOutputII=1`、`conflictFreeOutputMaxGap=1`，trace 中
最长连续 `s2_fire` 为 148 拍，超过 S0/S1/S2/FIFO 的有限暂存容量，证明 shared A
turnover 的稳态 II=1 不是启动前积压造成。SCons 本次仍未生成 `src/sau_n` GTest
可执行文件，因此新增的 standalone/shared 对照和 grant-denial C++ GTest 尚待
单独构建运行。

### 共享数据流 Step C：B0/B1 权重预取

已完成 `PLAN.md` 第 13.9 节 Step C 的源码实现：

- `buildSauInputs()` 不再直接调用 `weightValue()`；该生成器只在启动预加载 B 区时
  使用，运行时 SA weight 完全来自 B0/B1 entry；
- 每个 B entry 保存 weight tile identity、global K、16-lane data 和 ready mask；
  每个 buffer 保存连续 `chunkBaseK/chunkLength`，未使用 slot 保持 invalid；
- B prefetch 从 Step A 的 B 区产生逐 bank read，经 shared arbiter 与 A 竞争；
  当前策略严格为 A 优先，B 只获得 A 未使用的 bank，被拒绝 lane 保留 pending 并
  重试；
- 每个获准 B bank 保存 buffer/slot/global-K in-flight tag；下一拍 response 先更新
  working B state，本拍 request 生成可看到 working ready mask，SA 只能在下一拍
  消费新 ready entry；
- 两个 buffer 联合覆盖 `[0,K)` 且启用 reuse 时，首次 launch 等待完整 K ready，
  spatial tile 之间只重置 K 游标且禁止覆盖；默认 depth=K 因此只需 B0；
- 小容量或关闭 reuse 时，B0/B1 交替保存连续 chunk；只有另一 chunk 完整 ready
  才在周期末交换，旧 active 变为 inactive 后才 refill 后续 chunk；下一个 spatial
  tile 从 K0 重新读取；
- consumer launch/input fire 同时受 A FIFO 和 B ready 门控；每次 fire 校验
  FIFO/global K 与 `nextExpectedK` 一致，chunk 未就绪时产生真实 SA input bubble；
- 新增 B bank request/grant/response、fill/consume/hit、empty、switch、prefetch
  stall 和 cross-spatial reuse 统计，并加入 gem5 stats 输出；
- C++ 定向测试覆盖完整驻留只读取一次 B、后续 tile reuse、小容量 depth=2
  refill/重新读取、逐 tile K 顺序、half-K B0/B1 零气泡交换，以及所有配置与原
  numeric oracle 输出一致。

开发者已完成包含 Step C 的 `build/RISCV/gem5.opt` 增量构建。三项 gem5 smoke
run 和独立 verifier 全部通过：

- `01_c1_w1_oc1_ones`：cycle 58 drained，9 vectors、1 tile、3 outputs，并与
  reference output 一致；
- `n2_w6_stride2_depth2_no_reuse`：cycle 144 drained，36 vectors、2 tiles、
  36 outputs；B buffer fill/consume/hit 均为 36，发生 16 次 buffer switch、
  23 个 B empty cycle 和 12 个 prefetch stall cycle，证明小容量 refill 与真实
  input bubble 生效；
- `08_n1_c16_h16_w32_oc16`：cycle 6387 drained，4608 vectors、32 tiles、
  8192 outputs；B 仅填充 144 个 K vector，随后产生 4464 次
  cross-spatial reuse hit，且 B empty cycle 为 0。

独立 verifier 现会普遍检查 B request/grant/response 守恒、response 与
`fill*out_channels` 一致、consume/hit 等于 expected vectors，并按配置判定完整
驻留或逐 tile refill 的预期 fill/reuse 数量。新增的小深度 fixture 已注册为 quick
gem5 regression，target profile 也明确要求 cross-spatial reuse。最终验证为：
67 项 `util/conv_pipeline` Python 回归通过，三组已有 Step C artifact 均通过增强
verifier，相关 `py_compile` 和 `git diff --check` 通过。SCons 本次仍未生成
`src/sau_n` GTest 可执行文件，因此 Step C 新增的 C++ 定向 GTest 仍待单独构建
运行。

### 共享数据流 Step D：C bias 和 D output

已完成 `PLAN.md` 第 13.9 节 Step D 的源码实现：

- pipeline 启动时先独占共享 scratchpad 完成 C low/high 两次 byte sweep；C 未
  ready 时暂停 A producer、B prefetch 和 SA launch，每个 bank 保存 byte
  destination tag，下一拍 response 拼接并显式符号扩展为 int16 bias register；
- `buildSauInputs()` 不再运行时调用 `biasValue()`，首次及后续 spatial tile launch
  均使用 C 区读取后驻留的 bias register；既有 SA 最终 MAC/drain 后独立 bias
  phase 未修改；
- SA 最终 int8 row 先进入容量受 `d_pending_rows` 限制的 D pending queue；entry
  保存 output coordinate、valid/pending mask 和各 output-channel byte；
- 正常运行的逐 bank 仲裁改为严格 `A read > D write > B read`；D writer 只处理
  cycle-start old head，新产生的 SA output 本拍不能直接写回；
- `dPushReady = !full || headWillRetire`，再与周期性 output-ready 相与形成
  `outputGrant`；因此 depth=1 时支持 old head 全部写完的同拍 dequeue/enqueue，
  partial write 且 queue full 时会反压 SA；
- 每次获准 D lane 写入统一 `dAddress()`，写入时检查 duplicate；drained 增加 C
  response、D queue 和全部 D write 完成条件，并检查 missing write；
- `outputs()` 不再由 SA output host-side 直接收集，而是在 drain 时从 D 区按 NCHW
  顺序回读重建；
- 新增 C read request/grant/response、D write request/grant、D pending peak 和
  D write stall stats；独立 verifier 检查两次 C byte sweep、D 写入守恒、全部
  output element 已写回以及 pending depth 边界；
- C++ 定向测试增加负 bias byte 拼接及 launch 配置、D scratchpad 回读，以及
  depth=1 queue old-head retire/new-row enqueue 同拍周转检查。

非编译验证结果：67 项 `util/conv_pipeline` Python 回归、相关 `py_compile` 和
`git diff --check` 通过；源码路径检查确认 `biasValue()` 只用于启动预加载 C，
`collectedOutputs` 只在 drain 的 D 区回读阶段写入。

开发者随后完成包含 Step D 的 `build/RISCV/gem5.opt` 增量构建，binary 显示编译
时间为 2026-07-30 23:55:49。四项 gem5 smoke run 和增强 verifier 全部通过：

- `01_c1_w1_oc1_ones`：cycle 61 drained，C request/grant/response 均为 2，
  D request/grant 均为 3，并与 frozen reference output 一致；
- `n2_w6_stride2_depth2_no_reuse`：cycle 147 drained，C 三项计数均为 6，
  D request/grant 均为 36；Step C 的 36 次 B refill、16 次 buffer switch 和
  23 个 B empty cycle 保持不变；
- `08_n1_c16_h16_w32_oc16`：cycle 6390 drained，C 三项计数均为 32，
  D request/grant 均为 8192；B 仍只填充 144 个 K vector 并产生 4464 次
  cross-spatial reuse；
- `07_c2_w5_oc3_outbp`：使用 `ready_period=11/ready_high=1` 在 cycle 342
  drained，periodic output backpressure、reference output 和 C/D 统计检查通过。

四项 profile 的 `dPendingPeak` 均为 1，D grant 均严格等于 expected outputs，
输出 CSV 由 D 区回读后仍与独立 convolution oracle 一致。无外部 backpressure
时最长连续 SA output accept 分别达到 3、6、16 拍；结合 depth 1 peak 和逐拍完整
D grant，验证了 old-head retire/new-row enqueue 的连续周转。当前 profile 未触发
A/D 同 bank 冲突，`dWriteStallCycles` 均为 0；partial-write 且 queue-full 的反压
分支已有 C++ 定向测试，但本次 SCons 仍未生成 `src/sau_n` GTest 可执行文件，
因此该分支尚待单独构建运行。

### 共享数据流 Step E：周期 trace 和统计守恒

已完成 `PLAN.md` 第 13.9 节 Step E 的源码实现：

- compact trace 从 schema v1 的 54 字段升级为 schema v2 的 87 字段；原 54 字段
  顺序保持不变，后追加 A/B/C request/grant/response mask、A request/response
  tile/K、B request/response buffer/slot/global-K、C byte identity、D queue
  cycle-start occupancy/head pending mask/request/grant/enqueue/dequeue，以及 B hit、
  reuse、active buffer、next K 和 ready-entry occupancy；
- trace writer 使用实际 in-flight tag 记录 B/C response destination；A turnover
  request 按当前 S0/S1 来源记录 identity，下一拍 response 使用 old S1 identity；
- 新增 A bank request/grant/response stats，使 A/B/C 三类 read 和 D write 均具备
  requester 级守恒计数；
- 新增 cycle-start B ready-entry average/peak occupancy，以及 16-bank
  `perBankReadCycles`、`perBankWriteCycles`、`perBankReadWriteConflicts` vector
  stats；read/write conflict 定义为同一 bank 同拍存在 D request 和任一 A/B/C
  read request；
- C++ 每拍 invariant 检查 A/B/C response<=grant<=request、所有 per-bank read
  之和等于 A+B+C grant、per-bank write 之和等于 D grant、occupancy sample 数与
  已执行周期一致；
- drained 在原有 producer/FIFO/SA/read-in-flight/D queue 条件上，再要求 B
  prefetch engine 已无未完成 entry/request；
- 独立 Python verifier 逐拍重放 `A > D > B` 仲裁、上一拍 grant 到本拍 response、
  A/B/C identity、D queue occupancy 递推、depth-full output backpressure 和 B
  hit/reuse；随后将 trace 重算的 requester 总数、per-bank vectors、B occupancy、
  B/D stall 与 gem5 stats 逐项比较；
- C++ trace 测试已更新 schema v2 字段数和 drained 定位；模型测试增加 A 和
  per-bank 读写守恒、B request/response identity 边界；Python 增加共享 bank
  arbiter 优先级定向测试。

非编译验证结果：68 项 `util/conv_pipeline` Python 回归、相关 `py_compile` 和
`git diff --check` 通过；脚本检查确认 C++/Python trace schema 均为 87 个唯一
字段，且第 54 个字段仍为原 schema 的 `drained`，新增字段从
`a_request_mask` 开始。

开发者随后完成包含 Step E 的 `build/RISCV/gem5.opt` 增量构建，binary 显示编译
时间为 2026-07-31 00:24:13。gem5 Vector stats 的实际 key 确认为
`perBankReadCycles::0..15` 等形式，与 verifier 兼容。四项 schema v2 gem5 run
和逐拍 verifier 全部通过：

- `01_c1_w1_oc1_ones`：cycle 61 drained；87-field/87-unique、schema=2，
  A request/grant/response 均为 7；per-bank read/write 总数为 18/3，B ready
  occupancy average/peak 为 7.5/9，并与 frozen reference output 一致；
- `n2_w6_stride2_depth2_no_reuse`：cycle 147 drained；A 三项计数均为 160，
  per-bank read/write 总数为 274/36，B occupancy average/peak 为
  2.689189/4；refill、switch、bubble 和 identity 重放通过；
- `08_n1_c16_h16_w32_oc16`：cycle 6390 drained；A 三项计数均为 69184，
  per-bank read/write 总数为 71520/8192，B occupancy average/peak 为
  142.210609/144；完整驻留、4464 次跨 tile reuse 和 full FIFO exchange
  重放通过；
- `07_c2_w5_oc3_outbp`：cycle 342 drained；A 三项计数均为 260，
  per-bank read/write 总数为 320/60，periodic output backpressure 和 reference
  output 检查通过。

上述 verifier 已逐拍确认每个 bank 的 grant owner 唯一、`A > D > B` 选择、
grant 到下一拍 response、A/B/C identity、D occupancy 递推、B hit/reuse，以及
trace 重算 stats 与 gem5 输出完全一致。四项 profile 的 read/write conflict 均为
0，符合 Step D 已记录的当前数据流调度；冲突非零 profile 留待 Step F 性能对照。
本次 SCons 仍未生成 `src/sau_n` GTest 可执行文件。

### 共享数据流 Step F：验证和性能对照（完成）

已完成同一 workload 下可直接比较的三档 B profile：

- `n2_w6_stride2_depth1_no_reuse`：depth 1、关闭 reuse，作为每 tile/每 K
  都重新读取 B 的基线；
- `n2_w6_stride2_depth2_no_reuse`：depth 2、关闭 reuse，作为 B0/B1 分块预取；
- `n2_w6_stride2_full_reuse`：默认 depth 18、开启 reuse，完整驻留 `[0,K)`。

三者均为 K=18、2 个 spatial tile、36 个输入向量和 36 个输出元素。Python
fixture 测试已冻结 workload 相等性；verifier 新增
`--expect-depth-one-baseline`，要求 depth 1、关闭 reuse、36 次 B vector fill、
0 次 reuse，并实际出现 buffer switch、B empty 和 prefetch stall。

D pending queue 的 old-state 决策已从模型内联逻辑抽成
`decideDPendingQueue()` 契约函数，模型直接复用该函数。C++ 定向测试和独立
Python replay 测试同时覆盖：

- depth 1 old head 全部获准写回时，同拍 retire/dequeue 后允许 SA
  output enqueue；
- depth 1 old head 仅部分 bank 获准时，不 retire、`pushReady=false`，必须对
  SA output 反压；
- 外部 output-ready 为 false 时，即使 old head 本拍 retire 也不能接收新输出。

使用 Step E 已构建的 2026-07-31 00:24:13 binary（新 D 契约重构前，但其运行时
逻辑等价）运行三档 profile，并用加强后的 87-field verifier 逐拍重放，全部通过；
三份 NCHW output SHA256 均为
`363833976ee07dbfb18e166a0712b7282679de71eb87059ab9e57b313a5a3798`：

- depth 1：cycle 170 drained / `totalCycles=171`，B request/grant/response
  为 128/108/108，fill=36，empty=46，switch=34，prefetch stall=15，
  PE input bubble=46；
- depth 2：cycle 147 drained / `totalCycles=148`，B request/grant/response
  为 123/108/108，fill=36，empty=23，switch=16，prefetch stall=12，
  PE input bubble=23；
- 完整驻留：cycle 155 drained / `totalCycles=156`，B request/grant/response
  为 59/54/54，fill=18，weight reuse=18，empty=0，switch=0，
  PE input bubble=12。

因此在该小 workload 上，depth 2 相对 depth 1 将 inclusive cycles 从 171
降至 148（约 13.5%），并将 B-empty/PE-bubble 从 46 降至 23。完整驻留将 B
response 减半并实现第二 tile 的 18 次 reuse，但等待完整 `[0,K)` 的首次 launch
使总周期为 156；该结果记录的是启动延迟和复用带宽的真实权衡，不宣称完整驻留在
所有小形状上都必然最快。

另运行 `w17_stride1_tail_oc7` 覆盖 spatial tail、OC=7 tail、K 末向量、负输出和
INT8 饱和：cycle 597 drained，216 vectors、8 tiles、476 outputs，verifier
通过；输出范围为 `[-128,127]`，其中 -128/127 分别出现 160/167 次，负数 214
次。Step E 的单 vector/单 tile profile 和 periodic output-backpressure profile
继续作为 Step F 功能矩阵的一部分。

非编译验证结果：28 项相关 Python 回归、四份新/复用 trace 的独立 verifier、
三档 output 逐字节一致检查和 `git diff --check` 通过。当前集成 profile 的
D/read-write conflict 仍为 0；这是现有 33-cycle SA drain 与 A FIFO/B prefetch
调度的可观测结果，不伪造非零统计。仲裁竞争和 D 部分写回分支由纯契约定向测试
覆盖。新增 C++ 契约重构尚需开发者按目录规则增量构建；本次未主动执行 SCons，
且 GTest executable 仍未生成。

开发者随后完成增量构建；新 binary 显示编译时间为
2026-07-31 00:46:16。使用该 binary 正式重跑 depth 1、depth 2、完整驻留和
tail 四项 profile，drained cycle 仍分别为 170、147、155、597，四项
87-field trace 均通过独立 verifier。三档性能 profile 的输出 SHA256 仍全部为
`363833976ee07dbfb18e166a0712b7282679de71eb87059ab9e57b313a5a3798`，
上述 B 请求、fill、bubble、reuse 和总周期统计均未变化；tail 输出仍有 160 个
`-128`、167 个 `127` 和 214 个负数。由此确认 D queue 契约抽取没有改变模型
功能或周期行为。

rebuild 后再次运行 70 项 `util/conv_pipeline` Python 回归和
`git diff --check`，全部通过。

开发者进一步显式构建 `src/sau_n` 的四个相关 GTest executable，随后全部执行
成功：

- `streaming_pipeline_contract.test.opt`：16/16 通过，包括 depth 1 D old-head
  完整 retire 后同拍接收新输出，以及部分 bank 写回时反压 SA；
- `pipelined_im2col_model.test.opt`：10/10 通过，包括 shared one-cycle response、
  长 K S1 turnover II=1、grant denial retry 和无 phantom response；
- `streaming_conv_pipeline_model.test.opt`：8/8 通过，包括 ABCD preload/read/write、
  D 重建输出、B chunk refill/global K 顺序和 half-K 零气泡交换；
- `streaming_conv_pipeline_io.test.opt`：4/4 通过，包括 87-field compact trace、
  detailed PE trace、drained cycle 输出和 resolved hash 校验。

为精确覆盖计划中的 16-bank A/B conflict baseline，新增
`n2_w16_stride1_depth1_ab_conflict`。该 profile 为 depth 1、关闭 reuse、
OC=16，运行至 cycle 589 drained；trace 中出现 26 次
`A_request=B_request=0xffff`。每次第一 bank access slot 均为 A 获得全部 16
bank、B grant 为 0，下一 slot A request 为 0、B 获得全部 16 bank，确认一个
A/B pair 需要两个 access slot。独立 verifier 已冻结并检查该两槽序列。

最终回归结果：

- 71 项 `util/conv_pipeline` Python 测试全部通过；
- 四个 C++ GTest executable 共 38/38 项通过；
- `cd tests && ./main.py run --skip-build gem5/streaming_conv_pipeline`
  共 15 个 suite、45/45 项检查通过，覆盖 gem5 运行、drained exit 和独立
  trace/output/stats verifier；
- `py_compile` 和 `git diff --check` 通过。

对照 `PLAN.md` 13.12，A/B/C/D shared scratchpad、B0/B1、launch/turnover、
完整驻留 reuse、小容量 refill、C/D 数值、D finite queue/backpressure、drained、
16-bank 冲突、统计重放和现有回归均已验收。因此双缓冲共享数据流阶段可以正式
声明完成。仍不冻结 B buffer depth 和 D pending depth 的具体性能最优值；当前
模型仍是 architectural timing model，不代表真实 SRAM macro 物理端口、延迟或
功耗。集成 workload 中 D/read-write conflict 统计为 0，D 部分写回竞争由
`A > D > B` 仲裁 replay 和已执行的 depth 1 D 契约 GTest 定向覆盖。

### Step F 独立代码审查修订

独立 subagent 对 Step A-F 做只读审查，未发现会导致现有 profile 数值错误的
C++ 状态机问题，但指出 B0/B1 verifier、实际 C++ D 仲裁覆盖和 periodic
output-ready 重放仍可加强。经逐项复核后完成以下修订：

- 保持 87-field schema v2 不变。现有 trace 的 B request/response
  buffer/slot/global-K identity，加上确定性的 K/depth/reuse 配置和 input-fire
  事件，足以无歧义重建 chunk configure/invalidate epoch，无需为此扩 schema；
- verifier 新增独立 B0/B1 replay state，按真实 old-state 顺序执行
  cycle-start occupancy、refresh/switch、response fill、request selection、
  input consume 和 chunk reset；逐拍校验 active buffer、next K、ready entries、
  request mask/identity、launch/input readiness 和 reuse hit；
- `bBufferFillVectors`、`bBufferConsumedVectors`、`bBufferHitVectors`、
  `bBufferEmptyCycles`、`bBufferSwitches`、`weightReuseHits` 现在均由独立
  B replay 重算后与 gem5 stats 对比，不再只使用最终配置公式；
- 将实际模型原内联的 per-bank 仲裁抽成 `arbitrateSharedSpad()` C++ 契约，
  模型的 C 初始化、producer-active 和 producer-drained 三条路径均直接调用；
  GTest 覆盖 A/D/B 重叠 mask 的 `A > D > B` 选择、D bank 不进入 read grant
  和 C 初始化独占；
- testlib 将 `output_ready_period/high_cycles` 同时传给模型和 verifier；
  verifier 逐拍计算 periodic-ready，并严格检查
  `output_grant = periodic_ready && d_push_ready`，不再只要求观察到一次 stall。

开发者完成本轮增量构建；新 `gem5.opt` 显示编译时间为
2026-07-31 10:24:06。rebuild 后验证结果：

- 四个 C++ GTest executable 共 40/40 通过，其中 shared arbitration 新增
  2 项测试；
- 73 项 `util/conv_pipeline` Python 回归通过，包括 B chunk 覆盖后重配和完整
  K 跨 tile 驻留的独立 replay 测试；
- 新 binary 下完整 streaming quick suite 仍为 15 个 suite、45/45 项检查通过；
  output-backpressure profile 的 period/high=`11/1` 已按周期逐拍验证；
- `py_compile` 和 `git diff --check` 通过。

审查提出的 resolved SHA256 残余风险保留：该值由 Python canonical config
计算后传给 C++ 并原样写入 trace，不是 C++ 独立重复实现 JSON canonicalization。
本轮未引入第二份跨语言 hash 算法，因为这会扩大配置协议和重复序列化逻辑；当前
通过 SimObject 参数校验、地址/计数、B 状态重放和数值 oracle 对实际生效配置做
行为级交叉验证。该 hash 应视为 artifact/config identity，不应单独视为 C++
参数映射正确性的证明。

同一 subagent 随后完成闭环复审，确认原三项 finding 中 C++ 仲裁和 periodic-ready
已经关闭，B0/B1 replay 主体也已关闭。复审另发现 verifier 的 B empty 统计在保存
old-state readiness 后，错误地再次用 response 后状态查询，以及
`peInputBubbleCycles` 仍只检查非零。现已修正：

- B empty replay 直接使用 response 前保存的 `replay_input_ready`，严格匹配 C++
  本拍 consumer readiness 使用 old `bBuffers` 的语义；
- verifier 精确累计每拍 `AcceptK && acceptedK<K && !inputFire`，并将结果与
  `peInputBubbleCycles` stats 等值比较；
- 新增 half-K 双 buffer 完整驻留 replay-only 测试，覆盖 K 跨 B0/B1 分割、tile
  内切换、下一 tile 重置到 B0 以及第二 tile reuse。

最终 Python 回归更新为 74/74 通过；新 binary 的完整 streaming quick suite
再次运行并保持 15 suites、45/45 项检查通过，证明现有 bubble stats 与 trace
精确一致。`py_compile` 和 `git diff --check` 继续通过。

## 当前阶段

Im2Col 独立模型的原 Step 0 至 Step 8 验收保持有效。下游融合对象已从历史计划中的
Mikui `SA_ENGINE` 修订为项目自有 `sau_array_16x16.sv`。融合 RTL 已在 VCS
T-2022.06_Full64 完成六项独立阵列和七项端到端功能验收；本地已导入 RTL、工具、
来源记录和 VCS 结果摘要，并依据实际 trace 更新纯 C++ SA 周期模型。用户已成功
构建新 gem5 binary；七项 gem5/RTL strict trace comparison 已全部通过，融合迁移
和逐拍验收完成。

```text
PLAN revised
Step 0 baseline RTL compile/run passed
Step 1 contract implementation complete
Step 2 fixture loader and logical oracle complete
Step 3 address and scratchpad implementation complete
Step 4 tick model implementation complete
Step 5 gem5 SimObject integration complete
Step 6 trace, statistics, and comparator implementation complete
Step 7 RTL observer and regression implementation complete
Step 7 RTL golden matrix passed
RTL per-cycle validation passed
Step 8 documentation and final acceptance complete
Planned delivery complete
SAU downstream requirements frozen
SAU_PLAN revised after RTL risk review
SAU Pre-Step 0 src/sau baseline captured
SAU Step 0 local baseline package complete
SAU historical Mikui Step 0 retained as evidence, no longer active golden
SAU Step 1-7 development unblocked
SAU Step 1 pipeline contract/fixture/generators complete
SAU Step 2 independent oracle/tile mapping complete
SAU Step 3 C++ numeric core complete
SAU Step 4 fused-array cycle model calibrated from VCS traces
SAU Step 5 single-tile end-to-end C++ pipeline complete
SAU Step 6 gem5 SimObject, stats, output, and run entry complete
SAU Step 7 canonical trace and synthetic comparator validation complete
SAU fused RTL functional validation passed
SAU gem5 model migrated and build passed
SAU seven-profile gem5/RTL strict comparison passed
SAU fused pipeline migration and validation complete
SAU shared-dataflow Step F final acceptance passed
SAU shared-scratchpad double-buffer stage complete
```

对于原独立 Im2Col 模型，可以正式声明 `RTL per-cycle validation passed`；其
`PLAN.md` 定义的 Step 0 至 Step 8 已完成。该结论不自动覆盖新的融合 pipeline。

对新的融合 Im2Col + 16x16 SA pipeline，当前也可以声明：

```text
Im2Col-to-project-owned-16x16-SA gem5/RTL per-cycle validation passed.
```

### 2026-07-20 新 gem5 分支迁移

- 目标仓库：`sau_origin_feature_sau_command_types_b9fbc18_20260720`；
- 目标分支：`feature/sau-command-types`，迁移基线 commit `b9fbc18c20f3`；
- pipeline filelist 已改为 Im2Col DUT、项目自有 16x16 阵列、融合 wrapper 和
  testbench，不再编译 Mikui `SA_ENGINE/SA_ROW`；
- VCS 观测到的阵列状态冻结为 `IDLE/STREAM/DRAIN/BIAS/OUTPUT = 0/1/2/3/4`；
- 输入到 PE MAC commit 延迟为 `2 + row + column` 拍；末输入到全阵列 bias/首输出
  为 33 拍；输出按 `valid && grant` 同拍交付，末行握手后一拍 `cal_finish`；
- `pe_finish` 在第 0 输出行有效期间保持，`row_ready_mask` 对 `os_valid_mask` 晚一拍；
- 七份经 provenance 校验的融合 RTL trace/output/manifest 已导入
  `tests/gem5/conv_pipeline/ref/`，并新增七项 gem5 strict quick test；
- Python 42 项回归、Step 8 来源/结果校验、golden hash 核对和 `git diff --check`
  已通过；
- 用户报告 `build/RISCV/gem5.opt` 构建成功；
- 官方 gem5 testlib 的七项 suite 全部通过：7 个 gem5 运行、7 个退出原因检查和
  7 个融合 RTL trace/output verifier，共 21 项；
- 七份 53 字段 trace 合计严格比较 7,772 个 cycle，无字段差异；每项 NCHW output
  均与融合 RTL golden 逐字节一致；
- profile 的 drained cycle 分别为 85、276、574、1754、1551、3061 和 464，均与
  VCS manifest 一致；
- 已生成最终源码覆盖包
  `/home/xch/work/sau_n_gem5/sau_origin_feature_sau_command_types_fused_migration_20260720_r1.tar.gz`，
  包内 161 个条目，不包含 `.git`、`build/`、`testing-results`、`__pycache__` 或
  `.pyc`；最终压缩包 SHA256 随交付信息单独提供。

## SAU 下游新阶段（历史规划记录）

新增并修订 `src/sau_n/SAU_PLAN.md`，目标是在现有 Im2Col 后建立 Mikui 16x16 SAU
INT8 3x3标准卷积周期模型。当前已与用户确认：

- 以 Mikui `mikui_16x16` 分支 commit `2ca8252` 的 `SA_ENGINE`/PE层次为
  reference RTL核心；保留逐字节一致的original副本，集成黄金对象使用带有显式
  `FINISH_ROW/FINISH_COL`位宽补丁的patched variant；
- 第一版支持 `C=1..63`、`out_channels=1..16`，output channel不要求是16的倍数；
- 使用单 tile buffer把有气泡的 Im2Col feed转换为连续 `K=C*9` 拍SA输入；
- 使用确定性 signed INT8 weight和signed INT16 bias，不读取外部tensor；
- 固定100 MHz、24-bit逐次饱和累加、可配置cutbit和INT8输出饱和；
- 新增统一 `ConvPipelineTiming` SimObject，现有 `Im2ColTiming` 保持兼容；
- 最终输出为 NCHW，支持周期性output backpressure；
- 逐拍比较controller、接口和256个PE的valid activation/weight/accumulator；
- 允许复制最小Mikui RTL并新增integration wrapper，RTL在外部VCS工作站运行；
- 不修改并行开发的 `src/sau/`，已经保存初始status/diff/全文件SHA256基线；
- 当前完成风险修订、Pre-Step 0证据保存、Step 0本地baseline/工作站包和Step 1
  pipeline contract/fixture/generator实现。由于暂时拿不到VCS工作站结果，VCS不再
  阻塞Step 1至Step 7，尚未实测的周期锚点保持provisional。

静态审核发现当前 Mikui源码存在CONV模式编码的新旧注释矛盾：当前
`SA_pkg.sv` 和当前指令生成器使用 `2'b01`，旧注释和旧benchmark出现`2'b10`。
建模阶段按当前可执行RTL和指令生成器冻结`2'b01`，不得凭旧注释改用`2'b10`；后续
VCS baseline用于确认该契约，若结果不同再修订受影响的模型和测试。

### SAU_PLAN 风险审查修订（2026-07-17）

- 静态确认 `SA_ENGINE.sv` 中 `$clog2(16)` sized cast会把常量16截断为0，原始
  `FINISH_ROW/FINISH_COL`无法可靠控制合法的1..16尾行尾列；
- 计划现保留`rtl/mikui/original/`原始副本，并只允许在独立integration variant中
  通过同宽localparam修复两条FINISH赋值；original、patch、patched filelist均要求
  provenance和SHA256；
- 冻结候选控制协议：`ins_valid_i`单拍、`EN_i`连续K拍、配置保持到`cal_finish`，
  `Flag_o`作为输出序列请求，`Flag_o_ready`作为engine grant，并区分内部`valid_o`
  接受事件与下一拍注册输出；所有时序仍须Step 0 VCS baseline逐拍确认，但该确认
  不再作为Step 1至Step 7的开发前置条件；
- 明确物理总线映射：activation row0位于最高byte，weight/bias/output column0位于
  最低element；wrapper在SA输入前只反转activation的16个byte；
- 新增K567正、负standalone profile，分别强制第512和第517次MAC首次发生24-bit
  饱和，逐次比较accumulator；
- 明确100 MHz（10 ns）是与Im2Col统一的集成模型选定频率，RTL的200 MHz注释不构成
  本计划的物理时序或性能承诺；
- 新增Pre-Step 0，要求最终以路径、mode、size、SHA256和git状态逐项证明未修改
  `src/sau/`。

已在只读访问`src/sau/`的前提下保存初始证据：

```text
m5out/conv_pipeline/provenance/src_sau_baseline/
status entries: 38
tracked + untracked files: 49
SHA256(evidence_sha256.txt):
ab5b451211150f9237421222ffbf2fdf43b6ef822c7c365be71499de9e10b4ff
```

证据包含`status.txt`、working-tree/staged binary diff、file list、带mode/size/type的
全文件SHA256 manifest及其校验清单。该目录是本地只读验收证据，不纳入源码交付。

### SAU Step 0 本地基线和工作站包（2026-07-18）

- 从本地`npu_lpnpu` Git object读取完整commit
  `2ca8252ef1cac43ef843998e9e08023259ac17ee`，冻结
  `hardware/src/sa_execute/`下8个最小RTL文件；
- `src/sau_n/rtl/mikui/original/`的8个文件与upstream object逐字节一致，完整原路径、
  commit/tree和SHA256记录在`provenance.json`；
- `integration/SA_ENGINE.sv`只应用
  `0001-fix-finish-dimension-width.patch`，source verifier可从original重建并逐字节确认
  没有额外改动；
- 新增original/integration两个明确filelist和共用Step 0 testbench，覆盖：
  - `row_num_i/col_num_i=1/15/16`原始截断行为及patched tail行为；
  - K9非对称activation/weight/bias物理lane映射；
  - `ins_valid_i`、连续K拍`EN_i`、`Flag_o/Flag_o_ready`和输出反压；
  - K567正/负24-bit逐次饱和及256个PE的MAC/add commit和accumulator观察；
- 新增source/trace verifier、2项Python测试、VCS批量脚本和结果manifest生成器；
- 本地执行source hash/唯一补丁重建检查、Python AST、2项unit test、bash语法和完整
  31条确定性命令dry-run，全部通过；
- 本地未发现VCS、iverilog、Verilator、slang、Surelog、verible、svlint或yosys，
  因此RTL compile/elaboration/run仍待工作站执行；
- 最终工作站包：
  `/home/xch/workspace/sau_n_step0_workstation_20260718_r2.tar.gz`；
- 工作站包SHA256：
  `3f63304f0be075d6c800860d2980e72a57716571819b4fbb4e46de51570ae2df`；
- archive builder已在内存中逐文件回读验证归档内容，包内另含`SHA256SUMS`和
  `PACKAGE_METADATA.json`。VCS结果返回并通过全部14项trace验证前，不得声明
  `SAU Step 0 VCS validation passed`或最终`RTL per-cycle validation passed`；但允许
  根据冻结RTL源码继续Step 1至Step 7，首输入、MAC commit、bias、首输出、反压和
  `cal_finish`等未实测周期锚点统一标记为provisional。

### SAU VCS与开发流程解耦（2026-07-18）

- 当前暂时无法取得VCS工作站结果，因此不把外部仿真作为开始gem5建模的阻塞条件；
- Step 1至Step 7直接依据冻结RTL源码的组合逻辑、寄存器、nonblocking assignment和
  流水结构实现，并用directed contract test固定源码推导出的候选周期契约；
- VCS结果返回后先回到Step 0校准provisional周期锚点；若存在差异，只修订受影响的
  契约、模型和测试并重新回归，不用golden trace驱动模型或迁就实现；
- Step 8的golden导入、严格逐拍比较以及最终通过声明仍必须等待VCS结果。

### SAU Step 1 pipeline contract、fixture和生成器（2026-07-18）

- 新增Python `ResolvedPipelineConfig`/`DerivedPipelineConfig`，复用现有Im2Col
  derivation并以uint64 checked arithmetic计算`K`、tile、output和useful MAC数量；
- 新增严格nested pipeline fixture loader：外层和嵌套Im2Col对象均拒绝未知、重复、
  缺失和错误类型字段；runtime output-ready参数禁止写入fixture；
- canonical resolved JSON只由Python生成，包含完整resolved Im2Col子对象，并输出
  SHA256；C++接收同一resolved字段，不重新实现第二套JSON canonicalization；
- Python/C++均实现signed INT8 activation转换、`tb_weight_value_v1|zero|ones`和
  `tb_bias_value_v1|zero`确定性生成器，不使用随机数；
- Python/C++均冻结pipeline状态编码`0..6`、周期性output-ready公式、drained条件和
  `pe_index=row*16+col` packed顺序；周期锚点显式保持provisional；
- 新增最小fixture `step1_n1_c2_h4_w5_oc3.json`，两端共同派生锚点为
  `K=18`、`expected_tiles=2`、`expected_outputs=60`、`expected_macs=1080`；其
  canonical resolved SHA256为
  `849daa80c0dcca855a518224097dbb7e952a311742e467c2c7e86918ddce1ed8`；
- `SConscript`仅登记新增的纯C++ source和2个GTest目标；未执行gem5/scons编译；
- 验证结果：17项Step 1 Python测试、28项既有Im2Col contract/fixture回归和7项
  独立编译的C++ GTest全部通过，新增C++源码通过`-Wall -Wextra -Werror`；
- 用户随后报告已完成一次gem5编译且没有报错；本状态未取得具体命令和构建日志，
  因此只记录为用户确认的构建结果；
- 按Pre-Step 0 manifest逐文件复核`src/sau/`路径、mode、size、SHA256和git status，
  结果完全一致。

### SAU Step 2 独立convolution oracle和tile mapping（2026-07-18）

- 新增独立spatial tile mapping，分别实现`W<=16`多行packing和`W>16`连续16列
  splitting，将每个逻辑SA row映射到唯一`(n, oh, ow)`坐标；
- 每个tile强制有效spatial mask为从lane 0开始的canonical prefix，并在pipeline
  fixture启动前拒绝scattered mask；全部坐标同时检查无重复、无缺失；
- 新增`output_coordinate`和checked NCHW flat index，固定collector输出布局为
  `output[n][oc][oh][ow]`；
- 新增直接遍历NCHW坐标的Python convolution oracle，不调用Im2Col bank/address、
  feed、tile mapping、SA周期模型或RTL trace；
- oracle每次MAC后执行signed 24-bit饱和，最后单独加入signed INT16 bias，再做算术
  右移和signed INT8饱和，同时输出最终accumulator和16-bit RTL slot；
- 定向确认正饱和首次发生在第512次MAC、负饱和首次发生在第517次MAC，并确认饱和
  不是sticky状态，后续反向加数可离开边界；
- 冻结7个端到端golden matrix fixture和独立matrix manifest；output-ready仅存在于
  matrix运行参数，不进入workload fixture；
- 测试覆盖`W=1/5/16/17/20`、padding、stride、dilation、batch、spatial/column tail、
  bias、cutbit、INT8饱和、output/useful MAC守恒以及全部7个profile的完整NCHW输出生成；
- 30项`util/conv_pipeline`测试和33项既有Im2Col contract/fixture/logical-oracle回归
  全部通过，Step 0 frozen source verifier继续通过；未执行gem5/scons编译；
- 按Pre-Step 0 manifest再次逐文件复核`src/sau/`路径、mode、size、SHA256和git
  status，结果完全一致。

### SAU Step 3 纯C++数值核心（2026-07-19）

- 新增`sau_model.hh/.cc`，集中实现signed INT8乘法、signed 24-bit逐次饱和加法、
  无rounding算术右移、signed INT8饱和和16-bit符号扩展RTL slot；
- 新增16x16 `SauNumericCore`和256个`SauPeNumericState`，每个PE保存最后一次有效
  activation、weight、product、24-bit accumulator、valid和bias状态；
- `begin -> macStep* -> addBias -> outputSnapshot`固定纯数值生命周期，bias只能在至少
  一次MAC后加入一次；`reset/clear`清除全部PE和操作元数据；
- row/column tail只更新有效PE，无效PE保持canonical zero，packed下标继续使用
  `row*16+column`；
- 当前`product`是供Step 4寄存器流水调度使用的数值stage；本步骤不引入skew、
  multiplier cycle delay、valid传播或output backpressure；
- 2x3独立手算向量验证正负乘法、两次MAC、不同列bias和16-bit slot；边界测试验证
  每次24-bit加法饱和且饱和不是sticky状态；
- C++ K567正向profile在第512次MAC首次钳位`0x7fffff`，负向profile在第517次MAC
  首次钳位`0x800000`，后续同号MAC及bias继续保持边界；
- Step 3的8项GTest及Step 1至Step 3合并15项纯C++ GTest全部通过，使用C++17和
  `-Wall -Wextra -Werror`独立编译；30项pipeline Python测试、33项既有Im2Col回归和
  Step 0 frozen source verifier继续通过；8项Step 3测试的ASan/UBSan复跑通过，受管
  环境因ptrace限制不支持LeakSanitizer，关闭leak detection后其余sanitizer无报告；
  未执行gem5/scons编译；
- 按Pre-Step 0 manifest再次逐文件复核`src/sau/`路径、mode、size、SHA256和git
  status，结果完全一致。

### SAU Step 4 SA周期状态和256 PE阵列（2026-07-19）

- 在`sau_model.hh/.cc`新增`SauCycleModel`，使用每拍observe/compute/commit语义实现
  固定CONV、CNORMAL、INT8候选周期协议，并拒绝launch/input同拍和K拍输入气泡；
- 冻结RTL `IDLE/START/WORK/STORAGE/DONE`状态编码；`DONE`按原RTL保留但当前固定协议
  不进入该状态；
- 根据`active_delay`、`weight_delay`、PE输入寄存器、`DW02_mult`、
  `data_multi_tmp_reg`和`mac_en`链集中定义三个VCS待校准锚点：
  `MAC=4+r+c`、`bias=5+r+c`、`row_result=6+r+(valid_cols-1)`；
- 以上常量均以`Provisional*`命名，模型同时公开`cycleAnchorsProvisional=true`；当前只
  声明源码推导完成，不声明RTL逐拍通过；
- 每拍公开256 PE的activation、weight和24-bit accumulator，以及row-major
  `peValid/macCommit/addCommit` 256-bit mask；`PE[0][0]`固定在bit 0；
- 实现row/column skew、逐拍MAC/bias事件、row result、`PE_valid/OS_valid`候选脉冲、
  `storageReady`、内部output fire、注册输出、`rowSequence`和`calFinish`；
- output grant为0时保持输出序列位置；一次row fire后的注册输出不再受下一拍grant
  二次握手影响，符合第8.4节候选协议；
- 静态复核后修正顶层控制的nonblocking-assignment语义：`ins_valid_i`只锁存配置，
  `IDLE -> START`由首拍`EN_i`驱动；新注册的`OS_valid[0]`和`cal_finish`只在下一拍
  影响状态，`cal_finish`有效周期保持`STORAGE`，随后一拍才进入`IDLE`并清除
  `storageReady`；
- row/column tail之外的PE和output slot保持canonical zero；full 16x16测试确认256个PE
  均出现MAC/add commit、16行输出完整且packed位序正确；
- K9定向候选锚点为：launch/config cycle 0保持`IDLE`、输入cycle 1..9、首MAC
  cycle 5、末MAC cycle 13、bias cycle 14、row result/PE finish cycle 15保持`WORK`、
  storage ready cycle 16进入`STORAGE`、注册输出及`calFinish` cycle 17保持
  `STORAGE`、cycle 18进入`IDLE`；这些cycle编号必须由VCS返回结果校准；
- K567周期模型继续在第512/517次MAC首次正/负饱和；2x3 tail测试覆盖输出反压、连续
  两行fire和下一拍注册输出；
- Step 4的7项GTest及Step 1至Step 4合并22项纯C++ GTest全部通过；30项pipeline
  Python测试、33项既有Im2Col回归、Step 0 source verifier和Step 4 ASan/UBSan复跑
  全部通过；未执行gem5/scons编译。
- 上述控制时序修正后，重新以C++17和`-Wall -Wextra -Werror`独立编译并运行22项
  Step 1至Step 4 GTest，全部通过；30项pipeline Python测试、Step 0 source verifier
  以及关闭leak detection的ASan/UBSan复跑均通过。按本目录编译约束未执行gem5/scons
  编译。

### SAU Step 5 单tile buffer和端到端pipeline（2026-07-19）

- 新增`sau_tile_buffer.hh/.cc`，实现容量固定为`K=C*9`的单tile buffer；每个tile
  强制按canonical k顺序收集，全部K项必须使用同一个nonempty canonical-prefix
  spatial mask，满buffer禁止继续接收；
- C++侧独立实现W<=16多行packing和W>16宽度splitting的tile metadata，将每个有效
  SA row映射到唯一`(n,oh,ow)`，启动前检查全部tile的空间位置总数；
- 新增`conv_pipeline_model.hh/.cc`，按
  `COLLECT_TILE -> LAUNCH_SA -> STREAM_K -> WAIT_RESULT -> DRAIN_OUTPUT -> DONE`
  old-state/next-state流程连接`Im2ColModel`和`SauCycleModel`；
- 仅在`COLLECT_TILE`且buffer有空间时拉高Im2Col `feed_ready`；第K项收满后下一拍
  launch，不允许空buffer旁路或在SA执行/输出期间收集下一tile；
- `STREAM_K`连续驱动恰好K拍activation/weight，activation按signed INT8解释，weight
  和bias由冻结generator生成；中间没有输入气泡；
- output collector使用tile metadata和`rowSequence`写入NCHW，检查slot是canonical
  signed INT8符号扩展，并拒绝重复、越界、缺失row和提前tile完成；
- drained同时要求Im2Col已drained、tile buffer为空、SA回到`IDLE`、tile/output计数
  完整且无待输出；记录`im2colDoneCycle`、`sauLastResultCycle`和`drainedCycle`；
- 新增7项Step 5 GTest，覆盖手算C1/W1/OC1、W5 packing、W16完整阵列、W17
  splitting+dilation、N2/W20 stride、K567、row/column tail和output ready=1/11；
- C2/W5/OC3的60项NCHW输出同时匹配独立C++直接卷积oracle和现有Python oracle固定
  结果；默认及反压运行均无丢失、重复，反压增加完成周期且产生output stall；
- `SConscript`登记两个新增source和两个GTest目标；未执行gem5/scons编译；
- Step 1至Step 5合并29项C++ GTest在C++17、`-Wall -Wextra -Werror`下全部通过，
  ASan/UBSan复跑全部通过；30项pipeline Python测试、43项既有Im2Col Python回归和
  Step 0 source verifier继续通过。
- 按Pre-Step 0证据复核`src/sau/`的git status、完整路径集合以及逐文件mode、size、
  type和SHA256，均与初始baseline一致。

### SAU Step 6 gem5 SimObject、stats和运行入口（2026-07-19）

- 新增`ConvPipelineTiming` ClockedObject和对应SimObject参数，统一包装Step 5纯C++
  pipeline model；每个clock edge推进一拍，drained后写出结果、更新统计，并以固定
  `conv pipeline drained`标准gem5 exit cause退出；
- 新增`configs/example/conv_pipeline_timing.py`，从严格pipeline fixture映射全部
  resolved参数，固定100 MHz，默认写入`<outdir>/conv_pipeline/trace.csv`和
  `output.csv`，同时支持命令行覆盖路径及默认/周期性output ready；
- 新增NCHW signed INT8十进制output writer，固定字段为`n,oc,oh,ow,value`，并在
  写出前检查元素数量与resolved config完全一致；
- 新增Step 6高层27字段逐拍trace，覆盖pipeline/Im2Col/SA状态、tile buffer、feed
  handshake/data/mask、output grant/result和drained，并携带严格校验的resolved config
  SHA256；完整256 PE packed canonical schema、严格loader和比较器按计划留到Step 7；
- 新增计划第12节全部pipeline/SA stats，并在纯C++ model内记录tile buffer占用、
  Im2Col/output反压、useful MAC、array active cycle及INT8正负饱和；drained继续强制
  tile、handshake、engine input、output和useful MAC守恒；
- `SConscript`注册`ConvPipelineTiming`、两个新增source及IO GTest，原有
  `Im2ColTiming`文件和接口未修改；
- 32项Step 1至Step 6 C++ GTest以C++17、`-Wall -Wextra -Werror`通过，ASan/UBSan
  （关闭LeakSanitizer）复跑通过；33项pipeline Python测试、43项Im2Col Python回归、
  Step 0 frozen source verifier、Python/SConscript语法检查和`git diff --check`均通过；
- 按本目录约束未执行gem5/scons编译，因此最小fixture的真实SimObject构建、标准exit
  event、stats.txt名称和值仍待用户手动构建运行确认；
- 按Pre-Step 0证据复核`src/sau/`的git status、完整路径集合以及逐文件mode、size、
  type和SHA256，均与初始baseline完全一致。

### SAU Step 7 canonical trace和比较器（2026-07-19）

- 将Step 6高层trace扩展为冻结的53字段canonical schema，覆盖pipeline状态和tile
  metadata、Im2Col FIFO/feed、SA launch config/bias、连续input及row/column mask、
  controller/output协议和三个完成标志；
- 每拍输出256-bit `pe_valid/mac_commit/add_commit` mask、256x8-bit activation和
  weight以及256x24-bit accumulator；`PE[row=0][col=0]`固定占最低有效element，全部
  hex均使用`0x`前缀、固定宽度和小写字符；
- activation/weight只在对应PE valid时保留，accumulator只在PE valid或add commit时
  保留；无效Im2Col feed、SA config/input、PE payload和registered output统一归零，
  因而原始RTL无效X/Z不会进入canonical trace；
- C++ writer从cycle 0写到唯一drained cycle并在drained主动flush；新增row-ready mask、
  SA输入快照和last-result标志，纯C++完整pipeline trace smoke共58 cycles，可被Python
  严格loader直接接受；
- 新增`util/conv_pipeline/compare_pipeline_traces.py`，严格检查UTF-8、LF、最终LF、
  53字段header、连续cycle、固定SHA256、state/range、hex宽度、valid/ready关系、
  launch config与input prefix mask、invalid normalization及唯一末拍drained；
- comparator报告首个`cycle/field`差异；六个PE packed字段进一步按逻辑row-major顺序
  解码并报告首个`PE[row][column]`及该element的expected/actual值；合法trace支持
  load/write逐字节round-trip；
- 新增7项合成trace测试，覆盖C++/Python schema一致性、round-trip、普通字段和长度
  差异、三类PE mask及三类PE payload定位、header/cycle/hash/hex错误、无效payload、
  控制关系、非末拍drained、CRLF和缺失最终LF；
- Step 1至Step 7共33项C++ GTest以C++17、`-Wall -Wextra -Werror`通过，ASan/UBSan
  （关闭LeakSanitizer）复跑通过；40项pipeline Python测试、43项Im2Col Python回归、
  Step 0 source verifier、Python语法、行长和`git diff --check`均通过；
- 按目录约束未执行gem5/scons编译；当前没有RTL golden，因此只能声明Step 7 trace
  基础设施和合成差异测试通过，不能声明SAU gem5/RTL逐拍一致或RTL周期锚点已校准；
- 按Pre-Step 0证据复核`src/sau/`的git status、完整路径集合以及逐文件mode、size、
  type和SHA256，均与初始baseline完全一致。

### SAU Step 8 RTL集成和工作站包（2026-07-19）

- 新增`im2col_mikui_sau_pipeline.sv`，以单K-entry tile buffer连接冻结的
  `gemmini_im2col_chw_gather_readable`和patched `SA_ENGINE`，按Step 5状态机驱动
  launch、连续K拍输入、输出请求/反压及最终drained；配置和bias从launch保持到
  `cal_finish`；
- 新增pipeline RTL testbench，按冻结activation/weight/bias generator初始化scratchpad，
  写出53字段canonical trace和严格NCHW output，并报告Im2Col done、SA最后结果和
  pipeline drained三个周期锚点；256 PE observer在valid/commit条件下采集activation、
  weight和24-bit accumulator；
- 新增单profile RTL runner和7-profile矩阵runner；每个RTL output先逐元素匹配独立
  Python convolution oracle，trace通过严格schema/锚点校验后才写fixture manifest；
- 新增工作站入口，顺序执行legacy Im2Col、patched SA K567正/负饱和以及7个端到端
  profile；结果收集器记录RTL/filelist/provenance、resolved config、simulator和全部
  artifact SHA256，拒绝覆盖已有结果；
- 新增Step 8来源校验器，复用Step 0 original/integration唯一补丁检查，并额外冻结
  pipeline filelist顺序、7-profile矩阵和RTL testbench的53字段header；
- 新增回传校验器，拒绝路径逃逸，复核source/config/simulator/artifact hash，重新运行
  output oracle和trace校验；可在提供7组gem5结果后执行严格逐拍比较，validated golden
  只在显式指定全新导入目录时复制；
- 本地完整pipeline Python回归42项及Step 8工具专项3项通过；Step 8 source verifier、
  Python语法、shell语法、确定性VCS命令dry-run、行长和`git diff --check`通过；本机
  没有VCS、iverilog或Verilator，因此没有执行RTL compile/elaboration/run；
- 已生成确定性工作站包
  `/home/xch/workspace/sau_n_step8_workstation_20260719_r2.tar.gz`，SHA256为
  `0e874d259cc74617263f1f138ef02a7d0f898081ca4e548a7c6be1039214a789`；归档包含71个
  payload、`SHA256SUMS`和metadata，builder已逐文件回读验证；
- 按Pre-Step 0证据复核`src/sau/`的49个文件及38条git status，路径、mode、size、
  type、SHA256和状态全部一致。

当前只能声明Step 8本地实现和工作站交接包完成。必须等VCS返回后先校准Step 0/Step 4
provisional周期锚点，再完成7个pipeline的gem5/RTL strict per-cycle comparison和两个
K567 profile的C++ SA/RTL accumulator strict comparison；在此之前不得声明SAU
`RTL per-cycle validation passed`，也不导入最终golden。

## 已完成

### PLAN 审核和修订

- 审核并修订 `src/sau_n/PLAN.md`；
- 纠正 `ST_DONE`、`done` 和 `busy` 的逐拍关系；
- 固定 cycle 0、old-state/next-state/commit 和 trace 终止规则；
- 固定 CSV 字段格式、state 编码和 resolved config canonical JSON；
- 增加 64-bit checked multiplication 和大规模 workload 运行策略；
- 明确统计口径、构建确认点和最小 golden fixture 覆盖集合；
- 增加 Step 0 baseline RTL 和工具检查。

### Step 0：baseline RTL 和工具检查

- 只读检查了 reference DUT 和 testbench；
- DUT 与 testbench 的固定参数一致：
  - `BLOCK_SIZE = 16`；
  - `ELEM_W = 8`；
  - `SP_BANKS = 16`；
  - `SP_BANK_ENTRIES = 4096`；
  - `FIFO_DEPTH = 4`；
- 确认 testbench 使用 10 ns 时钟周期，即 100 MHz；
- 确认 SRAM response 为组合响应；
- 确认 legacy 模式固定 `feed_ready = 1`；
- 确认现有四个 baseline 案例及其静态推导结果：

| 案例 | 输出尺寸 | 预期 feed vectors |
|---|---:|---:|
| `w5_pack3_pad1_stride1` | 4×5 | 36 |
| `w7_pack2_pad0_stride2` | 2×3 | 12 |
| `w20_split_pad1_stride1` | 3×20 | 108 |
| `w9_pack1_pad2_stride2` | 4×6 | 72 |

### Step 1：类型、校验和周期契约

- 新增 Python `ResolvedConfig`、`DerivedConfig` 和对应校验；
- 新增 C++ `ResolvedConfig`、`DerivedConfig` 和对应校验；
- Python/C++ 都实现了：
  - 固定参数范围校验；
  - `kernel_h * kernel_w <= 16`；
  - `W <= 16` 时 `out_w <= W`；
  - scratchpad footprint 校验；
  - `cfg_dw_mode=0` 和 `cfg_kernel_pattern=0xffff`；
  - 64-bit checked add/multiply；
  - rows/words/groups 和 `expected_vectors` 派生；
- 冻结六态数值编码、47 字段 trace header 和十六进制字段宽度；
- 冻结 control old-state/next-state/commit 最小数据结构；
- 固定 cycle 0 为 `ISSUE/busy=1/done=0`；
- 固定 done 锚点为 `DONE/busy=1/done=0` 后接
  `IDLE/busy=0/done=1`；
- Python 实现 canonical resolved config JSON 和 SHA256。

### Step 2：fixture loader 和 logical oracle

- 新增严格 JSON fixture loader：
  - 拒绝缺失字段、未知字段、重复字段和非 object 根节点；
  - `out_h/out_w` 必须同时提供或同时省略；
  - 自动模式按对称 padding 公式计算输出尺寸；
  - 自动公式分子为负时拒绝；
  - 显式模式允许与自动结果不同并返回 warning；
  - runtime `feed_ready` 参数不允许写入 fixture；
  - 输出 resolved config、SHA256 和 `expected_vectors`；
- 新增独立 logical feed oracle：
  - 实现 `tb_act_value_v1`；
  - 使用 NCHW 坐标直接生成 `feed_data/feed_mask`；
  - 顺序为 `kw -> kh -> c -> output group -> n`；
  - 不计算或调用物理 bank、row、冲突、FIFO 和周期 helper；
  - padding lane 为 `mask=1,data=0`，尾部/打包空位为 `mask=0,data=0`；
- 覆盖 `W=1/5/16/17/20`、padding、stride、dilation、尾块和顺序测试。

### Step 3：地址和内部 scratchpad

- 新增 C++ `ChwAddressMapper`：
  - `W <= 16` 时按 `floor(16/W)` 个 H 行打包；
  - `W > 16` 时每个 H 行按 16-byte word 分段；
  - 实现 N/C/H/W 到 `wordOffset/row/bank/laneSel` 的映射；
  - `spad_base` 只增加 bank 内 row，不解释高 bank 位；
  - 越界 tensor 坐标和 scratchpad 地址明确拒绝；
- 新增 C++ `tbActValueV1` 低 8-bit 确定性输入；
- 新增 16×4096×8-bit `BankedScratchpad`：
  - clear/read/write 和边界检查；
  - 确定性 CHW preload；
  - 未使用 packed lane 保持为 0；
  - `resp_valid=req_valid` 的同周期组合 SRAM response；
- 定向覆盖 W=5 packing、W=20 splitting、W=16/17 分支、非零 base、重复源地址
  同 bank 同 row、同 bank 不同 row 和 footprint 最后一行边界。

### Step 4：状态机、仲裁和 FIFO

- 新增纯 C++ `Im2ColModel` tick 核心：
  - cycle 0 从 `ST_ISSUE` 开始；
  - 每拍先观察 old registers，再计算组合请求/响应，最后统一提交 next registers；
  - 实现 `IDLE/ISSUE/COLLECT/PUSH/NEXT/DONE` 六态转换；
  - `done` 保持为寄存器脉冲，固定 `DONE/done=0/busy=1` 后接
    `IDLE/done=1/busy=0`；
- 实现 16-lane 请求生成和 16-bank 仲裁：
  - 每 bank 每拍最多一个 row 请求；
  - 按 destination lane 升序选择；
  - 同 bank 同 row 的 lane 由一次响应共同完成；
  - 同 bank 不同 row 跨多个 COLLECT 周期串行；
  - 最后一次响应后保留额外的无请求 COLLECT 判定拍；
- 实现深度 4 FIFO 和原生握手语义：
  - feed 始终来自周期开始时的 FIFO head；
  - 空 FIFO 同拍 push 不旁路；
  - 满 FIFO 同拍 pop 不允许同拍 push；
  - 支持默认 ready 和经过校验的周期性 ready；
- 实现 `rtlDoneCycle/drainedCycle/postDoneDrainCycles` 和 64-bit 统计；
- 每拍检查 FIFO push/pop/handshake 守恒，drained 时强制检查 push 和 handshake
  均等于 `expected_vectors`；
- 统计请求周期、bank row 冲突、额外 COLLECT 周期、lane 分类、FIFO 占用、full
  stall 和 downstream backpressure。

### Step 5：gem5 SimObject 和运行入口

- 用户已明确确认本步骤所需的构建系统修改；
- 新增 `Im2ColTiming` ClockedObject 包装：
  - 通过生成的 SimObject params 构造 resolved config；
  - 在 gem5 时钟边沿逐拍调用纯 `Im2ColModel::tick()`；
  - 使用 `PeriodicReady` 驱动默认或周期性 `feed_ready`；
  - drained 后报告 done/drained 周期并通过标准 gem5 exit event 退出；
- 新增 `src/sau_n/SConscript`：
  - 注册 `Im2ColTiming` SimObject；
  - 注册 Step 1 至 Step 5 C++ source；
  - 注册四个独立 C++ GTest 目标；
- 新增 `configs/example/im2col_timing.py`：
  - 加载并严格校验 JSON fixture；
  - 打印 warning、`expected_vectors` 和 resolved config SHA256；
  - 默认使用 100 MHz，支持 `--ready-period/--ready-high-cycles`；
  - 不创建 CPU、内存总线、系统内存或完整 NPU；
  - drained 后检查退出原因并写出 gem5 `stats.txt`；
- trace writer 和 Im2Col 专项 gem5 stats 按计划保留到 Step 6。

### Step 6：cycle trace、专项统计和比较器

- 新增独立 C++ `Im2ColTraceWriter`：
  - 严格输出冻结的 47 字段 CSV header；
  - cycle 0 到 drained 全部记录，包含首尾行；
  - state、FIFO、16-bank request/response 和 feed 字段逐拍输出；
  - bank 0/lane 0 固定为 packed 字段最低有效位；
  - 无效 request/response/feed payload 规范化为 0；
  - 固定宽度小写 hex，并严格校验 64 位小写 resolved SHA256；
- `Im2ColTiming` 接入默认 `<outdir>/im2col/trace.csv`，也支持 `--trace` 覆盖路径；
- 注册 PLAN 定义的 Im2Col 专项 gem5 stats：
  - done/drained 和 inclusive cycle count；
  - feed vector、handshake 和 vectors/cycle；
  - presented/SRAM-read/padding-zero/invalid lane；
  - 每 bank request cycles 和 utilization；
  - bank row conflict 和额外 COLLECT cycle；
  - FIFO 平均/峰值占用、full stall 和 downstream backpressure；
- 新增 Python 严格逐拍比较器：
  - 校验 UTF-8、LF、最终 LF、47 字段 header 和连续 cycle；
  - 校验 bool/state/FIFO 范围、固定 hex 宽度和 invalid payload 规范化；
  - 对两个合法 trace 逐周期逐字段比较；
  - 首个差异报告 cycle、字段名及 expected/actual；
  - 支持模块导入和 direct-script CLI；
- 当前比较器已用合成 trace 和 gem5 自身 trace 验证，尚未输入 RTL trace。

### Step 7：RTL observer、fixture runner 和回归入口

- 保持 reference DUT 不变，只扩展现有 testbench：
  - 未提供 `+FIXTURE_MODE` 时继续运行原有四个 legacy 案例；
  - fixture 模式在 negedge 驱动 `cfg_valid/start`，避免 posedge 采样竞争；
  - cycle 0 从 start 被接受后的稳定区间开始；
  - fixture 模式支持与 gem5 相同的周期性 `feed_ready`；
  - negedge observer 输出冻结的 47 字段 CSV；
  - 无效 request/response/feed payload 规范化为 0；
  - testbench 自检每次 feed handshake，并从 done 继续记录到 FIFO drained；
- 新增 Python RTL fixture runner：
  - 复用严格 JSON fixture loader，将 resolved config 转成显式 plusargs；
  - 运行用户提供的预编译 simulator executable，不负责安装或编译 simulator；
  - 验证 RTL PASS 行、done/drained 周期、trace schema 和 logical feed sequence；
  - 保存 resolved config、source SHA256、硬件参数、simulator 名称/版本和周期结果
    manifest；
  - 支持 `--dry-run` 只打印确定性 simulator 命令；
- 新增 6 个 workload fixture 和 7 项最小 golden 运行矩阵，整体覆盖：
  - `W=1/5/16/17/20`；
  - 非零 `spad_base`、`N>1`、`C>1`；
  - padding、stride、dilation、尾 lane；
  - 全 padding vector；
  - 自然产生的同 bank 不同 row 串行；
  - 强反压下 FIFO full、full 同拍 pop 和 done 后继续 drain；
- 新增 7 项 gem5 quick test，逐项与已验证 RTL golden 严格比较，并检查
  done/drained stats 周期锚点。

## 文件状态

当前相关文件：

- `src/sau_n/PLAN.md`：计划已修订；
- `src/sau_n/README.md`：Step 8 用户文档、运行说明、限制和 RTL golden provenance；
- `src/sau_n/im2col_types.hh/.cc`：C++ Step 1 类型和契约；
- `src/sau_n/im2col_types.test.cc`：C++ Step 1 单元测试；
- `src/sau_n/im2col_address.hh/.cc`：Step 3 CHW 地址和 packing helper；
- `src/sau_n/im2col_address.test.cc`：Step 3 地址单元测试；
- `src/sau_n/banked_scratchpad.hh/.cc`：Step 3 banked scratchpad；
- `src/sau_n/banked_scratchpad.test.cc`：Step 3 scratchpad 单元测试；
- `src/sau_n/im2col_model.hh/.cc`：Step 4 纯 tick 状态机、仲裁、FIFO 和统计；
- `src/sau_n/im2col_model.test.cc`：Step 4 逐拍和反压单元测试；
- `src/sau_n/Im2Col.py`：Step 5 SimObject 参数描述；
- `src/sau_n/im2col_timing.hh/.cc`：Step 5 ClockedObject 包装；
- `src/sau_n/SConscript`：Step 5 source、SimObject 和 GTest 构建注册；
- `configs/example/im2col_timing.py`：Step 5 fixture 驱动的 gem5 运行入口；
- `src/sau_n/im2col_trace.hh/.cc`：Step 6 严格 cycle CSV writer；
- `src/sau_n/im2col_trace.test.cc`：Step 6 writer 单元测试；
- `util/im2col/compare_traces.py`：Step 6 严格逐拍 trace 比较器；
- `util/im2col/compare_traces_test.py`：Step 6 比较器单元测试；
- `src/sau_n/rtl/gemmini_im2col_chw_gather_readable.sv`：reference DUT，未修改；
- `src/sau_n/rtl/tb_gemmini_im2col_chw_gather_readable.sv`：Step 7 fixture 模式、
  周期性 ready、CSV observer 和 legacy 兼容入口；
- `util/im2col/rtl_fixture_runner.py`：Step 7 JSON 到 RTL plusargs runner、trace 验证
  和 manifest writer；
- `util/im2col/rtl_fixture_runner_test.py`：Step 7 runner 和 golden 矩阵测试；
- `tests/gem5/im2col/fixtures/*.json`：Step 7 最小 workload fixture；
- `tests/gem5/im2col/golden_matrix.json`：Step 7 fixture/runtime 运行矩阵；
- `tests/gem5/im2col/ref/*/trace.csv`：Step 7 已验证 VCS RTL golden trace；
- `tests/gem5/im2col/ref/*/manifest.json`：对应 resolved config、source SHA256、
  simulator 和周期 provenance；
- `tests/gem5/im2col/test_im2col.py`：7 项 RTL golden/gem5 strict quick test；
- `util/im2col/im2col_contract.py`：Python Step 1 类型和契约；
- `util/im2col/im2col_contract_test.py`：Python Step 1 单元测试；
- `util/im2col/im2col_fixture.py`：Step 2 JSON fixture loader 和 CLI；
- `util/im2col/im2col_fixture_test.py`：Step 2 loader 单元测试；
- `util/im2col/logical_oracle.py`：Step 2 logical feed oracle；
- `util/im2col/logical_oracle_test.py`：Step 2 oracle 单元测试；
- `src/sau_n/STATUS.md`：本状态记录。

当前 Git 将 `src/sau_n/`、`util/im2col/`、`tests/gem5/im2col/` 和
`configs/example/im2col_timing.py` 识别为未跟踪内容。

Step 0 记录的源码 SHA256：

```text
gemmini_im2col_chw_gather_readable.sv
1e7e085e53f2fa63a21336d407805ec6db354ffa16de5d6901061b9e6b0c680c

tb_gemmini_im2col_chw_gather_readable.sv
f9380a19ca801a70476256383656454b03dc775045f8aecad09407c5e9bdb2be
```

Step 7 扩展后的 testbench SHA256：

```text
tb_gemmini_im2col_chw_gather_readable.sv
989a054f0531b06c01431adadc96f258c8925e376336d04f7ee6798772e56d36
```

## Verification

已执行只读检查：

- `git status --short -- src/sau_n`；
- `sha256sum` 检查 DUT 和 testbench；
- 使用 `rg` 核对固定参数、时钟、SRAM response、`feed_ready` 和四个案例；
- 检查常见 SystemVerilog 工具的 `PATH`、命令索引和标准定位结果。

已执行 Step 1 验证：

```bash
python3 -m unittest util.im2col.im2col_contract_test -v
```

结果：15 项 Python 测试全部通过。

已执行 Step 2 Python 全量验证：

```bash
python3 -m unittest discover -s util/im2col -p '*_test.py' -v
```

结果：33 项测试全部通过，其中 Step 2 新增 18 项 loader/oracle 测试。

fixture CLI 的以下两种入口均已 smoke test 通过：

```bash
python3 util/im2col/im2col_fixture.py <fixture>
python3 -m util.im2col.im2col_fixture <fixture>
```

两种入口输出相同的 canonical resolved JSON，并在 stderr 打印
`expected_vectors`。

使用仓库自带 googletest 独立编译 C++ 测试，启用：

```text
-std=c++17 -Wall -Wextra -Werror
```

编译通过；运行 `/tmp/sau_n_im2col_types_test_werror`，12 项 C++ 测试全部通过。
该独立测试没有修改 gem5 build 目录，也没有新增 `SConscript`。

已执行 Step 1+3 合并 C++ 严格编译和回归：

```text
-std=c++17 -Wall -Wextra -Werror
/tmp/sau_n_step1_step3_test
```

结果：25 项 C++ 测试全部通过，其中 Step 3 新增 13 项地址/scratchpad 测试。

已执行 Step 1 至 Step 4 合并 C++ 严格编译和回归：

```text
-std=c++17 -Wall -Wextra -Werror
/tmp/sau_n_step4_test
```

结果：35 项 C++ 测试全部通过，其中 Step 4 新增 10 项仲裁、周期性 ready 和
tick model 测试。

另使用 AddressSanitizer 和 UndefinedBehaviorSanitizer 编译并运行同一组测试：

```text
-fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_OPTIONS=detect_leaks=0 /tmp/sau_n_step4_sanitize
```

结果：35 项测试全部通过。当前运行环境下 LeakSanitizer 因 ptrace 限制无法启动，
因此仅关闭 leak detection；AddressSanitizer 和 UndefinedBehaviorSanitizer 保持启用。

已执行 Step 5 静态验证：

```bash
scons --dry-run -j2 build/RISCV/gem5.opt
```

结果：SCons 成功读取新 `SConscript`，识别 `Im2ColTiming` params 生成、五个 C++
source、四个 GTest 注册和最终 `gem5.opt` 链接依赖；该命令为 dry-run，没有执行
增量编译。

另对两个新增 Python 文件执行无字节码语法编译，并检查 Step 5 文件的行长度和
尾随空白，结果通过。修改后重新运行 35 项 C++ 回归和 33 项 Python 回归，全部
通过。

开发者使用内存受限构建选项完成了真实增量构建：

```bash
scons build/RISCV/gem5.opt -j4 --ignore-style --limit-ld-memory-usage
```

确认 `build/RISCV/gem5.opt` 已更新并包含 `Im2ColTiming`。随后使用
`w5_pack3_pad1_stride1`（`expected_vectors=36`）执行两组 gem5 quick test：

```bash
build/RISCV/gem5.opt -d /tmp/sau_n_step5_default \
    configs/example/im2col_timing.py \
    --fixture /tmp/sau_n_step5_w5.json

build/RISCV/gem5.opt -d /tmp/sau_n_step5_periodic \
    configs/example/im2col_timing.py \
    --fixture /tmp/sau_n_step5_w5.json \
    --ready-period 7 --ready-high-cycles 2
```

结果：

- 默认 ready：`done=175`、`drained=175`、`post_done_drain=0`；
- 周期性 ready：`done=175`、`drained=176`、`post_done_drain=1`；
- 两组退出原因均为 `im2col model drained`；
- 两组均生成非空 `config.ini` 和 `stats.txt`；
- `config.ini` 中 fixture 名称、resolved SHA256、100 MHz clock 和 ready 参数均与
  命令一致；
- `stats.txt` 的 `simTicks` 分别为 1750000 和 1760000，与 100 MHz 下的 drained
  cycle 对应。

已执行 Step 6 增量构建：

```bash
scons build/RISCV/gem5.opt -j4 --ignore-style --limit-ld-memory-usage
```

结果：新 trace writer、stats、SimObject params 和 `gem5.opt` 编译链接成功。仅有
仓库既有的可选 Capstone/HDF5 缺失 warning。

重新运行默认 ready 和周期性 ready quick test，分别生成：

```text
/tmp/sau_n_step6_default/im2col/trace.csv
/tmp/sau_n_step6_periodic/im2col/trace.csv
```

- 默认 trace：cycle 0..175，共 176 个数据 cycle；
- 周期性 ready trace：cycle 0..176，共 177 个数据 cycle；
- 两份 trace 均通过严格 parser 和 self-compare；
- 两种模式的 36 个 handshake `feed_data/feed_mask` 均逐向量匹配独立 logical
  oracle，且顺序完全相同；
- trace 推导的 done/drained、push、handshake、backpressure、FIFO 平均/峰值/full
  stall、每 bank request cycles/utilization 均与 `stats.txt` 一致；
- lane 统计满足 `presented = SRAM-read + padding-zero`，且三类有效/无效 lane
  总数为 `expected_vectors * 16`；
- 用两种模式互相比对时，比较器正确报告首差异：

```text
cycle 2, field feed_ready: expected 1, actual 0
```

Step 6 合并 C++ 严格编译及 ASan/UBSan 回归为 38 项，全部通过；Python 全量回归
为 39 项，全部通过，其中新增 6 项 comparator 测试。LeakSanitizer 仍因 ptrace
限制关闭。

已执行 Step 7 Python 和 runner 验证：

```bash
PYTHONDONTWRITEBYTECODE=1 \
python3 -m unittest discover -s util/im2col -p '*_test.py' -v
```

结果：43 项全部通过，其中新增 4 项 runner、plusargs、manifest 和 golden matrix
测试。runner 的 direct-script `--dry-run` 入口也已通过，输出 resolved SHA256、
`expected_vectors` 和完整 simulator plusargs。

使用现有已构建的 `build/RISCV/gem5.opt` 运行完整 7 项 golden 配置矩阵，全部正常
到达 drained，且所有 trace 均通过严格 parser/self-compare：

| 运行 | expected vectors | done | drained |
|---|---:|---:|---:|
| W1、N2/C2、base7 | 4 | 21 | 21 |
| W5 baseline | 36 | 175 | 175 |
| W16 explicit tail | 2 | 11 | 11 |
| W17 dilation/tail | 24 | 109 | 109 |
| W20 stride2 conflict | 54 | 301 | 301 |
| W5 all padding | 1 | 5 | 5 |
| W5 ready=1/11 | 36 | 356 | 397 |

- W20 配置产生 `bankRowConflicts=70`、`extraCollectCycles=42`；
- 强反压配置产生 `fifoPeakOccupancy=4`、`fifoFullStallCycles=181`；
- 强反压 trace 在 cycle 33 覆盖 `state=PUSH/fifo_count=4/feed_ready=1`，即 FIFO
  full 且同拍 pop，push 仍按 old-full 语义停顿；
- 强反压配置的 `postDoneDrainCycles=41`，覆盖 done 后 FIFO 继续排空。

已通过官方 gem5 testlib 执行新增 quick test：

```bash
cd tests
./main.py run --skip-build gem5/im2col
```

结果：初始两项 suite 的运行、stdout、stats 和严格 trace verifier 共 6 项全部通过。

检查过但未发现可执行程序的工具包括：

```text
iverilog, vvp, verilator, vcs, xrun, irun,
vlog, vsim, questa, slang, surelog,
verible-verilog-syntax, svlint, yosys
```

本地环境没有 SystemVerilog simulator，因此 RTL 编译和仿真通过工作站交接包完成。

### Step 7 工作站交接包

- 已生成自包含工作站验证包：
  `/home/xch/workspace/sau_n_step7_workstation_20260717.tar.gz`；
- 压缩包 SHA256：
  `053b26f6b85ba1749850dca480fe85008d653cbd97958b8c825a226c2c26c4b0`；
- 包内包含 DUT、Step 7 testbench、fixture/矩阵、RTL runner、批量执行脚本、源码
  SHA256 清单和中文 README；
- 已从最终压缩包实际解包，并通过源码 SHA256、Python 语法、runner `--dry-run` 和
  matrix CLI 检查；
- 开发者已返回结果包：
  `/home/xch/workspace/sau_n_step7_rtl_results.tar.gz`；
- 结果包 SHA256：
  `babf0fdfb7d598db09a8aa4662a2ba4b9fda7677fb2749d47d47da9e62225351`。

### Step 7 VCS RTL 和逐拍验收

- 工作站环境：VCS `T-2022.06`，Linux `4.18.0-545.el8.x86_64`；
- DUT/testbench 编译、elaboration 和链接成功，无编译错误；
- legacy 四例全部运行并输出
  `PASS tb_gemmini_im2col_chw_gather_readable`；
- 7 项 RTL fixture 全部通过 testbench feed 自检、runner trace schema、done/drained
  锚点和 independent logical oracle 校验；
- 7 份 manifest 的 resolved config、DUT/testbench SHA256、固定硬件参数、trace schema
  和 simulator 信息均与当前工程一致；
- 使用当前 `build/RISCV/gem5.opt` 重新生成全部 7 项 gem5 trace；
- 严格比较器对 RTL expected 与 gem5 actual 的全部 47 个字段逐拍比较：

| 运行 | 比较 cycles | done | drained | 结果 |
|---|---:|---:|---:|---|
| W1、N2/C2、base7 | 22 | 21 | 21 | PASS |
| W5 baseline | 176 | 175 | 175 | PASS |
| W16 explicit tail | 12 | 11 | 11 | PASS |
| W17 dilation/tail | 110 | 109 | 109 | PASS |
| W20 stride2 conflict | 302 | 301 | 301 | PASS |
| W5 all padding | 6 | 5 | 5 | PASS |
| W5 ready=1/11 | 398 | 356 | 397 | PASS |

合计比较 1,026 个 cycle，无任何字段差异。已将对应 `trace.csv` 和 `manifest.json`
保存到 `tests/gem5/im2col/ref/`。

导入 golden 后重新运行官方 gem5 testlib：

```bash
cd tests
./main.py run --skip-build gem5/im2col
```

结果：7 项 suite 的 gem5 运行、stdout、stats 和 RTL strict trace verifier 共 21 项
全部通过。

当前验证状态为：

```text
RTL per-cycle validation passed
```

### Step 8 文档和最终验收

- 新增 `src/sau_n/README.md`，记录：
  - reference RTL 周期模型定位和非真实 Gemmini/NPU 声明；
  - 固定硬件参数和 cycle/done/drained 契约；
  - fixture 字段、范围、自动/显式输出尺寸规则和确定性输入；
  - 默认 ready、周期性反压、trace 比较和 RTL runner 命令；
  - `im2col.*` 统计项、守恒关系、golden provenance 和不支持范围；
  - 大规模合法显式输出可能运行很久且超时由外部框架负责；
- README 共 311 行，行长和尾随空白检查通过；
- README 的默认 gem5 运行和 strict comparator 示例已实际 smoke test，通过 176
  cycle RTL golden 比较；
- Step 8 最终 Python 回归：43 项全部通过；
- Step 8 最终官方 gem5 testlib：7 项 suite、21 项检查全部通过；
- C++ 源码自 Step 6 的 38 项普通及 38 项 ASan/UBSan 回归通过后未再修改；本轮尝试
  复跑时 `/tmp` 临时二进制已不存在，遵守项目约束未主动重新编译；当前
  `gem5.opt` 的 7 项 RTL strict 端到端运行全部通过；
- Step 7 VCS legacy、自检、logical oracle 和 1,026-cycle 严格逐拍结果继续有效；
- PLAN 定义的最终交付和验收条件均已满足。

## 已知风险

- legacy testbench 仍在 `@(posedge clk)` 返回后立即拉低 `cfg_valid` 和 `start`，与
  DUT 的 posedge 采样存在仿真调度竞争风险；Step 7 fixture 模式已改为 negedge
  驱动，legacy 行为为保持兼容而未改动；VCS T-2022.06 上 legacy 四例已通过；
- testbench 包含 VCS 专用 `$vcdplus*` 调用；使用其他 simulator 时需要确认兼容性；
- 大规模显式 `out_h/out_w` workload 可能需要很长运行时间，后续实现必须使用
  64-bit checked counters，并由外部测试框架负责超时。

## 下一步

原 Im2Col `PLAN.md` 范围内没有必需的下一步。Im2Col `src/sau_n/` baseline已纳入
版本控制；相关 `util/im2col/`、`tests/gem5/im2col/` 和
`configs/example/im2col_timing.py` 当前仍未纳入版本控制，后续交付前需要单独审核。

融合 RTL 的 VCS 验收、gem5 模型迁移、构建、七项 strict comparison 和最终源码
交付包均已完成，当前没有必需的剩余步骤。未来若修改 Im2Col DUT、项目自有阵列、
testbench、周期模型、fixture 解析或 trace schema，必须重新生成 provenance 并运行
七项 RTL strict 回归。

### 用户 workload fixture（2026-07-20）

- 新增 `tests/gem5/conv_pipeline/fixtures/08_n1_c16_h16_w32_oc16.json`；
- 配置为 N1/C16/H16/W32/OC16、3x3、stride 1、dilation 1、padding 1、cutbit 8；
- 该 fixture 用于独立 gem5 运行，未加入已冻结的七项 RTL golden matrix。

### 融合流水线 CollectTile 周期统计（2026-07-20）

- `ConvPipelineModelStats` 新增 `collectTileCycles`，按每拍开始时的
  `PipelineState::CollectTile` 累计，与 `trace.csv` 的 `pipeline_state=1` 口径一致；
- `convPipeline` stats 组新增：
  - `collectTileCycles`：处于 `CollectTile` 状态的周期数；
  - `nonCollectCycles`：`totalCycles - collectTileCycles`；
- 增加模型测试，将内部计数与逐拍 observation 独立计数比较；
- 已执行 `git diff --check` 和相关源码静态检查；根据 `src/sau_n/AGENTS.md` 的
  编译约束，未主动重新编译 gem5，新增 stats 尚待开发者增量编译后运行确认。

### Im2Col 流水化探索计划（2026-07-21）

- 新增 `src/sau_n/IM2COL_PIPELINE_PLAN.md`，用于规划 gem5 架构探索；
- 目标是在保留旧 RTL 等价模型的前提下，新增三级弹性 Im2Col、深度 4 FIFO 和支持
  输入气泡的直接 PE 流式路径；
- 第一版范围冻结为 3x3、dilation 1、统一 stride 1/2、统一 padding 0/1、OC 1..16，
  并保持 16 个单端口 bank 和组合 Scratchpad 响应；
- 目标 fixture 保持
  `tests/gem5/conv_pipeline/fixtures/08_n1_c16_h16_w32_oc16.json`，性能门槛为
  `totalCycles < 12000` 且无冲突稳态 `II=1`，数值输出必须逐字节一致；
- 计划审查后已冻结 streaming consumer 的 `pe_ready/input_fire`：PE ready 只在
  `ACCEPT_K` 状态有效，FIFO pop、weight 选择、SA 输入、MAC 调度和 `accepted_k`
  统一由同一个 `input_fire` 控制；
- `SauCycleModel` 计划增加默认 strict、显式 elastic 的输入协议模式；旧
  `ConvPipelineModel` 固定使用 strict 模式并保留气泡异常，新 streaming 入口才启用
  elastic 模式，避免改变已通过 RTL 严格验证的旧路径；
- 已补充 S0/S1/S2 同拍弹性推进公式、无符号 padding 下溢保护、单端口 bank 最少读取
  轮数公式，以及 `conflictFreeOutputII` 的整数计数验收口径；
- 当前仅完成需求收敛和实施计划，尚未修改模型、构建文件或 RTL，也未执行编译或
  性能验证。
- 计划审查发现自然输出尺寸的 `W6/stride2` 在现有 W<=16 packing 中会产生 scattered
  raw spatial mask，与旧 pipeline 的 canonical-prefix 限制冲突；计划已修订为仅在新
  streaming 路径的 S2 做稳定 lane compaction，并保存 compacted PE row 到 raw lane/
  NCHW 坐标映射。旧 pipeline fixture loader 和旧模型的 scattered-mask 拒绝行为保持
  不变。
- 已修正 PE 气泡契约：无 `input_fire` 只表示不注入新 token、不推进 K、不创建新
  MAC；更早输入已经调度的 MAC 仍可在气泡所在墙钟周期 commit 并更新 accumulator，
  不能冻结阵列内部流水。
- 目标 workload 的候选预算约为每 tile 195..196 拍、32 tiles 共 6240..6272 拍，说明
  `<12000` 门槛具有余量；该预算不是 magic expected cycle，最终仍以通用模型统计为准。
- `28378/22169/5891` 暂标为待 Step 0 provenance 冻结的候选基线；正式比较前必须保存
  运行命令、worktree/build 来源、fixture/stats/output SHA256 和对应原始 artifact。

### Im2Col 流水化 Step 0 启动（2026-07-21）

- 已确认目标 fixture 的 resolved config SHA256 为
  `888705e26fd978e08a95c702d688eb1eede4b1d522e43c02ac500d1313deae6c`，派生值为
  32 tiles、4608 activation vectors、8192 outputs 和 1179648 useful MACs；
- 使用现有 `build/RISCV/gem5.opt` 运行目标 fixture并正常以
  `conv pipeline drained` 退出，原始 artifact 保存到
  `m5out/im2col_pipeline_step0_baseline_20260721/`；
- 独立 direct-NCHW convolution oracle 已逐项验证全部 8192 个 output；
- 现有 binary SHA256 为
  `63fced0cdf780e82a5c355315bb5c7e34c3a299c81993fb3b5b6bac6435faa54`，报告编译时间为
  2026-07-19 19:28:25，早于当前源码和 `collectTileCycles/nonCollectCycles` 更新；
- 该旧 binary 得到 `totalCycles=28058`、`im2colBackpressureCycles=5581`，不包含
  `collectTileCycles/nonCollectCycles`，并输出历史 27 字段未完整 flush 的 trace，因此
  本次只作为 pre-rebuild 证据，不能冻结为正式性能基线；
- Step 0 当前等待开发者按目录约束手动增量编译当前源码。新 binary 构建成功后必须写入
  新输出目录重新运行，再冻结正式 stats、output、trace、命令和 SHA256；不得覆盖上述
  pre-rebuild 证据。

### Im2Col 流水化 Step 0 正式基线冻结（2026-07-21）

- 开发者已手动完成当前源码的增量编译；新 `gem5.opt` SHA256 为
  `4766b45e47e13049aa21c165fcf823c9918179c09d695fe888fa6709c56cdcc7`，binary 报告编译
  时间为 2026-07-21 21:32:32；
- 使用与 pre-rebuild 完全相同的目标 fixture 和默认 output ready 运行，正式 artifact
  保存到 `m5out/im2col_pipeline_step0_formal_20260721/`，未覆盖旧证据；
- 正式基线精确复现计划候选值：`totalCycles=28378`、`collectTileCycles=22169`、
  `nonCollectCycles=6209`、`im2colBackpressureCycles=5891`；
- 完成周期为 `im2colDoneCycle=28184`、`sauLastResultCycle=28375`、
  `drainedCycle=28377`；32 tiles、4608 activation handshakes、4608 engine input cycles
  和8192 outputs 全部满足守恒；
- 53字段 canonical trace 含 cycle 0..28377 共28378个数据周期，最终LF、严格loader和
  self-comparison均通过；
- 8192个NCHW output全部通过独立direct convolution oracle，并与pre-rebuild output
  逐字节一致；
- 已保存运行命令、HEAD/worktree状态、binary/fixture/resolved config信息、原始
  stats/trace/output/config/log和全artifact SHA256清单；目标 workload 的正式基线冻结
  已完成。
- 随后执行 `cd tests && ./main.py run --skip-build gem5/conv_pipeline`，结果为20项通过、
  1项失败；7个gem5运行和退出检查全部通过，6个RTL strict verifier通过，唯一失败是
  `03_c3_w16_oc16_full` 的resolved config SHA256不一致；
- 根因是当前commit `47b454350a`已将该fixture的`cutbit`从golden冻结的8改为12：当前
  hash为`0d9ee29f41d4851100dce8a5950fd1c8230a50899f3170586d02542d523e18d9`，RTL manifest
  hash为`292d4723cb4d570796e7d72700e0a6afaf39336ce49a1e35628d96290230646e`且记录`cutbit=8`；
- 该fixture/golden冲突早于本次Step 0且不影响目标fixture 08的正式基线；发现时旧
  strict regression验收保持pending，等待用户决定后再继续，处理结果记录如下。

### Im2Col 流水化 Step 0 完成（2026-07-21）

- 经用户确认，将冻结profile `03_c3_w16_oc16_full.json`的`cutbit`从12恢复为RTL
  manifest记录的8；未修改RTL golden；
- fixture重新解析后的resolved config SHA256为
  `292d4723cb4d570796e7d72700e0a6afaf39336ce49a1e35628d96290230646e`，与manifest
  精确一致；
- 重新执行`cd tests && ./main.py run --skip-build gem5/conv_pipeline`，7个suite的gem5
  运行、退出检查和RTL strict verifier共21项全部通过；
- 正式基线artifact的SHA256清单再次全量校验通过，目标fixture 08的28378-cycle基线、
  53字段trace和8192项NCHW output结论保持不变；
- Im2Col流水化Step 0的基线、provenance、功能回归和strict regression验收全部完成，
  可以进入Step 1契约实现。

### Im2Col 流水化 Step 1 完成（2026-07-21）

- 新增 `streaming_pipeline_contract.hh/.cc`，只定义新探索路径的纯 C++ 契约；既有
  `Im2ColModel`、`ConvPipelineModel`、`SauCycleModel` 和旧运行入口均未修改；
- 明确定义 S0/S1/S2 payload、lane source、深度 4 FIFO entry、三级
  valid/ready/fire 组合关系，以及 FIFO 满且同拍 pop 时允许 push 的计数语义；
- 冻结 vector tag 的 canonical `k_index=c*9+kh*3+kw`、tile first/last、同 tile
  metadata 稳定、tile/K 连续顺序和 drained vector/tile 守恒检查；
- 实现 raw scattered spatial mask 的稳定升序 compaction，保存 compacted PE row 到
  raw source lane 和 NCHW coordinate 的映射，并强制输出 prefix row mask；
- 冻结 consumer 的 IDLE/LAUNCH/ACCEPT_K/WAIT_RESULT/DRAIN_OUTPUT 决策，以及同一个
  `input_fire` 控制输入接受；strict 模式继续拒绝流中气泡，elastic 模式允许气泡且
  保留更早 token 的到期 MAC commit；
- 新增独立 Python mirror/oracle `util/conv_pipeline/streaming_contract.py`，没有复用
  C++ compaction helper；C++ 与 Python 各有 13 项对应定向测试；
- 独立 C++ 测试以 `-Wall -Wextra -Werror` 编译并通过 13 项；ASan/UBSan 版本同样
  通过 13 项；`util/conv_pipeline` 全部 55 项 Python 回归通过；
- 未修改 `SConscript`，未将新源码接入 gem5，也未主动编译 gem5；Step 2 将在这些
  已冻结契约上实现 `PipelinedIm2ColModel`。

### Im2Col 流水化 Step 2 完成（2026-07-21）

- 新增 `pipelined_im2col_model.hh/.cc`，以 `PipelineResolvedConfig` 构造并复用现有
  `ChwAddressMapper`、`BankedScratchpad` 和 Step 1 streaming contract；旧
  `Im2ColModel` 未修改；
- S0 按 `KW -> KH -> C -> W group/H group -> N` 生成 canonical tag、输出坐标、
  padding/invalid/read lane 分类和下溢安全的输入坐标；迭代器只随 S0 向 S1 fire 推进；
- S1 每 bank 每拍选择最低 raw destination lane 的未完成 row，同 bank/同 row 响应
  广播给全部匹配 lane，同 bank/不同 row 分轮读取；本拍组合 response 计入
  `s1_can_retire`，没有额外 all-done 空拍；
- 每个 S1 vector 退休时重新计算各 bank 的 distinct row 数，并断言实际读取轮数等于
  单端口理论最少轮数；W6/stride2 定向用例精确得到 6 个冲突 vector 和 6 个额外轮次；
- 审查修正全 padding/零 SRAM 请求 vector 的轮次口径：按冻结公式
  `max(1, max_b distinct_rows[b])` 计为一轮；定向测试同时确认下游阻塞期间该轮数
  保持为 1，不会重复累计；
- S2 在 S1 fire 时组合执行稳定 raw-lane compaction，寄存保存 raw/compacted payload、
  source-lane/NCHW mapping 和 tag；FIFO 不 ready 时保持 S2 并逐级反压；
- 无冲突且下游 ready 时，cycle 3 首次 `S2 -> FIFO` fire，之后连续 vector 每拍 push，
  同拍允许 S2、S1、S0 和 producer 四个边界全部 fire，寄存器之间没有组合穿透；
- 每拍检查 S0/S1/S2 payload canonical 性、prefix compaction 重算一致性、tile/K 连续、
  `inputVectors-outputVectors == validStages`、最终 tile 数和完整 drained 序列；
- 新增 7 项 `PipelinedIm2Col` 定向测试，覆盖 W1/W6/W17/W32、stride 1/2、padding 0/1、
  N/C 顺序、scattered mask、反压保持及恢复、自定义 Scratchpad 和 `C=63/K=567`；
- 新模型、Step 1 contract、旧 Im2Col、地址 mapper、Scratchpad 和 SA 配置相关测试合并
  为 47 项，普通 `-Wall -Wextra -Werror` 与 ASan/UBSan 构建各自全部通过；
  `util/conv_pipeline` 的 55 项 Python 回归也全部通过；
- 未修改 `SConscript`，未接入 gem5，未主动编译 gem5；下一步 Step 3 将单独修改
  `SauCycleModel`，为新路径增加显式 elastic bubble 模式并保持旧路径 strict。
- 本次 Step 2 审查修正后重新从当前源码严格编译：7 项 `PipelinedIm2Col` 专项、
  72 项 Step 1 至 Step 4 合并普通测试和 72 项 ASan/UBSan 测试全部通过；Python
  `util/conv_pipeline` 55 项及 `git diff --check` 同样通过，未执行 gem5 编译。

### Im2Col 流水化 Step 3 完成（2026-07-21）

- 将 `SauInputProtocol` 放入共享 `sau_types.hh`，定义
  `StrictRtlContinuous` 和 `ElasticBubbleEnabled` 两种协议；数值和状态编码不变；
- `SauCycleModel` 新增构造期协议参数，默认值为 strict，协议成员为 `const` 且无
  setter，避免运行过程中切换语义；非法枚举值在构造时立即拒绝；
- `ConvPipelineModel::InputProtocol` 固定为编译期 strict 常量，并在构造
  `SauCycleModel` 时显式传入；旧融合入口没有新增运行参数，无法启用 elastic；
- strict 模式继续在输入流开始后、K 尚未完成时拒绝气泡，既有 K9、全阵列、输出
  反压、K567 饱和和精确 cycle anchor 测试保持原结果；
- elastic 模式下，无 `inputValid` 不调度新 MAC、不增加 accepted count，也不提前安排
  completion；每次 MAC 仍按该项实际接受的墙钟周期加 row/column skew 调度；
- 新增定向 bubble 测试：3项输入之间插入4个气泡，只产生3项 MAC；更早输入安排的
  MAC 分别在气泡周期正常 commit，累加器依次为6、26、33，bias后输出34；最后输入
  和结果相对连续路径都自然后移4拍；
- 新增 strict/elastic 连续输入逐拍等价测试，比较 state、计数、MAC/add/valid mask、
  全部 PE activation/weight/accumulator、output slot 和 calFinish，结果完全一致；
- Step 1 contract、Step 2 producer、SA、新旧融合模型、旧 Im2Col、地址 mapper 和
  Scratchpad 合并 69 项 C++ 测试全部通过；ASan/UBSan 下同样 69 项全部通过；
  `util/conv_pipeline` 的 55 项 Python 回归全部通过；
- 本步修改的既有源码已能按原 SConscript 依赖独立编译，不依赖尚未登记的新 Step 1/2
  源文件；仍未主动编译 gem5。下一步 Step 4 将实现深度4 FIFO 和 streaming consumer。

### Im2Col 流水化 Step 4 完成（2026-07-21）

- 新增 `streaming_conv_pipeline_model.hh/.cc`，独立组合
  `PipelinedIm2ColModel`、固定深度4 FIFO、consumer FSM 和显式 elastic
  `SauCycleModel`；旧 `ConvPipelineModel` 保持独立 strict 路径；
- consumer 状态为 IDLE/LAUNCH/ACCEPT_K/WAIT_RESULT/DRAIN_OUTPUT；Step 1 decision
  进一步区分 IDLE 发现 tile-first head 的 `beginLaunch` 和 LAUNCH 周期实际发出的
  instruction，首项输入在下一 ACCEPT_K 周期发生，配置和输入严格分拍；
- 每拍先从旧 FIFO head 计算 `input_fire/pop`，再以
  `push_ready=(count<4)||pop` 驱动 producer；FIFO 空且同拍 push 时不旁路，满且同拍
  pop 时允许 producer push，ring read/write pointer 和 count 每拍检查一致；
- 同一个 `input_fire` 同时控制 FIFO pop、weight tag 选择、SA `inputValid`、MAC 调度和
  `acceptedK`；WAIT_RESULT/DRAIN_OUTPUT 即使 FIFO head 已是下一 tile-first 也不 pop；
- IDLE 只接受连续 tile index 的 tile-first head；active tile 的 spatial mask、valid rows、
  source-lane 和 NCHW coordinate mapping 在所有 K 项间保持一致，FIFO tag 不匹配立即
  报错；
- SA 输出按 active compacted row mapping 写回 NCHW，逐项检查 signed INT8 canonical
  sign extension、row sequence、索引范围和重复写；output backpressure 由既有周期 ready
  配置直接传给 SA；
- 每拍检查 producer output/FIFO push、FIFO pop/PE input、已完成 tile/active accepted K、
  launch/completion 状态和 FIFO pointer/count 守恒；drained 时进一步要求 producer、
  FIFO、PE、tile、output row 和 output element 全部达到派生值；
- 新增3项 streaming 端到端测试：单 tile 的 launch/input 分拍和手算输出；N2、W6、
  stride2 scattered compaction 与独立 direct convolution oracle；W32 多 tile 预取、
  FIFO full pop/push、PE busy 禁止消费下一 tile、producer 反压和周期 output stall；
- 新 streaming 路径、Step 1/2/3、旧融合模型、旧 Im2Col、地址 mapper 和 Scratchpad
  合并72项 C++ 测试全部通过；ASan/UBSan 下同样72项通过；Python streaming contract
  在 launch 相位细化后与 `util/conv_pipeline` 全部55项回归通过；
- 未修改 `SConscript`，未新增 SimObject 或运行入口，未主动编译 gem5。Step 5 开始前
  仍需按计划再次说明构建注册、trace/stats 和入口影响并取得用户确认。

### Im2Col 流水化 Step 5 完成（2026-07-21）

- `PipelinedIm2ColStats` 新增 pipeline fill、producer/output 相邻 pair 与 gap，以及
  合格无冲突相邻 pair/gap/max-gap 的整数统计；合格 pair 要求相邻两个 vector 都只需
  一轮读取，且两次 push 之间没有 S2/FIFO 下游反压；少于一个 pair 时平均值安全记 0；
- streaming 融合模型补齐第 13 节规定的 bank conflict、stage/downstream stall、FIFO
  occupancy/push/pop、PE launch/input/bubble/busy、tile 和 output 守恒统计；drained 时
  继续检查全部派生数量；
- 新增 `streaming_conv_pipeline_io.hh/.cc`，默认写 54 字段精简逐拍控制 trace；显式
  `detailed_pe_trace` 才追加 6 个规范化 PE mask/vector snapshot 字段，最终 drained 行会
  flush；探索 trace 与旧 53 字段 RTL strict schema 完全隔离；
- 新增 `StreamingConvPipelineTiming` ClockedObject 和 `StreamingConvPipeline.py`，注册
  `streamingPipeline.*` 全部统计，在 drained 时写 NCHW output 并以
  `streaming conv pipeline drained` 退出；
- 抽取旧 loader 的共享严格字段解析，同时保持旧 spatial canonical-prefix 校验不变；
  新增 streaming 专用 loader/config，允许 W6/stride2 scattered raw mask，但只接受计划
  冻结的 3x3、dilation1、stride/padding 范围；
- 新增独立 `configs/example/streaming_conv_pipeline_timing.py`，默认输出到
  `<outdir>/streaming_conv_pipeline/{trace,output}.csv`，并提供
  `--detailed-pe-trace`；旧运行入口未修改；
- 经用户确认，`SConscript` 已登记新 SimObject、source 和 GTest；README 已记录入口、
  trace 模式、stats 口径以及组合 Scratchpad/理想 weight 和未做 RTL 逐拍验证的假设；
- 当前源码的 81 项普通 C++ 测试和 81 项 ASan/UBSan 测试全部通过；
  `util/conv_pipeline` 的 59 项 Python 测试全部通过；timing 包装使用现有
  `build/RISCV` 生成头和参数桩完成 C++17 `-fsyntax-only` 检查；Python/SConscript AST、
  行长和 `git diff --check` 作为最终收尾检查执行；
- 按目录规则由开发者手动完成增量编译；新 `gem5.opt` SHA256 为
  `d38ad1b32b3eba3edcb827445c181b35f0d1c803c5b4da66696ac0c660dc3fa2`，binary 报告编译
  时间为 2026-07-21 23:02:30；
- 使用目标 fixture 08 和默认 output ready 运行新入口，artifact 保存到
  `m5out/im2col_pipeline_step5_streaming_20260721/`；模型以
  `streaming conv pipeline drained` 正常退出，`drainedCycle=6243`、
  `totalCycles=6244`、`pipelineFillCycles=4`；
- 4608 个 producer input/output、FIFO push/pop 和 PE input，32 个 generated/launched/
  completed tile，以及 8192 个 NCHW output 全部满足守恒；output SHA256 为
  `9eeda13ea3af2596815247691a4a74588b7e04795ccd1521b08b636037870892`，与 Step 0 正式
  基线逐字节一致；
- 目标 workload 无 bank conflict；`conflictFreeOutputPairs=4576`、
  `conflictFreeOutputGapCycles=4576`、`conflictFreeOutputMaxGap=1`，以整数条件确认合格
  无冲突稳态 II=1；默认 trace 为 54 字段，包含 header 和 cycle 0..6243 共 6245 行；
- 使用 fixture 01 和 `--detailed-pe-trace` 完成运行时冒烟，artifact 保存到
  `m5out/im2col_pipeline_step5_detailed_smoke_20260721/`；模型在 cycle 50 drained，详细
  trace 为预期 60 字段，9 个 vector、1 个 tile 和 3 个 output 全部守恒；
- 新 binary 下重新执行 `cd tests && ./main.py run --skip-build gem5/conv_pipeline`，旧融合
  路径 7 个 suite 共 21 项 RTL strict regression 全部通过；
- Step 5 的源码、构建、独立入口、两种 trace 模式、stats/output 守恒和旧路径隔离验收
  已完成。Step 6 完整功能/冲突矩阵和 Step 7 正式性能报告尚未开始。

### Im2Col 流水化 Step 6 完成（2026-07-21）

- 新增 `util/conv_pipeline/verify_streaming_pipeline.py`，严格解析 54 字段 compact trace、
  `streamingPipeline.*` stats 和 NCHW output；逐拍检查 schema/hash/cycle、tile/K 顺序、
  producer/S1/S2/FIFO/PE handshake 数量、FIFO 深度、launch/output/drained 守恒；
- verifier 不调用 C++ 或 Python compaction helper，而是从 W、outH/outW、rows-per-word
  和 tile 顺序独立推导每个 raw source lane、coordinate、raw mask、compacted prefix mask
  及 source-lane vector；W6/stride2 精确得到 `[0,1,2,6,7,8] -> [0..5]`，W5/pad0
  精确得到 `[0,1,2,5,6,7,10,11,12] -> [0..8]`；
- 所有 profile 的 output 都与独立 direct-NCHW convolution oracle 逐项比较；W1、W5、
  W16、N2/W20/stride2、C63 和周期 output-backpressure 六个兼容 profile 还与既有 strict
  路径冻结 output 逐字节一致；
- 新增 streaming 专用 W5/pad0、W6/stride2 scattered、W17 tail/OC7 和
  W32/stride2 conflict fixture；与六个既有 profile 及目标 fixture 08 组成 11-profile
  gem5 矩阵，覆盖 W1/W5/W6/W16/W17/W20/W32、padding 0/1、stride 1/2、C63、
  OC1/7/15/16、N2、宽度尾部和 packing；
- `cd tests && ./main.py run --skip-build gem5/streaming_conv_pipeline` 共 11 个 suite、
  33 项全部通过；W6/stride2 观测到 12 个 conflict vector、12 个 extra round、18 个
  scattered vector 和 10 个 PE bubble；W32/stride2 为 42/42 conflict/extra round、
  22 个 bubble；N2/W20/stride2 为 168/168 conflict/extra round、124 个 bubble；
- trace 定向验证实际覆盖满 FIFO 同拍 pop/push、PE input bubble 和 DRAIN_OUTPUT 期间
  周期 output grant 反压；目标 W32/stride1 profile 继续得到 6244 total cycles、无 bank
  conflict 和 FIFO peak 4；
- 重新执行旧融合路径 7 个 suite 共 21 项 RTL strict regression，既有 53 字段 golden
  trace 和 NCHW output 全部通过；没有修改旧 schema、golden 或入口；
- 最终从当前源码重新验证：81 项普通 C++、81 项 ASan/UBSan、62 项 Python、33 项
  streaming gem5 和 21 项旧 RTL strict 测试全部通过；`git diff --check` 和 Python AST/
  行长检查作为最终静态收尾执行；
- Step 6 功能和冲突回归完成。下一步 Step 7 将对目标 workload 输出正式新旧周期、冲突、
  FIFO、PE bubble 和 tile 间停顿分解报告。

### Im2Col 流水化 Step 7 完成（2026-07-21）

- 使用 binary SHA256
  `d38ad1b32b3eba3edcb827445c181b35f0d1c803c5b4da66696ac0c660dc3fa2` 和目标
  fixture 08 重新运行正式性能验收；artifact 独立保存到
  `m5out/im2col_pipeline_step7_performance_20260721/`，未覆盖 Step 0/5 证据；
- Step 6 独立 verifier 再次通过：54 字段 compact trace 包含 cycle 0..6243，4608 个
  producer/S1/S2/FIFO/PE vector、32 个 generated/launched/completed tile、512 个 output
  row 和 8192 个 NCHW output 全部守恒；
- 新 output 通过 direct-NCHW convolution oracle，并与 Step 0 正式基线逐字节一致；两者
  SHA256 都是 `9eeda13ea3af2596815247691a4a74588b7e04795ccd1521b08b636037870892`；
- `totalCycles=6244`，相对旧基线 28378 节省 22134 拍、减少 77.997%，等效周期加速
  4.545 倍；end-to-end vector rate 从 0.162379 提升为 0.737988 vector/cycle；
- `bankConflictVectors=bankConflictExtraRounds=bankConflictStallCycles=0`；
  `conflictFreeOutputPairs=4576`、gap sum=4576、max gap=1，以整数条件确认目标 workload
  的合格无冲突稳态 II=1；
- 精确周期闭合为 `6 startup + 4608 PE input + 31*51 inter-tile + 49 final drain =
  6244`；每个 tile 的 144 个输入连续、`peInputBubbleCycles=0`；每个 tile 输入结束后固定
  33 拍 WAIT_RESULT 和 16 拍 DRAIN_OUTPUT，非末 tile 再加 1 拍 IDLE/begin-launch 和
  1 拍 LAUNCH；
- producer 跨 tile 边界连续预取；4607 个相邻 output pair 的 gap sum 为 6187，超过理想
  值的 1580 拍与 S0/S1/S2/downstream stall 全部精确相等；
- FIFO 平均 occupancy=3.941063、peak=4；6041 个满周期由 4460 个满队列同拍 pop/push、
  1580 个 S2 真正下游 stall 和 1 个 producer exhausted 后仅 pop 周期组成；
  `peBusyNotAcceptingCycles=1568=32*(33+16)`；
- 正式报告、运行命令、模型假设、分解和 artifact SHA256 已写入
  `m5out/im2col_pipeline_step7_performance_20260721/PERFORMANCE_REPORT.md`。Step 7 的
  `<12000`、功能、II、守恒、输出一致和瓶颈解释要求全部通过。
