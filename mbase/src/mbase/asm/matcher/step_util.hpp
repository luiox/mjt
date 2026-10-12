#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "mbase/asm/matcher/match_step.hpp"
#include "mbase/asm/matcher/pattern_matcher.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/constants.hpp"

namespace mjt::base {

//step 的两种生命周期，均无需手动 delete：
//- 无谓词 step(opcode)：按 opcode 索引的数组静态缓存，同 opcode 全局复用；
//- 谓词 step(opcode, pred)：std::function 不可比较、无法按内容去重缓存，
//每次调用新建一个 SimpleStep，由进程级 arena（unique_ptr 注册表）接管
//所有权，进程退出时统一释放；返回裸指针供 MatchRule 等调用方长期持有。
struct StepUtil
{
    static IMatchStep* step(int opcode)
    {
        auto& arr = cache();
        if (!arr[opcode]) arr[opcode] = new SimpleStep(opcode);
        return arr[opcode];
    }
    static IMatchStep* step(int                                                     opcode,
                            std::function<bool(jbytecode::tree::AbstractInsnNode*)> pred)
    {
        auto& arena = predStepArena();
        arena.push_back(std::make_unique<SimpleStep>(opcode, std::move(pred)));
        return arena.back().get();
    }

    //基本操作
    static IMatchStep* nop() { return step(jbytecode::core::Opcodes::NOP); }
    static IMatchStep* aconst_null() { return step(jbytecode::core::Opcodes::ACONST_NULL); }
    static IMatchStep* iconst_m1() { return step(jbytecode::core::Opcodes::ICONST_M1); }
    static IMatchStep* iconst_0() { return step(jbytecode::core::Opcodes::ICONST_0); }
    static IMatchStep* iconst_1() { return step(jbytecode::core::Opcodes::ICONST_1); }
    static IMatchStep* iconst_2() { return step(jbytecode::core::Opcodes::ICONST_2); }
    static IMatchStep* iconst_3() { return step(jbytecode::core::Opcodes::ICONST_3); }
    static IMatchStep* iconst_4() { return step(jbytecode::core::Opcodes::ICONST_4); }
    static IMatchStep* iconst_5() { return step(jbytecode::core::Opcodes::ICONST_5); }

    static IMatchStep* pop() { return step(jbytecode::core::Opcodes::POP); }
    static IMatchStep* pop2() { return step(jbytecode::core::Opcodes::POP2); }
    static IMatchStep* dup() { return step(jbytecode::core::Opcodes::DUP); }
    static IMatchStep* dup_x1() { return step(jbytecode::core::Opcodes::DUP_X1); }
    static IMatchStep* dup_x2() { return step(jbytecode::core::Opcodes::DUP_X2); }
    static IMatchStep* dup2() { return step(jbytecode::core::Opcodes::DUP2); }
    static IMatchStep* dup2_x1() { return step(jbytecode::core::Opcodes::DUP2_X1); }
    static IMatchStep* dup2_x2() { return step(jbytecode::core::Opcodes::DUP2_X2); }
    static IMatchStep* swap() { return step(jbytecode::core::Opcodes::SWAP); }

    //算数
    static IMatchStep* iadd() { return step(jbytecode::core::Opcodes::IADD); }
    static IMatchStep* isub() { return step(jbytecode::core::Opcodes::ISUB); }
    static IMatchStep* imul() { return step(jbytecode::core::Opcodes::IMUL); }
    static IMatchStep* idiv() { return step(jbytecode::core::Opcodes::IDIV); }
    static IMatchStep* irem() { return step(jbytecode::core::Opcodes::IREM); }
    static IMatchStep* ineg() { return step(jbytecode::core::Opcodes::INEG); }
    static IMatchStep* iand() { return step(jbytecode::core::Opcodes::IAND); }
    static IMatchStep* ior() { return step(jbytecode::core::Opcodes::IOR); }
    static IMatchStep* ixor() { return step(jbytecode::core::Opcodes::IXOR); }
    static IMatchStep* ishl() { return step(jbytecode::core::Opcodes::ISHL); }
    static IMatchStep* ishr() { return step(jbytecode::core::Opcodes::ISHR); }
    static IMatchStep* iushr() { return step(jbytecode::core::Opcodes::IUSHR); }

    static IMatchStep* ladd() { return step(jbytecode::core::Opcodes::LADD); }
    static IMatchStep* lsub() { return step(jbytecode::core::Opcodes::LSUB); }
    static IMatchStep* lmul() { return step(jbytecode::core::Opcodes::LMUL); }
    static IMatchStep* ldiv() { return step(jbytecode::core::Opcodes::LDIV); }
    static IMatchStep* lrem() { return step(jbytecode::core::Opcodes::LREM); }
    static IMatchStep* lneg() { return step(jbytecode::core::Opcodes::LNEG); }
    static IMatchStep* land() { return step(jbytecode::core::Opcodes::LAND); }
    static IMatchStep* lor() { return step(jbytecode::core::Opcodes::LOR); }
    static IMatchStep* lxor() { return step(jbytecode::core::Opcodes::LXOR); }
    static IMatchStep* lshl() { return step(jbytecode::core::Opcodes::LSHL); }
    static IMatchStep* lshr() { return step(jbytecode::core::Opcodes::LSHR); }
    static IMatchStep* lushr() { return step(jbytecode::core::Opcodes::LUSHR); }

    static IMatchStep* fadd() { return step(jbytecode::core::Opcodes::FADD); }
    static IMatchStep* fsub() { return step(jbytecode::core::Opcodes::FSUB); }
    static IMatchStep* fmul() { return step(jbytecode::core::Opcodes::FMUL); }
    static IMatchStep* fdiv() { return step(jbytecode::core::Opcodes::FDIV); }
    static IMatchStep* frem() { return step(jbytecode::core::Opcodes::FREM); }
    static IMatchStep* fneg() { return step(jbytecode::core::Opcodes::FNEG); }

    static IMatchStep* dadd() { return step(jbytecode::core::Opcodes::DADD); }
    static IMatchStep* dsub() { return step(jbytecode::core::Opcodes::DSUB); }
    static IMatchStep* dmul() { return step(jbytecode::core::Opcodes::DMUL); }
    static IMatchStep* ddiv() { return step(jbytecode::core::Opcodes::DDIV); }
    static IMatchStep* drem() { return step(jbytecode::core::Opcodes::DREM); }
    static IMatchStep* dneg() { return step(jbytecode::core::Opcodes::DNEG); }

    //类型转换
    static IMatchStep* i2l() { return step(jbytecode::core::Opcodes::I2L); }
    static IMatchStep* i2f() { return step(jbytecode::core::Opcodes::I2F); }
    static IMatchStep* i2d() { return step(jbytecode::core::Opcodes::I2D); }
    static IMatchStep* i2b() { return step(jbytecode::core::Opcodes::I2B); }
    static IMatchStep* i2c() { return step(jbytecode::core::Opcodes::I2C); }
    static IMatchStep* i2s() { return step(jbytecode::core::Opcodes::I2S); }
    static IMatchStep* l2i() { return step(jbytecode::core::Opcodes::L2I); }
    static IMatchStep* l2f() { return step(jbytecode::core::Opcodes::L2F); }
    static IMatchStep* l2d() { return step(jbytecode::core::Opcodes::L2D); }
    static IMatchStep* f2i() { return step(jbytecode::core::Opcodes::F2I); }
    static IMatchStep* f2l() { return step(jbytecode::core::Opcodes::F2L); }
    static IMatchStep* f2d() { return step(jbytecode::core::Opcodes::F2D); }
    static IMatchStep* d2i() { return step(jbytecode::core::Opcodes::D2I); }
    static IMatchStep* d2l() { return step(jbytecode::core::Opcodes::D2L); }
    static IMatchStep* d2f() { return step(jbytecode::core::Opcodes::D2F); }

    //Load 步骤 — 按类型匹配
    static IMatchStep* loadInt();
    static IMatchStep* loadFloat();
    static IMatchStep* loadDouble();
    static IMatchStep* loadLong();
    static IMatchStep* loadStr();
    static IMatchStep* bipush() { return step(jbytecode::core::Opcodes::BIPUSH); }

    //invokestatic / invokevirtual / invokespecial 带过滤
    static IMatchStep* invokestatic(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                    ca::str::Utf8StringRef desc);
    static IMatchStep* invokevirtual(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                     ca::str::Utf8StringRef desc);
    static IMatchStep* invokespecial(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                     ca::str::Utf8StringRef desc);

private:
    static std::vector<IMatchStep*>& cache()
    {
        static std::vector<IMatchStep*> s(256, nullptr);
        return s;
    }
    //谓词 step 的进程级所有权登记表：SimpleStep 不可按内容去重（std::function
    //无相等比较），退化为只增 arena，进程退出时统一析构。
    static std::vector<std::unique_ptr<IMatchStep>>& predStepArena()
    {
        static std::vector<std::unique_ptr<IMatchStep>> arena;
        return arena;
    }
};

}   //namespace mjt::base
