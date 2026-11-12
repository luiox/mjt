# mjt

An **MCP server** for the JVM bytecode toolset. Through the[Model Context Protocol](https://modelcontextprotocol.io) it exposes classfile static analysis, bytecode editing, mapping I/O and cross-reference analysis to AI coding assistants.

**English** | [简体中文](README.zh-CN.md)

## What it is

`mjt-mcp` is a stdio MCP server process. On one side it talks to a trusted model client (Claude Code, opencode, etc.); on the other it operates on the JARs / classfiles under analysis — all analysis runs locally, with no network access to the analyzed artifacts.

Typical uses:

- Map out class structure, method signatures and call graphs of an unfamiliar JAR before touching it
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

Dependencies are pulled directly from[luiox-repo](https://github.com/luiox/luiox-repo): libca, libmcp, libjstr,libjzip, libgarena, libjclassfile, libjstd, libjdk_finder,libjhasm; zlib / nlohmann_json come from[xrepo](https://xmake.io). libjbc is defined as an in-repo inline package(git direct with a pinned commit, at or above the floor mjbase requires). The bytecode foundation itself is mjbase, pulled directly from luiox-repo (`add_requires("mjbase 0.0.22")`, pinned to an upstream main commit).

### Tests

```sh
xmake f --with_tests=y
xmake build mjt_mcp_unittest
xmake run mjt_mcp_unittest --gtest_brief=1
```

Some cases require a JDK; when one is missing they `GTEST_SKIP` instead of failing. Test artifacts land in the `test/` directory of each project root and can be redirected with `LIBCA_TEST_OUT_ROOT`.

The packaged libraries ship their own suites under `unittest/`(`libjhasm_unittest`, `libjdk_finder_unittest`), built and run the same way.

## Run

```sh
xmake run mjt-mcp
```

or execute `build/<plat>/<arch>/<mode>/artifacts/.../mjt-mcp.exe` directly. The process uses the stdio transport and performs the MCP handshake with the client.

On Windows the executable embeds an `activeCodePage=UTF-8` and `longPathAware` manifest, so Chinese class-name arguments and long paths work even under the default GBK code page (the former needs Windows 10 1903+).

## CLI usage

The `mjt` binary is a plain command-line tool (`xmake build mjt`) covering thirteen batch-style commands over JARs and mapping files. Exit codes: `0` success (including `--help`), `1` usage error (missing argument, missing input file, unknown format name, class selector without a match, invalid flag value), `2` runtime failure (open, parse, repair, export, decompile, extraction, scan or disassembly failure).

```sh
mjt --version          # print tool version
mjt --help             # command tree help
```

### `mjt info` — JAR overview

```sh
mjt info -i <jar> [--json]
```

Prints entry count, class count, member census (method / field totals, native method count, per-side visibility buckets), class-kind distribution (class / interface / enum / annotation / record / module), class origin distribution (`jdk` / `known` / `unknown`, derived from package prefixes), entry-path-shape facet distribution (`regular`, `empty_segment`, …), bytecode version distribution and MANIFEST presence. Census facts come from the analysis database projection; `--json` emits the same data with stable English keys.

### `mjt mapping` — mapping format conversion

```sh
mjt mapping -i <in> -o <out> --from <fmt> --to <fmt>
```

Converts a mapping file between formats through a unified intermediate representation. Format names are case-insensitive; the SRG, Tiny-V2 and Proguard families are all interchangeable (other registered formats: `Tiny-V1`, `Simple`, `Enigma`, `R8`, `YGuard`, `Allatori`, `Shadow-Alias`). An unrecognized format name fails loudly with exit code `1`.

Optional jar remapping with the source mapping:

```sh
mjt mapping -i <in> -o <out> --from <fmt> --to <fmt> \
    --remap-jar <jar> [--remap-out <jar>] [--remap-policy bytecode-only|reflection|metadata|all]
```

`--remap-jar` applies the source mapping to a jar via the mjbase remapper; the remapped copy defaults to `<jar stem>-remapped.jar` unless `--remap-out` gives a path.

### `mjt jar fix` — diagnose and repair a JAR

```sh
mjt jar fix -i <in> -o <out>
```

Scans the jar, removes entries that cannot be repaired (class bytes failing bytecode validation, `.class` entries that are not class bytes, and known corrupt-output marker entries), and writes a repaired copy to `<out>` (an existing output file is replaced). The report lists what was removed and what was kept per entry; an unrecoverable failure exits non-zero with the reason. Input and output must be different files.

### `mjt decompile` — decompile JAR classes to Java sources

```sh
mjt decompile -i <jar> -c <internalName> [-o <file.java>]   # single class
mjt decompile -i <jar> -o <outdir> --all                    # whole jar
mjt decompile -i <jar> -o <out.zip> --all --decompiler vineflower   # zip output
```

Single-class mode fetches the target class bytes by internal name (slash form, e.g. `fixture/Alpha`), stages them under a short safe name (`Target.class`) and runs the backend with the original jar attached as classpath for cross-type context. The class name itself never materializes on disk, so classes whose names exceed the 255 UTF-16 unit file system limit (or contain characters that cannot appear in file names) stay reachable — including the vineflower cascade failure where one hostile class used to poison every decompile call on its jar. With `-o` the source lands in a `.java` file; without it the source prints to stdout.

Batch mode hands the whole jar to the backend (CFR writes a source tree, vineflower writes a source tree or a `.zip` archive — archive entries are not bound by path limits). Entries whose names cannot be written to disk are skipped: the run continues, stderr lists the skipped entries and points to single-class mode or `mjt disasm`. `--all` confirms the whole-jar cost; batch without it exits `1` with the class count.

Shared options (aligned with the MCP decompile contract): `--decompiler cfr|vineflower|jcdc` (default `cfr`), `--decompiler-jar <jar>` (for `jcdc` this carries the executable path), `--timeout-ms <n>` (default 120000), `--max-memory-mib <n>` (default 1024). `jcdc` is an experimental native backend: no JVM involved, discovered via `MJT_JCDC_EXE` / `MJT_DECOMPILER_HOME`, and always invoked with `--synthetic` so no members are silently hidden.

### `mjt strings` — constant-pool string extraction

```sh
mjt strings -i <jar> [--class <internalName>] [--min-len N] [--json] [-o <file>]
```

Extracts constant-pool strings from the jar's classes (whole jar, or a single class with `--class`) and prints a deduplicated list ordered by occurrence count. Occurrences are constant-pool entries: a string literal contributes both its `CONSTANT_String` entry and its backing `CONSTANT_Utf8` entry (the `--json` output carries a per-value `kinds` breakdown of utf8 / string / class entries), so the same text referenced from two classes counts four. `--min-len N` filters values shorter than N bytes (canonical UTF-8 length, default `1`); malformed class entries are skipped and counted. With `-o` the full report goes to a file and stdout keeps the summary; values are kept on a single line (control characters escaped) and stay valid UTF-8 regardless of pool encoding quirks.

### `mjt xref` — class / member reverse reference lookup

```sh
mjt xref -i <jar> -c <internalName> [-m <member>] [--direction to|from|both] [--limit N] [--json]
```

Reverse-lookups the direct references around a class or member. `to` (default) finds every site whose encoded target is the class (method calls and field accesses owned by it, type instructions and class-header declarations naming it); `from` finds every reference emitted by the class's own methods; `both` is the union (self-edges deduplicated). `-m <member>` narrows to one member: `name` matches across descriptors, `name(descriptor)` (e.g. `act(I)V`) is an exact match. The class must exist in the jar; an unknown class or a malformed member selector exits `1`, while a member selector that matches nothing yields an empty result with exit `0`.

Output is always bounded: `--limit` caps the reference list (default `200`); when more matches exist the report says `total` and asks for a larger `--limit` — obfuscated jars can reach millions of rows and unbounded output is an incident. Each reference line shows kind (`method` / `field` / `type`), source (`class.method(desc)@insn` or `(class header)` for class-level sites) and target (`owner.name(desc)` or the type name).

### `mjt classes` — class inventory with kind and origin columns

```sh
mjt classes -i <jar> [--kind class|interface|enum|record|annotation|module] [--origin jdk|known|unknown] [--limit N] [--json] [-o <file>]
```

Lists every class in the jar (internal name + kind + origin columns) from the analysis census projection — the same derived columns as `mjt info`. `--kind` filters by class kind, `--origin` by package-prefix origin (`jdk` / `known` / `unknown`); both combine as an intersection and values are case-insensitive. Rows are ordered by internal name (byte order, stable across platforms). Output is bounded on stdout: `--limit` caps the printed list (default `10000`); when more rows match, the report shows `total` and asks for a higher limit or tighter filters. `-o` bypasses the cap — the full list goes to a file and stdout keeps the summary; `--json` emits the same data with stable English keys.

### `mjt disasm` — single-class jasm text

```sh
mjt disasm -i <jar> -c <internalName> [-o <file>]
```

Prints the normalized jasm text of one class — the same source the MCP `class_query kind=jasm` facet serves. The class is located by internal name (slash form); an unknown class exits `1`. This is a pure text facet with no decompiler involved, so it works where decompilation cannot: hostile class names, classes the batch precheck skipped, quick instruction-shape checks (`iload_0`, `invokestatic`, …). Without `-o` the jasm text prints to stdout only (redirect-friendly); with `-o` it lands in a file.

### `mjt subs` — single-class member inventory

```sh
mjt subs -i <jar> -c <internalName> [--json]
```

Lists the fields and methods of one class: name, descriptor, raw access flags (JVMS bitmask, decimal) and a `native` marker for methods. Fields come first, each group in classfile declaration order; the header reports the class kind (same derivation as `mjt classes`). An unknown class exits `1`; `--json` emits the same data with stable English keys.

### `mjt hexdump`: raw class byte dump

```sh
mjt hexdump -i <jar> <class> [--offset N] [--length N] [--width N] [--no-ascii]
```

Prints the raw bytes of one class as xxd-style rows (`%08x` offset, hex bytes with a half-line separator, ASCII column with non-printables as `.`). The class is located by `<class>`: a resource URI (`fixture/Alpha.class`) or internal name (`fixture/Alpha`) matches exactly first, otherwise the first entry whose name contains the selector wins. `--offset` / `--length` window the dump (`--length -1`, the default, means "to the end of the class"), `--width` sets the bytes-per-line count (default `16`), and `--no-ascii` hides the right-hand ASCII column. An offset past the end of the class prints an explicit empty note instead of rows; negative offsets, non-positive widths and lengths below `-1` are usage errors. This is a pure byte-level view: no class parsing involved, so it works on bytes `disasm` cannot render.

### `mjt eval`: expression evaluation and simplification

```sh
mjt eval "1 + 2 * 3"                  # 7
mjt eval -v x=10,y=20 "x + y"         # 30
mjt eval "x + 0"                      # unbound variables print the S-expression
mjt eval -s "(x ^ y) + 2*(x & y)"     # simplified form
```

Evaluates or simplifies a mathematical expression through the mjbase expr pipeline (`parseExpression` → optional `simplify` → `eval`; the same engine the Python SDK's `analysis_evaluate` binds). Operators: `+ - * / % & | ^ ~ << >> >>>` with standard C precedence; decimal and hex (`0xFF`) literals and named variables. `-v k=v,k2=v2` binds variables to integer values; a malformed binding string or an unparsable expression exits `1`. An expression with unbound variables is not a failure: the simplified S-expression form prints to stdout with exit `0` (with `-s` the MBA algebraic identities are applied first, so `(x ^ y) + 2*(x & y)` reduces to `x + y`). Other evaluation failures (depth limit exceeded) exit `2`.

### `mjt reflect`: reflection call-site binding analysis

```sh
mjt reflect -i <jar> [--json] [--unresolved] [--bindings-of <class>]
```

Answers a narrow question for every reflection sink call site (`Class.forName`, `ClassLoader.loadClass`, the `Class.get*Method(s)` / `get*Field(s)` member-lookup family, `Method.invoke` / `Constructor.newInstance` / `Class.newInstance`, `Field.get` / `set`; `MethodHandles.Lookup` is catalog-only): can the target be determined statically, and does it really exist in this jar? The analyzer (mjbase `ReflectionBindingAnalyzer`; bindings it cannot determine stay `Pending` rather than becoming false positives) folds method-local value flow (`ldc` constants, StringBuilder chains, `makeConcatWithConstants` recipes, `static final String` constants and name tables), then verifies resolved targets against the container's real declarations; anything underdetermined degrades to `pending` with a reason (`unknown_args`, `ambiguous_array`, `unknown_receiver`, `no_such_class`, `no_such_member`, `member_enumeration`), and a target class outside the jar stays `pending no_such_class` rather than guessing.

Output: three `# ` summary lines (classes scanned, resolved/pending/unsupported counts with resolution rate, per-API distribution) followed by one binding line per call site (`[state] api class=… member=… params=… reason=… target=… <- site insn=N`). With `--json` the summary moves to stderr and stdout carries a pure JSON array (stable English keys); `--unresolved` keeps only pending bindings; `--bindings-of <class>` filters to one source class (internal or dotted name, normalized like `xref`). Jar open or analysis failure exits `2`.

### `mjt cfg`: single-method CFG as Graphviz dot

```sh
mjt cfg -i <input.jar|.class> [-c <class>] [-m <name[:descriptor]>] [-o <out.dot>]
```

Dumps one method's block-level control-flow graph in Graphviz dot. The analysis kernel is the mjbase/asm `ControlFlowGraph` (block splitting + edge classification + exception edges), plus `CfgTraversal` (reverse postorder) and `DominatorTree` for the comment metadata. Input is a `.jar` (then `-c <internalName>` is required) or a `.class` file (class name inferred from the file name, `.class` suffix stripped). `-m <spec>` selects the method: `name` matches across descriptors, more than one match is an ambiguity error (exit `1`, use `name:descriptor` for an exact pick), and an omitted spec takes the first method that has a body. A method without a body (abstract / native) prints an explicit empty graph with a `no body` comment line and exits `0`.

Output shape: a quoted `digraph "<class>.<method><descriptor>"` header; `//` comment lines for the class / method / entry block (`B0`), the reverse postorder, and one `// idom: B<n> <- …` line per reachable block; one `B<n> [label="…" shape=box]` node per block whose label lists the block's instructions in program order as `<pc>: <mnemonic> <operands>` (branch targets rendered as target block numbers, constant-pool references resolved to text: `owner.name desc` for members, class internal names, quoted string literals; float/double constants as `0x…` bit patterns); and one labeled edge per control-flow edge. Edge labels are the mjbase EdgeKind stable names (`fallthrough`, `branch`, `condition`, `switch`, `exception`), with exception edges styled `dashed` and carrying the catch type (`exception: java/lang/Exception`, or `exception: any` for a catch-all handler). With `-o` the dot text overwrites the given file (failure exits `2`); without it stdout carries the dot and nothing else.

The result is a **block-level CFG**, not an SSA instruction-level graph: nodes are basic blocks with an instruction overview inside, edges carry EdgeKind labels (exception edges carry the catch type), and entry / RPO / idom facts appear as comment lines.

### `mjt dataflow`: method-local dataflow queries

```sh
mjt dataflow du   -i <input.jar|.class> [-c <class>] [-m <name[:desc]>] [--json]
mjt dataflow live -i <input.jar|.class> [-c <class>] [-m <name[:desc]>] [--json]
```

Method selection matches `cfg` exactly (spec `name` / `name:descriptor`, ambiguity exits `1`, default first method with a body). A selected method without a body is rejected as a usage error (`no body … no CFG to analyze`, exit `1`). The analysis kernels are mjbase/asm `MethodDefUse` (slot-level def-use over the `DomainEngine` frame analysis, with precise exception edges) and `LivenessView` (local-slot backward may-liveness). Both are **slot-level models**, not SSA: definitions are labeled by parameter / instruction slot, and liveness is tracked per local slot.

`du` prints one chain line per definition. Definitions are labeled `param[k]` (instance methods render slot 0 as `param[0]=this`), seeded from the descriptor (long/double occupy two slots but seed one definition), or `i<slot>(<mnemonic>)` for instruction definitions (store family / `iinc` writes and stack-value producers; a dead store with no uses is still listed). Use points render as `i<slot>(<mnemonic>)@stack<k>` (k-th operand-stack value from the top) or `i<slot>(<mnemonic>)@local<k>` (local-slot read), a trailing `!` marking an edge whose reaching-definition set overflowed the analyzer's `kMaxDefs` bound; an empty use list renders `-`. The header line reports chain / edge counts plus the analyzer states `ok` (frame analysis succeeded) and `truncated` (any reaching set overflowed, so the edge set may be incomplete).

`live` prints the entry live set followed by one row per instruction (program order) with the live-before and live-after **local slot** sets. The model is the mjbase/asm `LivenessView`: backward may-liveness over local slots (`out[b] = ∪ in[succ]`, exception handlers conservatively read every slot), with per-instruction transfer `in = (out − def) ∪ use`.

With `--json` (both subcommands) stdout carries a **single JSON object** with no summary lines. `du` keys: `class`, `method`, `descriptor`, `instance`, `ok`, `truncated`, `unresolved_field_refs` (a malformed field-reference constant pool entry; field facts incomplete), `edges` (total def→use edge count), `chains[]`: each `{def, uses[]}` with `def` = `{"kind":"parameter","slot":k}` (plus `"this":true` for an instance receiver) or `{"kind":"instruction","slot":n,"pc":p,"opcode":"…"}`, and each use = `{"insn":n,"pc":p,"opcode":"…","slot":"stack"|"local","index":k,"truncated":bool}`; plus `field_accesses[]` = `{"insn","pc","opcode","owner","name","descriptor","write","static","object_defs":[def…],"value_defs":[def…],"truncated"}`. `live` keys: `class`, `method`, `descriptor`, `instance`, `slots` (slot-count upper bound), `entry_live` (ascending slot ids), `points[]` = `{"insn","pc","opcode","live_before":[…],"live_after":[…]}`. All text fields are JSON-escaped; malformed class bytes exit `2`.

Output model: a slot-level view, not an SSA instruction-level graph: `du` chains are keyed by slot-level definitions (`param[k]` parameter slots, `i<slot>(<mnemonic>)` instruction slots) with use points located by consuming instruction plus operand-stack / local-slot position, and `live` is per-**local-slot** point liveness (there is no SSA-value liveness concept here). The `ok` / `truncated` / `unresolved_field_refs` states report partial or degraded results faithfully instead of hiding them.

Locale: message text follows the `MJT_LANG` environment variable (`en_US` default, `zh_CN` supported).

## Repository layout

```
mjt/mcp/src/     MCP server and tool set (core product code)
mjt/mcp/test/    MCP-layer unit tests
mjt/cli/         `mjt` CLI binary (info / mapping / jar fix / decompile / strings / xref / classes / disasm / subs / hexdump / eval / reflect / cfg / dataflow)
mjt/test/cli/    CLI-layer unit tests
mjt/src/shared/  capabilities shared across the MCP tool set
                 (decompiler integration, jar remapping, workspace…)
skills/          MCP skill definitions
mjt-py/src/      embedded Python SDK (mjt --script embedded interpreter and
                 the mjt.pyd pybind11 binding; gated by --with_pysdk)
unittest/        suites for the packaged libraries (libjhasm, libjdk_finder)
test_helper/     test scaffolding (gtest integration, artifact path resolution)
xmake/           build switches, inline libjbc package definition, embedded
                 libca rules, Windows manifest rule, package repository
                 overrides
```

The shared layer lives outside the repo: the mjbase source tree (sibling directory dependency) plus the luiox-repo packages. `mjt` is the product line;`mjt-py` is an optional SDK component.

## Conventions

- **No RTTI**: `dynamic_cast` / `typeid` are disabled; use the `typed::cast<T>` family
- **Types**: use the `ca::` namespace aliases (`i32`/`u32`/`usize`…), never bare `int`/`long`
- **Errors**: `Result<T, E>` value semantics; no exceptions across layers
- **Strings**: the `Utf8String` family; borrowed views use `std::string_view`
- **Headers**: `"file.hpp"` within a module, `"module/path/file.hpp"` across modules; `../` is forbidden
- **Comments**: Doxygen `///`; `/** */` is forbidden

See [AGENTS.md](AGENTS.md) for the fuller development conventions.

## License

[MIT](LICENSE)
