
首次遇到 LI 的文本时：
1. 如果不在行首，先输出换行（处理嵌套列表）
2. 读取 `list_stack[list_depth-1]` 获取列表类型
3. 有序列表：`snprintf("%u%c ", start+count-1, delim)` → `1. `、`2. `
4. 无序列表：`-`/`+` 直接输出原字符，`*` 输出 Unicode `•`
5. 任务列表：追加 `[x]`（绿色勾选）或 `[ ]`（灰色未选）
6. 设 `li_marker_emitted = 1`
   1. x
   2. y
      1. a
      2. b

* hello
- nihao
- test
  - test1
  - test2


**列表嵌套：**

