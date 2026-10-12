#include "mbase/spi/pass_manager.hpp"

#include "mbase/spi/pass_pipeline.hpp"   //MethodAnalysisManagerProxy / CurrentClassContext / InnerAMProxyResult（#PASS-3）

#include "mbase/analysis/method_frame_analysis.hpp"
#include "mbase/analysis/method_effect_analysis.hpp"
#include "mbase/analysis/alias_analysis.hpp"
#include "mbase/analysis/exception_flow_analysis.hpp"
#include "mbase/asm/classpath/call_graph.hpp"
#include "mbase/asm/classpath/resolved_call_graph.hpp"
#include "mbase/hierarchy/class_hierarchy_analysis.hpp"
#include "mbase/hierarchy/method_hierarchy_analysis.hpp"
#include "mbase/hierarchy/field_hierarchy_analysis.hpp"
#include "mbase/reference/global_reachability.hpp"
#include "mbase/reference/reference_index_analysis.hpp"

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

//── runAsmPipeline（新框架：PassManager 模板 + AnalysisManager）────

std::vector<ca::u8> runAsmPipeline(const std::vector<ca::u8>& classBytes,
                                   ClassPassManager* classManager, MethodPassManager* methodManager,
                                   int classWriterFlags, ICommonSuperClassProvider* typeResolver)
{

    AsmGlobalContext globalContext;
    auto&            arena = globalContext.stringArena();

    //各级 AnalysisManager 借用同一 arena（单 arena 贯穿作业）。
    //method 级 AM 经工厂构造：构造即注册内置分析，供 method pass 经 am.getResult 复用。
    ClassAnalysisManager  cam(arena);
    MethodAnalysisManager mam = makeMethodAnalysisManager(arena);

    ClassNode classNode;

    ///#PASS-3：单类入口前置 pin，对齐 jar 路径（runJarPipeline / ClassToMethodPassAdaptor）。
    ///原先单类路径不 pin proxy，导致：
    ///1) classManager 含 ClassToMethodPassAdaptor 时抛 logic_error（adaptor 取不到 proxy）；
    ///2) 直接经 methodManager->run 的方法 pass 取不到 CurrentClassContext，静默降级
    ///（如 LocalVariableInfoRebuilder 的 thisDescStr 留空）。
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});
    mam.setPinned<CurrentClassContext>({&classNode});

    ClassReader reader(arena, classBytes.data(), classBytes.size());
    int         readerFlags = 0;
    if ((classWriterFlags & ClassWriter::COMPUTE_FRAMES) != 0) {
        readerFlags |= ClassReader::SKIP_FRAMES;
    }
    reader.accept(&classNode, readerFlags);

    //类级 pass
    if (classManager && !classManager->empty()) {
        classManager->run(classNode, cam);
    }

    //方法级 pass：对每个 method 跑一遍方法流水线
    if (methodManager && !methodManager->empty()) {
        for (auto* method : classNode.methods) {
            if (method) methodManager->run(*method, mam);
        }
    }

    ClassWriter writer(arena, classWriterFlags);
    if (typeResolver) writer.setTypeResolver(typeResolver);
    classNode.accept(&writer);
    return writer.toByteArray();
}

//── runJarPipeline：ResourceContainer 级全局流水线 ──

void runJarPipeline(ResourceContainer& container, GlobalPassManager& gpm,
                    AsmGlobalContext& globalContext, int classWriterFlags,
                    ICommonSuperClassProvider* typeResolver)
{

    auto&                 arena = globalContext.stringArena();
    GlobalAnalysisManager gam(arena);
    gam.setTypeResolver(typeResolver);

    //ClassAnalysisManagerProxy 尚无跨作用域注入/撤销协议，因此不注册为可用分析。
    //registerAnalysis 本身幂等（重复注册直接返回），无需外层 isRegistered 守卫。
    gam.registerAnalysis<ReferenceIndexAnalysis>();
    gam.registerAnalysis<CallGraphAnalysis>();
    gam.registerAnalysis<ClassHierarchyAnalysis>();
    gam.registerAnalysis<MethodHierarchyAnalysis>();
    gam.registerAnalysis<ResolvedCallGraphAnalysis>();
    gam.registerAnalysis<GlobalReachabilityAnalysis>();
    gam.registerAnalysis<ResolvedGlobalReachabilityAnalysis>();
    gam.registerAnalysis<MethodEffectAnalysis>();
    gam.registerAnalysis<AliasAnalysisResult>();
    gam.registerAnalysis<ExceptionFlowAnalysis>();
    gam.registerAnalysis<FieldHierarchyAnalysis>();

    gpm.run(container, gam);

    //typeResolver 和 classWriterFlags 已通过 GlobalToClassPassAdaptor
    //构造参数传入，无需在此额外处理。
    (void)classWriterFlags;
    (void)typeResolver;
}

}   //namespace mjt::base
