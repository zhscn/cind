# 装饰层 — 文本区间元数据的存储、平移与渲染

## 定位

装饰层回答「元数据如何附着在文本区间上」——Emacs 用 text property + overlay 统一承担的东西：诊断、virtual text、inlay hint、搜索高亮、fold、生成式 buffer 的结构标注。本文档依赖 [02-buffer.md](02-buffer.md) 的快照 undo 树与 edit list、[01-kernel.md](01-kernel.md) 的 green tree、src/ui 的 Scene/Presenter 帧模型（Region + Prim + compose_line 的 Run）、[04-workbench.md](04-workbench.md) 的 LSP 会话（诊断生产者）。诊断通道的实现现状以 [../docs/diagnostics.md](../docs/diagnostics.md) 与 [../docs/lsp.md](../docs/lsp.md) 为权威。

## 实现状态

**已实现**（②类外部事实的第一个落地，形态见 §2.2——与 §3.2 的目标形态不同，是其保守特例）：

- 诊断存储：`DiagnosticSet` 按 producer（owner）分组、绑定出生 revision、按 owner 整组替换；**stale 集合直接隐藏**——只有匹配当前 Buffer revision 的集合参与查询与呈现，编辑立即排除旧诊断，等 producer 对新 revision 重新发布。没有 revision 重放。
- LSP 生产者：publishDiagnostics → per-session owner，UTF-16 → 字节转换在解析到打开的 Buffer 之后；stale revision / 已关闭资源 / 非归属 session 的通知被忽略。
- 呈现与消费：gutter sign 列与变更 sign 共用（诊断优先于变更 sign，最高严重级选 glyph 与样式）；`diagnostic.list`（`C-c l e`）生成 `cind.location-list` buffer；`buffer-diagnostics` Scheme 查询；inspector 在 `editor.buffers` 暴露计数。
- ①类派生装饰持续现算：token 高亮、选区、光标行、diff sign。

**未实现**（本文其余部分，正文均已标注）：AnnotationSet 通用存储与 revision 戳惰性重放（§3.2）、行内合并管线与显式优先级（§4）、Run 三态虚拟文本（§5.1）、fold 与 virt_lines（§5.2）、生成式 buffer 结构注入（§6）、inlay hint / semantic tokens 等后续 namespace。

## 需求背景（设计约束）

- **R1（空白确认）**：前序设计未覆盖"元数据附着在文本区间"这一层。设计时的空白：`StyleClass` 预留了 Diagnostic 四个槽位但无供给通道；诊断、virtual text、inlay hint、搜索高亮既无存储也无平移机制。（诊断的存储与呈现现已落地，见「实现状态」；行内合并与其余装饰仍空白。）
- **R2（按现成方案，自主处只留一处）**：五编辑器源码调研结论——现代实践收敛在**按数据来源三分**（§0.1），分歧只在②类外部事实的存储选型上。cind 按收敛面实现；自主决策只有一处：②类平移走 **revision 戳 + 惰性重放**（快照 buffer 与历史一等公民的直接兑现，kakoune 路线的内核化，§3.2）。
- **R3（magit 压力测试）**：magit 是 Emacs text property 最深的用法——buffer 当 DOM。分析结论：magit 几乎完全落在②类通道之外（内容与结构同趟生成、整体刷新、无用户编辑 → 锚定税的两个起因都缺席），不冲击三分法；真正被它拉出来的是 **fold/内容替换视图变换**（§5）与**生成式 buffer 显式化**（§6）两根线。关键事实：magit 折叠的 section 内容 inline 在 buffer 里、只是视图层隐藏（行号变化可证）——但 Emacs 把可见性也存成 buffer 域 property/overlay，cind 的 fold 归 view 域（§5.2）。
- **R4（agent 边界，沿袭 [05-jump.md](05-jump.md)）**：不为 agent 设计装饰消费接口；立论只靠人类交互痛点。

## 0. 最高层结论

1. **按数据来源三分，三类三种待遇**——五家调研的收敛面，本设计的骨架：
   - **① 派生可重算**（语法高亮、选区、光标行、括号匹配、diff sign）：零存储，渲染时视口内现算。重算成本 = 本地计算，随时可再来。
   - **② 外部事实**（LSP 诊断、inlay hint、semantic token）：必须存 + 随编辑平移。重算成本 = 网络往返，且异步到达时 buffer 已变——存与平移是被异步性逼出来的，不是审美。
   - **③ 瞬时视图态**（搜索匹配、jump label、当前 section 高亮）：视图域平坦存储或每帧现算，不 remap，用完即弃。
2. **派生数据永不物化进 buffer**。Emacs jit-lock 把 face/fontified 当 property 写进主存储，被迫用 `with-silent-modifications` 整套"假装没写"（绑掉 undo、压掉 hooks、文档自认 "undo data may become corrupted"）+ idle 补染 + antiblink 补丁 + 假写入强制重画。现代四家集体裁决：派生数据存主表是错的，正确形态是非物化视图。
3. **位置跟踪与视觉 payload 分层**。neovim 表述最直白：marktree 管坐标、decoration 管样子，"decoration 只是 extmark 的一个应用"。cind 对应：②类的坐标问题归 revision 重放（buffer 层既有能力），payload 归 AnnotationSet（本层新机制）——不发明第三棵坐标树。
4. **虚拟文本永不占文档坐标**。四家铁律（nvim virt_text、zed InlayMap transform、helix VirtualText 记 0 codepoint、kakoune ReplacedRange atom）。cind 落点：Run 模型三态化（§5.1），虚拟内容只存在于 compose 产物中。
5. **优先级必须显式数值，归策略层**。nvim 最工程化（priority<<16|subpriority + 生态梯度 50/100/125/150/200）；zed 藏在枚举声明序、helix 藏在 Vec push 序——两家自认"脆弱但简单"。cind 有 scheme 策略层，直接给数字梯度，不走隐式序。
6. **②类平移 = revision 戳 + 消费时惰性重放**（自主决策，目标形态——未实现）。外部事实入库带出生 revision；渲染消费时把区间从出生 revision 映射到当前 revision，映射函数 = undo 树 LCA 路径上的 edit list 重放（`undo_to` 同一套基建）。kakoune 用 option+timestamp 在插件侧做的事，cind 在内核里做，且因 undo 树是快照树，**undo/redo 后的映射天然正确**（nvim 需要 u_extmark 专门补）。已实现的诊断通道（§2.2）是它的保守特例：stale 即隐藏，相当于 stale 阈值为零、不做映射。
7. **fold/内容替换是视图层坐标变换，不是装饰**。与虚拟文本注入同族（Zed FoldMap/InlayMap 同构、kakoune replace-ranges/ranges 同构），影响光标移动、点击命中、滚动。fold 状态归 view 域（split 两侧可各自折叠——Emacs invisible 归 buffer 域是又一处混装）。
8. **生成式 buffer = 结构由生产者交付的 document**，不是新 buffer 类型。magit section 树就是一棵注入的 green tree；"光标在哪个 hunk"= `node_at` 同一查询。Emacs 需要 property 是因为它的 buffer 没有树、只能把结构打散成区间标注贴回文本；cind 的 buffer 本来配树，magit 只是换了树的来源。
9. **机制/策略切分走全**：存储、重放映射、合并管线、fold 变换 = C++ 机制；优先级梯度、样式解析、stale 阈值、gravity 默认、折叠策略 = scheme。

## 1. 调研结论

### 1.1 Emacs（反面教材，源码级）

- text property 底层：按**文本量**平衡的二叉 interval 树（intervals.c:443 "Balancing is by weight"），property 存 Lisp plist 线性查找；GC sweep 时整树重平衡。
- **property 是文本的一部分**：string 内嵌 interval 树（lisp.h:1569）、kill/yank 全程携带（editfns.c:1619）、undo 恢复（undo.c:225）、**改 property 走完整 buffer 修改协议**（modiff + hooks，textprop.c:77-94）；`equal` 忽略 property 而另设 `equal-including-properties`；yank 需要 12 项默认剥除清单（simple.el:6280）擦屁股。
- overlay：29 前双链表 + 手动 `overlay-recenter` 摊 O(n)；29 起 itree（增广红黑树 + 子树 lazy offset，编辑平移 O(log n)），但 `next-overlay-change` 摊还仍 O(N)（itree.c:65-97 bug#58342 自认）——而它是渲染 stop_charpos 扫描的原语。
- 两套机制并存且语义各异：查找 overlay 恒优先（textprop.c:646）；gravity 一边是 per-overlay 布尔 front/rear-advance、一边是逐属性 front-sticky/rear-nonsticky + 全局默认表——同一个"边界打字"问题两套规则。
- jit-lock 病理见 §0.2。三类数据（派生 face、外部事实、瞬时高亮）挤在同一机制里，是本设计三分法的反向证明。

### 1.2 neovim（②类的锚定树答案）

- 分层：marktree（B-tree 坐标，子树相对偏移 O(log n) splice）+ decoration（payload）。MTKey 内嵌 8 字节 union：纯高亮 inline（~12 字节），virt_text/sign/多重高亮走全局池 + 堆链 out-of-line；kind 冗余 bit + 子树 meta 计数支持"不用不付费"的按类过滤遍历（decoration.c:1190 "Only pay for what you use"）。
- **双供给路径的分工准则**（官方消费方验证）：treesitter 高亮 = decoration provider + `ephemeral=true`，每次重绘现算、完全不进 marktree；LSP 系（semantic token/inlay hint）= 持久 extmark + provider on_win 视口内惰性物化。semantic_tokens.lua:645 长注释明说 ephemeral 不可用的原因：LSP 响应滞后于编辑，ephemeral 会闪烁、stale token 对不齐——**"同步可现算→ephemeral；异步会过期→持久锚定"是被逼出的分界线**。
- 渲染 = 扫描线 + active 集按 `(priority<<16|subpriority, 插入序)` 排序 + `col_last` 缓存把逐列查询摊到 mark 边界；生态优先级梯度 syntax 50 < treesitter 100 < semantic 125 < diagnostics 150 < user 200（hl.lua:12-18）。
- **漂移是锚定的固有税**：删除区 mark 只收拢不删；invalidate/undo_restore 选项、undo header 存 extmark 位置（u_extmark）、官方 diagnostic 模块自己挂 on_lines 快照重放恢复（_handlers.lua:66-88）——全是事后补丁族。

### 1.3 helix（②类的 eager remap 答案）

- 语法高亮纯拉模式：每帧新建 highlighter 只喂视口字节区间，span 从来不存（存的是增量语法树）；injection 多层 = 事件流按字节序 merge。
- 三类 annotation（坐标全是裸 char idx，无锚定）：InlineAnnotation（inlay hint）/ Overlay（jump label）/ LineAnnotation（inline diagnostics 虚拟行）；每帧临时借用聚合，虚拟文本经 doc_formatter 以 `GraphemeSource::VirtualText`（0 codepoints）进文字流——不占文档坐标。
- **外部事实 = 存 + apply 时 eager remap**：诊断/inlay hint/document highlight 各在 `apply_impl` 里手写一段 `ChangeSet::update_positions`（作者自留 TODO 想改 hooks）；塌缩区间直接删除；为词边打字正确性，`Assoc` 长出 BeforeWord/AfterWord 特化 + 诊断入库预计算词边界标志——**eager remap 的复杂度税**。
- inlay hint 生命周期：3 倍视口缓冲带请求、`(first_line,last_line)` 身份键去重、编辑后 remap 旧数据继续显示 + outdated 标志、idle 后重拉——"remap 旧数据遮掩陈旧窗口"是有意的防闪烁设计。
- 优先级 = Vec push 顺序（syntax < whitespace < overlay，overlay 内后 push 胜），无显式系统。

### 1.4 zed（②类的 Anchor 答案 + 视图变换的完全体）

- 语法高亮同 helix 拉模式：视口行现跑 QueryCursor，chunk 只带 HighlightId（主题槽位），颜色解析推迟到 editor 层；interpolate/reparse 双版本号让 reparse 期间用平移过的旧树出高亮。
- **域划分**：诊断在 **buffer 域**（`TreeMap<ServerId, SumTree<DiagnosticEntry<Anchor>>>`，Summary 带 min_start/max_end 支持区间剪枝；且经 CRDT op 同步给协作端；按 server 整组替换无增量）；text/background highlights、inlay、fold 在**视图域**（HighlightKey 键控的平坦 HashMap + Vec<Range<Anchor>>）；最终样式合成每帧派生不存。Anchor 身份使平移免费——但这是 CRDT 轴的地基费换来的（cind 在补全设计中已对质过：快照轴用 revision 重放拿到等价能力）。
- **DisplayMap 五层 transform**（inlay→fold→tab→wrap→block）：内容注入（InlayMap/BlockMap）、内容替换（FoldMap）、纯坐标变换（TabMap/WrapMap）；层间通信 = Edit 补丁流，语义是"待失效区域"而非精确 diff（正确性可退化为全量重算）；每层 `TransformSummary{input,output}` 双侧汇总支撑 O(log n) 双向坐标换算。**内容注入与内容替换同族**是 §5 的直接依据。
- 优先级藏在 HighlightKey 枚举声明序（注释自认 "the order is important"），BTreeMap 遍历序即合并序——脆弱但简单。
- 自认痛点：SyntaxSnapshot 析构丢后台线程（TRIM 平滑的编辑器版）、空诊断 range 扩一 codepoint 才画得出下划线、fold 不跨 excerpt。

### 1.5 kakoune（③类的极端 + ②类的重放答案）

- highlighter = 对 DisplayBuffer 的纯变换函数（highlighter.hh:33 注释原话），每帧 clear 后全量重跑管线；DisplayAtom 三态 **Range / ReplacedRange / Text**——§5.1 Run 三态化的直接原型。
- **唯一的持久坐标通道**：range-specs/line-specs option 自带 buffer timestamp（= 只增 change log 的索引），渲染时 `update_ranges` 惰性重放 `changes_since(ts)` 切片做坐标级精确平移；kak-lsp 的诊断/inlay hint/semantic token 全走这条窄门零私有机制。**"生产者带戳存快照坐标、消费者按需重放"就是 cind §3.2 的原型**——kakoune 放在插件边界做，cind 放进内核，且 undo 树快照使跨 undo 的重放天然正确。
- 代价自认：regex 缓存任意编辑全弃、change log 只增不删（重放成本 ∝ 编辑距离）、一切跨帧语义绕道 option 序列化字符串。

### 1.6 收敛与分歧总表

| | 派生①（语法高亮） | 外部事实②（诊断/inlay） | 瞬时③ | 虚拟文本 | 优先级 |
|---|---|---|---|---|---|
| Emacs | **物化进 buffer**（反面） | property/overlay 混装 | overlay | before/after-string | overlay priority + face merge 序 |
| neovim | provider ephemeral 现算 | 持久 extmark（marktree 锚定） | ephemeral | virt_text（渲染期注入） | 显式数值梯度 |
| helix | 每帧视口现算 | char idx + eager remap | 存不 remap | VirtualText 0-codepoint | push 序（隐式） |
| zed | 视口 QueryCursor 现算 | Anchor（CRDT 身份） | 视图域平坦表 | InlayMap transform | 枚举序（隐式） |
| kakoune | 每帧管线现算 | option + timestamp 重放 | 帧属性 | ReplacedRange atom | 管线复合序 |
| **cind** | TokenBuffer/green tree 现算（已实现） | **revision 戳 + 惰性重放**（§3.2 目标形态；现状 = 诊断 revision 绑定隐藏，§2.2） | 视图域现算（§3.3） | Run 三态（§5.1，未实现） | scheme 数值梯度（§4，未实现） |

## 2. cind 基建与实现现状

### 2.1 既有基建

| 现有 | 位置 | 与本设计的关系 |
|---|---|---|
| `build_line_runs`：token 高亮 + 选区切分现算 | src/ui/compose_line | ①类已是模范生（green tree 主数据比四家的 tree-sitter 派生树硬）；§4 在此扩展合并管线 |
| `StyleClass` 含 Diagnostic 四槽位 + `presentation_role` 映射 | src/ui/style.hpp | ②类的样式出口；诊断已接通供给（§2.2 的 gutter 呈现），行内 Run 合并未接（§4） |
| `line_signs`/`diff_spans` sign 列 | src/ui、document | ①类第二实例（对 saved_text_ 的 diff 现算），佐证现算路线可行（0.31ms/帧@522k）；诊断 sign 与其共列 |
| 快照 undo 树 + edit list（唯一语义差异载体） | [02-buffer.md](02-buffer.md) | §3.2 重放映射的全部基建；`undo_to` 的 LCA 路径重放同构复用 |
| revision/世代模式 | [06-completion.md](06-completion.md) §3.2、[05-jump.md](05-jump.md) | ②类"生产者带戳"的既有惯例（LSP 响应绑请求时刻 revision） |
| Scene = Region + Prim；Run 行内模型 | src/ui | §5 虚拟文本/fold 的落点；新 widget 零结构改动承诺 |
| anchor + excerpt 内容锚 | [05-jump.md](05-jump.md) §4 | fold 存储（§5.3）与生成式 buffer 光标恢复（§6.2）的坐标基建 |

### 2.2 诊断通道（已实现）

②类的第一个、也是最保守的落地形态（[../docs/diagnostics.md](../docs/diagnostics.md)、[../docs/lsp.md](../docs/lsp.md)）：

- **存储**：`DiagnosticSet` = owner（producer 身份）+ 出生 Buffer revision + 若干 `Diagnostic`（UTF-8 字节区间、severity、message、source、可选 code）。发布时校验 owner/revision/区间/消息；**按 owner 整组替换**，多 producer 并存互不覆盖——LSP publishDiagnostics 语义，与目标形态（§3.2）一致。
- **平移的替代**：不平移。**只有匹配当前 Buffer revision 的集合参与查询与呈现**——编辑立即隐藏 stale 注解（不触碰 producer 状态），producer 对新 revision 重新发布或在服务卸下时显式清除。正确性靠"宁缺毋 stale"，可行性靠"诊断天然会被重新发布"（didChange 同步后 server 必然再发一版）。
- **LSP 生产者**：per-session owner；校验文档 URI 与版本；UTF-16 区间只在解析到打开的 Buffer 之后转换；stale revision、已关闭资源、非归属 session 的通知被忽略。
- **呈现与消费**：gutter sign 列与变更 sign 共用一列，诊断在其行上优先，最高严重级选 glyph 与样式；`diagnostic.list`（`C-c l e`）从当前 revision 的诊断生成 `cind.location-list` buffer（= [05-jump.md](05-jump.md) 位置列表的一个 producer，字节坐标，因为入库时已对当前快照解析）；`buffer-diagnostics` 供 Scheme 构建上层工具；inspector 在 `editor.buffers` 报告 count/errors/warnings。

这个形态覆盖不了两类需求：**stale 窗口内旧注解仍应显示并随文本平移**（inlay hint 的防闪烁、编辑风暴中的诊断连续性），以及**重拉昂贵、不随每键重发布**的 namespace（semantic tokens）。§3.2 的重放是它的升级路径：落地时诊断迁移为一个 namespace，「编辑即隐藏」退化为 stale 阈值为零的策略特例。

### 2.3 剩余差距

- ②类存储已有诊断特例，未通用化：无 namespace 注册、无 payload side table、无 revision 重放（inlay hint 无处安放）；
- Run 只表达真实字节，无虚拟文本/替换态（§5.1）；
- compose 合并硬编码（token + 选区），无优先级机制——诊断目前只呈现在 gutter sign 列与位置列表，不参与行内 Run 合并（§4）；
- 无 fold（§5.2）；无生成式 buffer 概念（§6）。

## 3. 三条通道

### 3.1 ①派生通道：维持现算，不新增机制（现状即设计）

语法高亮（现 token 层，将来 CST 结构层）、选区、光标行、括号匹配、diff sign 全部保持"渲染时从主数据现算"。CST 结构高亮层升级时仍走同路：compose 时按视口行查 green tree（`node_at`/按层二分已是 0.25ms 量级），不预存 span。**永不把①类结果写进任何持久结构**（§0.2 红线）。

### 3.2 ②外部事实通道：AnnotationSet + revision 重放（目标形态，未实现）

**存储**：per-buffer、per-**namespace**（生产者身份：`lsp-diagnostics(session)` / `inlay-hints(session)` / 将来 `semantic-tokens`）的平坦有序区间表：

```
Annotation { range: ByteRange,        // 出生 revision 坐标系
             gravity: {start_bias, end_bias},
             payload: PayloadRef }    // namespace 自有 side table 的索引
AnnotationSet { namespace, revision,  // 整组共享出生 revision
                Vec<Annotation> 按 range.start 有序 }
```

- **整组替换**（zed/LSP publishDiagnostics 语义）：同 namespace 新一批到达即整组换，无增量合并。组内共享一个 revision 戳——LSP 响应绑其请求时刻的 revision（[06-completion.md](06-completion.md) 世代模式同款）。已实现的 DiagnosticSet 在这两点上（per-owner 分组、整组替换）与目标形态一致，迁移只动平移语义。
- **payload 间接化**：Annotation 本体只有坐标 + 引用；severity/message/hint 文本等存 namespace 自有 side table（nvim inline/out-of-line 分档的简化版——我们的"inline"就是纯坐标，一切 payload out-of-line）。渲染只需 severity → style；跳转/悬停消费完整 payload（诊断列表 = [05-jump.md](05-jump.md) LocationList 的 producer，同一 payload 源）。

**平移 = 消费时惰性重放**：渲染（或任何消费者）拿到 AnnotationSet 时，若 `set.revision != current_revision`，用 undo 树上两 revision 间的 edit list 合成映射函数，把**视口内命中的区间**映射到当前坐标系（视口窗口先二分再映射，万条诊断只映射可见的几条）。要点：

- 映射函数 = `undo_to` 的 LCA 路径 edit list 重放（[02-buffer.md](02-buffer.md) 既有），helix `map_pos` 的等价物但**跨 undo/redo 天然正确**——undo 只是树上另一条路径，nvim 的 u_extmark 专门补丁在这里是免费定理。
- 映射结果**可选缓存回写**（set.revision 推进到当前）——等价于把惰性重放摊成 helix 式 eager remap，但发生在消费侧而非 apply 热路径，打字热路径零成本（apply_impl 不碰任何 annotation）。是否回写、多久回写归策略。
- **塌缩即删除**（helix retain_mut 教训）：映射后 start≥end 的区间丢弃；整组 revision 距离超过 stale 阈值（编辑数或时间）→ 标记 stale（淡显或隐藏，策略），并触发重新请求——不做 kakoune 式无限重放。
- gravity：默认 start=右吸附、end=左吸附（编辑不吞入两端）；诊断词边 sticky 问题（helix 的 Assoc 四变体税）记开放问题 O2，首期用简单默认。

**选型论证**（vs 另两条路）：不建第三棵坐标树（nvim marktree）——cind 没有"任意多生产者高频写锚点"的负载，②类是低频整组替换 + 视口窗口消费，平坦表 + 二分够用；不做 apply 热路径 eager remap（helix）——打字是 0.49ms 预算的圣域，每类外部事实往 apply 里加一段手写 remap 正是 helix 作者自己想逃的形状。重放函数是纯模块，若将来负载证明需要，存储可升级为区间树而消费方接口不变。

### 3.3 ③瞬时视图态通道：视图域，不 remap

搜索匹配（增量搜索本来每键重算）、当前 section 高亮（光标 + 树查询每帧现算）、jump label 类模态标注（模态结束即弃）。归 view，平坦存储或纯现算，编辑时直接失效重算，不进②类通道。**判据：重算成本 ≈ 本地遍历且数据源就在手边 → ③；重算 = 异步往返 → ②。**

## 4. 合并管线与优先级（未实现）

`build_line_runs` 扩展为分层 patch 合成（helix `patch` 语义，非整体替换）：

```
base（①语法：StyleClass）
  → ②annotation marks（按 namespace priority 升序逐个 patch：下划线/前景/背景修饰）
  → ③瞬时 marks（搜索匹配等）
  → 选区 → 光标
```

- Run 从单 `StyleClass` 扩展为 `base: StyleClass + marks: 有序小集合`；presentation 策略（scheme，`presentation_role` 的既有形状）把组合解析为最终属性——语义分层进 Scene，属性合成归策略，presenter 不变。
- **优先级 = 显式数值，per-namespace，scheme 配置**。默认梯度沿 nvim 生态惯例：syntax < semantic < diagnostics < user < ③瞬时 < 选区/光标（选区光标恒最高，不可配）。同 priority 按 namespace 注册序稳定。不采用 zed/helix 的隐式序（§0.5）。
- 切分算法：视口行内收集所有命中区间端点排序，端点处切 Run——nvim 扫描线的单行版，量级 = 每行个位数区间。

## 5. 虚拟文本与 fold（视图层变换族，未实现）

### 5.1 Run 三态化

kakoune DisplayAtom 三态直接移植到 Run：

```
Run = Source   { col, source_offset, text, style… }   // 现状
    | Virtual  { col, anchor_offset, text, style }     // 注入：不推进 source 游标，锚在 0 宽位置
    | Replaced { col, source_range, display_text, style } // 替换：盖住一段 source，显示别的
```

- Virtual 承载 inlay hint（行内）与 eol 诊断文本；Replaced 承载 fold placeholder（`…` / section 摘要行）与将来的 conceal 类。
- **坐标变换义务**：compose 产物中 Virtual 不占 source_offset 推进、Replaced 占一段——光标移动/点击命中经 Run 表反查 source 位置（text_position 现有职责扩展）。虚拟内容永不进 buffer、不进 anchor 坐标系（§0.4）。
- virt_lines（整行虚拟行，nvim virt_lines/helix LineAnnotation 形态）影响行高记账，触及 scene_layout 的行映射——与 fold 的行级变换一起做，不单独立项。

### 5.2 行级 fold（magit 的硬需求）

- 机制：view 持 fold 集合 `{anchor_range → placeholder 策略}`；scene layout 的"文档行 → 视觉行"映射经 fold 集折算（Zed FoldMap 的行级简化版——cind 无 softwrap，行映射目前是恒等，fold 是第一个打破恒等的机制，接口按此设计而非按恒等特化）。
- 折叠区间用 anchor 绑定（[05-jump.md](05-jump.md) 基建），编辑后随 anchor 重投影；**编辑落点在折叠区内 → 自动展开**（策略默认，围堵"看不见的地方被改了"）。
- fold 归 **view 域**：split 两侧各自折叠互不影响（Emacs invisible 归 buffer 域需要 overlay window 参数绕，是反面）。

### 5.3 与 completion ghost 预览的边界

ghost 预览（[06-completion.md](06-completion.md) §5，同样未实现）是**未发布快照的真实内容**，不是虚拟文本——两个机制刻意不同：预览要让光标/后文如实反映插入效果（走 buffer 快照），inlay hint 恰恰不能影响任何文档坐标（走 Virtual Run）。不合并。边界立场先行记录，两侧落地时各自遵守。

## 6. 生成式 buffer（magit 的地基，未实现，本文只立概念）

- **结构注入**：document 增加"green tree 由生产者交付"的构造路径——section 树（magit status 的 file/hunk 层级）以 green tree 形态与生成文本一并交付；`node_at`/结构导航/expand_selection 全部白拿。区间→语义对象 = 树查询，**不需要任何 property 存储**（§0.8）。
- **刷新协议**：内容 = f(外部状态)；刷新 = 整体重新生成 + **语义路径锚定恢复光标**（section 身份路径如 `unstaged/file.c/hunk#2`，[05-jump.md](05-jump.md) excerpt 内容锚同一价值观：身份绑语义，位置只是提示）。
- **只读 + 豁免**：生成式 buffer 默认拒绝编辑事务；commit message/rebase-todo 是普通 buffer + mode，不属此类。**可编辑生成视图**（status 里改 hunk、occur-edit/wdired）= [05-jump.md](05-jump.md) ComposedView 领地（编辑穿透 + 扇出写回）——ComposedViewModel 机制层已实现（编辑穿透产出 TransactionGroupEntry），视图集成未做；其漂移问题由②类同款 revision 重放覆盖。
- section 折叠走 §5.2 同一 fold 机制；当前 section 高亮走③类；`s`/`RET` 等按键 = 命令查光标处节点后分发（[03-input.md](03-input.md) keymap 分层，无需 per-region keymap 机制——magit 自己也基本是 buffer 全局键 + 命令内查 section）。

现状注记：当前的 generated buffer（位置列表等）是"生成文本 + 语义 location 表"（[../docs/location-lists.md](../docs/location-lists.md)），语言 mechanism 为空、无结构树——本节的结构注入是它的升级方向，不是另一类实体。

## 7. 机制（C++）/ 策略（scheme）表

| 项 | 机制 | 策略 | 状态 |
|---|---|---|---|
| D1 ②存储 | AnnotationSet 整组替换、payload side table、视口二分 | namespace 注册、stale 阈值（编辑数/时间）、stale 表现（淡显/隐藏/重请求） | 诊断特例已实现（DiagnosticSet：per-owner 整组替换 + revision 绑定隐藏）；通用化未实现 |
| D2 平移 | revision 重放映射、塌缩删除、缓存回写执行 | 回写节奏、gravity 默认与 per-namespace 覆盖 | 未实现 |
| D3 合并 | 端点切分、patch 合成执行 | per-namespace priority 数值梯度、组合样式解析（presentation 层） | 未实现（诊断走 gutter sign 列与位置列表） |
| D4 虚拟文本 | Run 三态、坐标反查 | 何处显示什么（inlay 开关/eol 诊断格式/padding） | 未实现 |
| D5 fold | view 域 fold 集、行映射折算、anchor 重投影 | placeholder 内容、折叠内编辑自动展开、初始折叠态 | 未实现 |
| D6 生成式 buffer | 结构注入构造路径、刷新 + 语义路径恢复、只读门 | 各生产者的 section 渲染与命令包（magit 设计另立） | 未实现（只读生成 buffer + location 表已有，无结构注入） |

## 8. 衔接

- **[04-workbench.md](04-workbench.md)**：LSP 会话注册表已实现（[../docs/lsp.md](../docs/lsp.md)），诊断已是②类首个生产者（publishDiagnostics → DiagnosticSet）；guest 绑定（其 §6.2 后续设计）落地后，guest buffer 的诊断经来源会话同样供给。
- **[05-jump.md](05-jump.md)**：诊断列表 = LocationList producer——已成立（`diagnostic.list`，payload 同源）；anchor 承载 fold 绑定与生成式 buffer 光标恢复（后续）；ComposedView 承接可编辑生成视图（机制已实现，视图集成未做）。
- **[06-completion.md](06-completion.md)**：ghost 预览与虚拟文本划清（§5.3）；signature help/hover 的光标随动 overlay 是 UI 层事务（[08-gui-chrome.md](08-gui-chrome.md) 两类锚定），与本层（buffer 内装饰）无耦合。
- **[03-input.md](03-input.md)**：生成式 buffer 的按键 = 命令查树分发，keymap 机制零新增。
- **[08-gui-chrome.md](08-gui-chrome.md)**：装饰的最终属性解析走 presentation 策略，视觉规格从其约束。

## 9. 非目标

- 不做通用 text property 存储（任意 key-value 附着于区间、随文本 kill/yank 携带）——Emacs 该机制的每个消费场景在三分法下各有归处。
- 不做 Zed 全套 DisplayMap 五层（无 softwrap、无 tab 变换需求——tab 展开已在 compose 内联）。
- 不做 conceal/spell/语法内嵌 URL 等 nvim 长尾 payload；Run 三态留了形状，需要时是策略扩展。
- 不做②类增量合并（LSP pull diagnostics 的 partial result 等）——整组替换，与 zed 同。
- 不做 semantic tokens 首期（clangd 语义高亮价值存疑于 N=1 场景，通道形状已兼容，需要时是新 namespace）。
- 不为 agent 提供装饰读写接口（R4）。

## 10. 场景走查

每条标注现状；「后续」= 依赖 §3.2 重放 / §4 合并 / §5 fold 落地。

1. **打字时诊断下划线**（后续——依赖重放 + 合并管线）：apply 热路径零触碰；下一帧 compose 视口二分命中 3 条诊断 → revision 差 1 → 重放映射 → 端点切 Run → 下划线随文本平移。现状行为：编辑即隐藏该组诊断，重新发布后恢复（gutter sign，无行内下划线）。
2. **LSP 响应迟到**（后续——依赖重放）：clangd 诊断绑请求时刻 revision N，到达时已 revision N+5 → 入库原样存 N 坐标 → 渲染时重放 N→N+5。现状行为：stale revision 的通知被忽略，等下一次发布。
3. **undo 跨分支**（后续——重放的免费定理）：undo_to 走 LCA 到另一分支 → 诊断 revision 与当前 revision 的映射 = 同一 LCA 路径 edit list → 位置正确。（nvim 要 u_extmark、helix 要正反 ChangeSet 都对不了分支 undo。）
4. **编辑风暴后的陈旧诊断**（部分成立）：现状 = 编辑即隐藏（比"标 stale 淡显"更保守）+ 新组到达整组替换（已成立）；stale 阈值/淡显/主动重请求未实现。
5. **万条诊断大文件**（部分成立）：诊断面板消费走 LocationList（已成立，不物化全量）；「视口二分只映射可见几条」依赖 AnnotationSet（后续）。
6. **inlay hint 编辑后陈旧**（后续）：remap 后旧 hint 继续显示（helix 防闪烁立场）+ stale 标志，idle 重拉替换。
7. **magit TAB 折叠 section**（后续——依赖 fold）：fold 集加入该 section 的 anchor 区间 → 行映射折算 → placeholder Replaced Run；再 TAB 移除。行号变化如实反映（内容 inline 在 buffer，视图层隐藏）。
8. **magit 刷新**（后续——依赖生成式 buffer）：git 状态变化 → 整体重新生成文本 + section 树 → 语义路径恢复光标到原 hunk；折叠态按 section 路径匹配保留。
9. **搜索高亮**（后续——依赖合并管线）：增量搜索每键重算命中（③类），compose 时作为瞬时 marks 参与合并，优先级压过诊断；退出搜索即弃。
10. **折叠区内被 LSP edit 命中**（后续——依赖 fold）：workspace edit 落点在 fold 内 → 自动展开策略触发 → 用户看见被改的内容。

## 11. 分期

- **已落地**：诊断链路（存储 → gutter sign / 位置列表 / Scheme 查询），revision 绑定形态；LSP 生产者接入；多 producer 并存。
- **P1 ②通道通用化 + 合并管线**：AnnotationSet（namespace / payload side table）、revision 重放映射、Run marks 合成、priority 策略。诊断迁移为一个 namespace，从「编辑即隐藏」升级为「平移显示 + stale 策略」。
- **P2 虚拟文本**：Run 三态、坐标反查、eol 诊断文本、inlay hint（LSP 侧新 feature adapter）。
- **P3 fold + virt_lines**：view 域 fold 集、行映射折算、行级虚拟行。
- **P4 生成式 buffer**：结构注入 + 刷新协议 + 只读门；magit 本体（section 渲染、git producer、命令包）另立设计。

## 12. 开放问题

- O1 重放映射的实现粒度：视口逐区间映射 vs 整组映射后缓存回写的默认取舍；edit list 合成的成本上界（编辑距离大时先粗筛行级再精映射？）。
- O2 诊断 gravity 的词边界问题：helix 为此付了 Assoc 四变体税，cind 是收这个税还是接受"词边打字下划线短暂粘连 + 下次响应修正"（stale 窗口本来存在）。
- O3 Run marks 的表示：小集合内联（预期每行个位数）还是位集 + payload 引用；与 presentation 策略的接口形状。
- O4 fold 与增量 reparse/缩进的交互：折叠只是视图变换、内核不知情——确认缩进/结构命令穿透折叠操作不可见区域时的用户预期（自动展开策略的覆盖面）。
- O5 生成式 buffer 的树注入接口：green tree 直接构造 vs 轻量 section 描述编译为 green tree；生产者在 C++（git porcelain 解析）还是 scheme 的边界。
- O6 semantic tokens 若将来引入：delta 编码的 LSP 协议与整组替换的冲突（token 量级大，可能是唯一需要增量合并的 namespace）。
