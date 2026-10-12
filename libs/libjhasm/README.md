# libjhasm

Jasm（JVM 汇编文本格式）的 C++ 实现：把 UTF-16 的 Jasm 源码汇编成 classfile 字节，
把 classfile 反汇编成规范化 Jasm，并可对 class 内的方法做帧分析。

## 这是重写，不是移植

libjhasm 是针对 Jasm 语言的一次**重写**，代码全部为自有实现，不含上游 Jasm 的
任何源码。`get_upstream_revision()` 返回本次重写所固定的上游 revision
（`aacebfa8b06a5c478adf13d9ce8e0e9f896ba840`），用于标记对齐基线；该基线只决定
语法覆盖到哪一版，不表示代码派生自它。

相对上游的语法差异见下文。**不要假设它与上游 Jasm 完全互通** —— 读写的都是
本库自己的规范化输出。

## 结构

```
parser/     tokenizer -> declaration parser -> AST processor
ast/        声明与指令的 AST 节点
compile/    AST -> classfile 字节
printer/    classfile -> 规范化 Jasm 文本
analysis/   JVM 帧分析
error/      Result<T> / ErrorCollector / Warn
util/       location、range、转义
```

## API

```cpp
#include "jhasm/jhasm.hpp"

namespace jhasm {
    // 完整 Jasm 源码 -> model + classfile 字节
    error::Result<compile::JvmCompileResult>
    assemble(ca::str::Utf16StringRef sourceName, ca::str::Utf16StringRef source);

    // classfile -> 可重新汇编的规范化 Jasm
    error::Result<ca::str::Utf16String> disassemble(const std::vector<ca::u8>& classBytes);

    // 单方法反汇编（不含外围 .class 声明）
    error::Result<ca::str::Utf16String>
    disassembleMethod(const std::vector<ca::u8>& classBytes,
                      ca::str::Utf8StringRef methodName,
                      ca::str::Utf8StringRef descriptor);

    // 帧分析
    error::Result<analysis::JvmAnalysisResult>
    analyze(const std::vector<ca::u8>& classBytes,
            const analysis::InheritanceChecker& inheritance);

    const char* get_lib_version();       // "0.2.0"
    const char* get_upstream_revision();  // 固定的 Jasm 基线 revision
}
```

所有接口返回 `error::Result<T>`，失败时带错误与警告集合，不抛异常。输入输出
统一走 `ca::str::Utf16String`（Jasm 的字符串字面量含 `\u` 转义，UTF-8 表示会
丢失信息）。

## 语法覆盖

声明级指令：`.class`、`.super`、`.implements`、`.inner`、`.nest`、`.outer`、
`.permitted`、`.record`、`.enum`、`.signature`、`.sourcefile`、`.annotation`、
`.visible` / `.invisible`、`.type`；指令集覆盖完整 JVM 操作码，含 `TABLESWITCH`
与 `LOOKUPSWITCH`。

## 往返保证

`disassemble` 的输出是**规范化**的，目标是能被 `assemble` 重新读回：

```
classfile -> disassemble -> Jasm 文本 -> assemble -> classfile'
```

这不是字节级恒等：常量池顺序、属性排列等非语义差异不保证保留。需要字节级保真
时应直接复制原始字节，而不是走文本往返。

## 构建

依赖 `libjblw` 与 `libjbytecode`。从仓库根构建：

```sh
xmake build libjhasm
```
