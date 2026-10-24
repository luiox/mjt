#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "libca/core/status.hpp"

namespace mjt {

///@brief 归档条目路径穿越（zip-slip）防护。
///
///拒绝空路径、绝对路径、根相对路径（Windows 上如 "/abs.txt"）、带盘符/root
///name 的路径，以及任何 `..` 段。解包归档条目（JAR extract、Vineflower 反编译
///产物归档解包等）拼接到输出目录前必须先经此检查，否则恶意或损坏归档条目名
///可能写到目标目录之外。这是安全关键检查，不能为了兼容性放宽。
bool isSafeArchivePath(const std::filesystem::path& path);

///@brief 归档条目名 → 落盘相对路径的唯一构造点（u8path 清扫；导出供 unittest
///逐字节钉，与 isSafeArchivePath 同一导出先例）。
///
///条目名是归档里的原始字节（UTF-8 类名家族 / 混淆器残字节），先经
///text_codec::toCanonicalUtf8 单一规范路径（合法 UTF-8 逐字节原样、GBK →
///UTF-8、残字节 U+FFFD——已合入语义，不另设第二套），再显式
///std::filesystem::u8path 构造，杜绝 ACP 隐式窄转码 mojibake。
std::filesystem::path archiveEntryRelativePath(const std::string& entryName);

///@brief mjt 支持的外部 Java 反编译器。
enum class DecompilerKind
{
    Cfr,
    Vineflower,
};

///@brief 外部反编译请求。
struct DecompileRequest
{
    ///@brief 输入 `.class` 或 `.jar` 路径。
    std::string inputPath;
    ///@brief 单 class 的 `.java` 文件，或 JAR 的源码输出目录。
    std::string outputPath;
    ///@brief 反编译器类型。
    DecompilerKind kind = DecompilerKind::Cfr;
    ///@brief 显式反编译器 JAR；为空时按环境变量发现。
    std::string decompilerJar;
    ///@brief Java 可执行程序；为空时按环境变量和 PATH 发现。
    std::string javaProgram;
    ///@brief 子进程超时。
    std::chrono::milliseconds timeout{120000};
    ///@brief JVM 最大堆内存，单位 MiB。
    ca::i32 maxMemoryMiB = 1024;
    ///@brief 可选 mapping 文件；设置后先重映射 JAR 再反编译。
    std::string mappingPath;
    ///@brief mapping 格式名。
    std::string mappingFormat;
    ///@brief mapping 重写策略。
    std::string mappingPolicy = "bytecode-only";
    ///@brief 临时目录根路径；为空时使用系统临时目录。
    std::string tempRoot;
    ///@brief 可选协作取消查询；返回 true 时终止正在运行的反编译器子进程。
    std::function<bool()> cancellationRequested;
};

///@brief 外部反编译执行结果。
struct DecompileResult
{
    ///@brief 实际使用的反编译器 JAR。
    std::string decompilerJar;
    ///@brief 子进程标准输出。
    std::string stdoutText;
    ///@brief 子进程标准错误。
    std::string stderrText;
};

///@brief 后端正常退出但产物缺席时的根因摘要：stderr（空则退 stdout）经
///规范化为合法 UTF-8 后取根因行并截断；无任何输出返回空串。供调用方拼进
///"产物缺失"错误消息，分诊「输入触发后端内部异常」还是「工具侧问题」。
std::string composeBackendSummary(const std::string& stderrText, const std::string& stdoutText);

///@brief 一个反编译器后端在当前环境中的发现结果。
struct DecompilerAvailability final
{
    ///@brief 后端枚举。
    DecompilerKind kind = DecompilerKind::Cfr;
    ///@brief 稳定 backend ID。
    std::string    id;
    ///@brief 人类可读名称。
    std::string    name;
    ///@brief 该后端使用的显式环境变量。
    std::string    environmentVariable;
    ///@brief JAR 实际来源层：`explicit`（请求显式路径）/ `env`（环境变量命中）/
    ///        `package_dir`（exe 旁 package 目录兜底）；与发现优先级一致。
    std::string    discoverySource;
    ///@brief discovery 解析后的 JAR 路径。
    std::string    decompilerJar;
    ///@brief discovery 解析后的 Java 程序。
    std::string    javaProgram;
    ///@brief JAR 自报版本（MANIFEST Implementation-Version / Specification-Version，
    ///        缺失时回退 JAR 文件名 `name-<version>.jar` 后缀）；无法确定时为空。
    std::string    version;
    ///@brief JAR 是否为可读取常规文件。
    bool           artifactAvailable = false;
    ///@brief Java 是否能通过显式路径或 PATH 找到。
    bool           javaAvailable = false;
    ///@brief 不可用时的稳定可操作原因；可用时为空。
    std::string    unavailableReason;
};

///@brief 拉起 CFR 或 Vineflower 反编译 class/JAR，并归一化到请求的输出位置。
///@note mapping 目前只支持 JAR 输入；函数在返回前清理自己创建的临时目录。
ca::StatusOr<DecompileResult> decompile(const DecompileRequest& request);

///@brief 解析 `cfr` 或 `vineflower` 名称。
ca::StatusOr<DecompilerKind> parseDecompilerKind(const std::string& name);

///@brief 返回稳定的 `cfr` 或 `vineflower` 后端 ID。
const char* decompilerKindId(DecompilerKind kind) noexcept;

///@brief 按与真实执行相同的优先级发现反编译器 JAR 和 Java。
///@param decompilerJar 非空时作为本次显式 JAR；空值继续查询环境变量和 home。
///@param javaProgram 非空时作为本次显式 Java；空值继续查询环境变量、JAVA_HOME 和 PATH。
DecompilerAvailability inspectDecompiler(DecompilerKind kind,
                                         const std::string& decompilerJar = {},
                                         const std::string& javaProgram = {});

///@brief 读取反编译器 JAR 自报版本（导出供 unittest 逐用例钉，先例同
///        discoverDecompilerJarDetailed）。
///
///顺序：MANIFEST `Implementation-Version` → `Specification-Version` → JAR
///文件名 `name-<version>.jar` 后缀（首段必须以数字开头，避免把
///`fake-mcp-decompiler.jar` 这类词尾当版本）。读取/解析失败一律返回空串，
///不抛异常；版本串截断到 64 字节并去掉首尾空白。
///
///@param jarPath UTF-8 编码的 JAR 路径。
std::string decompilerJarVersion(const std::string& jarPath);

///@brief 反编译器 JAR 发现结果。
struct DecompilerJarDiscovery final
{
    ///@brief 命中的 JAR 路径；全部候选未命中时为空。
    std::string jar;
    ///@brief 按探测顺序记录的全部候选路径/目录，供报错文案列清单。
    std::vector<std::string> probed;
};

///@brief 按 fallback 序发现反编译器 JAR，并带回已探测候选清单。
///
///优先级：显式 JAR → MJT_CFR_JAR / MJT_VINEFLOWER_JAR → MJT_DECOMPILER_HOME
///固定名候选 → 可执行文件同级 `<exe_dir>/package` 与 `<exe_dir>/../package`
///按「前缀 + .jar」目录扫描（mjt-mcp 发布包 / 开发树 artifact 布局，发布件
///文件名带版本）。全空时 jar 为空、probed 非空。
///
///@param request 反编译请求；仅消费 decompilerJar 与 kind。
///@param executablePath 测试注入用可执行文件路径；空值取当前进程可执行文件。
DecompilerJarDiscovery discoverDecompilerJarDetailed(const DecompileRequest& request,
                                                     const std::string& executablePath = {});

}   //namespace mjt
