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
                    └─> SkiaPresenter ─> render diagnostics
```

`EditorModel` 提供文档、caret、viewport 和编辑状态。`ui::Scene` 是后端无关的 cell
布局与 display list。`SkiaPresenter` 在 inspector 启用时采集字体指标和每个 primitive
的渲染边界。`InspectionHub` 在完成绘制后发布不可变的 `FrameInspection`，并保存最近
256 个输入事件。

每个 frame 通过 `cause_event_sequence` 关联触发重绘的事件。事件记录包含处理结果、
是否请求重绘以及处理前后的文档 revision。没有输入事件的重绘使用最近一次触发重绘
的事件序号。

Inspector 仅在显式启用时创建 socket、记录事件并采集逐 primitive 的 raster
diagnostics。普通 GUI 运行不承担逐 primitive 像素探测开销。

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
| `scene` | 完整 cell scene |
| `scene.cursor` | scene cursor |
| `scene.region.<role>` | 指定 region，例如 `scene.region.line-numbers` |
| `render` | 完整 renderer 状态和 primitive diagnostics |
| `render.font_metrics` | ascent、descent、leading 和行内 baseline |
| `render.primitives` | 所有已绘制 primitive 的 diagnostics |
| `render.primitive.<id>` | 指定稳定 ID 的 primitive diagnostics |
| `event.last_sequence` | 最新事件序号 |

例如：

```sh
cmk run -p gui cind-ui-inspect -- get scene.region.line-numbers
cmk run -p gui cind-ui-inspect -- get render.font_metrics
cmk run -p gui cind-ui-inspect -- get render.primitive.line:0/number
cmk run -p gui cind-ui-inspect -- pick 15 15
```

## Frame 数据模型

完整快照使用带版本号的 JSON schema。顶层包含：

- `editor`：文件、revision、文档大小、行数、dirty 状态、caret、viewport、line signs、
  tab width、style 来源、消息和最近按键。
- `scene`：cell 网格、cursor、region 几何和 display primitives。Scene region 使用
  0-based cell 坐标；scene cursor 使用 1-based 坐标。
- `render`：视频驱动、window 与 output 大小、device scale、cell 大小、字体、主题、
  pixel hash 和 primitive diagnostics。
- `recent_events`：事件序号、类型、细节、处理结果、重绘标记和 revision 转换。
- `violations`：模型、scene 与 renderer 之间的 invariant 违规。

Scene primitive ID 在一帧内用于连接 scene、pick 和 renderer 数据。编辑器生成的
primitive 使用面向文档位置的 ID，例如 `line:4/number` 和 `line:4/byte:27`。没有
显式 ID 的 primitive 使用 region、cell 和 kind 组成的确定性 ID。

## Renderer diagnostics

Renderer 坐标使用 device scale 之前的逻辑像素。物理 output 像素可以通过逻辑坐标
乘以 `display_scale` 得到。`pick` 接受同一逻辑像素坐标系中的 window 坐标。

每个 renderer primitive 包含以下边界：

- `cell_bounds`：scene 为该 primitive 分配的预期 cell 矩形。多 cell 文本的宽度由
  display width 和 cell width 共同决定。
- `shape_bounds`：Skia shaping 结果的保守 bounds。字体 ascent、fallback 字体和
  shaping 实现可能使它超出 cell；该字段用于解释 shape 决策，不能单独证明可见像素
  越界。非文本 primitive 的值为 `null`。
- `paint_bounds`：绘制 primitive 前后实际发生变化的 raster 像素包围盒，并转换回
  逻辑像素。没有产生可见像素时值为 `null`。

`draw_bounds_cross_region_clip` 表示 primitive 的计划绘制 bounds 超出 region clip。
`row_overflow` 和 `column_overflow` 使用 `paint_bounds` 与 `cell_bounds` 比较。跨行绘制
会同时写入 frame violations；横向 overhang 保留为诊断信息，因为部分 glyph bearing
允许正常的横向越界。

字体指标保留 Skia 的符号约定：`ascent` 通常为负值，`descent` 和 `leading` 通常为
非负值。`baseline_from_row_top` 是从 cell 行顶到 baseline 的逻辑像素距离。

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

## Invariant 检查

发布 frame 时会检查：

- caret 和 viewport 是否处于文档范围内；
- scene、region、primitive 和 cursor 几何是否合法；
- render grid 是否与 scene grid 一致；
- window、output、scale 和 cell 几何是否合法；
- renderer primitive 是否仍能关联到对应 scene primitive；
- 实际 raster paint 是否跨越分配的 scene row。

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
- Event ring 固定保留 256 条记录。请求早于可用范围时，事件批次使用 `gap: true`
  表示缺口。
- `paint_bounds` 通过比较 primitive 绘制前后的像素得到。与已有像素完全同色的重复
  绘制不会扩展该 bounds。
- Raster diagnostics 面向调试正确性，启用 inspector 会增加与可见 primitive 面积
  相关的内存复制和扫描成本。
- Inspector 暴露实现状态，不构成插件 ABI 或 accessibility API。

## 扩展约定

新增 inspector 字段时应保持跨层关联：模型字段进入 `EditorStateSnapshot`，布局字段
进入 `ui::Scene`，renderer 字段进入 `RenderStateSnapshot`。序列化、`get` 查询、tree
摘要和 invariant 检查应使用同一字段语义。

修改顶层 JSON 契约时递增 `FrameInspection::schema_version`。Primitive diagnostics
通过 scene index 采集，并在 frame 发布时解析为稳定 ID；presenter 不依赖 inspector
协议或 socket 实现。
