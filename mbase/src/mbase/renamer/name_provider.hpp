#pragma once

#include <memory>
#include <vector>

#include "libca/core/status.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/hierarchy/class_hierarchy.hpp"
#include "mbase/hierarchy/method_hierarchy.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/renamer/name_proposal.hpp"

namespace mjt::base {

class RenamerEngine;   //引擎友元通道前置声明（rename_engine.hpp）

///@brief override 闭包组件中的一个声明（值语义快照）。
struct OverrideComponentMember
{
    ca::str::Utf8String owner;
    ca::str::Utf8String name;
    ca::str::Utf8String desc;
};

///@brief 改名引擎的只读上下文视图（luiox/Q2 结论：首版只暴露
///类容器存在性查询 + 类层级查询，不得暴露可变句柄）。
///
///由引擎在 run 入口从容器一次性构建（ClassHierarchy + MethodHierarchy），
///provider 在 `propose` 中只读查询。所有返回的 `Utf8StringRef` 视图
///intern 进本对象私有 arena，寿命 = 本对象寿命。
///
///`RenamerEngine` 是友元：引擎读内部层级做成员枚举与撞名检测；provider
///侧看不到这些内部句柄（ClassEntry 持有可变 ClassNode*，不对 provider 暴露）。
class RenameContext
{
public:
    ///@brief 从容器构建只读视图（解析全部类并建类/方法层级）。
    ///
    ///堆上构造、以 unique_ptr 交付：ClassHierarchy 借用本对象内部 arena 的
    ///地址（indexOf 查询按需 intern），对象一经构造不得移动/拷贝。
    static ca::StatusOr<std::unique_ptr<RenameContext>> build(ResourceContainer& container);

    RenameContext(const RenameContext&)            = delete;
    RenameContext& operator=(const RenameContext&) = delete;
    RenameContext(RenameContext&&)                 = delete;
    RenameContext& operator=(RenameContext&&)      = delete;

    ///@brief 类是否真实存在于容器（phantom/tombstone 不算）。
    bool classExists(ca::str::Utf8StringRef internalName) const;

    ///@brief sub 是否（传递）继承自 super。
    bool isSubType(ca::str::Utf8StringRef sub, ca::str::Utf8StringRef super) const;

    ///@brief 返回直接父类内部名；不存在 / 无 super（java/lang/Object）返回空视图。
    ca::str::Utf8StringRef superClassOf(ca::str::Utf8StringRef internalName) const;

    ///@brief 方法 override 闭包组件（并查集同组全部声明，含自身）。
    ///
    ///目标方法不在容器中时返回 NotFound 错误（调用方自行决定降级为单例组）。
    ///组件可能延伸至容器外（父类/接口不在本 JAR）：容器外声明不参与改名、
    ///也不受阻于引擎消解，由上层（P1+ 工作区/支持库机制）负责。
    ca::StatusOr<std::vector<OverrideComponentMember>> overrideComponentOf(
        ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
        ca::str::Utf8StringRef desc) const;

    ///@brief 方法所属 override 组件根（MethodHierarchy 并查集根下标，run 内稳定）。
    ///
    ///供引擎按组件去重（同一组件被多条建议点名时只活一条）；目标方法不在
    ///容器/未声明时返回 NotFound。
    ca::StatusOr<ca::i32> overrideComponentRootOf(ca::str::Utf8StringRef owner,
                                                  ca::str::Utf8StringRef name,
                                                  ca::str::Utf8StringRef desc) const;

    ///@brief owner 类内是否存在精确 (name, desc) 方法声明（含 <init>/<clinit>）。
    bool methodExists(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                      ca::str::Utf8StringRef desc) const;

private:
    RenameContext() = default;

    //引擎内部通道：读 hierarchy 原始条目（含 ClassNode）做类枚举、成员枚举
    //与撞名检测；provider 侧只走上面的只读查询。
    friend class RenamerEngine;

    //-- arena 死亡点声明（memory-governance-plan §5-P1③）--
    //本成员持有本视图私有的 Utf8StringArena：build 时 ClassHierarchy 把全部
    //类名/super 名/接口名 intern 进来（class_hierarchy.cpp:33），mutable 供
    //const 查询按需 intern（当前查询不 intern，留作扩展）。classExists/
    //superClassOf 返回的视图均指向本 arena。
    //视图寿命 = 本对象寿命；死亡点：随持有者（engine run 局部）析构。
    //消费者不得把视图带过本对象的死亡点。
    mutable ca::str::Utf8StringArena arena_;
    ClassHierarchy                   hierarchy_;
    MethodHierarchy                  methods_;
};

///@brief 名字建议 provider 抽象（luiox/：只产建议流、不触字节码）。
///
///判定与消解都在 `RenamerEngine`：provider 只回答"改成什么"，引擎管
///"怎么改才合法"（Q7 结论）。单条建议非法由引擎逐条拒并记录；provider
///整体硬失败走 Err 返回，该 provider 建议流全弃（记 ProviderError）。
class NameProvider
{
public:
    virtual ~NameProvider() = default;

    ///@brief provider 标签：建议缺省 source、审计流水来源列。
    virtual ca::str::Utf8String name() const = 0;

    ///@brief 产出一批改名建议。context 只读，禁止缓存跨 run 引用（视图寿命 = run）。
    virtual ca::StatusOr<std::vector<NameProposal>> propose(const RenameContext& context) = 0;
};

}   //namespace mjt::base
