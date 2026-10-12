#include "mbase/pass/opt/global_str_pool_optimizer.hpp"
#include "mbase/pass/opt/inline_detail.hpp"
#include "class_io_detail.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "libca/core/tag_cast.hpp"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace mjt::base {

//=== 合并自 str_pool_detail.hpp（单消费者实现细节，原 _detail.hpp 拆分文件）===

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

constexpr const char* kStringPoolClass = "com/github/luiox/mjt/util/StringPool";
constexpr const char* kStringPoolUri   = "com/github/luiox/mjt/util/StringPool.class";
constexpr const char* kStringPoolField = "strPool";
constexpr const char* kStringArrayDesc = "[Ljava/lang/String;";

bool rewriteStringConstants(ClassNode& classNode, const std::map<std::string, int>& stringIndex)
{
    bool modifiedClass = false;
    for (auto* method : classNode.methods) {
        if (!method || method->instructions.isEmpty()) continue;

        bool                               modifiedMethod = false;
        //绑定方法 arena：!modifiedMethod 早退时析构掠过 arena 克隆产物（unbound
        //表会误 delete arena 节点）。
        InsnList                           result(&method->arena());
        std::unordered_map<Label*, Label*> labels;
        for (auto* insn : method->instructions.toArray()) {
            std::string value;
            if (InsnUtil::getStringValue(insn, value)) {
                auto it = stringIndex.find(value);
                if (it != stringIndex.end()) {
                    result.add(InsnUtil::getIntInsn(result, it->second));
                    result.add(result.create<MethodInsnNode>(
                        Opcodes::INVOKESTATIC,
                        ca::str::Utf8StringRef::from_cstr(kStringPoolClass),
                        ca::str::Utf8StringRef::from_cstr("getStr"),
                        ca::str::Utf8StringRef::from_cstr("(I)Ljava/lang/String;"),
                        false));
                    modifiedMethod = true;
                    continue;
                }
            }
            //B8 新 clone 协议：result 最终并回 method->instructions，产物归其 arena。
            result.add(insn->clone(method->arena(), labels));
        }

        if (!modifiedMethod) continue;
        method->instructions.clear();
        method->instructions.add(result);
        method->maxStack = std::max(method->maxStack, 3);
        modifiedClass    = true;
    }
    return modifiedClass;
}

std::vector<ca::u8> buildStringPoolClass(ca::str::Utf8StringArena&         arena,
                                         const std::map<std::string, int>& strings)
{
    ClassWriter cw(arena, 0);
    cw.visit(Opcodes::V1_8,
             Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
             ca::str::Utf8StringRef::from_cstr(kStringPoolClass),
             ca::str::Utf8StringRef(),
             ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
             {});

    auto* field = cw.visitField(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                ca::str::Utf8StringRef::from_cstr(kStringPoolField),
                                ca::str::Utf8StringRef::from_cstr(kStringArrayDesc),
                                ca::str::Utf8StringRef(),
                                nullptr);
    field->visitEnd();

    auto* init = cw.visitMethod(Opcodes::ACC_PUBLIC,
                                ca::str::Utf8StringRef::from_cstr("<init>"),
                                ca::str::Utf8StringRef::from_cstr("()V"),
                                ca::str::Utf8StringRef(),
                                {});
    init->visitCode();
    init->visitVarInsn(Opcodes::ALOAD, 0);
    init->visitMethodInsn(Opcodes::INVOKESPECIAL,
                          ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                          ca::str::Utf8StringRef::from_cstr("<init>"),
                          ca::str::Utf8StringRef::from_cstr("()V"),
                          false);
    init->visitInsn(Opcodes::RETURN);
    init->visitMaxs(1, 1);
    init->visitEnd();

    auto* clinit = cw.visitMethod(Opcodes::ACC_STATIC,
                                  ca::str::Utf8StringRef::from_cstr("<clinit>"),
                                  ca::str::Utf8StringRef::from_cstr("()V"),
                                  ca::str::Utf8StringRef(),
                                  {});
    clinit->visitCode();
    clinit->visitIntInsn(Opcodes::SIPUSH, static_cast<int>(strings.size()));
    clinit->visitTypeInsn(Opcodes::ANEWARRAY,
                          ca::str::Utf8StringRef::from_cstr("java/lang/String"));
    clinit->visitFieldInsn(Opcodes::PUTSTATIC,
                           ca::str::Utf8StringRef::from_cstr(kStringPoolClass),
                           ca::str::Utf8StringRef::from_cstr(kStringPoolField),
                           ca::str::Utf8StringRef::from_cstr(kStringArrayDesc));
    for (const auto& [value, index] : strings) {
        clinit->visitFieldInsn(Opcodes::GETSTATIC,
                               ca::str::Utf8StringRef::from_cstr(kStringPoolClass),
                               ca::str::Utf8StringRef::from_cstr(kStringPoolField),
                               ca::str::Utf8StringRef::from_cstr(kStringArrayDesc));
        clinit->visitIntInsn(Opcodes::SIPUSH, index);
        clinit->visitLdcInsn(ca::core::Any(value));
        clinit->visitInsn(Opcodes::AASTORE);
    }
    clinit->visitInsn(Opcodes::RETURN);
    clinit->visitMaxs(3, 0);
    clinit->visitEnd();

    auto* getStr = cw.visitMethod(Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC,
                                  ca::str::Utf8StringRef::from_cstr("getStr"),
                                  ca::str::Utf8StringRef::from_cstr("(I)Ljava/lang/String;"),
                                  ca::str::Utf8StringRef(),
                                  {});
    getStr->visitCode();
    getStr->visitFieldInsn(Opcodes::GETSTATIC,
                           ca::str::Utf8StringRef::from_cstr(kStringPoolClass),
                           ca::str::Utf8StringRef::from_cstr(kStringPoolField),
                           ca::str::Utf8StringRef::from_cstr(kStringArrayDesc));
    getStr->visitVarInsn(Opcodes::ILOAD, 0);
    getStr->visitInsn(Opcodes::AALOAD);
    getStr->visitInsn(Opcodes::ARETURN);
    getStr->visitMaxs(2, 1);
    getStr->visitEnd();

    cw.visitEnd();
    return cw.toByteArray();
}

}   //namespace


PreservedAnalyses GlobalStrPoolOptimizer::run(ResourceContainer&     container,
                                              GlobalAnalysisManager& gam)
{
    if (container.hasResource(kStringPoolUri)) return PreservedAnalyses::all();

    auto classes = loadClassesForInlining(container, gam.arena());
    if (classes.empty()) return PreservedAnalyses::all();

    std::map<std::string, int> stringIndex;
    for (const auto& loaded : classes) {
        if (!loaded.node ||
            loaded.node->name.equals(ca::str::Utf8StringRef::from_cstr(kStringPoolClass)))
            continue;
        for (auto* method : loaded.node->methods) {
            if (!method) continue;
            for (auto* insn : method->instructions) {
                std::string value;
                if (InsnUtil::getStringValue(insn, value) &&
                    stringIndex.find(value) == stringIndex.end()) {
                    stringIndex[value] = static_cast<int>(stringIndex.size());
                }
            }
        }
    }
    if (stringIndex.empty()) return PreservedAnalyses::all();

    bool modifiedAny = false;
    for (auto& loaded : classes) {
        if (!loaded.resource || !loaded.node) continue;
        if (loaded.node->name.equals(ca::str::Utf8StringRef::from_cstr(kStringPoolClass))) continue;
        if (!rewriteStringConstants(*loaded.node, stringIndex)) continue;
        writeClassNode(*loaded.resource, gam.arena(), *loaded.node);
        modifiedAny = true;
    }
    if (!modifiedAny) return PreservedAnalyses::all();

    //合成 URI 可能与既有条目同名，属有意替换，不走 first-wins 导入语义。
    container.replaceResource(kStringPoolUri,
                          std::make_unique<ClassResource>(
                              kStringPoolUri, buildStringPoolClass(gam.arena(), stringIndex)));
    return PreservedAnalyses::none();
}

}   //namespace mjt::base
