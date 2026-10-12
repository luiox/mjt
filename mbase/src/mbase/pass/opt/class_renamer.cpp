#include "mbase/pass/opt/class_renamer.hpp"
#include "mbase/asm/remapper/member_remapper.hpp"
#include "mbase/pass/opt/recover_detail.hpp"

namespace mjt::base {

PreservedAnalyses ClassRenamer::run(ResourceContainer& container, GlobalAnalysisManager& gam)
{
    auto classes = loadRecoverClasses(container, gam.arena());
    if (classes.empty()) return PreservedAnalyses::all();

    MemberRemapPlan       plan;
    std::set<std::string> allocatedNames;
    int                   id = 0;

    for (const auto& loaded : classes) {
        auto oldName = loaded.classNode->name;
        const std::string base = renamedClassBase(oldName.to_std_string());
        std::string       mappedName;
        do {
            mappedName = base + std::to_string(id++);
        } while (!allocatedNames.insert(mappedName).second);
        plan.mapClass(oldName, ca::str::Utf8StringRef::from_cstr(mappedName.c_str()));
    }

    struct UpdatedClass
    {
        std::string         oldUri;
        std::string         newUri;
        std::vector<ca::u8> bytes;
    };
    std::vector<UpdatedClass> updated;
    updated.reserve(classes.size());

    for (auto& loaded : classes) {
        if (!loaded.classNode) continue;
        if (!applyMemberRemapPlan(*loaded.classNode, gam.arena(), plan)) continue;
        const std::string newName = loaded.classNode->name.to_std_string();
        updated.push_back({loaded.uri,
                           newName + ".class",
                           writeRecoverClassNode(gam.arena(), *loaded.classNode)});
    }

    if (updated.empty()) return PreservedAnalyses::all();

    auto& resources = container.getResources();
    for (const auto& item : updated) {
        resources.erase(item.oldUri);
    }
    for (auto& item : updated) {
        resources[item.newUri] =
            std::make_unique<ClassResource>(item.newUri, std::move(item.bytes));
    }

    return PreservedAnalyses::none();
}

}   //namespace mjt::base
