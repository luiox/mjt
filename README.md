# mjt

An **MCP server** for the JVM bytecode toolset. Through the
[Model Context Protocol](https://modelcontextprotocol.io) it exposes classfile
static analysis, bytecode editing, mapping I/O and cross-reference analysis to
AI coding assistants.

**English** | [简体中文](README.zh-CN.md)

## What it is

`mjt-mcp` is a stdio MCP server process. On one side it talks to a trusted
model client (Claude Code, opencode, etc.); on the other it operates on the
JARs / classfiles under analysis — all analysis runs locally, with no network
access to the analyzed artifacts.

Typical uses:

- Map out class structure, method signatures and call graphs of an unfamiliar
  JAR before touching it
- Query mappings (ProGuard / R8 / Tiny / SRG / YGuard / Allatori and more)
- Apply bytecode-level edits to classfiles and persist them
- Track method / field cross-references
- Iterate on source restoration with CFR / Vineflower

## Build

Requires xmake 3.x and a C++17 compiler (MSVC on Windows, gcc/clang on Linux).

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f -p windows          # use -p linux on Linux
xmake build mjt-mcp
```

Dependencies are pulled directly from
[luiox-repo](https://github.com/luiox/luiox-repo): libca, libmcp, libjstr,
libjbytecode, libjzip, libgarena, libjclassfile; libarchive / zlib /
nlohmann_json come from [xrepo](https://xmake.io). Of these, libjbytecode is a
legacy frozen package (visitor architecture) that has been superseded by libjbc,
and is consumed here only as the transitional foundation for the in-repo
`mbase/` layer.

### Tests

```sh
xmake f --with_tests=y
xmake build mjt_mcp_unittest
xmake run mjt_mcp_unittest --gtest_brief=1
```

Some cases require a JDK; when one is missing they `GTEST_SKIP` instead of
failing. Test artifacts land in the `test/` directory of each project root and
can be redirected with `LIBCA_TEST_OUT_ROOT`.

The in-repo libraries ship their own suites (`libjhasm_unittest`,
`libjblw_unittest`, `libjdk_finder_unittest`), built and run the same way.

## Run

```sh
xmake run mjt-mcp
```

or execute
`build/<plat>/<arch>/<mode>/artifacts/.../mjt-mcp.exe` directly. The process
uses the stdio transport and performs the MCP handshake with the client.

On Windows the executable embeds an `activeCodePage=UTF-8` and
`longPathAware` manifest, so Chinese class-name arguments and long paths work
even under the default GBK code page (the former needs Windows 10 1903+).

## Repository layout

```
mjt/mcp/src/     MCP server and tool set (core product code)
mjt/mcp/test/    MCP-layer unit tests
mjt/src/shared/  capabilities shared across the MCP tool set
                 (decompiler integration, jar remapping, workspace…)
mjt/skills/      MCP skill definitions
mbase/           bytecode analysis and editing layer
                 (classfile, bytecode, mapping, xref, pass pipeline)
libs/            in-repo small libraries: JASM assembler/disassembler,
                 class bytecode wrapper, JDK/toolchain detection,
                 Java standard-library semantics
test_helper/     test scaffolding (gtest integration, artifact path resolution)
xmake/           build switches, embedded libca rules, Windows manifest rule,
                 package repository overrides
```

`mbase` and `libs` are the shared layer; `mjt` is the only product line.

## Conventions

- **No RTTI**: `dynamic_cast` / `typeid` are disabled; use the
  `typed::cast<T>` family
- **Types**: use the `ca::` namespace aliases (`i32`/`u32`/`usize`…), never
  bare `int`/`long`
- **Errors**: `Result<T, E>` value semantics; no exceptions across layers
- **Strings**: the `Utf8String` family; borrowed views use `std::string_view`
- **Headers**: `"file.hpp"` within a module, `"module/path/file.hpp"` across
  modules; `../` is forbidden
- **Comments**: Doxygen `///`; `/** */` is forbidden

See [AGENTS.md](AGENTS.md) for the fuller development conventions.

## License

[MIT](LICENSE)
