#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <libca/str/str.hpp>
#include "jbytecode/core/frame.hpp"

namespace mjt {
namespace jbytecode = ::jbytecode;
}

namespace mjt::base {

class CommonSuperClassProvider : public jbytecode::core::ICommonSuperClassProvider
{
public:
    void setSuperClassMap(std::unordered_map<ca::str::Utf8StringRef, ca::str::Utf8StringRef> map);
    void setFallback(ca::str::Utf8StringRef fallback);

    ///Scan a directory (recursively) for .jar/.jmod/.zip files and build the parent-class map.
    ///@param dirPath root directory to scan (e.g. path to jmods/ or lib/)
    void scan(const std::string& dirPath);

    ///Add all classes from a JAR/ZIP/JMOD archive to the class hierarchy.
    ///Useful for adding third-party libraries.
    void addJar(const std::string& jarPath);

    ///Add a single class→super mapping (e.g. from input JAR)
    void addClass(ca::str::Utf8StringRef className, ca::str::Utf8StringRef superName);

    std::vector<ca::str::Utf8StringRef> getSuperClasses(ca::str::Utf8StringRef type);
    ca::str::Utf8StringRef              getCommonSuperClass(const ca::str::Utf8StringRef& type1,
                                                            const ca::str::Utf8StringRef& type2) override;
    size_t                              getClassCount() const { return superClassMap_.size(); }

private:
    void scanArchive(const std::string& archivePath);

    //类名均为 JVM internal name，统一 intern 到 nameArena_，以 Utf8StringRef 做 map key/value
    //（std::hash<Utf8StringRef> 已特化，查找无需再分配）。
    //-- arena 死亡点声明（memory-governance-plan §5-P1③）--
    //本成员持有本 provider 私有的 Utf8StringArena：scan/addJar/addClass intern 的
    //superClassMap_ 键值，以及 getSuperClasses 返回的祖先链视图、
    //getCommonSuperClass 返回的公共父类视图均指向本 arena（fallback_ 的
    //"java/lang/Object" 为静态字面量，不占 arena）。
    //视图寿命 = 本 provider 寿命；死亡点：随持有者析构——当前构造点均为
    //unique_ptr/栈对象：WorkspaceBase::runPipeline（workspace.cpp，随管道请求
    //函数作用域结束）、mjt prepareJdk（随命令作用域结束）、mjt-optimizer
    //main、单测。COMPUTE_FRAMES 的 ClassWriter 经 typeResolver 查询结果当场
    //写入 StackMapTable 字节，不留视图。
    //消费者不得把视图带过本对象的死亡点；需跨期持有则 intern 进自己的 arena 或深拷贝。
    ca::str::Utf8StringArena                                           nameArena_;
    std::unordered_map<ca::str::Utf8StringRef, ca::str::Utf8StringRef> superClassMap_;
    ca::str::Utf8StringRef fallback_ = ca::str::Utf8StringRef::from_cstr("java/lang/Object");

    ///getCommonSuperClass 结果 cache（per-provider，跨类共享）。
    ///key 为归一化的 (min,max) 类名对（(t1,t2) 与 (t2,t1) 等价）。
    ///COMPUTE_FRAMES 对每个类用独立 SymbolTable（mergetype memo 不跨类），多类场景下
    ///相同类型对（如 swing 继承链上的 JPanel vs JComponent）被不同类反复查询，cache
    ///命中后避免重复 getSuperClasses 链遍历，缓解 的 fixpoint 调用量膨胀。
    std::map<std::pair<std::string, std::string>, std::string> commonSuperCache_;
};

}   //namespace mjt::base
