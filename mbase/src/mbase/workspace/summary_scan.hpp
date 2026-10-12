///@file summary_scan.hpp
///@brief ClassSummary 扫描入口：legacy（libjbytecode SKIP 路径）与 jbc 后端。
///
///decodeClassSummary 的声明原先只随 workspace.cpp 的定义隐式存在；jbc 后端
///（--with_mbase_jbc=y 接线）与对拍测试需要跨 TU 调用，收进本内部头。
///两个实现的语义合同：对同一 (uri, class 字节) 产出字段级相等的 ClassSummary
///（对拍即回归门）；失败路径都走 StatusOr 的 DATA_LOSS。
#pragma once

#include <string>

#include "libca/core/status.hpp"

#include "mbase/model/resource_model.hpp"
#include "mbase/workspace/workspace_types.hpp"

namespace mjt::base::workspace {

///@brief legacy 后端：ClassReader(SKIP_CODE|SKIP_DEBUG|SKIP_FRAMES) + 计数 visitor。
///@param uri 资源 URI（原样填入 ClassSummary::resourceUri）。
///@param resource class 字节资源。
///@return ClassSummary；解析异常转 Status(DATA_LOSS)。
ca::StatusOr<ClassSummary> decodeClassSummary(const std::string& uri,
                                               const ClassResource& resource);

///@brief jbc 后端：libjbc core 骨架扫描（不解任何属性体，preserveSourceBytes=false）。
///@details 仅在 MBASE_WITH_JBC_SUMMARY（--with_mbase_jbc=y）下编译与接线；
///与 legacy 版字段级对拍等价（unittest/workspace/jbc_summary_scan_test.cpp）。
///@param uri 资源 URI（原样填入 ClassSummary::resourceUri）。
///@param resource class 字节资源。
///@return ClassSummary；readClass 失败转 Status(DATA_LOSS)。
ca::StatusOr<ClassSummary> decodeClassSummaryJbc(const std::string& uri,
                                                 const ClassResource& resource);

}   //namespace mjt::base::workspace
