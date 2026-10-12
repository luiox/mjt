#include "mbase/spi/class_file_sanitize_gate.hpp"

#include "jbytecode/core/class_file_sanitizer.hpp"
#include "libca/core/tag_cast.hpp"
#include "mbase/model/resource_model.hpp"

#include <stdexcept>

namespace mjt::base {

PreservedAnalyses ClassFileSanitizeGate::run(ResourceContainer&     container,
                                             GlobalAnalysisManager& am)
{
    (void)am;
    auto&     resources = container.getResources();
    ca::usize rejected  = 0;

    for (auto it = resources.begin(); it != resources.end();) {
        if (!it->second || !ca::core::tag_cast::isa<ClassResource>(it->second.get())) {
            ++it;
            continue;
        }
        auto*       cls   = ca::core::tag_cast::cast<ClassResource>(it->second.get());
        const auto& bytes = cls->get();
        //空 bytes 资源沿既有契约放行（GlobalToClassPassAdaptor 对其原样跳过）。
        if (bytes.empty()) {
            ++it;
            continue;
        }

        auto issues = jbytecode::core::sanitizeClassFile(bytes.data(),
                                                         static_cast<ca::i32>(bytes.size()));
        if (issues.empty()) {
            ++it;
            continue;
        }

        const std::string detail = jbytecode::core::joinSanitizeIssues(issues).to_std_string();
        if (!options_.perClassFaultIsolation) {
            //默认档位 fail-fast（issue ）：任一阶段失败即上抛，作业级原子。
            throw std::runtime_error("ClassFileSanitizeGate: failed at '" + it->first +
                                     "': " + detail);
        }
        if (options_.faultReport) {
            options_.faultReport->add({/*scope*/ "class", /*target*/ it->first,
                                       /*reason*/ "sanitize_error", /*detail*/ detail});
        }
        it = resources.erase(it);   //剔除：下游 pass 与导出均不再见该类。
        ++rejected;
    }

    return rejected == 0 ? PreservedAnalyses::all() : PreservedAnalyses::none();
}

}   //namespace mjt::base
