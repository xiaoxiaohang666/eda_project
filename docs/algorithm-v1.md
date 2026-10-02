# 第一版多目标布线算法（2026-10-02）

本版以用户提供的《算法模块.md》为设计参考，在现有工程内实现共享三维资源图、容量感知接树、TSV 分配、协商拥塞、外层槽位候选和局部修复。默认命令运行 `negotiated` 算法；旧 L 形布线器仍可由 `--router baseline` 运行，作为实验对照。

本文件说明实际实现边界；参数与一网一个 TSV 的配置是首版实验政策，不是赛方已确认规则。仓库中的固定 `evaluator/` 保持原样，其 PASS 不能替代详细布线 DRC。

## 使用与复现

```bash
bash scripts/build.sh Release
ctest --test-dir build --output-on-failure

./build/bin/eda_router \
  --def benchmarks/sample001/sample001.input.def \
  --lef benchmarks/sample001/sample001.input.lef \
  --thermal-report benchmarks/sample001/sample001.thermal.input.json \
  --output output/algorithm-v1/router.guide \
  --stats output/algorithm-v1/router_stats.json

# 参考 guide、旧基线、初解、RRR、TSV 无局部修复、完整版本
# 每次自动使用新的 output/runs/algorithm-comparison-* 目录
python3 scripts/run_algorithm_comparison.py
```

正式参数名及旧框架别名均保留：`--def/--input-def`、`--lef/--lefs`、`--thermal-report/--thermal-json`、`--output/-o`。多个 LEF 可重复指定。

| 参数 | 默认值 | 含义 |
| --- | ---: | --- |
| `--router` | `negotiated` | `baseline` 保留旧算法对照 |
| `--rrr-rounds` | 30 | 内层协商轮数上限，0 可消融 |
| `--tsv-rounds` | 10 | 外层 TSV 搜索轮数上限，0 可消融 |
| `--search` | `astar` | `dijkstra` 使用同一图/代价并关闭启发式 |
| `--search-limit` | 250000 | 单次寻路累计展开上限 |
| `--time-limit` | 120 秒 | 算法软时间预算，预留末尾导出时间 |
| `--local-repair` | 1 | 设 0 时关闭森林修复，整网重布保留 |
| `--stats` | 不输出 | JSON：完整初解与最终指标、槽位、搜索/接受/回滚计数 |

`--time-limit` 是软截止时间，最终验证、导出和外部评分仍需时间；它不是操作系统强制 CPU/RSS 限制。Linux 的统计 JSON 另记录 `peak_rss_kb`（进程峰值常驻内存）、实际参数和候选拒绝原因。若初始化未得到完整合法解，预算不足会报错，不输出部分 Net。优化阶段保留最优完整解，失败候选恢复旧路线。输入文件不允许与 Guide 或统计文件使用同一路径。

## 实现结构

| 文件/目录 | 职责 |
| --- | --- |
| `src/parser/lef_parser.*`、`def_parser.*` | track、金属参数、VIA、OBS、实例与 Pin 几何的子集解析及全局坐标变换 |
| `src/router/routing_graph.*` | 隐式邻接、首选方向、物理容量、scorer 容量、OBS/VIA 保守合法性 |
| `src/router/tsv_policy.*` | 上下槽位几何配对、一网一槽配置、最小费用匹配、最终 Guide 映射检查 |
| `src/router/negotiated_router.*` | Pin access、多层接树、A*/Dijkstra、两套账本、RRR、TSV/局部修复和导出 |
| `src/objectives/guide_metrics.*` | Guide 需求、连通矩形 MST、HPWL 参考值、TSV 交叠点、Pearson 和质量值 |
| `src/validation/guide_validator.*` | 全 Net 集、Pin 覆盖、层/边界/连通性及 Die 内 Net 不越 Die |
| `tests/test_algorithm.py` | 独立 Python 需求/TSV 复算、确定性、完整解保留、限额失败和输出保护 |
| `scripts/run_algorithm_comparison.py` | 相同输入与固定评分器的六组消融、指标对齐和文件哈希 |

原来三个 `src/objectives/*/` 子目录保留职责说明；算法共享的指标实现集中在 `guide_metrics.*`，不维护三套互相独立的布线路径或占用状态。

## 资源、接树与接受条件

图节点是 `(col,row,layer)`，采用输入内 GCell 中心。平面边只走 LEF 首选方向；普通 via 只连接同 Die 相邻金属层，跨 Die 连接交由已选择的 M10/M11 TSV。原 Pin 的真实坐标通过独立局部 access 几何接入图，不能用格中心替代 Pin。

物理平面容量与评分容量分开。物理 H 边使用 Y track，V 边使用 X track，并考虑已解析 OBS；固定 scorer 的 HORIZONTAL 容量却来自 X track 格计数，VERTICAL 来自 Y track 格计数。后者的方向命名虽不符合通常物理直觉，兼容度量仍逐项复现。普通 via 没有可靠的数量容量时不假设容量为 1。

每个 Net 的图边去重占用。Guide 的评分需求按每个矩形覆盖的格分别累计；同 Net 的多个不同矩形覆盖一格仍可能产生多份需求。整理解不允许通过扩大不必要区域来改变 HPWL 分母。导出的实际几何重新计算指标，不能用边账本冒充评分器结果。

初解采用确定性拓扑候选和 Pin 接入已有树，以共享路径形成分叉。多层寻路用非负的长度/via/现时拥塞/历史拥塞代价，A* 的启发式只提供几何下界；局部窗口逐步扩展。达到展开上限报告 `SEARCH_LIMIT`，不会误报成电气无解。

RRR 先定位溢出相关 Net，再撤除自身占用并重布；比较与回滚在完整候选层面进行，历史拥塞价格帮助下一轮尝试其他路径。完整状态的排序为：

```text
通过硬几何、Pin、连通性和 TSV 映射检查后，最小化
(Guide 总溢出, 物理总溢出, -Q, 物理线长, 普通 via 数)
```

其中 `Q = 0.45 * wirelength_quality + 0.30 * congestion_quality + 0.25 * thermal_quality`，是固定 scorer 三个分项的内部组合，不是最终竞赛排名。即使 Q 增加，也不会接受总溢出更差的完整解。

## 与设计参考的具体取舍

- 隐式图采用 GCell 中心、真实 track 数和已解析 OBS/VIA 的保守几何检查。原 Pin 与实际 TSV 落点通过合法局部 access 接入；没有把每一根 track 离散成完整详细布线图。
- L/Z 模板作为当前路径费用上界，再用同一代价的 A* 搜索；本版没有采用“低于下界 1.2 倍就直接接受模板”的近似快路由。窗口寻路的结果不宣称是全芯片所有绕行路径中的全局最优。
- 局部森林修复用于**终端位置不变**时的拥塞枝条；实际 TSV 换槽采用整网重布回退。保留了换槽候选、交换、受影响邻网与事务流程，没有把“移动 TSV 时的最小尾枝切除”宣称为已优化完成。
- 固定 checker 忽略实例朝向，样例 64 处 FS 朝向 BUF 的 A Pin 检查点会落在真实中心之外。代码保留真实 Pin/access，仅当两点间最小接入窗口通过 OBS/Die 检查时补足 Guide 覆盖；无法兼容时报 `CHECKER_PIN_LOCATION_MISMATCH`。例：`u_buf_l0_14_bottom.A` 的真实中心为 `(78910,211575)`，checker 检查 `(78910,211225)`；实际 350 DBU 差异由周围空闲接入区域容纳，未移动 Pin。
- 采用确定性、有界搜索。隐式 GCell 图、候选数与搜索展开限额控制规模，但不承诺精确 Steiner 最优解，也未实现完整时序/RC 或详细布线 DRC。

## TSV 政策与样例的可移动范围

上下槽位按变换后的 port 中心和区域配对，保留原实例名；不按 slot 名字编号配对。候选使用实际 TSV port 中心，不能任意改为附近热点格。配置为每个跨 Die Net 一个实体 TSV、每槽容量一，并检查槽位冲突及 VIA 包络。

`sample001` 的三个固定上下 vlink Pin 分别在 `(51390,67340)`、`(188990,67340)`、`(118590,156940)`，对应上下端口已在同 XY。固定 scorer 会把同 Net 的每一对正面积 M10/M11 交叠都计算为一个统计点；额外移动至新槽位容易同时保留原位置交叠，产生未计划 TSV。

因此政策显式维护规划连接与导出 Guide 推断连接，逐 Net 检查数量及位置。当前样例的其他移动若无法表达为一致的合法连接就会被拒绝，并记入 `policy_rejections`。不删除固定 Pin、不制造零宽矩形、不将 Pin 卡在边界绕过检查。

外层具备候选替换/交换、Pearson 计数更新及局部/整网重布路径，但样例上“候选被政策拒绝”不能称为“TSV 移动改善了热分”。若六组消融中 `rrr`、`tsv_no_local`、`full` 相同，应保留这个结果。开放可移动政策前需要明确的官方映射规则或可验证的新输入。

## 本次样例实测

2026-10-02 在 Ubuntu-22.04 主工作树运行六组对照，全部取得固定 scorer PASS。每组 Guide 均为 203 个 Net；C++ 精确指标与未修改的 Python 评分器逐项一致（绝对容差 `1e-7`，相对容差 `1e-8`）。

| 版本 | Guide 总溢出 | 线长分项 | 拥塞分项 | 热分项 | scorer TSV 点数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 发放包参考 Guide | 4 | 58.5567 | 80.0000 | 51.2357 | 12 |
| 旧 L 形基线 | 87 | 52.2467 | 0.0000 | 50.5347 | 6 |
| 新算法初解，RRR/TSV 轮数为 0 | 1 | 69.9702 | 95.0000 | 50.2835 | 3 |
| 初解 + RRR | 0 | 70.0800 | 100.0000 | 50.2835 | 3 |
| + TSV 外层、关闭森林修复 | 0 | 70.0800 | 100.0000 | 50.2835 | 3 |
| 完整第一版 | 0 | 70.0800 | 100.0000 | 50.2835 | 3 |

完整结果的 Guide MST 代理线长为 2,672,382.5 DBU，HPWL 参考值为 1,872,805 DBU，归一化线长为 1.426941139，内部 Q 为 0.741068724；独立物理边模型的总溢出为 0。本次最终对照中完整命令约 0.42 秒，这只是该机器/样例一次运行的墙钟时间，未进行稳定性能基准或外推。

完整版本记录 7,632 次寻路展开、3 轮 RRR 尝试/2 轮接受、1 次回滚、1 次森林修复；30 个 TSV 候选被固定 Pin 政策拒绝。前三项改善来自多层接树和拥塞修复，不能把后两组相同结果解释成换槽热收益。

完整报告保存在本机 `output/runs/algorithm-comparison-t14dy154/comparison.json`，每组各有 `run_manifest.json`。当次二进制 SHA-256 为 `8c6463455223fa3cd8826c9f8a4dee36cd5350dc7dc7fe54409bc47072779b95`。生成结果目录被 Git 忽略；读者可使用上述命令重新复现。

此外，测试会生成**合成变体**：只从临时 DEF 中删除六个固定 M10/M11 IO 端点及其 Net 连接，保留其他端点和原始 benchmark 文件，用于开放候选搜索。此变体得到 18 次 TSV 试布、2 次接受、16 次回滚；仅运行前两轮与继续搜索至十轮输出完全相同，验证后续失败候选完整恢复 Guide、槽位和度量。重复运行确定一致，并通过固定 evaluator 及指标对齐。证据在 `output/runs/movable-fixture-e8eqezer/`；该结果只验证事务/候选机制，不作为原样例的改进成绩。

## 回归结果

最终源码通过 Release 和 Debug 两种构建，各 7/7 个 CTest 测试目标全部通过；Debug 开启 AddressSanitizer 和 UndefinedBehaviorSanitizer。测试覆盖原框架/CLI、评测包装器、真实资源几何、8 个同代价 A*/Dijkstra 对照、508 项指标/政策断言，以及算法确定性、固定检查点兼容、移动候选接受/回滚和预算失败保留旧输出。最终 Release 全套约 7 秒，Debug 全套约 197 秒；均为本机当次数据。

本次完整 sample001 运行的 `peak_rss_kb` 为 18192；这是路由进程的峰值常驻内存，不包含独立 Python 评分器，也不代表大规模用例的内存上限。

## 验证证据与边界

对照脚本将每组 Guide 交给未改的评分器，并比较 C++ 与 Python 报告的 Guide 总溢出、矩形 MST 线长、HPWL 参考值、归一化线长、相关性、TSV 统计点数和 Q。各组单独保留命令、Guide、日志、JSON 与 `run_manifest.json`；总 `comparison.json` 记录二进制、输入和评分器哈希。输出目录非空时脚本拒绝覆盖。

首版开发前封存的基线位于本机 `output/runs/v1-baseline-sM4p7zbR/`：203 个 Net、6 个 TSV 统计点、Guide 总溢出 87、线长分项 52.246748、拥塞分项 0、热分项 50.534701；此目录包含原二进制副本与当次评分器/输入/源码哈希。它是本次实测，不沿用历史报告数值。

解析器和资源图是面向当前基准的保守全局布线模型，并非完整 LEF/DEF 或详细布线器。VIA/OBS 几何合法性不能证明 track assignment、spacing、制造规则或 signoff DRC 全部合格。参考 guide 也仅作为同脚本对照，不能把参考报告当成本算法成绩。当前 TSV 数量及槽容量是显式首版政策，官方规则更新后需同步修改政策与回归。
