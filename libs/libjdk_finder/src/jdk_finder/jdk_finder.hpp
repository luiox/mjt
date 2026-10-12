//jdk_finder.hpp — JDK 与原生工具链探测
//
//服务于需要定位 JDK（JNI 头）和 C 编译器（MSVC/gcc/clang/Zig）的产品功能与测试。
//当前消费方：
//- 测试侧（javac/java 编译运行样本）
//
//零外部依赖（仅 std + libca），可被任意产品库和 test_helper 共用，避免探测逻辑
//在产品代码与测试代码各写一份。
//
//设计要点：
//- JDK 发现顺序：JAVA_HOME → MJT_JDK_HOME → fallbackHomes（调用方注入）
//不再硬编码作者机器路径；CI 靠 JAVA_HOME，本机开发可设 MJT_JDK_HOME。
//- 可执行文件双探测：先 <bin>/<name>.exe 再 <bin>/<name>（跨平台友好）。
//- JNI 头校验可选：纯 JVM 用途不要求 jni.h；--compile 等 native 用途要求。

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

namespace jdk_finder {

namespace fs = std::filesystem;

///JDK 定位结果。available=false 时 reason 填说明。
struct JdkLocation
{
    bool        available = false;
    std::string reason;
    fs::path    javaHome;
    fs::path    jdkInclude;     //<home>/include
    fs::path    jdkIncludePlat; //<home>/include/{win32|darwin|linux}
};

///findJdk 的可选配置。
struct FindOptions
{
    ///JAVA_HOME / MJT_JDK_HOME 都没命中时的额外回落候选（按顺序尝试）。
    std::vector<fs::path> fallbackHomes;
    ///是否要求 <jdk>/include/jni.h 存在。默认 false：纯 JVM 用途不需要。
    ///native 编译（--compile）设 true。
    bool requireJniHeader = false;
};

///探测 JDK：JAVA_HOME → MJT_JDK_HOME → opts.fallbackHomes。
///命中需存在 bin/javac(.exe)（视为合法 JDK 安装的标志）。
///requireJniHeader=true 时额外校验 <jdk>/include/jni.h。
JdkLocation findJdk(const FindOptions& opts = {});

///在已定位的 JDK 下解析可执行文件绝对路径（双探测 .exe / 无后缀）。
///name 形如 "javac" / "java"（不带后缀）。找不到返回空 path。
fs::path jdkExecutable(const JdkLocation& jdk, const std::string& name);

///原生 C 编译器种类。
enum class CCompilerKind
{
    Auto,
    Msvc,
    Gcc,
    Clang,
    Zig,
};

///探测到的原生工具链。
struct NativeToolchain
{
    bool           available = false;
    std::string    reason;
    CCompilerKind  compiler = CCompilerKind::Auto;
    fs::path       vcvarsAll;   //仅 MSVC：vcvarsall.bat 路径（INCLUDE 环境未配置时需要）
};

///探测原生 C 编译器。requested=Auto 时按平台默认顺序探测。
///Windows：MSVC（cl 在 PATH 或 vcvarsall 可定位）→ clang → gcc → Zig
///macOS：clang → gcc → Zig
///Linux：gcc → clang → Zig
NativeToolchain findNativeCompiler(CCompilerKind requested = CCompilerKind::Auto);

///定位 VS 的 vcvarsall.bat（Windows）。
///顺序：VCToolsInstallDir 环境变量 → ProgramFiles/ProgramFiles(x86) 下
///Microsoft Visual Studio/{2022,2019,2017}/{Community,Professional,Enterprise,BuildTools}
///扫描。
///非 Windows 或未找到返回空 path。
fs::path findVcvarsAll();

///检查命令是否在 PATH 中可用（where / command -v）。
bool commandExists(const std::string& command);

}   //namespace jdk_finder
