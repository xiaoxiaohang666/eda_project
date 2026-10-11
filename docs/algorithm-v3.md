# 第三版：固定 TSV 双侧真实端点路径恢复

日期：2026-10-11。分支 `llh-version3`，从 `llh-version2@027aaa3` 创建；只在 `.` 修改、构建和验证。第二版方案见 [algorithm-v2.md](algorithm-v2.md)。

## 优化对象与设计依据

第二版已将原始 sample001 两种溢出降为 0，但真实端点 L/Z 候选只处理同 Die 双 Pin 网。三个固定跨 Die 网仍通过 GCell 中心接树，保留了端点到中心的额外绕行。这些网各有四个 Pin，按 Die 加入已分配 TSV 落点、按坐标和层去重后，每侧恰好只有两个端点，可以直接复用合法 L/Z 路径构造。

第三版在第二版完整流程结束后增加一个独立阶段；先完成可对照的第二版结果，再逐网改进固定 TSV 两侧的几何。固定 TSV 位置、原始 Pin、热数据和评分器均保持不变。此处优化的是落点两侧路径，没有通过移动 TSV 提高热分。

## 已实现算法

1. `terminal_pair_candidates(first,last,low,high)` 构造未编码的显式几何，保留真实线段、普通 via、跨格节点与边。原有同 Die 路由调用该函数后继续完整编码；关闭第三版阶段仍能重现第二版。
2. 对每个已分配 slot 的 Net，各侧加入 `planned_tsv`，保留所有原始 Pin，按相同坐标与 layer 去重。仅每侧恰好两个端点时启用新阶段；其他扇出、模板不可行或时间不足时使用已有路线。
3. 下 Die 平面布线层限 M1–M9，上 Die 限 M12–M20；M10/M11 只承担固定落点和合法普通 via stack。普通 via 禁止跨越 M10/M11。每侧最多六个按 via 距离排序的 H/V 层对，每对四个 L/Z 形态，组合上限为 24×24；不声称连续几何全局最优。
4. 合并两侧的显式几何、accesses、资源节点和边，保留原 slot，再对完整 Net 编码。`validate_mapping` 检查唯一固定 TSV 接口及原始 Pin；接受前执行完整连通性与两套账本校验。
5. 从原有完整解撤回该 Net 的物理资源和 Guide 需求。候选经完整测量、验证后再撤回，以便评估下一候选。最终只安装最佳已验证候选；若没有候选改善则准确恢复原网。循环和完整测量前检查时间；超时保留已获得的合法最优结果。

接受同时要求相对该网替换前的完整解：

- Guide overflow、物理边 overflow 均不增加；
- 原始 Guide MST、显式物理线长估计均不增加；
- 普通 via 数不增加，质量 Q 不下降；
- 满足既有完整比较键 `(Guide overflow, physical overflow, -Q, physical length, vias)` 的改善条件。

因此扩大 Guide reference 本身不能获得接受。此 via 约束只保证第三版新增阶段不继续增加 via；没有消除第二版已产生的 via 增长，也没有实现完整 RC/时序优化。

主要代码在 `src/router/tsv_local_router.cpp`；`pin_local_router.cpp` 提供共用几何构造；`negotiated_router.*` 接入调用和统计；`main.cpp` 提供 `--tsv-local 0|1`（默认 1）。统计新增 `pre_tsv_local_metrics`、`tsv_local_candidates/trials/accepted`、`options.tsv_local`。

## 原始样例同输入对照

固定输入为原始 sample001，评分器和输入 SHA-256 在 [algorithm-v3-results.json](algorithm-v3-results.json) 中记录。以下 Q 是本地 45% 线长、30% 拥塞、25% 热质量的加权值，不是最终竞赛排名。

| 版本 | 100Q | 线长分 | 物理线长估计 DBU | Guide MST DBU | Guide reference DBU | 普通 via | 两种溢出 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 冻结第二版 | 81.018701 | 85.439595 | 1929495 | 1909682.5 | 1631625 | 1221 | 0 / 0 |
| 第三版关闭新增阶段 | 81.018701 | 85.439595 | 1929495 | 1909682.5 | 1631625 | 1221 | 0 / 0 |
| 完整第三版 | **83.099544** | **90.063689** | **1889355** | **1808392.5** | **1628705** | **1221** | **0 / 0** |

第三版提高 2.080842 个质量分点，物理线长估计减少 40140 DBU（约 2.08%），原始 Guide MST 减少 101290 DBU（约 5.30%）。reference 也减少 2920 DBU，没有扩大分母。三个跨 Die 网都接受替换；1248 个完整编码候选中，60 个通过长度/via 初筛进入完整测量，最终接受 3 个 Net。200 个同 Die Net 的输出保持一致。

拥塞分为 100；热分仍为 50.2835336。实体与评分器推断 TSV 都为 3，assignment、接口矩形和原始 Pin 保持一致。普通 via 仍为 1221，相比第一版的 901 没有回落，不能宣传 via 或时序改善。

本次单次命令墙钟：冻结第二版 2.678 秒、第三版关闭 2.577 秒、完整第三版 2.979 秒。机器同时有其他验证工作；这些数字只记录本次观察，不作为稳定性能基准。

## 复现与验证

```bash
cd .
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 scripts/compare_router_v3.py \
  --v2-binary output/runs/v3-development-20261011/eda_router_v2
```

对照脚本拒绝覆盖非空结果目录，保留完整命令、Guide、router_stats、独立 evaluation_report、输入/二进制/评分器哈希与测量一致性检查。本次完整证据位于 `output/runs/router-v2-v3-kw9flfle/`。冻结第二版从修改前的源码重新构建，不随 Git 分发；换机器须从 `027aaa3` 独立构建第二版，再传入 `--v2-binary`。

`--tsv-local 0` 的 Guide 与冻结第二版逐字节一致，全部既有指标一致。第三版程序如需复现第一版，应同时使用 `--tsv-local 0 --pin-local 0 --wirelength-rounds 0`。旧 v1/v2 对照脚本检测新版开关并显式关闭第三版，保持其版本含义。

Release 全部 10/10 个 CTest 目标通过。Debug 使用 `-g -Og` 并启用 AddressSanitizer/UndefinedBehaviorSanitizer，全部 10/10 个目标也通过（474.98 秒），未发现 sanitizer 错误。新增端到端测试独立检查真实 Pin（含 FS 方位变换）、固定 TSV、跨 Die 矩形连通、Guide 需求、仅跨 Die 网变化、完整指标非退化、确定性、参数与输出保护。新实现也经独立只读代码审查。

## 范围与后续方向

当前只处理每侧两个真实端点的固定 TSV 网络，其他网络保留第二版路径。通用多端 Steiner 树、共享分支真实几何去重、端点到已有枝条投影、via/RC Pareto 搜索仍未实现。后续应先加入高扇出、密集 OBS、紧容量和不同网格的基准，验证泛化，再扩展多端树。

物理资源仍为 GCell 图边容量，评分仍是 Guide 代理；零溢出和结构检查 PASS 不等同于 track assignment、详细布线或 signoff DRC。既有“一跨 Die 网一个 TSV、一个 slot 一网”是实验政策，不能外推为官方普适规则。
