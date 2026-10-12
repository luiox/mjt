#pragma once

#include "jblw/annotation/builders.hpp"
#include "jblw/annotation/element_traits.hpp"
#include "jblw/constant/constant_sink.hpp"
#include "jblw/constant/constant_traits.hpp"
#include "jblw/code/code_traits.hpp"
#include "jblw/classfile/generic/builders.hpp"
#include "jblw/asm/bytecode_library.hpp"
#include "jblw/asm/conversion.hpp"
#include "jblw/constantpool/constant_pool.hpp"
#include "jblw/constantpool/entry_traits.hpp"
#include "jblw/resolution/jvm_resolvers.hpp"
#include "jblw/simulation/execution.hpp"
#include "jblw/simulation/simulation_ops.hpp"
#include "jblw/type/dynamic_traits.hpp"
#include "jblw/type/type_factory.hpp"
#include "jblw/type/type_reader.hpp"
#include "jblw/type/type_traits.hpp"
#include "jblw/type/types.hpp"
#include "jblw/version/java_version.hpp"

namespace jblw {

///@brief 返回当前 libjblw 重写基线版本。
///
///该版本描述 C++ 库自身，不等同于上游 BLW 发布版本。
///
///@return 进程生命周期内有效的 UTF-8 静态字符串。
const char* get_lib_version() noexcept;

///@brief 返回本次重写所固定的 BLW revision。
///@return 进程生命周期内有效的 Git revision 静态字符串。
const char* get_upstream_revision() noexcept;

}   //namespace jblw
