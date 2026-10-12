# Contributing

## Build

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f -p windows --with_tests=y     # Linux: -p linux
xmake build mjt-mcp mjt_mcp_unittest
xmake run mjt_mcp_unittest --gtest_brief=1
```

One target per `xmake build` invocation.

## Testing

- Add tests for behaviour changes. New code without a test will not be reviewed favourably.
- Tests needing a JDK skip themselves (`GTEST_SKIP`) when it is absent — do not assert on
  its presence. The same applies to the external-artefact tests driven by environment
  variables; see `MJT_ARES_SAMPLE`, `MJT_MCP_REAL_CFR`, `MJT_MCP_REAL_JAVA`.
- Test output goes to `<project_root>/test/` via the test-helper API. Baselines live in
  `test_resource/`. Never commit generated test output.
- xmake's incremental build skips files that hash back to a previous state. If a test result
  looks stale, `rm <exe> && xmake -r <target>` before believing it.

## Code conventions

- **Doxygen `///` comments. `/** */` is not allowed.**
- **No RTTI.** `dynamic_cast` and `typeid` are forbidden; use the `typed::cast<T>` family.
- **Types** come from the `ca::` namespace aliases (`i32`, `u32`, `usize`, `Result<T, E>`);
  do not spell out `int` / `long` / `size_t`.
- **Errors** travel as `Result<T, E>` by value. Do not throw across a module boundary.
- **Strings** use the `Utf8String` family. Borrowed API parameters use `std::string_view`;
  do not add `std::string` storage except at byte boundaries.
- **Includes**: `"file.hpp"` within a module, `"module/path/file.hpp"` across modules.
  `../` is forbidden.
- **Warnings are errors in spirit.** The project builds with `set_warnings("all")`; do not
  introduce new warnings or suppress them without a comment explaining why.

## Dependencies

External packages are pulled straight from `luiox-repo` (see `xmake.lua`). Do not vendor
copies into the repository and do not add phony shim targets. Nested `libca` requirements
must stay pinned to the same `LUIOX_LIBCA_VERSION`.

Bumping `libca` means changing the single `LUIOX_LIBCA_VERSION` constant in `xmake.lua`,
after checking that the nested `add_requireconfs` paths still match the new package layout.

## Commits

Conventional prefixes, scoped: `feat(crypto): …`, `fix(decompiler): …`, `docs(mcp): …`,
`refactor(mbase): …`, `chore: …`.

Check `git status` before committing; test artefacts and scratch files must not be
committed.
