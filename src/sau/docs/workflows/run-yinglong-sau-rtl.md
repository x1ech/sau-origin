# Yinglong SAU Matmul RTL 仿真流程

本文记录从生成 SAU matmul case、编译固件到运行 Yinglong RTL 仿真的完整流程。
当前已用 `M=32, K=512, N=32, cutbit=8`、命令序列
`flowmode=2 -> flowmode=0`，以及 `M=32, K=768, N=32, cutbit=8`、命令序列
`flowmode=2 -> flowmode=2 -> flowmode=0` 验证通过。另有宏隔离的链式 case：
前三条命令完成 K768 GEMM，第四条 Flow0 command 把第三条写回结果作为
Operand-A 再执行 32x32x32 GEMM，用于验证跨 command memory visibility。

## 1. 工程位置

```text
RTL/固件工程：
/home/xch/work/npu_lpnpu

数据生成脚本：
/home/xch/work/npu_lpnpu/software/autodata/sau_matrix_yl.py

SAU 固件目录：
/home/xch/work/npu_lpnpu/software/benchmarks/yinglong_sau_test
```

以下命令均假设使用 Linux shell。

## 2. 配置并生成 case

在 `sau_matrix_yl.py` 的单 case 配置处设置矩阵大小和 cutbit。例如：

```python
mode = 8
m, k, n = 32, 512, 32
cutbit_val = 8
```

生成数据：

```bash
cd /home/xch/work/npu_lpnpu/software/autodata
python3 sau_matrix_yl.py ./data/config ./build
```

生成的当前 case 为：

```text
build/build_s32/matmul/sau_matmul_regress_0.h
```

将它复制为固件使用的测试数据头：

```bash
cp build/build_s32/matmul/sau_matmul_regress_0.h \
  ../benchmarks/yinglong_sau_test/sau_testdata.h
```

注意：

- `matmul_n32.hpp` 仅在 `K > 256` 时触发内部留存路径。
- 触发留存不代表每条命令的 `flowmode` 都是 2。以 `K=512` 为例，第一段
  K256 使用 `flowmode=2` 留存，最后一段 K256 使用 `flowmode=0` 输出。
- `K=768` 由两个 `flowmode=2` 的 K256 段连续留存，第三个 K256 段使用
  `flowmode=0` 输出，可用于验证多次跨 command 的 accumulator 保存/恢复。
- `flowmode=1` 表示输出转置；其结果应是正常正确答案的转置。
- `transposemode` 只控制 A/B 操作数转置，不控制输出布局。

## 3. 编译固件

```bash
cd /home/xch/work/npu_lpnpu/software/benchmarks/yinglong_sau_test
make
make mod3
```

链式 case 不改变默认固件行为。需要复现时显式启用：

```bash
make -B all mod3 SAU_TEST_CASE=1 CHAIN_MATMUL=1
```

成功后重点文件位于：

```text
build/instruction.hex
build/memory_mod_0.hex
build/memory_mod_1.hex
build/memory_mod_2.hex
```

`make mod3` 当前可能打印超出 192KB 的填充行被丢弃的 warning；只要命令返回
成功并生成上述三个 `memory_mod_*.hex`，该 warning 本身不代表 case 失败。

如果编译错误指向 `sau_testconv.h` 中其他用户的绝对路径
`/home/huhaiqin/...`，说明 matmul-only 固件仍包含了 conv 数据头。应确认
`sau_benchmark.c` 的 matmul-only 配置没有包含或调用 conv 测试；不要为此修改
生成的 matmul 数据。

## 4. 编译 SAU RTL 仿真器

第一次运行、执行过 `git pull`、RTL 源码发生变化，或者仿真器不是以 SAU
regression 配置编译时，先执行：

```bash
cd /home/xch/work/npu_lpnpu
make yinglong_compile_sau
```

该目标会定义 `SAU_REGRESS`，使 testbench 从
`software/benchmarks/yinglong_sau_test/build/` 加载固件。

如果跳过此步后日志显示读取：

```text
software/benchmarks/yinglong_test/build/instruction.hex
```

或者报告该文件不存在，说明当前 `simv` 是普通 Yinglong 配置。停止这次运行，
执行 `make yinglong_compile_sau` 后再仿真。

## 5. 运行无波形 RTL 仿真

```bash
cd /home/xch/work/npu_lpnpu
make yinglong_sim
```

主要日志：

```text
sim/vcs/build/yinglong/sim.log
```

matmul 比较结果：

```text
sim/vcs/build/yinglong/matmul_compare.csv
sim/vcs/build/yinglong/matmul_mismatches.csv
```

通过时日志应包含类似：

```text
Wrote matmul_compare.csv and matmul_mismatches.csv
(mismatches=0, ..., elems=1024)
*** TEST PASSED (npu_done) ***
```

不要只根据前面的 AHB scoreboard `ALL TESTS PASSED` 判断 matmul 正确；最终应同时
确认 `mismatches=0` 和 `TEST PASSED (npu_done)`。

## 6. 运行带 FSDB 的仿真

需要检查 start、busy/done、flowmode 或写回时序时：

```bash
cd /home/xch/work/npu_lpnpu
make yinglong_run
```

FSDB 输出：

```text
sim/vcs/build/yinglong/yinglong.fsdb
```

波形分析必须使用：

```text
/home/xch/work/npi_fsdb_probe
```

先完整阅读该工程的 `README.md`，再按其中说明导出所需信号。对于 K512 留存
case，至少检查：

```text
top_yinglong_tb.u_dut.u_dut_kui.SAU_1_inst.start
top_yinglong_tb.u_dut.u_dut_kui.SAU_1_inst.core_state_s
top_yinglong_tb.u_dut.u_dut_kui.SAU_1_inst.sa_flow_mode
top_yinglong_tb.u_dut.u_dut_kui.SAU_1_inst.sau_crossbar_done
top_yinglong_tb.u_dut.u_dut_kui.SAU_1_inst.result_final_valid_o
top_yinglong_tb.u_dut.u_dut_kui.SAU_1_inst.register_wraddr_valid
top_yinglong_tb.u_dut.u_dut_kui.SAU_1_inst.sau_sram_wstrb
top_yinglong_tb.u_dut.u_dut_kui.SAU_1_inst.sau_sram_addr
```

正确的 K512 留存行为应满足：

1. 第一条被接受的命令使用 `flowmode=2`；
2. 第一条命令完成后，第二条命令才启动；
3. 第二条命令使用最终输出模式，当前 case 为 `flowmode=0`；
4. 留存命令不提前产生最终写回；
5. 第二条命令结束后产生完整输出，最终 1024 个元素全部匹配。

链式 case 还应满足：

1. 命令 flow 序列为 `[2, 2, 0, 0]`；
2. 第三条命令写出 32 beats，第四条命令的 Operand-A 按相同地址顺序读取；
3. 第三条最后一次 write 早于第四条第一次 Operand-A read；
4. 第四条最终输出与独立软件参考的 1024 bytes 全部匹配。

## 7. 最短重复执行清单

只修改 case、RTL 没有变化时：

```bash
cd /home/xch/work/npu_lpnpu/software/autodata
python3 sau_matrix_yl.py ./data/config ./build
cp build/build_s32/matmul/sau_matmul_regress_0.h \
  ../benchmarks/yinglong_sau_test/sau_testdata.h

cd /home/xch/work/npu_lpnpu/software/benchmarks/yinglong_sau_test
make
make mod3

cd /home/xch/work/npu_lpnpu
make yinglong_sim
```

第一次运行或 RTL 变化后，在 `make yinglong_sim` 前增加：

```bash
make yinglong_compile_sau
```
