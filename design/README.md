# cind 设计文档

本目录是 cind 的中文设计文档：记录各子系统的设计依据（需求约束、调研结论、方案取舍）与尚未实现的方向。与 `../docs/` 的分工：**docs/ 是已实现子系统的实现文档（英文，现状权威）**；设计与实现描述不一致时以 docs/ 为准。每篇设计文档开头的「实现状态」小节标明已实现与未实现的边界。

## 阅读顺序

按架构层次自内向外排列，每篇只依赖排在它前面的文档：

| # | 文档 | 层次 | 内容 |
|---|---|---|---|
| 01 | [01-kernel.md](01-kernel.md) | 内核 | 语义缩进 / C++ 语法内核：lossless lexer、容错 CST（green tree）、增量 reparse、缩进服务、Enter 管线 |
| 02 | [02-buffer.md](02-buffer.md) | 内核 | 文本存储层：持久化 chunked B+-tree、值语义快照、undo 树 |
| 03 | [03-input.md](03-input.md) | 交互 | 输入系统：keymap / InputState / modal 策略包 / Noun（selection、thing、motion） |
| 04 | [04-workbench.md](04-workbench.md) | 工作台 | project / window / layout 管理：workbench、placement（intent + slot）、LSP 会话路由 |
| 05 | [05-jump.md](05-jump.md) | 工作台 | 跳转图与位置列表：主观图 + walk、LocationList、excerpt 组合视图、跨 buffer 复合事务 |
| 06 | [06-completion.md](06-completion.md) | 语言服务 | 补全管线：provider 扇出、世代取消、条目模型、语法门控 |
| 07 | [07-decoration.md](07-decoration.md) | 语言服务 | 装饰层：文本区间元数据的三分法、诊断通道、虚拟文本与 fold |
| 08 | [08-gui-chrome.md](08-gui-chrome.md) | 呈现 | GUI 视觉规格：N Λ N O 风格色板、modeline / minibuffer 度量 |
| 09 | [09-guile-first.md](09-guile-first.md) | 横切（提案） | 状态所有权倒置：Guile 是组合根，C++ 收缩为值内核 + IO + 渲染 |

层次关系：

```text
08 gui-chrome          呈现（视觉规格）
06 completion  07 decoration    语言服务界面
04 workbench   05 jump          工作台与导航
03 input                        交互
01 kernel      02 buffer        内核（语法 / 文本）
────────────────────────────────
09 guile-first         横切提案：重划以上各层机制的宿主边界
```

## 存档

- [archive/hoot-wasmtime.md](archive/hoot-wasmtime.md) — 扩展语言调研（Hoot Scheme on wasmtime），已搁置；实际采用的脚本层是内嵌 Guile（见 [../docs/scripting.md](../docs/scripting.md)）。

## 对应的实现文档

| 设计文档 | 实现文档（docs/） |
|---|---|
| 01-kernel / 02-buffer | （内核以测试与 bench 为准，无独立实现文档） |
| 03-input | [command-loop.md](../docs/command-loop.md)、[scripting.md](../docs/scripting.md) |
| 04-workbench | [workbenches.md](../docs/workbenches.md)、[projects.md](../docs/projects.md)、[lsp.md](../docs/lsp.md) |
| 05-jump | [location-lists.md](../docs/location-lists.md)、[workspace-edits.md](../docs/workspace-edits.md)、[workbenches.md](../docs/workbenches.md) |
| 06-completion | [lsp.md](../docs/lsp.md)、[scripting.md](../docs/scripting.md) |
| 07-decoration | [diagnostics.md](../docs/diagnostics.md) |
| 08-gui-chrome | [gui-architecture.md](../docs/gui-architecture.md)、[gui-inspector.md](../docs/gui-inspector.md) |
