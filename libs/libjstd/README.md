# libjstd

Java 语言 / JVM 语义的 C++ 拟合库，命名空间 `jstd`。

## 定位

字节码分析与优化过程中需要精确复现 Java 的整数运算与随机数行为（溢出回绕、
除法截断、`>>>` 逻辑右移、窄化转换、`java.util.Random` LCG 等）。本库把这些
语义收敛为一份实现，作为 C++ 侧的权威来源。

## 模块

- `jstd/arith.hpp`：`jstd::arith` 命名空间——
  - int32/int64 溢出回绕四则（`add`/`sub`/`mul`/`div`/`rem`，含 `INT_MIN/-1` 特例、
    除法向零截断、取模符号跟随被除数）、`neg`/`abs`；
  - 移位 `shl`/`shr`/`ushr`（位移量 `& 0x1F` / `& 0x3F` 掩码取模，`ushr` 逻辑右移）；
  - 窄化 `i2b`/`i2c`/`i2s`/`l2i`（低位位模式）。
- `jstd/random.hpp`：`jstd::Random` —— `java.util.Random` 位级复刻（48-bit LCG，
  multiplier `0x5DEECE66D`、addend `0xB`；`next`/`nextInt()`/`nextInt(bound)`
  拒绝采样/`nextLong`/`nextFloat`/`nextDouble`/`nextBoolean`/`setSeed`）。

## 权威性

JVM 是唯一权威：期望值应由真实 JDK 生成并固化后逐位对照，而不是凭记忆填写。
消费方若发现自己有第二份实现，应改用本库——重复实现正是本库要消除的问题。

## 构建

```sh
xmake build libjstd
```
