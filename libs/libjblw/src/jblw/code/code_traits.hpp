#pragma once

#include "generic/code_model.hpp"

#include "libca/core/tag_cast.hpp"

namespace ca::core::tag_cast {

#define JBLW_CODE_TYPE_OF(typeName, typeTag)                                \
    template<>                                                              \
    struct TypeOf<jblw::code::typeName>                                     \
    {                                                                       \
        static constexpr auto value = jblw::code::CodeElementKind::typeTag; \
    }

JBLW_CODE_TYPE_OF(Label, LABEL);
JBLW_CODE_TYPE_OF(GenericLabel, LABEL);
JBLW_CODE_TYPE_OF(SimpleInstruction, SIMPLE);
JBLW_CODE_TYPE_OF(StringConstantInstruction, CONSTANT_STRING);
JBLW_CODE_TYPE_OF(MethodHandleConstantInstruction, CONSTANT_METHOD_HANDLE);
JBLW_CODE_TYPE_OF(DynamicConstantInstruction, CONSTANT_DYNAMIC);
JBLW_CODE_TYPE_OF(LongConstantInstruction, CONSTANT_LONG);
JBLW_CODE_TYPE_OF(DoubleConstantInstruction, CONSTANT_DOUBLE);
JBLW_CODE_TYPE_OF(IntConstantInstruction, CONSTANT_INT);
JBLW_CODE_TYPE_OF(FloatConstantInstruction, CONSTANT_FLOAT);
JBLW_CODE_TYPE_OF(TypeConstantInstruction, CONSTANT_TYPE);
JBLW_CODE_TYPE_OF(AllocateInstruction, ALLOCATE);
JBLW_CODE_TYPE_OF(AllocateMultiDimArrayInstruction, ALLOCATE_MULTI_ARRAY);
JBLW_CODE_TYPE_OF(CheckCastInstruction, CHECK_CAST);
JBLW_CODE_TYPE_OF(InstanceofInstruction, INSTANCE_OF);
JBLW_CODE_TYPE_OF(ConditionalJumpInstruction, CONDITIONAL_JUMP);
JBLW_CODE_TYPE_OF(ImmediateJumpInstruction, IMMEDIATE_JUMP);
JBLW_CODE_TYPE_OF(LookupSwitchInstruction, LOOKUP_SWITCH);
JBLW_CODE_TYPE_OF(TableSwitchInstruction, TABLE_SWITCH);
JBLW_CODE_TYPE_OF(FieldInstruction, FIELD);
JBLW_CODE_TYPE_OF(MethodInstruction, METHOD);
JBLW_CODE_TYPE_OF(InvokeDynamicInstruction, INVOKE_DYNAMIC);
JBLW_CODE_TYPE_OF(PrimitiveConversionInstruction, PRIMITIVE_CONVERSION);
JBLW_CODE_TYPE_OF(VarInstruction, VARIABLE);
JBLW_CODE_TYPE_OF(VariableIncrementInstruction, VARIABLE_INCREMENT);

#undef JBLW_CODE_TYPE_OF

}   //namespace ca::core::tag_cast
