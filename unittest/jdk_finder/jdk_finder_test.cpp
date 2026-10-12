#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

namespace {

std::string readEnv(const char* name, bool& exists)
{
#ifdef _WIN32
    char*  value = nullptr;
    size_t size  = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        exists = false;
        return {};
    }
    exists = true;
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    exists            = value != nullptr;
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

void setEnv(const std::string& name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    if (value.empty()) unsetenv(name.c_str());
    else setenv(name.c_str(), value.c_str(), 1);
#endif
}

class ScopedEnv final
{
public:
    ScopedEnv(std::string name, const std::string& value)
        : name_(std::move(name))
        , oldValue_(readEnv(name_.c_str(), existed_))
    {
        setEnv(name_, value);
    }

    ~ScopedEnv() { setEnv(name_, existed_ ? oldValue_ : std::string{}); }

    ScopedEnv(const ScopedEnv&)            = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    std::string oldValue_;
    bool        existed_ = false;
};

fs::path freshDir(const std::string& name)
{
    fs::path dir = test::getTempOutPath("libjdk_finder/" + name + "/.keep").parent_path();
    std::error_code ec;
    fs::remove_all(dir, ec);
    EXPECT_FALSE(ec) << ec.message();
    fs::create_directories(dir, ec);
    EXPECT_FALSE(ec) << ec.message();
    return dir;
}

void touch(const fs::path& path)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    ASSERT_FALSE(ec) << ec.message();
    std::ofstream out(path);
    ASSERT_TRUE(out.good()) << path;
}

void touchExecutable(const fs::path& path)
{
    touch(path);
#ifndef _WIN32
    std::error_code ec;
    fs::permissions(path,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add,
                    ec);
    ASSERT_FALSE(ec) << ec.message();
#endif
}

}   //namespace

TEST(JdkFinderTest, ContinuesAfterCandidateWithoutJniHeader)
{
    fs::path root    = freshDir("jdk_fallback");
    fs::path invalid = root / "invalid";
    fs::path valid   = root / "valid";
    touch(invalid / "bin" / "javac");
    touch(valid / "bin" / "javac");
    touch(valid / "include" / "jni.h");

    ScopedEnv javaHome("JAVA_HOME", invalid.string());
    ScopedEnv mjtHome("MJT_JDK_HOME", valid.string());
    jdk_finder::FindOptions opts;
    opts.requireJniHeader = true;

    auto result = jdk_finder::findJdk(opts);

    ASSERT_TRUE(result.available) << result.reason;
    EXPECT_EQ(valid.lexically_normal(), result.javaHome.lexically_normal());
}

TEST(JdkFinderTest, SharedScriptStopsAfterFirstFailure)
{
#ifdef _WIN32
    const char* failingCommand = "cmd /c exit 7";
#else
    const char* failingCommand = "false";
#endif
    auto result = test::jdk::runBat({"echo before", failingCommand, "echo should-not-run"});

    EXPECT_NE(0, result.exitCode);
    EXPECT_NE(std::string::npos, result.out.find("before"));
    EXPECT_EQ(std::string::npos, result.out.find("should-not-run"));
}

TEST(JdkFinderTest, ExplicitMsvcReturnsVcvarsPath)
{
#ifndef _WIN32
    GTEST_SKIP() << "MSVC probing is Windows-only";
#else
    fs::path root   = freshDir("explicit_msvc");
    fs::path tools  = root / "VC" / "Tools" / "MSVC" / "14.40";
    fs::path vcvars = root / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
    touch(vcvars);

    ScopedEnv include("INCLUDE", "");
    ScopedEnv vcTools("VCToolsInstallDir", tools.string());
    auto result = jdk_finder::findNativeCompiler(jdk_finder::CCompilerKind::Msvc);

    ASSERT_TRUE(result.available) << result.reason;
    EXPECT_EQ(vcvars.lexically_normal(), result.vcvarsAll.lexically_normal());
#endif
}

TEST(JdkFinderTest, ExplicitZigUsesZigExecutableOnPath)
{
    fs::path bin = freshDir("explicit_zig") / "bin";
#ifdef _WIN32
    touchExecutable(bin / "zig.exe");
    constexpr char separator = ';';
#else
    touchExecutable(bin / "zig");
    constexpr char separator = ':';
#endif
    bool        pathExists = false;
    std::string oldPath    = readEnv("PATH", pathExists);
    ScopedEnv path("PATH", bin.string() + separator + (pathExists ? oldPath : std::string{}));

    auto result = jdk_finder::findNativeCompiler(jdk_finder::CCompilerKind::Zig);

    ASSERT_TRUE(result.available) << result.reason;
    EXPECT_EQ(jdk_finder::CCompilerKind::Zig, result.compiler);
}

TEST(JdkFinderTest, FindsVisualStudio2019Install)
{
#ifndef _WIN32
    GTEST_SKIP() << "Visual Studio probing is Windows-only";
#else
    fs::path root   = freshDir("vs2019");
    fs::path vcvars = root / "Microsoft Visual Studio" / "2019" / "BuildTools" / "VC" /
                      "Auxiliary" / "Build" / "vcvarsall.bat";
    touch(vcvars);

    ScopedEnv vcTools("VCToolsInstallDir", "");
    ScopedEnv programFiles("ProgramFiles", root.string());
    ScopedEnv programFilesX86("ProgramFiles(x86)", "");

    EXPECT_EQ(vcvars.lexically_normal(), jdk_finder::findVcvarsAll().lexically_normal());
#endif
}
