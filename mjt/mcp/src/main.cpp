#include "mcp_server.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "mjbase/cli/i18n_tables.hpp"
#include "mjbase/log/log_init.hpp"
#include "libca/log/log_macros.hpp"

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
    ca::i18n::init({mjbase::cli::kCliI18nTableSet, kI18nTableSet_mjt_shared}, {"MJT_LANG"});
    //诊断统一走 ca::log 门面。
    mjbase::log::initDefaultLogger();
    ca::i18n::set_lang("en_US");

    mjt::mcp::HttpOptions options;
    bool                           http        = false;
    bool                           interactive = false;
    std::string                    scriptPath;
    std::vector<std::string>       scriptArgs;
    long long                      deadlineMs     = 0;
    unsigned long long             maxOutputBytes = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--stdio") continue;
        if (argument == "--http") {
            http = true;
            continue;
        }
        if (argument == "-i") {
            interactive = true;
            continue;
        }
        //脚本资源上限（只认 --script 之前；缺省读同名环境变量）。
        if (argument == "--deadline-ms" || argument == "--max-output-bytes") {
            if (index + 1 >= argc) return fail(argument + " requires a value");
            const std::string value = argv[++index];
            unsigned long long  parsed = 0;
            if (value.empty() ||
                !std::all_of(value.begin(), value.end(),
                             [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                return fail(argument + " must be a non-negative integer");
            }
            parsed = std::strtoull(value.c_str(), nullptr, 10);
            if (argument == "--deadline-ms") deadlineMs = static_cast<long long>(parsed);
            else maxOutputBytes = parsed;
            continue;
        }
        //--script 之后的参数全部原样透传给脚本（不再按宿主旗标解析），
        //所以 -i 等宿主旗标必须写在 --script 之前。
        if (argument == "--script") {
            if (index + 1 >= argc) return fail("--script requires a script path");
            scriptPath = argv[++index];
            for (int rest = index + 1; rest < argc; ++rest) scriptArgs.emplace_back(argv[rest]);
            break;
        }
        if ((argument == "--host" || argument == "--port" || argument == "--token" ||
             argument == "--origin") &&
            index + 1 < argc) {
            const std::string value = argv[++index];
            if (argument == "--host") options.host = value;
            else if (argument == "--port") {
                //严格整端口校验：stoi 会静默接受 "8080abc" 这类前缀数字，
                //这里要求全数字且落在合法端口范围。
                if (value.empty() ||
                    !std::all_of(value.begin(), value.end(),
                                 [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                    return fail("--port must be an integer");
                }
                const long parsed = std::strtol(value.c_str(), nullptr, 10);
                if (parsed < 1 || parsed > 65535) return fail("--port out of range (1-65535)");
                options.port = static_cast<int>(parsed);
            }
            else if (argument == "--token") options.token = value;
            else options.allowedOrigins.push_back(value);
            continue;
        }
        if (argument == "--help") {
            std::cout << "Usage: mjt-mcp [--stdio] [--http --host <ip> --port <port> "
                         "--token <token> --origin <origin>]\n"
                         "       mjt-mcp --script <file.py> [script-args...] [-i]\n";
            return 0;
        }
        return fail("unknown or incomplete argument: " + argument);
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
    const auto result = server.runHttp(options);
    if (result == 2)
        return fail("HTTP host/port is invalid, the server is already running, or non-loopback HTTP lacks --token");
    return result;
}
