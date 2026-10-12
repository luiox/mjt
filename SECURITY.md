# Security Policy

## Supported versions

Security fixes land on `main`. There are no release branches.

## Reporting a vulnerability

Please report suspected vulnerabilities privately via GitHub's
[private vulnerability reporting](https://docs.github.com/code-security/security-advisories/guidance-on-reporting-and-writing/privately-reporting-a-security-vulnerability)
on this repository, rather than opening a public issue.

Include:

- what the MCP server exposes and how it is normally invoked
- a reproducer (a sample JAR plus the exact MCP request sequence)
- the version or commit you tested

You can expect an acknowledgement within a few days. Please give us a reasonable
window to ship a fix before public disclosure.

## Scope notes

`mjt-mcp` analyses arbitrary bytecode by design — malformed, unusual, or
deliberately hostile JARs are its normal input. Reports are most valuable when
they concern the analysis engine itself, for example:

- memory-safety faults reachable from a crafted classfile or JAR
- out-of-bounds reads/writes while parsing class structures, JASM, or mapping files
- unbounded resource consumption (memory or CPU) on small inputs
- path traversal when writing analysed results to disk

Ordinary "the analyser got confused by an unusual input" reports are not
security issues and belong in a regular issue.

## Trust boundary

The server performs analysis locally and does not fetch the JARs it analyses.
Network access is limited to the stdio MCP transport with the client that
launched it. Running it against an untrusted JAR is expected; running the
server binary itself from an untrusted source is not.
