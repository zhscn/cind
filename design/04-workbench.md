# Workbench：project / window / layout 管理

工作台层设计：buffer / project / layout 三者的归属关系，buffer 显示位置的 placement 模型（intent + slot），以及 LSP 会话的 provenance 路由。

## 定位

工作台层设计文档：下接 [03-input.md](03-input.md)（per-view InputState 与工作台正交）、[02-buffer.md](02-buffer.md)（buffer 全局池），上承 [05-jump.md](05-jump.md)（display 咽喉是跳转图的边写入点、workbench 是图与位置列表栈的归属容器）。实现文档：[../docs/workbenches.md](../docs/workbenches.md)、[../docs/projects.md](../docs/projects.md)、[../docs/lsp.md](../docs/lsp.md)、[../docs/gui-architecture.md](../docs/gui-architecture.md)（Workbench 与渲染的关系）。

调研基础：~/package/emacs、persp-mode、~/project/kakoune、~/package/{vim,neovim,helix} 源码。

## 实现状态

已实现（现状以 ../docs/workbenches.md 为准）：

- Workbench 实体（scope × layout × MRU × slots）、始终恰好一个 active workbench、切换时非活跃 Window/View 存活（caret/selection/输入状态/viewport 保留）、关闭按创建序选下一个；
- 成员语义：scope（声明式，多 project）∪ MRU（足迹式，显示即入）、visitor 零仪式、`C-x b` 的 scope 过滤与二次按键 widen 到全局池、`workbench.expel`；
- placement：display 咽喉 `display(buffer, intent)` 携带 origin window；intent 集合 `edit`/`jump`/`tools`/`doc`/`pop`/`explicit`；具名 slot 由 Window role 派生（role 在工作台内唯一）；pinned；policy 创建的窗口带 provenance，dismiss 规则两条；解析策略是可替换的 Guile 过程，native 校验完整 plan 并在策略报错时用确定性缺省回退；
- 异步落点：origin window 决定目标 workbench——异步 open/tool 完成时更新发起方的 layout 与 MRU，不切 active workbench；
- 会话序列化/恢复（含 jump 图与 walk 截断段、layout 树、role/pinned/provenance、MRU 资源路径），存稳定路径而非运行时 ID；
- LSP 会话注册表（按 project/root、语言、server 配置与 capability 片段建键，跨 workbench 共享），会话 root 按 project root → 资源父目录 → 当前目录解析；
- project 发现（`.git`/`cmk.yaml`/`compile_commands.json` 就近标记）、异步索引 + 目录 watch、`.clang-format` 发现与 project SettingsLayer；
- `C-x w` 命令面（new/switch/close/adopt-project/expel/set-role/toggle-pinned/dismiss/save-session/restore-session），modeline 仅在工作台 >1 时显示名字。

未实现（后续设计，见 §8）：

- LSP 会话的 provenance 绑定（home/guest，§6.2）——当前会话归属仍从路径推导，跳出 project 的文件没有会话；
- project 配置的 `.cind/` 两级信任模型（数据层键值 + 代码层 Guile 信任门控）；
- tab（workbench 内命名 layout 页）——立场记录，按需再做（§6）；
- per-frame active workbench（remote / 多 OS 窗口的扩展位，§10）。

职责划分（实现形态）：Guile `(cind workbench)` 持有名字与唯一性、scope、MRU、active workbench、每工作台的 active Window、Window 的 role/pinned/provenance 策略状态与 slot 派生、jump walk 与游标；native 持有 WindowLayout、jump 图节点/边/anchor、实体校验与 display plan 应用。

---

## 需求背景

以下需求来自长期 Emacs + persp-mode 使用经验，是本设计的验收基准：

- **R1（工作台隐喻）**：persp 的实际用法只是 layout manager——一个大工作台的抽象，台面上不同区域摆不同项目的东西，viewport 可以从各自 project 拿 buffer 摆放。"一个 persp 对应一个 project + hook 限制切换" 是流行配置但过于受限。→ §3
- **R2（层级与归属）**：repo 级别的东西可视为 project（tooling 边界）；但屏幕上的 layout 本身不归属任何 project；一个 editor 会话不限定 project——这与 IDE 的抽象正好相反。→ §0.1、§3
- **R3（划分与隔离/共享）**：editor 内多个 project 可产生数学意义上的"划分"（前后端分离等多个离散 repo 归属一个大项目）；同一划分单元内需要一个工作台概念，提供隔离与共享的抽象。→ §3.2
- **R4（反过度设计）**：担心系统设计复杂后丧失灵活性、沦为鸡肋。经验上 layout 最多 3 个（人脑工作会话容量所限）；更多 project 常常只是临时看一眼，纳入管理反而增加心智负担。→ §3.4、§12
- **R5（placement 失控）**：Emacs display-buffer-alist 复杂，xref 跳转经常不受控地覆盖当前 layout 中某个窗口。要求 buffer 显示位置可预测、可控。→ §4
- **R6（tab 的位置）**：Emacs tab 实践中没用起来；直觉上一个 tab 未必对应一个 buffer，具体形态无定论。→ §6
- **R7（LSP 会话的 project 边界）**：LSP 以 project 为单位；lsp-mode/eglot 下 find-def 跳出当前 project（如系统头文件）后无法继续 find-def，现有 workaround 皆是补丁。要求从架构层面解决。→ §7
- **R8（project 级配置）**：style 等配置是 per-project 的（dir-locals 设样式名 + hook 应用的形态）；`.clang-format` 发现与 project SettingsLayer 已落地，代码层配置需补全并给出信任模型。→ §8

---

## 0. 最高层结论

六个论点，后文分节展开：

1. **三层归属维持既有立场，拒绝 IDE 反转**。Buffer 是全局池（所有编辑器一致：emacs/kakoune/vim/helix 无一例外）；Project 是 tooling/配置边界而非窗口归属；layout 不属于任何 project。R2 在 cind 里不是要新做的东西，而是站对了的地基——工作台在此之上补缺，不动地基。

2. **Workbench = scope × layout × recency**。一个 workbench（工作台）持有：一组 project 的 **scope**（可多个——前后端分离即 scope = {backend, frontend}）、一棵 **WindowLayout**、一份工作台内 buffer 的 **MRU 顺序**。它是 persp-mode 的"内核化正确实现"：persp 靠替换 read-buffer-function + frame buffer-predicate + hook 围堵来过滤 buffer 列表，注定有漏网入口；cind 的 buffer 切换只有一个咽喉（interaction provider）、显示只有一个咽喉（display 请求），过滤在咽喉处一次做对，不需要围堵。

3. **Placement = 调用点 intent + 具名 slot，一个总函数，零几何回退**。display-buffer 失控的根因：意图信息在显示时刻已丢失，只能靠 buffer 名字正则去猜，猜不中就落到 `display-buffer-use-some-window` 按"最大/LRU 窗口"这种纯几何依据选窗（window.el:9284-9293）——这就是"随机覆盖一个窗口"的体感来源。反着做：**命令在调用点声明 intent**（`edit`/`jump`/`tools`/…），workbench 把 intent 映射到具名 slot（kakoune 的 toolsclient/jumpclient 是这个思路的先例），解析是一个小的总函数，没有启发式回退链。

4. **临时性分级，临时物永不污染持久模型**。三级：瞬时 UI 永不进入 WindowLayout——这是 helix compositor 的不变式；其内部再分两类锚定：**会话型**（picker/which-key/minibuffer 提示——注意力已转移到输入框）走底部 reflow band（[08-gui-chrome.md](08-gui-chrome.md)）；**光标随动型**（补全菜单/signature help/hover-at-point——注意力始终在插入点）是光标锚定的浮动 overlay，不 reflow（打字中推动正文不可接受），形态见 [06-completion.md](06-completion.md)。持久工具 buffer（grep 结果/编译输出/终端）走 slot；外项目文件是 **visitor**——被显示过就自动进入本工作台 MRU，零仪式，不触发任何"纳管"动作，这直接解决 R4 的"临时看一眼"。

5. **tab 不做一等概念**。Emacs tab-bar 的教训：tab = 布局快照但不隔离 buffer，与"浏览器标签"直觉错位，所以用不起来。本设计中若将来需要 tab，它 = workbench 内的**命名 layout 页**（同 scope、同 MRU，只换窗口排布）——tab 对应布局而非 buffer，恰好是 R6 直觉的形式化。见 §6。

6. **LSP 会话绑定按 provenance，不按路径；配置按 project 作用域收敛**。"跳出 project 后 find-def 断链"的根因：lsp-mode/eglot 从**文件路径**推导会话归属，系统头文件路径上不属于任何 project 就没有会话。正解：绑定跟随**到达路径**——由导航到达且无 home 会话的 buffer 以 guest 身份绑到来源会话，管道复用 §4 显示咽喉本就携带的 origin。project 配置 = 数据层（进 SettingsLayer）+ 代码层（信任门控的 Guile）；workbench 不拥有配置（R2 立场的延伸）。→ §7、§8

---

## 1. 调研结论

### 1.1 Emacs display-buffer（教训之源）

- 一次显示要合并 **7 个来源**的 action（overriding → alist → special → 参数 → extra → base → fallback，window.el:8399-8415），摊平成函数链逐个尝试。行为依赖窗口几何、LRU 时间戳、dedicated 状态等大量隐式全局态，不可预测；源码里自带 `;FIXME: why isn't this redundant?`（window.el:8122）。
- **xref 失控的具体路径**：单结果默认 same-window；多结果或原窗口 dedicated/已死时落入 fallback 链末端 `display-buffer-use-some-window`（window.el:9244），按 LRU/最大面积选一个窗口原地换 buffer。选择依据是几何而非语义——这是 R5 的病灶。
- `quit-restore-window`（window.el:5381+）为每次显示记录四元组以便恢复，机制本身又成为新的复杂度来源。
- **取**：buffer 显示需要 policy 这个问题意识；dedicated（本设计的 pinned）概念。**弃**：显示时刻按 buffer 名匹配、多来源合并、几何回退、quit-restore 全家。

### 1.2 Emacs tab-bar / window-state

- tab = frame-local 的 window-configuration + window-state 快照 + frame buffer-list 排序快照（tab-bar.el:1492-1529）。**不隔离 buffer**，`C-x b` 在任何 tab 都看到全部 buffer——期望与实现的错位是"用不起来"的根因。
- window-state-get（window.el:6388）证明布局可序列化：buffer 存名字、point 存整数偏移、恢复时容忍缺失。§9 的会话序列化沿用此参考。

### 1.3 persp-mode（正确的目标，错误的手段）

- perspective struct = name + **显式 buffer 成员表** + window-conf + parameters（persp-mode.el:1219-1229）。显式成员表是 tab-bar 缺的那块，方向正确。
- 实现手段是围堵：替换 `read-buffer-function`、设 frame `buffer-predicate`、挂 ido hook（persp-mode.el:444-463）。绕过这些入口的路径（直接 set-window-buffer、display-buffer）就漏出隔离。**结构性教训：过滤必须发生在唯一咽喉，而不是围堵所有入口**。
- `persp-def-auto-persp`（:1718）声明式自动归类（hook + predicate + get-name）值得作为 Guile 策略的形态参考。

### 1.4 project.el

- 无状态目录推断：`project-find-functions` 找 VCS 根；`project-buffers` 按 default-directory 前缀过滤（project.el:460-471），无显式成员表。轻量、不与窗口层耦合——cind 的 Project 发现（../docs/projects.md）是同一哲学。
- `project-switch-project` 只是"以该目录为上下文跑一个命令"，不动 layout 不隔离 buffer。**印证 R2：project 与 layout 正交是对的**；Emacs 的问题只在于没有第三者（工作台）把两者组合起来，用户被迫在 tab-bar/persp/project.el 三套互不知情的机制里拼装。

### 1.5 kakoune（placement 具名化的先例）

- session（server）持全局 buffer 池，client = 一个 window，splits/tabs 整个外包给 tmux/WM（rc/windowing/*.kak）。
- **`toolsclient` / `jumpclient` / `docsclient`**：用户具名声明"工具输出去哪个 client、跳转发生在哪个 client"（rc/tools/grep.kak:17-18）。placement 是用户声明的策略，不是编辑器启发式——本设计 slot 概念的直接来源。
- window 换 buffer 时旧 window 进共享池、按 buffer 复用以保留滚动位置（client_manager.cc:152-166）——cind 的 (window, buffer) View 复用是同构做法。

### 1.6 vim / neovim

- tabpage 持窗口树，buffer 全局；`tcd`/`lcd` 把工作目录挂在 tab/window 上（window.c:5699-5735 进窗时解析 window > tab > global），证明"目录上下文挂在 layout 节点、进入时解析"可行。cind 的 context-project 按窗口 buffer 推导，是更细粒度的等价物，workbench 无需自己的 cwd。
- quickfix/location list：工具输出进**结构化列表**而非任意 buffer，`:copen` 位置确定。cind 的 Generated buffer + locations（[../docs/location-lists.md](../docs/location-lists.md)）同构。
- 公认痛点：tabpage 是布局容器却被用户当项目容器用，靠 tcd 手工补——**印证"布局容器"和"项目集合"必须是两个正交轴**，workbench 把两轴显式组合。

### 1.7 helix

- Editor 持全局 documents + 一棵 split Tree，无 tab；per-(view,doc) 的 selection/scroll 存在 Document 上按 ViewId 索引（document.rs:144-145）。
- workspace = 无状态根查找（.git/.helix，helix-loader/src/lib.rs:257-275），喂给 fuzzy picker。
- **所有瞬时 UI 是 compositor overlay**（compositor.rs:78-105），picker/popup/menu 永不成为窗口或 buffer——结构上杜绝 display-buffer 问题。cind 底部 reflow band 是同一不变式的实现。

### 1.8 eglot / lsp-mode 的跨 project 跳转 workaround

- eglot：会话按 project 建键存 `eglot--servers-by-project`；`eglot-extend-to-xref`（eglot.el:536，默认 **nil**）开启后，xref 落点文件被记入全局哈希 `eglot--servers-by-xrefed-file`，`eglot-current-server` 在 project 查找失败后按**文件绝对路径**查它兜底（eglot.el:2509-2520）。局限：只覆盖 xref 一条到达路径；全局按路径记一份——两个 project 先后跳到同一头文件互相覆盖；默认关闭说明作者也视其为权宜。
- lsp-mode：提示选 project root、或把外部文件临时加入 session folders（`lsp-auto-guess-root` 一类），同样是在"绑定从路径推导"这个前提内打补丁。
- **取**：eglot 的哈希证明"记住到达来源"就是解；**弃**：opt-in、单路径、全局路径键——这三点都源于 provenance 不是一等信息，只能事后旁挂。

---

## 2. Workbench 模型

### 2.1 定义

```
Workbench {
  id, name,
  scope:   有序 ProjectId 列表        // 声明式成员：R3 的"划分单元"
  layout:  WindowLayout + active_window
  mru:     BufferId 列表              // 自动成员：在本工作台显示过的 buffer，MRU 序
  slots:   {intent-symbol → WindowId} // 由 Window role 派生（role 在工作台内唯一），
}                                     //   窗口关闭即失效
```

编辑器始终存在**恰好一个 active workbench**，且至少拥有一个 workbench。启动时创建一个匿名 workbench（scope 空、单窗格）——**只有一个 workbench 时，一切工作台机制不可见**（modeline 仅在 >1 时显示工作台名），行为与无工作台概念时完全相同。这是 R4 的第一道护栏：不用就不存在。

### 2.2 成员语义：声明 ∪ 足迹

工作台的"buffer 集合"= **scope 内 project 的所有打开 buffer（声明式，含未来打开的）∪ mru（足迹式，显示过即入）**。

- **声明式（scope）**：`workbench.adopt-project` 把 project 加入 scope。前后端分离场景：一个 workbench scope = {backend, frontend}，两 repo 的 buffer 都在此工作台"原生可见"，project.find-file / project.search 覆盖整个 scope。这就是 R3 的划分单元：隔离（其他工作台看不到）与共享（单元内多 repo 互通）同时成立。
- **足迹式（mru / visitor）**：打开 scope 外的文件（xref 跳进第三方库、临时看一眼别的 repo）**什么都不用做**——buffer 正常显示，自动进入本工作台 mru，buffer 列表里可见。不产生任何归属变更、不询问、不自动建工作台。要正式纳管，显式执行 adopt——一次一个动作，心智负担为零。这直接消化 R4 的"临时看一眼"。
- **划分不由内核强制**。数学划分要求不相交；内核允许同一 project 出现在两个 workbench 的 scope（罕见但合法，如 monorepo 局部视角）。"保持划分"是默认策略：adopt 时若 project 已属别的工作台，Guile 策略可提示或默认移交。机制宽松、策略收紧，避免为不变式付出灵活性。

### 2.3 切换与生命周期

- `workbench.switch`：换 active workbench = 整棵 layout + scope + mru 一起换。非活跃工作台的 Window/View **保持存活**（只是不参与 Scene 组合），滚动位置、光标、selection、输入状态原样保留——kakoune window 池的教训：状态保留是切换体验的全部。
- Buffer 全局共享：同一 buffer 可同时出现在两个工作台（各自的 View，View 多对一 buffer 的既有机制现成）。一处编辑，处处生效——共享由 buffer 层免费提供，工作台只管可见性。
- `workbench.new [project]`：新建，可选初始 adopt。`workbench.close`：关闭并释放其 Window/View（buffer 不动，仍在全局池）；关闭 active workbench 按创建序选下一个（尾部回绕）后再释放。
- **异步落点跟 origin 走**：display 请求携带 origin window，origin 决定目标 workbench。异步 open 或工具请求在另一工作台激活期间完成时，结果更新发起方的 layout 与 MRU，**不切换** active workbench；origin 已销毁则回退 active workbench。
- 切换 UI 走既有 picker（底部 band）；modeline 在 `>1` 个工作台时显示工作台名。

### 2.4 反过度设计护栏（R4 应答）

- 单工作台 = 零仪式、零可见性（§2.1）。
- visitor 免管理（§2.2）。
- 无嵌套工作台、无工作台级 keymap/settings（后者留 SettingsLayer 扩展口，§10）。
- 上限不设硬编码，但一切 UI 按"个位数工作台"设计——不做工作台的树、分组、搜索。经验值 3 个是设计假设而非限制。

---

## 3. Placement：intent + slot

### 3.1 原则

**意图在调用点声明，不在显示时刻猜。** 命令显示 buffer 时总是知道自己在干什么——xref 知道自己是跳转，grep 知道自己是工具输出。Emacs 让这个信息在 `display-buffer(buffer)` 的签名里丢失，再花两千行代码从 buffer 名字里猜回来。cind 的显示咽喉是：

```
display(buffer, intent, origin) → window   // intent: symbol；origin: 发起 window
```

placement 消费 intent，LSP 绑定消费 origin（§7），跳转图消费两者（[05-jump.md](05-jump.md)）——一条管道，三个消费者。

### 3.2 内置 intent 与默认策略（Guile 可整表替换）

| intent | 语义 | 默认解析 |
|---|---|---|
| `edit` | 用户主动打开文件编辑 | active window 原地显示 |
| `jump` | 导航结果（xref 定义、grep 命中、诊断） | 复用既有 `jump` slot；否则 active window，除非其 pinned 或是工具窗——则用相邻窗格或创建 slot |
| `tools` | 持久工具输出（grep 列表、编译输出、终端） | 复用或创建底部 `tools` slot |
| `doc` | 文档/帮助类 | 同 tools 规则，独立 `doc` slot |
| `pop` | 显式要求另开窗格 | split active window |
| `explicit` | 调用方已指名目标窗口 | 直接用指名的 window |

解析是**一个总函数**：查 slot 表 → 查 pinned → 应用规则，三步出结果，无回退链、无几何依据。同一 intent 在同一 layout 状态下永远落同一窗口——可预测性是第一目标。解析策略是可替换的 Guile 过程；native 校验完整 plan、维护 WindowLayout 不变量，并在扩展策略失败或非法时经内置缺省 Scheme 策略重试，placement 决策只有一个实现。

### 3.3 窗口属性

Window 携带三个 placement 属性（Guile 持策略状态，native 校验与应用）：

- **pinned**：此窗口的 buffer 不被任何 policy 解析替换，只有用户显式动作能换。等价 Emacs dedicated，但因为解析函数是总函数，pinned 的影响是精确可推的（jump 绕开它去 slot，而不是绕开它去"最大窗口"）。
- **role**：slot 登记的反向标记。role 在工作台内唯一——把同一 role 指给另一窗口即移动派生的 slot。用户可手动指定：`window.set-role tools` = kakoune 的 "这个 tmux pane 是 toolsclient" 的窗格版。
- **created-by-policy**：placement 策略创建的窗口带此 provenance，供 dismiss 规则区分。

### 3.4 关闭与返回（quit-restore 的减法）

两条正交规则代替 quit-restore 四元组：

- **policy 创建的窗口，dismiss = 删除窗口**（layout 树 erase 自动归还空间）。`q`/`window.dismiss` 在此类窗口删除布局叶。
- **policy 复用的窗口，dismiss = 清除 role、保留 split**；"返回上一个位置"走历史机制——per-view selection history 与跳转 walk（05-jump.md），不是窗口层恢复。窗口层不记"上一个 buffer"。

### 3.5 xref 走查（R5 验收）

单定义跳转：intent `jump` → active window 原地跳，与 Emacs 默认相同。多候选：走底部 picker（瞬时 UI，不占窗口）或位置列表。选中后仍 `jump`。用户把左窗格 pin 住看代码、想让所有跳转发生在右窗格：`window.set-role jump` 一次声明，此后所有跳转确定性落右窗格——"不受控覆盖"在这个模型里没有存在空间，因为不存在"policy 自己挑一个窗口"的路径。

---

## 4. Buffer 切换与过滤

- buffers provider 读 active workbench：候选 = scope buffers ∪ mru，按工作台局部 MRU 排序。**过滤只在这一个咽喉发生**——persp 围堵教训的正解。
- **widen 一键放宽**：picker 内再按一次触发键（`C-x b`）切到全局 buffer 列表。隔离是默认视图不是墙；从全局列表选中一个 buffer = 它成为本工作台 visitor。共享/隔离的边界由一次按键跨越。
- `buffer.next/previous` 同样按工作台 mru 循环。
- kill-buffer 语义不变（全局杀）；`workbench.expel` 只把 buffer 移出本工作台 mru，buffer 本身不动。

---

## 5. Tab 的立场（R6）

不做 tab。立场记录如下：

- **拒绝 VS Code 式 buffer-tab-strip**：tab=buffer 的模型与"buffer 全局池 + 工作台过滤"冗余，且是 Emacs 用户从未需要的东西（helix bufferline 默认 Never，同样态度）。
- **将来若需要，tab = workbench 内命名 layout 页**：同 scope、同 mru，仅 `layout` 字段多份可切换。这形式化了"tab 未必对应 buffer"的直觉——tab 对应的是**布局**。技术上便宜：WindowLayout 是持久树，非活跃页的 View 依赖 §2.3 已有的存活机制。
- 触发条件（到了再做）：真实出现"同一工作台内反复手动重排窗口"的使用模式。[08-gui-chrome.md](08-gui-chrome.md) 已预留顶部空间可配置，届时 tab 条有处安放。

---

## 6. LanguageSession 路由：provenance 绑定（R7）

### 6.1 问题与架构判断

lsp-mode/eglot 的模型：buffer 属于哪个 LSP 会话由 **buffer 的文件路径**推导（查 project root → 查会话表）。跳出 project 的文件（系统头文件、第三方库源码）路径上不属于任何 project → 无会话 → find-def 链条断在第一跳。§1.8 的 workaround 都是在这个前提内打补丁。

架构判断：**绑定的正确依据不是"文件在哪"，而是"你是怎么到达它的"（provenance）**。这不只是方便——语义上就是对的：`/usr/include/vector` 的语义完全取决于编译上下文（哪个 TU、哪组 -I/-D），只有来源 project 的 clangd 拥有这个上下文。从路径推导在这类文件上**不存在正确答案**，从来源推导答案唯一。

### 6.2 设计

**已实现的一半——会话注册表**：LspSessionRegistry 按 project/root、语言、server 配置与 client capability 片段建键，全局唯一、跨 workbench 共享（[../docs/lsp.md](../docs/lsp.md)）。workbench 是可见性层，不拥有语义会话（R2 立场的延伸：会话跟 project 走，正如配置跟 project 走）。会话启动时的 root 解析：buffer 属于 project 取 project root；file-backed 但无 project 取资源父目录；无资源取当前目录——注意这仍是路径推导，R7 的断链问题在当前实现中依然存在。

**后续设计（未实现）——provenance 绑定**：

- **buffer→session 显式绑定**，不从路径反复推导。open 时一次解析，此后粘滞：
  1. **home**：buffer 的 project 有（或可启动）该语言的会话 → 绑定之。
  2. **guest**：无 home，且 buffer 由导航从绑定了会话 S 的 view 到达 → 绑到 S。didOpen 发给 S，此后该 buffer 的一切 LSP 查询走 S——**find-def 在系统头文件里可以无限连跳**。
  3. 都没有 → 不绑定；策略可选回退到 workbench scope 首 project 的会话。
- **管道复用**：§3 的显示咽喉携带 origin view——placement 消费 intent，LSP 绑定消费 origin。eglot 需要在 xref 侧单独旁挂哈希，正是因为它没有这条统一管道；cind 里 provenance 是咽喉上的一等信息，任何到达路径（xref、grep 命中、诊断跳转）自动携带。
- **冲突与生命周期**：home 永远优先（guest 只填空缺）；guest 先到先得——同一头文件之后又从 project B 跳入，维持原绑定（查询结果稳定 > 上下文新鲜），显式 rebind 兜底；buffer kill 或会话关停即解绑。
- **与 visitor 的关系**：guest 是 visitor 在语义层的镜像——同一哲学（外来文件零仪式进入工作上下文），两层正交：visitor 管工作台可见性，guest 管语义会话，各自独立成立（见 §11 场景 7）。

## 7. Project 级配置（R8）

已实现的一半：`.clang-format` 发现（CLion 语义：向上找、InheritParentConfig、preset 换算，[01-kernel.md](01-kernel.md) §9.5）与 project 自带 SettingsLayer。作用域链：view > buffer > **project** > editor default；workbench **不**进这条链——工作台不拥有配置，工作台级 settings 只是 §10 的扩展口。与 R3 超项目的组合天然干净：前后端两 repo 各自的配置独立生效，workbench 无需"合并配置"。

**后续设计（未实现）**：`.clang-format` 表达不了的部分——LSP server 选择与参数、格式化器、project 专属命令/钩子。

Emacs 对照：dir-locals 形态的两个结构性别扭——**数据与应用分离**（project 里只存一个样式名，真身在全局配置）；任意 elisp 注入靠 `safe-local-variable` 白名单事后围堵。

设计——project 根放 `.cind/`（或单文件，形态待定），**两级信任**：

1. **数据层（总是加载）**：声明式键值——style 覆盖、LSP server 与参数、格式化器选择。载入 project SettingsLayer。与 `.clang-format` 的分工：缩进/排版以 `.clang-format` 为权威（生态标准，已实现），数据层只放它表达不了的键；同键显式覆盖。
2. **代码层（信任门控）**：Guile 代码——project 专属命令、钩子、keymap 追加。未信任的 project 不执行（helix workspace_trust / Emacs risky-local-variable 是同一问题；cind 嵌 Guile，风险即任意代码执行，必须门控）。信任决定持久化在 editor 级（不写进 repo）。

## 8. 机制与实现位置

| # | 机制 | 状态 / 实现位置 |
|---|---|---|
| M1 | Workbench 实体（scope/layout/MRU/slots）、active 唯一、非活跃 Window/View 存活 | ✓ ../docs/workbenches.md；Scene 组合只读 active workbench 的 layout（../docs/gui-architecture.md） |
| M2 | Window placement 属性 role/pinned/created-by-policy | ✓ Guile 持策略状态 + native 校验（与早期"三个 C++ 字段"的设想不同，实际分工是 Guile 策略态） |
| M3 | display 咽喉：`display(buffer, intent, origin)`，Guile 策略解析 + native 校验 + 失败回退缺省策略；所有 C++ 直接 show_buffer 路径收编 | ✓ ../docs/workbenches.md「Buffer placement」 |
| M4 | Guile 工作台 API（summaries/scope/mru/adopt/expel/set-role/set-pinned、provider 过滤 + widen） | ✓ ../docs/scripting.md workbench 原语族 |
| M5 | 会话序列化/恢复（稳定路径、layout 树、jump 图与 walk、恢复容忍缺失文件） | ✓ ../docs/workbenches.md「Persistent sessions」 |
| M6 | LSP 会话注册表（../docs/lsp.md）；home/guest provenance 绑定 | 注册表 ✓；provenance 绑定**未实现**（§6.2 后续设计） |
| M7 | project 配置：`.clang-format` 发现 + project SettingsLayer | ✓ 数据通道；`.cind/` 数据层键值与代码层信任门控**未实现**（§7 后续设计） |

Guile 策略（全部可替换）：intent→解析规则表（§3.2）、adopt 时的划分保持策略（§2.2）、widen 行为、visitor 标记样式、modeline 工作台指示、auto-workbench（persp-def-auto-persp 形态：谓词 + 命名函数——默认**不启用**）、guest 绑定的回退与 rebind 提示策略（§6.2）、project 信任的询问形态（§7）。

---

## 9. 与既有设计的衔接

- **[03-input.md](03-input.md)**：per-view InputState 与工作台正交——View 随工作台切换存活，输入状态自然保留。interaction-class（editing/interface）与 intent 互补：class 决定 buffer 内按键行为，intent 决定 buffer 落在哪。`tools` slot 里的 Generated buffer 通常是 interface class，两机制在"grep 结果窗格"上会合但互不依赖。
- **[05-jump.md](05-jump.md)**：display 咽喉是跳转图的唯一自动边写入点；图与位置列表栈归属 workbench；会话序列化一并覆盖。
- **SettingsLayer**：已有 Buffer/View/Project 作用域；工作台作用域是自然扩展口（如每工作台主题色区分），留接缝不做。
- **remote / 多 OS 窗口**：Workbench 是前端无关实体。将来 GUI 多 frame 或 remote client，frame 绑一个 active workbench（persp 的 frame 指针形态），两 frame 可显示两个工作台——kakoune client 模型的进程内版本。"active workbench 唯一"届时放宽为 per-frame，数据结构不用改。
- **命名**：渲染层已占用 "workspace"（compose_editor_workspace，指多窗格投影到一帧），故本实体取名 Workbench，避免冲突。

---

## 10. 场景走查

1. **前后端分离超项目**：`workbench.new shop` → adopt backend/ → adopt frontend/。find-file/search 覆盖两 repo；buffer 列表只见 shop 的东西。另一工作台开 cind 内核开发，互不可见。划分成立（R3）。
2. **xref 与 pinned**：左窗 pin 看头文件，右窗 set-role jump，之后一切跳转确定落右窗（R5）。
3. **临时看一眼**：xref 跳进 /usr/include 或另一个 repo 的实现——buffer 显示、入 mru、完事。工作台列表、scope 无任何变化（R4）。
4. **grep/编译输出**：intent tools → 底部 slot 窗格出现，反复 grep 复用同一窗格，`q` 删除还原布局。
5. **三个工作台**：shop / cind / notes，modeline 显示当前名，picker 一键切，各自 layout 与 MRU 独立且切回原样（R1）。
6. **单工作台用户**：什么都看不见，行为 = 无工作台概念的 cind（R4 底线）。
7. **系统头文件连跳**（R7 验收目标；依赖 §6.2 未实现的 guest 绑定）：从 shop/backend 的 socket.cpp find-def 跳到 /usr/include/sys/socket.h——buffer 无 home project → guest 绑到 backend 的 clangd → 头文件内继续 find-def 任意连跳；同时该 buffer 自动成为工作台 visitor（visitor 侧今天已成立）。guest 与 visitor 两机制各自生效，无一行互相特判。
8. **样式随 repo 走**（R8 验收）：repo 根 `.clang-format` 声明风格，打开即生效——不需要"全局注册样式表 + dir-locals 存名字索引"两段式。（`.clang-format` 之外的 project 配置键属 §7 后续设计。）

---

## 11. 非目标

- IDE 式 "project 拥有窗口/配置/一切" 的反转（R2 明确反对）。
- buffer-tab-strip、工作台嵌套/分组/树。
- 按 buffer 名正则的显示匹配规则（Guile 想加是策略自由，内核不提供）。
- 强制划分不变式（§2.2，策略维护而非机制强制）。
- 自动工作台默认开启（机制留口、默认关闭）。
- dir-locals 式"任意变量按目录注入"（配置面收敛为 SettingsLayer 已知键 + 代码层显式 API，不做通用变量注入）。

## 12. 开放问题

- **scope 级搜索的结果形态**：project.search 覆盖多 repo 时结果标注形态（按 repo 分组 vs 混排）——看真实数据再定。
- **Process/终端 buffer 的工作台归属**：终端常跨项目使用，进 mru 即可还是要"全局常驻"特例——留到有终端 buffer 之后。
- **visitor 的退出时机**：mru 无限增长 vs 视图关闭后淡出——先无限（buffer 数量级小），观察后定。
- **`.cind` 数据层格式**：scheme data（read 即得，与代码层同文件分节）vs 独立纯数据格式——随 §7 后续设计一起定。
- **guest 的 rebind 时机**：同一头文件后来又从另一 project 跳入，是否提示 rebind——先不提示（稳定优先），观察后定。
