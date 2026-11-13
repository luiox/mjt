/// @file        cli_subs_test.cpp
/// @brief       `mjt subs` 核心面测试：字段/方法清单与旗标 / native 标记 /
///              排序契约 / 类形态派生 / 错误分支 / 文本与 JSON 渲染确定性。
#include <gtest/gtest.h>

#include <string>

#include "cli_fixture.hpp"
#include "commands.hpp"
#include "shared/output_sink.hpp"

namespace {

using mjt::cli::collectSubs;
using mjt::cli::SubsMember;

/// 成员行查找：kind + 名字命中返回该行，未命中返回 nullptr。
const SubsMember* findMember(const mjt::cli::SubsReport& report, const std::string& kind,
                             const std::string& name)
{
    for (const auto& member : report.members) {
        if (member.kind == kind && member.name == name) return &member;
    }
    return nullptr;
}

using CapturedSink = cli_fixture::CapturedSink;

}   // namespace

TEST(MjtCliSubsTest, ListsFieldsAndMethodsWithAccessFlags)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectSubs(jar.string(), "fixture/Alpha");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    EXPECT_EQ(info.internalName, "fixture/Alpha");
    EXPECT_EQ(info.kind, "class");
    EXPECT_EQ(info.fields, 1);
    EXPECT_EQ(info.methods, 1);
    ASSERT_EQ(info.members.size(), static_cast<std::size_t>(2));

    // 字段：private I → access = 2（ACC_PRIVATE），native 恒 false。
    const auto* field = findMember(info, "field", "state");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->descriptor, "I");
    EXPECT_EQ(field->access, 2);
    EXPECT_FALSE(field->native);

    // 方法：public static (I)I → access = 9（ACC_PUBLIC | ACC_STATIC）。
    const auto* method = findMember(info, "method", "act");
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->descriptor, "(I)I");
    EXPECT_EQ(method->access, 9);
    EXPECT_FALSE(method->native);
}

TEST(MjtCliSubsTest, MarksNativeMethods)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectSubs(jar.string(), "fixture/Native");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    EXPECT_EQ(info.fields, 1);
    EXPECT_EQ(info.methods, 2);

    // native 方法：public native (I)V → access = 257（ACC_PUBLIC | ACC_NATIVE）。
    const auto* native = findMember(info, "method", "probe");
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->descriptor, "(I)V");
    EXPECT_EQ(native->access, 257);
    EXPECT_TRUE(native->native);

    // 同类常规方法 native = false（位测试不误伤）。
    const auto* plain = findMember(info, "method", "calc");
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->access, 9);
    EXPECT_FALSE(plain->native);
}

TEST(MjtCliSubsTest, ListsDexMembers)
{
    const auto dex = test::getTopProjectPath() / "test_resource" / "dex" / "hello_refs.dex";
    auto report = collectSubs(dex.u8string(), "com/mjt/dexfix/Subject");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();
    EXPECT_EQ(info.fields, 1);
    EXPECT_EQ(info.methods, 4);
    const auto* field = findMember(info, "field", "sCache");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->descriptor, "Ljava/lang/String;");
    const auto* method = findMember(info, "method", "onEvent");
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->descriptor, "(I)V");
}

TEST(MjtCliSubsTest, FieldsPrecedeMethodsAndOrderIsStable)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectSubs(jar.string(), "fixture/Native");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    // 字段在前（censusMembers kind 编码序），各自保持 classfile 声明序。
    ASSERT_EQ(info.members.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(info.members[0].kind, "field");
    EXPECT_EQ(info.members[0].name, "token");
    EXPECT_EQ(info.members[1].kind, "method");
    EXPECT_EQ(info.members[1].name, "probe");
    EXPECT_EQ(info.members[2].kind, "method");
    EXPECT_EQ(info.members[2].name, "calc");
}

TEST(MjtCliSubsTest, DerivesInterfaceAndEnumKinds)
{
    const auto jar = cli_fixture::writeClassesJar();

    auto api = collectSubs(jar.string(), "fixture/Api");
    ASSERT_TRUE(api.is_ok()) << api.unwrap_err().message;
    {
        const auto info = std::move(api).unwrap();
        EXPECT_EQ(info.kind, "interface");
        EXPECT_EQ(info.methods, 1);
        const auto* method = findMember(info, "method", "act");
        ASSERT_NE(method, nullptr);
        // public abstract → access = 1025（ACC_PUBLIC | ACC_ABSTRACT）。
        EXPECT_EQ(method->access, 1025);
    }

    auto color = collectSubs(jar.string(), "fixture/Color");
    ASSERT_TRUE(color.is_ok()) << color.unwrap_err().message;
    EXPECT_EQ(std::move(color).unwrap().kind, "enum");
}

TEST(MjtCliSubsTest, UnknownClassIsUsageError)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectSubs(jar.string(), "fixture/NoSuch");
    ASSERT_TRUE(report.is_err());
    EXPECT_EQ(report.unwrap_err().kind, mjt::cli::SubsError::Kind::ClassNotFound);
}

TEST(MjtCliSubsTest, UnopenableJarIsRuntimeError)
{
    auto report = collectSubs("mjt_cli_subs_missing.jar", "fixture/Alpha");
    ASSERT_TRUE(report.is_err());
    EXPECT_EQ(report.unwrap_err().kind, mjt::cli::SubsError::Kind::Runtime);
}

TEST(MjtCliSubsTest, TextAndJsonRenderingsAreDeterministic)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectSubs(jar.string(), "fixture/Native");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    CapturedSink textCapture;
    mjt::cli::renderSubsText(info, textCapture.sink());
    const auto text = textCapture.text();
    EXPECT_NE(text.find("subs: fixture/Native"), std::string::npos);
    EXPECT_NE(text.find("class kind: class"), std::string::npos);
    EXPECT_NE(text.find("fields: 1, methods: 2"), std::string::npos);
    // 文本行 = kind 名 描述符 access 十进制 + native 标记。
    EXPECT_NE(text.find("  method probe (I)V access=257 native"), std::string::npos);
    EXPECT_NE(text.find("  field token J access=2\n"), std::string::npos);
    EXPECT_NE(text.find("  method calc (II)I access=9\n"), std::string::npos);

    CapturedSink jsonCapture;
    mjt::cli::renderSubsJson(info, jsonCapture.sink());
    const auto json = jsonCapture.text();
    EXPECT_NE(json.find("\"class\": \"fixture/Native\""), std::string::npos);
    EXPECT_NE(json.find("\"kind\": \"class\""), std::string::npos);
    EXPECT_NE(json.find("\"fields\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"methods\": 2"), std::string::npos);
    EXPECT_NE(json.find("{\"kind\": \"method\", \"name\": \"probe\", "
                        "\"descriptor\": \"(I)V\", \"access\": 257, \"native\": true}"),
              std::string::npos);
    EXPECT_NE(json.find("{\"kind\": \"field\", \"name\": \"token\", "
                        "\"descriptor\": \"J\", \"access\": 2, \"native\": false}"),
              std::string::npos);

    // 渲染纯函数：同数据两次渲染逐字节一致。
    CapturedSink textAgain;
    mjt::cli::renderSubsText(info, textAgain.sink());
    EXPECT_EQ(text, textAgain.text());
    CapturedSink jsonAgain;
    mjt::cli::renderSubsJson(info, jsonAgain.sink());
    EXPECT_EQ(json, jsonAgain.text());
}

TEST(MjtCliSubsTest, UnparsableClassBytesIsRuntimeWithInfoFailureMessage)
{
    // 与 disasm 同款语料：URI 形选择器直命中容器键，jbc 会话解析失败 →
    // DataLoss（非 NotFound）→ class_info_failed 文案 + Runtime；此前该
    // 分支只有类缺失（用法 1）档。
    mjbase::model::ResourceContainer container;
    container.addResource("fixture/Stub.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Stub.class",
                              std::vector<ca::u8>{0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00}));
    const auto path = test::getTempOutPath("mjt_cli_subs_unparsable_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());

    auto report = collectSubs(path.string(), "fixture/Stub.class");
    ASSERT_TRUE(report.is_err());
    auto error = std::move(report).unwrap_err();
    EXPECT_EQ(error.kind, mjt::cli::SubsError::Kind::Runtime);
    EXPECT_NE(error.message.find("cannot read class info"), std::string::npos);
}
