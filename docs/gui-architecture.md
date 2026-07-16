# GUI architecture

cind 的 GUI 是编辑器核心的一种像素前端。编辑命令、buffer、view、window、interaction
和场景语义不依赖 SDL 或 Skia；SDL 管理平台窗口与输入，Skia 把已经准备好的帧布局绘制到
retained raster。

```text
SDL event
    │
    ├─> normalized key / text / pointer
    │          │
    │          v
    │   EditorApplication ──> Buffer / View / WindowLayout / Interaction
    │          │
    │          v
    │   layout_editor_scene (pure view-state transition)
    │          │
    │          v
    │   compose_editor_workspace ──> semantic Scene ──> ViewTree
    │                                      │
    │                                      v
    │                             GuiFrameController
    │                                      │
    │                                      v
    │                               PresentedFrame
    │                         ┌────────────┼────────────┐
    │                         v            v            v
    └─ pointer hit <── prepared layout   damage      inspector
                              │
                              v
                         SkiaPresenter ──> retained raster ──> SDL texture
```

## State and command boundary

`EditorApplication` is the frontend-independent controller. It owns the runtime registries,
buffer lifecycle, view state, window bindings, command loop and active interaction. Platform
frontends translate native events into `KeyStroke`, UTF-8 text input, scroll deltas and semantic
pointer targets. Editing commands do not inspect SDL events or rendering geometry.

`EditorModel` adapts the application state to the standard editor scene. It owns only presentation
state that belongs to this frontend composition, such as the popup list viewport and line-sign
cache. The document caret, selection, viewport and interaction input remain owned by their editor
objects.

`WindowLayout` is the persistent split tree. Its leaves are `WindowId` values and its branches
carry row/column orientation plus a normalized ratio. Splitting creates a new View for the same
Buffer, so caret, selection and viewport can diverge without duplicating document state. Deleting
a pane removes its cached views through the same registry lifecycle. The tree partitions a display
extent into window rectangles and divider lines for both frontends.

The keymap stack follows the focused target. Window, view, buffer and mode maps describe document
focus; interaction maps describe popup and minibuffer focus; application-global and system
override maps remain available at both targets. This keeps input routing data-driven and prevents
widgets from implementing special key paths.

## View reduction and scene composition

`layout_editor_scene` is a pure reducer from immutable model input and retained view state to a new
`EditorSceneViewState`. It resolves caret reveal and list selection without drawing or mutating the
editor model. `compose_editor_scene` converts one resolved view into an immutable local Scene;
`compose_editor_workspace` projects those local Scenes into the window layout and adds global
overlay and echo chrome.

A Scene describes one frame in backend-independent terms:

- document and gutter regions contain local cell primitives and document-coordinate mappings;
- status, echo and popup regions contain semantic content, including text input byte offsets and
  list selection;
- pane metadata records stable owner IDs, rectangles and active state, while divider metadata
  records orientation and span;
- each pane-owned region declares its pane ID, active state and independent content offset;
- the Scene carries the active document grid offset and logical caret state; overlay and echo
  remain global workspace layers.

Every region has exactly one content variant. Backends project semantic chrome into their native
layout instead of receiving both semantic data and a second mirrored display list.

`ViewTree` derives the explicit `Grid`, `Chrome` and `Overlay` hierarchy from the Scene. Painting
walks these layers from back to front; hit testing walks them from front to back. Z-order therefore
does not depend on the storage order of `Scene::regions`.

## Pixel layout and frame ownership

`SkiaFrameLayout` is the immutable pixel layout of one stable Scene at one logical viewport size.
It owns shaped document runs for every TextArea, popup layout, echo layout, cursor geometry and the
semantic view tree used by a Skia presenter. Its source Scene outlives the layout and remains
unchanged.

Workspace rows partition the logical pixel area above the global echo strip. Each pane derives one
pixel rectangle from that partition; its modeline is bottom-aligned at the standard modeline
height, and its document and gutter regions share the remaining clipped body rectangle. Document
baselines, divider positions, pointer hits and damage projection all consume this mapping, so
fractional resize space belongs to pane content instead of overlapping pane-local chrome.

`GuiFrameController` turns a composed Scene into a `PresentedFrame`. A presented frame owns:

- the target Scene and its prepared layout;
- every visible scroll-keyframe Scene and its prepared layout;
- the interpolated caret and active-line presentation;
- logical and output-pixel damage;
- the animation and output geometry inspected by the platform shell.

The controller caches prepared layouts by Scene identity and logical viewport. Exact-equivalent
successive Scenes retain their Scene identity. During scrolling, the cache retains the current
target and the keyframes referenced by the animation; unrelated layouts are released. Painting,
pointer hit testing, IME placement, damage conversion and inspection consume the layouts stored in
the presented frame. None of these consumers reshapes text or reconstructs geometry from rows,
columns or font metrics.

`SkiaAnimationFrame` is the logical transition assembled from the scroll timeline.
`SkiaPreparedAnimationFrame` is its frame-scoped rendering form: every layer pairs its Scene with
the exact `SkiaFrameLayout` used for that layer. Public convenience rendering can prepare a logical
animation, while the GUI runtime passes the already prepared form.

## Painting and damage

`SceneDamageTracker` compares semantic scene content and produces cell-level damage. Stable region
IDs identify content across frames; identity revision metadata does not independently invalidate
pixels. `SkiaPresenter::damage_rects` projects scene damage through the prepared layout and expands
text damage conservatively for shaped glyph ink. `GuiFrameController` converts logical damage into
outward-rounded physical output rectangles.

Static frames update only damaged ranges of the retained raster and the corresponding SDL streaming
texture rectangles. Animated frames repaint the full raster because several Scene keyframes and a
transient view overlay contribute to one output. SDL composites the retained texture into the
current swapchain image.

Bottom-anchored chrome and overlay regions are fixed while grid layers move. A scroll timeline
selects adjacent viewport snapshots around the visual scroll position. Each prepared layer owns a
non-overlapping vertical band, with an ink guard at the seam for glyph overhang. The caret,
active-line fill and active line number use one `SkiaViewPresentation`, so they share one animation
sample.

A workspace contains independently scrolling pane grids and therefore uses direct presentation
with normal scene damage. A single-grid Scene uses the scalar scroll timeline and caret animation.
Active and inactive panes remain Scene semantics: the presenter selects dedicated theme tokens for
modeline surfaces and text emphasis, and paints every split from the same hairline token used by
the rest of the chrome.

## Input hit path

Pointer input is resolved from the frame that was actually displayed:

```text
logical pixel
    ─> fixed overlay hit
    ─> visible prepared scroll layer hit
    ─> target grid hit
    ─> ViewHit
    ─> HitTarget
    ─> editor command/state transition
```

`ViewHit` records scene-local geometry. `resolve_hit_target` adds stable view and pane identity and semantic
meaning such as document line/display column, popup item, status or echo. `EditorModel::click`
focuses the owning Window before applying a document position. It does not derive document
positions from gutter widths or viewport offsets.

## Backend boundary

The ANSI and Skia backends consume the same Scene and ViewTree. ANSI projects semantic status,
echo and popup content into terminal cells. Skia shapes the same content in logical pixels and may
use fractional positioning, clipping, shadows and native font fallback. Backend-specific layout
objects do not enter the editor runtime or command APIs.

SDL is a platform shell rather than a widget toolkit in this design. It owns the Wayland window,
scale, input method, clipboard integration, renderer and texture upload. Scene composition and hit
semantics remain valid for another window-system or graphics backend.

Background work enters the frontend through a wakeup rather than a render timer. The shared
[asynchronous runtime](async-runtime.md) posts an SDL user event when main-thread completions are
ready; the event loop drains those completions before deciding whether to compose another frame.

## Inspector boundary

The inspector publishes model state, semantic Scene content, ViewTree order, prepared layout
diagnostics, animation state, damage and recent input events for the same `PresentedFrame`. Stable
view and primitive IDs connect these layers. Pixel picking follows the presented frame's prepared
layers and returns a semantic target, allowing a rendering defect and an editor-state defect to be
distinguished without a native widget tree.

## Architectural invariants

The following contracts prevent local rendering fixes from becoming alternate architecture:

1. Editor behavior is expressed as commands and state transitions, not platform-event branches.
2. Layout reduction is pure; scene composition does not mutate model or retained view state.
3. A region has one authoritative content representation.
4. Layer order is explicit in `ViewTree` and is shared by painting and hit testing.
5. Pixel geometry is prepared once for a Scene and viewport and is reused by every frame consumer.
6. Input is resolved against the displayed `PresentedFrame`, including transient scroll layers.
7. Animation samples caret, active-line and scroll presentation through one frame controller.
8. Damage is derived from semantic visual state and projected through prepared geometry.
9. Backends translate presentation only; buffer, view, window and interaction ownership stays in
   the editor core.
10. Inspector data describes the same objects used for presentation and input, rather than a
    separately reconstructed debug model.
11. Pane geometry, focus and active styling originate in the WindowLayout and Scene; presenters do
    not maintain an alternate split tree.

New GUI features join these boundaries by adding semantic Scene content, ViewTree structure or a
prepared-layout type. A feature that needs separate geometry in paint, hit testing and inspection
belongs in the prepared layout rather than in three consumer-specific calculations.
