#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@defgroup intrinsic_folding 已知 JDK intrinsic 折叠家族
///
///本家族把可证明无副作用、且参数全为常量的已知 JDK 调用或固定模式替换为等价常量
///或更小的指令序列。三类 pass 各负责一个白名单：
///
///- `BoxUnboxEliminator`：消除 `valueOf` + `xxxValue` 装箱/拆箱对。
///- `RandomSeedResolver`：折叠固定种子的 `java.util.Random.nextXxx`。
///- `StdConsFunctionOptimizer`：折叠小型 JDK 纯函数（`String.length`、`Math.abs`、
///`Integer.parseInt` 等）。
///
///三类共享的设计原则：
///
///- 白名单驱动：不在白名单的 owner/name/desc 不动。
///- 参数必须可证明为常量（BoxUnbox 例外，因为它只是消去成对调用）。
///- 不删可能抛异常、有副作用或身份敏感的调用。
///- parse、边界、浮点 NaN/有符号零、整数溢出必须遵循 JVM 语义。
///- 成功改写返回 `none`，否则 `all`，方法/frame 分析失效。
///
///三类都不进入 `default-optimize`，因为它们编码了 JDK 库语义。调用方通过名称显式 opt in，
///直到白名单和兼容性策略足够广泛。新增规则族一次添加一个，附带 roundtrip 测试和异常/平台
///特定行为记录。
///
///@note 本家族的 matcher helper 在 `intrinsic_folding.cpp` 匿名命名空间内，不通过
///`*_detail.hpp` 暴露实现。家族内三类互不共享 matcher，只共享家族契约和文件归属。
///@{

///@brief 删除紧邻且匹配的装箱/拆箱对。
///
///@par 输入与不支持范围
///只支持精确的 wrapper `valueOf` 与对应 primitive `xxxValue`；对象被 dup、存储、身份比较、
///逃逸、跨基本块或分析失败时必须保留装箱。不得改变 null、异常或身份语义。
///
///变换前：
///@code
///iconst_0
///invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;
///invokevirtual java/lang/Integer.intValue I
///@endcode
///变换后：
///@code
///iconst_0
///@endcode
///
///@par 结果、失效与发布状态
///成功删除返回 `none`，否则 `all`；方法/帧分析失效，替换节点由方法指令表拥有。
///需要各 wrapper、null/异常、identity 和 JVM roundtrip fixture。当前为 `opt-in`，不进入默认链。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class BoxUnboxEliminator : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 折叠固定种子的 `java.util.Random` 调用。
///
///@par 输入与不支持范围
///仅匹配 `NEW; DUP; loadLong; <init>(J)V; nextXxx` 的完整形状，且不允许子类、插入调用、
///`setSeed`、对象逃逸或其他 Random API。种子或算法结果无法证明时保持原样。
///
///变换前：
///@code
///new java/util/Random
///dup
///ldc2_w 123
///invokespecial java/util/Random.<init> (J)V
///invokevirtual java/util/Random.nextInt I
///@endcode
///变换后：
///@code
///ldc <computed-int>
///@endcode
///
///@par 结果、失效与验证
///用 Java Random LCG 计算常量并替换完整表达式时返回 `none`，否则 `all`；方法/帧分析
///失效。必须用 JDK 8/17/21 结果、边界种子和 roundtrip fixture 验证。当前为 `opt-in`，
///迁移忠实度为支持形状内 `FAITHFUL`，不进入默认链。
///
///@par 标签
///- `scope:method`
///- `action:fold`
///- `stability:opt-in`
class RandomSeedResolver : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///在输入全为常量时折叠已知的纯标准库调用。
///
///## 目标
///
///`StdConsFunctionOptimizer` 当每个输入都是字节码常量时，折叠小型标准库白名单中的
///函数调用。用于混淆常量和紧凑的局部清理，不用于通用部分求值。
///
///此 pass 暂时不进入 `default-optimize`，因为它编码了库语义。调用方应通过名称显式
///opt in，直到白名单和兼容性策略足够广泛可默认使用。
///
///## 白名单模型
///
///每条规则有三部分：
///
///- 精确的 owner/name/descriptor 匹配
///- 每个操作数的常量加载器
///- 将计算结果物化为字节码常量的替换值
///
///不支持的 overload 保持原样。可能失败的规则（如 parsing 或 substring）在匹配时验证。
///如果转换、parse 或边界检查会抛出异常，则不匹配，原字节码保留给后续 pass。这一点
///很重要，因为 matcher 会在策略运行后删除匹配区间。
///
///## 受支持的标准库函数
///
///### String / Math （已有）
///
///- `String.length`、`String.isEmpty` 和常量 `String.substring`
///- `String.toCharArray` 后紧跟 `String.valueOf(char[])`
///- `Math.abs`（int/long/float/double）
///- `Math.min` / `Math.max`（int/long/float/double）
///- `Integer.signum`
///- `Integer.parseInt` 和 `Long.parseLong`（合法十进制字符串）
///
///### java.lang.Long 位运算和格式化（v675）
///
///一元（常量 long → 常量结果）：
///- `reverse(J)J` — 位反转
///- `reverseBytes(J)J` — 字节序反转
///- `bitCount(J)I` — 二进制 1 的个数
///- `numberOfLeadingZeros(J)I` — 前导零个数
///- `numberOfTrailingZeros(J)I` — 末尾零个数
///- `highestOneBit(J)J` — 最高位 1 的幂值
///- `lowestOneBit(J)J` — 最低位 1 的幂值
///- `signum(J)I` — 符号函数
///- `toBinaryString(J)Ljava/lang/String;`
///- `toHexString(J)Ljava/lang/String;`
///- `toOctalString(J)Ljava/lang/String;`
///- `toString(J)Ljava/lang/String;`
///
///二元（常量 long + 常量 int → 常量结果）：
///- `rotateLeft(JI)J`
///- `rotateRight(JI)J`
///- `toString(JI)Ljava/lang/String;`
///
///### java.lang.Integer 位运算和格式化（v675）
///
///一元（常量 int → 常量结果）：
///- `reverse(I)I` — 位反转
///- `reverseBytes(I)I` — 字节序反转
///- `bitCount(I)I` — 二进制 1 的个数
///- `numberOfLeadingZeros(I)I` — 前导零个数
///- `numberOfTrailingZeros(I)I` — 末尾零个数
///- `highestOneBit(I)I` — 最高位 1 的幂值
///- `lowestOneBit(I)I` — 最低位 1 的幂值
///- `signum(I)I` — 符号函数（已在 v675 前支持）
///- `toBinaryString(I)Ljava/lang/String;`
///- `toHexString(I)Ljava/lang/String;`
///- `toOctalString(I)Ljava/lang/String;`
///- `toString(I)Ljava/lang/String;`
///
///二元（常量 int + 常量 int → 常量结果）：
///- `rotateLeft(II)I`
///- `rotateRight(II)I`
///- `toString(II)Ljava/lang/String;`
///
///浮点 `min` / `max` 在支持的常量情况下保留 Java NaN 和有符号零的边界行为。
///显式处理而非直接委托给 C++ `std::min`/`std::max`，因为其 NaN 行为与 Java
///`Math` 不一致。
///
///整型 `Math.abs` 也保留 Java 溢出行为：`Integer.MIN_VALUE` 和 `Long.MIN_VALUE`
///折叠为自身。`parseInt`/`parseLong` 要求整个字符串合法，含范围检查。`"42x"` 或
///空白填充的值不折叠，但前导加号被接受以匹配 Java 解析行为。
///
///所有 Long/Integer 位运算公式使用 C++17 的手工实现（`std::rotl` 等 C++20 设施
///不可用），语义与 JDK 的 `Long`/`Integer` 一致。
///
///产字符串规则（`toXxxString`/`toString`/`substring`）经 `MatchContext::internString`
///把结果收进作业级 arena（`AnalysisManager::arena`，生命周期覆盖 class 序列化），
///`LdcInsnNode` 持有 arena 视图而非 `std::string` 拷贝（issue /）。
///
///## 边界
///
///pass 不：
///
///- 检查接收者别名或存储值
///- 求值 locale 敏感 API
///- 将解析失败折叠为抛出异常
///- 折叠 C++ `std::stoi`/`std::stoll` 可提前停止的部分解析
///- 推理 classpath 覆盖或非 JDK 的 owner
///- 修改操作数非直接常量的调用点
///
///后续扩展应一次添加一个规则族，附带 roundtrip 测试并记录任何异常或平台特定行为。
///
///## 输入与不支持范围
///
///只接受白名单调用和直接字节码常量；非法 parse、部分 parse、substring 越界、
///locale 相关、未支持 overload、局部加载或副作用调用保持原样。
///浮点 NaN/有符号零和整数溢出必须遵循 JVM。
///
///变换前：
///@code
///ldc "hello"
///invokevirtual java/lang/String.length I
///@endcode
///变换后：
///@code
///iconst_5
///@endcode
///
///## 验证覆盖
///
///- String 长度和 parse 折叠
///- 部分字符串的 parse 拒绝和前导加号支持
///- int/long/float/double 数学函数折叠
///- 整型 `Math.abs` 的 Java 溢出行为
///- `toCharArray` / `String.valueOf(char[])` 抵消
///- 非法或不受支持形状保持不变
///- 浮点数学重写后的 class 重载
///- Long/Integer 位运算（rotate/reverse/bitCount/leadingZeros 等）常量折叠
///- Long/Integer toXxxString / toString 常量折叠
///- rotate/reverse 边界值（0, -1, MIN_VALUE）
///- 非常量输入保持 invokestatic 不动
///
///@par 标签
///- `scope:method`
///- `action:fold`
///- `stability:opt-in`
class StdConsFunctionOptimizer : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@}

}   //namespace mjt::base
