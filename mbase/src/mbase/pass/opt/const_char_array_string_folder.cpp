#include "mbase/pass/opt/const_char_array_string_folder.hpp"

#include "libca/str/utf16_string.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "libca/core/tag_cast.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using ca::str::Utf8StringRef;

namespace {

Utf8StringRef u8(const char* value)
{
    return Utf8StringRef::from_cstr(value);
}

//在 int 常量栈上求值 [startPos, ...)，直到遇到 opcode == terminator 的指令。
//
//只支持无副作用、全为整型常量的表达式：常量加载（经 InsnUtil::getIntValue 识别
//iconst/bipush/sipush/ldc int）以及总函数运算（不含会陷入的 idiv/irem）。遇到
//Label/Line/Frame（常量表达式必须直线）或任何不支持指令时失败。成功时把终止指令位置写入
//termPos，栈内容留在 stack。
bool evalIntStack(InsnList& insns, int startPos, int terminator, std::vector<std::int32_t>& stack,
                  int& termPos)
{
    stack.clear();
    for (int p = startPos; p < insns.size(); ++p) {
        auto* n = insns.get(p);
        if (!n) return false;

        const int type = n->getType();
        if (type == AbstractInsnNode::LABEL || type == AbstractInsnNode::LINE ||
            type == AbstractInsnNode::FRAME) {
            return false;
        }

        const int op = n->getOpcode();
        if (op == terminator) {
            termPos = p;
            return true;
        }

        int loaded = 0;
        if (InsnUtil::getIntValue(n, loaded)) {
            stack.push_back(loaded);
            continue;
        }

        auto binary = [&](std::int32_t (*fn)(std::uint32_t, std::uint32_t)) -> bool {
            if (stack.size() < 2) return false;
            const auto b = static_cast<std::uint32_t>(stack.back());
            stack.pop_back();
            const auto a = static_cast<std::uint32_t>(stack.back());
            stack.pop_back();
            stack.push_back(fn(a, b));
            return true;
        };
        auto unary = [&](std::int32_t (*fn)(std::uint32_t)) -> bool {
            if (stack.empty()) return false;
            stack.back() = fn(static_cast<std::uint32_t>(stack.back()));
            return true;
        };

        bool ok = true;
        switch (op) {
        case Opcodes::IADD:
            ok = binary([](std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a + b); });
            break;
        case Opcodes::ISUB:
            ok = binary([](std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b); });
            break;
        case Opcodes::IMUL:
            ok = binary([](std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a * b); });
            break;
        case Opcodes::IXOR:
            ok = binary([](std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a ^ b); });
            break;
        case Opcodes::IAND:
            ok = binary([](std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a & b); });
            break;
        case Opcodes::IOR:
            ok = binary([](std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a | b); });
            break;
        case Opcodes::ISHL:
            ok = binary([](std::uint32_t a, std::uint32_t b) {
                return static_cast<std::int32_t>(a << (b & 31u));
            });
            break;
        case Opcodes::ISHR:
            ok = binary([](std::uint32_t a, std::uint32_t b) {
                return static_cast<std::int32_t>(static_cast<std::int32_t>(a) >> (b & 31u));
            });
            break;
        case Opcodes::IUSHR:
            ok = binary([](std::uint32_t a, std::uint32_t b) {
                return static_cast<std::int32_t>(a >> (b & 31u));
            });
            break;
        case Opcodes::INEG:
            ok = unary([](std::uint32_t a) { return static_cast<std::int32_t>(0u - a); });
            break;
        case Opcodes::I2C:
            ok = unary([](std::uint32_t a) { return static_cast<std::int32_t>(a & 0xFFFFu); });
            break;
        case Opcodes::I2B:
            ok = unary([](std::uint32_t a) {
                return static_cast<std::int32_t>(static_cast<std::int8_t>(a));
            });
            break;
        case Opcodes::I2S:
            ok = unary([](std::uint32_t a) {
                return static_cast<std::int32_t>(static_cast<std::int16_t>(a));
            });
            break;
        default:
            return false;
        }
        if (!ok) return false;
    }
    return false;
}

//尝试匹配从 startPos 起的 `new String(constant char[])` 形状。成功时 endExclusive 指向
//invokespecial 之后一位，utf8out 为还原出的 UTF-8 字符串。
bool matchNewStringCharArray(InsnList& insns, int startPos, int& endExclusive, std::string& utf8out)
{
    auto* nNew = insns.get(startPos);
    if (!nNew || nNew->getOpcode() != Opcodes::NEW) return false;
    auto* typeNode = ca::core::tag_cast::cast<TypeInsnNode*>(nNew);
    if (!typeNode || !typeNode->desc.equals(u8("java/lang/String"))) return false;

    auto* nDup = insns.get(startPos + 1);
    if (!nDup || nDup->getOpcode() != Opcodes::DUP) return false;

    std::vector<std::int32_t> stack;
    int                       newArrayPos = -1;
    if (!evalIntStack(insns, startPos + 2, Opcodes::NEWARRAY, stack, newArrayPos)) return false;
    if (stack.size() != 1) return false;
    const std::int32_t length = stack[0];
    if (length < 0 || length > 65535) return false;

    auto* naNode = insns.get(newArrayPos);
    auto* naInt  = ca::core::tag_cast::cast<IntInsnNode*>(naNode);
    if (!naInt || naInt->operand != Opcodes::T_CHAR) return false;

    //-1 作哨兵表示未写入；合法 code unit 为 0..65535。同一下标多次 castore 取最后一次（Java 语义）。
    std::vector<std::int32_t> units(static_cast<std::size_t>(length), -1);
    int                       pos = newArrayPos + 1;
    for (std::int32_t e = 0; e < length; ++e) {
        auto* dup = insns.get(pos);
        if (!dup || dup->getOpcode() != Opcodes::DUP) return false;

        int castorePos = -1;
        if (!evalIntStack(insns, pos + 1, Opcodes::CASTORE, stack, castorePos)) return false;
        if (stack.size() != 2) return false;

        const std::int32_t idx = stack[0];
        const std::int32_t val = stack[1];
        if (idx < 0 || idx >= length) return false;
        units[static_cast<std::size_t>(idx)] = static_cast<std::int32_t>(static_cast<std::uint32_t>(val) & 0xFFFFu);
        pos = castorePos + 1;
    }

    for (const auto unit : units) {
        if (unit < 0) return false;   //存在未覆盖下标
    }

    auto* nInit = insns.get(pos);
    if (!nInit || nInit->getOpcode() != Opcodes::INVOKESPECIAL) return false;
    auto* mi = ca::core::tag_cast::cast<MethodInsnNode*>(nInit);
    if (!mi || !mi->owner.equals(u8("java/lang/String")) || !mi->name.equals(u8("<init>")) ||
        !mi->desc.equals(u8("([C)V"))) {
        return false;
    }
    endExclusive = pos + 1;

    std::vector<ca::u16> codeUnits(static_cast<std::size_t>(length));
    for (std::int32_t i = 0; i < length; ++i) {
        codeUnits[static_cast<std::size_t>(i)] = static_cast<ca::u16>(units[static_cast<std::size_t>(i)]);
    }
    try {
        ca::str::Utf16String utf16(codeUnits.data(), codeUnits.size());
        utf8out = utf16.to_utf8_string().ref().to_std_string();
    }
    catch (...) {
        return false;   //非法 surrogate 等：保持原指令
    }
    return true;
}

}   //namespace

PreservedAnalyses ConstCharArrayStringFolder::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    //绑定方法 arena：!changed 早退时析构掠过 arena 克隆产物（unbound 表会误
    //delete arena 节点），堆上合成的 Ldc 节点仍由表 delete。
    InsnList                           result(&methodNode.arena());
    std::unordered_map<Label*, Label*> clonedLabels;
    bool                               changed = false;

    for (int i = 0; i < insns.size();) {
        int         endExclusive = -1;
        std::string text;
        if (matchNewStringCharArray(insns, i, endExclusive, text)) {
            const auto interned =
                am.arena().intern(reinterpret_cast<const ca::u8*>(text.data()), text.size());
            result.add(methodNode.create<LdcInsnNode>(interned));
            i       = endExclusive;
            changed = true;
            continue;
        }
        //B8 新 clone 协议：产物归 methodNode 的内容 arena（result 结束时并回
        //instructions）。注意 am.arena 是字符串池 Utf8StringArena，与节点无关。
        result.add(insns.get(i)->clone(methodNode.arena(), clonedLabels));
        i++;
    }

    if (!changed) return PreservedAnalyses::all();
    insns.clear();
    insns.add(result);
    return PreservedAnalyses::none();
}

}   //namespace mjt::base
