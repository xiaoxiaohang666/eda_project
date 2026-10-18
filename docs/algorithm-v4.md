# 第四版：预算内接树改进与 TSV 分配邻域扩展

日期：2026-10-15。分支：`llh-version4`。本说明对应 `.` 当前源码；第三版官方模型及交接基线见 [项目整体交接报告](项目整体交接报告_llh-version3_20261015.md) 和 [继续优化报告](第三版算法继续优化报告_20261014.md)。完整验证结果见 [本轮结果报告](第四版算法优化报告_20261015.md)。

## 1. 改进对象与流程

公开大样例原有 fast 路径成功后直接返回模板，不继续执行精确搜索。其目标 shortlist 很小，模板可能已经穿过现有树，却仍延伸到 shortlist 中的预定终点；混合 H/V 模板的零长线段也可能触发无用的换层。另一方面，只比较两种单层 L 不能避开两个均合法但拥塞的拐角走廊。

Projected TSV 初始可行匹配之后，原有 polish 主要查看每个网锚点附近的少量槽位，最多运行两轮。经全局补齐而被放到远处的网，其当前槽位可能恰好适合另一网，但局部正向查询不一定发现对应交换。

第四版保留“可行分配 → 接树初解 → RRR → 完整校验/导出”的结构，在两个位置扩展候选：

```text
Projected TSV 可行匹配
  → 可选：独立 polish 候选预算、多轮迭代、反向锚点 KD 查询
  → 逐网接树
      → 同格直接接入
      → L/混合层模板在首个树交点截止
      → 有预算的同层中线 Z 比较
  → 原有 RRR 完整目标接受/回滚
  → Guide 编码、资源账本检查、整数导出与独立读回
```

## 2. 官方大设计 fast 接树

### 2.1 生效条件与直接接入

新增 `RouterOptions::fast_quality`，默认 `true`。改进仅在以下条件同时满足时生效：

- 使用 official 模型；
- Net 数大于 1000；
- `--fast-initial 1`；
- 本次搜索没有显式选择 Dijkstra；
- `--fast-quality 1`。

official 大设计初始接树和 RRR refinement 都满足 fast 搜索的使用范围，因此本开关同时影响这两个阶段。physical 路径、小设计及显式 Dijkstra 保留既有行为。

搜索先计算各 source 的 access cost。若某个 source 已属于目标树，且其 access cost 等于全部 source 的最低值，则立即返回该 source 的单节点路径。后续边成本非负，因此这已达到当前搜索成本的下界。不能仅因某个 source 位于树上就无条件提前返回：其他 source 的接入成本可能更低。

### 2.2 首个树交点截断

每条合法模板在参与比较前，从 source 方向查找第一个属于完整 `targets` 集合的节点；若找到，则删除其后的尾部，再对保留路径累计 `edge_cost`。

这里查询完整目标树，包含未进入最近两个终点 shortlist 的节点。例如，为绕开低层拥塞而选择的高层走廊，可能先经过另一个高层树节点；此时已经连通，无需继续平移、下降多层并返回预定终点。

截断只删除已经连通之后的尾部，保留路径上的每条边仍需通过 `legal_edge`。它不增加新连接，也不移动真实 Pin 或 TSV。计数 `fast_truncated_paths` 统计被截断的候选次数，包含最终落选候选，不能解释为接受的 Net 数。

### 2.3 跳过零长 leg 的换层

混合 H/V 模板中的 `leg(c,r,layer)` 在平面坐标已经到达 `(c,r)` 时直接返回，避免仅为一条零长线段先换层再换回。最终连接真实 target layer 的 `stack` 继续执行。

这解决同层单轴路径因另一轴长度为零而附带普通 via 往返的问题；同格直接接入则提前处理无需任何边的情况。所有非零长 leg 仍先到达所需布线层，再沿合法邻接移动。

### 2.4 预算内同层 Z

仅在图允许同层非首选方向邻接时增加同层 Z。对每个选中的 source/target 对：

1. 仍比较全部可用层上的 XY、YX 两种单层 L，并保留当前最优路径。
2. 按 L 的实际搜索成本排序，取最便宜的两个不同层。每层以较便宜的 L 代表该层；只有该 L 含拥塞或 history 附加成本时，才扩展 Z。
3. 每个入选层最多构造两种 Z：经中间列，或经中间行。中点采用整数 GCell 索引；中点与端点重合的对应方向跳过。
4. Z 与原有 L、混合层模板统一检查合法边、截断首个树交点，并按相同 `edge_cost` 竞争。

这里的“拥塞”按模板搜索成本超过其几何/换层基础成本判断，因此也包含协商 history 的影响。预算是“最便宜两层中有附加成本的层”，并非遍历所有有拥塞的层。

fast shortlist 仍最多使用前两个 source 和最近两个 target。因此每次搜索新增 Z 最多为 `2 × 2 × 2 × 2 = 16` 条；不会按全部树节点或网格面积枚举 Z。原有混合层 Z 的障碍回退策略继续保留。

### 2.5 保证边界

上述改进在当前搜索快照中按同一成本比较合法候选。首树交点截断删除非负成本尾部；新增 Z 保留 L incumbent。它们不证明全局最短 Steiner 树，也不保证最终官方分数对任意输入单调提高：逐网路径会影响后续资源分布，搜索成本也仍是最终 Guide 评分的代理。

特别是，official 线长按 covered GCell 与层间矩形 overlap 计算；搜索成本仍按图边长度、via 及拥塞/history 估计。普通 via 到新层的 covered-cell 成本、非方格网格的层方向 pitch、编码后相邻层 overlap 均可能使二者不完全相等。最终候选仍需经过 Guide 编码和完整指标比较。

`--fast-quality 0` 关闭上述改进，提供旧 fast 行为对照；`--fast-initial 0` 或 `--search dijkstra` 可用于受限问题的精确成本对照。后两者可能显著增加大设计搜索资源，不能据此无预算切换整个 case005。

## 3. Projected TSV polish 邻域

### 3.1 独立预算与多轮迭代

新增 polish 专用预算，不改变初始稀疏匹配的候选图：

- `--tsv-polish-candidates 0`：继承 `--tsv-candidates`；后者默认 8。
- `--tsv-polish-candidates K`：polish 每次近邻查询最多请求 K 个候选，允许 `1…65536`。
- `--tsv-polish-passes P`：最多 P 轮，允许 `1…64`，默认 2。
- 任意一整轮没有改善则提前结束，`assignment_polish_passes` 记录实际执行轮数。

每个网仍先考虑空闲槽移动，再比较已占用槽的双网交换。移动后将旧槽恢复到空闲 KD 索引，并从索引移除新槽，使后续网和后续轮能够利用释放的真实资源。

扩大 polish 候选数不会自动扩大 `--tsv-max-candidates` 控制的初始匹配池，也不会修改已有的初始匹配约 800 万候选边保护。该保护不限制全部 polish 查询与成本评估总量；较大 K、P 仍可能造成明显开销。

### 3.2 反向锚点 KD 查询

`--tsv-polish-bidirectional 1` 为可移动接口网的 Pin 中位数锚点建立一个静态 KD 索引。对当前网 n，交换候选来自两部分：

```text
正向：在网 n 的锚点附近查槽位 → 读取槽位当前 owner
反向：在网 n 当前槽位附近查其他网的锚点 → 读取这些网当前 chosen 槽位
```

第二种查询可发现“其锚点接近本槽，但当前被分配到远处”的网。锚点位置固定，因此网在 polish 中移动后无需重建此索引；每次通过最新 `chosen[partner]` 得到对方当前槽位。候选按槽位去重，避免正向和反向命中同一交换时重复评估。

反向索引不纳入固定接口锚点；所有真正执行的移动和交换仍检查 `compatible`。交换保持已占用槽集合不变，空闲移动继续检查槽位冲突。它不创建新 reservation，不改变一网一槽及每槽容量 1 的既有政策。

### 3.3 接受条件延续第三版

weighted 模式保持原有整数代理成本：

```text
proxy = MST/ell0 + 2 × risk + λ × (1 − normalized_heat)
```

其中 MST 汇总 TSV 两侧终端的估计，risk 使用槽位两侧的资源容量估计；代码还保留与槽位无关的常数及整数缩放。空闲移动须严格降低本代理，交换须严格降低两网代理总和。初始匹配热权重仍为 2；`--tsv-polish-heat-weight` 只影响 polish。

pareto 模式延续精确全网格 Pearson 约束：空闲移动要求几何/风险代理不增加、相关性不下降且至少一项改善；交换保持 TSV 槽集合与 Pearson 不变，要求两网代理下降。它仍不等价于最终 W/C/T 加权分的 Pareto 保证。

默认 `--tsv-assignment-polish 0`。启用后，第四版默认预算仍为继承最近 8 个候选、最多两轮、反向查询关闭；较宽候选、多轮和 reverse KD 均需显式指定。多轮迭代只搜索有限局部邻域，不保证全局最小费用匹配或完整路由最优。

## 4. CLI 与对照方式

| 参数 | 默认 | 作用 |
| --- | --- | --- |
| `--fast-quality 0\|1` | 1 | 官方大设计 fast 接树的直接接入、首树交点截断、零长 leg 与预算 Z |
| `--fast-initial 0\|1` | 1 | 原有 fast 路径总开关；official 大设计 RRR 同样使用 |
| `--tsv-assignment-polish 0\|1` | 0 | Projected 可行分配后的移动/交换 |
| `--tsv-polish-candidates K` | 0 | 0 继承 `--tsv-candidates`；显式范围 0…65536 |
| `--tsv-polish-passes P` | 2 | 最大迭代轮数，范围 1…64 |
| `--tsv-polish-bidirectional 0\|1` | 0 | 启用反向锚点 KD 交换候选 |
| `--tsv-polish-thermal weighted\|pareto` | weighted | 代理改善或 Pearson Pareto 接受规则 |
| `--tsv-polish-heat-weight λ` | 2 | weighted polish 热权重，范围 0…1000000 |

显式设置 polish 预算、热规则或热权重时必须同时启用 `--tsv-assignment-polish 1`。pareto 要求 official 模型，且不能与显式热权重一起使用。实际 polish 扩展仅作用于 projected 分配。

受控 fast 对照应保持输入、热权重、TSV 预算、时间限制及 RRR 参数一致，仅改变 `--fast-quality`。TSV 扩展对照则保持 fast 开关相同，仅逐步改变候选数、pass 或反向查询。下面是直接调用 `eda_router` 时可附加的参数片段：

```bash
# 旧 fast 对照，保留原有 polish 预算
--fast-quality 0 --tsv-assignment-polish 1

# 新 fast，TSV 仍使用原有预算
--fast-quality 1 --tsv-assignment-polish 1

# 另做 TSV 邻域扩展实验；此处数值只是实验配置
--fast-quality 1 --tsv-assignment-polish 1 \
  --tsv-polish-candidates 32 --tsv-polish-passes 8 \
  --tsv-polish-bidirectional 1
```

使用 `benchmark_official.py` 时，脚本会自动注入 `--tsv-assignment-polish 1` 并禁止覆盖。放在脚本末尾 `--` 后的附加参数中应删去这一对参数；完整可运行示例见 [本轮实测报告](第四版算法优化报告_20261015.md)。

每次使用新的输出目录，记录命令、输入/程序/Guide 哈希及独立校验。大设计的 RRR 仍最多三轮、每轮 scope 最多 128 网，仍跳过全设计后续 TSV 外层搜索与 v2/v3 恢复阶段；这些调度未因新增开关自动扩大。

## 5. 复杂度与资源边界

设单侧层数为 L、source shortlist 为 S≤2、target shortlist 为 T≤2、候选路径节点数为 D。新增同层 Z 至多 `4ST` 条，路径构造与计费随 D 增长；首树交点查找使用集合成员判断，约为 `O(D log|targets|)`。单层 L 排序保存 `O(L)` 条记录。为准确选择扩展层，当前实现对单层 L 完整计费，不再沿用该部分旧 incumbent 提前剪枝，因此不能仅凭固定 Z 条数推断总体加速。

TSV polish 设跨 die 网数为 N、可用槽数为 M、候选预算为 K、pass 上限为 P。反向索引新增 `O(N)` 存储及平衡 KD 建树开销；候选评估量约随 `PNK` 增长，单次代理评估还取决于该网终端 MST 的计算成本。KD 近邻通常能剪枝，但最坏情况仍可能访问大量索引节点，不能将每次查询宣称为严格 `O(log M + K)`。候选按需查询，未建立 N×M 的稠密费用矩阵。

本轮未据此承诺固定速度或内存收益；应以同预算实际运行的墙钟、峰值 RSS 及最终 W/C/T 比较。原有 `--time-limit` 仍为内部软预算，分配、初始化和导出阶段也可能影响进程总墙钟。

## 6. 已确认的局部证据与回归

在 case001 中，保持第三版 weighted 热权重 8192 以及原有 TSV polish 预算，仅启用新 fast 的本地 official 模型结果为：

| 指标 | 旧 fast 对照 | 新 fast |
| --- | ---: | ---: |
| 综合分 `100Q` | 69.7795693083 | 70.4868237342 |
| W | 74.5250252317 | 74.8574921018 |
| C | 67.4254349865 | 69.2842494342 |
| T | 64.0627098325 | 64.0627098325 |

证据位于 `output/runs/v4-20261015/case001-legacy-control/` 与 `case001-fast/` 的 `stats.json`、`run.json` 和独立校验记录。热分不变，与保持 TSV 分配相符。这里记录的是自有 official 模型；新 Guide 的原版官方评分状态由主结果报告另行记录，不能用第三版旧 Guide 的原版 PASS 替代。

`tests/fast_quality_tests.cpp` 用可重现图快照覆盖：同格单节点、preferred-only 单轴去除冗余 via、合法 L 受拥塞时同层 Z 更优、首个树交点截断、旧开关对照，以及显式 Dijkstra 成本一致。`tests/tsv_polish_tests.cpp` 和 CLI 回归覆盖新增 TSV 预算与候选行为。测试通过数量及 Release/Debug 完整状态由本轮验证报告记录。

case001 的改善不能直接外推至 case003、case005 或未知样例。最终 Guide 仍须通过真实端点、整数坐标、DIE、连通、TSV 映射、资源账本及导出读回检查；这些检查也不等同于详细布线 DRC 或制造签核。
