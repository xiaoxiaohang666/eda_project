# 第二版：真实端点直连与逐网线长恢复

日期：2026-10-03。分支 `codex/algorithm-v2-wirelength`，从第一版 `fe9b979` 创建。代码、构建和测试均在 `.`；保留 `llh` 的第一版。第一版教学见 [详细指南](algorithm-v1-detailed-guide.md)。

## 为什么选这两个方向

第一版已经把 sample001 的 Guide/物理图边溢出降到 0。三个固定 vlink 的上下接口 Pin 对齐，在现有一网一个 TSV 政策下不能任意换到热区，因此此次优先优化线长。

按未改评分器拆解，203 个 Net 中有 200 个双 Pin 网，其 Guide 线长减 reference 的差值共 556427.5 DBU，占全图差值的 69.6%；其余三个跨 Die 网占 30.4%。`BOTTOM__l0_b24` 的 Pin HPWL 仅 1425 DBU，第一版 Guide 代理线长却达 15165 DBU。必须减少真实终端到 GCell 中心的绕行，而不能只增加相同路径候选的 RRR 轮数。

## 已实现的变化

1. **端点 L/Z 路径**：针对同 Die 双 Pin 网，在真实坐标上生成 H→V、V→H、H→V→H、V→H→V 候选。最多选 6 个 H/V 层对，每段都检查首选方向、线宽、Die 边界、已解析 OBS 和 via 包络。无长度的折线段直接跳过，避免无用换层。候选失败保留原 A* 路线。
2. **完整资源预留**：显式路径跨越哪个 GCell 边界，就占用哪条图资源；普通 via 也保留资源所有权。`explicit_geometry` 防止编码时又输出一份中心线绕行；路径长度只按真实显式段累计一次，森林修复遇此类路径改用整网候选。Guide 需求仍从输出矩形独立计算。
3. **独立线长恢复**：原有 RRR 和 TSV 阶段之后，按 Guide 线长余量排列全部 Net。三轮搜索的现时拥塞权重依次为 0.25、0.05、0，历史权重暂时归零；每次先撤掉自己的占用，再生成候选，测量完整解，接受或恢复两个账本。降低搜索价格不等于放松最终零溢出要求。
4. **全局边际比较**：恢复阶段选择候选时，使用 `(其他网线长+候选线长)/(其他网reference+候选reference)`，避免用单网比值代替全局目标。接受时要求两种溢出、物理线长估计、原始 Guide 线长均不增加，并至少有溢出、原始长度或 via 数之一严格下降，再通过完整比较键与连通性检查；仅扩大 reference 无法被接受。

默认 `--pin-local 1 --wirelength-rounds 3`。添加 `--pin-local 0 --wirelength-rounds 0` 可重现第一版。统计新增 `pre_recovery_metrics`、`pin_local_candidates`、`wirelength_trials`、`wirelength_accepted`，其他 CLI 保持兼容。

主要代码：`src/router/pin_local_router.cpp`，以及 `negotiated_router.*` 的候选接入、编码、统计与调用顺序；评分器和 benchmark 未修改。

## 同输入独立评分结果

2026-10-03，冻结第一版二进制与当前 Release 二进制共五组，均通过未修改固定评分器，并逐项核对 C++ 与 Python 指标。Q 为本地 45%/30%/25% 加权质量值，不是官方竞赛总分或排名。

| 版本 | 100Q | 物理线长估计 DBU | Guide MST DBU | Guide reference DBU | 两种溢出 |
|---|---:|---:|---:|---:|---:|
| 冻结第一版 | 74.106872 | 2542455 | 2672382.5 | 1872805 | 0 / 0 |
| 第二版关闭新增功能 | 74.106872 | 2542455 | 2672382.5 | 1872805 | 0 / 0 |
| 仅端点候选 | 79.236661 | 1933505 | 2004927.5 | 1633605 | 0 / 0 |
| 仅线长恢复 | 75.659788 | 2532405 | 2527692.5 | 1858635 | 0 / 0 |
| 完整第二版 | **81.018701** | **1929495** | **1909682.5** | 1631625 | **0 / 0** |

完整第二版相对第一版提高 6.911829 个质量分点；物理线长估计减少约 24.11%，原始 Guide MST 减少约 28.54%。reference 同时变小，这次收益不是扩大分母得到的。线长分约 85.4396，拥塞分 100，热分 50.2835336；实体 TSV 与推断统计点均为 3，原有 Pin 未改变。

代价也需记录：普通 via 估计由 901 增至 1221；当次命令墙钟从约 0.439 秒增加至 2.773 秒，属于单次机器/样例观察，非稳定性能基准。完整版本试了 609 次线长替换，接受 8 次；搜索展开 28711 次。后续应加入 via/RC 权衡与更多基准，不能把单样例改善外推为所有设计改善。

可随 Git 获取的指标与哈希快照见 [algorithm-v2-results.json](algorithm-v2-results.json)。完整本机证据：`output/runs/router-v1-v2-pm_6tg3t/comparison.json` 及 `comparison.md`。每组保存 commands、Guide、router_stats、独立 evaluation_report、manifest；汇总保存输入、两版二进制、评分器哈希。生成目录被 Git 忽略。关闭开关后的 Guide SHA-256 与冻结第一版完全一致。

## 运行与验证

```bash
cd .
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 scripts/compare_router_versions.py \
  --v1-binary output/runs/v2-development-20261002/eda_router_v1
```

冻结二进制是本机证据，不随 Git 分发。换机器时需从第一版提交构建参考二进制，再传入 `--v1-binary`；不要用当前默认第二版冒充第一版。对照脚本拒绝覆盖非空结果目录。

Release 全部 9/9 个 CTest 目标通过；Debug 使用 `-g -Og` 并启用 AddressSanitizer/UndefinedBehaviorSanitizer，也已分两次完成全部 9/9 个目标。前七组在额度暂停前通过，恢复后 `algorithm_v2` 与 `pin_local` 分别用时 115.91 秒和 0.29 秒并通过，未发现 sanitizer 错误。新增端到端测试覆盖关闭开关复现、确定性、完整解不退化、原始长度改善、短预算保留完整解和无效参数保护。新增 C++ 小场景覆盖同格短网、真实跨格资源占用与撤回、不同层 Pin、上 Die、无用 via 消除、OBS 拒绝及 A* 回退、显式路径不误用森林修复。

## 下一步优先级与边界

1. **via 与 RC 权衡**：本次真实平面长度减少但 via 增多，下一版应报告长度-via Pareto 曲线，加入有依据的 RC 或 via 约束。
2. **跨 Die 长网和高扇出树**：联合选择 root access、支持真实端点直接接入已有枝条，消除长主干末端重复 access。保留原 Pin 和完整几何；不能只删矩形来改变代理分数。
3. **增量完整指标**：当前每次恢复都重算全部 Net，可缓存不变网的贡献，重点验证撤回、重复覆盖和全局比值的一致性。
4. **更细的物理资源和多用例验证**：显式路径仍映射到 GCell 边容量；格内 track assignment、详细 spacing、完整 CUT 规则和电气参数未实现。至少加入拥挤、高扇出、OBS 密集和非均匀网格基准，再判断泛化收益。
5. **TSV 政策有新证据后再扩展**：只有新增输入或正式规则允许移动，才扩大热优化空间。固定原样例的热分不变是预期结果。

局部模板目前只覆盖双 Pin、少量层对和中点 Z；没有实现连续几何全局最优。路径检查与零溢出是全局布线代理证据，不代替详细布线或 signoff DRC。
