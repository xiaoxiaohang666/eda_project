# 3D IC 多目标全局布线课程项目

这是早期算法开发仓库。项目包含 C++ 路由器、评估脚本和测试。主要实现位于 `src/`，构建入口为 `bash scripts/build.sh Release`。

## 课程开发与贡献记录

| 仓库 | xiaoxiaohang666 主作者提交 | CatsLove1 主作者提交 |
| --- | ---: | ---: |
| [3dic-routing-lab](https://github.com/xiaoxiaohang666/3dic-routing-lab) | 181 | 0 |
| [eda_project](https://github.com/xiaoxiaohang666/eda_project) | 27 | 2 |
| 合计 | 208 | 2 |

截至课程源码发布前，两个仓库的全部分支与标签共记录 210 次唯一提交，xiaoxiaohang666 担任其中 208 次的主作者（99.0%）。排除 15 次合并提交后，主作者计数为 193 和 2。该统计反映 Git 署名，无法直接衡量工时。主仓库有 150 次提交带有 Claude 共同作者标记，历史中保留了这些标记。

早期开发见 eda_project，后续集成见 3dic-routing-lab。队友的两次提交位于早期仓库的开发分支。完整克隆后可执行以下命令核验各仓库的课程记录：

```sh
git shortlog -sne --all --invert-grep --grep='^Publish course project overview$'
```

公开版保留原始提交的主作者、时间和合并关系。为保护隐私，公开内容集中于项目源码和经过检查的技术文档。部分仅修改内部材料的历史提交在公开版中没有文件差异。公开说明单独提交，未计入上表。
