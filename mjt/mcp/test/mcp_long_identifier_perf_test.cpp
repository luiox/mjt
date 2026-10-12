#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

//（perf）超长类名链路的 mjt-mcp 面测量：ClassWriter 运行时生成
//内部名总长 8/64/512/819/4096/65000 的合法类（JVM 常量池 UTF8 上限 65535
//字节，这些长度全部合法），普通名类作对照。经 McpSessionHarness 实测
//workspace_open / class_list / class_get / class_jasm / search_classes /
//class_bytes 六条操作的 wall time；另测「多类 × 长名」组合档
//（200 类 × 819 字符 vs 200 类 × 普通名）验证索引/扫描是否随
//类数 × 名长复合增长。本测试只产出数字与正确性断言，不设严格耗时
//上界（CI 抖动防护；耗时结论落 issue/PR）。

namespace mjt::mcp {
namespace {

using namespace test_support;

///单次预热后取多次读数最小值（min 对调度抖动最稳）；单次已超 1s 判为病态，
///只跑一次、以单次读数为证据，避免病态档位把测试拖到分钟级。
template <typename Fn>
double measureMs(Fn&& fn)
{
    const auto warmStart = std::chrono::steady_clock::now();
    fn();
    const double first =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warmStart)
            .count();
    if (first > 1000.0) return first;
    double best = first;
    for (int iteration = 0; iteration < 4; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        best = std::min(best, ms);
    }
    return best;
}

struct LengthRow
{
    std::string tag     = "";
    double      openMs  = 0;
    double      listMs  = 0;
    double      getMs   = 0;
    double      jasmMs  = 0;
    double      searchMs = 0;
    double      bytesMs = 0;
};

void printTable(const std::vector<LengthRow>& rows)
{
    std::cout << "\n== #1093 mjt-mcp long-identifier measurement (ms, min of 5) ==\n";
    std::printf("%-12s %10s %10s %10s %12s %12s %10s\n",
                "case", "open", "list", "get", "jasm", "search", "bytes");
    for (const auto& row : rows) {
        std::printf("%-12s %10.3f %10.3f %10.3f %12.3f %12.3f %10.3f\n",
                    row.tag.c_str(),
                    row.openMs,
                    row.listMs,
                    row.getMs,
                    row.jasmMs,
                    row.searchMs,
                    row.bytesMs);
    }
    std::cout.flush();
}

///梯度档：单 jar = 对照类 + 一条 totalLength 长名类；六操作逐档实测，
///长名类的 jasm 渲染正确性同场钉住。
void measureGradient(std::vector<LengthRow>& rows)
{
    const std::size_t lengths[] = {8, 64, 512, 819, 4096, 65000};

    for (const std::size_t length : lengths) {
        const auto jar = writeLongNameFixture(length);
        const auto longName = longInternalName(length);

        McpSessionHarness server;
        LengthRow row;
        row.tag = std::to_string(length);

        row.openMs = measureMs([&] {
            const auto closed = call(server, "workspace_close", {{"workspace_id", "perf"}});
            (void)closed;
            const auto opened = call(server, "workspace_open", {{"path", jar.string()},
                                                                {"workspace_id", "perf"}});
            ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
        });

        row.listMs = measureMs([&] {
            const auto classes = call(server, "class_list", {{"workspace_id", "perf"}});
            ASSERT_FALSE(classes.at("result").at("isError")) << classes.dump();
        });

        row.getMs = measureMs([&] {
            const auto detail =
                call(server, "class_get", {{"workspace_id", "perf"}, {"class", longName}});
            ASSERT_FALSE(detail.at("result").at("isError")) << detail.dump();
            EXPECT_EQ(longName, payload(detail).at("internal_name"));
        });

        row.jasmMs = measureMs([&] {
            const auto jasm =
                call(server, "class_jasm", {{"workspace_id", "perf"}, {"class", longName}});
            ASSERT_FALSE(jasm.at("result").at("isError")) << jasm.dump();
        });

        //唯一命中长名类的探针：长度后缀 _<len>。
        const std::string probe = "_" + std::to_string(length);
        row.searchMs = measureMs([&] {
            const auto search =
                call(server, "search_classes", {{"workspace_id", "perf"}, {"query", probe}});
            ASSERT_FALSE(search.at("result").at("isError")) << search.dump();
            ASSERT_EQ(1U, payload(search).at("items").size());
        });

        row.bytesMs = measureMs([&] {
            const auto bytes =
                call(server, "class_bytes", {{"workspace_id", "perf"}, {"class", longName}});
            ASSERT_FALSE(bytes.at("result").at("isError")) << bytes.dump();
        });

        //正确性钉住：长名类的 jasm 渲染非空且含 .class 声明行与完整长名。
        const auto jasm = call(server, "class_jasm", {{"workspace_id", "perf"}, {"class", longName}});
        ASSERT_FALSE(jasm.at("result").at("isError"));
        const auto& source = payload(jasm).at("source").get_ref<const std::string&>();
        EXPECT_NE(std::string::npos, source.find(".class public super"));
        EXPECT_NE(std::string::npos, source.find(longName));

        rows.push_back(row);
    }
}

TEST(McpLongIdentifierPerfTest, MeasuresReadPathLatencyAcrossNameLengths)
{
    std::vector<LengthRow> rows;
    measureGradient(rows);
    printTable(rows);
}

///组合档：200 类 × 819 字符 vs 200 类 × 普通名，同类同规模对照。
///class_get 选末位长名类，让 resolveClassUri 扫到最坏位置。
void measureBulk(std::vector<LengthRow>& rows)
{
    const std::size_t count = 200;
    for (const std::size_t length : {std::size_t{0}, std::size_t{819}}) {
        const auto jar = writeBulkLongNameFixture(count, length);
        McpSessionHarness server;
        LengthRow row;
        row.tag = length == 0 ? "bulk200-norm" : "bulk200-819";

        row.openMs = measureMs([&] {
            const auto closed = call(server, "workspace_close", {{"workspace_id", "bulk"}});
            (void)closed;
            const auto opened = call(server, "workspace_open", {{"path", jar.string()},
                                                                {"workspace_id", "bulk"}});
            ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
        });

        row.listMs = measureMs([&] {
            const auto classes = call(server, "class_list", {{"workspace_id", "bulk"}});
            ASSERT_FALSE(classes.at("result").at("isError")) << classes.dump();
        });

        const std::string probe =
            length == 0 ? "mcp/Bulk199"
                        : longInternalNameWithSuffix(length, "_" + std::to_string(length) + "_199");
        row.getMs = measureMs([&] {
            const auto detail =
                call(server, "class_get", {{"workspace_id", "bulk"}, {"class", probe}});
            ASSERT_FALSE(detail.at("result").at("isError")) << detail.dump();
            EXPECT_EQ(probe, payload(detail).at("internal_name"));
        });

        row.jasmMs = measureMs([&] {
            const auto jasm =
                call(server, "class_jasm", {{"workspace_id", "bulk"}, {"class", probe}});
            ASSERT_FALSE(jasm.at("result").at("isError")) << jasm.dump();
        });

        row.searchMs = measureMs([&] {
            const auto search =
                call(server, "search_classes", {{"workspace_id", "bulk"}, {"query", "_"}});
            ASSERT_FALSE(search.at("result").at("isError")) << search.dump();
        });

        row.bytesMs = measureMs([&] {
            const auto bytes =
                call(server, "class_bytes", {{"workspace_id", "bulk"}, {"class", probe}});
            ASSERT_FALSE(bytes.at("result").at("isError")) << bytes.dump();
        });

        rows.push_back(row);
    }
}

TEST(McpLongIdentifierPerfTest, MeasuresBulkJarsWithManyLongNames)
{
    std::vector<LengthRow> rows;
    measureBulk(rows);
    printTable(rows);
}

///patch 往返档：patch_validate_jasm 用含长名的 jasm 源文本走
///装配 → 严格校验 → diff 全链（dry_run，不落 revision）。jasm 装配器
///解析长名源文本是 MCP 面最后一个未测量的潜在慢点。
TEST(McpLongIdentifierPerfTest, MeasuresJasmPatchRoundTripWithLongNames)
{
    struct PatchRow
    {
        std::string tag;
        double validateMs = 0;
    };
    std::vector<PatchRow> rows;
    for (const std::size_t length : {std::size_t{8}, std::size_t{819}, std::size_t{4096},
                                     std::size_t{65000}}) {
        const auto jar = writeLongNameFixture(length);
        const auto longName = longInternalName(length);
        McpSessionHarness server;
        const auto opened = call(server, "workspace_open", {{"path", jar.string()}});
        ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
        const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

        PatchRow row;
        row.tag = std::to_string(length);
        row.validateMs = measureMs([&] {
            const auto preview = call(server,
                                      "patch_validate_jasm",
                                      {{"workspace_id", workspaceId},
                                       {"uri", longName + ".class"},
                                       {"source", classSourceFor(longName, "42")},
                                       {"expected_revision", 0},
                                       {"dry_run", true}});
            ASSERT_FALSE(preview.at("result").at("isError")) << preview.dump();
        });
        rows.push_back(row);
    }

    std::cout << "\n== #1093 mjt-mcp long-identifier patch round-trip (ms, min of 5) ==\n";
    std::printf("%-12s %14s\n", "len", "patch_validate");
    for (const auto& row : rows)
        std::printf("%-12s %14.3f\n", row.tag.c_str(), row.validateMs);
    std::cout.flush();
}

///真反编译档（门控）：MJT_MCP_REAL_CFR=<cfr.jar> 时启用，用真 CFR 走
///class_source 同步反编译，量「用户侧 decompile」假设。无 env 时跳过，
///不进 CI。819+ 字符的类名会产出同名 .java 输出文件，Windows MAX_PATH
///下的行为本身就是 要的数据点。
TEST(McpLongIdentifierPerfTest, RealCfrDecompileWithLongNames)
{
    const char* cfrPath = std::getenv("MJT_MCP_REAL_CFR");
    if (cfrPath == nullptr) GTEST_SKIP() << "set MJT_MCP_REAL_CFR=<cfr.jar> to enable";
    const char* javaPath = std::getenv("MJT_MCP_REAL_JAVA");
    const std::string java = javaPath != nullptr ? javaPath : "java";

    std::cout << "\n== #1093 real-CFR class_source (ms, single run) ==\n";
    std::printf("%-12s %12s %10s %s\n", "len", "ms", "ok", "note");
    for (const std::size_t length : {std::size_t{8}, std::size_t{819}, std::size_t{4096},
                                     std::size_t{65000}}) {
        const auto jar = writeLongNameFixture(length);
        const auto longName = longInternalName(length);
        McpSessionHarness session;
        const auto settings = call(session,
                                   "decompiler_settings_set",
                                   {{"backend", "cfr"},
                                    {"cfr_jar", cfrPath},
                                    {"java", java},
                                    {"timeout_ms", 120000}});
        ASSERT_FALSE(settings.at("result").at("isError")) << settings.dump();
        const auto opened = call(session, "workspace_open", {{"path", jar.string()}});
        ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
        const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

        const auto start = std::chrono::steady_clock::now();
        const auto source = call(session,
                                 "class_source",
                                 {{"workspace_id", workspaceId}, {"class", longName}});
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        const bool ok = !source.at("result").at("isError").get<bool>();
        std::string note;
        if (!ok) {
            note = source.at("result").dump();
            if (note.size() > 300) note = note.substr(0, 300);
        }
        std::printf("%-12zu %12.1f %10s %s\n", length, ms, ok ? "yes" : "no", note.c_str());
    }
    std::cout.flush();
}

}   //namespace
}   //namespace mjt::mcp
