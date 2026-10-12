#include "jvm_jasm_service.hpp"

#include <jbytecode/jbytecode.hpp>
#include <jhasm/jhasm.hpp>
#include <jhasm/jhasm.hpp>
#include "mbase/hierarchy/class_hierarchy.hpp"

#include <jblw/version/java_version.hpp>

#include <exception>
#include <string>

namespace mjt::base::jasm {
namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

ca::str::Utf8String own(ca::str::Utf8StringRef value)
{
    return ca::str::Utf8String::from_data(value.data(), value.byte_length(), value.length());
}

class HierarchyInheritanceChecker final : public ::jhasm::analysis::InheritanceChecker
{
public:
    explicit HierarchyInheritanceChecker(const ClassHierarchy& hierarchy)
        : hierarchy_(&hierarchy)
    {}

    bool isSubclassOf(ca::str::Utf8StringRef child, ca::str::Utf8StringRef parent) const override
    {
        const ClassIndex childIndex  = hierarchy_->indexOf(child);
        const ClassIndex parentIndex = hierarchy_->indexOf(parent);
        if (childIndex == INVALID_CLASS || parentIndex == INVALID_CLASS) return child == parent;
        return hierarchy_->isSubType(childIndex, parentIndex);
    }

    ca::str::Utf8String commonSuperclass(ca::str::Utf8StringRef first,
                                         ca::str::Utf8StringRef second) const override
    {
        if (first == second) return own(first);
        const ClassIndex firstIndex  = hierarchy_->indexOf(first);
        const ClassIndex secondIndex = hierarchy_->indexOf(second);
        if (firstIndex == INVALID_CLASS || secondIndex == INVALID_CLASS)
            return ca::str::Utf8String("java/lang/Object");
        const ClassIndex commonIndex = hierarchy_->commonSuper(firstIndex, secondIndex);
        if (commonIndex == INVALID_CLASS) return ca::str::Utf8String("java/lang/Object");
        return own(hierarchy_->nameOf(commonIndex));
    }

private:
    const ClassHierarchy* hierarchy_;
};

SourceLocation sourceLocation(const ::jhasm::util::Location& location)
{
    SourceLocation result;
    result.line   = location.line();
    result.column = location.column();
    result.length = location.length();
    if (location.has_source()) result.source = location.source().to_string();
    return result;
}

Diagnostic diagnostic(const ::jhasm::error::Error& error, DiagnosticSeverity severity,
                      DiagnosticStage stage)
{
    std::optional<SourceLocation> location;
    if (error.location()) location = sourceLocation(*error.location());
    return {severity, stage, error.message().to_string(), std::move(location)};
}

template<typename T>
std::vector<Diagnostic> diagnostics(const ::jhasm::error::Result<T>& result, DiagnosticStage stage)
{
    std::vector<Diagnostic> output;
    output.reserve(result.errors().size() + result.warnings().size());
    for (const auto& error : result.errors())
        output.push_back(diagnostic(error, DiagnosticSeverity::Error, stage));
    for (const auto& warning : result.warnings())
        output.push_back(diagnostic(warning, DiagnosticSeverity::Warning, stage));
    return output;
}

template<typename T>
Result<T> exceptionResult(DiagnosticStage stage, const std::exception& exception)
{
    Result<T> result;
    result.diagnostics.push_back(
        {DiagnosticSeverity::Error, stage, utf16(exception.what()), std::nullopt});
    return result;
}

ca::str::Utf8String readInternalName(const std::vector<ca::u8>& classBytes)
{
    ca::str::Utf8StringArena     arena;
    jbytecode::core::ClassReader reader(arena, classBytes);
    return own(reader.getClassName());
}

}   //namespace

Result<JvmJasmClass> JvmJasmService::assemble(ca::str::Utf16StringRef sourceName,
                                              ca::str::Utf16StringRef source,
                                              const JvmJasmContext&   context) const
{
    try {
        ::jhasm::compile::JvmCompilerOptions options;
        options.version      = jblw::version::JavaVersion::classVersion(context.classMajorVersion,
                                                                   context.classMinorVersion);
        options.maxStackHint = context.maxStackHint;

        std::optional<HierarchyInheritanceChecker> checker;
        if (context.hierarchy) {
            checker.emplace(*context.hierarchy);
            options.inheritance = &*checker;
        }

        auto                 compiled = ::jhasm::assemble(sourceName, source, options);
        Result<JvmJasmClass> result;
        result.diagnostics = diagnostics(compiled, DiagnosticStage::Assemble);
        if (!compiled.is_ok()) return result;

        auto         value = compiled.take_value();
        JvmJasmClass output;
        output.internalName = readInternalName(value->bytes);
        output.bytes        = std::move(value->bytes);
        result.value        = std::move(output);
        return result;
    }
    catch (const std::exception& exception) {
        return exceptionResult<JvmJasmClass>(DiagnosticStage::Assemble, exception);
    }
}

Result<JvmJasmDisassembly> JvmJasmService::disassemble(const std::vector<ca::u8>& classBytes) const
{
    try {
        auto                       internalName = readInternalName(classBytes);
        auto                       disassembled = jhasm::disassemble(classBytes);
        Result<JvmJasmDisassembly> result;
        result.diagnostics = diagnostics(disassembled, DiagnosticStage::Disassemble);
        if (!disassembled.is_ok()) return result;

        auto source  = disassembled.take_value();
        result.value = JvmJasmDisassembly{std::move(internalName), std::move(*source)};
        return result;
    }
    catch (const std::exception& exception) {
        return exceptionResult<JvmJasmDisassembly>(DiagnosticStage::Disassemble, exception);
    }
}

Result<JvmJasmMethodDisassembly> JvmJasmService::disassembleMethod(
    const std::vector<ca::u8>& classBytes, ca::str::Utf8StringRef name,
    ca::str::Utf8StringRef descriptor) const
{
    try {
        auto internalName = readInternalName(classBytes);
        auto disassembled = jhasm::disassembleMethod(classBytes, name, descriptor);
        Result<JvmJasmMethodDisassembly> result;
        result.diagnostics = diagnostics(disassembled, DiagnosticStage::Disassemble);
        if (!disassembled.is_ok()) return result;

        auto source = disassembled.take_value();
        result.value = JvmJasmMethodDisassembly{std::move(internalName), own(name), own(descriptor),
                                                 std::move(*source)};
        return result;
    }
    catch (const std::exception& exception) {
        return exceptionResult<JvmJasmMethodDisassembly>(DiagnosticStage::Disassemble, exception);
    }
}

Result<JvmJasmClass> JvmJasmService::validate(const std::vector<ca::u8>& classBytes,
                                              const JvmJasmContext&      context) const
{
    try {
        auto                                         internalName = readInternalName(classBytes);
        std::optional<HierarchyInheritanceChecker>   checker;
        const ::jhasm::analysis::InheritanceChecker* inheritance =
            &::jhasm::analysis::EmptyInheritanceChecker::instance();
        if (context.hierarchy) {
            checker.emplace(*context.hierarchy);
            inheritance = &*checker;
        }

        auto                 analyzed = ::jhasm::analyze(classBytes, *inheritance);
        Result<JvmJasmClass> result;
        result.diagnostics = diagnostics(analyzed, DiagnosticStage::Validate);
        if (!analyzed.is_ok()) return result;
        result.value = JvmJasmClass{std::move(internalName), classBytes};
        return result;
    }
    catch (const std::exception& exception) {
        return exceptionResult<JvmJasmClass>(DiagnosticStage::Validate, exception);
    }
}

}   //namespace mjt::base::jasm
