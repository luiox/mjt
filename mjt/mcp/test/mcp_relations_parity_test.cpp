#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

/// @brief class_relations DB-first 等价断言（W3 验收门）。
///
/// 同一 fixture 两个会话：A 无支撑挂载 → 分析库直查组合；B 挂一个无关
/// 支撑 jar → 引擎扫描回退（xref/relations 共用守卫）。五组关系面 +
/// sources 全序逐字段相等 = DB 组合与引擎零漂移。
class McpRelationsParityTest : public ::testing::Test
{
protected:
    /// 层次 fixture：Base ← Child ← GrandChild（extends 链）；IFoo ← IBar
    /// （interface extends，父接口在 interfaces[] 列表）；Impl extends Base
    /// + implements IFoo；Unrelated 孤立。jhasm 方言同 classSourceFor
    /// （R"JASM 定版，不声明 <init>——jhasm 方法名不支持尖括号）。
    std::filesystem::path writeHierarchyFixture()
    {
        mjbase::model::ResourceContainer container;
        const auto addClass = [&](const std::string& uri, const std::string& source) {
            mjbase::jasm::JvmJasmService assembler;
            const auto assembled = assembler.assemble(utf16(uri + ".jasm"), utf16(source));
            if (!assembled.isOk())
                throw std::runtime_error("hierarchy fixture did not assemble: " + uri);
            container.addResource(
                uri, std::make_unique<mjbase::model::ClassResource>(uri, assembled.value->bytes));
        };

        addClass("hier/Base.class",
                 R"JASM(.super java/lang/Object
.class public super hier/Base {
  .method public static marker ()I {
    code: {
      iconst_1
      ireturn
    }
  }
}
)JASM");
        addClass("hier/Child.class",
                 R"JASM(.super hier/Base
.class public super hier/Child {
  .method public static marker ()I {
    code: {
      iconst_2
      ireturn
    }
  }
}
)JASM");
        addClass("hier/GrandChild.class",
                 R"JASM(.super hier/Child
.class public super hier/GrandChild {
  .method public static marker ()I {
    code: {
      iconst_3
      ireturn
    }
  }
}
)JASM");
        addClass("hier/IFoo.class",
                 R"JASM(.super java/lang/Object
.class public interface hier/IFoo {
}
)JASM");
        addClass("hier/IBar.class",
                 R"JASM(.super java/lang/Object
.implements hier/IFoo
.class public interface hier/IBar {
}
)JASM");
        addClass("hier/Impl.class",
                 R"JASM(.super hier/Base
.implements hier/IFoo
.class public super hier/Impl {
  .method public static marker ()I {
    code: {
      iconst_4
      ireturn
    }
  }
}
)JASM");
        addClass("hier/Unrelated.class",
                 R"JASM(.super java/lang/Object
.class public super hier/Unrelated {
  .method public static marker ()I {
    code: {
      iconst_5
      ireturn
    }
  }
}
)JASM");

        const auto path = test::getTempOutPath("mjt_mcp_relations_parity_fixture.jar");
        const auto written = mjbase::model::ResourceHelper::exportToJar(container, path.string());
        if (written.is_err()) throw std::runtime_error("hierarchy fixture did not export");
        return path;
    }

    static Json relationsOf(McpSessionHarness& server, const std::string& projectId,
                            const std::string& className)
    {
        const auto result = call(server,
                                 "xref_query",
                                 {{"project_id", projectId},
                                  {"kind", "relations"},
                                  {"class", className}});
        EXPECT_FALSE(result.at("result").at("isError")) << payload(result).dump();
        return payload(result);
    }
};

TEST_F(McpRelationsParityTest, DbComposeMatchesEngineScan)
{
    const auto fixture = writeHierarchyFixture();

    // A：无支撑挂载 → DB 组合路径。
    McpSessionHarness serverDb;
    const auto openedDb = call(serverDb, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(openedDb.at("result").at("isError")) << payload(openedDb).dump();
    const auto dbProject = payload(openedDb).at("project_id").get<std::string>();

    // B：挂无关支撑 jar → 引擎回退路径（xref/relations 共用守卫）。
    McpSessionHarness serverEngine;
    const auto openedEngine =
        call(serverEngine, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(openedEngine.at("result").at("isError")) << payload(openedEngine).dump();
    const auto engineProject = payload(openedEngine).at("project_id").get<std::string>();
    const auto mounted = call(serverEngine,
                              "container_mount",
                              {{"project_id", engineProject},
                               {"role", "support"},
                               {"path", writeFixture().string()}});
    ASSERT_FALSE(mounted.at("result").at("isError")) << payload(mounted).dump();

    for (const auto& className : {"hier/Base", "hier/Child", "hier/Impl", "hier/IFoo",
                                  "hier/IBar", "hier/Unrelated"})
    {
        const auto fromDb      = relationsOf(serverDb, dbProject, className);
        const auto fromEngine = relationsOf(serverEngine, engineProject, className);
        EXPECT_EQ(fromEngine.dump(), fromDb.dump()) << "relations diverged: " << className;
    }

    // 语义抽查（两路径同值，取 DB 面断言）：类起点的后代与实现者分桶。
    const auto base = relationsOf(serverDb, dbProject, "hier/Base");
    EXPECT_EQ(Json::array({"java/lang/Object"}), base.at("superclasses"));
    EXPECT_EQ(Json::array({"hier/Child", "hier/Impl"}), base.at("direct_subclasses"));
    EXPECT_EQ(Json::array({"hier/Child", "hier/GrandChild", "hier/Impl"}),
              base.at("all_subclasses"));
    EXPECT_EQ(Json::array(), base.at("implementors"));

    // 接口起点：实现者 = 非接口子型；接口闭包含传递父接口。IBar 起点钉
    // 严格语义——只实现其父接口 IFoo 的 Impl 不是 IBar 的实现者（库
    // implementors 面向上收父接口的泄漏在此回归）。
    const auto ifoo = relationsOf(serverDb, dbProject, "hier/IFoo");
    EXPECT_EQ(Json::array({"hier/Impl"}), ifoo.at("implementors"));
    EXPECT_EQ(Json::array(), ifoo.at("all_subclasses"));
    const auto impl = relationsOf(serverDb, dbProject, "hier/Impl");
    EXPECT_EQ(Json::array({"hier/IFoo"}), impl.at("interfaces"));
    const auto ibar = relationsOf(serverDb, dbProject, "hier/IBar");
    EXPECT_EQ(Json::array({"hier/IFoo"}), ibar.at("interfaces"));
    EXPECT_EQ(Json::array(), ibar.at("implementors"));

    call(serverDb, "project_close", {{"project_id", dbProject}});
    call(serverEngine, "project_close", {{"project_id", engineProject}});
}

}   // namespace
}   // namespace mjt::mcp
