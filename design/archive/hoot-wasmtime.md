# 扩展语言：Hoot Scheme on wasmtime（调研存档）

状态：**搁置存档**。cind 实际采用的扩展语言是内嵌 Guile（见 [../../docs/scripting.md](../../docs/scripting.md)）；本文记录当时评估的 Hoot-on-wasmtime 路线——可行性已验证，无致命障碍，供将来需要沙箱化扩展时重启参考。

## 0. 动机与定位

- 编辑器核心保持静态、开箱即用；不做 elisp 式运行时重定义。
- 扩展语言用于：命令/keymap 定义、init 配置（init-cc.el 的等价物）、
  puni 之上更个人化的结构编辑命令、项目模型钩子。
- 选型：Guile Scheme，经 Hoot 编译到 WebAssembly，跑在 cind 内嵌的 wasmtime 上。
  cind 暴露 wasm host functions 作为编辑器 API。
  好处：能力安全沙箱（扩展默认无 IO）、引擎级中断（epoch/fuel，优于 elisp C-g）、
  扩展语言与内核完全解耦。

## 1. 可行性结论（2026-07-14 实测）

**代码生成层无障碍。** 关键事实：

- Hoot 0.9（~/project/hoot，HEAD c7126df）默认**不再产出 stringref**：
  `module/wasm/lower.scm` 默认策略 `'wtf8`，字符串降级为 `(array i8)` + 迭代器 struct，
  仅在宿主边界经 `rt::wtf8_to_string` / `rt::string_to_wtf8` 转换。
  （stringref wasmtime 永不支持，但已无关。）
- 默认产物只需要：**Wasm GC + tail-call + 新版异常（try_table/exnref）**。
  无 SIMD、无线程、无 stringref。
- wasmtime 对这三者全部 Tier 1 完整支持且可组合
  （唯一组合限制 GC×stack-switching，与 Hoot 无关）。
  repo main（2026-07 起）默认开启 WASM3 基线；
  已发布的 43.0.0 / 46.0.1（~/.wasmtime/bin/wasmtime）需要显式
  `-W gc=y,tail-call=y,exceptions=y,function-references=y`。

**实测记录**：`~/project/hoot/test/basic-types.wasm`（仓库自带的反射测试模块）
在 wasmtime 43.0.0 与 46.0.1 上完整通过 Cranelift 编译；
其 14 个 `rt::` import 用 ~30 行 wat stub（`--preload rt=rt.wat`）满足后实例化成功，
`scm_false` / `scm_from_f64` 等导出实际执行并返回 GC 引用。
stub 签名不必预先精确——wasmtime 链接报错会给出期望类型，逐个校正即可。
注意 `$0`（wtf8 数组）是独立的 `(type (array (mut i8)))`，不在大 rec group 里，
结构化类型匹配容易满足。

**官方立场**：Hoot README 明说 "unsupported on WASI runtimes"，git 史零 wasmtime commit。
但对象模型刻意宿主无关（identity hash word 内置于对象，design/log.md 2023-02-27
明确讨论过 wasmtime），且自带 Scheme 写的完整 wasm 解释器
（`module/wasm/vm.scm`）——`module/hoot/reflect.scm` 的 `%runtime-imports`
是全部宿主接口语义的**权威参考实现**，移植时照抄它，不用逆向 JS。

## 2. 双向调用协议（已有，纯 wasm）

- **Scheme → 编辑器**：`(hoot ffi)` 的 `define-foreign`。
  `(define-foreign buffer-insert! "editor" "insert" (ref string) i32 -> none)`
  string 自动 WTF-8 转换；编辑器对象走 `(ref extern)` + `define-external-type`
  包装成不相交类型。cind 侧注册同名 host function 即可。
- **编辑器 → Scheme**：`reflect-wasm/reflect.wat` 导出 `call`
  （`(ref $proc) (ref $vector)` → `(ref $vector)`），内部 trampoline 循环
  处理异常捕获与延续恢复。按键 → keymap 查 Scheme 闭包 → `call`。
- **值构造/解构**：反射模块导出 `scm_from_*` / `fixnum_value` / `car` / `cdr` 等。
- 参数传递经 `rt::get_argument` / `rt::set_return_value`（>8 参数走 argv table）。

## 3. 宿主要实现的 import（工程量，无未知数）

JS 参考实现：`reflect-js/reflect.js`（六个 namespace：rt、abi、debug、io、ffi、finalization）。

| 难度 | 内容 |
|---|---|
| 直接 | bignum（num-bigint 挂 externref）、Math 超越函数、时间、io 的 stdio/file |
| 免费 | WTF-8 转换：`reflect-wasm/wtf8.wat` 纯 wasm，preload 即可 |
| 麻烦 | weak map / FinalizationRegistry（见 §4.1）；RegExp 方言差异（先 stub） |
| 不适用 | fetch/WebSocket/streams 等浏览器专属（stub 成 die） |

调度器：Hoot 的 promises/fibers 依赖宿主 `async_invoke`
（JS 是 queueMicrotask/setTimeout）→ 映射到 TUI 主循环的任务队列。

## 4. 设计缺口（搁置前的评估）

### 4.1 Finalization/weak 语义 —— 最实质的一个
JS 宿主把 weak map 和 FinalizationRegistry 挂在 JS GC 上；wasmtime 宿主侧
**拿不到 GC 对象死亡通知**，host 持有的引用全是强引用。后果：
`define-external-type` 句柄无法自动回收，weak table 退化为强表（缓慢泄漏）。
解法方向：cind 句柄指向持久快照，本来就是值语义，泄漏的只是小句柄——
用**世代句柄表**（每次命令结束批量清一代）替代 finalizer。
这是需要明确接受的语义偏离，不是自动正确。

### 4.2 跨边界 API 粒度
按"一次调用返回一批结构"设计，不暴露逐 token 游标。
内核现有 API（`sexp_forward` / `soft_kill_end` 返回 TextRange）恰好就是对的粒度。
wasmtime host call 纳秒级，几十次/命令大概率无感，但要实测。

### 4.3 多扩展世界模型
- 共享：Hoot `--import-abi/--export-abi`，多 .wasm 共享 GC 类型空间和 stdlib
  实例，扩展间可传 Scheme 值（elisp 式单一世界）；宿主需实现 Hoot 的模块
  linker（reflect.js 有参考逻辑）。
- 隔离：每扩展一个 instance，只能经编辑器中转。
- **决定：先做单 instance（一个 init.wasm），多扩展模型推迟。**

### 4.4 反向持有
keymap 中的 Scheme 闭包是 GC 引用，须用 wasmtime 持久 root 管理；
rebind 时 unroot，防 root 泄漏。

### 4.5 配置的编译步骤
扩展作者需要 guile main + hoot 工具链产出 .wasm。
官方功能预编译分发（与"静态开箱即用"一致）；用户 init.scm 需要一次
`hoot compile`——配置即编译产物，与"不要运行时重定义"的立场一致。
远期选项：hoot 编译器自举成 wasm 随编辑器分发。

### 4.6 C++ 嵌入路径
wasmtime 文档矩阵标注 GC/EH 的 C API 已覆盖（Tier 1 全 ✅），
cind 直接链 libwasmtime C API 理论可行；但 GC 的 C API 较新，**首个实验就该验证它**。
备选：薄 Rust shim 编成静态库。

## 5. 本地环境缺口（重启时的第一步）

- Hoot 编译器需要 **guile main 分支**：3.0.9 缺 `(language cps utils)` 的
  `primcall-raw-representations`，本机 3.0.9 加载 `(hoot compile)` 直接失败（已试）。
  注意本机 `guile` 是 alias，真实命令 `guile3.0`；无 guix。
- 顺序：源码编 guile main → hoot `./bootstrap.sh && ./configure && make`
  → `./pre-inst-env guild compile-wasm -o out.wasm in.scm`。
- Node 22+ 也可当对照宿主：`hoot compile --run=node` 只额外需要
  `--experimental-wasm-exnref`。

## 6. 重启清单

1. 编 guile main + 构建 hoot，产出第一个自己的 .wasm。
2. 最小 embedder（wasmtime C API 或 Rust shim）：实例化 wtf8.wat + reflect.wat
   + 编译产物；验证 GC 引用在 C API 的往返 ← 最有信息量的实验。
3. rt 核心 host functions（bignum/Math/时间/io），语义照抄 `module/hoot/reflect.scm`。
4. 跑通 `(display "hello")` → 再接 `define-foreign` 的编辑器 API 原型。
5. 世代句柄表原型，验证 §4.1 的规避方案。
