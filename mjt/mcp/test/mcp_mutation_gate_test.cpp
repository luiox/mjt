#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <libca/core/datatype.hpp>

#include <jbc/jbc.hpp>

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

// ── #73 复现面 fixture ────────────────────────────────────────
// 类内容一律 jasm 装配生成；坏帧类走最小偏差路线：好类装配后仅在测试里把
// value 的 ireturn (0xAC) 翻成 areturn (0xB0)——()I 返回引用是验证器必拒的
// 帧错误，单字节偏差、类骨架与常量池零改动。不手拼 classfile 字节。

/// @brief 目标类模板：counter 字段（旗标编辑目标）+ value/seed 两方法。
///        value 的返回指令整行由 @p returnLine 控制（坏帧偏差即翻这一条）。
std::string gateClassSource(const std::string& name, const char* returnLine)
{
    return ".super java/lang/Object\n"
           ".class public super " +
           name +
           " {\n"
           "  .field public counter I\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      iconst_0\n" +
           returnLine +
           "\n"
           "    }\n"
           "  }\n"
           "  .method public static seed ()V {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      pop\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 好类 → 先在坏帧类（最小偏差单字节翻位，写回经 libjbc 会话面）。
std::vector<ca::u8> withBadReturnValue(const std::vector<ca::u8>& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) throw std::runtime_error("gate fixture did not parse");
    auto parsed = std::move(read).unwrap();
    bool patched = false;
    for (jbc::core::MemberInfo& member : parsed.file->methods) {
        if (parsed.file->nameOf(member) != "value") continue;
        for (jbc::core::AttrSlot& slot : member.attrs) {
            if (!parsed.file->cp.utf8Equals(slot.nameCp, "Code")) continue;
            if (!slot.decoded() && parsed.file->decodeAttr(slot).is_err())
                throw std::runtime_error("gate fixture code attribute did not decode");
            auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
            if (code == nullptr || code->code.empty())
                throw std::runtime_error("gate fixture code body missing");
            if (code->code.back() != 0xAC)
                throw std::runtime_error("gate fixture expected trailing ireturn");
            code->code.back() = 0xB0;
            patched = true;
        }
    }
    if (!patched) throw std::runtime_error("gate fixture method value not found");
    auto written = jbc::core::writeClass(*parsed.file);
    if (written.is_err()) throw std::runtime_error("gate fixture did not rewrite");
    return std::move(written).unwrap();
}

/// @brief 一 jar 双类：gate/Good（全好）+ gate/Bad（value 先在坏帧）。
///        装配/翻位后各做一次 relaxed validate 自检，fixture 假设失真即炸。
std::filesystem::path writeGateFixture()
{
    mjbase::jasm::JvmJasmService          validator;
    mjbase::jasm::JvmJasmContext          relaxed;
    const std::vector<ca::u8>             good = assembleFixtureClass(
        "gate/Good", gateClassSource("gate/Good", "      ireturn"));
    if (!validator.validate(good, relaxed).isOk())
        throw std::runtime_error("gate fixture good class must pass relaxed validate");
    std::vector<ca::u8> bad = withBadReturnValue(assembleFixtureClass(
        "gate/Bad", gateClassSource("gate/Bad", "      ireturn")));
    if (validator.validate(bad, relaxed).isOk())
        throw std::runtime_error("gate fixture bad class must fail relaxed validate");

    mjbase::model::ResourceContainer container;
    container.addResource("gate/Good.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "gate/Good.class", good));
    container.addResource("gate/Bad.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "gate/Bad.class", bad));
    const auto path = test::getTempOutPath("mcp_mutation_gate_input.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// @brief 复述 gate/Bad 坏帧的 jasm 源（value 载 areturn；全量替换语义下
///        源本身携带坏帧，jasm 编辑照旧被拒）。
const char* badFrameJasmSource()
{
    return ".super java/lang/Object\n"
           ".class public super gate/Bad {\n"
           "  .field public counter I\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      iconst_0\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "  .method public static seed ()V {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

void gateSession(McpSessionHarness& server, std::string& projectId)
{
    const auto opened = call(server, "project_open", {{"path", writeGateFixture().string()}});
    if (opened.at("result").at("isError")) throw std::runtime_error("gate fixture jar did not open");
    projectId = payload(opened).at("project_id").get<std::string>();
}

}   // namespace

// a) 先在坏帧类上的合法旗标变更（public → private）在默认档 dry_run 通过：
//    旗标-only 编辑不吃整类 rebuild 门（#73 主修）。
TEST(McpMutationGateTest, FlagsOnlyMutationPassesOnBadFrameClassDryRun)
{
    McpSessionHarness server;
    std::string       projectId;
    gateSession(server, projectId);

    const auto preview = call(server,
                              "edit_apply",
                              {{"project_id", projectId},
                               {"kind", "structured"},
                               {"mutation_kind", "set_field_access"},
                               {"class_uri", "gate/Bad.class"},
                               {"member_name", "counter"},
                               {"member_descriptor", "I"},
                               {"access", 0x0002},
                               {"dry_run", true}});
    ASSERT_FALSE(preview.at("result").at("isError")) << payload(preview).dump();
    EXPECT_TRUE(payload(preview).at("status").at("ok").get<bool>());
    EXPECT_FALSE(payload(preview).at("committed").get<bool>());

    // d) byte_diffs 在分档路径照常返回：旗标变更的最小字节足迹可核对。
    const auto& diffs = payload(preview).at("byte_diffs");
    ASSERT_EQ(1U, diffs.size());
    EXPECT_EQ("gate/Bad.class", diffs.at(0).at("uri").get<std::string>());
    EXPECT_EQ(diffs.at(0).at("before_size").get<ca::usize>(),
              diffs.at(0).at("after_size").get<ca::usize>());
    EXPECT_FALSE(diffs.at(0).at("ranges").empty());
}

// b) 触碰方法体的编辑在坏帧类上仍被 rebuild 门拒（分档不误放），且错误
//    信息归因到先在问题；jasm 全量编辑携坏帧源同样照拒。
TEST(McpMutationGateTest, MethodBodyMutationOnBadFrameClassStillGated)
{
    McpSessionHarness server;
    std::string       projectId;
    gateSession(server, projectId);

    // 换体 seed（合法新体 return），value 的先在坏帧仍在候选里 → 整类门照拒。
    const auto replaceBody = call(server,
                                  "edit_apply",
                                  {{"project_id", projectId},
                                   {"kind", "structured"},
                                   {"mutation_kind", "replace_method_body"},
                                   {"class_uri", "gate/Bad.class"},
                                   {"member_name", "seed"},
                                   {"member_descriptor", "()V"},
                                   {"initial_instructions",
                                    Json::array({
                                        Json{{"kind", "simple"}, {"opcode", 0xB1}},
                                    })},
                                   {"dry_run", true}});
    ASSERT_TRUE(replaceBody.at("result").at("isError")) << payload(replaceBody).dump();
    // 归因保底：先在字节同档位重验同样不过门 → 错误信息注明先在问题。
    EXPECT_NE(std::string::npos,
              payload(replaceBody).at("status").at("message").get<std::string>().find(
                  "pre-existing"));
    // 诊断仍出自 rebuild 的 validate 档（stage_name=validate / gate=rebuild）。
    bool sawRebuildValidate = false;
    for (const auto& diagnostic : payload(replaceBody).at("diagnostics")) {
        if (diagnostic.at("stage_name").get<std::string>() == "validate" &&
            diagnostic.at("gate").get<std::string>() == "rebuild")
            sawRebuildValidate = true;
    }
    EXPECT_TRUE(sawRebuildValidate);

    // jasm 全量编辑：源本身复述同一坏帧 → assemble/validate 链照拒（该路径
    // 未参与分档，行为与此前一致）。
    const auto jasmPreview = call(server,
                                  "edit_apply",
                                  {{"project_id", projectId},
                                   {"kind", "jasm"},
                                   {"dry_run", true},
                                   {"uri", "gate/Bad.class"},
                                   {"source_name", "Bad.jasm"},
                                   {"source", badFrameJasmSource()}});
    ASSERT_TRUE(jasmPreview.at("result").at("isError")) << payload(jasmPreview).dump();
}

// c) 好类上触碰方法体的编辑行为不变：合法换体照常过门；坏换体仍被 rebuild
//    门拒（门对方法体面编辑保持现役）。
TEST(McpMutationGateTest, MethodBodyMutationOnGoodClassUnchanged)
{
    McpSessionHarness server;
    std::string       projectId;
    gateSession(server, projectId);

    // 合法新体（iconst_1 → pop → return 收敛为 return）dry_run 照常通过。
    const auto replaceBody = call(server,
                                  "edit_apply",
                                  {{"project_id", projectId},
                                   {"kind", "structured"},
                                   {"mutation_kind", "replace_method_body"},
                                   {"class_uri", "gate/Good.class"},
                                   {"member_name", "seed"},
                                   {"member_descriptor", "()V"},
                                   {"initial_instructions",
                                    Json::array({
                                        Json{{"kind", "simple"}, {"opcode", 0xB1}},
                                    })},
                                   {"dry_run", true}});
    ASSERT_FALSE(replaceBody.at("result").at("isError")) << payload(replaceBody).dump();
    EXPECT_FALSE(payload(replaceBody).at("byte_diffs").empty());

    // 坏换体（areturn 弹引用而栈上是 int，内部数据流不一致）仍被 rebuild
    // 门拒——门未被分档误放。注：校验门查的是方法体内数据流一致性，不查
    // 返回指令与描述符的匹配（ireturn 载 ()V 内部自洽，门不拒）。
    const auto badBody = call(server,
                              "edit_apply",
                              {{"project_id", projectId},
                               {"kind", "structured"},
                               {"mutation_kind", "replace_method_body"},
                               {"class_uri", "gate/Good.class"},
                               {"member_name", "seed"},
                               {"member_descriptor", "()V"},
                               {"initial_instructions",
                                Json::array({
                                    Json{{"kind", "simple"}, {"opcode", 0x03}},
                                    Json{{"kind", "simple"}, {"opcode", 0xB0}},
                                })},
                               {"dry_run", true}});
    ASSERT_TRUE(badBody.at("result").at("isError")) << payload(badBody).dump();
    bool sawRebuildValidate = false;
    for (const auto& diagnostic : payload(badBody).at("diagnostics")) {
        if (diagnostic.at("stage_name").get<std::string>() == "validate" &&
            diagnostic.at("gate").get<std::string>() == "rebuild")
            sawRebuildValidate = true;
    }
    EXPECT_TRUE(sawRebuildValidate);
}

}   // namespace mjt::mcp
