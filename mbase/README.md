# mbase

`mbase` 是本仓的字节码分析与编辑层：一个静态库，不直接面向最终用户，由 `mjt-mcp`
消费。它为 MCP 工具提供 classpath 解析、引用索引、mapping 读写、workspace 编辑
会话与 pass 管线等底座能力。

## 能力

| 领域 | 内容 |
|---|---|
| classpath | 类路径解析、调用图、已解析调用图缓存（读写/格式） |
| 分析 | 引用索引（含缓存）、别名分析、异常流、方法副作用、方法帧、继承层级 |
| 映射 | mapping 中间表示、格式注册、TinyV2、shadow 别名格式、标识符工具 |
| 改名 | shadow 别名/令牌、改名引擎（建议 → plan/应用） |
| 容器 | JAR 读写、资源模型、Manifest、classfile 读写接入 |
| 编辑 | workspace 会话（打开/搜索/事务化变更）、类变更服务、summary 扫描 |
| pass | 三级 pass 架构（global/class/method）、`PreservedAnalyses` 保全语义、优化 pass 集 |
| 表达式 | 表达式 IR、解析/打印、化简、匹配求值 |
| CLI | ArgParser 框架、命令注册表、i18n 文案表、输出通道 |

## 模块

```
asm/         字节码 IR、分析器、匹配（DSL/tmatcher）、指令工具、remapper
analysis/    通用分析设施（别名/异常流/副作用/帧）
reference/   引用索引与反射绑定分析、可达性
hierarchy/   类/方法/字段继承层级
mapping/     mapping 格式注册、TinyV2、shadow 别名格式
renamer/     改名引擎与 shadow 别名
model/       资源模型与容器
jar/         JAR 读取
jasm/        JASM 汇编/反汇编服务（经 libjhasm）
workspace/   编辑会话抽象与类变更服务
pass/        pass 注册表与优化 pass 集（pass/opt）
expr/        表达式 IR、化简与匹配
spi/         pass 契约（AsmPass / PassManager / 分析管理）
cli/         命令注册表与运行框架（ArgParser、i18n 表、输出）
util/ log/   通用工具与日志门面
```

## 构建

依赖 `libjhasm` / `libjstd` 与 libca 家族的 libjbytecode、libjstr、libjzip、
libjclassfile；链接 libarchive。

```sh
xmake build mbase
```

库本身不带单测 target；行为验证由消费层（`mjt_mcp_unittest`）覆盖。
