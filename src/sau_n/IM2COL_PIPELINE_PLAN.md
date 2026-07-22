# Gem5 Im2Col 流水化与直接流式送入 PE 实施计划

## 1. 文档定位

本计划用于在 gem5 中探索新的 Im2Col 微架构，解决当前融合流水线中 Im2Col
输出过慢的问题，并尽可能实现：

```text
无 bank 冲突且无下游反压时，每拍产生并交付一组 16-lane activation。
```

本阶段只修改 gem5 模型，不修改 RTL。新模型是架构探索模型，不得声明与现有或
未来 RTL 逐拍一致。现有已经通过 RTL 严格逐拍验证的 `Im2ColModel`、
`ConvPipelineModel`、golden trace 和运行入口必须保留，不能用探索模型覆盖。

如果探索结果满足本计划的功能和性能验收条件，后续独立阶段再实现 RTL、生成新
RTL golden，并据此校准周期模型。

## 2. 问题与基线

当前融合模型的数据流是：

```text
Scratchpad
  -> Im2Col ISSUE/COLLECT/PUSH/NEXT
  -> 收满完整 K 项 tile buffer
  -> 连续 K 拍送入 PE
  -> 等待、加 bias、排出结果
  -> 收集下一 tile
```

验收 fixture 为
`tests/gem5/conv_pipeline/fixtures/08_n1_c16_h16_w32_oc16.json`，当前统计为：

```text
totalCycles                 = 28378
collectTileCycles           = 22169
nonCollectCycles            = 6209
tiles                       = 32
activationHandshakes        = 4608
engineInputCycles           = 4608
im2colBackpressureCycles    = 5891
```

每个空间 tile 的归约长度及整个 workload 的 vector 数为：

```text
K = C * KH * KW = 16 * 3 * 3 = 144
expectedVectors = 32 * 144 = 4608
```

当前 `CollectTile` 阶段平均约为：

```text
22169 / 4608 = 4.81 拍/组
```

以上数值已在 Step 0 使用当前源码重新构建的 binary 精确复现并正式冻结。运行命令、
构建来源、resolved config、原始 `stats.txt/output.csv/trace.csv` 和 SHA256 清单位于：

```text
m5out/im2col_pipeline_step0_formal_20260721/
```

后续加速比必须以该目录的正式 artifact 为基线，不能仅引用本文摘录值。

目标 workload 的主要问题不是物理 bank 冲突，而是旧六态控制、旧状态可见性、
完整 tile 收集以及前后端串行造成的固定控制开销和反压。

## 3. 第一版目标

### 3.1 功能目标

- 新增独立的 `PipelinedIm2ColModel`；
- 新增直接连接 PE 的 `StreamingConvPipelineModel`；
- 新增独立的 `StreamingConvPipelineTiming` 和运行入口；
- 用三级弹性流水替代每个 vector 重复执行的
  `ISSUE -> COLLECT -> PUSH -> NEXT`；
- 删除新探索路径中的完整 `K` 项 activation tile buffer；
- 在 Im2Col 和 PE 之间只保留深度为 4 的弹性 FIFO；
- S2 对 raw Im2Col spatial lane 做稳定的升序 compaction，使 PE row mask 始终为
  canonical prefix，同时保存 compacted row 到原始 lane/NCHW 坐标的映射；
- PE 输入支持气泡，只在有效握手时注入新 token、调度对应 MAC 和推进 K；更早
  token 的到期 MAC 仍按实际周期 commit；
- `SauCycleModel` 增加显式输入协议模式：旧入口固定使用默认的 RTL 严格连续模式，
  新探索入口显式使用允许气泡的 streaming 模式；
- stride 1 无冲突路径以 II=1 为目标；
- stride 2 遇到同 bank 不同 row 时保证正确，并使用单端口 bank 所允许的尽可能少
  的读取轮数；
- 数值结果与当前模型及独立卷积 oracle 逐字节一致。

### 3.2 性能目标

目标 fixture、默认每拍 output ready 的条件下，必须同时满足：

```text
totalCycles < 12000
conflictFreeOutputII = 1
```

`totalCycles` 采用 cycle 0 到 drained cycle 的 inclusive 口径。

目标 workload 的预算不是验收 magic cycle，而是用于证明门槛具有合理余量：

```text
每 tile 约为：launch 1 + K 144 + 末输入后 drain 33
             + 输出 16 + calFinish/切换 1..2
           = 195..196 拍
32 tiles   = 6240..6272 拍
```

再加首次 producer 流水填充和边界状态切换，预期仍明显低于 12000 拍。最终结果只能
由通用模型和实际统计得到，不得把上述估算写成 fixture 专用周期表。

### 3.3 非目标

第一版不实现：

- RTL 修改或新的 RTL golden；
- OC 大于 16 的分组、activation 回放或重新生成；
- 多端口 bank、Scratchpad 复制、增加 bank 数或新的 bank swizzle；
- 多个并发 gather slot、bank 间乱序调度或 reorder buffer；
- PE 双累加上下文或 tile 间完全无缝切换；
- 同步 SRAM 延迟参数；
- DMA、DRAM、gem5 memory port 或 scratchpad preload 时序；
- weight memory 的读取时序和带宽竞争；
- 非对称 stride 或非对称 padding；
- 3x3 之外的 kernel；
- dilation 1 之外的配置。

## 4. 支持范围和拒绝规则

新探索入口只接受：

```text
N/H/W：沿用现有字段范围和 checked arithmetic
C：1..63，且 K=C*9 <= 567
OC：1..16
kernel_h = kernel_w = 3
stride_h = stride_w，且 stride 只能为 1 或 2
pad_top = pad_left，且 padding 只能为 0 或 1
dilation_h = dilation_w = 1
```

同时保留现有约束：

- resolved `out_h/out_w` 必须合法；
- `W <= 16` 时保留当前 `out_w <= W` 限制；
- scratchpad footprint 不得超过每 bank 4096 row；
- 所有计数和连乘使用 checked `uint64_t`；
- raw Im2Col spatial mask 允许 scattered；S2 必须按原始 lane 升序压紧，压紧后的 PE
  spatial mask 必须是从 row 0 开始的 canonical prefix；
- compaction 必须保持每个有效 raw lane 到唯一 NCHW 坐标的一一映射，不得丢失、
  重复或改变空间顺序。

范围外配置必须在启动前明确拒绝，不能静默降级、截断、回绕或自动切回旧模型。
旧运行入口和旧 pipeline fixture loader 继续保留原有支持范围及 scattered mask 拒绝
行为。新 streaming 入口复用字段解析和 resolved config 类型，但使用独立的
compaction-aware spatial mapping 校验，不能通过修改旧 loader 放宽冻结路径。

## 5. 固定建模假设

第一版固定：

```text
lanes / PE rows       = 16
PE columns            = 16
Scratchpad banks      = 16
ports per bank        = 1 read/cycle
Scratchpad response   = combinational
elastic FIFO depth    = 4
clock                  = 100 MHz
```

权重侧沿用当前确定性 weight generator，并假设每次 PE 输入握手都能提供当前
`(C, KH, KW)` 对应的最多 16 个 weight。本计划得到的周期结果不包含真实 weight
SRAM、DMA 或端口冲突。

组合 Scratchpad 响应与当前 readable reference RTL 的已验证契约一致。真实 RTL
采用何种 SRAM macro 留到探索方案被接受后的 RTL 设计阶段确认。

## 6. 总体数据流

```text
预加载的 16-bank Scratchpad
          |
          v
PipelinedIm2ColModel: S0 -> S1 -> S2
                            raw lane compaction
          |
          v
深度 4 elastic FIFO
          |
          v
支持 valid/ready 和输入气泡的 16x16 PE
          |
          v
bias / quantize / output collector
```

完整 activation tile buffer 不存在。Im2Col 完成当前 tile 的最后一个 K 后，可以
立即生成下一 tile，并在 PE 完成当前 tile 期间向深度 4 FIFO 预放最多 4 项。FIFO
满后反压逐级传回 S2、S1 和 S0。

compaction 是 S2 内的组合 lane 重排，不增加新的流水级。S1 的 bank 请求、冲突轮数
和最少读取轮数始终按 raw Im2Col lane/address 计算；不能对 compacted row 重新计算
bank 冲突。

## 7. Im2Col 三级弹性流水契约

### 7.1 S0：坐标和标签生成

S0 每次接受一个逻辑 vector，固定一个空间 tile 和一个 `(C, KH, KW)`：

- 根据 tile metadata 取得最多 16 个 `(n, oh, ow)`；
- 计算每 lane 的输入坐标；
- 区分 SRAM read、padding zero 和无效尾 lane；
- 生成地址映射字段和完整控制标签；
- 当前 vector 被 S1 接受时才推进迭代器。

坐标公式为：

```text
IH = OH * stride + KH - padding
IW = OW * stride + KW - padding
```

C++ 实现不得直接用无符号整数执行上述减法。必须先计算：

```text
padded_h = OH * stride + KH
padded_w = OW * stride + KW
```

先比较 `padded_h/padded_w` 与 padding 和输入边界，确认不是 padding 后再执行减法，
与现有 `Im2ColModel::buildIssuedVector()` 的下溢安全顺序保持一致。

### 7.2 S1：地址映射、bank 仲裁和分轮收集

S1 保存当前 vector 的 16 个 lane 请求、`lane_done`、已收集 activation、每 bank
本拍选择的 row、冲突轮数和控制标签。

- 每个 bank 每拍最多发出一个 row 请求；
- 同 bank、同 row 的多个 lane 使用一次响应广播完成；
- 同 bank、不同 row 按 destination lane 升序分拍收集；
- padding lane 直接得到数据 0，但保留空间有效 mask；
- 无效尾 lane 得到数据 0 且空间 mask 为 0；
- 所有 lane 完成后，vector 才能进入 S2；
- 冲突期间 S1 保持当前 vector，并通过 `ready=0` 暂停 S0；
- 第一版不允许后续 vector 绕过发生冲突的 vector。

无冲突 vector 只占用一个 S1 读取周期。不能因旧状态可见性再增加空的
`all_done` 检查周期。

### 7.3 S2：lane compaction、组装和 FIFO push

S2 接收 S1 完成的 raw 16-lane activation、raw spatial mask 和每 lane 的 NCHW
坐标。它按 raw lane 0 到 15 的升序，把所有 `raw_spatial_mask=1` 的 lane 稳定压紧到
PE row 0 到 `valid_rows-1`：

```text
dst = 0
for src in 0..15:
    if raw_spatial_mask[src]:
        activation[dst] = raw_activation[src]
        source_lane[dst] = src
        coordinate[dst] = raw_coordinate[src]
        dst++
spatial_mask = (1 << valid_rows) - 1
```

padding zero lane 的 raw mask 仍为 1，因此必须参与 compaction；无效尾 lane 不参与。
S2 将 compacted activation、prefix spatial mask、source-lane/coordinate mapping 和
标签写入深度 4 FIFO。发生下游反压时，raw/compacted payload、映射和标签都必须
保持稳定，直到 push handshake 成功。compaction 不改变 vector、tile 或 K 顺序。

### 7.4 弹性推进规则

每一级都使用 `valid/ready`。数据只在下式成立时向后推进：

```text
fire = valid && ready
```

发生 stall 时，该级的 payload、标签和完成位必须保持不变。无冲突且 FIFO 可接收
时，流水填满后必须允许 S0、S1、S2 中的三个相邻 vector 同拍向前推进。

第一版冻结以下组合关系，避免实现中重新产生额外空拍：

```text
fifo_push_ready = (fifo_count < 4) || fifo_pop
s2_ready        = !s2_valid || fifo_push_ready
s1_can_retire   = s1_valid && current_read_round_completes_all_lanes
s1_ready        = !s1_valid || (s1_can_retire && s2_ready)
s0_ready        = !s0_valid || s1_ready
producer_ready  = s0_ready && more_vectors_to_generate
```

其中 `s1_can_retire` 必须组合地包含本拍 Scratchpad response 的完成结果，不能等待
下一拍再检查旧的 `lane_done`。流水填满、无冲突且无反压时，同一拍必须允许：

```text
S2 -> FIFO
S1 -> S2
S0 -> S1
新 vector -> S0
```

寄存器提交仍统一发生在拍末；同拍多级 fire 不能让新 payload 组合穿透多个寄存器级。

## 8. Vector 顺序和 FIFO 条目

迭代器保持现有 canonical 顺序：

```text
KW 最快 -> KH -> C -> W group / H group -> N
```

一个空间 tile 的 `K=C*9` 项连续出现：

```text
k_index = c * 9 + kh * 3 + kw
```

FIFO 每项至少包含：

```text
activation[16]
spatial_mask
source_lane[16]    # compacted PE row -> raw Im2Col lane，无效项为 canonical 0
coordinate[16]     # compacted PE row -> (n, oh, ow)，无效项为 canonical invalid
tile_index
oc_group          # 第一版固定为 0
valid_columns     # 等于 OC，范围 1..16
c
kh
kw
k_index
tile_first
tile_last
```

必须检查：

- 同一 tile 的 `k_index` 从 0 连续到 `K-1`；
- `tile_first` 当且仅当 `k_index==0`；
- `tile_last` 当且仅当 `k_index==K-1`；
- 同一 tile 的 spatial mask 和 metadata 不变；
- 同一 tile 的 source-lane/coordinate mapping 不变且覆盖该 tile 的全部空间输出一次；
- `spatial_mask` 是 compacted PE prefix mask；允许对应的 raw Im2Col mask scattered；
- FIFO 中允许同时存在当前 tile 尾项和下一 tile 首项；
- PE 不得在当前 tile 完成前消费下一 tile。

## 9. 深度 4 FIFO 契约

FIFO 是弹性解耦缓冲，不是 tile 回放存储。

- 容量固定为 4；
- head 只有在 PE `input_fire` 时 pop；
- S2 只有在 `push_ready` 时 push；
- FIFO 满且同拍 pop 时必须允许同拍 push：

```text
push_ready = (count < 4) || pop
```

- FIFO 空且同拍 push 时不要求组合旁路，数据可在下一拍被 PE 看到；
- push/pop 同拍时计数保持不变；
- 每拍检查 `0 <= count <= 4` 和 push/pop 守恒。

## 10. PE 输入气泡和握手协议

### 10.0 旧模型隔离

`SauCycleModel` 增加显式的输入协议模式：

```text
StrictRtlContinuous    # 默认值，旧 ConvPipelineModel 固定使用
ElasticBubbleEnabled  # 仅新 StreamingConvPipelineModel 显式使用
```

严格模式保持当前全部行为，包括 K 项输入开始后出现气泡时抛出异常。弹性模式只放宽
输入气泡，不改变连续输入时的状态、MAC 延迟、completion、bias、输出和
`calFinish` 周期。旧入口不得通过运行参数切换到弹性模式。

### 10.1 Tile 启动

FIFO 中出现一个新 tile 的第一项后，pipeline 立即向空闲 PE 发出 launch，不等待
FIFO 填满 4 项。launch 与第一项输入保持为两个独立周期，兼容当前配置锁存方式。

consumer 状态转换固定为：

```text
IDLE --FIFO head.tile_first--> LAUNCH
LAUNCH --本拍只锁存配置--> ACCEPT_K
ACCEPT_K --接受第 K 项--> WAIT_RESULT
WAIT_RESULT --结果可输出--> DRAIN_OUTPUT
DRAIN_OUTPUT --calFinish--> IDLE
```

### 10.2 输入握手

```text
pe_ready    = consumer_state == ACCEPT_K && accepted_k < K
input_valid = FIFO 非空
           && FIFO head.tile_index == active_tile
           && FIFO head.k_index == accepted_k
input_fire  = input_valid && pe_ready
```

第一版 PE 在 `ACCEPT_K` 内没有其他内部反压，因此 `pe_ready` 只由上述 consumer
状态产生。FIFO 为空时形成输入气泡；FIFO head tag 不匹配不是正常 stall，必须立即
报错。传给弹性 `SauCycleModel` 的 `inputValid` 等于 `input_fire`，而不是未经 ready
筛选的上游 `input_valid`。

只有 `input_fire` 时才：

- pop FIFO；
- 读取该项 `(C, KH, KW)` 对应的 weight；
- 调度 16x16 有效 PE 的 MAC；
- `accepted_k++`；
- 更新最后接受的 `k_index`。

必须使用同一个 `input_fire` 同时控制 FIFO pop、weight 选择、SA 输入、MAC 调度和
`accepted_k`，禁止先 pop 后由 PE 拒绝，也禁止 PE 接受而 FIFO 不 pop。

无 `input_fire` 时，不注入新的 activation/weight token，不增加 `accepted_k`，也不为
该气泡调度新的 MAC；上游 FIFO head 和 consumer 标签保持不变，并且不再抛出
“输入流包含气泡”的异常。已经由更早输入调度、正在阵列内部传播的 token 不得冻结：
它们仍可在该气泡所在的墙钟周期产生 `macCommit` 并更新 PE activation、weight 和
accumulator。

只有接受第 K 项后才能调度 completion、bias 和输出。MAC 的 row/column skew 延迟
从每项实际 `input_fire` 周期开始计算，因此输入气泡会自然传播为空的阵列时隙，
不得被压缩成连续的虚假 MAC。

### 10.3 Tile 完成和切换

接受第 K 项后，PE 仍需完成：

```text
末尾 MAC 传播 -> bias -> 结果 ready -> 逐行输出 -> calFinish -> IDLE
```

此期间 PE 不接收下一 tile。Im2Col 可以继续生成下一 tile，FIFO 满后自动反压。
PE 回到 IDLE 且 FIFO head 是下一 tile 的 `tile_first` 后立即 launch 下一 tile。
第一版不实现双累加上下文，因此 tile 之间允许停顿。

`peInputBubbleCycles` 从 LAUNCH 完成、consumer 进入 `ACCEPT_K` 后开始统计；LAUNCH
这个强制配置周期本身不算输入气泡。

## 11. Pipeline 控制和 drained

新 `StreamingConvPipelineModel` 不再用旧的
`CollectTile -> LaunchSa -> StreamK -> WaitResult -> DrainOutput` 串行状态作为唯一
全局控制，而是拆成：

1. Im2Col producer：推进 S0/S1/S2 和 tile/K 迭代器；
2. PE consumer：`IDLE/LAUNCH/ACCEPT_K/WAIT_RESULT/DRAIN_OUTPUT`；
3. FIFO：独立处理 push/pop 和反压。

三个控制器每拍统一执行 old-state -> combinational decision -> next-state -> commit，
不能依赖 C++ 语句顺序模拟硬件寄存器提交。

drained 必须同时满足：

- Im2Col 已生成全部预期 vector；
- S0/S1/S2 均无有效项；
- FIFO 为空；
- PE 为 IDLE；
- 所有 tile 完成；
- 所有 NCHW output 恰好写入一次。

## 12. Bank 冲突行为

### 12.1 Stride 1

验收 workload `W=32, stride=1` 的满 16-lane 横向 tile 对固定
`(C, KH, KW)` 形成 16 个不同 bank 请求。即使地址跨越两个 Scratchpad row，只要
每个 bank 只对应一个 row，仍是一拍完成，不记为 conflict。

### 12.2 Stride 2

`W=32, stride=2` 的典型满 tile 可能产生：

```text
bank 0,2,4,...,14 对 row0
bank 0,2,4,...,14 对 row1
```

S1 必须用两轮完成，不能丢弃后八个 lane，也不能用一份响应代替不同 row。

`W=6, stride=2` 使用自动推导的自然输出尺寸，用于同时覆盖 `W<16` 多行 packing
下的纵向重复 bank group和 scattered raw spatial mask。S1 先按 raw lane 完成 bank
仲裁，S2 再按第 7.3 节压紧到 prefix PE rows；测试必须分别检查 raw bank 轮数和
compacted NCHW mapping。因为本计划不支持非对称 stride，这两个测试都使用：

```text
stride_h = stride_w = 2
```

stride 2 不设 II=1 或固定总周期门槛，但必须使用当前单端口 bank 所允许的最少读取
轮数，保证数值正确、无丢失、无重复、无乱序，并输出冲突轮数和平均 vector 间隔。

每个 vector 的理论最少读取轮数固定定义为：

```text
required_read_rounds = max(1, max_b distinct_requested_rows_in_bank[b])
```

padding 和无效 lane 不产生 row 请求；同 bank 同 row 只计一个 distinct row。S1 的
实际读取轮数必须严格等于 `required_read_rounds`，contract test 直接逐 vector
检查该等式，而不是只检查最终输出正确。

## 13. 统计定义

新对象至少提供：

```text
totalCycles
drainedCycle
pipelineInputVectors
pipelineOutputVectors
pipelineFillCycles
producerInitiationInterval
producerInputPairs
producerInputGapCycles
im2colOutputInterval
im2colOutputPairs
im2colOutputGapCycles
conflictFreeOutputII
conflictFreeOutputPairs
conflictFreeOutputGapCycles
conflictFreeOutputMaxGap
endToEndVectorRate
bankConflictVectors
bankConflictExtraRounds
bankConflictStallCycles
rawScatteredMaskVectors
compactedSpatialVectors
downstreamStallCycles
s0StallCycles
s1StallCycles
s2StallCycles
fifoAverageOccupancy
fifoPeakOccupancy
fifoFullCycles
fifoPushes
fifoPops
peLaunches
peInputCycles
peInputBubbleCycles
peBusyNotAcceptingCycles
tilesGenerated
tilesLaunched
tilesCompleted
outputRows
outputElements
```

口径固定如下：

- `totalCycles = drainedCycle + 1`，cycle 从 0 开始且包含 drained cycle；
- `pipelineInputVectors`：`新 vector -> S0` 的 fire 次数；
- `pipelineOutputVectors`：`S2 -> FIFO` 的 push 次数；
- `pipelineFillCycles`：cycle 0 到第一次 `S2 -> FIFO` fire 的 inclusive 周期数；没有
  输出时为 0；
- `producerInitiationInterval`：按连续逻辑 vector 的 S0 接收 cycle，计算相邻 cycle
  差的算术平均；原始整数分子/分母分别保存为 `producerInputGapCycles` 和
  `producerInputPairs`；
- `im2colOutputInterval`：按连续逻辑 vector 的 FIFO push cycle，计算相邻 cycle 差的
  算术平均；原始整数分子/分母分别保存为 `im2colOutputGapCycles` 和
  `im2colOutputPairs`；
- `conflictFreeOutputPairs`：两个连续逻辑 vector 均满足无 bank conflict，且从前一项
  push 后到后一项 push（不含后一项 push 周期）没有 S2/FIFO 下游反压的相邻 pair
  数；允许跨 tile，但不允许跳过中间 vector 重新配对；
- `conflictFreeOutputGapCycles`：上述所有 pair 的 push cycle 差之和；
- `conflictFreeOutputMaxGap`：上述所有 pair 的最大 push cycle 差；
- `conflictFreeOutputII = conflictFreeOutputGapCycles /
  conflictFreeOutputPairs`；样本不足一个 pair 时记为 0，并由验收单独判定样本数；
- `endToEndVectorRate = fifoPops / totalCycles`；
- `bankConflictExtraRounds`：每个 vector 超过第一轮的实际 S1 读取轮数之和；
- `bankConflictStallCycles`：S1 valid、当前 vector 尚需额外读取轮且因此不能 retire 的
  周期；
- `rawScatteredMaskVectors`：进入 S2 的 raw spatial mask 非零且不是 canonical prefix
  的 vector 数；
- `compactedSpatialVectors`：完成 S2 compaction 并成功 push 到 FIFO 的 vector 数；
- `downstreamStallCycles`：S2 valid 且 `fifo_push_ready=0` 的周期；
- `s0StallCycles`：S0 valid 且 `s1_ready=0` 的周期；
- `s1StallCycles`：S1 valid 且本拍不能向 S2 retire 的周期；它包含 bank conflict 和
  S2 反压造成的 stall，不要求与其他 stall 项互斥；
- `s2StallCycles`：S2 valid 且 `fifo_push_ready=0` 的周期，数值应与
  `downstreamStallCycles` 相同；
- `peInputBubbleCycles`：consumer 位于 `ACCEPT_K`、尚未接受完 K 项且本拍没有
  `input_fire` 的周期；
- `peBusyNotAcceptingCycles`：consumer 位于 `WAIT_RESULT/DRAIN_OUTPUT` 的周期；
- 所有平均值必须处理少于两个样本的情况，不能除零。

`conflictFreeOutputII=1` 的验收不使用浮点近似，必须同时满足：

```text
conflictFreeOutputPairs > 0
conflictFreeOutputGapCycles == conflictFreeOutputPairs
conflictFreeOutputMaxGap == 1
```

drained 时必须满足：

```text
pipelineInputVectors  = expectedVectors
pipelineOutputVectors = expectedVectors
fifoPushes            = expectedVectors
fifoPops              = expectedVectors
compactedSpatialVectors = expectedVectors
peInputCycles         = expectedTiles * K
tilesGenerated        = expectedTiles
tilesLaunched         = expectedTiles
tilesCompleted        = expectedTiles
outputElements        = N * OC * outH * outW
```

## 14. Trace

默认精简控制 trace 至少包含：

```text
cycle
S0/S1/S2 valid-ready-fire
tile_index/c/kh/kw/k_index
S1 lane_done/request banks/conflict round/raw spatial mask
S2 compacted spatial mask/source-lane mapping
FIFO count/push/pop/head tile/head k
PE state/ready/launch/input_valid/input_fire/accepted_k
output ready/fire/row sequence
drained
```

详细的 256 PE activation、weight、MAC commit 和 accumulator trace 使用独立可选
开关，默认关闭。探索 trace 不与旧 RTL trace 比较，也不得放入现有 RTL golden
目录。

## 15. 预计文件影响

具体文件名允许在实施时根据现有命名小幅调整，但范围限制为：

- 新增 `pipelined_im2col_model.hh/.cc` 及测试；
- 新增 `streaming_conv_pipeline_model.hh/.cc` 及测试；
- 新增 streaming 专用 compaction-aware spatial mapping/fixture 校验；旧 pipeline
  loader 和旧 canonical-prefix 拒绝规则保持不变；
- 新增 streaming trace/stats ClockedObject 包装；
- 扩展 `sau_model.hh/.cc` 增加显式输入协议模式；默认严格模式保持当前异常和周期
  行为，新探索入口才启用弹性模式；
- 新增独立 SimObject 参数描述和 Python 运行入口；
- 为探索入口增加严格配置校验；
- 在 `SConscript` 中登记新文件和测试；
- 新增 stride 1/2 和目标 workload 测试；
- 更新 `README.md` 和 `STATUS.md`。

不得修改 reference Im2Col RTL、项目自有 16x16 SA RTL、现有 RTL golden
trace/manifest、旧模型 trace schema 和已冻结运行入口行为。

实施到 `SConscript` 或构建入口修改前，需要再次向用户说明影响并获得确认。根据
本目录 `AGENTS.md`，agent 不主动编译 gem5；源码完成后由开发者手动增量构建。

## 16. 实施步骤

### Step 0：冻结旧基线

- 保存目标 fixture、resolved config、canonical SHA256 和当前 stats；
- 固定基线 `totalCycles=28378`、`collectTileCycles=22169`；
- 保存实际运行命令、worktree commit/status、`gem5.opt` 构建来源或可识别时间；
- 保存原始 `stats.txt`、`output.csv` 和必要日志，不只摘录数值；
- 保存 fixture、resolved config、`stats.txt`、`output.csv` 的 SHA256；
- 复核旧模型最小功能测试；
- 确认工作树现有修改，避免覆盖用户内容。

Step 0 状态：已完成。正式基线精确复现第 2 节数值，目标 output 通过独立 oracle，
53字段 trace通过严格校验，旧融合路径7个suite共21项RTL strict regression全部通过。

### Step 1：定义流水和握手契约

- 定义 S0/S1/S2 payload、valid/ready/fire；
- 定义 FIFO 同拍 push/pop；
- 定义 vector 标签、tile/K 顺序和守恒关系；
- 定义 raw scattered mask、稳定 lane compaction 和 PE prefix-row mapping；
- 定义 PE launch、input_fire、bubble 和 tile 完成协议；
- 定义 strict/elastic SA 输入协议隔离，并确认旧入口只能使用 strict 模式；
- 用纯 contract test 冻结无冲突、冲突和反压时序。

Step 1 状态：已完成。新增独立 C++ contract 与 Python oracle，冻结 S0/S1/S2/FIFO
payload、valid/ready/fire、scattered raw lane compaction、tile/K 顺序、drained 守恒、
consumer 和 strict/elastic 气泡语义；两侧各 13 项定向测试通过。尚未登记 SConscript，
未修改既有模型或旧入口。

### Step 2：实现 `PipelinedIm2ColModel`

- 复用现有配置类型、地址 mapper 和 `BankedScratchpad`；
- 实现 S0 坐标生成；
- 实现 S1 单端口 bank 仲裁、广播和分轮收集；
- 实现 S2 保持和 FIFO push；
- 在 S2 实现不增加流水级的稳定 lane compaction，并保存 raw lane/NCHW 映射；
- 移除旧 FSM 每 vector 的固定控制空拍；
- 加入迭代器和 lane/vector 守恒断言。

Step 2 状态：已完成。新增独立 `PipelinedIm2ColModel`，复用现有地址 mapper 和
`BankedScratchpad`，实现三级寄存器流水、组合完成的 S1 单端口最少轮次仲裁、S2
稳定 compaction/保持以及 FIFO push ready/fire 接口；加入逐级 payload、tile/K、
lane/vector 和 drained 不变量。全 padding/零 SRAM 请求 vector 按冻结的
`max(1, ...)` 公式计为一轮，并在下游 stall 时保持轮数不重复累计。相关新旧模块
合并 47 项普通及 47 项 ASan/UBSan 测试通过。尚未登记 SConscript，未修改旧模型或
gem5 入口。

### Step 3：让 PE 周期模型支持输入气泡

- 为 `SauCycleModel` 增加默认 strict、显式 elastic 的协议模式；
- 旧 `ConvPipelineModel` 固定构造 strict 模式，新 streaming 模型固定构造 elastic
  模式；
- `accepted_k` 只在 `input_fire` 时增加；
- 仅在 elastic 模式下允许未完成 K 时出现空拍；strict 模式继续抛出当前异常；
- 每项 MAC 按实际接受周期调度；
- 第 K 项接受后才调度 completion；
- 连续输入时保持旧模型的状态、数值和周期行为；
- 新增定向气泡测试，确认气泡不注入新 token、不增加 K、不创建新 MAC；同时确认
  更早 token 的到期 MAC 可在气泡周期正常 commit；保留 strict 模式气泡抛异常测试。

Step 3 状态：已完成。`SauCycleModel` 现在以构造期不可变协议选择默认 strict 或显式
elastic；旧 `ConvPipelineModel` 通过公开编译期常量固定 strict，不能由运行参数切换。
elastic 只取消未完成 K 时的气泡异常，accepted count、MAC 和 completion 仍只由真实
`inputValid` 推进；连续输入下 strict/elastic 的逐拍状态和数值完全一致。相关新旧模块
合并 69 项普通及 69 项 ASan/UBSan 测试通过。

### Step 4：实现直接流式融合模型

- 用深度 4 FIFO 连接新 Im2Col 和 PE；
- 第一项到达即 launch 空闲 PE；
- 允许下一 tile 预取到 FIFO；
- PE 忙时通过 FIFO 和 ready 链反压 producer；
- 用 tag 检查 tile、K、weight 和 output metadata 对齐；
- 实现完整 drained 和守恒检查。

Step 4 状态：已完成。新增独立 `StreamingConvPipelineModel`，以深度4 ring FIFO连接
Step 2 producer 和 Step 3 elastic SA；consumer、FIFO 和 producer 使用同一拍旧状态
决定 pop/push/ready，支持满 FIFO 同拍交换且禁止空 FIFO 组合旁路。模型支持下一 tile
预取、PE 忙时逐级反压、NCHW output 收集和全边界 drained 守恒。相关新旧模块合并
72 项普通及 72 项 ASan/UBSan 测试通过，尚未登记 SConscript 或接入 gem5。

### Step 5：统计、trace 和独立运行入口

- 注册第 13 节统计；
- 实现默认精简 trace 和可选详细 PE trace；
- 新增 `StreamingConvPipelineTiming`；
- 新增独立配置脚本，不改变旧入口；
- 修改构建注册前按项目规则再次确认；
- 源码完成后暂停，等待开发者手动增量编译。

Step 5 状态：已完成。已注册第 13 节全部统计，新增默认 54 字段精简控制 trace 和可选
6 字段详细 PE snapshot，新增独立
`StreamingConvPipelineTiming` SimObject、streaming 专用严格 fixture/config loader 和
运行入口，并在用户确认后登记 `SConscript`；旧 `ConvPipelineTiming` 入口和旧 trace
schema 未改变。当前源码的 81 项普通 C++ 测试、81 项 ASan/UBSan 测试和 59 项 Python
测试全部通过。开发者手动增量编译后，新入口在目标 fixture 上以预期原因 drained：
`totalCycles=6244`、4608 个 vector、32 个 tile 和 8192 个 output 全部守恒；默认 trace
为 54 字段、cycle 0..6243，output 与 Step 0 正式基线逐字节一致，无冲突合格 pair 的
整数统计精确满足 II=1。最小 fixture 的可选详细 trace 为 60 字段。新 binary 下旧融合
路径 7 个 suite 共 21 项 RTL strict regression 全部通过。完整 Step 6 回归矩阵和 Step 7
正式性能报告尚未开始。

建议手动命令为：

```bash
scons build/RISCV/gem5.opt -j4 \
    --ignore-style --limit-ld-memory-usage
```

### Step 6：功能和冲突回归

- 比较新模型、当前模型和独立 Python convolution oracle；
- 独立验证 raw-to-compacted lane/NCHW 映射，不能复用实现侧 compaction helper 作为
  唯一 oracle；
- 覆盖 W 小于、等于和大于 16；
- 覆盖 padding 0/1、stride 1/2；
- 覆盖 C/OC 尾部和 `C=63` 上限；
- 覆盖 W32 stride2 横向冲突；
- 覆盖 W6 stride2 packing 冲突；
- 覆盖 W6 stride2 的 scattered raw mask 到 prefix PE rows compaction；
- 覆盖 FIFO 满且同拍 pop/push；
- 覆盖 PE 输入气泡和 output backpressure；
- 运行旧模型全部七项 RTL strict regression，严格比较既有 53 字段、7,772 个 cycle
  和 NCHW output，确认连续输入路径未变化。

Step 6 状态：已完成。新增独立 Python artifact verifier，不调用实现侧 C++ 或 Python
compaction helper，而是从 resolved output geometry 直接推导每个 tile 的 raw source
lane、compacted prefix mask 和 NCHW 顺序；同时逐拍检查 54 字段 compact trace、tile/K
连续、FIFO/PE handshake 和 drained 守恒，并用 direct-NCHW convolution oracle 检查
output。新增 11 个 streaming gem5 profile，覆盖 W1/W5/W6/W16/W17/W20/W32、padding
0/1、stride 1/2、C63、OC1/7/15/16、N2、scattered compaction、bank conflict、满 FIFO
同拍交换、PE bubble 和周期 output backpressure，共 33 项全部通过；其中 6 个可兼容
profile 的 output 还与当前旧模型冻结 output 逐字节一致。旧融合路径 7 个 suite 共 21 项
RTL strict regression 全部通过。当前源码 81 项普通 C++、81 项 ASan/UBSan 和 62 项
Python 测试也全部通过。

### Step 7：目标 workload 性能验收

使用目标 fixture 和默认每拍 output ready，检查：

- `totalCycles < 12000`；
- `conflictFreeOutputPairs>0`、`conflictFreeOutputGapCycles` 等于 pair 数且
  `conflictFreeOutputMaxGap=1`，由此得到无冲突稳态 `conflictFreeOutputII=1`；
- 4608 个 vector 全部生成、push、pop 和送入 PE；
- 32 个 tile 全部生成、launch 和完成；
- 8192 个 NCHW output 全部写入一次；
- output.csv 与基线及 oracle 逐字节一致；
- 输出新旧周期、冲突、FIFO、PE bubble 和 tile 间停顿分解。

Step 7 状态：已完成。使用当前构建对目标 fixture 08 生成独立正式 artifact，并由 Step 6
verifier 再次检查 54 字段 trace、全部守恒、direct-NCHW oracle 和旧基线 output。
`totalCycles=6244`，相对旧基线 28378 减少 22134 拍（77.997%），等效 4.545 倍周期
加速；4608 个 vector、32 个 tile 和 8192 个 output 全部守恒。目标 workload 无 bank
conflict、无 PE input bubble；4576 个合格无冲突 pair 的 gap sum 为 4576、最大 gap 为
1，整数条件确认 II=1。精确周期分解为 6 拍启动、4608 拍 PE 输入、31×51 拍 tile 边界
和 49 拍最终 drain。正式报告和 SHA256 保存于
`m5out/im2col_pipeline_step7_performance_20260721/PERFORMANCE_REPORT.md`。

### Step 8：记录探索结论

- 更新 README 和 STATUS；
- 明确哪些性能来自组合 SRAM 和理想 weight 供给假设；
- 明确探索模型没有 RTL 逐拍验证；
- 若达到目标，记录未来 RTL 所需的 valid/ready、FIFO、bank 仲裁和 bubble 协议；
- 若未达到 12000 拍，保留正确实现和统计，定位剩余瓶颈，不通过硬编码周期、跳过
  工作或修改结果来满足门槛。

## 17. 验证矩阵

| 类别 | 配置重点 | 验证目标 |
|---|---|---|
| 无冲突满 tile | W32, stride1, pad1 | 稳态 II=1 |
| 横向冲突 | W32, stride2 | 单端口最少读取轮数 |
| packing 冲突/重排 | W6, stride2，自然输出尺寸 | W<16 同 bank 不同 row、scattered raw mask、稳定 compaction |
| packing 尾部 | W5, stride1 | spatial mask 和补零 |
| 宽度尾部 | W17, stride1 | 第二个 wGroup 有效 lane |
| 无 padding | pad0 | 坐标和输出尺寸 |
| channel 上限 | C63 | K=567 和 bubble 后完成 |
| OC 尾部 | OC1/7/16 | 有效 PE column 和 NCHW 输出 |
| batch | N>1 | tile 顺序和输出索引 |
| FIFO 边界 | push/pop/full | 守恒和同拍交换 |
| PE bubble | 人工及自然 stall | 累加器和 K 不误推进 |
| 输出反压 | 周期性 ready | 无丢失、重复和提前 drained |
| 旧模型回归 | 现有 golden matrix | 已验证路径不变 |

## 18. 最终验收标准

1. 新探索入口严格接受和拒绝本计划规定的参数；
2. 无冲突时三级流水填满后每拍可向 FIFO push 一组 vector；
3. stride2 冲突使用单端口 bank 的最少读取轮数；
4. FIFO、流水级、tile、K、lane 和 output 守恒断言全部成立；
5. PE 气泡不注入新 token、不推进 K、不创建新 MAC；更早 token 的到期 MAC 在气泡
   周期仍正常 commit；
6. 所有功能测试与独立 convolution oracle 一致；
7. 旧模型及其 RTL strict regression 不退化；
8. 目标 fixture 的 `totalCycles < 12000`；
9. 目标 fixture 的 `conflictFreeOutputPairs>0`、全部合格 pair 间隔均为 1，且
   `conflictFreeOutputII=1`；
10. 目标 fixture 的 output.csv 与当前基线逐字节一致；
11. stats 能解释剩余的 conflict、FIFO、PE 和 tile 切换开销；
12. 文档明确声明探索模型的假设和未完成的 RTL 验证。

## 19. 风险

- 深度 4 FIFO 无法覆盖较长的 PE result drain，tile 间仍会反压 Im2Col；
- lane compaction 增加 S2 组合选择和 metadata；本探索模型不据此声称未来 RTL 已满足
  100 MHz 时序，后续 RTL 阶段必须重新评估组合路径；
- 允许 PE 输入气泡是新的探索协议，未来 RTL 需要额外 valid/clock-enable 控制；
- 组合 Scratchpad 和理想 weight 供给可能高估真实 RTL 吞吐；
- 单端口 bank 下 stride2 不保证 II=1；
- OC 第一版限制为 16，不能代表任意 OC 的最终方案；
- `<12000` 依赖 tile 切换和 PE drain 开销，必须通过实际统计验证；
- 扩展 `SauCycleModel` 时必须保证旧连续 K 输入的周期行为不变，否则会破坏已有 RTL
  strict validation 结论。
- strict/elastic 模式若共享错误的 ready 或状态逻辑，可能让探索协议污染旧冻结路径；
  必须用构造模式、旧回归和异常行为测试三层隔离。

## 20. 已确认决策

- 只修改 gem5，先探索再决定是否实现 RTL；
- 新旧模型并存；
- 3x3、dilation1、统一 padding 0/1、统一 stride 1/2；
- N/C/H/W 在现有硬件约束内可配置，OC 第一版为 1..16；
- 保持 16 个单端口 bank 和组合 Scratchpad 响应；
- 三级弹性流水，冲突时 S1 暂停，不做多 gather slot；
- 深度 4 FIFO，删除完整 activation tile buffer；
- raw Im2Col spatial mask允许 scattered，S2 按 raw lane 升序稳定压紧为 PE prefix row，
  并保存 source-lane/NCHW mapping；旧 loader 的 scattered mask 拒绝行为不变；
- FIFO 第一项到达即 launch PE；
- PE 支持输入气泡；
- PE ready 由 streaming consumer 的 `ACCEPT_K` 状态唯一产生，FIFO pop 与 SA 输入
  统一使用同一个 `input_fire`；
- `SauCycleModel` 默认保持 RTL strict continuous 协议，新入口显式启用 elastic
  bubble 协议；
- 允许预取下一 tile，允许 tile 间停顿；
- canonical K 顺序保持 KW -> KH -> C；
- stride2 以正确和最少读取轮数为目标，不要求 II=1；
- 目标 workload 同时要求 `totalCycles<12000` 和无冲突 II=1；
- 默认精简 trace，可选详细 PE trace；
- 本文的 Step 0 基线、Step 1 纯契约、Step 2 流水化 Im2Col、Step 3 弹性 SA 输入和
  Step 4 直接流式融合模型已完成；Step 5 的统计、trace、SimObject、独立入口、构建
  登记和构建后冒烟验收也已完成。Step 6 及后续完整回归和正式性能报告尚未开始。
