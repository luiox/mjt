#include "mbase/pass/opt/class_member_sorter.hpp"
#include "mbase/pass/opt/recover_detail.hpp"

namespace mjt::base {

PreservedAnalyses ClassMemberSorter::run(ClassNode& classNode, ClassAnalysisManager& cam)
{
    (void)cam;

    //字段排序：先按 desc，再按 name 字典序（对齐 Java f1.desc.compareTo(f2.desc)）
    std::sort(classNode.fields.begin(),
              classNode.fields.end(),
              [](const FieldNode* f1, const FieldNode* f2) {
                  int c = f1->desc.compare(f2->desc);
                  if (c != 0) return c < 0;
                  return f1->name.compare(f2->name) < 0;
              });

    //方法排序：先抽出 <clinit>/<init>，其余按 name 字典序
    MethodNode*              clinit = nullptr;
    std::vector<MethodNode*> inits;
    std::vector<MethodNode*> newMethods;
    for (auto* method : classNode.methods) {
        if (MethodUtil::isClinit(*method)) {
            clinit = method;
        }
        else if (MethodUtil::isConstructor(*method)) {
            inits.push_back(method);
        }
        else {
            newMethods.push_back(method);
        }
    }
    //字典序（对齐 Java newMethods.sort(comparing m -> m.name)）
    std::sort(newMethods.begin(), newMethods.end(), [](const MethodNode* a, const MethodNode* b) {
        return a->name.compare(b->name) < 0;
    });
    //<clinit> 插到最前，<init> 放其后（对齐 Java addFirst(clinit) + addAll(init)）
    if (clinit != nullptr) {
        newMethods.insert(newMethods.begin(), clinit);
    }
    for (auto* init : inits) {
        newMethods.push_back(init);
    }

    classNode.methods = std::move(newMethods);
    return PreservedAnalyses::none();
}

}   //namespace mjt::base
