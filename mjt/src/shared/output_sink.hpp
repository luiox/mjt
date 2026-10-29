#pragma once

// ── 输出抽象（FILE* 封装）──────────────────────────────────────────
//
// MCP 渲染面的输出 sink（自 mjbase cli/output_sink 移交，随 cli 模块退役）：
// 默认指向 stdout/stderr，可指向任意 FILE*（测试捕获、重定向、换输出后端）。
//
// 用法：
//   mjt::out().printf("  Version: %d.%d\n", major, minor);
//   mjt::err().printf("Error: %s\n", msg);
//   mjt::out().puts("done");
//

#include <cstdio>
#include <cstdarg>
#include <string>

namespace mjt {

/// FILE* 封装的输出 sink。printf 签名与 fprintf 一致（首参为 sink 自身），
/// 故 fprintf(stdout, fmt, ...) → out().printf(fmt, ...) 是 1:1 机械替换。
class OutputSink
{
public:
    explicit OutputSink(FILE* f)
        : file_(f)
    {}

    /// 变参格式化输出（与 fprintf 同语义，写到 sink 持有的 FILE*）。
    void printf(const char* fmt, ...) const
    {
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(file_, fmt, ap);
        va_end(ap);
    }

    /// 输出一行（自动加换行）。
    void puts(const std::string& s) const
    {
        std::fputs(s.c_str(), file_);
        std::fputc('\n', file_);
    }

    /// 输出原始字符串（不加换行）。
    void write(const std::string& s) const { std::fputs(s.c_str(), file_); }

    FILE* file() const { return file_; }

private:
    FILE* file_;
};

/// 默认 stdout sink（进程级单例，避免每个 cmd 都构造）。
inline OutputSink& out()
{
    static OutputSink sink(stdout);
    return sink;
}

/// 默认 stderr sink。
inline OutputSink& err()
{
    static OutputSink sink(stderr);
    return sink;
}

}   // namespace mjt
