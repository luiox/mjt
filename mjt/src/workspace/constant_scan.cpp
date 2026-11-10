#include "constant_scan.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <utility>

#include "jbc/core/opcode.hpp"

#include "classfile_support.hpp"

namespace mjt::workspace {

namespace {

using mjbase::project::DbNumberKind;
using mjbase::project::DbNumberRecord;

/// @brief 底层文本（string_view）→ 拥有型 Utf8String（与 analysis_ingest 的
///        utf8() 同口径：字节原文直通）。
ca::str::Utf8String utf8(std::string_view value)
{
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

/// @brief |value| < 2^63 且可整表示（double 域内判定）。
bool integralInI64Range(double value) noexcept
{
    return std::isfinite(value) && std::trunc(value) == value &&
           value >= -9223372036854775808.0 && value < 9223372036854775808.0;
}

/// @brief double 的 IEEE 位型（memcpy 到 i64；查询侧 numberNarrowKeys 同键）。
ca::i64 doubleBits(double value) noexcept
{
    ca::i64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// @brief 整型行装配：valueInt = 原值；valueText = numberText 十进制。
DbNumberRecord intRecord(ca::i64 insnIndex, ca::i32 opcode, DbNumberKind kind, ca::i64 value)
{
    DbNumberRecord record;
    record.insnIndex = insnIndex;
    record.opcode    = opcode;
    record.kind      = kind;
    record.valueInt  = value;
    record.valueText = utf8(numberText(value));
    return record;
}

/// @brief Float 行装配：valueText 按 float 精度档格式化（引擎口径）；位型
///        收窄键 = 宽化后 double 的 IEEE 位型。
DbNumberRecord float32Record(ca::i64 insnIndex, ca::i32 opcode, float value)
{
    DbNumberRecord record;
    record.insnIndex = insnIndex;
    record.opcode    = opcode;
    record.kind      = DbNumberKind::Float;
    record.valueBits = doubleBits(static_cast<double>(value));
    if (integralInI64Range(static_cast<double>(value)))
        record.valueInt = static_cast<ca::i64>(value);
    record.valueText = utf8(numberText(value));
    return record;
}

/// @brief Double 行装配（同 float32Record，精度档 17）。
DbNumberRecord float64Record(ca::i64 insnIndex, ca::i32 opcode, double value)
{
    DbNumberRecord record;
    record.insnIndex = insnIndex;
    record.opcode    = opcode;
    record.kind      = DbNumberKind::Double;
    record.valueBits = doubleBits(value);
    if (integralInI64Range(value)) record.valueInt = static_cast<ca::i64>(value);
    record.valueText = utf8(numberText(value));
    return record;
}

}   // namespace

ConstantUtf8Facet constantUtf8Facet(std::string_view value)
{
    if (value.empty()) return ConstantUtf8Facet::Name;
    if (value.front() == '(') return ConstantUtf8Facet::Descriptor;
    if (value.front() == '[') return ConstantUtf8Facet::ClassRef;
    if (value.front() == 'L' && value.back() == ';') return ConstantUtf8Facet::ClassRef;
    if (value.find('/') != std::string_view::npos) return ConstantUtf8Facet::ClassRef;
    return ConstantUtf8Facet::Name;
}

mjbase::project::DbUtf8Facet dbUtf8Facet(ConstantUtf8Facet facet)
{
    switch (facet) {
    case ConstantUtf8Facet::Descriptor: return mjbase::project::DbUtf8Facet::Descriptor;
    case ConstantUtf8Facet::ClassRef: return mjbase::project::DbUtf8Facet::ClassRef;
    case ConstantUtf8Facet::Name: break;
    }
    return mjbase::project::DbUtf8Facet::Name;
}

std::vector<mjbase::project::DbStringRecord> cpStringRecords(const jbc::core::ClassFile& cf)
{
    std::vector<mjbase::project::DbStringRecord> records;
    // String 背书 utf8 去重（引擎口径）：被 CONSTANT_String 引用的 utf8 条目由
    // String 行承载（string_literal 分面），不再以裸 utf8 行重复出现。
    std::set<ca::u32> stringBackedUtf8;
    for (ca::u32 index = 1; index < cf.cp.size(); ++index) {
        if (!cf.cp.valid(index)) continue;
        const auto& candidate = cf.cp.at(index);
        if (candidate.tag == jbc::core::CpTag::String && cf.cp.valid(candidate.a))
            stringBackedUtf8.insert(candidate.a);
    }
    for (ca::u32 index = 1; index < cf.cp.size(); ++index) {
        if (!cf.cp.valid(index)) continue;
        const auto& entry = cf.cp.at(index);
        mjbase::project::DbStringRecord record;
        if (entry.tag == jbc::core::CpTag::Utf8) {
            if (stringBackedUtf8.count(index) > 0) continue;
            record.kind  = mjbase::project::DbStringKind::Utf8;
            record.facet = dbUtf8Facet(constantUtf8Facet(cf.cp.utf8Of(index)));
            record.value = utf8(support::utf8Text(cf, index));
        }
        else if (entry.tag == jbc::core::CpTag::String) {
            // String 行 facet 落库归一 0（mjbase 侧已做，此处显式不预分）。
            record.kind  = mjbase::project::DbStringKind::String;
            record.facet = mjbase::project::DbUtf8Facet::Name;
            record.value = utf8(support::utf8Text(cf, entry.a));
        }
        else {
            continue;
        }
        records.push_back(std::move(record));
    }
    return records;
}

std::vector<ca::str::Utf8String> resourceStringRecords(const std::vector<ca::u8>& bytes)
{
    std::vector<ca::str::Utf8String> values;
    // 可打印 run 走查（引擎 resourceStrings 逐字节同形）：printable 谓词
    // [0x20,0x7e]，run 边界 = 越界字节或缓冲尾（循环走到 size 含尾冲刷），
    // 最短长度 1。空缓冲恒零产出（首步即边界，零长 run 不收）。
    ca::usize start = 0;
    for (ca::usize index = 0; index <= bytes.size(); ++index) {
        const bool printable =
            index < bytes.size() && bytes[index] >= 0x20 && bytes[index] <= 0x7e;
        if (printable) continue;
        if (index - start >= 1) {
            values.push_back(utf8(std::string_view(
                reinterpret_cast<const char*>(bytes.data()) + start, index - start)));
        }
        start = index + 1;
    }
    return values;
}

std::string numberText(ca::i64 value)
{
    std::ostringstream output;
    output << value;
    return output.str();
}

std::string numberText(float value)
{
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return output.str();
}

std::string numberText(double value)
{
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}

bool numbersEqual(const std::string& value, const std::string& query)
{
    if (value == query) return true;
    try {
        std::size_t valueOffset = 0;
        std::size_t queryOffset = 0;
        const auto  parsedValue = std::stold(value, &valueOffset);
        const auto  parsedQuery = std::stold(query, &queryOffset);
        return valueOffset == value.size() && queryOffset == query.size() &&
               parsedValue == parsedQuery;
    }
    catch (const std::exception&) {
        return false;
    }
}

NumberNarrowKeys numberNarrowKeys(const std::string& query)
{
    NumberNarrowKeys keys;
    try {
        std::size_t offset = 0;
        const auto  parsed = std::stold(query, &offset);
        if (offset != query.size()) return keys;
        if (std::isfinite(parsed) && std::trunc(parsed) == parsed &&
            parsed >= -9223372036854775808.0L && parsed < 9223372036854775808.0L)
            keys.valueInt = static_cast<ca::i64>(parsed);
        keys.valueBits = doubleBits(static_cast<double>(parsed));
    }
    catch (const std::exception&) {
        // 解析失败按超集处理：两键皆空，只剩 needle 字节等值守卫。
    }
    return keys;
}

std::vector<DbNumberRecord> cpNumberRecords(const jbc::core::ClassFile& cf)
{
    std::vector<DbNumberRecord> records;
    for (ca::u32 index = 1; index < cf.cp.size(); ++index) {
        if (!cf.cp.valid(index)) continue;
        const auto& entry = cf.cp.at(index);
        switch (entry.tag) {
        case jbc::core::CpTag::Integer:
            records.push_back(
                intRecord(-1, -1, DbNumberKind::Int, static_cast<ca::i64>(entry.ival)));
            break;
        case jbc::core::CpTag::Float:
            records.push_back(float32Record(
                -1, -1, support::bitsToFloat(static_cast<ca::u32>(entry.ival))));
            break;
        case jbc::core::CpTag::Long:
            records.push_back(intRecord(-1, -1, DbNumberKind::Long, entry.lval));
            break;
        case jbc::core::CpTag::Double:
            records.push_back(
                float64Record(-1, -1, support::bitsToDouble(static_cast<ca::u64>(entry.lval))));
            break;
        default: break;
        }
    }
    return records;
}

std::vector<MethodNumberSlice> methodNumberSlices(jbc::ir::ClassSession& session)
{
    std::vector<MethodNumberSlice> slices;
    jbc::core::ClassFile&          cf = session.backing();
    for (auto& visit : jbc::ir::collectCodeUnits(session)) {
        if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
        auto raw = visit.unit->dom.exportRaw();
        if (raw.is_err()) continue;
        const auto& insns = std::move(raw).unwrap();
        auto&       method = cf.methods[visit.methodIdx];

        MethodNumberSlice slice;
        slice.name       = support::utf8Text(cf, method.nameCp);
        slice.descriptor = support::utf8Text(cf, method.descCp);
        for (ca::i32 index = 0; index < static_cast<ca::i32>(insns.size()); ++index) {
            const auto& insn = insns[static_cast<ca::usize>(index)];
            const ca::i32 opcode = static_cast<ca::i32>(insn.op);
            std::vector<DbNumberRecord> pending;
            switch (insn.op) {
            case jbc::core::Op::ICONST_M1:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, -1));
                break;
            case jbc::core::Op::ICONST_0:
            case jbc::core::Op::LCONST_0:
            case jbc::core::Op::FCONST_0:
            case jbc::core::Op::DCONST_0:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, 0));
                break;
            case jbc::core::Op::ICONST_1:
            case jbc::core::Op::LCONST_1:
            case jbc::core::Op::FCONST_1:
            case jbc::core::Op::DCONST_1:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, 1));
                break;
            case jbc::core::Op::ICONST_2:
            case jbc::core::Op::FCONST_2:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, 2));
                break;
            case jbc::core::Op::ICONST_3:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, 3));
                break;
            case jbc::core::Op::ICONST_4:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, 4));
                break;
            case jbc::core::Op::ICONST_5:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, 5));
                break;
            default: break;
            }
            switch (jbc::core::shapeOf(insn.op)) {
            case jbc::core::Shape::Imm1:
            case jbc::core::Shape::Imm2:
            case jbc::core::Shape::Atype1:
            case jbc::core::Shape::Local1:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, insn.a));
                break;
            case jbc::core::Shape::Iinc1:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, insn.b));
                break;
            case jbc::core::Shape::TableSwitch:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, insn.a));
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, insn.b));
                break;
            case jbc::core::Shape::LookupSwitch:
                for (const auto& key : insn.cases)
                    pending.push_back(intRecord(index, opcode, DbNumberKind::Int, key.key));
                break;
            case jbc::core::Shape::CP2Dims:
                pending.push_back(intRecord(index, opcode, DbNumberKind::Int, insn.a));
                break;
            case jbc::core::Shape::CP1:
            case jbc::core::Shape::CP2: {
                if (cf.cp.valid(insn.cp)) {
                    const auto& entry = cf.cp.at(insn.cp);
                    switch (entry.tag) {
                    case jbc::core::CpTag::Integer:
                        pending.push_back(intRecord(index, opcode, DbNumberKind::Int,
                                                    static_cast<ca::i64>(entry.ival)));
                        break;
                    case jbc::core::CpTag::Long:
                        pending.push_back(
                            intRecord(index, opcode, DbNumberKind::Long, entry.lval));
                        break;
                    case jbc::core::CpTag::Float:
                        pending.push_back(float32Record(
                            index, opcode,
                            support::bitsToFloat(static_cast<ca::u32>(entry.ival))));
                        break;
                    case jbc::core::CpTag::Double:
                        pending.push_back(float64Record(
                            index, opcode,
                            support::bitsToDouble(static_cast<ca::u64>(entry.lval))));
                        break;
                    default: break;
                    }
                }
                break;
            }
            default: break;
            }
            for (auto& record : pending) slice.records.push_back(std::move(record));
        }
        slices.push_back(std::move(slice));
    }
    return slices;
}

}   // namespace mjt::workspace
