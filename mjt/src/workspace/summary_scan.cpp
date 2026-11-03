#include "summary_scan.hpp"

#include "jbc/core/reader.hpp"

namespace mjt::workspace {

namespace {

/// @brief std::string → 拥有型 Utf8String（DTO 文本字段赋值口径，与
///        analysis_ingest 的 utf8() 同款：cp modified UTF-8 字节保真，不二次校验）。
ca::str::Utf8String utf8(const std::string& value)
{
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

}   // namespace

WorkspaceResult<ClassSummary> decodeClassSummary(const std::string&          uri,
                                                 const model::ClassResource& resource)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(resource.get()));
    if (read.is_err())
        return ca::core::Err(WorkspaceErrorInfo{
            WorkspaceError::DataLoss,
            "decode class summary " + uri + ": " +
                jbc::core::to_string(std::move(read).unwrap_err())});
    auto                        parsed = std::move(read).unwrap();
    const jbc::core::ClassFile& cf     = *parsed.file;

    ClassSummary output;
    output.resourceUri   = utf8(uri);
    output.internalName  = utf8(std::string{cf.thisName()});
    output.superName     = utf8(std::string{cf.superName()});
    output.access        = static_cast<ca::i32>(cf.access);
    // ASM 版本语义：version = major | (minor << 16)。
    output.version       = static_cast<ca::i32>(cf.major) | (static_cast<ca::i32>(cf.minor) << 16);
    output.interfaceCount = cf.interfaces.size();
    output.fieldCount     = cf.fields.size();
    output.methodCount    = cf.methods.size();
    return ca::core::Ok(std::move(output));
}

}   // namespace mjt::workspace
