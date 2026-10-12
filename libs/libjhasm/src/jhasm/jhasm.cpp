#include "jhasm.hpp"

namespace jhasm {

const char* get_lib_version() noexcept
{
    return "0.2.0";
}

const char* get_upstream_revision() noexcept
{
    return "aacebfa8b06a5c478adf13d9ce8e0e9f896ba840";
}

error::Result<compile::JvmCompileResult> assemble(ca::str::Utf16StringRef     sourceName,
                                                  ca::str::Utf16StringRef     source,
                                                  compile::JvmCompilerOptions options)
{
    auto tokens = parser::Tokenizer{}.tokenize(sourceName, source);
    if (!tokens.is_ok())
        return error::Result<compile::JvmCompileResult>(
            std::nullopt, tokens.errors(), tokens.warnings());
    auto declarations = parser::DeclarationParser{}.parseAny(*tokens.get());
    if (declarations.has_err())
        return error::Result<compile::JvmCompileResult>(std::nullopt, declarations.errors(), {});
    auto processed = parser::ASTProcessor{}.process(declarations.get());
    if (!processed.is_ok())
        return error::Result<compile::JvmCompileResult>(
            std::nullopt, processed.errors(), processed.warnings());
    return compile::JvmCompiler(options).compile(*processed.get());
}

error::Result<ca::str::Utf16String> disassemble(const std::vector<ca::u8>& classBytes)
{
    return printer::JvmPrinter{}.print(classBytes);
}

error::Result<ca::str::Utf16String> disassembleMethod(const std::vector<ca::u8>&    classBytes,
                                                       ca::str::Utf8StringRef methodName,
                                                       ca::str::Utf8StringRef descriptor)
{
    return printer::JvmPrinter{}.printMethod(classBytes, methodName, descriptor);
}

error::Result<analysis::JvmAnalysisResult> analyze(const std::vector<ca::u8>&          classBytes,
                                                   const analysis::InheritanceChecker& inheritance)
{
    return analysis::JvmAnalyzer(inheritance).analyze(classBytes);
}

}   //namespace jhasm
