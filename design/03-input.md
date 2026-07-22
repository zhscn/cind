# 输入系统：Keymap / InputState / Modal Editing

输入系统把按键变成命令：一等具名 keymap、多层合并解析、per-view 输入状态机，以及承载各 modal 编辑方案的 Noun（selection / thing / motion）抽象。

## 定位

交互层设计文档：下接 [01-kernel.md](01-kernel.md)（transaction / anchor / CST）与 [02-buffer.md](02-buffer.md)（快照 undo 树），上承 [04-workbench.md](04-workbench.md)（interaction-class 与 placement 正交）、[05-jump.md](05-jump.md)（back/forward 是普通命令）与 [06-completion.md](06-completion.md)（补全菜单的键层）。实现文档：[../docs/command-loop.md](../docs/command-loop.md)（命令循环与交互架构）、[../docs/scripting.md](../docs/scripting.md)（Guile API 全集）。

调研基础：Emacs（src/keymap.c, keyboard.c）、meow、Helix、Kakoune、vim/neovim 源码。

## 实现状态

已实现（现状以 ../docs/command-loop.md 与 ../docs/scripting.md 为准）：

- keymap 一等具名 trie：`define-keymap!` / `bind-key!` / `bind-remap!` / `keymap-bindings` / `resolve-key-sequence`，parent 显式声明，remap 单遍；
- 层解析：override → transient 栈 → durable state → Window → View → Buffer → minor（逆激活序）→ major（含 parent 链）→ editor.default → application.global；层策略可由 `configure-keymap-policy!` 整体替换；
- per-view InputState 栈、state-change 事件、handler dispatch、on-enter/on-exit 生命周期、position hints；
- 文本输入归一：keydown 一律先进 `handle_key`，未消费的 TEXT_INPUT / IME / 粘贴按 state 的 accept/ignore 策略处理；
- Selection 常驻多 range 模型（anchor 对、granularity、metadata、primary）、thing / motion registry、selection verb 单事务多段编辑与多 range 文本插入；
- pending-prefix slot（count / register / extras）注入 `CommandInvocation`；
- `read-key-then!` 单键捕获、interaction-class（editing / interface）+ 具名 input strategy + selection-after-edit 策略；
- 策略包：`(cind emacs)`（含 C-u universal-argument）、`(cind meow)`（keypad、leader、数字 expand hints、register 捕获、char-thing 表）、`(cind vim)`（operator transient 态、区间可预览）、`(cind helix)`（select = extend 变体、`mi`/`ma`）、`(cind structural)`（sticky node 态）、`(cind toy-modal)`。

未实现：宏录制/回放与 dot-repeat 命令日志（§11 的设计红利，前提已成立但未兑现）；text-property / overlay keymap 与区域级 class override（预留扩展位）；用户级 key-translation 层。

---

## 需求背景（设计约束）

* **R1 — Emacs mode/keymap 骨架**：major/minor mode 绑定 keymap 是已知最灵活的
  输入系统设计，原则上复刻；除非有充分理由，不换骨架。（→ §3、§4、§8）
* **R2 — meow keypad 的非标准用法**：用户配置中 normal state 的 `x/c/g/m` 直接绑
  `meow-keypad-start`，即 `x c` ⇒ `C-x C-c`、leader dispatch 指向 `C-c`。这种
  "无修饰键序列翻译到 C-/M- 前缀世界"的能力必须原生支持，且是一等设计而非外挂。
  （→ §6）
* **R3 — 横跨多个编辑方案**：R2 本质是 modal editing 需求的一个实例。keymap 与
  state 的设计要足够通用，能同时承载 emacs 原味、meow 式自定义 modal、以及
  helix/vim 方案——方案是可插拔的策略，不是内核分支。（→ §0、§5、§10）
* **R4 — 界面 buffer 与 modal 的接缝**：Emacs 有 dired/magit 等以按键为命令面的
  特殊界面，modal 方案与之集成历来困难（evil-collection 的体量即是反证）；meow 的
  motion state 方向正确，但配合 R2 的非标准配置后，normal/motion/emacs 原生三者的
  接缝仍有摩擦。既然是从头设计的编辑器，要求**从机制上**消除这类逐包适配。
  （→ §8，机制解 = interaction-class）
* **R5 — noun 的承接能力**：major mode 的抽象要能承接 vim 的 text object 与
  helix 的 selection-based operations——对象/选区的定义权在 mode（语言）侧，
  消费权在方案侧。（→ §7）
* **R6 —（隐含）自洽与前瞻**：与 cind 内核（transaction/anchor/CST/
  command-as-data/Scene）成为一体，并为（半）结构化编辑留出输入侧的落点。
  （→ §2、§7.3）

---

## 0. 最高层结论

输入系统拆成四个正交概念，各取一家之长：

1. **Keymap**——一等具名对象。内部表示是 Helix 式声明 trie，外部语义是 Emacs 式
   显式 parent 继承 + `[remap cmd]`。可完全从 Guile 定义与修改，天然可内省
   （which-key / describe / reverse-lookup 免费）。
2. **Layer 解析**——Emacs `current_active_maps` 的复刻：一次按键对**一组同时激活的
   keymap 按固定优先级**做合并查找（window→view→buffer→minor→major→default→global），
   顶部插入 state 层与 transient 层。
3. **InputState**——per-**view** 的状态机，Kakoune 式**栈**。栈底是 durable state
   （normal / insert / motion / emacs …），其上可压 transient state（keypad、
   operator-pending、单键捕获）。**内核不认识任何具体 state 名**；normal/insert
   全部是 Guile 里注册的数据。
4. **Noun（typed range）**——vim `oparg_T` 的显式化 ≡ Helix `Selection`。
   `{anchor, head, granularity}` 的列表是所有 motion / text-object 的输出、
   所有 verb 的输入。Thing registry 基于自家 CST，而不是 tree-sitter query。

于是每个编辑方案（emacs 原味 / meow / vim / helix）都是一个 **Guile 策略包**：
一组 state 定义 + 若干 keymap + 命令绑定表。C++ 内核只提供机制，各方案共存
且可 per-view 混用。

---

## 1. 调研结论：为什么是这个拼法

**Emacs** 的本质是"有序激活 keymap 列表 + 整个列表本身表现得像一个大 keymap"
（keymap.c `Fcurrent_active_maps`）。这一层组合子设计（overriding → text-prop →
emulation → minor → major → global）与 remap 复用 keymap 机制、parent 链继承，
是必须保留的骨架。包袱：cons 裸表示、Meta 拆 ESC 前缀两级查找、menu 与绑定混居、
`read_key_sequence` 千行 goto——全部不带走。

**meow** 证明了 modal state 只是"一张高优先 keymap 层"（emulation-mode-map-alists）：
normal = 稠密全接管，motion = 稀疏几乎全穿透，两者只差 keymap 密度，这个洞察直接
决定了本设计里 state 与 keymap 的关系。keypad 是独立的按键翻译循环（无修饰序列 →
C-/M- 前缀序列），带两级 fallback。meow 最大的工程债在 `meow-shims.el`：六百行
逐包 advice，根因是 Emacs 没有"buffer 交互性质变化"的统一信号——§8 从设计上消灭它。

**Helix** 的 KeyTrie 是单张 keymap 的最佳表示：声明式、可合并、可枚举、prefix 节点
自动出 infobox。Select mode = Normal keymap 换绑 extend 变体命令，证明 mode 差异可以
纯粹用"绑定不同命令"表达而不需要 motion 内部读 mode 标志。缺陷：mode 是三值枚举，
无法叠加/嵌套，静态 trie 外的一切都逃逸进 `on_next_key` 闭包。

**Kakoune** 的 mode 栈（push/pop + on_enabled/on_disabled 生命周期 + ModeChange
hook 观测每次转换）是瞬态/嵌套模式的最佳底座；`declare-user-mode` 证明用户 mode
应该是运行时可注册的数据。缺陷：只查栈顶单张表，没有 Emacs 式多层合并，命令表
编译期写死。

**vim** 贡献两条：`oparg_T{start,end,motion_type,inclusive}` 让 motion 和 text object
共享同一接口喂给 operator——这是 vim 最成功的解耦，本设计把它显式化为常驻 Selection。
neovim 的"回调即伪按键"（K_LUA）与 `<Cmd>` 证明绑定目标应是数据而非闭包——cind 的
CommandDispatch 正是这个形态。必须抛弃的：operator-pending 悬空态（语义分散在
nv_operator/normal_check/do_pending_operator 三处、跨两次循环、区间不可见不可预览、
迫使每个 motion 写双分支）、timeoutlen 时间歧义、隐式模式（State 位掩码 + 三个正交
全局布尔推导真实模式）。

**结论**：Kakoune 的栈管理 state 生命周期，Emacs 的层解析管理同时激活的 keymap，
Helix 的 trie 表示每一张 keymap，vim 的 typed range 做 noun。这四者恰好正交。

---

## 3. Keymap：一等具名 trie

### 3.1 数据与语义

```
Keymap := { name, parent: optional<KeymapId>, entries: trie<KeyStroke, Entry> }
Entry  := Command(name)            ; 叶子
        | Prefix(KeymapId | inline) ; 子树，可挂 label（infobox 标题，如 "Goto"）
        | Remap(cmd-name → cmd-name) ; 伪前缀 remap 表，同 Emacs [remap]
```

* parent 在**定义期显式声明**（修掉 Emacs derived-mode 惰性设 parent 的 wart）；
  查找时子遮蔽父，未绑定的分支继承父行为。parent 与具名 prefix 的环在注册时拒绝。
* **无时间歧义**：同一 keymap 内一个 key 要么是绑定要么是前缀，后写覆盖先写
  （把命令延展为更长序列时替换为 inline prefix）。不做 vim timeoutlen 式
  "等更长映射"的挂钟消歧——`jj` 逃逸这类需求交给 insert state 的 handler 钩子
  （§5.4），不进 keymap 语义。
* 键记法沿用 `parse_key_sequence`（"C-x C-s"）。KeyStroke 带真实修饰符，
  **Meta 不拆 ESC**；TUI 的 ESC 消歧是前端 input-decode 层的事，进核之前完成。
* 可内省：枚举某前缀下全部绑定（which-key）、按命令反查键位（where-is）都是
  trie 上的纯函数。
* remap 是**解析后的一次替换**：先按层序解析出命令，再按同一层序对各层 Remap 表
  做一遍查找，不递归。minor mode 由此覆写 major mode 的命令而不动键位。

### 3.2 Guile API

```scheme
(define-keymap! host 'meow-normal #f)                    ; keymap 存在即可绑
(bind-key! host 'meow-normal "d" 'meow.delete)           ; 值：命令名
(bind-key! host 'meow-normal "g" '(prefix goto-map))     ; 值：子 keymap
(bind-remap! host 'my-minor 'edit.newline 'my.newline)   ; remap
(keymap-bindings host 'meow-normal)                      ; 内省
(resolve-key-sequence host '(meow-normal editor.default) "C-x C-f")
                                                         ; 纯查询 → none|prefix|command
```

`resolve-key-sequence` 是 keypad（§6）的地基：把 CommandLoop 的解析核心作为
无副作用纯函数暴露，layers 是任意 keymap 名列表，与实际 dispatch 用同一优先级与
remap 规则。`bind-key-if-command!` 是可选能力变体：命令不存在返回 `#f` 而非报错，
使策略包能在应用组合缺某个命令时静默跳过绑定。

---

## 4. Layer 解析

一次按键的合并查找顺序（高 → 低）：

```
0. override 层（C-g 等单键逃生，永远生效，在 pending 序列之前解析）
1. transient 层：state 栈中非栈底元素的 keymaps（栈顶在前）
2. state 层：栈底 durable state 的 keymaps
3. window → 4. view → 5. buffer
6. minor modes（激活逆序）→ 7. major mode（含 parent 链）
8. editor.default → 9. application.global
```

* 解析得到命令后做**一次** remap pass（按同一优先级序查各层 Remap 表，不递归）。
* 重复出现的同一 keymap 只保留最高优先级的一次。
* **完整 pending 序列对每一层逐层求值**：第一个认识该完整序列的层决定它是命令还是
  前缀。高优先层的稀疏前缀因此贡献自己的续键而不遮蔽低层的不同续键；同一完整序列
  在高层有定义时仍然优先。`pending_completions` 跨层合并，供 which-key。
* 整个解析是无副作用函数，同一入口服务实际分发、Guile 查询、describe UI。
* 层的根选择与优先级是 Scheme 策略：`configure-keymap-policy!` 可整体替换
  editor / application / override 根；C++ 暴露聚焦 Buffer kind 与各来源挂的
  keymap 名，校验组装结果后交给 CommandLoop。trie 查找、remap、pending 序列、
  命令执行保持 native 机制。minibuffer 焦点栈省略 editor 根与被遮文档的各层（§5.5）。

对应 Emacs 的取舍：text-property/overlay keymap（层 3'）不做，留作嵌入编辑
的扩展位（§8）；三层翻译 map 只保留概念——input-decode 归前端，function-key
归一化在 KeyStroke 构造时完成，用户级 key-translation 不提供。

---

## 5. InputState：per-view 状态机

### 5.1 定义（全在 Guile）

```scheme
(define-input-state! host 'meow-normal-state
  #:keymaps   #(meow-normal)      ; 注入层 2 的有序 keymap 列表
  #:text-input 'ignore            ; 'accept | 'ignore（§5.3）
  #:cursor    'block              ; 'beam | 'block | 'underline，进 Scene
  #:indicator "N"                 ; modeline 徽标
  #:on-enter  proc  #:on-exit proc
  #:position-hints provider)      ; 文档位置标注提供者（expand hints 等）

(define-input-state! host 'meow-keypad-state
  #:handler   keypad-handle-key   ; handler dispatch：每键先进此过程（§5.4）
  #:cursor    'block #:indicator "K")
```

state 分两种分发方式：**keymap dispatch**（缺省，走 §4 层解析）与
**handler dispatch**（每键交给 Guile 过程）。handler 收到完整命令上下文与规范
键记法，返回 `pass` / `consume` / `#(dispatch cmd [args])` / `#(pending sequence
hints)`：pass 刷新层继续走命令循环；dispatch 进入与 keymap 查找相同的命令执行
路径；pending 消费该键并把 hints 发布到与 keymap 前缀相同的 popup 通道。override
层在 handler 之前解析，`C-g` 是无条件逃生。handler 错误成为 scripting 诊断并
消费该键，不逃逸进前端事件循环。

`position-hints` 是挂在 state 上的纯文档标注查询（如 meow 的数字 expand hints）：
输入是当前 snapshot、完整 Selection 与有效 mode policy，输出 `#(byte-offset label)`
向量；只有栈顶 state 贡献 hints（transient 自然遮蔽 durable 的标注）；应用侧按
输入元组 memoize，结果以替换装饰进入前端无关 Scene。

### 5.2 栈、作用域与观测

* state 栈挂在 **View** 上（同一 buffer 两个窗口可一个 insert 一个 normal）；
  major/minor mode 维持 buffer 作用域不变——两者回答的问题不同：mode 回答
  "这是什么内容"，state 回答"这个视口正处于何种输入姿态"。
* 原语：`(push-input-state! host view 'name)` / `(pop-input-state! host view)` /
  `(set-base-input-state! host view 'name)`（替换栈底 = durable 切换）/
  `(reset-input-states! host view)`（弹空 transient，保留栈底，每弹一层发一个事件）。
* 每次 push/pop/base 切换发布 **state-change 事件**（≡ Kakoune ModeChange hook，
  携带 `#(kind view-id from to)`），`observe-input-state-changes!` 订阅——meow-shims
  里所有"进 X 切 normal、出 X 切回"的 advice 在这里变成一个订阅者。观测者的
  条件不回滚已完成的转换，也不打断其他观测者。
* **生命周期 = 栈成员身份**：`on-enter` 在加入栈后运行，`on-exit` 在 pop、
  durable 替换或 View 释放移除它之后运行；压入另一个 transient 遮蔽但不结束
  被遮 state 的会话。View 销毁先重置 transient、退出 durable，再释放 View。
* 栈底永不为空；`keyboard.quit`（C-g）默认行为 = 弹空到栈底 + 清 pending
  （≡ Kakoune reset_normal_mode），经同一命令路径取消前缀、交互焦点或 transient
  handler 态。
* input feedback（transient 态的回显序列与 hints）归属 View 的当前 state 栈：
  push/pop/base 切换或下一个投递键自动失效，hints 不会活过产生它的 state。

### 5.3 文本输入归一

规则收敛为两条，前端不再自作主张：

1. **所有 KeyStroke 一律先进 `handle_key`** 走层解析。被消费（含 prefix 待续）
   则该键结束，前端丢弃紧随的配对 TEXT_INPUT 事件。
2. **TEXT_INPUT / IME / 粘贴事件**进 `insert_text`，由聚焦 state 的
   `text-input` 策略决定：`accept` → 插入（insert/emacs 态）；`ignore` → 丢弃
   （normal/motion 态）。

推论：insert 态**不在 keymap 里绑可打印字符**（keydown 未消费 → 文本事件插入，
IME 组合文本天然正确）；normal 态把可打印字符绑成命令（keydown 消费 → 文本
事件被丢）。不需要 Emacs 的 self-insert-command 全局绑定与 suppress-keymap
这对补丁。TUI 单通道，同一契约在单个解码输入事件内完成——未消费且 state=accept
才插入。minibuffer 拥有焦点期间文本一律 accept，与被遮文档的 state 无关。

### 5.4 transient state 与单键捕获

Helix `on_next_key`、Kakoune `NextKey`、vim replace-char 统一为一个语法糖：

```scheme
(read-key-then! host view-id proc #:sequence text #:hints vector)
```

`(cind input)` 提供共享的 `input.read-key` transient state：压栈、捕获恰好一个
规范键、**先 pop 再调用** continuation；override 命令取消该态时清除 continuation。
hints 走 `pending_key_hints`/popup 通道渲染 infobox。register、Thing 等单键
提示共享这一个生命周期与分发路径，解释权留在各方案包。`jj` 式逃逸、isearch
的增量输入同样用 handler state 表达，不污染 keymap 语义。

### 5.5 与 interaction 子系统的关系

interaction（minibuffer/picker）创建 transient 的 minibuffer Buffer/View/Window，
焦点切换即层切换：其 View 携带继承 `editor.text-input` 的交互 keymap，**复用与
文档 View 相同的 durable InputState 与 handler 管线**，其后是自己的 Window/View/
Buffer 层与 `application.global`；`editor.default` 与被遮文档的各层不在此焦点栈。
普通的移动、删除、undo、kill/yank、selection、prefix 命令因此经由正常
`CommandContext` 直接作用于 minibuffer，无需交互专用的命令副本。

---

## 6. Keypad：按键翻译器（R2 的正式化）

keypad 是一个 **transient handler state，逻辑 100% 在 Guile（`(cind meow)`）**，
机制依赖仅两个：`resolve-key-sequence`（§3.2）与 hints/echo 通道。

状态机（复刻 meow-keypad.el 语义）：

* 累积序列为 `(modifier . key)` 列表，modifier ∈ control/meta/both/literal。
* 首键规则：start-keys 表（`x→C-x`、`c→C-c`、`h→C-h`，可配置）；`m`→Meta 待定、
  `g`→C-M- 待定、`SPC`→literal 待定；其余首键 → leader dispatch。
* 进入序列后，后续键**默认加 C-**。
* 每步把累积序列翻译成规范键序（"x f" → "C-x C-f"），对**发起 Window 的 base
  layers**（= §4 层 3–9，排除 keypad 自己的 state/transient 层）做
  `resolve-key-sequence`：
  - `prefix` → 回显 + 发布与前缀帮助同源的分层补全，继续读；
  - `command` → pop 自身，dispatch 该命令（带 pending 前缀参数）；
  - `none` → 两级 fallback：末键 C- 降级 literal 重试（"C-x f" 之于 "C-x C-f"）；
    再失败且策略允许时，用原始键序对同一 base layers 透明穿透（motion 态的
    SPC 回退到 major mode 原生绑定，即 meow leader-transparent）。
* leader dispatch 三态同 meow：keymap 名 / 键序字符串（如 "C-c"）/ 缺省表。

入口即普通命令：normal/motion 态的 `SPC`、`x`、`c`、`g`、`m` 都绑 keypad 入口，
`x c` ⇒ `C-x C-c` 完全落地；由于翻译目标是 base layers，任何 major/minor mode
新增的 C-x/C-c 绑定自动对 keypad 可见，无需重复声明。`SPC 0`–`SPC 9` 经 `C-c`
leader 解析到 prefix 命令并进入 `meow-numeric` transient handler，后续数字由它
累积，首个普通键落回 durable normal map 并带上 count（§7.4）。

---

## 7. Noun：Selection / Motion / Thing / Verb

### 7.1 Selection：常驻、有向、带粒度、可多段

```
Range     := { anchor: AnchorId, head: AnchorId, granularity: char|line|block|node }
Selection := { ranges: [Range]（非空）, primary: index, meta: scheme datum }
```

* 底层是 Anchor 对，随 transaction 结算（01-kernel.md §5.5），**天然跨编辑存活**
  ——内核没有"编辑即清 selection"的硬规则。selection 生命周期是命令完成语义的
  一部分：命令结果可显式 preserve / collapse / 整体替换 Selection；缺省结果在
  命令链改变了上下文 Buffer 时询问当前 InputStrategy 的 `selection-after-edit`
  策略（emacs 包 collapse，helix 包 preserve），直接文本与 IME 提交走同一策略。
* `granularity` 显式承载 vim 的 motion_type（charwise/linewise/blockwise）与
  结构化的 node——vim 的 forced-motion（`dVj`）退化为"改写 granularity 再消费"。
* `meta` 是 Guile 自留 slot：meow 的 selection-type（expand/char/word…）与配对
  展开 motion 都放这，内核不解释。
* primary head 即 View caret；移动 caret 保留其余 range；分屏把完整 selection
  模型复制进新 View；两个前端的 Scene 组合高亮所有非空 range。
* 多 range = helix/kakoune 式**真多光标**——一个命令一次作用于 N 个 range，
  全部编辑落在同一个 transaction（一个 undo 单元）：`insert-text!` 在每个 head
  插入（重合 head 去重），selection verb 每 range 一段替换。它与 kmacro /
  meow beacon（录宏后逐位置重放）是正交机制，后者属宏回放的应用（§11），
  Selection 侧无特殊支持。
* **undo 粒度**：维持 per-transaction——每次按键一个 transaction 一个 undo 单元，
  与 typed-char 管线一致；undo 树（02-buffer.md §5）使细粒度历史廉价，无聚合
  必要；多 range verb 本就是单 transaction，不受影响。

### 7.2 Motion 与 Thing registry

* motion = 纯函数 `(snapshot, selection, count) → selection`，注册进 registry；
  `define-motion!` 把名字映射到 native 纯机制（前后向 char/word/symbol/
  expression、up-list）。同一 motion 由绑定决定 move（替换）还是 extend
  （保 anchor）——学 Helix，用两个命令变体而非让 motion 读 state。
  `motion-selection` 对 Selection 的每个 range 施加带符号 count 的变换。
* thing = 声明式 DSL 编译为 `(inner . bounds)` 两个求值器：

```scheme
(define-thing! host 'angle  '(pair "<" ">"))            ; 配对，depth 计数
(define-thing! host 'string '(cst-node string-literal)) ; ← CST 直取，优于 tree-sitter query
(define-thing! host 'defun  '(cst-node function-definition))
(define-thing! host 'word   '(char-class word))
(define-thing! host 'sym    '(multi (cst-node id) (char-class symbol)))  ; 链式 fallback
(char-thing-table-add! 'meow #\a 'angle)   ; 按方案命名空间的 char→thing 表（meow 包内）
```

  `pair` 优先取 CST group 配对，再退文本扫描；`cst-node` 解析最内层匹配的语法
  节点或字面量 token——在注释/字符串/宏里比正则和 tree-sitter query 更可靠。
  `thing-selection` 先解析 mode 的语义名绑定（§8 `#:things`）再落到具体注册名，
  对 Selection 的每个 head 求值 inner/bounds，**全部命中才返回**（不产生部分
  展开的多 range Selection）。

### 7.3 Verb 与组合律

* verb = `(context, selection) → transaction + resulting selection`。selection
  verb 提交 typed Selection 与每 range 一段替换：先解析 char/line/node 粒度、
  查重叠，作为一个 transaction 一个 undo 节点提交，返回收敛后的 Selection——
  可见选区的生命周期留给方案包。
* **helix 方案**：motion 的 extend 变体 + verb 直接消费常驻 selection，天然成立。
* **vim 方案**：operator-pending = `d` push 一个 transient state 捕获后续
  motion/object/count，算出 range 后调 verb——**内核没有 op-pending 概念**，
  它只是方案包里的少量 Guile。捕获期的暂定区间即时发布为 View Selection，
  区间可预览（修掉 vim 悬空态不可预览的缺陷）。
* **meow 方案**：先选后动，selection-type 放 meta，`,`/`.`（inner/bounds of
  thing）经共享单键捕获态调 thing registry。
* **结构化编辑落点**：`(cind structural)` 的 sticky `structural-node` transient
  态（`C-c e` 进入）——`expand-node-selection` 是对完整 Selection 的纯 CST 查询
  （每 range 换到下一个包围语法区间，全命中才成立，粒度标 node）；per-view
  anchor 化 selection history（push/pop/clear/depth）由 native 存储、跨事务
  结算，历史语义归 Scheme。expand/contract 留在态内，删除 pop 态后走共享
  selection verb，退出保留选中节点并交还底层策略。

### 7.4 前缀参数（count / register / 任意）

command loop 挂一个 **pending-prefix slot**：`{count, register, extra: alist}`。

* prefix 命令返回完整的替换 prefix 值；keymap 层刷新保留它，dispatch 链继承它；
  下一个普通终端命令在不可变 invocation 里收到并消费。未定义输入、错误、禁用
  命令、交互请求与 `keyboard.quit` 清空它。单个未绑定可打印键把 prefix 带进
  配对的文本提交：归一化文本路径按正数 count 重复提交的 UTF-8 文本（零不插入，
  负数报输入错误），不合成按键事件。
* emacs 包的 `C-u` = transient `emacs-universal` handler（照抄
  universal-argument 的 transient map 设计）：初值 4、重复 `C-u` ×4、十进制键
  替换/扩展数字、`-` 取负；首个普通键 pop 该态并经 durable state 重新分发，
  prefix 原样带入；Backspace/Delete 在态内分发显式的 raw-deletion 逃生命令。
* meow 包：normal 态数字键做 selection 数字展开（position hints 标注 1–9/0 =
  展开一到十位）；`SPC 数字` 走 leader + `meow-numeric` 累积 count；`"` 经共享
  单键捕获写 register（保留已累积的 count/extra，取消时连 prefix 一起清）。
* 格式化的 prefix 状态进 echo（"3"、"C-u 4"、`"a`），独立于 pending 键序与
  transient 态 feedback 的投影。

---

## 8. Mode 系统与接缝的系统解

major/minor mode 维持 buffer 作用域与 keymap 关联，三件关键机制：

```scheme
(define-major-mode! host 'magit-status-mode
  #:parent 'special-mode
  #:keymap 'magit-status-map
  #:interaction-class 'interface)   ; ← 关键属性

(define-major-mode! host 'cind.cpp
  #:parent 'prog-mode
  #:interaction-class 'editing
  #:things '((defun . cst-defun) ...))
```

1. **parent 定义期显式**（同 kind 才能继承）；子 mode 无显式 keymap parent 时，
   继承最近祖先 mode 的主 keymap 作为缺省 parent；settings、interaction 属性、
   语义 thing 绑定沿 parent 链继承。
2. **`interaction-class` 属性**：`editing`（正文编辑）| `interface`（dired/magit
   式命令面板）。minor mode 可覆写（wdired 式 minor 声明 `editing`）。
   **有效 class = 最近启用的 minor 声明 ∨ major 声明**。
3. **class 变化即事件 + 具名 InputStrategy**：input strategy 是
   `(define-input-strategy! host name editing-state interface-state
   selection-after-edit)` 定义的具名映射；应用有缺省 strategy，每个 View 可独立
   覆写（`set-view-input-strategy!`，传 `#f` 恢复继承）。mode policy 变化发布
   Buffer 身份与前后策略，有效 class 或 initial-state 变化时，展示该 Buffer 的
   每个 View 经**其自己选择的 strategy** 重推导 durable state，transient 栈
   原样保留；仅 thing/补全策略变化不动输入姿态。

这就是 meow-shims 六百行的系统解：wdired/wgrep/magit-blame/edebug 全部退化为
"开关一个翻转 class 的 minor mode"，一条规则覆盖，特殊包零适配。per-mode 的
state 例外（如 REPL 类 mode 直接进 insert/normal）用 mode 属性
`#:initial-state` 覆盖，优先于 class 映射，不是全局 alist。

区域级 override（macrostep overlay、markdown 内嵌代码块）：预留
"text-property keymap + region class"扩展位（Emacs 层 3'），未实现。

---

## 9. 机制与实现位置

C++ 机制全部遵守 [../docs/scripting.md](../docs/scripting.md) 契约（名字、
generational ID、快照、typed 值）：

| # | 机制 | 实现位置 |
|---|------|---------|
| M1 | keymap 定义/parent/remap/内省 + `resolve` 纯查询 | ../docs/command-loop.md「Keymaps and default bindings」、scripting.md keymap 原语族 |
| M2 | InputState registry + per-view 栈 + state-change 事件 + handler dispatch + position hints | ../docs/command-loop.md、scripting.md `(cind input)` |
| M3 | 文本输入策略化（`handle_key` 先行、`insert_text` 查 state 策略） | ../docs/command-loop.md「Normalized input」 |
| M4 | Selection 列表化 + granularity + meta + 命令声明式选区结果 + selection verb | ../docs/command-loop.md、scripting.md selection 原语族 |
| M5 | pending-prefix slot 注入 CommandInvocation | ../docs/command-loop.md「Command loop and prefix help」 |
| M6 | thing/motion registry 的注册与求值（求值在 C++，快照 + CST 上跑） | scripting.md `define-thing!`/`define-motion!` |
| M7 | mode 声明属性 + interaction-class + InputStrategy + class 变化事件 | ../docs/command-loop.md、scripting.md mode 原语族 |

**Guile 策略**（每个编辑方案一个模块，可并存）：state 集合与 keymap 表、keypad
全部逻辑、leader、thing 定义与 char-thing 表、prefix 命令、strategy 的
class→state 映射、state-change 订阅者。`(cind core)` 持有缺省 Emacs 绑定表，
`(cind emacs)` 持有对应 strategy。

---

## 10. 验证：各方案的落地形态

| 方案 | durable states | 特有机制 → 设计对应 |
|------|----------------|---------------------|
| emacs（缺省） | `emacs`（text-input=accept，空 state keymap 列表保持缺省层策略） | `C-u` → `emacs-universal` transient handler；一切走层 3–9 |
| meow（`C-c m`） | `meow-normal` / `meow-insert` / `meow-motion` | `SPC/x/c/g/m` → keypad；leader=`C-c`；数字 expand → position hints；`"`/`,`/`.` → 共享单键捕获；motion 稀疏穿透 + keypad 透明回退 |
| vim（`C-c v`） | `vim-normal` / `vim-insert` / `vim-visual` | op-pending → transient 捕获态（区间即时发布为 Selection，可预览）；count/register 经 `input.read-key` 进 prefix slot；text object → thing registry |
| helix（`C-c h`） | `hx-normal` / `hx-select` / `hx-insert` | select = normal keymap 换 extend 变体；multi-range Selection 跨编辑保留；`mi`/`ma` → read-key-then! + thing；删除走共享原子 selection verb |
| structural（`C-c e`） | （transient `structural-node`，压在任意 durable 之上） | expand/contract 走 CST 查询 + per-view anchored history；`d` pop 后走共享 verb |
| toy-modal（`C-c n`） | `toy-normal` | 极小策略样例，验证扩展只用公共机制即可成立 |

特殊 buffer 全景：magit 式 status buffer（interface）在 meow 策略下自动 motion，
`n/p` 穿透到 major keymap，keypad 翻译失败时透明回退给原生绑定；按下 wdired 式
编辑开关 → minor 翻 class → 该 buffer 所有 View 自动切 normal，退出自动切回。

---

## 11. 副产品（设计红利）

* **which-key 全域一致**（已兑现）：prefix、keypad、单键捕获、transient feedback
  全走同一 hints/popup 通道，运行时/mode/继承/翻译/用户绑定同表呈现。
* **headless 测试**（已兑现）：`parse_key_sequence` 驱动的回放路径覆盖全部机制。
* **宏录制/回放**（未实现）：`handle_key` 是唯一入口的前提已成立——录 KeyStroke
  流、回放走同一路径。
* **dot-repeat / 命令日志**（未实现）：CommandInvocation 全是数据，journal 可重放。

---

## 12. 非目标

* 不复刻：cons keymap、Meta=ESC 两级展开、menu 与绑定混居、timeoutlen 挂钟消歧、
  minor-mode-overriding-map-alist（remap + 层序足够）。
* 不做 evil 级 vim 完整兼容；vim 包是"证明表达力"的样例，不追 ex 命令面。
* 不做 meow beacon 式假多光标——多光标是 Selection 列表的真实现（已落地）。
* text-property/overlay keymap、用户级 key-translation：预留扩展位，不做。

---

## 13. 开放问题

* keypad 透明回退的边界：回退查找是否也看 window/view 层（当前定为层 3–9），
  有真实用例再调。
* per-view state 在 terminal buffer（未来）下是否需要 buffer 粘性例外——
  终端 buffer 存在后再定。
