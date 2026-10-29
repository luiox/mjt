#include "mcp_server.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "mjbase/log/log_init.hpp"
#include "libca/log/log_macros.hpp"
#include "libca/opt/opt.hpp"

#include "i18n_mjt_shared_translations.generated.hpp"
#include "libca/i18n/i18n.hpp"

#if defined(MJT_WITH_PYSDK)
#include "pysdk_embed.hpp"
#endif

namespace {

int fail(const std::string& message)
{
    CA_LOG_ERROR("mjt-mcp: {}", message);
    return 2;
}

}   //namespace

int main(int argc, char* argv[])
{
    //MCP 是协议文本面,固定英文:共享层错误经 errStatus 在构造时渲染,
    //故进程启动即锁定 en_US(环境变量对 MCP 不生效)。
    ca::i18n::init({kI18nTableSet_mjt_shared}, {"MJT_LANG"});
    //诊断统一走 ca::log 门面。
    mjbase::log::initDefaultLogger();
    ca::i18n::set_lang("en_US");

    mjt::mcp::HttpOptions options;

    //--script 之后的参数全部原样透传给脚本（不再按宿主旗标解析），
    //所以宿主旗标必须写在 --script 之前：先按首个 --script 预切分 argv，
    //头部交 ca::opt 解析，尾部整段留给脚本。
    std::vector<const char*> headArgv{argv[0]};
    std::string              scriptPath;
    std::vector<std::string> scriptArgs;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view{argv[index]} == "--script") {
            if (index + 1 >= argc) return fail("--script requires a script path");
            scriptPath = argv[++index];
            for (int rest = index + 1; rest < argc; ++rest) scriptArgs.emplace_back(argv[rest]);
            break;
        }
        headArgv.push_back(argv[index]);
    }

    //宿主旗标面（ca::opt）：--stdio/--http/-i 开关 + 四个带值选项。
    //数值上限语义保持 strtoull 全数字校验（u64 域，不用 Int 的 32 位口径），
    //错误文案与 help 原样保留。
    ca::opt::Command cmd;
    cmd.name = "mjt-mcp";
    cmd.help = "MCP server (stdio / http) with optional embedded Python script host";
    ca::opt::Arg a;
    a.name = "stdio"; a.kind = ca::opt::OptKind::Flag;
    a.help = "run over stdio (default)"; cmd.args.push_back(a);
    a = ca::opt::Arg{};
    a.name = "http"; a.kind = ca::opt::OptKind::Flag;
    a.help = "run over http"; cmd.args.push_back(a);
    a = ca::opt::Arg{};
    a.name = "interactive"; a.aliases = {"-i"}; a.kind = ca::opt::OptKind::Flag;
    a.help = "interactive script mode (requires --script)"; cmd.args.push_back(a);
    a = ca::opt::Arg{};
    a.name = "host"; a.kind = ca::opt::OptKind::String; a.metavar = "<ip>";
    a.help = "http bind host"; cmd.args.push_back(a);
    a = ca::opt::Arg{};
    a.name = "port"; a.kind = ca::opt::OptKind::String; a.metavar = "<port>";
    a.help = "http bind port (1-65535)"; cmd.args.push_back(a);
    a = ca::opt::Arg{};
    a.name = "token"; a.kind = ca::opt::OptKind::String; a.metavar = "<token>";
    a.help = "http auth token"; cmd.args.push_back(a);
    a = ca::opt::Arg{};
    a.name = "origin"; a.kind = ca::opt::OptKind::StringList;
    a.metavar = "<origin>"; a.help = "allowed origin (repeatable)"; cmd.args.push_back(a);
    a = ca::opt::Arg{};
    a.name = "deadline-ms"; a.kind = ca::opt::OptKind::String;
    a.metavar = "<ms>"; a.help = "script wall-clock deadline"; cmd.args.push_back(a);
    a = ca::opt::Arg{};
    a.name = "max-output-bytes";
    a.kind = ca::opt::OptKind::String; a.metavar = "<n>";
    a.help = "script output cap in bytes"; cmd.args.push_back(a);

    ca::opt::Parser parser{std::move(cmd)};
    const auto parsed = parser.parse(static_cast<int>(headArgv.size()), headArgv.data());
    if (parsed.is_err()) {
        const auto err = std::move(parsed).unwrap_err();
        if (err.category == ca::opt::ParseErrorCategory::HelpRequested) {
            std::cout << err.message;
            return 0;
        }
        return fail(err.message);
    }
    const auto& result = std::move(parsed).unwrap();

    const bool http        = result.has("http");
    const bool interactive = result.has("interactive");
    long long          deadlineMs     = 0;
    unsigned long long maxOutputBytes = 0;
    if (result.has("host")) options.host = result.get("host");
    if (result.has("token")) options.token = result.get("token");
    if (result.has("origin")) options.allowedOrigins = result.get_list("origin");
    if (result.has("port")) {
        const std::string& value = result.get("port");
        //严格整端口校验：stoi 会静默接受 "8080abc" 这类前缀数字，
        //这里要求全数字且落在合法端口范围。
        if (value.empty() ||
            !std::all_of(value.begin(), value.end(),
                         [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            return fail("--port must be an integer");
        }
        const long parsedPort = std::strtol(value.c_str(), nullptr, 10);
        if (parsedPort < 1 || parsedPort > 65535) return fail("--port out of range (1-65535)");
        options.port = static_cast<int>(parsedPort);
    }
    //脚本资源上限（缺省读同名环境变量，旗标显式给值时覆盖）。
    if (result.has("deadline-ms")) {
        const std::string& value = result.get("deadline-ms");
        if (value.empty() ||
            !std::all_of(value.begin(), value.end(),
                         [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            return fail("--deadline-ms must be a non-negative integer");
        }
        deadlineMs = static_cast<long long>(std::strtoull(value.c_str(), nullptr, 10));
    }
    if (result.has("max-output-bytes")) {
        const std::string& value = result.get("max-output-bytes");
        if (value.empty() ||
            !std::all_of(value.begin(), value.end(),
                         [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            return fail("--max-output-bytes must be a non-negative integer");
        }
        maxOutputBytes = std::strtoull(value.c_str(), nullptr, 10);
    }

    if (interactive && scriptPath.empty()) return fail("-i requires --script <file>");
    if (!scriptPath.empty()) {
#if defined(MJT_WITH_PYSDK)
        //环境变量做缺省（部署侧统一策略），旗标显式给值时覆盖。
        if (deadlineMs == 0) {
            if (const char* env = std::getenv("MJT_SCRIPT_DEADLINE_MS")) {
                deadlineMs = std::strtoll(env, nullptr, 10);
            }
        }
        if (maxOutputBytes == 0) {
            if (const char* env = std::getenv("MJT_SCRIPT_MAX_OUTPUT_BYTES")) {
                maxOutputBytes = std::strtoull(env, nullptr, 10);
            }
        }
        mjt::pysdk::EmbedOptions embed;
        embed.scriptPath      = scriptPath;
        embed.scriptArgs      = scriptArgs;
        embed.interactive     = interactive;
        embed.deadlineMs      = deadlineMs;
        embed.maxOutputBytes  = maxOutputBytes;
        return mjt::pysdk::runEmbeddedScript(embed);
#else
        return fail("--script needs a build with the embedded Python SDK: "
                    "reconfigure with `xmake f --with_pysdk=y` and rebuild mjt-mcp");
#endif
    }

    mjt::mcp::McpServer server;
    if (!http) return server.runStdio(std::cin, std::cout);
    const auto httpResult = server.runHttp(options);
    if (httpResult == 2)
        return fail("HTTP host/port is invalid, the server is already running, or non-loopback HTTP lacks --token");
    return httpResult;
}
