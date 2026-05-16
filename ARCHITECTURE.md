# mdrender 实现架构

## 1. 架构总览

```
输入 (.md) → md4c 解析器 → 回调函数 → ANSI 码输出 → 终端着色
```

程序不自己解析 markdown，而是把这项工作全权交给 **md4c** 库。md4c 是一个 SAX 风格的解析器：它边解析边调你注册的 5 个回调，而不是先构建 AST 再遍历。

---

## 2. md4c 回调模型

`md4c.h` 定义了 `MD_PARSER` 结构体，包含 5 个回调指针：

```c
parser.enter_block = enter_block_cb;   // 进入块元素（标题、段落、代码块…）
parser.leave_block = leave_block_cb;   // 离开块元素
parser.enter_span  = enter_span_cb;    // 进入行内元素（粗体、斜体、代码…）
parser.leave_span  = leave_span_cb;    // 离开行内元素
parser.text        = text_cb;          // 输出文本内容
```

调用 `md_parse(input, size, &parser, &ctx)` 后，md4c 按文档顺序依次触发这些回调。例如 `**Hello**` 会产生：

```
enter_span(MD_SPAN_STRONG)
  text(MD_TEXT_NORMAL, "Hello", 5)
leave_span(MD_SPAN_STRONG)
```

我们的工作就是在回调里输出 ANSI 转义码和文本。

---

## 3. 样式栈 —— 处理嵌套格式

核心挑战：`***粗斜体***` 本质是粗体套斜体（或反之）。ANSI 的 `\033[0m` 会**清空所有**样式，不能简单地进入时设、离开时清。

解决方案是维护一个**样式栈** (`ctx.styles[]`, `ctx.style_depth`)，在 `restyle()` 函数中统一重建所有样式：

```c
static void restyle(ctx *c) {
    out_str(ANSI_RESET);                   // 先清空
    if (c->heading_level > 0)              // 块级：标题颜色
        out_str(heading_ansi(c->heading_level));
    if (c->in_blockquote) out_str(ANSI_CYAN);  // 块级：引用色
    for (int i = 0; i < c->style_depth; i++)    // 遍历栈：行内样式
        out_str(span_ansi(c->styles[i]));
}
```

使用场景：

| 事件 | 操作 |
|------|------|
| `enter_span(type)` | `styles[style_depth++] = type` → `restyle(c)` |
| `leave_span(type)` | `style_depth--` → `restyle(c)` |

这样无论嵌套多深（如 `<strong><em><code>text</code></em></strong>`），终端显示始终正确。

---

## 4. 渲染上下文 (`ctx`)

```c
typedef struct {
    int bol;                   // Beginning Of Line —— 是否在行首

    int heading_level;         // 当前标题级别 (1-6)，0 表示不在标题中

    int in_blockquote;         // 是否在引用块中

    list_info list_stack[16];  // 列表嵌套栈
    int list_depth;
    int in_li_count;           // LI 嵌套计数器（计数器而非布尔值）
    int li_marker_emitted;     // 当前 LI 是否已输出标记
    int li_task_mark;          // GFM 任务标记字符 (x/' ')

    int in_code_block;         // 是否在代码块中
    buffer code_buf;           // 代码内容缓冲区
    int code_has_lang;
    char code_lang[64];

    int in_html_block;

    int tbl_active;
    int tbl_thead;
    unsigned tbl_cols;
    MD_ALIGN tbl_aligns[32];
    int tbl_in_cell;
    unsigned tbl_col;
    FILE *tbl_saved;

    MD_SPANTYPE styles[64];    // 行内样式栈
    int style_depth;
} ctx;
```

每个字段说明：

| 字段 | 作用 |
|------|------|
| `bol` | 标记当前是否在行首。用于在引用块、列表项等元素中输出前缀（`> `、`• ` 等）。 |
| `heading_level` | 0 表示不在标题中，1-6 分别对应 H1-H6。 |
| `in_blockquote` | 布尔值，进入 `<blockquote>` 时置 1，离开时清 0。 |
| `list_stack[]` | 存储每层列表的元信息（有序/无序、起始编号、分隔符），支持 16 层递归嵌套。 |
| `in_li_count` | **计数器而非布尔值**——进入 LI 时 +1，离开时 -1。这解决了 `* A\n  * B\n    * C` 多层嵌套时标记输出的问题。 |
| `li_marker_emitted` | 每个 LI 的标记（`1.`、`•`）只输出一次，用此标志确保不重复。 |
| `li_task_mark` | 任务列表的 `[x]` 或 `[ ]` 由 `MD_BLOCK_LI_DETAIL` 提供。 |
| `code_buf` | 代码块的内容不直接输出，用 buffer 缓存后整块处理。 |
| `tbl_active` | 是否在表格块内。 |
| `tbl_thead` | 是否在表头区 (THEAD) 内。 |
| `tbl_cols` | 表格列数，来自 `MD_BLOCK_TABLE_DETAIL.col_count`。 |
| `tbl_aligns[]` | 每列的对齐方式，来自 TH 块的 `MD_BLOCK_TD_DETAIL.align`。 |
| `tbl_in_cell` | 是否正在捕获单元格内容。 |
| `tbl_col` | 当前单元格的列索引。 |
| `tbl_saved` | 进入单元格时保存的全局 `g_out`，退出时恢复。 |
| `styles[]` | 行内样式栈，详见第 3 节。 |

---

## 5. 各元素的渲染逻辑

### 5.1 标题

**位置：** `enter_block_cb` / `MD_BLOCK_H`

```
enter_block(MD_BLOCK_H)：
  1. 输出 heading_ansi(level) — 例如 H1: 粗体+下划线+亮黄
  2. 输出 `# `（按 level 重复 # 号）
  3. bol = 0（后续文本直接跟在后面）
```

标题的 ANSI 颜色映射：

| 级别 | ANSI 码 |
|------|---------|
| H1 | 粗体 + 下划线 + 亮黄 |
| H2 | 粗体 + 亮青 |
| H3 | 粗体 + 亮绿 |
| H4 | 粗体 + 亮蓝 |
| H5 | 粗体 + 亮洋红 |
| H6 | 粗体 + 亮黑（灰） |

### 5.2 段落 & 文本

段落本身不做特殊处理。`enter_block(P)` 只是设 `bol=1`（新段落从行首开始），`leave_block(P)` 输出一个换行。

### 5.3 引用块

**位置：** `emit_text` → `bol && in_blockquote`

```
每次在行首输出文本时：
  1. 输出 `> `（青色）
  2. restyle() 恢复内部样式（如粗体、代码等）
```

效果：引用块内的所有内容都有青色底色，且内部的格式嵌套（粗体、代码等）不受影响。

```
> 青色底色下的 **粗体** 和 `代码`
  ──┬──      ──┬──      ─┬─
    │           │          └ restyle 恢复的颜色
    │           └ restyle 恢复后 + span_ansi(STRONG)
    └ restyle 恢复后 + ANSI_CYAN
```

### 5.4 列表

**列表标记输出：** `emit_text` → `in_li_count > 0 && !li_marker_emitted`

首次遇到 LI 的文本时：
1. 如果不在行首，先输出换行（处理嵌套列表）
2. 读取 `list_stack[list_depth-1]` 获取列表类型
3. 有序列表：`snprintf("%u%c ", start+count-1, delim)` → `1. `、`2. `
4. 无序列表：`-`/`+` 直接输出原字符，`*` 输出 Unicode `•`
5. 任务列表：追加 `[x]`（绿色勾选）或 `[ ]`（灰色未选）
6. 设 `li_marker_emitted = 1`

**列表嵌套：**

关键设计是 `in_li_count` 为计数器。考虑输入：

```
- Top
  - Nested
    - Deep
```

解析器产生的回调序列：

```
enter(UL)                     list_depth=1
  enter(LI)                   in_li_count=0→1, bol=1
    text("Top")               marker="- Top", bol=0
    enter(UL)                 list_depth=1→2
      enter(LI)               in_li_count=1→2, bol=1 → 输出 \n（因为 in_li_count 从1变为2且在行中）
        text("Nested")        marker="- Nested"
        enter(UL)             list_depth=2→3
          enter(LI)           in_li_count=2→3, bol=1 → 输出 \n
            text("Deep")      marker="- Deep"
          leave(LI)           in_li_count=3→2, 输出 \n
        leave(UL)             list_depth=3→2
      leave(LI)               in_li_count=2→1, 输出 \n
    leave(UL)                 list_depth=2→1
  leave(LI)                   in_li_count=1→0, 输出 \n
leave(UL)                     list_depth=1→0
```

`in_li_count` 从 1→2 表示进入了嵌套 LI，需要换行；从 2→1 表示离开嵌套回到外层，不需要额外操作。

### 5.5 代码块

与普通文本不同，代码块的内容需要**缓冲后统一处理**：

```
enter_block(MD_BLOCK_CODE)：
  1. in_code_block = 1
  2. 初始化 code_buf
  3. 保存语言标识（如 "python"）

text_cb(MD_TEXT_CODE)：
  1. 如果 in_code_block → buf_append() 到 code_buf
  2. 否则 → emit_text()（这是行内代码）

leave_block(MD_BLOCK_CODE)：
  1. 调用 render_code_block()
```

`render_code_block()` 的工作：

```
1. 输出语言标签（暗色 + 白色文字）
   例如：  python

2. 按 \n 分割缓冲区，每行输出：
   绿色  │  代码内容
   绿色  │  def hello():
   绿色  │      print("world")
```

### 5.6 行内代码

与代码块共享 `MD_TEXT_CODE` 文本类型，但区分方式：

| 场景 | `in_code_block` | 处理方式 |
|------|----------------|----------|
| 代码块内容 | 1 | `buf_append()` 到缓冲区 |
| 行内 `` `code` `` | 0 | `emit_text()` 直接输出（样式由样式栈中 `MD_SPAN_CODE` 的 ANSI 码决定） |

行内代码的颜色由 `span_ansi(MD_SPAN_CODE)` 返回亮绿色 `\033[92m`。

### 5.7 链接

**位置：** `leave_span_cb` → `MD_SPAN_A`

```
enter_span(MD_SPAN_A)：
  1. 压栈 (style_depth++)
  2. restyle() → 链接文本变蓝色下划线

leave_span(MD_SPAN_A)：
  1. 读取 MD_SPAN_A_DETAIL.href
  2. 输出暗色 <url>
  3. 弹栈 (style_depth--)
  4. restyle() → 恢复父级样式
```

输出示例：

```
GitHub 上的蓝色下划线链接文本，后面跟着暗色的 <https://github.com>
```

### 5.8 图片

**位置：** `enter_span_cb` → `MD_SPAN_IMG`

```
enter_span(MD_SPAN_IMG)：
  1. 输出 [IMG: url]（暗黄）
  2. restyle() → 后续 alt 文本继承暗黄色
```

md4c 会把图片的 alt 文本通过 `text_cb` 传入，此时样式栈中 `MD_SPAN_IMG` 已压栈，alt 文本自动显示为暗黄色。

### 5.9 表格

表格采用**缓冲 + 后计算宽度**的方式，而非流式输出：

```
技术要点：

  1. 进入 TH/TD 块时：将全局输出句柄 g_out 切换到 per-cell 的
     open_memstream，所有文本和样式写入此流

  2. 离开 TH/TD 时：关闭 g_out，析出 styled 字符串，恢复全局 g_out

  3. TCell 结构体：
     - styled: 带 ANSI 码的单元格内容
     - col, is_header, cap_size

  4. 宽度计算使用 dispw() 函数，它跳过 ANSI 转义码统计可见字符数：
     static int dispw(const char *s) {
         int n = 0;
         while (*s) {
             if (*s == '\033') {
                 while (*s && *s != 'm') s++;
                 if (*s) s++;
             } else { n++; s++; }
         }
         return n;
     }

  5. 在 leave_block(MD_BLOCK_TABLE) 中，所有单元格已捕获完毕：
     a) 计算每列最大可见宽度 colw[ci]
     b) 输出顶部边框：┌───┬───┐（Unicode 箱型字符）
     c) 逐行输出内容：│A··│B··│（左/中/右对齐）
     d) 行间输出分隔行：├───┼───┤
     e) 表头/表体之间输出带对齐标记的分隔行：
        ├:───┼:───:┼───:┤（: 表示对齐方向）
     f) 输出底部边框：└───┴───┘
     g) 释放所有 styled 字符串
```

### 5.10 其他元素

| 元素 | 处理位置 | 方式 |
|------|----------|------|
| 分割线 HR | `enter_block` | 72 个 `─` 字符（暗色） |
| HTML 块/行内 | `text_cb` → `MD_TEXT_HTML` | 直接输出（暗色） |
| LaTeX 公式 | `text_cb` → `MD_TEXT_LATEXMATH` | 直接输出（洋红） |
| 软换行 | `text_cb` → `MD_TEXT_SOFTBR` | `\n` + `bol=1` |
| 硬换行 | `text_cb` → `MD_TEXT_BR` | `\n` + `bol=1` |

---

## 6. main() 与 process_file()

```c
int main(int argc, char **argv) {
    FILE *in = stdin;
    if (argc > 1) in = fopen(argv[1], "rb");
    int ret = process_file(in);
    ...
}
```

`process_file()` 的工作：

```
1. 用可伸缩 buffer 读取全部输入（按需 realloc，倍增扩容）

2. 清空 ctx，设 bol=1

3. 配置 MD_PARSER：
   flags = MD_FLAG_COLLAPSEWHITESPACE    // 折叠多余空白
         | MD_FLAG_PERMISSIVEAUTOLINKS   // 自动链接（URL、Email）
         | MD_FLAG_TABLES                // GFM 表格
         | MD_FLAG_STRIKETHROUGH         // 删除线 ~~text~~
         | MD_FLAG_TASKLISTS             // 任务列表 - [x]
         | MD_FLAG_UNDERLINE             // 下划线 _text_
         | MD_FLAG_SUPERSCRIPTS          // 上标 ^text^
         | MD_FLAG_SUBSCRIPTS            // 下标 ~text~

4. 调用 md_parse(input, size, &parser, &ctx)

5. 输出 ANSI_RESET 恢复终端
```

---

## 7. ANSI 颜色映射

### 块级颜色

| 上下文 | ANSI 码 |
|--------|---------|
| 标题 (H1-H6) | 见 5.1 节表格 |
| 引用块 | `\033[36m` (青色) |
| 代码块 | `\033[32m` (绿色) |
| HTML | `\033[2m` (暗色) |

### 行内样式

| 类型 | ANSI 码 | 终端效果 |
|------|---------|----------|
| `MD_SPAN_EM` | `\033[3m\033[96m` | 斜体 + 亮青 |
| `MD_SPAN_STRONG` | `\033[1m` | 粗体 |
| `MD_SPAN_CODE` | `\033[92m` | 亮绿 |
| `MD_SPAN_DEL` | `\033[9m` | 删除线 |
| `MD_SPAN_A` | `\033[4m\033[94m` | 下划线 + 亮蓝 |
| `MD_SPAN_IMG` | `\033[2m\033[33m` | 暗黄 |
| `MD_SPAN_U` | `\033[4m` | 下划线 |
| `MD_SPAN_SPOILER` | `\033[7m` | 反白 |
| `MD_SPAN_LATEXMATH` | `\033[35m` | 洋红 |
| `MD_SPAN_WIKILINK` | `\033[4m\033[92m` | 下划线 + 亮绿 |

---

## 8. 构建方式

```bash
gcc -o mdrender mdrender.c md4c/src/md4c.c -I md4c/src
```

- 直接编译 `md4c.c` 进入二进制，不依赖动态库
- `mdrender.c` 只包含 `md4c.h` 一个头文件
- 整个 md4c 库是自包含的单文件实现 (`md4c.c` + `md4c.h`)
