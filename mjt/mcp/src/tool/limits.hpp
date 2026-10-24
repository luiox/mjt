#pragma once

#include "service.hpp"

namespace mjt::mcp::detail {

///分页相关上限。kMaximumPageSize 限制单页返回元素数；kMaximumClassListPayloadBytes
///限制 class 列表单次响应的序列化字节数，防止单响应过大。kDefaultPageSize 是
///page_size/limit 均未给时的缺省页大小（大响应不再以百条
///为缺省）。
inline constexpr ca::usize kMaximumPageSize              = 1000;
inline constexpr ca::usize kMaximumClassListPayloadBytes = 4 * 1024 * 1024;
inline constexpr ca::usize kDefaultPageSize              = 50;

///单次 resource_read/class_bytes 分块读取的最大字节数（1 MiB）。
inline constexpr ca::usize kMaximumResourceChunkSize = 1024 * 1024;

///反编译子进程防护上限。kMaximumDecompilerSourceBytes（16 MiB）与 class bytes 上限
///对齐；超时/内存为子进程级 DoS 防护。
inline constexpr ca::usize kMaximumDecompilerSourceBytes         = 16 * 1024 * 1024;
inline constexpr ca::i64   kMaximumDecompilerTimeoutMilliseconds = 300000;
inline constexpr ca::i32   kMaximumDecompilerMemoryMiB           = 4096;

///异步 job 保留上限（含 method_source 与 workflow 两条路径）。
inline constexpr ca::usize kMaximumRetainedJobs             = 64;
inline constexpr ca::usize kMaximumWorkflowDecompileClasses = 64;

///常驻 workspace 数量上限：每个 workspace 持有完整 ResourceContainer，
///无上限会让长会话内存无界增长。超限时要求先 workspace_close。
inline constexpr ca::usize kMaximumRetainedWorkspaces = 16;

///stdio 与 HTTP 传输层的单消息/请求体上限（24 MiB）。16 MiB class bytes 经 base64
///膨胀约 21.4 MiB，留余量后取整 24 MiB，保证 stdio 与 HTTP 工具对等。
inline constexpr ca::usize kMaximumMessageBytes = 24 * 1024 * 1024;

///请求体 DoS 防护上限（contract.cpp 校验）。集中到 header 以便测试引用。
inline constexpr ca::usize kMaximumJsonDepth            = 32;
inline constexpr ca::usize kMaximumJsonArrayItems       = 4096;
inline constexpr ca::usize kMaximumJsonObjectProperties = 256;
inline constexpr ca::usize kMaximumJsonStringBytes      = 23U * 1024U * 1024U;

///ConstantSpec（常量池 dynamic/bsm 参数）的递归嵌套深度上限。contract.cpp 构造
///schema 与 parser.cpp 运行时校验必须共用此值，避免 schema 允许 8 层而 parser
///拒绝（或反之）的硬耦合漂移。
inline constexpr ca::usize kMaximumConstantDynamicDepth = 8;

///class/owner/URI 类选择器参数的字节长度上限。JVM 常量池 UTF8 常量以 u2 存长度
///（JVMS 4.4.7）→ 单个名字最多 65535 字节；819/4096 字符类名是 JVM 合法名
///，不做更低量级的一刀切拒载。守卫只拦必然非法的病态输入：
///超限返回结构化错误（含实际长度），杜绝静默挂起/静默截断。
inline constexpr ca::usize kMaximumIdentifierBytes = 65535;

}   //namespace mjt::mcp::detail
