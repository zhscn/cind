# GUI Inspector

GUI inspector 是 cind 自绘界面的只读调试接口。它把编辑器模型、cell scene、Skia
渲染状态和输入事件关联到同一个已绘制帧，使人和自动化 agent 能在不依赖平台 UI
控件树的情况下检查界面状态。

Inspector 不是 accessibility tree。Accessibility 描述用户可感知的语义和可执行
操作；inspector 描述实现状态、布局中间表示、渲染边界和事件因果关系。两者可以共享
稳定的语义 ID，但具有不同的数据契约和安全边界。

## 架构

```text
SDL event ────────────────> event ring
                               │
EditorModel ──> ui::Scene ─────┼──> InspectionHub ──> Unix socket ──> cind-ui-inspect
                    │          │
                    └─> SceneDamageTracker ─> SkiaPresenter ─> retained raster
                                                  │
                                                  └─> render diagnostics
```

`EditorModel` 提供文档、caret、viewport 和编辑状态。`ui::Scene` 是后端无关的 cell
布局与 display list。`SceneDamageTracker` 比较相邻帧的 cell visual state，生成内容和
cursor 损伤区域。`SkiaPresenter` 只清除并重绘损伤逻辑像素矩形；SDL streaming texture
只上传对应的物理 output 像素矩形。SDL renderer 仍将完整 retained texture 合成到当前
swapchain image，因为 swapchain image 不保留上一帧内容。

Scene 网格对 output 可容纳的 cell 数向上取整。Status 和 echo region 使用 bottom
anchor，在 Skia 逻辑像素空间保持完整行高并贴合 output 底边；正文、行号和 change
sign region 裁剪到 footer 顶边。拖拽 resize 的不足一行余量因此显示为正文的部分 cell，
不会裁掉 footer 或留下未布局区域。Picker 和 which-key 窗口使用 overlay anchor，不随
正文的分数滚动位置移动。最后一列直接由 output 右边界裁剪。同一个垂直布局映射用于
raster、damage、鼠标 hit-test、IME 输入位置和 inspector `pick`。

`SkiaPresenter` 在 inspector 启用时采集字体指标和每个 primitive 的渲染边界。增量帧还
会绘制一份独立的完整参考 raster，并与 retained raster 比较。`InspectionHub` 在完成
绘制后发布不可变的 `FrameInspection`，并保存最近 256 个输入事件。

每个 frame 通过 `cause_event_sequence` 关联触发重绘的事件。事件记录包含处理结果、
是否请求重绘以及处理前后的文档 revision。没有输入事件的重绘使用最近一次触发重绘
的事件序号。

Inspector 仅在显式启用时创建 socket、记录事件、采集逐 primitive 的 raster
diagnostics，并执行完整参考 raster 校验。普通 GUI 运行只执行损伤重绘和局部纹理
上传。

## 启动与连接

使用默认 socket 启动 GUI：

```sh
cmk run -p gui cind-gui -- --inspect path/to/file.cpp
```

默认 socket 为：

```text
$XDG_RUNTIME_DIR/cind/ui-<pid>.sock
```

没有 `XDG_RUNTIME_DIR` 时，runtime 目录位于系统临时目录下的 `cind-<uid>`。也可以
指定固定路径：

```sh
cmk run -p gui cind-gui -- \
  --inspect-socket /tmp/cind-debug.sock path/to/file.cpp
```

列出可发现的 inspector：

```sh
cmk run -p gui cind-ui-inspect -- list
```

只有一个 inspector 运行时，CLI 会自动选择它。多个实例并存时，使用进程号或 socket
路径：

```sh
cmk run -p gui cind-ui-inspect -- --pid 12345 tree
cmk run -p gui cind-ui-inspect -- --socket /tmp/cind-debug.sock snapshot
```

## 命令

| 命令 | 返回值 |
| --- | --- |
| `snapshot` | 最新 frame 的完整 JSON 快照 |
| `snapshot-after <frame-id>` | 仅在存在更新 frame 时返回完整 JSON |
| `tree` | 适合人工阅读的 editor、scene 和 renderer 树 |
| `get <path>` | 指定状态节点的 JSON 或标量 |
| `pick <window-x> <window-y>` | window 逻辑像素坐标对应的 cell、region、primitive 和 render diagnostics |
| `events [after-sequence]` | 指定序号之后的事件批次 |
| `wait-frame <frame-id>` | 等待更新 frame，单次等待上限为一秒 |
| `wait-events <event-sequence>` | 等待更新事件，单次等待上限为一秒 |
| `watch frames` | 持续输出更新 frame |
| `watch events` | 持续输出事件批次 |

常用 `get` 路径：

| 路径 | 内容 |
| --- | --- |
| `frame.id` | frame 序号 |
| `frame.cause_event_sequence` | 触发该 frame 的事件序号 |
| `frame.violations` | 跨层 invariant 违规 |
| `editor` | 完整编辑器状态 |
| `editor.caret` | caret 的 byte、line 和 column |
| `editor.viewport` | viewport 起始行列 |
| `editor.line_signs` | change sign 摘要 |
| `editor.command_loop` | 按作用域排列的 active keymap、override map、pending key sequence、repeat 和 last command |
| `editor.interaction` | prompt/picker 输入、provider、候选、选中项、generation 和错误 |
| `editor.buffers` | 所有打开 buffer 的资源、当前 window 缓存的 view ID、modified、saving 和 active 状态 |
| `editor.windows` | window、绑定的 view/buffer ID 和 active 状态 |
| `editor.focus` | active window 和当前输入目标（window 或 interaction） |
| `scene` | 完整 cell scene |
| `scene.cursor` | scene cursor |
| `scene.region.<role>` | 指定 region，例如 `scene.region.line-numbers` |
| `render` | 完整 renderer 状态和 primitive diagnostics |
| `render.font_metrics` | ascent、descent、leading 和行内 baseline |
| `render.animation` | 当前滚动/光标过渡的进度和逻辑像素位置 |
| `render.damage` | 当前帧的逻辑/物理损伤矩形、覆盖率和参考 raster 校验结果 |
| `render.primitives` | 所有已绘制 primitive 的 diagnostics |
| `render.primitive.<id>` | 指定稳定 ID 的 primitive diagnostics |
| `event.last_sequence` | 最新事件序号 |

例如：

```sh
cmk run -p gui cind-ui-inspect -- get scene.region.line-numbers
cmk run -p gui cind-ui-inspect -- get editor.command_loop
cmk run -p gui cind-ui-inspect -- get editor.interaction
cmk run -p gui cind-ui-inspect -- get editor.buffers
cmk run -p gui cind-ui-inspect -- get editor.windows
cmk run -p gui cind-ui-inspect -- get editor.focus
cmk run -p gui cind-ui-inspect -- get render.font_metrics
cmk run -p gui cind-ui-inspect -- get render.animation
cmk run -p gui cind-ui-inspect -- get render.damage
cmk run -p gui cind-ui-inspect -- get render.primitive.line:0/number
cmk run -p gui cind-ui-inspect -- pick 15 15
```

## Frame 数据模型

完整快照使用带版本号的 JSON schema。顶层包含：

- `editor`：活动文件、revision、文档大小、行数、dirty 状态、caret、连续行单位的
  viewport、line signs、tab width、style 来源、消息、最近按键、active window、输入焦点、
  command loop、交互状态以及 buffer/window 列表。Command loop 的 layer 同时记录 keymap
  名称、parent chain 和 window/view/buffer/mode/editor/global/interaction 作用域；交互状态的
  `input_cursor` 是 minibuffer UTF-8 输入中的 byte offset。
- `scene`：cell 网格、cursor、活动文本行、region 几何和 display primitives。Scene
  region 使用 0-based cell 坐标并声明 vertical anchor；`grid_offset_rows` 表示顶部行的
  分数偏移；scene cursor 使用 1-based 坐标。Popup region 还携带结构化 title、input、
  `input_cursor`、`first_item`、`total_items`、`selected_item` 以及可见项的 label 和 detail，
  供 GUI 进行独立于 cell 网格的浮层布局并检查 text/list viewport；StatusBar region 同样
  携带结构化 path、dirty、line、column、line_count、revision、style_origin 和 key，供
  GUI modeline 独立排版（TUI 仍消费 cell primitives）。
- `render`：视频驱动、SDL render driver、window 与 output 大小、device scale、cell
  大小、字体、主题、pixel hash、animation progress/velocity、damage 和 primitive
  diagnostics。
- `recent_events`：事件序号、类型、细节、处理结果、重绘标记和 revision 转换。
- `violations`：模型、scene 与 renderer 之间的 invariant 违规。

Scene primitive ID 在一帧内用于连接 scene、pick 和 renderer 数据。编辑器生成的
primitive 使用面向文档位置的 ID，例如 `line:4/number` 和 `line:4/byte:27`。没有
显式 ID 的 primitive 使用 region、cell 和 kind 组成的确定性 ID。

## Renderer diagnostics

Renderer 坐标使用 device scale 之前的逻辑像素。物理 output 像素可以通过逻辑坐标
乘以 `display_scale` 得到。`pick` 接受 window 坐标并转换到这一逻辑像素空间，优先
命中最上层 renderer primitive 的 `layout_bounds`，因此能检查脱离 cell 网格的 GUI
浮层。底部锚定的 region 使用 `ui::editor_footer_heights` 的逐 region 像素高
（modeline = cell + 12，echo = cell + 8），`pick` 的行映射与 GUI 绘制保持一致。

每个 renderer primitive 包含以下边界：

- `layout_bounds`：presenter 为 primitive 分配的布局矩形。编辑器网格 primitive 使用
  cell 映射后的逻辑像素；GUI popup primitive 使用浮层自己的 logical-pixel 布局。
- `shape_bounds`：Skia shaping 结果的保守 bounds。字体 ascent、fallback 字体和
  shaping 实现可能使它超出 cell；该字段用于解释 shape 决策，不能单独证明可见像素
  越界。非文本 primitive 的值为 `null`。
- `paint_bounds`：绘制 primitive 前后实际发生变化的 raster 像素包围盒，并转换回
  逻辑像素。没有产生可见像素时值为 `null`。

`draw_bounds_cross_region_clip` 表示 primitive 的计划绘制 bounds 超出 region clip。
`row_overflow` 和 `column_overflow` 使用 `paint_bounds` 与 `layout_bounds` 比较。跨行绘制
会同时写入 frame violations；横向 overhang 保留为诊断信息，因为部分 glyph bearing
允许正常的横向越界。

字体指标保留 Skia 的符号约定：`ascent` 通常为负值，`descent` 和 `leading` 通常为
非负值。`baseline_from_row_top` 是从 cell 行顶到 baseline 的逻辑像素距离。

### Animation diagnostics

滚动过渡在逻辑像素空间分别平移上一帧和目标帧的 grid layer。Bottom-anchored status bar
和 echo area 保持在 output 底部。光标过渡使用独立的逻辑像素矩形覆盖 scene 的 cell 对齐
光标。过渡持续期间，每一帧使用完整 raster 和完整 texture upload；静止帧继续使用 retained
raster 和局部 damage。标准滚轮步进移动三行；高精度滚轮的分数 delta 会累积到同一
滚动尺度。

`render.animation` 包含：

- `active`、`scroll` 和 `cursor`：当前帧启用的过渡层；
- `scroll_progress` 和 `cursor_progress`：应用缓动后的 `[0, 1]` 进度；
- `source_grid_offset_y` 和 `target_grid_offset_y`：滚动源与目标 grid layer 的逻辑像素
  偏移；
- `cursor_rect`：动画光标的逻辑像素矩形；未启用光标过渡时为 `null`。

动画帧的 `render.damage.full_repaint` 为 `true`。Inspector 使用相同的动画状态生成独立完整
raster，并通过 `full_reference_match` 校验呈现缓冲区。

### Damage diagnostics

Scene damage 以发生视觉变化的 cell 为输入，而实际渲染和上传以像素矩形为边界。文本
损伤在逻辑像素空间保守扩展，以覆盖 italic bearing、抗锯齿和 fallback glyph 可能产生
的 cell 外像素；cursor 损伤使用 cursor 自身的窄像素范围。Bottom-anchored cell 先映射
到 footer 的实际逻辑像素位置。逻辑矩形乘以 `display_scale` 后向外取整并裁剪到 output，
得到 streaming texture 的更新矩形。

`render.damage` 包含：

- `full_repaint`：首次绘制、输出几何变化、scale 变化、大面积 scene 变化或动画帧触发
  完整 raster。
- `damaged_cells`：内容 visual state 发生变化的 cell 数。仅 cursor 移动时可以为零。
- `damaged_output_pixels` 和 `output_fraction`：合并后的物理损伤矩形覆盖面积及其占
  output 的比例。
- `rects[].logical`：Skia 使用的逻辑像素 clip。
- `rects[].output`：SDL texture 上传使用的物理像素矩形。
- `full_reference_match`：retained raster 与同一 scene 的独立完整 raster 是否逐像素
  相同。

没有视觉变化的重绘请求产生空 damage 列表，并直接复用 retained raster。多个相接或
相交的损伤矩形会合并，避免重复 shaping、raster 和 texture upload。

## 调试工作流

### 布局或绘制错位

先检查跨层摘要：

```sh
cmk run -p gui cind-ui-inspect -- tree
cmk run -p gui cind-ui-inspect -- get frame.violations
```

使用 window 坐标找到目标 primitive：

```sh
cmk run -p gui cind-ui-inspect -- pick 15 15
```

再按 ID 比较 scene cell、shape 和实际 raster bounds：

```sh
cmk run -p gui cind-ui-inspect -- get render.primitive.line:0/number
```

Scene row 或 cell 错误属于布局层。Scene 正确而 `paint_bounds` 错误属于 presenter、
字体或缩放层。只有 `shape_bounds` 超出 cell 且 `paint_bounds` 正常时，属于保守字体
bounds，而不是可见溢出。

### 编辑状态或事件问题

持续观察事件：

```sh
cmk run -p gui cind-ui-inspect -- watch events
```

事件的 `handled`、`repaint` 和 revision 转换用于判断输入是否到达命令层、是否修改
文档以及是否请求新 frame。`cause_event_sequence` 用于把屏幕结果关联回输入事件。

持续观察 frame：

```sh
cmk run -p gui cind-ui-inspect -- watch frames
```

`pixel_hash` 用于判断 raster output 是否发生变化；它不是持久缓存键，也不替代逐字段
状态比较。

### 增量绘制问题

检查损伤摘要和参考校验：

```sh
cmk run -p gui cind-ui-inspect -- get render.damage
cmk run -p gui cind-ui-inspect -- get frame.violations
```

`full_reference_match: false` 表示 damage 计算或 clip 范围遗漏了可见像素。对应 frame 的
scene、damage rectangles、primitive bounds 和 pixel hash 共同提供复现上下文。
`output_fraction` 用于区分局部编辑是否退化为大面积 raster；首次绘制、resize 和 scale
变化的值为 1。

## Invariant 检查

发布 frame 时会检查：

- caret 和 viewport 是否处于文档范围内；
- viewport 分数偏移是否与 scene grid 原点一致；
- 静止帧的文本 caret 是否被 grid clip 裁切；
- scene、region、primitive 和 cursor 几何是否合法；
- render grid 是否与 scene grid 一致；
- window、output、scale 和 cell 几何是否合法；
- renderer primitive 是否仍能关联到对应 scene primitive；
- 实际 raster paint 是否跨越分配的 scene row；
- damage 的物理像素矩形是否处于 output 内；
- retained raster 是否与完整参考 raster 一致。

`tree` 会显示 violation 总数，并展开跨行 primitive 的 cell 与 paint Y 范围。自动化
工具应读取 `frame.violations`，同时保留完整 snapshot 作为问题上下文。

## 并发、协议与安全

`InspectionHub` 使用不可变的共享 frame 快照。发布操作在锁内替换最新 frame，查询端
不会观察到半写入状态。等待 frame 和事件使用条件变量；socket server 可以并发处理
多个只读客户端。

Unix socket request 是以换行结束、最大 4096 bytes 的单行命令。Response header 为
`OK <bytes>` 或 `ERR <bytes>`，后接精确长度的 payload。CLI 拒绝超过 64 MiB 的
response。

默认 runtime 目录权限为 `0700`，socket 权限为 `0600`。Inspector 不提供状态修改、
输入注入或命令执行接口；访问权限由本地 Unix socket 文件权限控制。

## 约束

- Frame 只在完成绘制后发布；没有重绘时，模型的中间状态不会成为 inspector frame。
- Window 列表包含且仅包含一个 active window，其 ID 与 `editor.active_window` 一致。
- Command keymap stack 的末层是 `global`；interaction 获得焦点时首层是 `interaction`，
  document window 的 local/mode layers 不进入该焦点栈。
- Popup 的可见项位于 `[first_item, first_item + items.size())`，选中项使用全局索引并位于
  该区间内；可见项与 popup primitives 一一对应。
- Event ring 固定保留 256 条记录。请求早于可用范围时，事件批次使用 `gap: true`
  表示缺口。
- `paint_bounds` 通过比较 primitive 绘制前后的像素得到。与已有像素完全同色的重复
  绘制不会扩展该 bounds。
- Raster diagnostics 面向调试正确性，启用 inspector 会增加完整参考帧的 raster、
  内存复制和逐像素比较成本。
- Inspector 暴露实现状态，不构成插件 ABI 或 accessibility API。

## 扩展约定

新增 inspector 字段时应保持跨层关联：模型字段进入 `EditorStateSnapshot`，布局字段
进入 `ui::Scene`，renderer 字段进入 `RenderStateSnapshot`。序列化、`get` 查询、tree
摘要和 invariant 检查应使用同一字段语义。

修改顶层 JSON 契约时递增 `FrameInspection::schema_version`。Primitive diagnostics
通过 scene index 采集，并在 frame 发布时解析为稳定 ID；presenter 不依赖 inspector
协议或 socket 实现。
