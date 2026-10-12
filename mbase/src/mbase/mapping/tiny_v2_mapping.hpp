#pragma once
#include "mapping_file_format.hpp"

namespace mjt::base {

class TinyV2Mapping : public MappingFileFormat
{
public:
    ca::str::Utf8StringRef getFormatName() const override;

    IntermediateMapping parse(ca::str::Utf8StringRef mappingText) override;

    ca::str::Utf8String format(const IntermediateMapping& mappings) override;
};

}   //namespace mjt::base
