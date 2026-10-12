#include "mbase/pass/opt/unused_label_remover.hpp"
#include "mbase/pass/opt/clean_pass_detail.hpp"

namespace mjt::base {

PreservedAnalyses UnusedLabelRemover::run(jbytecode::tree::MethodNode& methodNode,
                                          MethodAnalysisManager&       context)
{
    (void)context;

    std::unordered_set<Label*> usedLabels;
    collectReferencedLabels(methodNode, usedLabels);

    bool  modified = false;
    auto* node     = methodNode.instructions.getFirst();
    while (node) {
        auto* next = node->getNext();
        if (node->getType() == AbstractInsnNode::LABEL) {
            auto* labelNode = ca::core::tag_cast::cast<LabelNode*>(node);
            if (usedLabels.find(labelNode->label) == usedLabels.end()) {
                methodNode.instructions.removeAndDelete(node);
                modified = true;
            }
        }
        node = next;
    }

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
