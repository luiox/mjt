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

/// OutputSink 捕获（FILE* 往返；strings 测试同款构造）。
class CapturedSink
{
public:
    CapturedSink()
        : file_(std::tmpfile())
        , sink_(file_)
    {}
    ~CapturedSink()
    {
        if (file_ != nullptr) std::fclose(file_);
    }
    CapturedSink(const CapturedSink&)            = delete;
    CapturedSink& operator=(const CapturedSink&) = delete;

    mjt::OutputSink& sink() { return sink_; }

    std::string text()
    {
        std::fflush(file_);
        std::rewind(file_);
        std::string out;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file_)) > 0) out.append(buffer, read);
        return out;
    }

private:
    std::FILE*      file_ = nullptr;
    mjt::OutputSink sink_;
};

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
