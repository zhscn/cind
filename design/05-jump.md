# 跳转图与位置列表

导航层设计：一次注意力游走的轨迹如何成为可回溯的图，工具输出的位置集合如何成为一等值，以及由此牵出的可编辑位置视图与跨 buffer 原子 undo。

## 定位

与 [01-kernel.md](01-kernel.md)（内核）、[02-buffer.md](02-buffer.md)（文本层）、[03-input.md](03-input.md)（输入系统）、[04-workbench.md](04-workbench.md)（工作台）并列，属工作台/导航层。依赖 04-workbench.md 的 `display(buffer, intent)` 显示咽喉与 workbench 实体，以及 02-buffer.md 的快照 undo 树。实现文档：[../docs/workbenches.md](../docs/workbenches.md)（跳转图归属与会话序列化）、[../docs/location-lists.md](../docs/location-lists.md)（位置列表）、[../docs/workspace-edits.md](../docs/workspace-edits.md)（跨 buffer 事务）、[../docs/lsp.md](../docs/lsp.md)（语义导航 producer）。

## 实现状态

已实现：

- **跳转图**：native JumpGraph（节点、边、buffer anchor、行列 fallback，节点带 excerpt 内容锚）；per-window walk 与游标由 Guile 持有；显示咽喉自动记边（Guile jump policy 决定 intent 是否入 walk 并赋 EdgeKind）；LSP 导航记 `def`/`ref`/`list` 边；workbench 会话序列化含 durable 图与每个 leaf 的有界 walk。
- **位置列表**：native `LocationListStack`（条目含 resource、行列区间、摘录、元数据、惰性 resolved 锚），resolve 在跳转时与目标 buffer 打开时批量提升；物化为只读 `cind.location-list` generated buffer；`(cind workbench)` 持发布顺序、当前列表与选中索引；producer：项目搜索（ripgrep）、诊断、LSP references。
- **跨 buffer 复合事务**：workspace edit 执行器（全量校验 → per-buffer pending transaction → 全部就绪才提交 → 失败回滚）与 TransactionGroup 注册表；组 undo 是 conflict-aware 的（成员被单独改过则跳过并报告）。
- **组合视图机制层**：`ComposedViewModel`（excerpt 锚定借用、快照投影、编辑穿透并产出 TransactionGroupEntry），有单测。

未实现（正文中标注「后续」）：

- 组合视图的 View/Window/渲染集成——可编辑位置视图的用户可见形态。
- 落点兜底链的内容重锚定（excerpt 邻域搜索）；现状是 anchor + 行列 fallback，条目的 `stale` 标志已预留。
- 策略层形态未核实的项按设计保留、不作已实现断言：手动边原语（`jump.link!`/`jump.mark!`）、`jump-branches` 图查询 picker、图节点 LRU 淘汰上限、列表流式追加（rg 现为一次性解析安装，version 字段已预留）。
- 图形化图 UI（本就可后置的纯策略层）。

## 需求背景（设计约束，源自用户输入）

- **R1（跳转图）**：看代码时的语义跳转天生是 call graph 的子图，但现有编辑器的历史全是线性的，无法承载这个子图的语义。分叉阅读（从一个函数分别看两个 callee）在线性栈里必然丢失一支。概念上的跳转（RPC 两端、配置→代码）相当于手动标记一条边——这类边不属于任何静态分析的产出。→ §3
- **R2（历史碎片化）**：Emacs 的 mark-ring（per-buffer）+ global-mark-ring + xref 自带栈互不相认，"回到我刚才在的地方"没有唯一答案；vim 有 jumplist/changelist/tagstack 三个互相重叠的栈。→ §1.1、§1.2、§3.4
- **R3（位置列表）**：Emacs next-error 是 compilation/grep/occur/flymake 的持续争夺战，"当前错误列表"是薛定谔状态；vim quickfix 方向正确但全局单例 + per-window location list 双轨制造成新混乱；wgrep/occur-edit 证明位置列表还应是可编辑的物化视图。→ §4
- **R4（agent 边界）**：跳转图这类辅助抽象对 agent 是**负面影响**（人类先验限制模型的泛化搜索）；本设计的一切立论只靠人类交互痛点，不为 agent 设计任何消费接口。确定性语义 query（soundness）对 agent 有价值，但那与 editor 本身无关。→ §9 非目标
- **R5（客观图边界）**：fact-extractor 类全程序语义索引（客观图）是独立工具域，不进编辑器；编辑器侧只保留"位置列表 producer"这一个中立接口，设计**不依赖**它的存在。→ §4.1、§9
- **R6（依赖来自 zed 证据）**：跨文件原子 undo 不是用户需求，是可编辑位置视图的**结构依赖**——一次编辑动作跨多个 buffer，undo 不原子则功能即坏。它只占一节，不是支柱。磁盘边界/崩溃恢复不立项。→ §6

## 0. 最高层结论

1. **两张图严格分离**：主观图（一次注意力游走的轨迹，交互中产生，只能是编辑器状态）vs 客观图（程序语义结构，离线工具产出，不进编辑器）。本文档只设计主观图。
2. **跳转图是近零成本的一等机制**：边的唯一写入点就是 04-workbench.md 的 `display(buffer, intent)` 咽喉——请求本就携带 (origin view, intent, 目的地)，恰好是一条带类型的边。Emacs/vim 做不成图的根因是没有这个咽喉（跳转散落几十处，事后靠 advice 猜）；zed 有咽喉但把结构选成了栈（tag_stack 已是 origin/target 成对的边，止步于没连成图）。
3. **图是真相，栈是投影**：图归属 workbench（一次阅读会话的产物），每个 window 持有自己的 walk（游走序列 + 游标）。back/forward 操作 walk，行为与线性历史完全一致；图永久保留 walk 丢弃的分支，"去另一支"是图上的查询。split = 同一张图上两条 walk，天然表达分叉阅读。vim 三栈被统一收编。
4. **位置列表是一等惰性值**：`(resource, 行列区间, 摘录, 元数据)` 的条目序列，grep/LSP references/诊断/编译错误统一产出这一种东西；导航消费惰性态，**按需提升**为 (buffer, anchor)。zed 证明了终局形态（multibuffer）但它的"命中即全量加载"对 cind 不可行（开 buffer = lex+parse），本设计取 zed 自己放弃的惰性混合层。
5. **可编辑位置视图 = View 模型的一次真实扩展**：excerpt 组合视图（View 跨多 buffer），编辑即时穿透回底层 buffer（zed 模型，无 wgrep 式暂存），save 扇出。机制层（ComposedViewModel）已落地；View/渲染集成是后续。
6. **跨 buffer 复合事务**：快照 undo 树下异常便宜——组记录 = `{buffer → (快照A, 快照B)}`，undo = 各自回 A。为组合视图与 LSP rename/code action 服务，不独立立项。
7. **图 UI 是纯策略层**：树状/图形面板可以晚做甚至不做；默认键位体验（back/forward/去另一支）不需要任何新 UI。

## 1. 调研结论

### 1.1 Emacs

"回到刚才的位置"没有唯一机制：mark-ring per-buffer、global-mark-ring 语义混乱、xref 自带独立历史，社区长出 better-jumper/dogears/gumshoe 补丁群——与 persp 同病：目标对，手段是围堵。next-error-function 是二十年争夺战，编译完再 flymake 一下，`M-g n` 走哪个列表不可预测。wgrep 证明"位置列表可编辑"的需求真实，但它是 grep buffer 上的文本 hack + 一次性 apply，与 buffer 状态有分歧窗口。

### 1.2 vim / neovim

三个互相重叠的栈：jumplist（per-window）、changelist、tagstack，`C-o` 和 `C-t` 何时用哪个是老用户都答不准的问题。quickfix 是位置列表作为一等值的正确雏形——可入栈（`:colder`）、可驱动批量编辑（`:cdo`）——但全局单例 + per-window location list 双轨制再造混乱。`:cdo` 的 undo 是碎的（逐 buffer 各撤各的）。

### 1.3 helix

收敛为单一 per-view jumplist——方向对但太粗，什么算一次 jump 硬编码。

### 1.4 zed：导航历史（源码级）

- per-pane 双栈（backward/forward，VecDeque，上限 1024）+ 第三栈 closed_stack（重开 tab）+ **第四结构 tag_stack**：go-to-definition 专用，每条 `{origin, target}` 成对，带游标可双向遍历（pane.rs:4847）——**已承认语义跳转是边，但存成栈**：中间位置发起新跳转即 truncate，分叉丢失。
- 可抄机械：**payload 类型擦除**（历史层只存 item 弱句柄 + 不透明 blob，位置语义由 view 自己 `navigate(blob)` 回放）；**anchor + 行列坐标双存**抗编辑漂移；**回放期切 mode**（GoingBack/Forward/Disabled）抑制自记录、复用同一 push 函数实现双栈对压；**10 行防抖** + 栈内同 item 同行去重；deactivate（离开 buffer）必记一条；paths_by_item 让历史跨 item 关闭存活。
- split 时深拷贝整个历史（fork_nav_history）——两 pane 起点相同、各自演化。本设计用"共享图 + per-window walk"消掉这次拷贝并让分叉语义成立。
- 三种"历史"分层：位置历史（nav）、tab MRU（activation）、project MRU——互不相混，仅共享时间戳源。此分层保留。

### 1.5 zed：multibuffer（源码级）

- **没有 quickfix**。可导航结果和可编辑结果是同一个 multibuffer：excerpt 只存 `(buffer_id, anchor 区间)`，**借用不拥有**；底层 buffer 变化经版本比对懒同步；编辑**即时穿透**（`convert_edits_to_buffer_edits` 后当场调各底层 `Buffer::edit`），无暂存区；save = 扇出保存 N 个真实 dirty 文件。
- 代价：**每个命中文件都被真实加载为 buffer**（project search 的 open_buffers 对每个命中路径 open_buffer），无惰性 file:line:text 表示。zed 用内存换零状态分歧。
- **跨 buffer undo**：multibuffer 级 `Transaction { buffer_transactions: HashMap<BufferId, text::TransactionId> }`（transaction.rs:39），undo 扇出各 buffer 回滚到对应 tx。LSP rename 收敛到同一机制：LSP store 先把 workspace edit 应用到各 buffer 产出 per-buffer transaction，multibuffer 事后 `push_transaction` 登记为一组——rename 后按一次 undo 撤销所有文件。**这是 R6 的证据来源**。
- 生产者（search/diagnostics/references/rename）各自 new MultiBuffer，但 excerpt 装配走统一 API（set_excerpts_for_path 族）。

### 1.6 客观图（fact-extractor，边界结论）

全程序语义索引（call graph 闭包、override 展开、CFG 级查询）是独立工具：独立进程、独立生命周期、可 headless。与编辑器的全部接口 = ①它的查询结果天然是一个位置列表（作为 §4 的一种 producer 接入，无特殊地位）；②若接入，会话按 (kind, ProjectId) 建键复用 04-workbench.md 的会话注册表与 provenance 路由；③facts 位置以构建时刻冻结，编辑器以 anchor 重投影调和陈旧性——这是交互侧唯一编辑器独有的职责。**本设计不依赖它**：没有它，producer 退化为 LSP/grep，一切照常工作。

## 3. 跳转图（主观图）

### 3.1 数据模型

```
JumpNode  { id, buffer, anchor, line_col_backup, excerpt, created_at, last_visit }
JumpEdge  { from: NodeId, to: NodeId, kind: EdgeKind, at }
EdgeKind  = def | ref | search | open | list(位置列表跳转) | manual | ...（开放集，源于 intent）
JumpGraph { nodes, edges }                          // 归属 workbench，一个 workbench 一张
Walk      { entries: [(NodeId | RawPos)], cursor }   // 归属 window，append-only
```

- **节点身份 = (buffer, 量化位置)**：新落点在既有节点的合并半径内（默认同一行 ±0，策略可调）则复用该节点——分叉点因此是**同一个**节点，出边自然汇聚，图才有意义。anchor 抗编辑漂移，行列坐标做 anchor 失效时的回退（zed 双存）。
- **excerpt 内容锚**：节点额外携带锚点处的摘录文本，与 LocationItem 的 excerpt 同一价值观（§4.1）——闭 buffer 重开或外部改动后，节点获得位置提示之外的内容校验依据。
- **归属切分**：图的节点、边、anchor 与行列 fallback 在 native 侧；每个 window 的 walk 序列与游标是 Guile 策略状态（[../docs/workbenches.md](../docs/workbenches.md)）。
- **双层记录**：图只收**语义边**（经咽喉的 intent 跳转、manual 边、位置列表跳转）；walk 额外收**位置项**（view 内选区移动超过防抖阈值——默认 10 行——的落点，RawPos 不建图节点）。这对应 zed 的 nav 双栈（位置粒度）与 tag_stack（语义粒度）二分，但语义层从栈升级为图。
- buffer 关闭不删节点：节点带 path 冗余（zed paths_by_item），回访时重开文件、anchor 从行列回退重建。

### 3.2 写入时机

1. **显示咽喉**（唯一的自动语义边来源）：`display(buffer, intent)` 携带 origin view——origin 的当前位置结算为 from 节点，落点为 to 节点，Guile jump policy 决定该 intent 是否入 walk 并把 intent 映射为 EdgeKind。placement 与 LSP provenance（04-workbench.md §7）消费同一请求的其他字段——一条管道三个消费者。LSP 语义导航与位置列表访问由此自动记 `def`/`ref`/`list` 边（[../docs/lsp.md](../docs/lsp.md)）。
2. **手动边**（策略层，形态未最终核实）：`jump.link!`（对两个位置）与 `jump.mark!`（对当前位置建节点）。R1 的"概念跳转手动标边"就是这一条原语，机制上与自动边完全同构（kind=manual）。
3. **离开 view**（切 window/切 buffer/切 workbench）：当前位置必记入 walk（zed deactivated 规则），保证"离开一个文件时其位置一定可回"。
4. **回放抑制**：back/forward 执行期间 walk/图均不自记（zed mode 切换法：机制在状态里，不在调用方 flag 里）。

### 3.3 归属与游标

- **图归 workbench**：一次阅读会话（一个工作台）一张图。跨 workbench 跳转（罕见）在目标 workbench 的图里生长，不建跨图边。
- **walk 归 window**：每个 window 一条 append-only 游走 + 游标。**split = 新 window 得到空 walk（起点 = 分屏时位置），共享同一张图**——zed 的深拷贝消失，"同图双游标"正是分叉阅读的形状。
- window 关闭：walk 丢弃（图保留一切语义信息）；workbench 会话序列化含 durable 图与每个 leaf 的有界（截断）walk（[../docs/workbenches.md](../docs/workbenches.md)）。

### 3.4 遍历语义（back/forward 与"去另一支"）

- `jump-back` / `jump-forward`：游标沿本 window 的 walk 移动，行为与 vim `C-o/C-i`、zed 双栈**逐键一致**——这是验收底线：不看图的用户感受不到图的存在（对齐 04-workbench.md 的反过度设计护栏）。
- walk 是 append-only：在历史中间发起新跳转不 truncate，只 append 新 visit 并把游标置尾。"前进历史被清空"这一线性栈经典行为消失，被"forward 走 walk 上被跳过的段"取代——语义仍单义（walk 是全序）。
- `jump-branches`（策略层）：列出当前节点在**图**上的出边（含其他 window 的 walk 产生的、含 manual 边），选择即跳——"分叉点回来后去另一支"的正式解，也是 R2 三栈统一后的红利：tagstack 的"回到跳入点"= 入边查询，changelist = 按 kind=edit 过滤（编辑落点由事务 commit 顺带记边，kind=edit，默认开关见开放问题 5）。
- 遍历策略（图上如何选边、防抖阈值、合并半径）全部 Guile 可换；机制只提供 walk 移动与图查询原语。

### 3.5 生命周期与上限

节点/边带 last_visit；图设软上限（默认节点 4096），超限按 LRU 淘汰**无 manual 边**的节点（manual 边是用户显式意图，不自动清）。淘汰是策略，机制只提供 evict 原语。

## 4. 位置列表

### 4.1 一等惰性值

```
LocationItem { resource, range: 行列区间（byte 或 UTF-16 列编码）, excerpt, metadata,
               resolved: Option<(BufferId, AnchorRange, stale)> }
LocationList { id, source(grep/lsp-ref/diagnostic/...), items, materialized_buffer, version }
```

- **所有 producer 产出同一种值**：grep、LSP references/implementations、诊断收集、编译错误解析、（将来）语义 query 工具。producer 是开放集，接口中立（R5）。
- **惰性是默认态**：条目只有 resource + 行列 + 摘录，不触碰文件。百万命中的 grep 不加载任何 buffer。条目坐标保留 producer 的原生列编码（LSP 的 UTF-16、grep 的 byte），到目标 buffer 打开后才换算为 UTF-8 byte 位置——protocol 坐标不在中途失真。
- **excerpt 是承重字段，不只是显示摘录**：它是条目携带的**内容锚**——range 绑位置（可 stale），excerpt 绑内容（跨搬迁有效）。存储谱系的直译：指针里同时编码"位置提示 + 内容校验和"，读路径的正确性不依赖位置提示的正确性（cinnabar 杂交红利；kakoune 只存坐标故只能整批作废，zed 的 anchor 离了活 buffer 无意义故被迫全量加载——cind 两者都不必）。当前实现消费 excerpt 做显示与序列化，内容重锚定链是后续（§4.2）。
- 列表是**值**，可持有多份：`LocationListStack` 保留 durable 列表，`(cind workbench)` 持发布顺序与当前列表（vim `:colder` 的正确版本——归 workbench 而非全局，双轨制消失；"当前列表"永远单义）。新结果发布即成为当前列表；杀掉物化 buffer 只清除关联，列表本身仍可导航。流式追加（异步 producer 按版本号增量刷新）是后续；version 字段已预留，rg 现为一次性解析后整体安装。

### 4.2 提升（resolve）

条目在两个时机从 (resource, 行列) 提升为 (BufferId, AnchorRange)：①实际跳转到该条目；②该 resource 的 buffer 因任何原因打开时，**批量提升列表内同文件条目**（一次遍历，之后该文件的编辑不再使条目漂移）。提升是单向的，列表因此对"边导航边编辑"稳健——这是 wgrep 分歧窗口问题的结构性消解。buffer 被杀时条目退回行列 fallback 位置。

**落点兜底链（后续设计）**——未提升条目落到已被编辑/外部改动的 buffer 时，显式分级而非"尽力"：

1. range 处内容与 excerpt 匹配 → 直接落点（位置提示仍有效）；
2. 不匹配 → 以 range 为中心邻域搜索 excerpt（先精确、后按需放宽），命中即**内容重锚定**——落点并就地提升；
3. 邻域无命中 → 全 buffer 唯一匹配则落，多义或零命中 → 落行列位置并**标记 stale**（UI 可见，不假装准确）。

stale 诊断、fact 类构建时刻冻结的位置（§1.6③）走同一条链。链的搜索半径/放宽规则是策略（scheme），匹配执行是机制。条目的 `stale` 标志已在数据模型中预留。

### 4.3 导航消费

`M-g n`/`M-g p`/`` C-x ` ``（及物化 buffer 内的 `M-n`/`M-p`/`RET`）经显示咽喉以 `intent=list` 跳转——**位置列表遍历自动成为跳转图的游走**（kind=list 的边链），back 天然可用，零附加机制。next-error 争夺战的解 = "当前列表"是 workbench 的单一显式引用，producer 不抢注回调；访问把源 buffer 换进窗口后，导航命令继续沿用保留的列表。选择式消费（picker）走既有 interaction provider，列表就是 provider 的数据源。

### 4.4 物化视图

列表物化为只读 `cind.location-list` generated buffer（language-less major mode，`interface` 交互类，见 [../docs/location-lists.md](../docs/location-lists.md)）——渲染摘录文本即可，不要求提升。**可编辑物化**是 §5 的 excerpt 组合视图（后续）：进入可编辑态时按 excerpt 惰性提升+加载对应 buffer——**加载的是你要改的文件，不是所有命中文件**。这是对 zed"全量加载"的修正：交互路径相同，成本按需支付。

## 5. Excerpt 组合视图（可编辑位置视图）

### 5.1 机制层（已实现）

`ComposedViewModel`：有序 excerpt 列表，每个 excerpt = (BufferId, context AnchorRange, primary AnchorRange)，构造时校验并合并相邻/重叠 excerpt，全部区间转为底层 buffer 的 anchor（借用不拥有）。`snapshot()` 按需重建投影文本与 segment 映射（投影区间 ↔ 底层 buffer 区间）；`apply_edits()` 把投影坐标的编辑映射回各底层 buffer 并**当场提交底层事务**（zed 模型，无暂存区；被改 buffer 在别处的 view 即时可见 dirty），返回 TransactionGroupEntry 列表供 §6 登记为一组。

### 5.2 视图集成（后续）

新增 View 种类 **ComposedView**：Window 1:1 View 不变；ComposedView N:M Buffer 是"View N:1 Buffer"的推广。上下文行数与展开（方向性扩边）同 zed。渲染：每 excerpt 一个 Region + 文件头分隔 Region，Scene/Presenter 零结构改动（帧模型的既有承诺）。save = 扇出保存 dirty 的底层 buffer；buffer→view 方向走既有 revision 比对（zed 懒同步形）。只读↔可编辑切换、上下文行策略归 Guile。

### 5.3 与跳转图/工作台的关系

从 excerpt 跳到真实文件 = 经咽喉的一次 jump（自动建边；落点抑制自记，zed take-nav-history 教训）；back 回到组合视图内原 excerpt 位置（视图是一等 view，walk 项指向它）。组合视图内出现 scope 外文件不改变 workbench 成员语义（visitor 规则照常）。

## 6. 复合事务（跨 buffer 原子 undo）

- **机制**：workspace edit 执行器接收带源 revision 的 per-buffer 编辑集，全量校验（buffer 存在且可写且 revision 匹配、区间含于该 revision、区间不重叠、同一位置多插入拒绝为歧义）后逐 buffer 开 pending transaction、从文档尾向头应用，**全部就绪才提交**；发布失败回滚已提交成员。结果为每个被改 buffer 一条 `TransactionGroupEntry`，登记进 active workbench 的 `TransactionGroupRegistry`——组只是快照对（undo 树边）的关联记录，per-buffer undo 树**完全不动**。组 undo = 对每个成员回到记录边的另一端（走既有 LCA 重放）；redo 对称。Guile 记录组的 undo 方向并决定可用性。
- **消费者**：§5 的跨 excerpt 编辑；LSP rename / code action（协议层归一化为 resource 键的 UTF-16 编辑，策略层解析 buffer、按快照换算区间后提交同一执行器，见 [../docs/lsp.md](../docs/lsp.md)）；将来任何"对列表逐项应用"的批量命令。
- **漂移语义（conflict-aware）**：组 undo 时若某成员 buffer 的当前 undo 位置不再匹配记录的边（用户后来单独改过），**跳过该 buffer 并报告**（部分撤销）；因 undo 树永不丢数据，激进策略（照撤，用户可再 redo 回来）可作为 Guile 选项。这是与 zed（无 undo 树，只能盲扇出）的实质差异点。
- 未打开文件不进组：rename 触碰的文件在应用时必然已加载为 buffer（LSP 应用路径保证）；纯磁盘批量改写不在本设计范围（非目标）。

## 7. 机制 / 策略边界

| # | 机制（C++） | 对应策略（Guile 可换） |
|---|---|---|
| J1 | JumpGraph 存储（节点/边/anchor/excerpt/fallback）；显示咽喉记边；回放抑制 | walk 与游标本身；intent 是否入 walk、EdgeKind 映射、防抖阈值、合并半径 |
| J2 | 手动建点/建边原语 | 何时标、绑什么键 |
| J3 | 图查询原语（出边/入边/按 kind 过滤） | back/forward/branches 的具体语义、picker 呈现 |
| J4 | 图 LRU evict 原语 | 上限、保留规则 |
| L1 | LocationListStack：durable 列表、物化 buffer 关联、锚生命周期 | 发布顺序、当前列表、选中索引（(cind workbench)） |
| L2 | producer 的解析执行（rg 解析、诊断收集、LSP 归一化） | 有哪些 producer、结果排序过滤、显示形态 |
| L3 | 惰性→anchor 提升（跳转时/开 buffer 时批量）、行列 fallback | —（纯机制） |
| V1 | ComposedViewModel：excerpt 锚定、投影快照、编辑穿透 | 上下文行数、合并/展开、只读↔可编辑切换（视图集成后） |
| T1 | workspace edit 执行器 + TransactionGroup 登记 + conflict-aware 组 undo | 漂移时跳过 vs 照撤；undo 方向记录 |

## 8. 与既有设计的衔接

- **[04-workbench.md](04-workbench.md)**：显示咽喉是 J1 的唯一自动边来源（请求携带 origin/intent）；图与当前列表归属 workbench 实体；会话序列化覆盖图与有界 walk；LSP 会话注册表为将来语义 query producer 预留（不依赖）。
- **[03-input.md](03-input.md)**：back/forward/branches 是普通命令，走 keymap；`jump.mark!` 可组合 Noun（对 selection 标节点）。
- **[02-buffer.md](02-buffer.md)**：复合事务零侵入复用 undo 树与 undo_to；diff_edit 复用于脏检查。
- **[07-decoration.md](07-decoration.md)**：诊断列表 = LocationList producer（payload 同源）。
- **[08-gui-chrome.md](08-gui-chrome.md) / Scene**：组合视图 = 新 region 种类；picker = 既有 reflow band。

## 9. 非目标

- 不做客观图/全程序语义索引（独立工具域，R5）。
- 不为 agent 设计任何消费接口或导出格式（R4）；图的存在理由是人类的手和眼。
- 不做图形化图 UI（可后置的纯策略；`jump-branches` picker 已覆盖核心交互）。
- 不做磁盘边界/崩溃恢复/swap 语义（R6 裁决）。
- 不做 zed 式命中即全量加载；不做全局（跨 workbench）jumplist。
- 不做纯磁盘批量改写事务（未加载文件不进复合事务）。

## 10. 场景走查

设计验收基准；涉及后续项（组合视图集成、手动边、兜底链）的场景已标注。

1. **分叉阅读**：在 `f` 处 find-def 进 callee A（边 f→A），back 回 f，find-def 进 callee B（边 f→B）。walk 单义，forward 可达 A；`jump-branches` 在 f 列出 A、B 两条出边。线性编辑器在此丢失 A。
2. **系统头文件连跳后回溯**（连跳依赖 [04-workbench.md](04-workbench.md) §6.2 未实现的 guest 绑定）：guest 绑定保证连跳可行，每跳一条 def 边；深入 6 层后 back×6 原路返回，或 `jump-branches` 看清来路——两机制正交，无一行互相特判。
3. **grep → 导航 → 编辑**：grep 产出列表（零加载）；逐条跳（自动 list 边入图；跳到哪个文件加载哪个、就地批量提升该文件条目）；就地改三处；改动在各 buffer 各自的 undo 树里，无组（散编辑不需要原子性）。
4. **列表物化批量改**（组合视图集成后）：同一列表物化为可编辑组合视图，多光标一次替换 12 处跨 5 文件 = 一个 TransactionGroup；save 扇出 5 文件；一次 undo 全部撤销。
5. **LSP rename 撤销**：rename 应用后登记组；发现改错，一次组 undo 全量回滚；其中一个文件用户已手动继续改过 → 该文件被跳过并报告，其余 4 个干净回滚。
6. **概念边**（手动边落地后）：读 RPC client 时 `jump.link!` 到 server handler；此后两点间双向 `jump-branches` 直达；该边永不被 LRU 清除。
7. **split 分叉**：分屏后左右两 window 从同一节点出发各读一条调用链；图上是同源两条路径；任一侧 `jump-branches` 能看到对侧的足迹（同一张图）。
8. **buffer 关闭后回访**：关掉某文件，back 走到指向它的 walk 项 → 按 path 重开、anchor 从行列 fallback 落点（zed paths_by_item 形）；兜底链落地后再叠加 excerpt 内容校验。

## 11. 开放问题

1. walk 上 RawPos（防抖位置项）与图节点的边界是否会让用户困惑（back 停在"没有边"的位置）——先按 zed 行为走，实测再调。
2. 合并半径的单位：行 vs 语义单元（同一函数体内视为一个节点？）——CST 在手，但先做行，语义量化留策略钩子。
3. 组合视图的 excerpt 内缩进/结构命令（press_enter 等）语义：直接透传底层 buffer 坐标即可，但跨 excerpt 边界的选区如何裁剪。
4. 位置列表的持久化范围：会话序列化当前含跳转图与有界 walk，不含位置列表——是否纳入（全存 vs 只存当前列表）待定。
5. 编辑落点自动记边（kind=edit，changelist 收编）默认开还是关——倾向开（等价 vim changelist），待实测噪声。
