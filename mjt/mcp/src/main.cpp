#include "mcp_server.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>

#include "mbase/cli/i18n_tables.hpp"
#include "mbase/log/log_init.hpp"
#include "libca/log/log_macros.hpp"

#include "i18n_mjt_shared_translations.generated.hpp"
#include "libca/i18n/i18n.hpp"

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
    ca::i18n::init({mjt::base::cli::kCliI18nTableSet, kI18nTableSet_mjt_shared}, {"MJT_LANG"});
    //诊断统一走 ca::log 门面。
    mjt::base::log::initDefaultLogger();
    ca::i18n::set_lang("en_US");

    mjt::mcp::HttpOptions options;
    bool                           http = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--stdio") continue;
        if (argument == "--http") {
            http = true;
            continue;
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
                         "--token <token> --origin <origin>]\n";
            return 0;
        }
        return fail("unknown or incomplete argument: " + argument);
    }

    mjt::mcp::McpServer server;
    if (!http) return server.runStdio(std::cin, std::cout);
    const auto result = server.runHttp(options);
    if (result == 2)
        return fail("HTTP host/port is invalid, the server is already running, or non-loopback HTTP lacks --token");
    return result;
}
