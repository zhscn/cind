# 补全管线

语言服务层设计：多源候选的产生、竞态治理、合并排序、应用与菜单呈现。

## 定位

依赖 [03-input.md](03-input.md) 的分层 keymap 与 interaction 基建、[04-workbench.md](04-workbench.md) 的 LSP 会话注册表（provenance 路由）、[02-buffer.md](02-buffer.md) 的快照 undo 树、src/ui 的 Scene/Presenter 帧模型。实现文档：[../docs/lsp.md](../docs/lsp.md)（Completion feature）、[../docs/scripting.md](../docs/scripting.md)（scripted provider 与门控政策）、[../docs/command-loop.md](../docs/command-loop.md)（`editor.completion` 检视状态）。

## 实现状态

已实现：

- **管线核心**：世代取消（每次输入前进 generation，响应落地前验代）、多 provider 并发扇出（本地候选可先开菜单，LSP 迟到按 provider 整组替换合入）、统一 fuzzy 排序、条目稳定 id 寻址、可视窗口 + 选中项懒 resolve（原地写回，不重置排序与选中）、单事务应用（insert/replace 双范围 + additionalTextEdits 合并为一个 undo 步；接受未 resolve 条目时排队等 resolve 完成）、isIncomplete 二分（complete 结果随 query 增减纯本地 refilter；incomplete 的 provider 重新请求）。
- **provider 形态**：固定 native 管线 + 动态注册的 scripted provider（`define-completion-provider!`）。Word 源与 Path 源是 Scheme 代码（分别消费 `view-identifier-words` 语义快照与 include 上下文 + 多目录枚举任务）；Ares Scheme 补全同为 scripted provider；LSP 是结构化 provider 规格（经 04-workbench.md 的会话注册表取会话）；Snippet 为预留的 provider kind。
- **语法门控**：Scheme mode 政策消费 native 语法快照（`view-syntax-token`/`view-line-prefix`）——注释/字符串内抑制、`#include` 上下文切 Path 源单独请求。
- **UI 与按键**：菜单 = 光标锚定浮动 overlay（不 reflow）；选中项文档在视口有空间时显示为毗邻 overlay；菜单按键走 keymap 上下文层，未绑定键透传插入并推进 refilter。
- **自动触发**：mode 政策的 completion-auto 布尔（独立继承），标识符字符后自动请求；无命中且各 provider 结清后自动关闭。

未实现（见 §12 后续设计）：snippet 编辑态（tabstop/placeholder；条目 `is_snippet` 元数据已预留）、ghost 预览、signature help、RTT 自适应 debounce、per-session 超时竞速、commit characters（策略层未配置）。

## 需求背景（设计约束，源自用户输入）

- **R1（接口即痛点）**：补全曾被归为"标准工作流，没什么可设计的"。lsp-bridge 对 capf 的反思（manateelazycat 2022-06-26）证伪了这个假设：Emacs 的补全*接口本身*是卡顿与丑陋 workaround 的根源——候选词当查询 key、每次绘制重复查询、exit-function 与 additionalTextEdits 顺序冲突、"菜单弹出即数据完结"的同步假设 vs LSP 流式返回。协议处理是标准工作流，**管线接口的形状不是**。→ §1.0、§1.1
- **R2（UI 锚定）**：补全候选必须跟随光标，与 picker/which-key 的底部 minibuffer band 是两类瞬时 UI——会话型（注意力已转移到输入框）reflow band；光标随动型（注意力始终在插入点）光标锚定浮动 overlay，**不 reflow**（打字中推动正文不可接受）。锚定分类见 [04-workbench.md](04-workbench.md) 论点 4 与 [08-gui-chrome.md](08-gui-chrome.md)。→ §7
- **R3（不重新发明已收敛的答案）**：本设计的问题形态是"是否要重新设计，还是按已有方案实现"。调研结论：helix 与 zed 两个独立实现在全部关键决策上收敛，这块**按现成方案实现**，自主设计只保留内核独有资产能兑现的三处（语法门控、零成本预览、机制/策略切分）。→ §0
- **R4（语义层外挂）**：C++ 语义补全来自外挂 clangd（cxx-frontend 评估结论：自建语义层不如外挂 clangd）；内核 green tree 不产出语义候选，但**产出触发决策所需的语法上下文**——这是五个被调研编辑器都没有的资产。→ §6
- **R5（agent 边界，沿袭 [05-jump.md](05-jump.md) R4）**：不为 agent 设计任何补全消费接口；立论只靠人类交互痛点。

## 0. 最高层结论

1. **骨架按 helix，细节按 zed，capf 是反面清单，kakoune 路线否决**。helix 提供整体形状（内核内建、事件驱动、世代取消、客户端统一 fuzzy）；zed 提供数据模型与细节决策（insert/replace 双范围、可视区懒 resolve、isIncomplete 前缀延续判定、sortText 后置 tie-break、additionalTextEdits 并入单事务、key-context 式菜单按键）。两家独立收敛到同一组答案，收敛本身即证据。
2. **多源并发 + 客户端统一打分是唯一正确的合并模型**。capf（run-hook-wrapped 短路）与 kakoune（setup_ifn 先到先得）犯同一个错：顺序短路、第一个非空源赢家通吃。正解：所有源并发扇出，结果归一化为同一条目类型，客户端 fuzzy 统一打分排序；LSP sortText 只做同分 tie-break，filterText 替代 label 参与匹配。
3. **竞态用世代计数，不用锁不用等待**。helix TaskController（AtomicU64 打包世代+计数）与 zed next_completion_id（新请求 drop 全部旧 task，落地前验代数）同构。用户每次输入使世代前进，迟到响应自然作废。
4. **isIncomplete 二分**：complete 且新输入是原 query 前缀延续 → 纯本地 refilter 零 RPC；incomplete → 只重查声明 incomplete 的 provider，替换其条目、保留其他源。
5. **条目按 id 寻址、预组装、可视区懒 resolve**。菜单显示所需（label/kind/detail 摘要）在响应落地时一次组装；documentation 等重字段只对可视范围 + 选中项异步 resolve、原地写回。这两条直接消解 lsp-bridge 批评 #1（候选词当 key）与 #2（重复查询）。
6. **应用是单事务**：textEdit（insert vs replace 按 intent 选）+ additionalTextEdits 合并为一个 undo 步。消解批评 #3（插入-删除-再插入闪烁）。快照 undo 树使 ghost 预览（后续）近零成本：预览 = 不发布的快照，取消 = 丢弃。
7. **触发决策语法门控**——本设计唯一的自主决策点。五家全部用"文本 + 词字符正则"判断触发，注释/字符串照弹。cind 光标处语法上下文现成：注释/字符串内不触发、`#include <` 后切 path 源、`.`/`->`/`::` 后必触发。green tree 白拿的能力；谓词执行位于 Scheme mode 政策，native 只提供语法快照。
8. **机制/策略切分走全**：管线（扇出、取消、条目池、事务应用）= C++ 机制；provider 语义、触发条件、门控规则、源组合 = scheme 策略（scripted provider 与 mode 政策）。zed 的 sort_completions/filter_completions trait 开关是这个方向的半步。
9. **补全菜单 = 光标锚定浮动 Region**，overlay 不 reflow，新 widget 零结构改动（Scene 模型既有承诺）；按键经分层 keymap 的菜单上下文层，不做编程式拦截。

## 1. 调研结论

### 1.0 lsp-bridge 的四条批评（对照清单，均已在实现中兑现）

| # | 批评 | capf 代码根源 | 本设计的解 |
|---|---|---|---|
| 1 | 候选词本身当查询 key（同名候选/超长签名） | annotation 等按候选字符串回查 | 条目按稳定 id 寻址（§4） |
| 2 | 每次菜单绘制重复查询候选+注解 | 每键 flush 缓存全量重算 | 落地时预组装 + 可视区懒 resolve（§4.3） |
| 3 | 先插入再 exit-function，与 additionalTextEdits 顺序冲突 | 补全流程硬编码插入时机 | 单事务应用管线（§5） |
| 4 | "菜单弹出即数据完结"同步假设 vs LSP 流式 | table 三参数协议必须当场返回 | 菜单与计算解耦，迟到响应按 provider 替换（§3.3） |

结论：lsp-bridge 用外挂 Python 进程解决的问题是 Emacs 单线程同步 funcall 的特产；内核原生异步的编辑器里这些问题不存在，但**接口若画错（短路合并、同步契约、无 id 寻址）会在任何宿主上复现同类痛点**。

### 1.1 Emacs capf（反面教材，源码级）

- table 三参数协议 `(string pred action)` 完全同步，必须当场返回候选（minibuffer.el:232-256）；没有 continuation/回调的位置——异步 IPC 后端结构性塞不进去。
- 每敲一键 `completion--flush-all-sorted-completions` 清缓存（:1791-1800，挂 after-change-functions），下次动作全量重查 table——对 IPC 后端 = 每键一次同步 RPC。
- 多后端 `run-hook-wrapped ... 'all` 短路（:3301），只取第一个非 nil 后端；`:exclusive 'no` 的 fallthrough 用前缀 try-completion 近似判断（:3277-3289 自带 FIXME），对 fuzzy 后端不可靠，且仍是二选一非合并。
- completion styles 在客户端对 table 返回全集再过滤（:1378），与服务端已 fuzzy/已排序的 LSP 模型重复冲突；capf 契约还明令后端不得预过滤（:3249-3251）。
- `while-no-input` + idle timer 只保护 `*Completions*` 重绘（:2772-2812），中断不了正在执行的同步 funcall。

### 1.2 neovim（0.12-dev，追赶中的同步内核）

- 内建 ins-completion：compl_* 全局状态 + 双向循环链表 + ~20 个 CTRL-X 子模式（insexpand.c:91-161），pum 是核心私有资产。
- 0.12 已收编：`'complete'` 多 `F{func}` 源合并进同一 pum、per-source 限流（`F{func}^N`）与衰减超时（80ms 起每源减半，function 源单独 300/1000ms）、内建 autocomplete、模糊过滤、commit_chars——正在收窄 nvim-cmp 存在的理由。
- 但 function source 仍同步阻塞（callback_call + textlock，E840），真异步靠 findstart 返回 -2 留在补全模式、事后 `complete()` 注入的逃逸通道（completion.lua:1360）——"同步内核上补异步"的形状警告。
- 值得偷：LSP 层**按 RTT 指数移动平均自适应 debounce**（adaptive_debounce，completion.lua:149-155；resolve 独立均值）；响应落地前验光标行/mode 的过期校验。

### 1.3 helix（骨架来源）

- 事件漏斗：CompletionEvent{AutoTrigger,TriggerChar,ManualTrigger,DeleteText,Cancel} → AsyncHook 按类型分层 debounce（手动 0 / 触发字符 5ms / 自动按配置）（request.rs:71-149）。
- 取消：TaskController/TaskHandle，单 AtomicU64 打包世代+运行计数，restart() 一次作废全部在途（cancel.rs:149-155）；阻塞型 provider 协作式轮询 is_canceled。
- 多源：固定枚举 CompletionProvider{Lsp(id),Path,Word}（helix-core/completion.rs:15-20）；多 LSP 各起 future 进 JoinSet，首响应后开 100ms 收集窗，迟到走 replace_completions；path/word 走 spawn_blocking。合并 = 拼接 + nucleo 统一打分，provider 优先级只做 tie-break。
- isIncomplete per-provider 记录，输入时只重查 incomplete 的 provider、替换其条目（completion.rs:186-187, ui/completion.rs:427-441）。
- resolve 双轨：接受时同步 block_on 保证 additionalTextEdits 可用；悬停文档走独立 150ms debounce 的 ResolveHandler。
- 预览：ghost transaction 走 doc.apply_temporary + savepoint 恢复，不发给 LSP。
- 键路由：insert 模式按键先递给 completion.handle_event，消费则不达命令层（ui/editor.rs:1503-1548）。

### 1.4 zed（细节来源）

- CompletionProvider trait（completions.rs:1068-1122）：completions/resolve_completions（批量 index + 原地写回）/apply_additional_edits/is_completion_trigger + sort/filter/show_snippets 三个策略开关。多 LSP 扇出全部藏在 `Entity<Project>` 实现之下，editor 不知道 LSP 存在。
- 无输入 debounce，纯世代取消：next_completion_id 递增，新 task 启动即 retain 掉旧的（completions.rs:557-560），落地前验菜单代数（:672-677）；唯一时间控制是 per-server lsp_fetch_timeout_ms 竞速——慢 server 出局不拖累别人（lsp_store.rs:6818-6862）。
- isIncomplete：was_complete 且 query.starts_with(initial_query) 且位置一致 → return 纯本地 refilter（completions.rs:366-390）。
- 合并：LSP + words（±5000 行窗口扫词，Fallback 模式有 LSP 则让位）+ snippets（伪 LSP server_id）同一条管线，统一 CompletionSource tag；inline prediction（copilot 类）是完全独立的 EditPredictionDelegate 系统，仅 UI 层交汇。
- 排序 MatchTier 多级 key：WordStartMatch 分层 → exact → snippet 位次 → **fuzzy 分 → … → sortText（后置 tie-break）** → kind → label（code_context_menus.rs:1404-1500）；匹配串用 filterText 非 label（:340）。
- 可视区懒 resolve：只 resolve last_rendered_range ± 预取余量，选中项永远 resolve（兼容不合规 server），完成后原地写回（:626-731）。
- 应用：insert vs replace 双范围按 intent + lsp_insert_mode 选（editor.rs:11010-11149）；additionalTextEdits 的事务 merge_transactions 进主编辑事务，undo 一步（completions.rs:1029-1039）。
- 菜单按键 = key context：菜单可见时注入 `menu`/`showing_completions` 上下文，keymap 绑定自然优先匹配，非编程式拦截（editor.rs:2675-2681）。

### 1.5 kakoune（否决的路线）

- 编辑器零 IO：外部进程异步算完 `set` 一个带 `line.column[+len]@timestamp` 头的 completions option，InsertCompleter 作为 OptionWatcher 消费（insert_completer.cc:258-342）。架构最纯粹，代价清单太长：
  - 多源不聚合（setup_ifn 顺序短路，:531-559）；
  - 无 resolve 协议——候选只有 `text|on_select cmd|menu text` 三段，一切信息注入时内联；
  - 坐标脆弱：字节 line.column，锚点上游被改或跨行即整批静默作废（changes_since 校验，:278-286），乐观 + 丢弃，无增量修正；
  - 用户已选中时新候选被忽略到下次 reset（:583-585）。
- cind 有 anchor，坐标失效问题不存在；解耦卖点用 provider 接口同样拿到。可借鉴的只有 timestamp 过期校验的思路（cind 以 revision + anchor 重投影替代，严格更强）。

## 3. 管线机制

### 3.1 provider 形态

```
provider = Lsp(结构化规格，经会话注册表)     ; native 归一化协议条目
         | scripted(name)                    ; define-completion-provider! 注册的 Scheme 过程
         | Snippet                           ; 预留 kind
```

管线（扇出、竞态、合并、排序、菜单、应用）是固定的 native 机制；候选语义是开放的 scripted provider 集合。这与 helix 的"固定枚举"立场的差别在于：cind 有脚本层，Word/Path 这类源本身就是策略（词源消费 `view-identifier-words` 语义快照——identifier-aware，优于 zed 的 ±5000 行正则扫词；Path 源识别 include 上下文并走多目录枚举任务），Ares Scheme 补全也以同一接口接入。mode 政策按 [../docs/scripting.md](../docs/scripting.md) 的 completion-providers 继承规则决定每个 buffer 的有效源组合与顺序。接口边界按 zed：候选生产、resolve、additional-edits 归 provider；竞态、合并、排序、菜单归管线。scripted provider 的阻塞工作表达为 typed 异步任务（`completion-provider-task`），worker 不进 Guile。

### 3.2 请求扇出与世代

- 触发事件（§6 门控通过后）分配世代号 `gen`，前进世代即作废全部在途（helix restart 语义）。
- 各 provider 并发请求；本地源当场结清，**首个结果即可开菜单**，LSP 迟到按 provider 整组替换合入（helix replace_completions 形），菜单原地增长不闪烁。
- 落地前验世代 + 验请求快照仍有效（响应绑请求时刻 revision/anchor）。
- per-session 超时竞速（zed lsp_fetch_timeout_ms：慢 server 出局不拖累别人）未实现，见 §12。

### 3.3 refilter 与 isIncomplete

- per-provider 记录 is_incomplete。输入推进时：complete 的结果随 query 增减做纯本地 refilter（保持条目身份稳定）；incomplete 的 provider 重新请求，替换该 provider 条目、保留其他源。
- 删除退回触发点左侧 → 取消会话（helix DeleteText 语义）。

### 3.4 触发节奏

自动触发由 mode 政策的 completion-auto 布尔控制（独立继承的政策位，语言 mode 自行 opt-in）；标识符字符后自动请求，无命中且全部 provider 结清后自动关闭；`C-M-i` 手动请求走同一管线。本地源无需 debounce（增量分析预算内直接算）。分层 debounce 窗口（helix：手动 0 / 触发字符 ~5ms / 自动配置窗）与 RTT 自适应（neovim）作为策略参数留白，见 §12。

## 4. 条目模型与过滤排序

### 4.1 条目

```
CompletionItem { id, provider, filter_text, label, kind, detail,
                 edit: { insert_range, replace_range, new_text } | textless,
                 sort_text, is_snippet, resolved, doc: lazy, raw: provider私有 blob }
```

id 寻址（批评 #1 的解）；raw blob 类型擦除，resolve/应用时由 provider 自解释（zed lsp_completion 存法）。落地时一次组装菜单显示所需字段（批评 #2 的解）。scripted provider 的条目给出显式 byte 替换范围或回退到通用 query 范围；LSP 条目保留 insert/replace 双范围，协议字段（filterText/sortText/kind/detail/documentation/snippet 元数据）在响应落地时一次归一化。管线对照请求快照校验范围。

### 4.2 过滤排序

- 匹配串 = filter_text（无则 label）；全部源进同一 fuzzy 打分。
- 排序多级 key（zed MatchTier 简化）：词首命中分层 → exact → fuzzy 分 → sortText tie-break → kind → label；具体权重序列是策略参数（§8）。
- provider 间去重语义重复（Word 源剔除与语义源 new_text 相同者，zed :596-612）。

### 4.3 懒 resolve

只 resolve 可视菜单窗口 + 选中项（选中项永远 resolve）；resolve 响应按条目 id 原地写回，不重置排序与选中。世代推进先取消在途 resolve 再复用缓存条目。接受未 resolve 的条目时，应用排队在 resolve 之后——additionalTextEdits 不丢、仍在同一事务（批评 #3、#4 的收口）。

## 5. 应用

- **单事务**：主 textEdit（insert vs replace 双范围按确认 intent 选，默认 policy 决定）+ additionalTextEdits 合并为一个 undo 步。
- **snippet**：条目 `is_snippet` 元数据已预留；tabstop/placeholder 编辑态未实现（§12）。
- ghost 预览（选中即预览）、接受后重触发与 signature help 联动均为后续（§12）。

## 6. 触发（语法门控）

- 触发候选事件：词字符输入 / provider 触发字符（`.` `->` `::` 等，来自 LSP capabilities）/ 手动键。
- **语法门控**在事件与请求之间：光标处语法上下文查询——注释/字符串字面量内**不触发**（除 path 源在 `#include "` 内）；`#include <`/`"` 后**切 path 源单独请求**；member access 后必触发。谓词由 Scheme mode 政策执行，native 提供 `view-syntax-token`（显式 offset 的 token kind 与范围）与 `view-line-prefix`（行首到 caret 的文本）两个语法快照——语言特定判断不进 `EditorApplication`，策略可扩展（如 doc comment 内放行 word 源）。
- 五家对照：全部文本正则判定，无一读自己的语法树（helix/zed 有 tree-sitter 而未用于此）。cind 的 green tree 在按键路径上本来就是热的，此门控零额外成本。

## 7. UI 与按键

- **光标锚定浮动 Region**：菜单是 Scene 新 region，paint 顺序在正文之上；锚点 = 补全起始列（非光标列，neovim compl_col 语义）；默认光标行下方，空间不足翻上方；宽度按内容有界。overlay 不 reflow——弹出/收起不移动正文一个 cell。候选行含语义 kind 与 detail 列。
- **文档侧栏**：选中项的 documentation（resolve 结果）在视口有空间时显示为毗邻 overlay，不改变文档与菜单几何。
- 壳克制（ui-taste）：无边框阴影圆角，色块分区沿用 band 的两级地面思路；选中行实色块同 picker。
- **按键 = keymap 上下文层**：菜单可见时其导航/确认/取消绑定经分层 keymap 优先生效；未绑定键透传插入并推进 refilter。非编程式拦截（zed key-context 思路映射到 03-input.md 分层）。commit characters 若引入即是该层的策略绑定。

## 8. 机制（C++）/ 策略（scheme）表

| 项 | 机制 | 策略 |
|---|---|---|
| C1 触发 | 世代分配、请求快照校验 | 门控规则（语法快照消费）、completion-auto、触发字符表、自动关闭 |
| C2 扇出 | 世代取消、并发扇出、按 provider 替换 | 源组合与顺序（mode completion-providers）、源实现（scripted provider） |
| C3 合并排序 | fuzzy 打分、多级 key 排序、语义去重执行 | 权重序列、Word 源 fallback 行为 |
| C4 resolve | 可视区调度、按 id 原地写回、接受排队 | resolver 本体（scripted）、预取余量、文档侧栏开关 |
| C5 应用 | 单事务、范围校验 | insert vs replace 默认 |
| C6 UI | 浮动 Region 布局、翻转、文档侧栏几何 | 菜单行数上限、kind 图标/文案 |

## 9. 衔接

- **[04-workbench.md](04-workbench.md)**：LSP provider 的会话来源（会话注册表已实现；provenance/guest 绑定属其 §6.2 后续设计——落地后，由导航到达的系统头文件经来源会话同样供补全，跳进 `<vector>` 里也有补全）。
- **[03-input.md](03-input.md)**：菜单按键是分层 keymap 的上下文层；which-key 的 pending_completions 基建不混用（那是命令补全，走会话型 band）。
- **[05-jump.md](05-jump.md)**：无交集（补全不产生跳转边；goto-definition 属显示咽喉/jump 管辖）。
- **[08-gui-chrome.md](08-gui-chrome.md)**：光标随动型 overlay 的第一个实例在此落地。

## 10. 非目标

- 不做 inline prediction（copilot 类）——若将来做，按 zed 证据独立成系统，不塞进本管线。
- 不做 kakoune 式声明式外部注入通道。
- 不自建 C++ 语义候选（clangd 外挂，R4）。
- 不为 agent 提供补全消费接口（R5）。
- 不做 capf 兼容层。

## 11. 场景走查

1. **`.` 后成员补全**：语法门控放行（member access）→ clangd 会话请求，Word 源并发 → 本地候选先开菜单（光标下浮动 Region，正文不动）→ 语义响应按 provider 合入 → 继续敲字纯本地 refilter → RET 单事务应用。
2. **注释里打字**：词字符事件 → 语法门控拦截（注释 token）→ 无请求无菜单。五家对照组全部弹出。
3. **`#include <vec` 补头文件**：门控识别 include 上下文 → path 源单独请求 → 系统 include 路径候选（Scheme 组装、过滤、去重、标注）。
4. **auto-import**：接受带 additionalTextEdits 的候选 → 主编辑 + 文件头 insert 一个事务 → 一次 undo 全回。无闪烁（批评 #3 对照）。
5. **慢 server**：clangd 冷启动期间 Word/Path 源正常开菜单；clangd 迟到按 provider 替换合入，菜单原地增长不闪烁（批评 #4 对照）。
6. **isIncomplete**：海量候选 server 返回 incomplete → 每键只重查该 provider，其余源本地 refilter。
7. **快速连打**：每键前进世代，在途请求全作废，无过期菜单闪现；响应落地前验请求快照。
8. **ghost 预览**（后续）：C-n 换选中项，正文即时显示候选文本（未发布快照），C-e 取消回原状，undo 历史零痕迹。

## 12. 后续设计

- **snippet 编辑态**：接受 SNIPPET 条目 → 展开 tabstop/placeholder，进入 snippet 编辑态（Tab 跳位）。条目模型已预留 `is_snippet`。
- **ghost 预览**：预览 = 基于当前快照的未发布编辑，换选中项或取消 = 回到原快照。快照 undo 树使其免 savepoint 舞蹈（helix 需要 savepoint+restore 双保险，cind 的快照本身就是保险）。
- **signature help**：复用光标随动 Region；与补全菜单同屏的布局仲裁见开放问题 O3。
- **触发节奏打磨**：分层 debounce 窗口、RTT 自适应（neovim adaptive_debounce）、per-session 超时竞速（zed）、commit characters、接受后重触发自动补全（`crate::` 场景，helix :281）。

## 13. 开放问题

- O1 fuzzy 算法归一：文档补全排序在 native 管线，picker 排序在 Scheme（`rank-completion-candidates`，case-folded 多词子串匹配）——两套是否收敛为一套仍开放。
- O2 收集窗长度（helix 100ms）与"首响应即开菜单"的取舍——单 clangd 场景收集窗可能无意义；当前实现取"先开菜单 + 迟到替换"，无收集窗。
- O3 signature help 与补全菜单同屏时的 Region 布局仲裁。
- O4 ghost 预览与增量 reparse 的交互：预览编辑是否走完整 lex+parse（按键预算下可能直接可行）。
- O5 多光标应用（kakoune/zed 都做了 per-selection 复用）：cind 多光标模型定型后回补。
