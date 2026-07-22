# Guile-first：状态所有权倒置

把编辑器状态的所有权从 C++ 倒置到 Guile：Guile 是组合根、持有一切编辑器状态；C++ 收缩为值内核、IO/传输与渲染三类无状态机制库。

## 定位

横切文档：不新增子系统，重划 [01](01-kernel.md)–[08](08-gui-chrome.md) 已定义机制的**宿主边界**。各子系统的设计立场（机制/策略切分的内容）不变，变的是"机制状态住在哪边"。[../docs/](../docs/scripting.md) 实现文档记录的是倒置前的现状形态；本文落地后 docs/ 需相应改写。

## 实现状态

**未实现——本文是提案**，唯一一篇纯前瞻文档。已发生的先行步：截至 2026-07-22 的最近八个 commit 全部是 "move X to Guile"（transaction group / window walk / display transition / completion selection / settle / session policy / prefix state），证明方向已在逐项执行；本文把逐项偿还升级为一次有终态定义的倒置。三个前置问题（§4 状态模型、§5 事务边界、§7 GC 测量）未定稿前不动手。

## 需求背景

- **R1（规模失控信号，2026-07-22 实测）**：C++ 共 60k 行，其中内核仅 9.3k；editor/ 15.3k + script/ 13.7k + gui/ 11.3k 的壳占三分之二。单文件之最是 `guile_runtime.cpp` 11.2k 行（176 个 `scm_c_define_gsubr` 原语），全部 C++ 的近 1/5 是手写跨界桥。
- **R2（四份账本）**：每个有状态概念要维护四份——native registry/状态（editor/）、逐概念 marshalling 与校验（guile_runtime.cpp）、inspector 序列化（gui/inspection.cpp 3.4k）、Scheme 侧策略镜像。费用正比于**有状态 native 概念的数量**，随功能线性增长，且每一份都要与另外三份保持一致。
- **R3（设计无罪，宿主有罪）**：01–08 的机制/策略切分本身经受住了实现检验；失控的不是设计而是"C++ 应用就 policy 咨询 Guile"的宿主形态。结论（用户判断）：C++ 不应管理编辑器状态。

## 0. 最高层结论

1. **失控的不是内核，是边界**。内核（document/cpp_lexer/syntax/indentation/formatting/commands）9.3k 行、值语义、全绿，是项目资产；40k 行的壳里，真正不可约的只有渲染。
2. **病根是 neovim 形态**：状态住 native，Guile 只做决策，于是每次决策都要"Scheme 返回 plan → C++ 对 native registry 校验 → 应用 → 发布回去"的舞蹈。docs/ 里遍布的 "native code validates the complete plan" 就是这笔税的文面。
3. **倒置后 C++ 只保留三类**：值内核（§3.1）、IO/传输（§3.2）、渲染（§3.3）。native 状态只允许三种形态：**值**（Text/GreenNode）、**句柄资源**（进程/socket/watch/字体）、**可重算派生缓存**（frame layout/语法分析缓存/keymap trie）。其余一律是 Scheme 数据。
4. **Scene 是让 Guile-first 可行的接缝**。Emacs 把 buffer/window/frame 状态留在 C，因为 redisplay 在 C 里逐帧直读它们；cind 的帧边界已物化为 Scene 值——Guile 持有状态、每帧交付一次 compose 输入，渲染侧不伸手进 Scheme 状态。这是 cind 能比 Emacs 倒置得更彻底的结构原因。
5. **桥塌缩**：176 个逐概念原语收敛为少数值类型（snapshot、selection、scene 输入、async task、document 编辑）上的统一 FFI。桥的规模从"概念数 × 4"变为"值类型数"。
6. **inspector 蒸发**：状态即 Scheme 数据，检视 = 结构化打印 + Ares REPL 直查活状态；inspection.cpp 的逐字段序列化不再需要。Emacs 的真正卖点（对活系统的可检视/可修补）在这里兑现。
7. **containment 立场切换**（§6）：从"边界校验、Scheme 不可能破坏 native 不变量"切换到"native 只守值不变量，组合层错误是 Scheme 条件"。这是哲学变更，必须显式接受而非默默发生。
8. **迁移走 strangler，不走 big-bang，也不再走逐 export**：在旧组合根旁立 Guile 组合根，按子系统整块切换并删除旧侧；fixture/pty 测试在行为层，天然支持双跑对比。
9. **三个前置问题先落纸再动手**：Guile 状态模型、事务边界、按键路径 GC 测量。不落纸，倒置会在中途重新长出今天这层桥。

## 1. 现状解剖（2026-07-22）

```
src/editor/       15.3k   组合根：EditorApplication、registries、controller 们
src/script/       13.7k   桥：guile_runtime.cpp 11.2k（176 原语）+ async bridge
src/gui/          11.3k   渲染 7.9k + inspection.cpp 3.4k
src/syntax/        2.9k ┐
src/ui/            2.8k │
src/lsp/           2.6k │
src/indentation/   1.9k ├ 内核 9.3k + 机制库：倒置后基本不动
src/document/      1.8k │
src/async/         1.5k │
src/cpp_lexer/     1.4k ┘
scheme/           8.9k   策略层（core.scm 3.9k）
```

四份账本的典型样例（display placement）：Guile 策略算出 plan → native `apply display plan` 校验实体身份与 layout 不变量 → 应用 → workbench/jump 状态发布回 Guile → inspector 另走一条序列化路径。同一个决策穿过四段代码。对照组：workbench 的 role/pinned/MRU 已经 Guile 持有（[../docs/workbenches.md](../docs/workbenches.md)），native 只剩 WindowLayout 与校验——正是本文终态在一个子系统上的预演。

## 2. 与 01–08 的关系

各文档的"机制（C++）/策略（Guile）"表全部保留**语义列**，宿主列按 §3 重划：原"机制"中凡属状态管理的（registry、controller、session 表、栈、MRU、slot 表）迁 Guile；凡属计算的（trie 查找、fuzzy 打分、noun 求值、diff、重放映射）留 C++ 纯函数。02-buffer 的 Text/undo 树、01-kernel 全部、07-decoration 的重放映射、05-jump 的图存储（anchor 密集）是显式例外——见 §3.4 灰区裁决。

## 3. 边界划分

### 3.1 留 C++：值内核

Text、snapshot-pure 语法分析、缩进查询、Enter 计划、结构原语、undo 树、anchor 结算、diff。理由：从第一天就是 revision-pure 的值库，不持有"编辑器状态"（它持有的是**文档**状态，即值本身）；性能敏感；已被 fuzz/oracle 锁定。倒置后零改动，Document 对象经 handle 暴露给 Guile。

### 3.2 留 C++：IO 与传输

AsyncRuntime（libuv）、LSP transport（进程 + JSON-RPC 框架）、目录 watch、剪贴板、终端/SDL 事件源。这些是句柄资源不是编辑器状态；现有"worker 不进 Guile、完成回调回编辑器线程"的契约（[../docs/async-runtime.md](../docs/async-runtime.md)）原样保留。

### 3.3 留 C++：渲染

compose（纯函数，输入由 Guile 状态导出）→ Scene → presenter（ANSI/Skia）、frame controller、damage、动画、字体整形。frame layout 缓存是派生缓存（§0.3 第三类），不是状态所有权。gui/ 的 7.9k 渲染代码是诚实复杂度，不动；砍掉的是 inspection.cpp。

### 3.4 移 Guile：组合根

buffer/view/window/workbench registry（"哪些实体存在、叫什么、怎么关联"）、mode 与 keymap 所有权、command loop 编排、interaction、placement、completion 会话、location list 当前引用、jump walk、session 持久化。灰区裁决：

- **buffer 文本/undo 树/anchor 结算留 native**（值机械），但"buffer 表"是 Guile 数据：Guile 持 handle→元数据（名字、resource、mode、project、只读位），native 持 handle→Document。
- **keymap trie**：所有权归 Guile（定义即数据），native 保留一个由 Guile 数据编译的只读查找缓存（派生缓存，可随时重建）——热路径查找不进 Scheme，失效协议见开放问题。
- **selection**：anchor id 组成的值住 Guile（最近 commit 已完成），结算仍是 transaction kernel 的义务。
- **jump 图**：节点/边 anchor 密集且量大，存储留 native（05-jump 现状），所有权语义（哪张图属哪个 workbench、walk）在 Guile——维持现状切分，不倒置回来。
- **诊断/装饰存储**（07）：revision 绑定的区间表贴着文档与渲染热路径，留 native 值存储；namespace 注册与策略在 Guile。

## 4. Guile 状态模型（前置问题一）

- **表示**：每 application 一个状态根（延续现有"per-application mutable state keyed by host capability"惯例），子系统为嵌套 record（Guile records，不用 alist 做热路径结构）；集合用 vector/hash-table。
- **身份**：跨边界继续用 generational ID（native 资源的引用必须防悬垂）；Guile 内部结构之间直接引用 record，不绕 ID 查表。
- **检视**：状态根可整体结构化打印；inspector 面板 = 状态投影 + native 侧仅存的值/缓存诊断。Ares REPL 可直接读改活状态（开发红利，也是 §6 风险的另一面）。
- **持久化**：session 序列化直接 write Guile 状态投影（稳定路径而非运行时 ID 的既有原则不变）。

## 5. 事务边界（前置问题二）

一个命令 = native 值效果（document transaction、LSP 通知）+ Guile 状态变更。立场：**native 值效果先提交、Guile 状态随后记录事实**——文档不变量永远由 native transaction 保证；Guile 侧中途出条件时，状态未更新但文档已变，语义等同今天 `buffer-edited!` 观察者失败（诊断保留、权威转换不回滚）。整批定义类操作（extension 加载）沿用现有 checkpoint-rollback。数据不丢是唯一戒律：最坏情形是"编辑器行为错乱但文档与 undo 树完好"，`C-g` + 重载 init 恢复。是否需要跨子系统的 Guile 状态 checkpoint 原语，留开放问题。

## 6. Containment 新立场（前置问题三）

今天：typed boundary 校验一切，Scheme bug 被挡在门口（代价就是 11.2k 行门）。之后：native 只守三类不变量——值合法性（Text/树/anchor）、句柄生命周期、派生缓存一致性；组合层 bug 是 Scheme 条件，表现为命令报错而非进程损坏。接受依据：Emacs 四十年证明该模型对单用户编辑器足够；cind 的文档层甚至更硬（undo 树使任何状态错乱都不威胁数据）。**不接受的部分**：涉及句柄释放的路径（buffer 释放、window 删除）保留 native 校验——悬垂是值不变量问题，不是策略问题。

## 7. 性能前置测量（gate）

倒置把按键路径的编排放进 Scheme（keymap 查找除外，§3.4）。动手前测三件事，任一不达标则采用降级路线：

1. C++↔Guile 往返开销：每键 1 次进入 + 若干原语调用的实测（预期 µs 级）；
2. Boehm GC 停顿：在状态根全量迁入、持续编辑负载下的 p99 停顿；
3. 端到端：522k 行文件每键中位 < 1ms、p99 < 5ms（现状 0.49ms 中位，允许倒置花掉一部分预算）。

降级路线（Emacs 同款）：self-insert 与光标移动留 native fast path，其余命令进 Scheme——机制上就是保留今天的 typed-char 管线直通，只有命令分发进 Guile。

## 8. 迁移路线

- **P0**：§4–§6 三个立场定稿 + §7 测量原型。产物：本文修订 + 测量数据入文。
- **P1**：Guile 状态根骨架 + 已多半在 Guile 的子系统整块收编（workbench 元数据、jump walk、location 当前引用、completion 会话）——删除对应 native 半边与桥原语，验证"删得动"。
- **P2**：buffer/window registry、placement、interaction 迁移；EditorApplication 的编排逐块改为薄转发。
- **P3**：command loop 编排与 keymap 所有权（native trie 降为派生缓存）；modeline/chrome 等 presentation 政策的 fact 收集改为读 Guile 状态。
- **P4**：删除旧组合根路径；guile_runtime.cpp 收缩为统一 FFI；inspection.cpp 改为状态打印投影；docs/ 相应改写。
- **验收**：fixture/pty 行为测试双跑逐字节对比；规模目标（诚实区间）：editor/ + script/ 从 29k 收缩到 8–12k，gui/inspection 3.4k → <1k。

## 9. 非目标

- 不动内核与渲染架构（01/02/Scene-Presenter）。
- 不借倒置夹带新功能；行为逐字节等价是每步验收线。
- 不做多语言扩展 ABI、不回退到 wasm 沙箱路线（[archive/hoot-wasmtime.md](archive/hoot-wasmtime.md) 维持搁置）。
- 不追求纯 Scheme 按键路径——§7 测量否决时 fast path 豁免是设计的一部分，不是妥协。
- 不做协作/多写者（02-buffer 非目标继承）。

## 10. 开放问题

1. 事务边界终态：顺序纪律（§5）是否够用，还是需要跨子系统 Guile 状态 checkpoint 原语（extension 加载已有先例）。
2. keymap trie 派生缓存的失效协议：Guile 定义变更 → 缓存重建的触发粒度（整表 vs 按 map）。
3. generational ID 是否在 Guile 内部也保留一层（调试可读性 vs 双重身份的复杂度）。
4. 多 application 隔离（现有 per-host 状态键控）在状态根模型下的映射——一个 Guile VM 多个状态根的边界执法。
5. inspector 的 native 残余（渲染缓存、damage、动画）与 Guile 状态投影如何在一个面板里拼合。
6. 迁移期间 docs/ 的维护策略：逐步改写 vs P4 一次性改写（倾向后者，期间 docs/ 加"倒置进行中"注记）。
