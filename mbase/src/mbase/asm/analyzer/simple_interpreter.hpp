#pragma once

#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "jbytecode/tree/insn_list.hpp"

#include "mbase/asm/matcher/match_step.hpp"
#include "mbase/asm/matcher/step_util.hpp"
#include "original_source_value.hpp"

namespace mjt::base {

struct MethodInterpreter
{
    mjt::base::IMatchStep* step_;
    std::function<std::optional<OriginalSourceConstantValue>(
        const jbytecode::tree::AbstractInsnNode*, std::vector<OriginalSourceValue>&)>
        methodComputation_;

    static MethodInterpreter of(
        mjt::base::IMatchStep* step,
        std::function<std::optional<OriginalSourceConstantValue>(
            const jbytecode::tree::AbstractInsnNode*, std::vector<OriginalSourceValue>&)>
            computation)
    {
        return {step, std::move(computation)};
    }
};

struct SimpleInterpreter
{
    static std::vector<MethodInterpreter>& getMethodInterpreters()
    {
        static std::vector<MethodInterpreter> instances = buildDefault();
        return instances;
    }

    static const std::vector<MethodInterpreter>& METHOD_INTERPRETERS;

    static void registerInterpreter(MethodInterpreter interpreter)
    {
        getMethodInterpreters().push_back(std::move(interpreter));
    }

private:
    static std::vector<MethodInterpreter> buildDefault()
    {
        std::vector<MethodInterpreter> result;

        //String#length
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/String"),
                                   ca::str::Utf8StringRef::from_cstr("length"),
                                   ca::str::Utf8StringRef::from_cstr("()I")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    auto* text = cv->get().as<std::string>();
                    if (text)
                        return std::make_optional(
                            OriginalSourceConstantValue::of(static_cast<int>(text->length())));
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //String#hashCode
        result.push_back(MethodInterpreter::of(
            StepUtil::invokevirtual(ca::str::Utf8StringRef::from_cstr("java/lang/String"),
                                    ca::str::Utf8StringRef::from_cstr("hashCode"),
                                    ca::str::Utf8StringRef::from_cstr("()I")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    auto* text = cv->get().as<std::string>();
                    if (text)
                        return std::make_optional(OriginalSourceConstantValue::of(
                            static_cast<int>(std::hash<std::string>{}(*text))));
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //Integer#parseInt(String)
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                   ca::str::Utf8StringRef::from_cstr("parseInt"),
                                   ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;)I")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    auto* text = cv->get().as<std::string>();
                    if (text) {
                        try {
                            return std::make_optional(
                                OriginalSourceConstantValue::of(std::stoi(*text)));
                        }
                        catch (...) {
                        }
                    }
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //Integer#valueOf(String)
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(
                ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                ca::str::Utf8StringRef::from_cstr("valueOf"),
                ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;)Ljava/lang/Integer;")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    auto* text = cv->get().as<std::string>();
                    if (text) {
                        try {
                            return std::make_optional(
                                OriginalSourceConstantValue::of(std::stoi(*text)));
                        }
                        catch (...) {
                        }
                    }
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //Integer#parseInt(String, int)
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                   ca::str::Utf8StringRef::from_cstr("parseInt"),
                                   ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;I)I")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& firstValue  = stackValues[stackValues.size() - 2];
                auto& secondValue = stackValues[stackValues.size() - 1];
                auto* cv1         = firstValue.getConstantValue();
                auto* cv2         = secondValue.getConstantValue();
                if (cv1 && cv2) {
                    auto* text  = cv1->get().as<std::string>();
                    auto* radix = cv2->get().as<int>();
                    if (text && radix) {
                        try {
                            return std::make_optional(
                                OriginalSourceConstantValue::of(std::stoi(*text, nullptr, *radix)));
                        }
                        catch (...) {
                        }
                    }
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //Integer#reverse
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                   ca::str::Utf8StringRef::from_cstr("reverse"),
                                   ca::str::Utf8StringRef::from_cstr("(I)I")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    if (auto* value = cv->get().as<int>()) {
                        unsigned int u = static_cast<unsigned int>(*value);
                        u              = ((u & 0x55555555) << 1) | ((u >> 1) & 0x55555555);
                        u              = ((u & 0x33333333) << 2) | ((u >> 2) & 0x33333333);
                        u              = ((u & 0x0F0F0F0F) << 4) | ((u >> 4) & 0x0F0F0F0F);
                        u              = ((u & 0x00FF00FF) << 8) | ((u >> 8) & 0x00FF00FF);
                        u              = (u << 16) | (u >> 16);
                        return std::make_optional(
                            OriginalSourceConstantValue::of(static_cast<int>(u)));
                    }
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //Long#reverse
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                   ca::str::Utf8StringRef::from_cstr("reverse"),
                                   ca::str::Utf8StringRef::from_cstr("(J)J")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    if (auto* value = cv->get().as<int64_t>()) {
                        uint64_t u = static_cast<uint64_t>(*value);
                        u = ((u & 0x5555555555555555ULL) << 1) | ((u >> 1) & 0x5555555555555555ULL);
                        u = ((u & 0x3333333333333333ULL) << 2) | ((u >> 2) & 0x3333333333333333ULL);
                        u = ((u & 0x0F0F0F0F0F0F0F0FULL) << 4) | ((u >> 4) & 0x0F0F0F0F0F0F0F0FULL);
                        u = ((u & 0x00FF00FF00FF00FFULL) << 8) | ((u >> 8) & 0x00FF00FF00FF00FFULL);
                        u = ((u & 0x0000FFFF0000FFFFULL) << 16) |
                            ((u >> 16) & 0x0000FFFF0000FFFFULL);
                        u = (u << 32) | (u >> 32);
                        return std::make_optional(
                            OriginalSourceConstantValue::of(static_cast<int64_t>(u)));
                    }
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //Float#floatToIntBits
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Float"),
                                   ca::str::Utf8StringRef::from_cstr("floatToIntBits"),
                                   ca::str::Utf8StringRef::from_cstr("(F)I")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    auto* value = cv->get().as<float>();
                    if (value) {
                        int bits;
                        std::memcpy(&bits, value, sizeof(bits));
                        return std::make_optional(OriginalSourceConstantValue::of(bits));
                    }
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //Float#intBitsToFloat
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Float"),
                                   ca::str::Utf8StringRef::from_cstr("intBitsToFloat"),
                                   ca::str::Utf8StringRef::from_cstr("(I)F")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    auto* value = cv->get().as<int>();
                    if (value) {
                        float bits;
                        std::memcpy(&bits, value, sizeof(bits));
                        return std::make_optional(OriginalSourceConstantValue::of(bits));
                    }
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //Double#doubleToLongBits
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Double"),
                                   ca::str::Utf8StringRef::from_cstr("doubleToLongBits"),
                                   ca::str::Utf8StringRef::from_cstr("(D)J")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    auto* value = cv->get().as<double>();
                    if (value) {
                        int64_t bits;
                        std::memcpy(&bits, value, sizeof(bits));
                        return std::make_optional(OriginalSourceConstantValue::of(bits));
                    }
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        //Double#longBitsToDouble
        result.push_back(MethodInterpreter::of(
            StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Double"),
                                   ca::str::Utf8StringRef::from_cstr("longBitsToDouble"),
                                   ca::str::Utf8StringRef::from_cstr("(J)D")),
            [](const jbytecode::tree::AbstractInsnNode*,
               std::vector<OriginalSourceValue>& stackValues) {
                auto& sourceValue = stackValues[stackValues.size() - 1];
                auto* cv          = sourceValue.getConstantValue();
                if (cv) {
                    auto* value = cv->get().as<int64_t>();
                    if (value) {
                        double bits;
                        std::memcpy(&bits, value, sizeof(bits));
                        return std::make_optional(OriginalSourceConstantValue::of(bits));
                    }
                }
                return std::optional<OriginalSourceConstantValue>{};
            }));

        return result;
    }
};

inline const std::vector<MethodInterpreter>& SimpleInterpreter::METHOD_INTERPRETERS =
    SimpleInterpreter::getMethodInterpreters();

}   //namespace mjt::base
