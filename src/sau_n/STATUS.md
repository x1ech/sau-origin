# Gem5 Im2Col Reference RTL 独立周期模型状态

最后更新：2026-07-17

## 当前阶段

当前已完成计划冻结、Step 0 至 Step 8、VCS RTL/gem5 逐拍验收和最终文档交付。

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
```

当前可以正式声明 `RTL per-cycle validation passed`。PLAN 定义的 Step 0 至 Step 8
均已完成，计划范围内没有未完成的实现或验收项。

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

计划范围内没有必需的下一步。当前 `src/sau_n/`、`util/im2col/`、
`tests/gem5/im2col/` 和 `configs/example/im2col_timing.py` 仍为 Git 未跟踪内容，建议
在确认后纳入版本控制。未来若修改 DUT、testbench、周期模型、fixture 解析或 trace
schema，必须重新生成 provenance 并运行 7 项 RTL strict 回归。
