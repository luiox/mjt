#include "jblw/classfile/generic/builders.hpp"
#include "jblw/simulation/execution.hpp"
#include "jblw/simulation/simulation_ops.hpp"
#include "jblw/type/types.hpp"

#include <gtest/gtest.h>

#include <limits>

using namespace jblw::classfile;
using namespace jblw::classfile::generic;
using namespace jblw::code;
using namespace jblw::constant;
using namespace jblw::simulation;
using namespace jblw::type;

namespace {
class RecordingEngine final : public ExecutionEngine
{
public:
    void label(Label&) override { events.push_back(CodeElementKind::LABEL); }
    void execute(const SimpleInstruction& value) override
    {
        events.push_back(value.getType());
        specializedSimple = true;
    }
    void execute(const ConstantInstruction& value) override
    {
        events.push_back(value.getType());
        specializedConstant = true;
    }
    void executeInstruction(const Instruction& value) override
    {
        events.push_back(value.getType());
        fallback = true;
    }

    std::vector<CodeElementKind> events;
    bool                         specializedSimple   = false;
    bool                         specializedConstant = false;
    bool                         fallback            = false;
};
}   //namespace

TEST(BlwSimulationTest, StraightforwardSimulationWalksAndDispatchesCode)
{
    GenericMethodBuilder builder(
        AccessFlag::ACC_PUBLIC, ca::str::Utf8String("run"), Types::methodType(Types::VOID));
    builder.code()
        .codeList()
        .addLabel(std::make_shared<GenericLabel>())
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::NOP))
        .addInstruction(std::make_shared<IntConstantInstruction>(std::make_shared<const OfInt>(7)))
        .addInstruction(std::make_shared<VarInstruction>(JavaOpcodes::ILOAD, 0));
    MethodPtr       method = builder.build();
    RecordingEngine engine;

    StraightforwardSimulation().execute(engine, *method);
    ASSERT_EQ(4U, engine.events.size());
    EXPECT_EQ(CodeElementKind::LABEL, engine.events[0]);
    EXPECT_EQ(CodeElementKind::SIMPLE, engine.events[1]);
    EXPECT_EQ(CodeElementKind::CONSTANT_INT, engine.events[2]);
    EXPECT_EQ(CodeElementKind::VARIABLE, engine.events[3]);
    EXPECT_TRUE(engine.specializedSimple);
    EXPECT_TRUE(engine.specializedConstant);
    EXPECT_TRUE(engine.fallback);
}

TEST(BlwSimulationTest, StraightforwardSimulationRequiresCode)
{
    GenericMethodBuilder builder(
        AccessFlag::ACC_ABSTRACT, ca::str::Utf8String("run"), Types::methodType(Types::VOID));
    RecordingEngine engine;
    EXPECT_THROW(StraightforwardSimulation().execute(engine, *builder.build()),
                 SimulationException);
}

TEST(BlwSimulationTest, IntegerOpsFollowJvmOverflowDivisionAndShiftRules)
{
    using O = JavaOpcodes;
    EXPECT_EQ(std::numeric_limits<ca::i32>::min(),
              SimulationOps::execute(O::IADD, 1, std::numeric_limits<ca::i32>::max()));
    EXPECT_EQ(std::numeric_limits<ca::i32>::min(),
              SimulationOps::execute(O::IDIV, -1, std::numeric_limits<ca::i32>::min()));
    EXPECT_EQ(0, SimulationOps::execute(O::IREM, -1, std::numeric_limits<ca::i32>::min()));
    EXPECT_THROW(SimulationOps::execute(O::IDIV, 0, 1), std::domain_error);
    EXPECT_EQ(1, SimulationOps::execute(O::IUSHR, 31, -1));
    EXPECT_EQ(-1, SimulationOps::execute(O::ISHR, 31, -1));
    EXPECT_EQ(2, SimulationOps::execute(O::ISHL, 33, 1));
    EXPECT_EQ(1, SimulationOps::executeShift(O::LUSHR, 63, -1));
}

TEST(BlwSimulationTest, FloatingCompareAndBranchesFollowJvmRules)
{
    using O           = JavaOpcodes;
    const ca::f64 nan = std::numeric_limits<ca::f64>::quiet_NaN();
    EXPECT_EQ(-1, SimulationOps::compare(1.0, nan, -1));
    EXPECT_EQ(1, SimulationOps::compare(1.0, nan, 1));
    EXPECT_EQ(-1, SimulationOps::compare(0.0, -0.0, 1));
    EXPECT_EQ(1, SimulationOps::compare(-0.0, 0.0, -1));
    EXPECT_DOUBLE_EQ(1.5, SimulationOps::execute(O::DREM, 2.0, 5.5));
    EXPECT_TRUE(SimulationOps::test(O::IFLT, -1));
    EXPECT_TRUE(SimulationOps::test(O::IF_ICMPGT, 1, 2));
    EXPECT_THROW(SimulationOps::test(O::GOTO, 0), std::invalid_argument);
}
