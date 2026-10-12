#include "mbase/spi/pass_pipeline.hpp"

#include "mbase/analysis/method_frame_analysis.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/core/tag_cast.hpp"
#include "mbase/model/resource_model.hpp"
#include "mbase/spi/progress_sink.hpp"
#include "mbase/util/class_name_util.hpp"

#include <cstdio>
#include <cstdlib>
#include "libca/log/log_macros.hpp"

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

PreservedAnalyses GlobalToClassPassAdaptor::run(ResourceContainer&     container,
                                                GlobalAnalysisManager& gam)
{
    //job 级 arena 只归 GlobalAnalysisManager 的全局分析使用（合法作业级驻留）。
    //类循环内的一切分配（IR、reader/writer 中间物、AM 分析数据）一律走下面的
    //每类作用域 classArena：garena 语义是整池释放、无逐条 free，若类级数据借用
    //job 池，"AM 每类一造"的析构不归还任何内存，大罐（万类）会累积到 GB 级。

    auto pa = PreservedAnalyses::all();

    //进度反馈（issue ）：先数 ClassResource 总数，循环里计数刷新。
    //total 在循环前一次性算出（container 已 import 完毕，非流式）。
    int classTotal = 0;
    if (progress_) {
        for (const auto& [uri, resource] : container.getResources()) {
            if (ca::core::tag_cast::isa<ClassResource>(resource.get())) ++classTotal;
        }
    }
    int classIndex = 0;

    //诊断探针（临时，MJT_ARENA_PROBE=1 开启）：每 25 类打印每类 arena 足迹、
    //job arena 增长与 pending 字节，用于定位大罐内存爬升的归属层级。
    const bool arenaProbe = std::getenv("MJT_ARENA_PROBE") != nullptr;

    //作业级原子性（dev_plan C）：两阶段提交。阶段一逐 class 读+跑+序列化到
    //pending，任何失败抛出且容器保持原样（无部分写回）；阶段二全部成功后统一
    //提交。前序 class 的进度反馈照常发出（只读过程），但写回被推迟到提交点。
    std::vector<std::pair<ClassResource*, std::vector<ca::u8>>> pending;
    pending.reserve(container.getResources().size());

    for (auto& [uri, resource] : container.getResources()) {
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto*       cls   = ca::core::tag_cast::cast<ClassResource>(resource.get());
        const auto& bytes = cls->get();
        if (bytes.empty()) continue;

        //每类作用域 classArena：迭代结束整体释放（garena 析构=整池 reset 语义）。
        //ClassNode IR、reader/writer 中间物与 AM 分析数据全部落此池。
        //类间无指针跨越——P1② 纪律下 AM 每类一造；pending 只存序列化字节
        //（toByteArray 返回独立 vector），faultReport 条目均为 std::string 拷贝。
        //method 级 AM 经工厂构造：构造即注册内置分析，供 method pass 经 am.getResult
        //复用。mam 先于 cam 构造，析构序 cam→mam，pinned proxy 里的 &mam 在 cam
        //析构时仍有效。pinned proxy 不受 class pass 的 invalidate 影响，
        //ClassToMethodPassAdaptor 始终可经 getPinned 取到。
        ca::str::Utf8StringArena classArena;
        MethodAnalysisManager mam = makeMethodAnalysisManager(classArena);
        ClassAnalysisManager  cam(classArena);
        cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});

        ++classIndex;   //1-based
        if (progress_) {
            ProgressSink::State s;
            s.phase = ProgressSink::Phase::Pipeline;
            s.total = classTotal;
            s.index = classIndex;
            //uri 是多字节 UTF-8，按 Utf8Iterator 码点转义（见 class_name_util）。
            s.currentName = escapeClassName(ca::str::Utf8StringRef::from_data(
                reinterpret_cast<const ca::u8*>(uri.data()), uri.size()));
            //PR-A：pass 名未透传（passIndex/passTotal 留 0，进度行不显 pass 段）。
            progress_->update(std::move(s));
        }

        //失败语义分档（issue ）：默认档位任一阶段失败即向上抛出（作业级原子，
        //dev_plan C），由于写回推迟到提交点，前序 class 也未写回——整个作业保持原子。
        //perClassFaultIsolation 档位仅捕获 std::exception：失败类记录诊断（reason 按
        //失败阶段 reader/pass/writer 区分）后跳过——不进 pending，原 bytes 保持原样，
        //其余类继续；作业结束照常统一提交全部成功类。非 std 异常两档都不吞，照常传播。
        const char* failStage = "reader_error";
        try {
            //读字节 → ClassNode
            ClassNode   classNode;
            ClassReader reader(classArena, bytes.data(), static_cast<ca::i32>(bytes.size()));
            int         readerFlags = 0;
            if ((classWriterFlags_ & ClassWriter::COMPUTE_FRAMES) != 0) {
                readerFlags |= ClassReader::SKIP_FRAMES;
            }
            reader.accept(&classNode, readerFlags);

            //跑 class pipeline（ClassToMethodPassAdaptor 内部经 pinned proxy 取 mam）
            failStage = "pass_error";
            PreservedAnalyses classPA = cpm_.run(classNode, cam);
            pa.intersect(classPA);

            //序列化到 pending：仅当 class pipeline 改动了 IR 时重新序列化。changed
            //与分析保留集合解耦，不再把「保留全部分析」误当成「未改 IR」（见
            //PreservedAnalyses）。
            failStage = "writer_error";
            if (classPA.changed()) {
                ClassWriter writer(classArena, classWriterFlags_);
                if (typeResolver_) writer.setTypeResolver(typeResolver_);
                classNode.accept(&writer);
                pending.emplace_back(cls, writer.toByteArray());
            }
        }
        catch (const std::exception& e) {
            if (!options_.perClassFaultIsolation) {
                throw std::runtime_error("GlobalToClassPassAdaptor: failed at '" + uri + "': " +
                                         e.what());
            }
            if (options_.faultReport) {
                options_.faultReport->add({/*scope*/ "class", uri, failStage, e.what()});
            }
            continue;   //跳过失败类：不写回，其余类继续。
        }
        catch (...) {
            //非 std 异常不吞（issue ）：默认档位转 runtime_error 保持原语义；
            //容错档位同样向上传播终止作业（不静默跳过来源不明的异常）。
            throw std::runtime_error("GlobalToClassPassAdaptor: failed at '" + uri +
                                     "': unknown exception");
        }

        if (arenaProbe && (classIndex <= 3 || classIndex % 25 == 0)) {
            ca::usize pendingBytes = 0;
            for (const auto& p : pending) pendingBytes += p.second.size();
            CA_LOG_DEBUG("[arena-probe] class={} classArena={:.1f}MB jobArena={:.1f}MB pending={:.1f}MB",
                        classIndex, static_cast<double>(classArena.total_bytes()) / 1048576.0, static_cast<double>(gam.arena().total_bytes()) / 1048576.0, static_cast<double>(pendingBytes) / 1048576.0);
        }
    }

    //阶段二：全部成功，统一提交写回。
    for (auto& [cls, newBytes] : pending) {
        cls->set(std::move(newBytes));
    }

    return pa;
}

}   //namespace mjt::base
