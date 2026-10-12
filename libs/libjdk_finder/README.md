# libjdk_finder

JDK 与原生 C 工具链（MSVC / gcc / clang / Zig）探测库。零外部依赖（仅 std + libca）。

## 为什么独立成库

测试需要真实调用 `javac` / `java` 编译运行样本，而这类探测又不该把耦合带进
业务层。独立成库让探测逻辑只有一份实现，同时不污染消费方的依赖图。

## API

```cpp
#include "jdk_finder/jdk_finder.hpp"

namespace jdk_finder {
    // JDK 发现
    struct JdkLocation { bool available; std::string reason;
                         fs::path javaHome, jdkInclude, jdkIncludePlat; };
    struct FindOptions { std::vector<fs::path> fallbackHomes;
                         bool requireJniHeader = false; };
    JdkLocation findJdk(const FindOptions& opts = {});
    fs::path    jdkExecutable(const JdkLocation& jdk, const std::string& name);

    // 原生 C 编译器发现
    enum class CCompilerKind { Auto, Msvc, Gcc, Clang, Zig };
    struct NativeToolchain { bool available; std::string reason;
                             CCompilerKind compiler; fs::path vcvarsAll; };
    NativeToolchain findNativeCompiler(CCompilerKind requested = CCompilerKind::Auto);
    fs::path        findVcvarsAll();
    bool            commandExists(const std::string& command);
}
```

## JDK 发现顺序

`JAVA_HOME` → `MJT_JDK_HOME` → `FindOptions::fallbackHomes`。
命中判据：候选根下存在 `bin/javac`（或 `.exe`）。`requireJniHeader=true` 时额外校验
`<jdk>/include/jni.h`，供 native 编译场景使用。

Windows 的 MSVC 初始化脚本按 `VCToolsInstallDir`、Visual Studio 2022、2019、2017
的顺序探测；未初始化的 MSVC 结果会在 `NativeToolchain::vcvarsAll` 返回脚本路径。
Zig 探测检查 PATH 中的 `zig`，在各平台默认编译器之后作为末位 fallback。

## 与测试侧 helper 的关系

`test_helper/jdk_fixture.hpp` 在本库之上提供测试专用能力（`runBat` / `compileJava` /
`runJavaMain`——Windows 使用临时 `.bat`、POSIX 使用临时 `sh` 脚本执行 javac/java 并捕获
输出），这些是测试基础设施，不属于本库职责。
