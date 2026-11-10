/// @file        constant_scan.hpp
/// @brief       常量事实抽取层：类常量池字符串/数值切片的单一事实源。
///
/// search kind=strings / kind=numbers 的 DB 化（analysis.db 常量事实面，
/// mjbase schema v12）把引擎 workspace.cpp 内联的两条行走抽成纯函数：
/// 引擎搜索（fallback 路径）与 analysis_ingest / analysis_edit_sync 的
/// 入库切片喂同一组函数，抽取规则只写一份、两条调用路零漂移：
/// - cpStringRecords：String 行 + 裸 utf8 行（含 facet 形状预分，规则与
///   MCP 面 utf8_* 文本同源——constantUtf8Facet）；
/// - cpNumberRecords / methodNumberSlices：常量池数值行 + 逐方法指令立即
///   数行（valueText 用引擎 numberText 逐字节，valueInt/valueBits 收窄键
///   在本层收口）；
/// - numberText / numbersEqual：数值格式化与等值终审（引擎查询面的原语，
///   DB 查询路径的客户端终审共用）；
/// - resourceStringRecords：非 class 条目可打印串 run（资源串事实面，
///   mjbase schema v13，引擎 resourceStrings 活扫描的抽取层对应物）。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/project/analysis_db.hpp"

namespace mjt::workspace {

/// @brief 裸 UTF8 常量池条目的内容形态分面（MCP 面 utf8_* 文本同规则）。
enum class ConstantUtf8Facet : ca::u8
{
    Name       = 0,
    Descriptor = 1,
    ClassRef   = 2,
};

/// @brief 内容形态分类器（json.cpp utf8StringKind 的单一事实源）：
///        空 → name；'(' 开头 → descriptor；'[' 开头 / 'L…;' / 含 '/' →
///        classref；否则 name。
ConstantUtf8Facet constantUtf8Facet(std::string_view value);

/// @brief 分面 → mjbase cp_strings facet（枚举值同序，显式映射钉住对应）。
mjbase::project::DbUtf8Facet dbUtf8Facet(ConstantUtf8Facet facet);

/// @brief 类常量池字符串切片（CONSTANT_String 行 + 裸 utf8 行含 facet 预分；
///        String 背书 utf8 去重与池遍历序逐字节同引擎 searchStrings 常量池分支）。
std::vector<mjbase::project::DbStringRecord> cpStringRecords(const jbc::core::ClassFile& cf);

/// @brief 非 class 条目字节的资源串切片（资源串事实面，mjbase schema v13
///        `resources` 表的抽取源）：可打印 run 规则逐字节同引擎
///        `resourceStrings` 活扫描——谓词 [0x20,0x7e]，run 边界 = 越界字节
///        或缓冲尾，最短长度 1 档（引擎搜索路 resourceStrings(1) 同档；
///        同条目同值由库 UNIQUE 折叠吸收，引擎按 offset 区分的重复行在
///        SearchHit 去重后同集）。
std::vector<ca::str::Utf8String> resourceStringRecords(const std::vector<ca::u8>& bytes);

/// @brief 数值文本格式化（引擎 numberText 逐字节）：整型十进制；浮点
///        max_digits10 + defaultfloat（1.0f → "1"）。
std::string numberText(ca::i64 value);
std::string numberText(float value);    ///< float 精度档（max_digits10 = 9）。
std::string numberText(double value);   ///< double 精度档（max_digits10 = 17）。

/// @brief 数值等值终审（引擎 matchesNumber 语义）：文本等值 OR 双侧 stold
///        完整解析后数值等值。DB 数值查询的候选超集在此收口。
bool numbersEqual(const std::string& value, const std::string& query);

/// @brief 数值查询的收窄键（DbNumberQuery 构造共用）：stold 完整解析成功时
///        整值且 |v| < 2^63 → valueInt；static_cast<double> 的 IEEE 位型 →
///        valueBits；解析失败两者皆空（超集不收窄）。
struct NumberNarrowKeys
{
    std::optional<ca::i64> valueInt;   ///< 整型值收窄键；空 = 不参与。
    std::optional<ca::i64> valueBits;  ///< IEEE 位型收窄键；空 = 不参与。
};
NumberNarrowKeys numberNarrowKeys(const std::string& query);

/// @brief 类常量池数值切片（Integer/Long/Float/Double 条目，池序遍历）：
///        整型行带 valueInt；浮点行带 valueBits（统一经 double 位型，float
///        先宽化——查询侧同键）且值可整表示、|v| < 2^63 时另带 valueInt。
std::vector<mjbase::project::DbNumberRecord> cpNumberRecords(const jbc::core::ClassFile& cf);

/// @brief 单个方法的数值字面量切片（name+descriptor 身份键，库成员行解析用）。
struct MethodNumberSlice
{
    std::string                                name;
    std::string                                descriptor;
    std::vector<mjbase::project::DbNumberRecord> records;
};

/// @brief 逐方法数值切片（collectCodeUnits 程序序；iconst/lconst/fconst/
///        dconst 族、bipush/sipush、atype/local/iinc 立即数、switch 键、
///        CP1/CP2 数值 CP 目标——规则逐字节同引擎 searchNumbers 指令分支）。
///        有 Code 的方法恒产出一个切片（零数值 = 空 records，编辑同步的
///        清切片纪律需要空切片也是一次显式替换）。
std::vector<MethodNumberSlice> methodNumberSlices(jbc::ir::ClassSession& session);

}   // namespace mjt::workspace
