#include "mbase/pass/opt/illegal_class_file_cleaner.hpp"
#include "mbase/pass/opt/clean_pass_detail.hpp"
#include "mbase/spi/class_resource_pending_commit.hpp"

namespace mjt::base {

PreservedAnalyses IllegalClassFileCleaner::run(ResourceContainer&     container,
                                               GlobalAnalysisManager& am)
{
    (void)am;
    bool                      modified = false;
    ClassResourcePendingCommit pending;

    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!resource || !ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(resource.get());

        try {
            auto classFile = jclassfile::io::ClassFileReader().read(cls->get());
            jclassfile::transform::IllegalStrippingTransformer(classFile).transform();
            auto output = jclassfile::io::ClassFileWriter().write(classFile);
            if (output != cls->get()) {
                //两阶段提交：写回推迟到循环结束，任何类抛出时
                //已处理的类保持原样（作业级原子性）。
                pending.stage(cls, std::move(output));
                modified = true;
            }
        }
        catch (const std::exception&) {
            //Keep the original bytes when the low-level classfile layer cannot
            //parse or write this resource. Later tolerant readers may still use it.
        }
        catch (...) {
        }
    }

    pending.commit();
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
