# Gem5 Im2Col Reference RTL 独立周期模型状态

最后更新：2026-07-20

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
