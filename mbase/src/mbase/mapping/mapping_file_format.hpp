#pragma once
#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "invalid_mapping_exception.hpp"
#include "intermediate_mapping.hpp"

namespace mjt::base {

class MappingFileFormat
{
public:
    virtual ~MappingFileFormat() = default;

    virtual ca::str::Utf8StringRef getFormatName() const = 0;

    virtual IntermediateMapping parse(ca::str::Utf8StringRef mappingText) = 0;

    virtual ca::str::Utf8String format(const IntermediateMapping& mappings) = 0;
};

}   //namespace mjt::base
