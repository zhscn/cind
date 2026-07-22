# GUI Chrome — N Λ N O 风格视觉规格

## 定位

呈现层的视觉规格：色板、chrome（modeline / minibuffer / echo）布局与度量。GUI（Skia）与 TUI 消费同一 Scene 语义，规格对两个前端同时生效。帧模型与渲染架构见 [../docs/gui-architecture.md](../docs/gui-architecture.md)；配色、样式、度量经 Guile presentation 策略可整表替换（[../docs/scripting.md](../docs/scripting.md) 的 theme/style/metrics/typography 策略）；装饰（诊断等）的最终属性解析立场见 [07-decoration.md](07-decoration.md)；补全菜单的光标随动 overlay 见 [06-completion.md](06-completion.md)。

## 实现状态

已实现：色块分区 chrome、底部 minibuffer band 与 reflow、modeline chip、echo 行待决键序列、headless 截图调试，GUI 与 TUI 共享 scene 层实现；补全菜单的光标锚定浮动 overlay 已实现（[06-completion.md](06-completion.md)）。未实现：signature help 类其余光标随动 UI；modeline 位置可配置（当前固定底部）。

## 设计立场

借鉴 nano-emacs 的**视觉呈现语言**，不照搬其布局：

- **色块分区，零装饰**：无边框、无阴影、无圆角、无 hairline。
  画布（正文+gutter+echo）↔ 色带（modeline+minibuffer）两级地面，
  唯一的实色块是 modeline 状态 chip 与 picker 选中行，均配画布色文字。
- **modeline 保持在底部**（nano 在顶，但未来 tab/workspace 标识需要顶部空间；
  位置将来做成可配置，emacs header-line/mode-line 语义）。
- **会话型瞬时 UI 不用浮动 popup**：picker/which-key 是
  modeline 与 echo 行之间的通宽 minibuffer 色带。
  （范围限于会话型 UI——注意力已转移到输入框的交互；
  补全菜单/signature help 等光标随动型 UI 不适用此条——
  它们必须光标锚定浮动，补全菜单已按此实现，见 [06-completion.md](06-completion.md) §7。）
- **reflow 而非 overlay**：band 出现时正文与 modeline 上移让位
  （scene 层完成，TUI 同享），光标/层级不穿帮。
  （同样限会话型；光标随动型恰好相反——overlay 而非 reflow，
  打字中弹出/收起不得推动正文。）
- 配色 catppuccin Mocha 默认值，token 语义沿用 nano 九色思路，本身可换（theme 策略）。

## 色板（catppuccin Mocha 默认值，经 theme 策略可换）

| token | 值 | 用途 |
|---|---|---|
| canvas | #1E1E2E | 正文、gutter、echo 地面（base）|
| highlight | #2A2B3C | 当前行；非活动 modeline 底 |
| band | #313244 | modeline + minibuffer 色带（surface0）|
| selection | #45475A | 选区；非活动 chip 底（surface1）|
| divider | #11111B | 分屏 pane 分隔线（crust）|
| text | #CDD6F4 | 正文 |
| strong | #DEE4F7 | 加粗墨水：文件名、picker 输入 |
| faded | #7F849C | 次级墨水：注释、路径、echo；RW chip / 选中行底（overlay1）|
| faint | #6C7086 | 行号、计数、rev（overlay0）|
| salient | #89B4FA | 关键字/预处理、prompt（blue）|
| popout | #FAB387 | 字符串/数字（peach）|
| critical | #F9E2AF | 脏缓冲 `**` chip；修改 sign（yellow）|
| cursor | #F5E0DC | 光标（rosewater）|
| sign_added/deleted | #A6E3A1 / #F38BA8 | green / red |

语法 = nano 四色纪律：Keyword/Preprocessor→salient，String/Number→popout，
Comment→faded，Gutter→faint，其余 text。非活动 pane 整体 alpha 0xB0 降一档。

## Modeline（高 cell+12）

```
[RW] skia_presenter.cpp  /home/…/src/gui   ···spring···  .clang-format  120:1  4%
```

- **状态 chip**：占满带高、实色块 + 画布色粗体字。
  `RW` = faded 底；脏 `**` = critical 底；非活动 = selection 底 + faded 字。
  脏状态由 chip 表达，无独立脏点。（RO chip 预留 popout 底。）
- basename：strong 粗体；父目录 faded。
- 右组（右→左）：`--inspect` 时 rev faint → 百分比 faint → `行:列` faded
  → style origin faint。
- 无 keychip：**待决键序列**（非 last_key）显示在 echo 行右端 text 色，
  来源 `EchoContent.key`（EditorSceneInput.pending_key ←
  command_loop.pending_sequence_text()）。

## Minibuffer band

- scene 层：`RegionRole::Popup`、VerticalAnchor::Bottom、通宽，
  位于 modeline 与 echo 之间；`prompt 行 + 候选行` 占整数 cell 行，
  正文 text_rows 与 caret reveal 同步减去 popup_rows（精确 reflow）。
  容量 = min(候选数, 12, text_rows/2 − 1)。
- prompt 行：salient 粗体 prompt + strong 输入 + faint `k/N` 计数右对齐；
  which-key 无输入时为 faded 粗体标题。
- 候选行：label text 色，detail 紧随 label 后 14px、faded；
  选中行 = faded 平铺 + 画布色墨水（nano ivy 语义）。
- GUI 几何完全由 region rect 经 SceneVerticalLayout 导出；
  TUI 用同一 rect 逐行渲染（终端里同样是底部 minibuffer）。

## 度量

- footer 像素高：modeline cell+12、echo cell+8（`editor_footer_heights`），
  minibuffer 行 = 整 cell（保证 reflow 的 cell 数学精确）。
- 水平 padding：footer 12px、minibuffer 16px、chip 内 10px。
- 粗体 = 独立 bold SkFont（strong_font），非 fake bold。
- 各值经 metrics 策略（Scheme）可调；C++ 由 profile 导出字体相关矩形、
  hit target、damage 与 inspector 几何。

## 调试

- `--screenshot PATH` headless 渲染 raw BGRA；`--screenshot-keys "M-x s a"`
  可在截图前驱动按键（走 handle_key + insert_text 双通道，镜像 SDL 路由），
  用于截取 picker/which-key 状态。
- `rev` 段由 `set_show_debug_status`（--inspect）控制。
