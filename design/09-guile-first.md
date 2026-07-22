# Guile-first：状态所有权倒置

把编辑器状态的所有权从 C++ 倒置到 Guile：Guile 是组合根、持有一切编辑器状态；C++ 收缩为值内核、IO/传输与渲染三类无状态机制库。

## 定位

横切文档：不新增子系统，重划 [01](01-kernel.md)–[08](08-gui-chrome.md) 已定义机制的**宿主边界**。各子系统的设计立场（机制/策略切分的内容）不变，变的是"机制状态住在哪边"。[../docs/](../docs/scripting.md) 实现文档记录的是倒置前的现状形态；本文落地后 docs/ 需相应改写。

## 实现状态

**P0 与 P1 已完成，P2 未开始**。先行步：本文之前的八个 commit 已是逐项 "move X to Guile"（transaction group / window walk / display transition / completion selection / settle / session policy / prefix state）；本文把逐项偿还升级为一次有终态定义的倒置。

进度：

- **§7 性能 gate 已完成并通过**（测量工具 `indent-core guile-perf` 已入库；结论：按键路径 95% 的时间今天就在 Scheme 内，倒置预期减负而非加负，真正风险是 GC 停顿）。
- **§4 状态模型已实现且迁移完成**：`(cind state)` 状态根落地，10 个模块的 41 张 per-host 表全部收编为声明槽位；测试全绿、按键路径实测与基线持平（0.114 ms vs 0.115 ms）。
- **§5 事务边界与 §6 containment 已定稿**：§5 把既有的"native 值效果先提交、Guile 事后记录"形态正式化并否决了跨子系统状态 checkpoint；§6 用 P1 实测得到的"校验挂失败路径"规则回答了"省校验会不会变脆"。P2（buffer/window registry）的前置条件因此齐备。

## 需求背景

- **R1（规模失控信号，2026-07-22 实测）**：C++ 共 60k 行，其中内核仅 9.3k；editor/ 15.3k + script/ 13.7k + gui/ 11.3k 的壳占三分之二。单文件之最是 `guile_runtime.cpp` 11.2k 行（176 个 `scm_c_define_gsubr` 原语），全部 C++ 的近 1/5 是手写跨界桥。
- **R2（四份账本）**：每个有状态概念要维护四份——native registry/状态（editor/）、逐概念 marshalling 与校验（guile_runtime.cpp）、inspector 序列化（gui/inspection.cpp 3.4k）、Scheme 侧策略镜像。费用正比于**有状态 native 概念的数量**，随功能线性增长，且每一份都要与另外三份保持一致。
- **R3（设计无罪，宿主有罪）**：01–08 的机制/策略切分本身经受住了实现检验；失控的不是设计而是"C++ 应用就 policy 咨询 Guile"的宿主形态。结论（用户判断）：C++ 不应管理编辑器状态。

## 0. 最高层结论

1. **失控的不是内核，是边界**。内核（document/cpp_lexer/syntax/indentation/formatting/commands）9.3k 行、值语义、全绿，是项目资产；40k 行的壳里，真正不可约的只有渲染。
2. **病根是 neovim 形态**：状态住 native，Guile 只做决策，于是每次决策都要"Scheme 返回 plan → C++ 对 native registry 校验 → 应用 → 发布回去"的舞蹈。docs/ 里遍布的 "native code validates the complete plan" 就是这笔税的文面。
3. **倒置后 C++ 只保留三类**：值内核（§3.1）、IO/传输（§3.2）、渲染（§3.3）。native 状态只允许三种形态：**值**（Text/GreenNode）、**句柄资源**（进程/socket/watch/字体）、**可重算派生缓存**（frame layout/语法分析缓存/keymap trie）。其余一律是 Scheme 数据。
4. **Scene 是让 Guile-first 可行的接缝**。Emacs 把 buffer/window/frame 状态留在 C，因为 redisplay 在 C 里逐帧直读它们；cind 的帧边界已物化为 Scene 值——Guile 持有状态、每帧交付一次 compose 输入，渲染侧不伸手进 Scheme 状态。这是 cind 能比 Emacs 倒置得更彻底的结构原因。
5. **桥塌缩，但不是无差别塌缩**（P1 实测修正）：原设想是把 176 个逐概念原语收敛为少数值类型上的统一 FFI。实际清点发现，**桥不只服务 bundled 模块，它同时是 [../docs/scripting.md](../docs/scripting.md) 记录的扩展 ABI**——有 11 个原语当前无 bundled 调用方（`bind-remap!`、`keymap-bindings`、`observe-input-state-changes!` 等），但它们是用户 init.scm 的公开能力，不是死代码。因此可删的是**冗余往返**而非"概念粒度"本身：真正该消失的是那些"状态已在 Guile、native 只做转发"的原语（§8 已删六个）。剩余原语的重量在每个都手写的校验与 marshalling 样板（每个约 15–30 行），那是可以用统一封装收敛的部分，且不牺牲类型安全。
6. **inspector 蒸发**：状态即 Scheme 数据，检视 = 结构化打印 + Ares REPL 直查活状态；inspection.cpp 的逐字段序列化不再需要。Emacs 的真正卖点（对活系统的可检视/可修补）在这里兑现。
7. **containment 立场切换**（§6）：从"边界校验、Scheme 不可能破坏 native 不变量"切换到"native 只守值不变量，组合层错误是 Scheme 条件"。这是哲学变更，必须显式接受而非默默发生。
8. **迁移走 strangler，不走 big-bang，也不再走逐 export**：在旧组合根旁立 Guile 组合根，按子系统整块切换并删除旧侧；fixture/pty 测试在行为层，天然支持双跑对比。
9. **三个前置问题先落纸再动手**：Guile 状态模型、事务边界、按键路径测量。不落纸，倒置会在中途重新长出今天这层桥。第三项已完成（§7）：实测推翻了"倒置会吃掉按键预算"的担忧——95% 的按键时间本就在 Scheme 内，48.5 次/键的跨界往返正是要消除的东西；风险改判为 GC 停顿，约束是大宗数据不进 Guile 堆。

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

## 4. Guile 状态模型（前置问题一，已实现）

倒置前 Scheme 侧本身也是碎片化的：**41 张各自为政的 `(define x-states (make-weak-key-hash-table))` 散在 10 个模块**（command 11、lifecycle 8、core 6、minibuffer 5…），共 132 处 `hashq` 存取，每张按 host capability 索引。后果与 native 侧同构——应用状态没有单一的值，无法枚举、检视、整体释放或序列化，每个子系统还要自带一份"取配置否则用默认"的样板。

**`(cind state)` 是唯一的状态根**（`scheme/cind/state.scm`）：每 application 一张表，持有**声明式命名槽位**。声明先于使用是关键——检视与释放遍历声明表，而不是猜哪个模块拥有什么。

两种槽位，因为生命周期不同：

| 槽位 | 语义 | API |
|---|---|---|
| `state` | 可变运行时数据。首次引用时由 initializer 物化，此后保留至应用释放 | `define-state-slot!` / `state-ref` / `state-set!` / `state-update!` / `state-clear!` |
| `policy` | 配置进来的过程。未配置即用声明的默认值；无默认值的策略是必需的，未配置时报错 | `define-policy-slot!` / `policy-ref` / `policy-set!` / `policy-configured?` |

- **身份**：跨边界继续用 generational ID（native 资源引用必须防悬垂）；Guile 内部结构之间直接引用值，不绕 ID 查表。
- **多 application 隔离**：根表以 host 为**弱键**——释放是优化而非正确性要求，应用消失后其状态自然可回收。槽位**声明**是进程级的，槽位**值**是每应用的（开放问题 4 由此关闭）。
- **检视**：`application-state-snapshot` 返回 `#(name kind value-or-'unset)` 序列，按槽名排序——这就是替代 per-subsystem 序列化器的东西（§0.6）。Ares REPL 可直接读改活状态。
- **持久化**：session 序列化直接写状态投影（稳定路径而非运行时 ID 的既有原则不变）。

**迁移已完成**：10 个模块的 41 张 per-host 表全部收编为 41 个声明槽位，`make-weak-key-hash-table` 在 `scheme/cind/` 下只剩状态根自己那一处。

| 模块 | 槽位 | 代表性收敛 |
|---|---|---|
| command | 11 | 8 个表现层策略（display/modeline/chrome/theme/style/motion/metrics/typography）原本各有一对"配置/解析"函数、每对重复"取表→未配置则报错→调用"，塌缩为 8 行声明加薄封装 |
| lifecycle | 8 | 4 个必需策略 + startup placeholder / buffer saves / edit observers / style origins |
| core | 6 | pending opens、buffer cycle、session restore、project search、LSP navigation、jump link origins |
| minibuffer | 5 | interaction、histories、navigation、selection + history 策略 |
| workbench / pointer / lsp / development / completion / application | 各 1–2 | — |

一个反复出现的模式值得记录：许多槽位的"无值"本身有语义（`#f` = 无活动 interaction / 无补全会话 / 未在浏览历史），初始化器返回 `#f` 即可保真，`state-clear!` 与旧的 `hashq-remove!` 语义一致。

**热路径代价与修正**（迁移中实测发现，值得作为后续迁移的前车之鉴）：槽位访问在按键路径上，天真实现让每次访问从"1 次哈希查找"变成 3 次——声明表（校验种类）+ 弱根表 + 槽表——同机况实测 **+17%**（中位 0.115→0.135 ms、每次跨界调用 2.65→3.14 µs）。修正是把根拆成 state / policy 两张表：命中 = 根查找 + `vector-ref` + 一次哈希查找，而**用错访问器的读取自然落空进冷路径**，在那里查声明并报错——种类校验的语义完整保留，但它只在出 bug 时才付代价，不再占据热路径。修正后回到基线（0.114 ms / 2.63 µs）。教训：状态根是所有子系统的公共通道，其每次访问的常数会被调用次数放大，必须按热路径对待。

## 5. 事务边界（前置问题二，已定稿）

一个命令 = native 值效果（document transaction、LSP 通知）+ Guile 状态变更。

**立场：native 值效果先提交，Guile 随后记录事实。** 文档不变量永远由 native transaction 保证；Guile 侧中途出条件时，状态未更新而文档已变。这不是新设计，而是**既有形态的正式化**——今天 `buffer-edited!` 观察者、`command-result!`、`lsp-buffer-edited!` 全部在权威转换之后运行，观察者抛条件只留下诊断、不回滚已提交的编辑。P1 迁移未触及这一顺序，41 个槽位仍是"事后记录"语义。

三条推论：

1. **不引入跨子系统的 Guile 状态 checkpoint**（开放问题 1 就此关闭）。整批*定义*类操作（extension 加载）已有 checkpoint-rollback 且继续保留——那是注册表语义，不是编辑语义；而单条命令的状态变更本就应与已提交的文档效果同向，回滚状态只会让二者失配。
2. **最坏情形是有界的**：编辑器行为错乱而文档与 undo 树完好，`C-g` 加重载 init 即恢复。数据不丢是唯一戒律，这条不依赖 Guile 侧的正确性。
3. **顺序即契约**：需要"要么都发生要么都不发生"的操作（跨 buffer 重构）已经有 native `TransactionGroup`（[05-jump.md](05-jump.md) §6），原子性由值层提供，不靠状态层补。

## 6. Containment 新立场（前置问题三，已定稿）

今天：typed boundary 校验一切，Scheme bug 被挡在门口，代价是那道门本身。之后：native 只守三类不变量——**值合法性**（Text/树/anchor）、**句柄生命周期**、**派生缓存一致性**；组合层 bug 是 Scheme 条件，表现为命令报错而非进程损坏。接受依据：Emacs 四十年证明该模型对单用户编辑器足够；cind 的文档层更硬（undo 树使任何状态错乱都不威胁数据）。

**不接受的部分**：涉及句柄释放的路径（buffer 释放、window 删除）保留 native 校验——悬垂是值不变量问题，不是策略问题。

**校验放哪里：P1 给出了可推广的技术。** 状态根的种类校验最初放在每次访问上，实测占按键路径 +17%（§4）。改成"用错访问器的读取自然落空进冷路径、在那里查声明并报错"后，**语义一字未减而热路径归零**。推广为规则：

> 校验不该按"要不要"取舍，而该按"放在哪条路径"设计。凡是只在 bug 时才为真的检查，都应挂在失败路径上而非成功路径上。

这条同时回答了"倒置会不会因为省校验而变脆"：不会——被删掉的是**冗余往返**（§8 的 window policy：native 校验窗口存在，然后回调 Scheme 取值，而 Scheme 侧的 `require-window-entry-with-workbench` 本就在校验更强的条件"窗口属于某工作台"），不是校验本身。

## 7. 性能前置测量（gate，已完成）

工具：`indent-core guile-perf <file> [keystrokes]`（`src/cli/main.cpp`）驱动真实 `EditorApplication` 的完整按键路径（`handle_key` → 未消费则 `insert_text`），光标每 8 键漫游到伪随机行以免退化为单点最优；`run_guile_call` 是唯一的 C++→Guile 入口，在此计数与计时（`src/script/guile_call_stats.hpp`，计时按需开启，编辑会话不付钟表开销）。语料为 LLVM `llvm/lib/Support/*.cpp` 拼接。Release，空闲机况（**测量必须在无并发构建下进行**：与构建争用时同一档位的尾部从 5ms 劣化到 21ms）。

| 语料 | 中位 | p90 | p99 | max | Guile 调用/键 | Guile 占比 | 每次调用 |
|---|---|---|---|---|---|---|---|
| 21k 行 | 0.122 ms | 0.159 ms | 0.308 ms | 6.47 ms | 48.5 | 94.4 % | 2.97 µs |
| 78k 行 | 0.126 ms | 0.180 ms | 0.464 ms | 4.86 ms | 48.5 | 94.8 % | 3.08 µs |
| 522k 行 | 0.123 ms | 0.191 ms | 0.363 ms | 7.53 ms | 48.5 | 95.0 % | 3.16 µs |

### 7.1 结论：问题被测反了

**按键路径今天已经在 Scheme 里**——每键 48.5 次进入 Guile，占按键时间 **95%**；内核（增量 relex + reparse + 缩进 + 提交）只占剩下的 5%，且中位数从 21k 到 522k 行几乎不变（0.12 ms）。原 gate 设想的"把编排搬进 Scheme 会不会吃掉预算"是个伪问题：编排早就在那里，native 侧留下的只有状态与校验。

由此，**倒置的预期是减负而非加负**：今天每次决策都要"取状态→过界→算→回写"的往返，48 次调用正是这笔账；状态归 Guile 后，同一个决策在 Scheme 内直接读自己的数据，跨界次数应显著下降。3 µs/次的边际成本乘以调用数才是真正的支出项。

gate 判定（原阈值中位 < 1 ms、p99 < 5 ms）：中位 0.12 ms（8× 余量）、p99 0.36–0.46 ms（10× 余量）——**通过**，且降级路线（self-insert fast path）当前无需启用。

### 7.2 真正的风险：GC 尾部

max 4.9–7.5 ms 的尖峰 **99.6 % 以上落在 Guile 内**，周期约每 200–250 次按键一次，**幅度与文档规模无关**（21k 与 522k 同为 ~5–7 ms）——即 Boehm 对 Guile 自身堆的整堆回收，由每键的稳定分配速率驱动，与内核数据无关（文本/树/token 在 native 堆）。

这把风险从"吞吐"移到"停顿"，并给出明确的设计约束：

1. **大宗数据永不进 Guile 堆**（§3 的三类 native 状态本就如此）：状态根持 handle 与元数据，文本、树、token、诊断区间表留 native。倒置若把 per-buffer 大数组搬进 Scheme，周期会缩短、停顿会变长——这是唯一需要划红线的地方。
2. **每键分配速率是可优化量**：48 次调用的 marshalling 是当前分配的主要来源，跨界次数下降会同时降低 GC 频率。
3. **迁移期需要回归监测**：每个 P 阶段后重跑本工具，盯 `calls/keystroke`、`guile share` 与尖峰周期三项；周期显著缩短即为倒置引入了新的按键路径分配。

顺带记录一个既有缺陷：~200 键一次 5–7 ms 卡顿在今天已经存在（约每 20 秒一次连续输入的可感顿挫），与倒置无关，可独立处理（Boehm 参数调优或降低每键分配）。归入开放问题 7。

### 7.3 未采用 perf-cpp 的理由

评估过 [perf-cpp](https://github.com/jmuehlig/perf-cpp)（Linux perf_event 的 C++ 封装，硬件计数器：cycles/instructions/cache-miss/branch-miss）。本机 `perf_event_paranoid = 2`，用户态计数可用，技术上可接入。不在本阶段引入：gate 要回答的是"时间花在界的哪一侧"，墙钟 + 调用计数已给出 95% 这个决定性答案；剩下的尾部问题是 **GC 停顿**，需要的是 GC 统计（回收次数、堆大小、mark 时间，Guile 的 `(gc-stats)` 直接可得），硬件计数器不直接回答。若将来要优化 marshalling 本身（3 µs/次里指令数、cache miss、分支预测各占多少），perf-cpp 是合适工具，届时按可选依赖引入（Linux-only，需 guard macOS 构建）。

## 8. 迁移路线

- **P0**：§4–§6 三个立场定稿 + §7 测量原型。§7 **已完成**（工具入库、数据入文、gate 通过）；§4/§5 待定稿。每阶段结束重跑 `guile-perf` 作回归监测（§7.2）。
- **P1（已完成）**：Guile 状态根骨架 + 全部 41 张 per-host 表收编；window policy 的 native 往返删除（六个原语、114 行，见下）。**"删得动"已验证**：状态既已在 Guile，转发型原语可以整组删掉而行为逐字节不变（既有 workbench 测试经真实命令覆盖整条路径）。
  - 同时得到一个边界认识：`workbench-slot` 这类原语连 Scheme 调用方都没有，而另 11 个无 bundled 调用方的原语是**扩展 ABI**（§0.5）——清理桥面时必须区分"冗余往返"与"公开能力"，后者不能按死代码处理。
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

1. ~~事务边界终态是否需要跨子系统 Guile 状态 checkpoint~~ — 已定：不引入，理由见 §5 推论 1。
2. keymap trie 派生缓存的失效协议：Guile 定义变更 → 缓存重建的触发粒度（整表 vs 按 map）。
3. generational ID 是否在 Guile 内部也保留一层（调试可读性 vs 双重身份的复杂度）。
4. ~~多 application 隔离在状态根模型下的映射~~ — 已定：槽位声明进程级、槽位值每应用，根表以 host 弱键索引（§4）。
5. inspector 的 native 残余（渲染缓存、damage、动画）与 Guile 状态投影如何在一个面板里拼合。
6. 迁移期间 docs/ 的维护策略：逐步改写 vs P4 一次性改写（倾向后者，期间 docs/ 加"倒置进行中"注记）。
7. 既有的 GC 卡顿（~200 键一次 5–7 ms，§7.2）如何处理：Boehm 参数调优（增量/分代模式、堆下限）vs 降低每键分配（跨界次数下降是倒置的顺带收益）。需要先用 `(gc-stats)` 确认回收类型与堆规模，再决定是否独立立项。
