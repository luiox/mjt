#include "constant_value.hpp"

namespace mjt::base {

const ConstantValue ConstantValue::Null{jbytecode::analysis::BasicValue::REFERENCE_VALUE,
                                        ca::core::Any{}};

}
