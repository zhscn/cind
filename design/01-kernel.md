# 语义缩进 / 语法内核

**实现语言：** C++23
**运行形式：** 可嵌入的 headless library + CLI test harness

## 定位

本文是 cind 文档体系的最内层：定义文档模型、lossless lexer、容错 CST（green tree）、增量 reparse、缩进服务与 Enter 管线。文本存储（Text 值、undo 树）由配套的 [02-buffer.md](02-buffer.md) 定义。UI、输入系统、脚本、LSP、补全属于编辑器其他层，不在本层内：输入系统见 [03-input.md](03-input.md)，工作台与 LSP 会话见 [04-workbench.md](04-workbench.md)，补全见 [06-completion.md](06-completion.md)，命令与脚本的实现文档见 [../docs/command-loop.md](../docs/command-loop.md) 与 [../docs/scripting.md](../docs/scripting.md)。

## 实现状态

已实现：T0–T3 全部缩进层级、`.clang-format` 读取、lossless lexer 与分块 TokenBuffer、green tree 完全体（长度编码 GreenNode 唯一 source of truth、red 节点 lazy 物化、增量 reparse splice 复用）、PP 跨分支括号平衡（snapshot-restore + PPReopenedScope phantom）、Enter/typed-char 管线、trace、CLI harness、Scene/Presenter 渲染切分。实测：52 万行文件 0.49ms/字符、0.94ms/Enter；bench（`--style file --clean-only`）Support 99.93% / Sema 99.38% / IR 99.75%。

未实现：兄弟列表的 (2,3)-树平衡、多文件、remote 帧 diff presenter（§18）。

---

## 1. 目标

本层解决一个问题：

> 在源代码尚未完成、可能包含语法错误和宏的情况下，根据当前文档的结构语义，为一次换行或显式缩进操作计算稳定、可配置、可解释的缩进。

目标体验对标 CLion 内置 C++ 缩进器：

* 按下 Enter 立即得到正确缩进；
* namespace、class、case、access specifier、构造函数初始化列表等结构分别可配置；
* 能处理未闭合的括号、缺失的分号和正在输入的声明；
* Enter 行为由**动作时刻的文档结构**决定，而不是按键历史——光标处于 `{|}` 之间按 Enter，右括号移动到独立行并正确缩进，无论这对括号何时、如何输入；
* 局部语法错误不导致远距离缩进崩坏；
* 每次计算输出可解释的 trace；
* 相同文档、位置和配置得到确定结果。

### 1.1 路线定位

与扫描式启发式（Emacs cc-mode 一系）的分界不在"有没有语法分析"——cc-mode 的 `cc-engine` 本质是回溯扫描式的隐式 parser，单行缩进准确度可以很高。分界在于结构是**每次隐式重扫**还是**显式建树**：

* 结构编辑（`{|}` 展开等）需要"匹配的那对括号"作为可查询对象，扫描式方案没有稳定的结构身份，只能靠按键时机触发（electric 系）；
* 不完整代码在树上是统一的 incomplete/missing 机制，扫描式方案靠逐案补丁；
* 树查询是 O(depth)，回溯扫描是 O(到锚点距离)，且宏和 raw string 会破坏锚点猜测；
* trace、golden fixture、prefix 确定性测试都建立在显式结构上。

因此本内核的价值主张不是"算得比 cc-mode 准"，而是：**同等单行准确度之上，行为从持久结构派生**，使 smart enter、结构选择、可解释性和有界错误恢复成为自然延伸。

参考架构：

* IntelliJ：`Document` 是可编辑纯文本，PSI 是派生结构层；formatter 使用独立 block model，Enter 缩进由 `getChildAttributes()` + `isIncomplete()` 决定；Enter 按键分发到有序 handler 链。
* Roslyn：full-fidelity 不可变语法树，零长度 missing token 表达不完整结构，读者无锁共享旧快照。
* rust-analyzer / rowan：lossless 容错语法树的开源生产级实现，green/red 切分与长度编码的直接对照。
* clang-format：见 §3——它是"无 AST 的 token 级分析足以覆盖 C++ 格式化"的存在性证明。

---

## 2. 非目标

1. 完整、符合标准的 C++ 编译器前端；
2. 类型推导、名字解析或模板实例化（语义层外挂 clangd，见 [06-completion.md](06-completion.md)）；
3. 精确的宏展开 AST；
4. 整文件格式化；
5. 自动换行、空格和 brace placement；
6. Smart Enter / Complete Current Statement（补全分号、大括号）；
7. 括号自动配对输入、注释续行前缀（语言机制层职责，见 [../docs/scripting.md](../docs/scripting.md) 的 language profile）；
8. 屏幕光标、选择区或渲染（Scene/Presenter 层职责，§17）；
9. 多线程编辑；
10. 持久化工程索引；
11. `AlignConsecutive*` 类整文件对齐重排——格式化器的批处理功能，输入时缩进用不到；T3 的其余部分在范围内（§3）。

"语义"指**源代码结构语义**（当前处于 namespace body、是 case label 的后续语句、是参数列表 continuation、当前 block 未完成），不要求知道任何 identifier 的类型。

---

## 3. 缩进规则分层与范围

以 clang-format 的缩进相关选项为全集，按所需 parser 能力分层：

| 层级 | 需要的能力 | 代表选项 |
|---|---|---|
| T0 行分类 | 行首 token + `#if` 栈深 | `IndentPPDirectives`、`IndentGotoLabels` |
| T1 括号配对 + 构造归属 | 括号嵌套 + "这个 `{` 属于什么" | `IndentWidth`、`NamespaceIndentation`、`IndentCaseLabels`、`IndentCaseBlocks`、`IndentAccessModifiers`、`IndentExternBlock`、`BracedInitializerIndentWidth` |
| T2 声明/语句结构 | 声明边界、declarator、ctor list、lambda | `ConstructorInitializerIndentWidth`、`BreakConstructorInitializers`、`LambdaBodyIndentation`、`IndentWrappedFunctionNames`、`IndentRequiresClause` |
| T2.5 括号内对齐 | 开括号列 + closing 独行判定（仍是 token 级） | `AlignAfterOpenBracket`、`ContinuationIndentWidth` |
| T3 表达式内部 | 运算符优先级链 | `AlignOperands`、三元 `?:` 对齐、链式调用对齐 |

**本内核的范围是 T0–T3。** T3 以只算运算符优先级、不建真 AST 的 mini expression parser 实现（§10.2 表达式续行引擎），不是完整表达式前端。唯一排除的是 `AlignConsecutive*` 类整文件对齐重排（§2）。

架构佐证：clang-format 本身没有 C++ AST，其实现是 `UnwrappedLineParser`（token 级行拆分与构造识别）+ `TokenAnnotator`（局部上下文注 token 角色）+ `ContinuationIndenter`，仅为 `AlignOperands` 配一个只算优先级的 `ExpressionParser`。本内核与它属同一路线，区别是把结构组织为带 revision 的持久容错树。

---

## 4. 核心架构

```text
Document
    ↓
Lossless Lexer（分块 TokenBuffer）
    ↓
Error-tolerant CST（green tree + lazy red）
    ↓
Formatting Model（caret 路径上的隐式 block）
    ↓
Indentation Service
```

外围两个入口：

```text
Command Engine
    ├── insert / replace / undo
    ├── enter（handler pipeline，§11）
    └── indent-line / on-typing reindent

CLI Test Harness（§13）
```

设计原则：

> Document 是唯一权威数据；其余结构都是带 revision 的派生数据。行为从当前结构派生，不从编辑历史派生。

---

## 5. Document 模型

### 5.1 文本与位置

内部统一 UTF-8，换行统一 `\n`（载入时规范化 `\r\n`/`\r`）。存储是值语义的持久化 chunked B+-tree `Text`（[02-buffer.md](02-buffer.md)）：快照 = 值拷贝 = 引用计数 +1，O(1)；行索引与 UTF-16 换算是树内 summary 查询。

位置统一为 UTF-8 byte offset，全系统 `std::uint32_t`（文档上限 4 GiB）：

```cpp
struct TextOffset { std::uint32_t value; };
struct TextRange { TextOffset start, end; };   // half-open
struct VersionedRange { RevisionId revision; TextRange range; };
```

行列转换由 `LineIndex` 提供（Text summary 的薄封装）；缩进中的"列"按 tab policy 转换后的逻辑显示列定义，不涉及 grapheme 和屏幕列。

### 5.2 Revision 与 Snapshot

每次成功提交 transaction，revision 加一：

```cpp
class DocumentSnapshot {
    DocumentId document_id() const;
    RevisionId revision() const;
    std::string_view text(TextRange) const;
    const LineIndex& lines() const;
};
```

Snapshot 不可变、O(1) 获取，在后续编辑后仍然有效。

### 5.3 Transaction

所有修改发生在 transaction 中：

```cpp
class EditTransaction {
    void replace(TextRange, std::string_view);
    void insert(TextOffset, std::string_view);
    void erase(TextRange);

    TextOffset anchor_offset(AnchorId) const;   // pending 状态下实时结算
    DocumentSnapshot speculative_snapshot() const;

    CommitResult commit();
    void abort();
};
```

编辑坐标是**当前 pending 坐标**（每次编辑看到的是被此前编辑修改后的文本）。transaction 内部维护规范化 edit list：**旧 revision 坐标、按 start 升序、互不重叠**，与 pending 区间重叠或相接的既有编辑合并为一条。一次 Enter 命令是一个 transaction，undo 一次即可整体撤销。

```cpp
struct TextEdit { TextRange old_range; std::string new_text; };
struct DocumentChange {
    RevisionId old_revision, new_revision;
    std::vector<TextEdit> edits;
    TextRange affected_old_range, affected_new_range;
};
```

edit list 是唯一的语义差异载体：anchor 调整、增量 reparse、LSP didChange 的输入都是它；快照是值，edit list 是变化的解释，两者并存不冗余。

### 5.4 Undo

* 粒度是 transaction，一个 transaction 一个 undo 单元；
* undo/redo **产生新 revision**，其内容等于历史文本；revision 单调递增，派生数据的 revision 匹配语义无歧义；
* undo 是**快照值的树**：分支历史保留（undo 后继续编辑不丢弃旧分支），`undo_to` 走 LCA 路径重放。结构与代价见 [02-buffer.md §5](02-buffer.md)。

### 5.5 Anchor

```cpp
enum class AnchorAffinity { BeforeInsertion, AfterInsertion };
```

Anchor 表示 caret、selection、diagnostics 和长期语义区域。更新只由 transaction kernel 负责，规则：编辑之前的 anchor 不动，之后的按 delta 平移，落在被替换区间内的结算到替换文本之后（reindent 后 caret 自然落在新缩进末尾）；affinity 只在插入点恰好等于 anchor 时生效。

**Pending 语义**：anchor 在未提交的 transaction 内随每次 pending edit 实时结算。这是 Enter pipeline 的前提——一次 `{|}` 展开包含四次插入，caret 必须全程正确，最终落点由编辑计划显式声明（§11.3）。

---

## 6. 同步模型

Document 与语法层不共享可变状态。语法分析是**对任意 snapshot 的纯函数**：

```cpp
SyntaxSnapshot analyze(const DocumentSnapshot&);   // 幂等，可按 (document, revision) 缓存
```

speculative snapshot 同样可以传入；其结果不进入跨 revision 缓存，abort 后自然丢弃。

基本不变量：

```text
syntax.revision == document_snapshot.revision
```

缩进服务不得把 revision N 的语法树用于 revision N+1 的文本。

执行模型：编辑路径是**同步增量**——edit → 增量 lex（TokenBuffer splice，§7）→ 增量 reparse（green splice，§8.1）→ 沿 caret 路径计算 block → 查询缩进。全量解析只在文件加载等冷路径运行，同时作为增量的 oracle（§14 增量等价）。实测（52 万行 / 470 万 token，Release）：0.49ms/字符、0.94ms/Enter，相对全量重建约 20×。

未来引入后台解析时，所有结果携带 revision，过期结果只能丢弃或显式 rebase。

---

## 7. Lossless Lexer 与 token 存储

Lexer 保留源文件全部内容：空白、换行、注释、literal、raw string、preprocessor 行、escaped newline、无效字节。Token 只引用原始 Document range。

```cpp
struct Token { TokenKind kind; TextRange range; LexicalFlags flags; };
```

* `TokenKind` 只区分缩进相关的 keyword（namespace/class/switch/case/public/if/template/operator 等）与结构 punctuator（`{}()[]<>`、`:`、`::`、`,`、`;`、`->`、`=`）；其余 operator 拼写全部枚举化为独立 TokenKind（lexer 一次判定，parser 与 T3 引擎做 kind 比较而非文本比较——chunked 存储下跨 chunk 取文本需小拷贝，热路径不取文本）；未单列的按最长匹配合并，其余 keyword 归 `Identifier` 由 parser 按需判别。
* flags：`PreprocessorLine`（token 属于 pp 指令行，含反斜杠续行）、`Unterminated`（literal/块注释缺终结符，在行尾或 EOF 截断以限制损害）、`EscapedNewline`。
* 任意字节输入必须终止：无效字节成为 `Invalid` token，token 序列精确铺满全文并以零长度 `EndOfFile` 结尾。

**分块 TokenBuffer（token 层的长度编码）**：token 存储为分块结构，块内 range 相对块 base（约 512 token/块），对外物化绝对坐标 Token——`operator[]`/`size`/迭代器同形 API，proxy iterator 兼容 ranges 算法与二分。编辑的后缀字节平移 = 后缀块 base += delta（O(块数)）；中段 splice 只重打包损伤窗口与块表，不移动整条扁平数组。全量解析热路径（lexer 产出、parser 构建读取）仍走扁平 vector，`run()` 末尾一次性分块入树——冷路径 O(n) 不变、增量路径次线性。

两个配套索引消除每键 O(n) 扫描：pp 安全检查走树上维护的**条件指令索引 `pp_dirs_`**（全量解析时分类构建，splice 时前缀保留/损伤窗口重收集/后缀平移，phantom 语义由 `pp_phantom_hi_` 兜底，§8.3）；EnterBetweenBraces 的谓词按 offset 二分 + 局部回走。

跨行词法状态显式建模并逐行记录，**增量 lexing 的收敛判据是"新旧 token 与出向 state 相同即停止传播"**：

```cpp
struct LexerState {
    bool inside_block_comment;
    bool inside_raw_string;
    bool preprocessor_continuation;
    std::string raw_delimiter;
};
```

---

## 8. 容错 CST

### 8.1 树表示：green tree

语法树是**长度编码的 green tree，也是唯一的 source of truth**（Roslyn / rowan 路线）：

* **GreenNode 只存 kind、长度与 children，不存绝对坐标**；绝对坐标由根到节点路径上的前缀和导出。编辑因此只更新到编辑点的 spine，未触及子树零偏移修正地整体复用。
* **增量 reparse 在 green 上做 splice**：损伤窗口外的兄弟子树按指针复用，只重建 spine。复用条件 = extent 未被编辑触及 + 祖先上下文一致。骨架 parser 的非局部上下文可枚举——逐行 LexerState（收敛判据）、`enum_body_`/linkage 等可从祖先推导的状态、声明内部状态在声明粒度复用下天然重置——正确性论证远比 lookahead 记账简单，且两侧都是自有代码。
* **red 节点按需 lazy 物化**进 deque 池，提供绝对 token 区间、parent、children 的遍历视图；消费方（缩进、命令、结构查询）看到的 API 与物化树同形，不感知 green/red 切分：

```cpp
// red 视图（lazy 物化）
struct SyntaxNode {
    SyntaxKind kind;
    std::uint32_t first_token, end_token;   // half-open，绝对坐标
    SyntaxNodeId parent;
    std::vector<SyntaxNodeId> children;
    bool incomplete;
    TokenKind expected;                      // MissingToken 节点专用
};
```

* trivia 隐式归属：区间内未被 child 覆盖的 token 属于节点自身；leading/trailing trivia 归父节点；每个 token 恰好属于一个最深节点。
* 接口不承诺节点身份跨 revision 稳定；增量/全量等价的 oracle 是 **green_equal**（id-free 的结构比较，§14）。
* 公共 API 命名显式标注成本：惰性迭代、O(1) 缓存值与显式复制分开命名，不做隐藏遍历的 getter。

### 8.2 Parser 策略

手写 recursive descent，规则：

* 所有 parser function 要么消费 token，要么显式产生 error node；每个解析循环带**前进保证**（无进展则吞一个 token 进 Error 节点），任意输入必然终止；
* **不完整结构用零长度 MissingToken 表示预期 token**（Roslyn 式 full fidelity）：`foo(` 是 CallExpression + missing `)`，parser 不通过"修正输入"获得漂亮的树；formatter 由 missing token 直接推导 incomplete；
* 结构判定用**局部 token 模式**而非文法：FunctionDefinition = "`)` 加 declarator 后缀后跟 `{`，或同样前提下跟 `:` 且前方无 `?`（ctor initializer）"。declarator 后缀 = 标识符（`const`/`noexcept`/`override`）、`::`、`->`、`&`/`&&`/`*`、`noexcept(...)` 括号组与模板实参的任意序列，覆盖 cv 限定、引用限定与尾置返回类型；lambda body = 括号组内 "`)`/`]` + 同样后缀" 后跟的 `{`（实参位置的尾置返回 lambda 是 continuation 逻辑最易崩坏处）。判错的代价是角色偏差，不是崩溃；
* 语句边界的词法启发：全大写标识符紧跟 `(...)` 且其后换行，视为声明位置的宏调用并就地终止（`LLVM_YAML_IS_SEQUENCE_VECTOR(...)` 不把后续声明吞成自己的续行）；enum body 内 `,` 终止当前声明（枚举项是 sibling 不是续行，末项以 `}` 结束视为完整）；单个标识符后跟 `:` 是完整的 goto label，下一语句是 sibling；`= default` 中的 `default` 不触发 case 同步；
* brace 的 init/block 二义按 clang-format `calculateBraceTypes` 的证据规则裁决：brace group 内出现语句证据（`;`、`return`、`if`/`for`/`while`/`do`/`switch`）就地重分类为 CompoundStatement 并继续按语句解析（`LLVM_DEBUG({...})`、无参 capture-only lambda 的兜底）；preprocessor 行在 brace group 内是**中性**的，整行消费后继续（`{a,\n#ifdef X\n b,\n#endif\n c}` 仍是一个初始化列表）；其余 sync introducer 仍旧止损退出。lambda introducer 按 `tryToParseLambdaIntroducer` 的前导判定：`[` 前是语句起点/`=`/`,`/`return` 时，capture list 之后允许 `{` 直接作为 body；
* 表达式不解析：fallback 是 opaque 声明/语句，吞 token 至 `;`，途中只对 `( [ { <` 建组。这是 §3 范围划分的实现面——C++ parser 的主要难度（声明/表达式歧义、most vexing parse）因缩进不需要区分而整体消失；
* 错误恢复的同步点：顶层分号、brace 边界、preprocessor directive、namespace/class/function introducer、case/default label、access specifier。未知结构在这些位置重新同步，损害有界。

解析范围（T0–T2.5 所需全部结构）：translation unit、namespace、`extern "C"` linkage block（namespace 语义，复用 NamespaceDecl/NamespaceBody）、class/struct/enum/union 与 access specifier、function（含 ctor initializer list）、compound statement、if/for/while/do/switch 与 case section、无括号单语句体、lambda、parameter/argument list、braced init、goto label、preprocessor directive（含指令体内的 group 结构）、宏调用 fallback。

### 8.3 三个 C++ 硬问题的降级策略

**模板尖括号**：仅在声明性上下文尝试配对 `<...>`；扫描遇到 `;`、`{`、`}`、不平衡 `)` 或 token 数超限即放弃，`<` 降级为普通运算符。配对失败不产生 error node，只影响该表达式内部精度。

**`#if/#else/#endif` 跨分支括号平衡（snapshot-restore 模型）**：preprocessor directive 仍作为行级元素挂在树上，但**条件分支按 alternative 语义**建括号嵌套——clang-format"每趟只有一个分支 active"的 brace 模型：一个分支的净 brace 效果计数，而非各分支相加。机制：容器 item-loop 层消费的 `#if/#ifdef/#ifndef` 压一帧记录 owner 的 `stack_` 深度；其后的 `#else/#elif` 通过 cooperative unwind（`unwind_target_`）把递归下降回退到该深度，使替代分支成为**兄弟节点**；`#endif` 弹帧。`#ifdef _WIN32\n if(){ #else if(){ #endif ... }` 由此不把 if2 嵌进 if1、不向函数尾级联。**无 `#else` 的孤立 `#if/#endif` 永不 restore**——include guard 与 `#ifdef __cplusplus` / `extern "C" {` / `#endif` … 尾部对称 `}` 的经典框架保持全 active 括号配对。分类（Open/Alt/Close/Other）集中在 `syntax/pp_conditional.hpp`，parser 与 IndentPPDirectives 深度计算共用。**与增量 reparse 的纠缠**：块修复复用损伤窗口外的兄弟子树，而 `#else` 的 unwind 依赖任意远处 `#if` 处的 brace 深度（非局部），brace 编辑会令复用的前缀/后缀 frame 上下文失效——故 reparse 在 (a) 窗口起点位于未闭合条件内，或 (b) 窗口处及其后存在 `#else/#elif` 时回退整文件重解析（`pp_repair_unsafe`）；无可达 alternative 分支的文件走增量快路径，与 flat baseline 完全一致；条件指令本身的增删改由 `splice_touches_pp_conditional` 触发整文件重解析。指令体内部的 `( [ {` 建为 group 子节点（多行宏体获得正常的续行/对齐锚点），但 group 解析不越出指令边界——`#define LPAREN (` 的不平衡困在行内。

**跨分支闭括号（phantom scope 重开，snapshot-restore 的对称另一半）**：上一段处理"分支各**开**多余 `{`"（unwind 回收）；`do { … #ifdef } while(A); #else } while(B); #endif` 是对称情形——各分支**闭**同一个 conditional 之外打开的 `{`，只应计一次。机制：`PPFrame` 额外记录 `#if` 时刻的 **brace-scope 深度**（`stack_` 中 CompoundStatement/NamespaceBody/ClassBody/PPReopenedScope 计数；statement wrapper 不消费 `}` 不计）；owner loop 消费 `#else/#elif` 后若出现亏欠（分支净关了 N 个外层 scope），按 N **嵌套打开 phantom 容器 `PPReopenedScope`**——替代分支的 `}` 由内向外逐层关闭它们，恰好镜像原 scope 的关闭顺序；phantom 的 item loop 同时停在本条件的 `#elif/#else/#endif`（不足的 `}` 补 MissingToken），`#endif` 交回外层循环弹帧，下一个 `#elif` 分支重新开 phantom（每分支独立从快照出发）。owner 消费 Alt 时就地清 `unwind_target_`（unwind 的使命就是把控制带回 owner；否则残留 target 会误伤 phantom 更深的循环）。**embedded body 的条件指令**：`if(A) #else`（body 在本分支缺失）→ mark_incomplete 交回 item loop；`if(B) #endif stmt;`（跨分支 if 头、正文在公共后缀）→ `#endif` **透明**（弹帧+消费后继续找真正 body）——两个决策只依赖 token 流、不依赖 pp_frames_（sandbox 与全解析不可分歧）。**phantom 有界性**：`pp_open_item`/embedded 在 `pp_frames_.size() <= 最内 phantom 的 frame floor` 时不消费 Close（交还 phantom loop 停止），防 phantom 泄漏过 `#endif`；但 paren/brace 组扫描仍可吞指令使 phantom 越界——因此 parser 记录 `pp_phantom_hi_`（所有 phantom 的实测最远 token）存入树，reparse 在 `guard_lo < pp_phantom_hi_` 时无条件整文件重解析（parser 实测边界优先于 token 扫描的猜测；这是对 `pp_repair_unsafe` 的补充而非替代）。**缩进侧**：generic 的 relevant 上行跨过"起始于本行"的 PPReopenedScope 时每层 +indent_width（分支首行拿到快照 body 级别）；phantom 起始于更早行时其内部行与首行平齐、行首 `}`（phantom 的闭括号）回退一级；零内部行的常见形态（`} while`）则因 phantom 起始于 `}` 行本身而自然落到父容器的 item 级别——三条路径覆盖 `}` 行、内部行、`#endif` 后缀行。相关规则：`parse_paren_group` 的 class/struct bail 只在**行首**生效——行中的 `struct` 是类型引用（如 `(struct sockaddr *)&Addr`），不弃械。该语料（raw_socket_stream.cpp）逐行精度 100%；约 28 万次 PP-heavy 随机编辑差分 fuzz（增量 == 全解析，green_equal）全绿。

**lambda**：在解析范围内。`[` 在表达式上下文优先尝试 lambda introducer，失败回退为下标/attribute。lambda body 嵌在 argument list 中是 continuation 逻辑最易崩坏处，必须有 body block。

### 8.4 宏与预处理

主语法树锚定用户实际编辑的 spelled source，保留 `#define`、`#if` 各分支、宏调用与原始 trivia，不以预处理后 token 流为输入。未知宏按局部上下文形成 opaque node（declaration prefix / statement / expression 等），不要求展开：

```cpp
MY_API
Foo::Foo() : value_(0) { }
```

`MY_API` 作为 declaration prefix 保留，不阻止后续函数与 initializer list 识别。宏定义信息（compile database）将来只能作为增强，不是缩进计算的前提。

### 8.5 结构视图

编辑器功能消费 CST 的方式是绑定于不可变 SyntaxSnapshot 的只读 typed 查询（node_at、按层二分、thing/motion 的 cst-node 求值），不是另一棵复制的树；所有修改通过 Document edit。CST 是编辑器功能共享的结构词汇，不是文本权威，也不是语义数据库。

---

## 9. Formatting Model

### 9.1 隐式 block

Formatting model 不物化为独立的树：block 沿 **caret 路径**从 CST 即时计算，每个 block 由三元组表达——`FormatRole`（这一行是什么槽位）、缩进贡献、可选 anchor。格式语义不进入语法树；同一语法节点可以映射多个角色（switch 的 CompoundStatement 因子节点是否 CaseSection 而贡献不同），多个节点也可合并（FunctionDefinition 与其 pending initializer list）。显式 `FormatBlock` 对象化留待需要更复杂对齐/换行规则时进行。

```cpp
enum class FormatRole {
    File,
    NamespaceBody, TypeBody, AccessSpecifierLabel,
    FunctionBody, CompoundBody, LambdaBody,
    SingleStatementBody, ControlHeaderContinuation,
    CaseLabel, CaseBody,
    ConstructorInitializerIntro, ConstructorInitializerItem,
    ParenContinuation, BracketContinuation, TemplateArgsContinuation,
    BraceInit, StatementContinuation,
    PreprocessorDirective, ClosingToken,
    PreservedRawString, PreservedBlockComment,
    Opaque,
};
```

### 9.2 两个对称查询

每个 block 回答两个问题：

1. caret 位于该 block 的某个 child slot 时，新 child 应获得什么缩进（child attributes）；
2. 该 block 的 closing token（`}`、`)`、`]`）自身应获得什么缩进。

后者服务三个场景：`{|}` 展开的 `}` 行、`}` 输入后的 reindent、显式 indent 落在 closing token 行。

### 9.3 Style

```cpp
struct CppIndentStyle {
    int indent_width = 4;
    int continuation_indent = 4;
    int tab_width = 4;
    bool use_tabs = false;

    // T2.5：开括号后同行有内容时，折行对齐该内容的实际列
    // （clang-format AlignAfterOpenBracket / CLion "align when multiline"）。
    bool align_open_bracket = true;
    // braced list 内容用 continuation_indent 而非 indent_width
    // （clang-format Cpp11BracedListStyle）。
    bool brace_init_continuation = false;
    // false：折行的函数声明名行保持声明缩进不加 continuation
    // （clang-format IndentWrappedFunctionNames；LLVM 返回类型断行后函数名顶格）。
    bool indent_wrapped_function_names = true;

    // None: namespace 体不缩进；Inner: 仅嵌套 namespace 的体缩进；All: 全缩进
    //（clang-format NamespaceIndentation 三值一一对应）。
    enum class NamespaceIndentation { None, Inner, All };
    NamespaceIndentation namespace_indentation = NamespaceIndentation::None;
    bool indent_type_body = true;
    bool indent_case_label = false;
    bool indent_case_body = true;
    int access_specifier_offset = 0;    // 相对 type 声明行；0 = 与 class 对齐

    enum class ConstructorInitializerStyle {
        NormalIndent, ContinuationIndent, AlignFirstInitializer, AlignAfterColon,
    };
    ConstructorInitializerStyle constructor_initializers =
        ConstructorInitializerStyle::AlignFirstInitializer;

    // T3（§10.2）：AlignOperands 与 BreakBeforeTernaryOperators。
    bool align_operands = true;
    bool break_before_ternary = true;
};
```

### 9.4 缩进物化

`target_column` 到缩进文本的转换唯一确定：

* `use_tabs = false`：全空格；
* `use_tabs = true`：smart tabs——结构层级部分用 tab 填充，对齐超出层级的剩余列一律空格。对齐列永不依赖 tab 宽度的可视化设置。

`target_column` 与物化文本的一致性是不变量（§14）。

### 9.5 clang-format 配置读取

CLion 的 ClangFormat 模式：读取 `.clang-format`，把其中**缩进相关**的键映射进 `CppIndentStyle`，其余键忽略（换行决策、对齐美化、include 排序是格式化器的职责，不进缩进内核）。读取器分两层：

* **纯函数层**（formatting，无文件系统）：`parse_clang_format_yaml(text, base) → {style, inherit_parent, warnings}`。YAML 子集解析：顶层 `key: value` 标量、`#` 注释、`---` 多文档（`Language` 缺省或为 `Cpp` 的文档按序生效，其余整段跳过）、嵌套块（有前导空白的行）跳过。每个文档内 `BasedOnStyle` 先于其他键应用（与 clang-format 的读取顺序一致），preset 表内置七个：LLVM/Google/Chromium/Mozilla/WebKit/GNU/Microsoft，名字大小写不敏感；`BasedOnStyle: InheritParentConfig` 置 `inherit_parent` 交由发现层解析。
* **文件发现层**（cli）：从文件所在目录向上找 `.clang-format`/`_clang-format`（先到先得）；`inherit_parent` 时继续向上解析父配置作为 base 重新应用本文件。base 是 **LLVM preset**——clang-format 的 `getStyle` 对配置文件以 fallback（默认 LLVM）起算，无 `BasedOnStyle` 的文件意为"LLVM 加这几项"，不是工具自身默认值。`DisableFormat: true` 作为标志上浮：这类目录（vendored 代码，如 LLVM 树里的 BLAKE3）明确不在格式化契约内，bench 跳过并计数，repl 加载时提示。

映射表（枚举值的新旧拼写同时接受，与 clang-format 自身的向后兼容名单一致）：

| .clang-format | CppIndentStyle | 换算 |
|---|---|---|
| IndentWidth / ContinuationIndentWidth / TabWidth | indent_width / continuation_indent / tab_width | 直接 |
| UseTab | use_tabs | Never/false → false；ForIndentation/ForContinuationAndIndentation/AlignWithSpaces/Always/true → true |
| AccessModifierOffset | access_specifier_offset | **语义换算**：clang-format 的 offset 相对成员缩进，我们相对类型声明行，`ours = IndentWidth + offset`（LLVM 2+(-2)=0）。offset 以相对值贯穿解析（base 反推 `raw = base.access − base.indent_width`），最终 IndentWidth 定值后才物化——`BasedOnStyle: LLVM` + `IndentWidth: 4` 得 2，与 clang-format 一致 |
| NamespaceIndentation | namespace_indentation | None/Inner/All 一一对应 |
| IndentCaseLabels | indent_case_label | 直接 |
| Cpp11BracedListStyle | brace_init_continuation | Block/false → false；AlignFirstComment/FunctionCall/true → true |
| AlignAfterOpenBracket | align_open_bracket | Align/true → true；DontAlign/false/AlwaysBreak/BlockIndent → false（后两者断行后走 continuation，不对齐） |
| IndentWrappedFunctionNames | indent_wrapped_function_names | 直接 |
| ConstructorInitializerIndentWidth | constructor_initializers | 七个 preset 中该值恒等于 ContinuationIndentWidth，此时保持 AlignFirstInitializer（clang-format 的折行 initializer 同样对齐首项）；不相等时告警不改值 |
| AlignOperands / BreakBeforeTernaryOperators | align_operands / break_before_ternary | T3（§10.2）；Align/DontAlign 与布尔直接映射 |

缩进相关但暂不支持的组合产生 warning 而非静默忽略：`IndentAccessModifiers: true`、`LambdaBodyIndentation: OuterScope`、`IndentExternBlock: Indent`（extern 块复用 namespace 规则）、`IndentCaseBlocks: true`。未知 `BasedOnStyle` 告警且不动 base。preset 表的正确性以 `clang-format --dump-config` 交叉验证（测试在工具缺失时跳过）——dump 输出本身就是一份全键 YAML，同时充当解析器的真实输入回归。

---

## 10. Indentation Service

### 10.1 查询

查询是纯函数：不修改 Document，revision 不匹配即拒绝。节点定位按层二分（children 按源序排列），单次查询 O(depth · log children)——长兄弟列表（大 translation unit、巨型函数体）不退化为线性扫描。

```cpp
IndentDecision compute_line_indent(const DocumentSnapshot&, const SyntaxTree&,
                                   std::uint32_t line, const CppIndentStyle&);

struct IndentDecision {
    int target_column;
    std::string indentation_text;
    FormatRole role;
    std::optional<TextOffset> anchor;
    bool preserve;                      // 该行不得改动
    std::vector<std::string> trace;
};
```

### 10.2 算法

四步：

1. **保护检查**：行首落在 raw string 内部——修改前导空白会改变程序语义，强制 preserve；块注释内部同理（用户不期望被重排）。对所有触发方式生效。
2. **行首分类**（这一行是什么）：closing token 对齐到 owner 锚定行（见步骤 3）的实际缩进；`#` 按 `IndentPPDirectives` 处理（缺省列 0）；`case`/`default` 取最近 SwitchStatement 的行缩进 + `indent_case_label`；access specifier 取最近 ClassDecl 的行缩进 + offset；行首 `:` 属于 CtorInitializerList 时按 intro 规则；`else`/do-while 的 `while` 与其配对语句对齐；**纯注释行按原始列归属**（clang-format `distributeComments` 的对齐尾段规则）：向下找第一个以显著 token 开头的行，注释当前列与该行的目标列相等时采纳其决策（与下一 case label 对齐的注释拿 label 缩进），否则走通用路径保持所在结构的缩进（case body 内未对齐的注释、块尾注释保持 body 缩进）。
3. **通用路径**（这一行在什么里面）：
   * 有内容的行取行首 token 所在最深节点；空白/新行取 **caret 前最后一个非 trivia 块**，该块 complete 且已结束则逐级上浮到 parent（IntelliJ `isIncomplete`/`getChildAttributes` 语义）；
   * 沿祖先链找到第一个**起始行早于当前行**的节点作为 controlling block；
   * **锚定行**：brace 体（CompoundStatement/ClassBody/NamespaceBody/BraceGroup）锚定到拥有该 brace 的构造（函数/控制语句/声明）的起始行——折行签名或折行条件把 `{` 带到续行时，body 与 `}` 不随之偏移。只上爬一步：外层语句以本语句为 body 时不再继续（braceless `for` 里的 `if {}` 锚定 `if` 行而非 `for` 行）。其余节点锚定自身起始行；
   * `target = 锚定行的实际缩进 + f(SyntaxKind, style)`。**相对实际缩进锚定**而非从根全量重算：局部错误缩进不级联，天然兼容用户手工调整。贡献表：NamespaceBody 按 `namespace_indentation`（Inner 仅当该体嵌套于另一 NamespaceBody 时缩进），ClassBody/CaseSection 按 style，CompoundStatement +indent_width，Paren/Bracket/TemplateArgs/BraceGroup 开括号后同行有内容且 `align_open_bracket` 时**对齐该内容的实际列**（T2.5），否则 +continuation（BraceGroup 按 `brace_init_continuation` 选 continuation 或 indent_width），控制语句（缺 compound body 时）+indent_width，CtorInitializerList 按四种风格（AlignFirstInitializer 对齐第一个 initializer 的实际列），FunctionDefinition 的 pending initializer list（列表已结束但 body 未开始）延续 item 规则，PreprocessorDirective 体（`\` 续行、组外部分）+indent_width（clang-format 的宏体排版），OpaqueDeclaration +continuation（`indent_wrapped_function_names = false` 时，声明作用域下以 declarator 名开头且参数列表同行开启的续行——clang-format `TT_FunctionDeclarationName` 的 token 级近似：声明起点到 `(` 之间全为名字/类型成分，无 `=`、`,`、表达式运算符——保持声明缩进），Error 保留上一非空行缩进。
4. **物化**（§9.4）并记录 trace。

Fallback 链保证始终返回结果：结构父节点 opening token → 同类 sibling → introducer token → parent 缩进 → 上一非空行 → 列 0。error node 不导致失败。

**表达式续行引擎（T3）**：controlling block 是表达式性上下文（Paren/Bracket/TemplateArgs/BraceGroup 续行、语句续行）时，通用路径第 3 步先尝试表达式模拟器，不适用（region 超 token 上限）时回退上述贡献表。移植自 clang-format 的 `TokenAnnotator::ExpressionParser` + `ContinuationIndenter`，但有一个本质简化：**clang-format 联合选择断点与列，我们的断点是文件/用户给定的事实，只重放列规则**——无 penalty、无优化器，单遍线性。

* **语句区域**：从 controlling block 上爬到 parent 为块级作用域（TranslationUnit/CompoundStatement/NamespaceBody/ClassBody/CaseSection）的节点，即 clang-format 的 unwrapped line 近似。区域内 CompoundStatement 子块（lambda body 等）整段跳过（clang-format 的 child lines）；括号配对直接取自 CST 的 group 节点，容错免费获得。
* **fake paren 标注**（ExpressionParser 移植）：按 C++ 优先级递归下降，为每个运算序列起点记 FakeLParens（优先级栈）、终点记 FakeRParens 计数；三目经 `parseConditionalExpr` 单独配对；一元运算符前缀按上文 token 判定。`.`/`->` 链取 PrecedenceArrowAndPeriod。
* **状态机重放**：ParenState 栈 {Indent, LastSpace, QuestionColumn, FirstLessLess, CallContinuation, VariablePos, NestedBlockIndent, StartOfFunctionCall, IsWrappedConditional}，全局 {FirstIndent, StartOfStringLiteral}。已排版 token 用**文件实际列**；换行 token 触发 addTokenOnNewLine 更新（LastSpace=实际列、`<<` 补 3、成员访问记 CallContinuation），同行 token 触发 addTokenOnCurrentLine 更新（逗号后/条件冒号后/非赋值二元运算后 LastSpace=列、if/for 开括号后 LastSpace=列、开括号对齐置 Indent=列、顶层 `=` 记 VariablePos）；每 token 依序执行 fake-LParen 压栈（Indent = max(列, Indent, LastSpace)，首个 fake paren 在 return/赋值/开括号后不加宽，Conditional 恒 +cont，其余 >Assignment 时 +cont）、scope 关闭弹栈、scope 开启压栈（block 跳过；braced init：LastSpace + braced_width；其余：max(LastSpace, StartOfFunctionCall) + cont）、fake-RParen 弹栈（保留 VariablePos）、字符串起点跟踪。
* **查询行取列**（getNewLineColumn 的 C++ 子集，按序）：字符串字面量接续 → StartOfStringLiteral；`<<` → FirstLessLess；`.`/`->` → CallContinuation；三目 `?`/`:`（QuestionColumn 已置且非链式）→ QuestionColumn，IsWrappedConditional 抑制链式 unindent 分支；逗号后 VariablePos；closing `)`+`;`/`{` → 外层 LastSpace；closing `}`/`]` of braced init → 外层 LastSpace；`=`/`::` 之后或 `)` 之后 → max(LastSpace, Indent)+cont；fallback：Indent==FirstIndent 时 Indent+cont，否则 Indent。
* style 字段 `align_operands`（控制 fake-paren 的 max(列) 对齐与三目 unindent）与 `break_before_ternary`（控制 QuestionColumn 记录时机）进 `.clang-format` 映射表（§9.5）。

### 10.3 显式 indent 与 on-typing reindent

显式 indent 只修改目标行的 `[行首, 首个非空白)` 区间。

On-typing reindent（`type_char`）与 Enter 对称：字符插入与 reindent 同一 transaction、同一 undo 单元；触发条件是结构谓词而非字符本身——`:` 在插入后该行分类为 case label、access specifier 或 ctor intro 时触发，`}` 与 `#` 在行首到 caret 只有空白时触发；谓词不满足（如字符串内、三目的 `:`）只插入字符不做任何修改，保护行（raw string / 块注释）永不触发。session 与 fixture 的 `type` 动作逐字符走这条管线，等价于编辑器逐键投递。这条路径解释了序列 fixture 的行为：`Foo::Foo()` 后 Enter 时 parser 尚不知道下一行以 `:` 开头，Enter 给 continuation 缩进，`:` 输入后由 reindent 修正。

---

## 11. Enter Handler Pipeline

### 11.1 原则

Enter 是一条**有序 handler 链**：每个 handler 是结构谓词 + 编辑计划，第一个命中者执行，整体一个 transaction、一个 undo 单元，链尾 fallback 是 newline-and-indent。

谓词只依赖动作时刻的 snapshot 与 CST（括号匹配由树判定，不是计数扫描）：光标从任何位置到达 `{|}` 之间行为一致；`#if` 不平衡或前方有 error node 时匹配自然失败，退到普通 Enter，不做破坏性展开。

### 11.2 Handler 链

```text
1. EnterBetweenBraces
   谓词: caret 两侧最近非 trivia token 是 '{' 与同一节点的匹配 '}'，
         之间只有单行 trivia（'}' 已独立成行时只需 fallback），
         且 caret 不在任何 token 内部（注释、literal）
   计划: caret 处插入 "\n\n"
         → speculative reparse
         → 中间行查询 child attributes，'}' 行查询 closing token indent
         → 插入两段前导空白
         → caret 落在中间行缩进之后

2. NewlineAndIndent（fallback）
   计划: caret 处插入 "\n" → speculative reparse → §10.2 → 插入前导空白
         → caret 落在缩进之后
   保护: 新行落在 raw string 内不插入任何缩进；块注释内延续上一行前导空白
```

### 11.3 编辑计划与 caret 契约

每个 handler 的编辑计划显式声明最终 caret；caret 落点不隐含等于"最后一次插入之后"。kernel 按 §5.5 的 pending 语义在多次插入间维护 anchor，commit 后结算。

---

## 12. 可解释性

解释能力是核心功能，不是调试附属品。每个决策携带 trace，第一行报告命中的 Enter handler。

```text
indent-core explain file.cpp --line 12

target-column: 8
role: ConstructorInitializerItem
anchor: line 10, column 6

trace:
  enter handler: NewlineAndIndent (fallback)
  OpaqueDeclaration before caret is complete; deferring to its parent
  controlling block: CtorInitializerList opened at line 10 (indent 4)
  style AlignFirstInitializer: aligned to first initializer at line 10 column 6
  role ConstructorInitializerItem -> column 6
```

没有 trace，宏和错误恢复问题无法排查；trace 也是比较两个 revision 为何产生不同结果的基础。

---

## 13. CLI Test Harness

```text
indent-core tokens <file>              token dump（kind/range/flags）
indent-core tree <file>                CST dump（含 incomplete 与 missing）
indent-core explain <file> --line N    缩进决策 + trace
indent-core apply-enter <file> --offset N
indent-core repl [file]                stdin command loop
indent-core test <fixture.yaml|dir>    序列 fixture runner
indent-core bench <file|dir>... [--style default|file|<preset>] [--show N] [--clean-only]
                                       真实语料逐行精度测量；--style=file 逐文件
                                       发现 .clang-format（目录级缓存），<preset>
                                       为 clang-format 内置风格名
indent-core edit <file>                终端编辑器（编辑器壳入口，属其他层，
                                       见 ../docs/command-loop.md）
```

### 13.1 repl

命令：`type <text>`、`enter`、`indent`、`caret <offset>`、`undo`、`redo`、`show`、`explain`、`tree`、`style <key> <value>`、`loadstyle <path>`（读取 `.clang-format` 文件或从目录向上发现，warning 逐条回显）。每次变更输出带 `^` caret 标记的全文；caret 历史与 undo 栈镜像，undo 同时恢复 caret。repl 会话可直接誊写为 fixture。

### 13.2 Fixture 格式

`^` 表示 caret（initial 必须有，expected 可选，出现则校验落点）：

```yaml
name: constructor initializer list typed step by step
style:
  indent_namespace_body: false
initial: |
  Foo::Foo()^
actions:
  - enter
  - type: ": a_(1),"
  - enter
  - type: "b_(2)"
expected: |
  Foo::Foo()
      : a_(1),
        b_(2)^
```

actions 支持 `enter`、`indent`、`undo`、`redo`、`type: "..."`。runner 对文件或目录（递归 `*.yaml`）执行并汇报，接入 ctest。

---

## 14. 测试策略

**Document 不变量**：transaction 原子；undo 后文本等于历史 snapshot；line index 等于全文重算；anchor 始终在合法 offset 且 pending 期间实时正确；一次 Enter 一个 undo 单元；规范化 edit list 回放等于最终文本（随机编辑 fuzz 对照模型验证）。

**Lexer 不变量**：token ranges 连续铺满全文、不重叠、拼接等于原文；任意字节输入终止（fuzz）。

**Parser 不变量**：任意输入终止；根节点覆盖全部 token；children 有序嵌套；无零长度死循环（前进保证 + fuzz）。

**增量等价**：`incremental_parse(edit(old_tree)) == full_parse(new_text)`，比较用 green_equal（id-free 结构比较）。随机编辑差分 fuzz 锁定，含 PP-heavy 语料（约 28 万次编辑全绿）；`pp_repair_unsafe` / `pp_phantom_hi_` 等回退护栏的正确性也由这条不变量覆盖。

**缩进不变量**：结果确定；显式 indent 只改 leading whitespace；raw string / 块注释行不产生编辑；`target_column` 与物化文本一致（§9.4）；过期 revision 拒绝；整文件错误文本也返回列；局部错误扫描范围有界。

**Prefix typing**：对样例文件的每个前缀执行 Enter，验证 incomplete 结构、错误恢复、缩进不跳变、closing token 出现前后行为合理。同时记录 **structural churn**：每输入一个字符后 caret ancestor path 改变的层数——错误恢复的目标是编辑稳定性，逐字符输入 `Foo::Foo() : a_(1),` 应尽早收敛到 `FunctionDefinition + IncompleteInitializerList` 并保持，不在 label/expression/error 间翻转。对样例给 churn 上界并入 golden test。

**golden 语义样例**必须覆盖：namespace（含不缩进配置）、class + access specifier、switch/case/default、ctor initializer（完整、bare `:`、逐字符序列、四种风格）、`MY_API` 前缀宏、无括号 if 及 else 链、lambda（变量初始化与实参内、尾置返回类型）、cv/noexcept 限定与尾置返回的函数体、`= default`、无分号宏调用边界、折行签名的 body/`}` 锚定、开括号对齐、braced list 两种缩进宽度、注释行对齐、`{^}` 展开、未闭合宏调用远处结构稳定、`#if` 跨分支括号不平衡（snapshot-restore 与 phantom 重开两个方向）。

**真实语料精度**：`bench` 对真实代码库逐行比较 `target_column` 与实际缩进，按 FormatRole 聚类差异。`--clean-only` 以 clang-format 本身为 oracle，剔除输出与原文不一致的文件——遗留格式是噪声不是信号，歧义规则（注释归属等）的 ground truth 也以 clang-format 实际输出为准，不靠猜测。**真实精度数以 `--style file --clean-only` 计**：当前 LLVM 语料 Support 99.93% / Sema 99.38% / IR 99.75%。过滤后的不可归因差异视为缺陷。

---

## 15. 目录结构与依赖方向

内核层目录：

```text
src/
  document/      document, snapshot, transaction, line_index, anchor, text
  cpp_lexer/     token, lexer_state, lexer, token_buffer
  syntax/        syntax_kind, green tree, parser, pp_conditional
  formatting/    format_role, cpp_indent_style, clang-format 读取
  indentation/   indentation_service
  commands/      editor_commands（enter pipeline, indent-line, file io）
  cli/           main, session, repl, fixture_runner, bench
```

依赖单向：document ← cpp_lexer ← syntax ← formatting ← indentation ← commands/cli。Document 不引用 syntax；syntax 不引用 formatting。

`src/` 其余目录（editor、script、lsp、project、async、ui、tui、gui、presentation）属编辑器壳与服务层，见 [../docs/](../docs/) 各实现文档；它们单向依赖内核层，内核层不反向引用。

---

## 16. 关键设计决策

1. **Document 是权威，语法树是派生缓存**。不允许绕过 transaction 修改任何派生结构。
2. **保真容错 CST，而不是编译器 AST**。空白、注释、宏和错误文本保留在原始坐标空间。
3. **Formatting model 独立于语法树**。格式语义不污染语法模型。
4. **不完整代码是一等状态**。incomplete 与 missing token 是统一机制，不是 error 的别名。
5. **缩进查询同步执行**。不依赖 clangd、编译器或网络服务。
6. **宏采用源码优先的容错模型**。宏展开信息只能增强，不是前提。
7. **内核无 UI**。正确性通过 CLI、dump、trace 和自动测试验证。
8. **增量与全量语义等价是不变量**。green tree 增量 reparse 是每键热路径，全量解析是冷路径与 oracle，green_equal fuzz 锁定等价；接口自始 revision 化。
9. **行为从结构派生，不从按键历史派生**。Enter handler 谓词只查询动作时刻的 CST。
10. **表达式内部缩进（T3）用 mini expression parser，不建完整表达式前端**。fake-paren 优先级标注 + ContinuationIndenter 状态机重放（§10.2）；`AlignConsecutive*` 排除。

---

## 17. 渲染架构：Scene / Presenter 切分

编辑器壳的渲染按 `compose() → ui::Scene → presenter` 切分，为 remote（key 上行 / 帧 diff 下行）与 GUI（Skia 自绘）提供统一切口。GUI 侧完整管线见 [../docs/gui-architecture.md](../docs/gui-architecture.md)；本节记录内核视角的设计依据。

* **`ui::Scene`**（`src/ui/scene.hpp`）：后端无关的帧模型，按 UI 领域的标准分解——**layout（区域切分）+ paint（显示列表）**。`Region` = 角色 + 格子单位 `Rect`；每个 region 携带 `Prim` 显示列表（区域局部坐标的样式化文本，语义 `StyleClass` 不含颜色值）。**新 widget（minimap、折叠条、侧板）= 新增一个 region 并画同样的原语，Scene 结构与 presenter 均零改动**——presenter 只在想要像素级原生绘制时才按 role 特化（如 Skia 用编辑器侧富数据画像素 minimap 而非格子原语）。这是给"非文本 UI 元素"留的设计空间：不是逐个 widget 加字段（gutter/sign/minimap 各一个硬编码字段的形态被否决）。
* **`ui::build_line_runs`**：单行纯布局函数（token 高亮、选区切分、tab 展开、宽字形测量、双侧水平裁剪），无 pty 即可单测。列宽统一走 `code_point_width`（近似 wcwidth），光标数学与渲染共享同一函数，CJK 双列宽正确。
* **`ui::render_ansi`**：终端 presenter，纯函数 `Scene → 转义串`，整帧重绘一次 flush。remote 需要的帧 diff presenter 未来与它并列，不动 compose 侧。
* **变更 sign 列**：`diff_spans`（`diff_edit` 的不物化兄弟，共享 chunk 指针相等跳过）找出两侧变更窗口，`LineSigns` 映射为紧凑 span（Modified×n + Added×m + 删除边界），**O(1) 按行查询、不物化 per-line map**——首尾同时有未保存编辑的 522k 行文件实测 0.31ms/帧。基线目前是 saved_text_，将来换 git HEAD blob 即得 git-gutter，模型不变。
* remote 的切分决策：编辑核心贴数据侧运行，客户端是哑帧渲染器；Scene 序列化即协议底座，广域网本地回显预测（mosh 式）是 presenter 层追加，不动内核。
* **布局求解器（yoga）评估结论：不引**。当前 layout = 少量矩形单轴算术，配不上 flexbox 求解器；编辑器布局的难点（内容驱动尺寸、视口滚动、window split 二叉树）yoga 也不管；项目依赖面刻意收敛。region 模型使此决策可逆且局部：Scene 契约 = region+rect 恰是 yoga 的输出形状，布局算法整个藏在 compose() 的 layout 段，将来换手写 split 树或 yoga 均零波及 Scene/presenter。**触发条件**：可拖拽多窗格 split / 带 min-max 的停靠面板 / 比例分配——先评估 ~50 行 split 树，不够再上 yoga（注意 float→格子取整的"多余一列给谁"策略）。

---

## 18. 后续方向

* **(2,3)-树兄弟平衡**：兄弟列表的平衡仅在增量 concat 的性能数据要求时引入；当前分块 TokenBuffer + green splice 的实测性能未触发这个需求。
* **多文件**：内核目前单 translation unit；多文件化时 cxx-frontend 的 PendingInclude 拦截模式值得借鉴。
* **后台解析**：当前每键同步增量在预算内；引入后台解析时按 §6 的 revision/rebase 协议。
* **remote**：帧 diff presenter 与 Scene 序列化协议（§17）。

---

## 19. 架构判断

本层不是编辑器，而是一个可嵌入的**文档与语义缩进/语法内核**：

```text
text edits → revisioned document → lossless error-tolerant syntax
           → implicit formatting blocks → deterministic indent / enter plan
```

链路全程纯文本可测：绝大多数故障通过 fixture 确定性复现，输入、渲染和终端状态问题被隔离在内核之外。编辑器的其余层（输入、工作台、语言服务、呈现）全部建立在这条链路的 revision 化接口之上。
