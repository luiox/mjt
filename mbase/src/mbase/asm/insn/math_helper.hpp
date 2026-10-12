#pragma once

///@file math_helper.hpp
///
///@brief JVM 算术与条件求值的统一入口。
///
///本 helper 提供两类 API：
///- **类型保留 Any API**（推荐）：`binaryAny` / `unaryAny` / `classifyValue`
///以 `ca::core::Any` 载荷为输入输出，按 JVM 类型严格分派，正确处理整数
///补码环绕、`MIN / -1` 特例、移位掩码（int 低 5 位、long 低 6 位）以及
///浮点 NaN 比较语义。常量折叠、溯源分析、引用重写共用这套实现。
///- **简化 double API**（历史遗留）：`binaryOp` / `unaryOp` 用 double 做通用
///载体，会丢失类型信息，仅用于条件谓词和近似求值。新代码请使用 Any API。

#include "jbytecode/core/opcodes.hpp"
#include "libca/core/any.hpp"
#include <functional>
#include <unordered_map>

namespace mjt::base {

///JVM 跳转/算术指令的条件求值与操作映射
struct MathHelper
{
    ///一元条件：IFEQ/IFNE/IFLT/IFGE/IFGT/IFLE
    static bool isOneValueCondition(int opcode);
    static bool condition(int value, int opcode);

    ///二元条件：IF_ICMPEQ/IF_ICMPNE/IF_ICMPLT/IF_ICMPGE/IF_ICMPGT/IF_ICMPLE
    static bool isTwoValuesCondition(int opcode);
    static bool condition(int a, int b, int opcode);

    ///二元算术运算（double 载荷，丢失类型精度，仅供条件谓词/近似求值）。
    ///@brief 需要 JVM 严格语义（环绕/移位掩码/NaN）时改用 `binaryAny`。
    static bool   isBinaryOp(int opcode);
    static double binaryOp(double a, double b, int opcode);

    ///一元算术运算（double 载荷，同上精度限制）。
    ///@brief 需要 JVM 严格语义时改用 `unaryAny`。
    static bool   isUnaryOp(int opcode);
    static double unaryOp(double a, int opcode);

    ///取 jump opcode 的逆（例如 IFEQ → IFNE）
    static int negateJumpOpcode(int opcode);

    ///@name 类型保留的 JVM 算术（Any 载荷）
    ///
    ///输入 `Any` 必须持有以下类型之一：`int` / `int64_t` / `float` / `double`
    ///（及其窄整型如 `bool`/`char`/`short`，会被 classify 归一到 int）。
    ///输出 `Any` 持有对应 JVM 类别结果；输入空或类型不匹配时返回空 `Any`。
    ///整数加减乘按补码环绕，`MIN / -1` 在 `I*DIV` 上返回 `MIN`、在 `I*REM`
    ///上返回 `0`（JVM 规范），除零返回空 `Any`（上层视为抛 `ArithmeticException`）。
    ///移位距离按 JVM 规范分别取低 5 位（int）或 6 位（long）。
    ///`*CMP*` 返回 `int` 类型的 -1/0/1；`FCMPL`/`DCMPL` 与 `FCMPG`/`DCMPG`
    ///按 NaN 语义分别返回 -1 或 1。
    ///@{

    ///@brief 把 `Any` 归一到 JVM 类别标签。
    ///
    ///返回 `(规范化后的值, 类别)`，类别 0=int、1=long、2=float、3=double、
    ///-1=未知。窄整型（bool/char/short/int8/int16/uint16）归一到 int。
    static std::pair<ca::core::Any, int> classifyValue(const ca::core::Any& v);

    ///@brief 类型保留的二元 JVM 算术（IADD/ISUB/.../LCMP/FCMPL/...）。
    ///
    ///双操作数类型必须相同，唯一例外是 `LSHL`/`LSHR`/`LUSHR`，其 shift 量
    ///为 int 而被操作数为 long。输入任一为空、类型不匹配、除零时返回空。
    static ca::core::Any binaryAny(int opcode, const ca::core::Any& a, const ca::core::Any& b);

    ///@brief 类型保留的一元 JVM 算术（INEG/I2B/.../L2I/F2D/...）。
    ///
    ///输入为空返回空；`CHECKCAST` 原样返回输入。
    static ca::core::Any unaryAny(int opcode, const ca::core::Any& value);

    ///@}
};

}   //namespace mjt::base
