# mjt

JVM 字节码工具集的 **MCP 服务器**。通过 [Model Context Protocol](https://modelcontextprotocol.io)
把 classfile 静态分析、字节码编辑、mapping 读写与交叉引用分析暴露给 AI 编码助手。

[English](README.md) | **简体中文**

## 是什么

`mjt-mcp` 是一个 stdio MCP 服务器进程。它在一侧连接受信任的模型客户端（Claude Code、
opencode 等），在另一侧连接受分析的 JAR / classfile —— 分析能力全部在本地执行，
不依赖被分析产物的网络访问。

典型用途：

- 在改动一个陌生的 JAR 前，先摸清它的类结构、方法签名与调用关系
- 查询 mapping（ProGuard / R8 / Tiny / SRG / YGuard / Allatori 等格式）的解析结果
- 对 classfile 做字节码级编辑并落盘
- 追踪方法/字段的交叉引用
- 配合 CFR / Vineflower 做源码还原迭代

## 构建

需要 xmake 3.x 与 C++17 编译器（Windows 用 MSVC，Linux 用 gcc/clang）。

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f -p windows          # Linux 用 -p linux
xmake build mjt-mcp
```

依赖经 [luiox-repo](https://github.com/luiox/luiox-repo) 直连拉取：libca、libmcp、
libjstr、libjbytecode（仅链接闭包兜底——mjbase 的 relaxed_verifier 消费，
RFC 0002 B8 收口后摘除）、libjzip、libgarena、libjclassfile、libjstd、
libjdk_finder、libjhasm；libarchive / zlib / nlohmann_json 走
[xrepo](https://xmake.io)。libjbc 走项目内联包定义（git 直连 + commit 锁，与
mjbase 同版本）。字节码基座本体是 mjbase，打包进 luiox-repo 前以兄弟目录源码级
依赖消费（`--mjbase_root`，默认 `../mjbase`）。

### 测试

```sh
xmake f --with_tests=y
xmake build mjt_mcp_unittest
xmake run mjt_mcp_unittest --gtest_brief=1
```

部分用例需要 JDK；缺失时相关用例 `GTEST_SKIP` 而非失败。测试产物落在各项目根的
`test/` 目录，可用 `LIBCA_TEST_OUT_ROOT` 改写。

已包化的各库在 `unittest/` 自带单测（`libjhasm_unittest`、
`libjdk_finder_unittest`），构建与运行方式相同。

## 运行

```sh
xmake run mjt-mcp
```

或直接执行 `build/<plat>/<arch>/<mode>/artifacts/.../mjt-mcp.exe`。进程走 stdio
传输，按 MCP 协议与客户端握手。

Windows 下 exe 嵌了 `activeCodePage=UTF-8` 与 `longPathAware` 的 manifest，
使中文类名参数与长路径在默认 GBK 代码页下也能正常工作（需 Windows 10 1903+
识别前者）。

## 仓库结构

```
mjt/mcp/src/     MCP 服务器与工具集（核心产品代码）
mjt/mcp/test/    MCP 层单测
mjt/src/shared/  MCP 工具集共用的底层能力（反编译接入、jar 重映射、workspace…）
mjt/skills/      MCP 技能定义
mjt-py/src/      嵌入式 Python SDK（mjt --script 嵌入解释器与 mjt.pyd
                 pybind11 binding；--with_pysdk 门控）
unittest/        已包化各库的单测（libjhasm、libjdk_finder）
test_helper/     测试脚手架（gtest 集成、产物路径解析）
xmake/           构建开关、libjbc 内联包定义、libca 规则内嵌、Windows manifest
                 规则、包仓覆盖
```

共享层在仓外：mjbase 源码树（兄弟目录依赖）与 luiox-repo 各包。`mjt` 是产品线，
`mjt-py` 是可选的 SDK 组件。

## 约定

- **No RTTI**：`dynamic_cast` / `typeid` 禁用，用 `typed::cast<T>` 家族
- **类型**：统一走 `ca::` 命名空间的别名（`i32`/`u32`/`usize`…），不裸写 `int`/`long`
- **错误**：`Result<T, E>` 值语义，不抛异常跨层
- **字符串**：`Utf8String` 家族；借用视图用 `std::string_view`
- **头文件**：模块内 `"file.hpp"`，跨模块 `"module/path/file.hpp"`，禁 `../`
- **注释**：Doxygen `///`；禁止 `/** */`

更完整的开发约定见 [AGENTS.md](AGENTS.md)。

## License

[MIT](LICENSE)
