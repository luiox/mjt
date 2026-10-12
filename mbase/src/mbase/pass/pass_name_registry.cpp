#include "mbase/pass/pass_name_registry.hpp"

#include "mbase/pass/opt/illegal_class_file_cleaner.hpp"
#include "mbase/pass/opt/meta_info_remover.hpp"
#include "mbase/pass/opt/unused_label_remover.hpp"
#include "mbase/pass/opt/local_constant_propagation.hpp"
#include "mbase/pass/opt/constant_jump_optimizer.hpp"
#include "mbase/pass/opt/jump_label_layout.hpp"
#include "mbase/pass/opt/try_catch_cleanup.hpp"
#include "mbase/pass/opt/dead_code_remover.hpp"
#include "mbase/pass/opt/char_arr_to_str_simplifier.hpp"
#include "mbase/pass/opt/const_char_array_string_folder.hpp"
#include "mbase/pass/opt/intrinsic_folding.hpp"
#include "mbase/pass/opt/expression_simplifier.hpp"
#include "mbase/pass/opt/fix_pattern_simplifier.hpp"
#include "mbase/pass/opt/type_cast_simplifier.hpp"
#include "mbase/pass/opt/constant_folder.hpp"
#include "mbase/pass/opt/constant_propagator.hpp"
#include "mbase/pass/opt/stack_operation.hpp"
#include "mbase/pass/opt/insn_rearranger.hpp"
#include "mbase/pass/opt/method_inliner.hpp"
#include "mbase/pass/opt/unused_member_remover.hpp"
#include "mbase/pass/opt/enum_cast_check_remover.hpp"

#include "mbase/pass/opt/global_str_pool_optimizer.hpp"
#include "mbase/pass/opt/label_dead_var_store_eliminator.hpp"

#include "mbase/pass/opt/pure_function.hpp"

#include "mbase/pass/opt/static_field_default_inliner.hpp"
#include "mbase/pass/opt/tool_class_combiner.hpp"
#include "mbase/pass/opt/access_modifier_tightener.hpp"
#include "mbase/pass/opt/class_member_sorter.hpp"
#include "mbase/pass/opt/class_renamer.hpp"
#include "mbase/pass/opt/local_variable_info_rebuilder.hpp"
#include "mbase/pass/opt/stack_trace_recover.hpp"

#include <algorithm>

namespace mjt::base {

//── PassNameRegistry ──────────────────────────────────────────

PassNameRegistry& PassNameRegistry::instance()
{
    static PassNameRegistry registry;
    return registry;
}

PassNameRegistry& PassNameRegistry::registerMethodPass(const std::string& name,
                                                       const std::string& description,
                                                       MethodFactory      factory,
                                                       PassStability      stability)
{
    auto& e         = entries_[name];
    e.meta          = {name, description, PassScope::Method, stability};
    e.methodFactory = std::move(factory);
    e.classFactory  = nullptr;
    e.globalFactory = nullptr;
    return *this;
}

PassNameRegistry& PassNameRegistry::registerClassPass(const std::string& name,
                                                      const std::string& description,
                                                      ClassFactory       factory,
                                                      PassStability      stability)
{
    auto& e         = entries_[name];
    e.meta          = {name, description, PassScope::Class, stability};
    e.methodFactory = nullptr;
    e.classFactory  = std::move(factory);
    e.globalFactory = nullptr;
    return *this;
}

PassNameRegistry& PassNameRegistry::registerGlobalPass(const std::string& name,
                                                       const std::string& description,
                                                       GlobalFactory      factory,
                                                       PassStability      stability)
{
    auto& e         = entries_[name];
    e.meta          = {name, description, PassScope::Global, stability};
    e.methodFactory = nullptr;
    e.classFactory  = nullptr;
    e.globalFactory = std::move(factory);
    return *this;
}

bool PassNameRegistry::has(const std::string& name) const
{
    return entries_.find(name) != entries_.end();
}

const PassNameInfo* PassNameRegistry::info(const std::string& name) const
{
    auto it = entries_.find(name);
    if (it == entries_.end()) return nullptr;
    return &it->second.meta;
}

std::vector<PassNameInfo> PassNameRegistry::list() const
{
    std::vector<PassNameInfo> result;
    result.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        result.push_back(entry.meta);
    }
    //CLI list output is part of the MVP user surface; keep it deterministic
    //even though the registry storage is an unordered_map.
    std::sort(result.begin(), result.end(), [](const PassNameInfo& a, const PassNameInfo& b) {
        return a.name < b.name;
    });
    return result;
}

bool PassNameRegistry::applyTo(const std::string& name, PipelineBuilder& builder) const
{
    auto it = entries_.find(name);
    if (it == entries_.end()) return false;
    const Entry& e = it->second;
    switch (e.meta.scope) {
    case PassScope::Method:
        if (e.methodFactory) {
            builder.addMethodPass(e.methodFactory, name);
            return true;
        }
        return false;
    case PassScope::Class:
        if (e.classFactory) {
            builder.addClassPass(e.classFactory, name);
            return true;
        }
        return false;
    case PassScope::Global:
        if (e.globalFactory) {
            builder.addGlobalPass(e.globalFactory, name);
            return true;
        }
        return false;
    }
    return false;
}

//── registerBuiltinPasses ─────────────────────────────────────
//
//名字用真实类名（ConstantFolder、ArithmeticSimplifier 等），与
//pass_registry.cpp 内 phase builder 里 new 出的类一一对应。
//里 DeadCodeCleaner/ControlFlowSimplifier/UnusedMethodRemover
//是示意名：DeadCodeCleaner→SafeDeadCodeRemover，UnusedMethodRemover 当前
//不存在故不注册。help 里会列全可用名。

namespace {
//用宏减少重复样板：每个方法级 pass 一行注册。
#define REG_METHOD(NAME, DESC, CLASS) \
    reg.registerMethodPass(NAME, DESC, [] { return std::make_unique<CLASS>(); })
#define REG_CLASS(NAME, DESC, CLASS) \
    reg.registerClassPass(NAME, DESC, [] { return std::make_unique<CLASS>(); })
#define REG_GLOBAL(NAME, DESC, CLASS) \
    reg.registerGlobalPass(NAME, DESC, [] { return std::make_unique<CLASS>(); })
#define REG_DEFAULT_METHOD(NAME, DESC, CLASS) \
    reg.registerMethodPass(NAME, DESC, [] { return std::make_unique<CLASS>(); }, PassStability::Default)
}   //namespace

//描述统一存 key(mbase.opt.pass.*),由消费方经 mjt::base::tr 渲染,
//保证插件宿主等未 ca::i18n::init 的环境也不泄漏裸 key(见 mbase/i18n.hpp)。
ca::usize registerBuiltinPasses()
{
    auto& reg = PassNameRegistry::instance();
    if (reg.has("ConstantFolder")) return 0;   //幂等

    REG_DEFAULT_METHOD("ArithmeticSimplifier",
                       "mbase.opt.pass.arithmetic_simplifier",
                       ArithmeticSimplifier);
    REG_METHOD("ExpressionSimplifier", "mbase.opt.pass.expression_simplifier", ExpressionSimplifier);
    REG_DEFAULT_METHOD("StackOperationSimplifier",
                       "mbase.opt.pass.stack_operation_simplifier",
                       StackOperationSimplifier);
    REG_METHOD("TypeCastSimplifier", "mbase.opt.pass.type_cast_simplifier", TypeCastSimplifier);
    REG_METHOD("FixPatternSimplifier", "mbase.opt.pass.fix_pattern_simplifier", FixPatternSimplifier);

    REG_DEFAULT_METHOD("NopRemover", "mbase.opt.pass.nop_remover", NopRemover);
    REG_DEFAULT_METHOD("UnusedLabelRemover", "mbase.opt.pass.unused_label_remover", UnusedLabelRemover);
    REG_CLASS("IllegalAccessRemover", "mbase.opt.pass.illegal_access_remover", IllegalAccessRemover);
    REG_CLASS("MetaInfoRemover", "mbase.opt.pass.meta_info_remover", MetaInfoRemover);
    REG_METHOD("InvalidTryCatchRemover", "mbase.opt.pass.invalid_try_catch_remover", InvalidTryCatchRemover);
    REG_CLASS("SpecificAnnotationRemover", "mbase.opt.pass.specific_annotation_remover", SpecificAnnotationRemover);
    REG_GLOBAL("IllegalClassFileCleaner", "mbase.opt.pass.illegal_class_file_cleaner", IllegalClassFileCleaner);

    REG_CLASS("IllegalAnnotationRemover", "mbase.opt.pass.illegal_annotation_remover", IllegalAnnotationRemover);
    REG_CLASS("IllegalSignatureRemover", "mbase.opt.pass.illegal_signature_remover", IllegalSignatureRemover);
    REG_CLASS("IllegalTypeAnnotationRemover",
              "mbase.opt.pass.illegal_type_annotation_remover",
              IllegalTypeAnnotationRemover);
    REG_METHOD("IllegalVarargsRemover", "mbase.opt.pass.illegal_varargs_remover", IllegalVarargsRemover);
    REG_CLASS("IllegalSourceInfoRemover",
              "mbase.opt.pass.illegal_source_info_remover",
              IllegalSourceInfoRemover);
    REG_CLASS("UnknownAttributeCleaner", "mbase.opt.pass.unknown_attribute_cleaner", UnknownAttributeCleaner);
    REG_METHOD("IllegalLocalVariableInfoRemover",
               "mbase.opt.pass.illegal_local_variable_info_remover",
               IllegalLocalVariableInfoRemover);

    REG_CLASS("FinalRemover", "mbase.opt.pass.final_remover", FinalRemover);
    REG_CLASS("KotlinMetadataRemover",
              "mbase.opt.pass.kotlin_metadata_remover",
              KotlinMetadataRemover);
    REG_CLASS("SourceInfoRemover", "mbase.opt.pass.source_info_remover", SourceInfoRemover);
    REG_METHOD("LineNumberRemover", "mbase.opt.pass.line_number_remover", LineNumberRemover);
    REG_CLASS("SyntheticBridgeRemover", "mbase.opt.pass.synthetic_bridge_remover", SyntheticBridgeRemover);
    REG_METHOD("LocalVariableInfoRemover", "mbase.opt.pass.local_variable_info_remover", LocalVariableInfoRemover);
    REG_METHOD("ParametersInfoRemover", "mbase.opt.pass.parameters_info_remover", ParametersInfoRemover);
    REG_METHOD("ExceptionInfoRemover", "mbase.opt.pass.exception_info_remover", ExceptionInfoRemover);
    REG_METHOD("LongIndyNameOptimizer", "mbase.opt.pass.long_indy_name_optimizer", LongIndyNameOptimizer);
    REG_CLASS("SignatureRemover", "mbase.opt.pass.signature_remover", SignatureRemover);

    REG_DEFAULT_METHOD("ConstantFolder", "mbase.opt.pass.constant_folder", ConstantFolder);
    REG_METHOD("SimpleConstantFolder",
               "mbase.opt.pass.simple_constant_folder",
               SimpleConstantFolder);
    REG_METHOD("LocalConstantFolder",
               "mbase.opt.pass.local_constant_folder",
               LocalConstantFolder);
    REG_METHOD("ConsLocalVarOptimizer", "mbase.opt.pass.cons_local_var_optimizer", ConsLocalVarOptimizer);
    REG_METHOD("LabelConsLocalVarPropagator",
               "mbase.opt.pass.label_cons_local_var_propagator",
               LabelConsLocalVarPropagator);
    REG_GLOBAL("ConsReturnPropagator", "mbase.opt.pass.cons_return_propagator", ConsReturnPropagator);
    REG_GLOBAL("ConsStaticFieldReplacer",
               "mbase.opt.pass.cons_static_field_replacer",
               ConsStaticFieldReplacer);

    REG_DEFAULT_METHOD("ConstantSwitchFolder", "mbase.opt.pass.constant_switch_folder", ConstantSwitchFolder);
    REG_METHOD("ConstantStateMachineEliminator", "mbase.opt.pass.constant_state_machine_eliminator", ConstantStateMachineEliminator);
    REG_DEFAULT_METHOD("ConsJumpOptimizer", "mbase.opt.pass.cons_jump_optimizer", ConsJumpOptimizer);
    REG_DEFAULT_METHOD("RedundantJumpRemover", "mbase.opt.pass.redundant_jump_remover", RedundantJumpRemover);
    REG_DEFAULT_METHOD("SafeDeadCodeRemover", "mbase.opt.pass.safe_dead_code_remover", SafeDeadCodeRemover);
    REG_METHOD("ImpossibleJumpEliminater", "mbase.opt.pass.impossible_jump_eliminater", ImpossibleJumpEliminater);
    REG_METHOD("RedundantTryCatchRemover", "mbase.opt.pass.redundant_try_catch_remover", RedundantTryCatchRemover);
    REG_METHOD("TryCatchRepair", "mbase.opt.pass.try_catch_repair", TryCatchRepair);
    REG_METHOD("GotoRearranger", "mbase.opt.pass.goto_rearranger", GotoRearranger);

    REG_METHOD("DeadLocalStoreRemover", "mbase.opt.pass.dead_local_store_remover", DeadLocalStoreRemover);
    REG_DEFAULT_METHOD("PopUnUsedLocalVariables", "mbase.opt.pass.pop_unused_local_variables", PopUnUsedLocalVariables);
    REG_DEFAULT_METHOD("UselessPopCleaner", "mbase.opt.pass.useless_pop_cleaner", UselessPopCleaner);
    REG_METHOD("StackOperationExpander",
               "mbase.opt.pass.stack_operation_expander",
               StackOperationExpander);
    REG_GLOBAL("RedundantInvokeRemover", "mbase.opt.pass.redundant_invoke_remover", RedundantInvokeRemover);
    REG_METHOD("CommutativeOperatorReorder", "mbase.opt.pass.commutative_operator_reorder", CommutativeOperatorReorder);
    REG_METHOD("RedundantLoadValEliminator", "mbase.opt.pass.redundant_load_val_eliminator", RedundantLoadValEliminator);
    REG_METHOD("NoLoadStoreEliminater", "mbase.opt.pass.no_load_store_eliminater", NoLoadStoreEliminater);
    REG_METHOD("RedundantStoreEliminater", "mbase.opt.pass.redundant_store_eliminater", RedundantStoreEliminater);
    REG_METHOD("ArithmeticInsnRearranger", "mbase.opt.pass.arithmetic_insn_rearranger", ArithmeticInsnRearranger);
    REG_METHOD("UnusedResultCallEliminater", "mbase.opt.pass.unused_result_call_eliminater", UnusedResultCallEliminater);
    REG_METHOD("UnusedCheckCleaner", "mbase.opt.pass.unused_check_cleaner", UnusedCheckCleaner);

    REG_METHOD("CharArrToStrSimplifier", "mbase.opt.pass.char_arr_to_str_simplifier", CharArrToStrSimplifier);
    REG_METHOD("ConstCharArrayStringFolder",
               "mbase.opt.pass.const_char_array_string_folder",
               ConstCharArrayStringFolder);
    REG_METHOD("BoxUnboxEliminator", "mbase.opt.pass.box_unbox_eliminator", BoxUnboxEliminator);
    REG_METHOD("RandomSeedResolver", "mbase.opt.pass.random_seed_resolver", RandomSeedResolver);
    REG_METHOD("StdConsFunctionOptimizer", "mbase.opt.pass.std_cons_function_optimizer", StdConsFunctionOptimizer);

    REG_METHOD("LocalConstantPropagation", "mbase.opt.pass.local_constant_propagation", LocalConstantPropagation);

    REG_METHOD("ContinuousLabelMerger", "mbase.opt.pass.continuous_label_merger", ContinuousLabelMerger);
    REG_METHOD("LabelDeadVarStoreEliminator",
               "mbase.opt.pass.label_dead_var_store_eliminator",
               LabelDeadVarStoreEliminator);
    REG_CLASS("EnumCastCheckRemover", "mbase.opt.pass.enum_cast_check_remover", EnumCastCheckRemover);
    REG_METHOD("UselessTryCatchRemover", "mbase.opt.pass.useless_try_catch_remover", UselessTryCatchRemover);
    REG_METHOD("SelfCaughtThrowRewriter",
               "mbase.opt.pass.self_caught_throw_rewriter",
               SelfCaughtThrowRewriter);
    REG_METHOD("DeadSideEffectFreeCallRemover",
               "mbase.opt.pass.dead_side_effect_free_call_remover",
               DeadSideEffectFreeCallRemover);
    REG_CLASS("UnusedMethodRemover", "mbase.opt.pass.unused_method_remover", UnusedMethodRemover);
    REG_GLOBAL("EmptyClassRemover", "mbase.opt.pass.empty_class_remover", EmptyClassRemover);
    REG_GLOBAL("UnusedClassRemover", "mbase.opt.pass.unused_class_remover", UnusedClassRemover);
    REG_GLOBAL("UnusedFieldRemover", "mbase.opt.pass.unused_field_remover", UnusedFieldRemover);
    REG_GLOBAL("GlobalUnusedMethodRemover",
               "mbase.opt.pass.global_unused_method_remover",
               GlobalUnusedMethodRemover);
    REG_CLASS("StaticFieldDefaultInliner",
              "mbase.opt.pass.static_field_default_inliner",
              StaticFieldDefaultInliner);
    REG_GLOBAL("GetterInliner", "mbase.opt.pass.getter_inliner", GetterInliner);
    REG_GLOBAL("ShortMethodInliner", "mbase.opt.pass.short_method_inliner", ShortMethodInliner);
    REG_GLOBAL("ArgForwardMethodInliner", "mbase.opt.pass.arg_forward_method_inliner", ArgForwardMethodInliner);
    REG_GLOBAL("PureFunctionAnnotator", "mbase.opt.pass.pure_function_annotator", PureFunctionAnnotator);
    REG_GLOBAL("PureFunctionLocalizer", "mbase.opt.pass.pure_function_localizer", PureFunctionLocalizer);
    REG_GLOBAL("LocalPureFunctionInliner", "mbase.opt.pass.local_pure_function_inliner", LocalPureFunctionInliner);
    REG_GLOBAL("MethodInliner", "mbase.opt.pass.method_inliner", MethodInliner);
    REG_GLOBAL("GlobalStrPoolOptimizer", "mbase.opt.pass.global_str_pool_optimizer", GlobalStrPoolOptimizer);
    REG_GLOBAL("ToolClassCombiner", "mbase.opt.pass.tool_class_combiner", ToolClassCombiner);

    REG_CLASS("ClassMemberSorter", "mbase.opt.pass.class_member_sorter", ClassMemberSorter);
    REG_CLASS("StackTraceRecover", "mbase.opt.pass.stack_trace_recover", StackTraceRecover);
    REG_METHOD("LocalVariableInfoRebuilder", "mbase.opt.pass.local_variable_info_rebuilder", LocalVariableInfoRebuilder);
    REG_GLOBAL("AccessModifierTightener", "mbase.opt.pass.access_modifier_tightener", AccessModifierTightener);
    REG_GLOBAL("ClassRenamer", "mbase.opt.pass.class_renamer", ClassRenamer);

    return reg.list().size();
}

}   //namespace mjt::base
