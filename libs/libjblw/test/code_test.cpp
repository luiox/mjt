#include "jblw/code/code_traits.hpp"
#include "jblw/constant/of_int.hpp"
#include "jblw/type/types.hpp"

#include <gtest/gtest.h>

#include "libca/core/tag_cast.hpp"

#include <memory>
#include <vector>

using namespace jblw::code;
using namespace jblw::code::attribute;
using namespace jblw::code::attribute::generic;
using namespace jblw::constant;
using namespace jblw::type;

namespace {
class RecordingConversion final : public PrimitiveConversion
{
public:
    void i2l() override { opcodes.push_back(JavaOpcodes::I2L); }
    void i2f() override { opcodes.push_back(JavaOpcodes::I2F); }
    void i2d() override { opcodes.push_back(JavaOpcodes::I2D); }
    void l2i() override { opcodes.push_back(JavaOpcodes::L2I); }
    void l2f() override { opcodes.push_back(JavaOpcodes::L2F); }
    void l2d() override { opcodes.push_back(JavaOpcodes::L2D); }
    void f2i() override { opcodes.push_back(JavaOpcodes::F2I); }
    void f2l() override { opcodes.push_back(JavaOpcodes::F2L); }
    void f2d() override { opcodes.push_back(JavaOpcodes::F2D); }
    void d2i() override { opcodes.push_back(JavaOpcodes::D2I); }
    void d2l() override { opcodes.push_back(JavaOpcodes::D2L); }
    void d2f() override { opcodes.push_back(JavaOpcodes::D2F); }
    void i2b() override { opcodes.push_back(JavaOpcodes::I2B); }
    void i2c() override { opcodes.push_back(JavaOpcodes::I2C); }
    void i2s() override { opcodes.push_back(JavaOpcodes::I2S); }

    std::vector<ca::i32> opcodes;
};
}   //namespace

TEST(BlwCodeTest, CodeElementListMaintainsLabelIndicesAcrossMutation)
{
    auto            first = std::make_shared<GenericLabel>();
    auto            last  = std::make_shared<GenericLabel>();
    CodeElementList elements{
        first,
        std::make_shared<SimpleInstruction>(JavaOpcodes::NOP),
        last,
    };
    EXPECT_EQ(0, first->getIndex());
    EXPECT_EQ(2, last->getIndex());

    elements.insert(1, std::make_shared<SimpleInstruction>(JavaOpcodes::RETURN));
    EXPECT_EQ(3, last->getIndex());
    elements.remove(0);
    EXPECT_EQ(Label::UNSET, first->getIndex());
    EXPECT_EQ(2, last->getIndex());

    EXPECT_TRUE(ca::core::tag_cast::isa<GenericLabel>(last.get()));
    EXPECT_TRUE(ca::core::tag_cast::isa<SimpleInstruction>(elements[0].get()));
    EXPECT_EQ(nullptr, ca::core::tag_cast::dyn_cast<VarInstruction>(elements[0].get()));
}

TEST(BlwCodeTest, BranchAndSwitchInstructionsSelectSharedLabels)
{
    auto                    dflt = std::make_shared<GenericLabel>();
    auto                    one  = std::make_shared<GenericLabel>();
    auto                    two  = std::make_shared<GenericLabel>();
    LookupSwitchInstruction lookup({1, 5}, dflt, {one, two});
    EXPECT_EQ(one, lookup.select(1));
    EXPECT_EQ(two, lookup.select(5));
    EXPECT_EQ(dflt, lookup.select(4));
    EXPECT_FALSE(lookup.hasFallthrough());
    EXPECT_EQ(3U, lookup.targets().size());

    TableSwitchInstruction table(10, dflt, {one, two});
    EXPECT_EQ(std::vector<ca::i32>({10, 11}), table.keys());
    EXPECT_EQ(one, table.select(10));
    EXPECT_EQ(dflt, table.select(12));
    EXPECT_EQ(JavaOpcodes::TABLESWITCH, table.opcode());
}

TEST(BlwCodeTest, InstructionsPreserveStructuralAndConstantSemantics)
{
    auto owner =
        Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("example/Owner"));
    FieldInstruction first(JavaOpcodes::GETFIELD, owner, ca::str::Utf8String("value"), Types::INT);
    FieldInstruction second(JavaOpcodes::GETFIELD, owner, ca::str::Utf8String("value"), Types::INT);
    EXPECT_EQ(first, second);

    InstructionPtr constant = ConstantInstruction::wrap(std::make_shared<const OfInt>(7));
    EXPECT_EQ(ExtensionOpcodes::INT_CONSTANT, constant->opcode());
    EXPECT_TRUE(ca::core::tag_cast::isa<IntConstantInstruction>(constant.get()));
}

TEST(BlwCodeTest, PrimitiveConversionsExpandLikeBlw)
{
    RecordingConversion conversion;
    PrimitiveConversionInstruction(Types::LONG, Types::CHAR).accept(conversion);
    EXPECT_EQ(std::vector<ca::i32>({JavaOpcodes::L2I, JavaOpcodes::I2C}), conversion.opcodes);

    conversion.opcodes.clear();
    PrimitiveConversionInstruction(Types::INT, Types::INT).accept(conversion);
    EXPECT_TRUE(conversion.opcodes.empty());
    EXPECT_THROW(PrimitiveConversionInstruction(Types::VOID, Types::INT).accept(conversion),
                 std::logic_error);
}

TEST(BlwCodeTest, GenericBuilderOwnsCodeGraphAndWalkerCanJump)
{
    auto               start = std::make_shared<GenericLabel>();
    auto               end   = std::make_shared<GenericLabel>();
    GenericCodeBuilder builder;
    builder.maxStack(1).maxLocals(1);
    builder.codeList()
        .addLabel(start)
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::NOP))
        .addLabel(end)
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::RETURN));
    builder.tryCatchBlock({start, end, end, nullptr});
    builder.localVariable(std::make_shared<const GenericLocal>(
        start, end, 0, ca::str::Utf8String("this"), Types::OBJECT));

    CodePtr code = builder.build();
    EXPECT_EQ(start, code->start());
    EXPECT_EQ(4U, code->elements().size());
    EXPECT_EQ(1U, code->tryCatchBlocks().size());
    EXPECT_EQ(1U, code->localVariables().size());

    auto walker = code->walker();
    walker->advance();
    EXPECT_EQ(start.get(), walker->element());
    walker->set(*end);
    walker->advance();
    EXPECT_EQ(end.get(), walker->element());
}
