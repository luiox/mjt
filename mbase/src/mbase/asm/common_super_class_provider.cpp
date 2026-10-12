#include "common_super_class_provider.hpp"

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/jar/jar_reader.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <fstream>
#include <unordered_set>

namespace mjt::base {
using namespace ::jbytecode::core;

void CommonSuperClassProvider::scan(const std::string& dirPath)
{
    namespace fs = std::filesystem;
    if (!fs::exists(dirPath)) return;
    for (auto& entry : fs::recursive_directory_iterator(dirPath)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".jar" || ext == ".zip" || ext == ".jmod") {
            scanArchive(entry.path().string());
        }
    }
}

void CommonSuperClassProvider::addJar(const std::string& jarPath)
{
    scanArchive(jarPath);
}

void CommonSuperClassProvider::scanArchive(const std::string& archivePath)
{
    try {
        JvmJarReader reader(archivePath);
        reader.read([&](const std::string& name, const std::vector<ca::u8>& data) {
            if (name.size() < 6) return;
            if (name.substr(name.size() - 6) != ".class") return;
            if (name.find("module-info") != std::string::npos) return;
            try {
                ca::str::Utf8StringArena entryArena;
                ClassReader              cr(entryArena, data.data(), data.size());
                auto                     clsName   = cr.getClassName();
                auto                     superName = cr.getSuperName();
                if (!clsName.is_empty() && !superName.is_empty()) {
                    //intern 到本 provider 的 arena，以 Utf8StringRef 存入 map。
                    superClassMap_.try_emplace(nameArena_.intern(clsName),
                                               nameArena_.intern(superName));
                }
            }
            catch (...) {
                //skip corrupt entries
            }
        });
    }
    catch (...) {
        //skip corrupt archives
    }
}

void CommonSuperClassProvider::setSuperClassMap(
    std::unordered_map<ca::str::Utf8StringRef, ca::str::Utf8StringRef> map)
{
    superClassMap_ = std::move(map);
}

void CommonSuperClassProvider::addClass(ca::str::Utf8StringRef className,
                                        ca::str::Utf8StringRef superName)
{
    if (!className.is_empty() && !superName.is_empty()) {
        superClassMap_.try_emplace(nameArena_.intern(className), nameArena_.intern(superName));
    }
}

void CommonSuperClassProvider::setFallback(ca::str::Utf8StringRef fallback)
{
    fallback_ = nameArena_.intern(fallback);
}

std::vector<ca::str::Utf8StringRef> CommonSuperClassProvider::getSuperClasses(
    ca::str::Utf8StringRef type)
{
    std::vector<ca::str::Utf8StringRef> result;
    ca::str::Utf8StringRef              current = type;

    //环检测：继承链 A→B→A（畸形 class/混淆产物）会让朴素上升遍历死循环，
    //COMPUTE_FRAMES 因此无限卡死（根因）。重复出现即视为环，
    //截断到 fallback 返回，保证 fixpoint 可收敛。
    std::unordered_set<ca::str::Utf8StringRef> seen;

    while (true) {
        if (!seen.insert(current).second) {
            if (!(current == fallback_)) result.push_back(fallback_);
            break;
        }
        result.push_back(current);
        auto it = superClassMap_.find(current);
        if (it == superClassMap_.end() || it->second.is_empty()) {
            if (!(current == fallback_)) {
                result.push_back(fallback_);
            }
            break;
        }
        current = it->second;
        if (current == ca::str::Utf8StringRef::from_cstr("java/lang/Object")) {
            result.push_back(current);
            break;
        }
    }

    std::reverse(result.begin(), result.end());
    return result;
}

ca::str::Utf8StringRef CommonSuperClassProvider::getCommonSuperClass(
    const ca::str::Utf8StringRef& type1, const ca::str::Utf8StringRef& type2)
{
    //归一化 key：(t1,t2) 与 (t2,t1) 等价，按字典序取 (min,max)。
    std::string s1 = type1.to_std_string();
    std::string s2 = type2.to_std_string();
    auto        key = (s1 < s2) ? std::make_pair(s1, s2) : std::make_pair(s2, s1);
    if (auto it = commonSuperCache_.find(key); it != commonSuperCache_.end()) {
        return nameArena_.intern(it->second.c_str());
    }

    auto chain1 = getSuperClasses(type1);
    auto chain2 = getSuperClasses(type2);

    size_t size = std::min(chain1.size(), chain2.size());
    size_t i    = 0;
    while (i < size && chain1[i] == chain2[i]) {
        ++i;
    }
    ca::str::Utf8StringRef result = (i == 0) ? fallback_ : chain1[i - 1];
    commonSuperCache_[key] = result.to_std_string();
    return result;
}

}   //namespace mjt::base
