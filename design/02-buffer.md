# 文本存储层

## 定位

本文与 [01-kernel.md](01-kernel.md) 配套：内核定义 Document 语义（revision 化快照、原子 transaction、anchor、缩进服务），本文定义支撑它的文本存储 `Text`。范围是**单机内存中的文本值**及其 undo 衍生；渲染、selection 模型、LSP client 不在此层。

## 实现状态

已实现：持久化 chunked B+-tree `Text`（值语义快照、summary 聚合、行索引/UTF-16 换算内化）、undo 树（快照值的树，`undo_to` 走 LCA 重放）、结构 diff（`diff_edit` / `diff_spans`）、lexer 的 chunk 游标、operator 拼写的 TokenKind 枚举化。内核语义层（transaction、anchor、revision、undo 单元的对外契约）在换底中原样保留，`speculative_snapshot` 从 O(n) 变 O(1)。

未实现：持久化衍生（append-only edit journal / checkpoint，§6）。

---

## 0. 最高层结论

> **持久化（persistent）chunked B+-tree，节点带 monoid summary，值语义快照。**
> 单写者串行提交、读者无锁共享快照；edit list 是唯一的语义差异载体；undo 是快照值的树。
> 手写，不引库。

---

## 1. 需求推导（从内核倒推，不从数据结构正推）

内核（[01-kernel.md §5](01-kernel.md)）定义了 Document 的语义：revision 化快照、原子 transaction、speculative snapshot、anchor、一次 transaction 一个 undo 单元。POC 曾用整串拷贝实现这些语义——语义层是对的，编辑器规模下失效的只是存储层，因此换底不动语义层。

| 需求 | 来源 | 对存储的要求 |
|---|---|---|
| speculative snapshot 在**每键路径**上 | Enter/typed-char 管线（01-kernel §10.3/§11）：插入 → 投机 reparse → 决策 → 提交 | snapshot 必须 O(1)，整串拷贝在 300k 行文件上每键 O(n) 不可接受 |
| revision 化接口 | 增量与全量语义等价的不变量 | 旧 revision 的文本可廉价保留（fuzz 等价性验证要同时持有新旧） |
| 行索引 | LineIndex：offset↔(line,col) 双向 | 内化为树内聚合值，不再是旁挂结构、不再整表重算 |
| UTF-16 换算 | LSP position encoding（内核字节 offset，LSP UTF-16 code unit） | 同上：作为聚合值，O(log n) 换算 |
| 异步读者 | 其他语言 parser、LSP didChange 差分、全文搜索、git gutter、autosave | 读者持有快照期间写者继续前进，无锁、无失效 |
| undo | 一次 transaction 一个单元；undo 树 | 保留历史的内存代价必须是 O(编辑量) 而非 O(revision × n) |
| 增量 reparse | 01-kernel §8.1：长度编码 green tree + 复用 | 编辑表达为 (range, replacement) + 新旧快照 + edit list |

这些需求几乎逐条指向同一件事：**文本是值，不是容器**。

## 2. 方案盘点

| 方案 | 代表 | 判定 |
|---|---|---|
| gap buffer | Emacs | 局部编辑最快、单块内存、线性扫描友好；但无廉价快照，与 revision 模型根本冲突 |
| 行数组 | Neovim (memline) | 长行病态；快照靠拷贝 |
| piece tree | VSCode | 强项是大文件秒开（original buffer 可 mmap）与 append-only 省内存；快照要拷 piece 表，长会话碎片化 |
| rope（chunked，引用计数共享） | Helix (ropey)、Lapce | 现代默认解；clone 近乎免费 |
| **带 summary 的 CoW B+-tree** | Zed (SumTree)、CodeMirror 6 (Text)、JetBrains Fleet | rope 的泛化：叶存 chunk，内部节点存可加聚合（monoid）；快照 = 引用计数 +1；一个容器复用于一切"有单调聚合的序列" |
| 持久化向量（RRB-tree） | immer::flex_vector | 有 O(1) 快照与 O(log n) concat/slice，但**无 summary 扩展点**——行/UTF-16 索引得旁挂第二棵持久树并双树同步，恰好丢掉最值钱的东西 |

采用**带 summary 的持久化 chunked B+-tree，手写**。理由：

- summary 是载荷最重的能力（行索引 + UTF-16 + green tree 的长度和是同一模式），不能没有扩展点；
- C++ 生态在这里是空的：libstdc++ 的 `__gnu_cxx::rope` 是 SGI 遗物且无 summary，Boost 无对应物，immer 差关键能力。手写不是洁癖，是默认选项；
- 组件规格清晰、不变量强、可对照平凡模型 fuzz（§8），是编辑器计划里风险最低的"硬"组件。

## 3. 结构规格

```
Text            —— 不可变值。拷贝 = 根指针引用计数 +1。
  内部节点       —— fanout 8–32（B 树而非二叉：缓存行友好、树浅），
                   每 child 一份 Summary，节点自身存子树 Summary 之和。
  叶子           —— 不可变 chunk（KiB 量级，取值以 bench 为准，§8），引用计数共享。

Summary（monoid：有单位元、可结合、可加）
  { bytes, lines, utf16_units }        —— 可扩展 grapheme/display 维度
```

- **编辑 = 复制根到叶的一条路径**：O(log n) 个新节点（每键几百字节），其余全共享。
  没有叶内 delta/overlay 优化——那类机制是为摊薄**设备**写放大而生的；内存里改一个 2 KiB chunk
  就是拷 2 KiB，没有需要摊薄的东西。
- **查询**：offset↔(line,col)、offset↔utf16、slice、chunk 迭代，全部 O(log n) 起步、
  沿 summary 下降。LineIndex 是 summary 查询的薄封装，不再是独立结构。
- **拼接/切分** O(log n)：B 树 concat/split 标准算法，供大粒度操作（整块粘贴、文件加载分片构建）。

## 4. 并发模型

- **单写者，串行提交**。编辑器主线程的 transaction 本就串行，串行提交零额外成本。
- **读者 = 无锁快照**。任何异步消费者（其他语言 parser、LSP 差分、搜索、autosave）拿走一个
  `Text` 值即隔离；写者继续前进，互不感知。引用计数（原子）是唯一同步点。
- **multi-cursor 不是并发，是写集宽度**：N 个光标的编辑 = 一个 transaction 内 offset 有序的
  edit list（transaction/anchor 语义直接覆盖）。可并行的是**读侧**——N 个光标的缩进查询是
  同一快照上的纯函数，天然可并行；apply 仍是串行合并。
- 协作编辑（CRDT）为非目标（§7），但值语义结构不排斥将来叠加。

## 5. Undo = 快照值的树

undo 单元 = `(snapshot: Text, edits: EditList, caret)`。结构共享使保留**整个会话历史**的代价为
O(总编辑字节 × log n)，因此：

- **undo 是树而非栈**：分支历史（undo 后继续编辑不丢弃旧分支）在可变 buffer 编辑器里出了名地难，
  在值语义下就是一棵廉价值的树加两个指针。`undo_to` 走 LCA 路径重放。
- **edit list 仍是唯一的语义差异载体**：anchor 调整、LSP didChange、增量 reparse 的输入都是它；
  快照是值、edit list 是变化的**解释**，两者并存不冗余。
- **任意两个 revision 的结构 diff 是 O(变化) 的**（共享子树整棵跳过）。这是一个通用原语：
  `diff_edit` 产出编辑序列，`diff_spans` 是它的不物化兄弟（sign 列消费，01-kernel §17）；
  undo 预览、对 saved 版本的 gutter diff、didChange 增量差分都是同一原语。
- 回收 = 丢弃：undo 树上被裁掉的分支释放其未共享节点，无独立 GC。

跨 buffer 的复合事务（LSP rename、组合视图编辑的一次 undo）建立在本层之上：组记录只是快照对的关联，per-buffer undo 树不动。见 [05-jump.md §6](05-jump.md) 与 [../docs/workspace-edits.md](../docs/workspace-edits.md)。

## 6. 后续设计：持久化衍生（未实现）

buffer 本身不承担 durability——磁盘文件才是 durable 载体，save = 临时文件 + fsync + rename 原子
发布（已实现，走异步运行时，见 [../docs/async-runtime.md](../docs/async-runtime.md)）。跨会话的
undo/崩溃恢复（vim undofile/swapfile 的原理版）是可选衍生，未实现；若做，形态如下：

- **per-file append-only edit journal**：record = 一个 transaction 的逻辑 edit list
  （记逻辑意图而非树节点字节——不依赖任何内部表示，格式天然稳定）；
- 帧格式 = `len + CRC32C` 自定界：CRC 验完整性、len 给 torn-tail 检测；
  **不完整的尾帧整条丢弃**（未落盘的 transaction 原子消失）；
- checkpoint = sidecar 文件原子发布（temp → fdatasync → rename → 父目录 fsync），
  发布落定后才裁剪 journal 前缀——**durable-before-trim**，崩溃后要么旧-未裁要么新-已裁；
- fsync 纪律可放松：编辑器丢失最后几百毫秒的击键可接受，按时间/字节批量刷。

## 7. 非目标

1. mmap 秒开 GB 级文件（piece tree 的独门强项；个人 C++ 编辑器的文件尺寸不需要）；
2. CRDT / 协作编辑；
3. 叶内 delta/overlay、版本号过滤等面向设备写放大的机制（§3 已述其不适用）；
4. 并发写者（真需要时先做"并行准备 + 串行验证提交"，而不是放开写锁）；
5. buffer 层的文本编码转换（内核统一 UTF-8 字节，编码转换在文件 IO 边界一次完成）。

## 8. 测试与度量

- **模型对照 fuzz**：随机编辑序列同时打在 `Text` 与 `std::string` 上，全量比对文本、行索引、
  UTF-16 换算（沿用 Document 的 fuzz 策略）；
- **不变量**：summary 一致性（每节点聚合值 == 重算值）、快照隔离（持有旧快照期间写者任意编辑，
  旧快照字节不变）、引用计数无泄漏/无环（Asan + 计数审计）；
- **结构 diff 正确性**：diff(a,b) 应用到 a 得 b（随机对 fuzz）；
- **chunk 大小与 fanout 以实测为准，不猜**：以 bench 语料（LLVM 树）的每键路径
  （插入 + speculative snapshot + 增量 reparse）与大文件加载为基准；
- **接入 bench**：`indent-core bench` 的 parse/indent 计时是换底的回归护栏
  （lexer 走 chunk 游标后的顺序扫描代价在这里现形）。

## 9. 与内核的衔接

1. **Document 语义层原样保留**：transaction、anchor、revision、undo 单元的对外契约不变，
   底下的整串拷贝换成 `Text` 值；`speculative_snapshot` O(1)。
2. **lexer 走 chunk 游标**：lexer 是顺序单字符扫描，cursor 跨 chunk 前进；
   token 存 offset range 而非指针。
3. **Punctuator 拼写枚举化**：parser 与 T3 引擎不做 token 文本比较（`"->*"`、`"<<="`…）——
   chunked 存储下跨 chunk 取文本需小拷贝，全部关心的 operator 拼写升为 TokenKind
   （lexer 一次判定），热路径是 kind 比较。
4. **green tree 同构**（[01-kernel.md §8.1](01-kernel.md)）：长度编码语法树的"节点存长度、
   父节点存和"就是 summary monoid 的特例；增量 reparse 的输入 = 旧快照（免费持有）+
   新快照 + edit list——"与 buffer 联合设计"在此闭合。
