#include "resource_jasm_service.hpp"

#include <jbytecode/jbytecode.hpp>
#include "mbase/hierarchy/class_hierarchy_analysis.hpp"
#include "libca/core/tag_cast.hpp"

#include <exception>
#include <iterator>
#include <memory>
#include <utility>

namespace mjt::base::jasm {
namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

template<typename T>
Result<T> resourceError(const std::string& message)
{
    Result<T> result;
    result.diagnostics.push_back(
        {DiagnosticSeverity::Error, DiagnosticStage::Resource, utf16(message), std::nullopt});
    return result;
}

ca::str::Utf8String readInternalName(const std::vector<ca::u8>& bytes)
{
    ca::str::Utf8StringArena     arena;
    jbytecode::core::ClassReader reader(arena, bytes);
    const auto                   name = reader.getClassName();
    return ca::str::Utf8String::from_data(name.data(), name.byte_length(), name.length());
}

void appendDiagnostics(std::vector<Diagnostic>& target, std::vector<Diagnostic> source)
{
    target.insert(target.end(),
                  std::make_move_iterator(source.begin()),
                  std::make_move_iterator(source.end()));
}

ResourceContainer candidateContainer(const ResourceContainer&   container,
                                     const std::string&         targetUri,
                                     const std::vector<ca::u8>& candidateBytes)
{
    ResourceContainer candidate;
    container.visitResources([&](const std::string& uri, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        candidate.addResource(uri,
                              std::make_unique<ClassResource>(
                                  classResource->getLocation(),
                                  uri == targetUri ? candidateBytes : classResource->get()));
    });
    return candidate;
}

}   //namespace

Result<JvmJasmDisassembly> ResourceJasmService::disassemble(const std::string& uri) const
{
    const auto* resource = ca::core::tag_cast::dyn_cast<const ClassResource>(container_->getResource(uri));
    if (!resource) return resourceError<JvmJasmDisassembly>("Class resource not found: " + uri);
    return jasm_.disassemble(resource->get());
}

Result<JvmJasmClass> ResourceJasmService::assemble(const std::string&      uri,
                                                   ca::str::Utf16StringRef sourceName,
                                                   ca::str::Utf16StringRef source,
                                                   JvmJasmContext          context)
{
    IResource* rawResource = container_->getResource(uri);
    if (!ca::core::tag_cast::isa<ClassResource>(rawResource))
        return resourceError<JvmJasmClass>("Class resource not found: " + uri);
    auto* resource = ca::core::tag_cast::cast<ClassResource>(rawResource);

    try {
        const ca::str::Utf8String expectedName = readInternalName(resource->get());
        if (!analysisManager_->isRegistered<ClassHierarchyAnalysis>())
            analysisManager_->registerAnalysis<ClassHierarchyAnalysis>();
        auto& currentHierarchy = analysisManager_->getResult<ClassHierarchyAnalysis>(*container_);

        context.hierarchy = &currentHierarchy;
        auto preliminary  = jasm_.assemble(sourceName, source, context);
        if (!preliminary.isOk()) return preliminary;
        if (preliminary.value->internalName != expectedName) {
            auto result = resourceError<JvmJasmClass>(
                "Class name mismatch for " + uri + ": expected " + expectedName.to_std_string() +
                ", got " + preliminary.value->internalName.to_std_string());
            appendDiagnostics(result.diagnostics, std::move(preliminary.diagnostics));
            return result;
        }

        auto candidate = candidateContainer(*container_, uri, preliminary.value->bytes);
        ca::str::Utf8StringArena candidateArena;
        ClassHierarchy           candidateHierarchy;
        candidateHierarchy.build(candidate, candidateArena);

        context.hierarchy = &candidateHierarchy;
        auto compiled     = jasm_.assemble(sourceName, source, context);
        if (!compiled.isOk()) return compiled;
        if (compiled.value->internalName != expectedName)
            return resourceError<JvmJasmClass>("Class name changed between compilation passes");

        auto validated = jasm_.validate(compiled.value->bytes, context);
        if (!validated.isOk()) {
            appendDiagnostics(validated.diagnostics, std::move(compiled.diagnostics));
            return validated;
        }

        JvmJasmClass            output            = std::move(*compiled.value);
        std::vector<Diagnostic> outputDiagnostics = std::move(compiled.diagnostics);
        appendDiagnostics(outputDiagnostics, std::move(validated.diagnostics));
        resource->set(output.bytes);
        analysisManager_->clear(*container_);
        return {std::move(output), std::move(outputDiagnostics)};
    }
    catch (const std::exception& exception) {
        return resourceError<JvmJasmClass>(exception.what());
    }
}

}   //namespace mjt::base::jasm
