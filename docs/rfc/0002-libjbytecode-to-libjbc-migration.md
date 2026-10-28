# RFC 0002：libjbytecode → libjbc 迁移 + mjbase 设施完善

状态：执行中（goal 模式，一次性全弧做完）。

## 一、现状盘点

### 1.1 依赖图（迁移前）

```
mjt 仓（产品线）
├── mjt-mcp：libjbc 直连；libjbytecode 仅链接闭包传递项（无源码级 include）
├── mjt-py（--with_pysdk）：libjbytecode 唯一源码级消费方
│     pysdk_facade（2480+669 行）：tree API（ClassNode/MethodNode/19 种指令
│     节点 + uid 纪律 + 标签/异常表/LVT/LNT）+ ClassReader/ClassWriter
│     （COMPUTE_FRAMES 写回）；pysdk_jar：双阶段落盘容器
└── test_helper：asm-test 夹具数据（兄弟目录 libjbytecode 仓 test_resource/
      origin 51 个 .class，JDK 分版本子目录）——数据耦合，非代码依赖

luiox 家族包（luiox-repo 直连）
├── libjhasm 0.0.1 → libjbytecode + libjblw（mjbase jasm 装配后端）
├── libjblw 0.0.1 → libjbytecode（mjt/mjbase 源码面仅一处 jblw/version 引用，
│   已被 libjhasm 0.3.0 迁移工作触及）
└── libjclassfile：与 libjbytecode 无关（不经它）
```

mjbase 本身零 libjbytecode（纯 libjbc 基座）。

### 1.2 libjbytecode API 面（ASM 移植）与 libjbc/mjbase 对位

| libjbytecode | libjbc / mjbase 对位 | 状态 |
|---|---|---|
| core ClassReader/ClassWriter | jbc reader/writer + ClassSession promote/lower | 就绪 |
| COMPUTE_MAXS/COMPUTE_FRAMES | jbc/analysis frame 引擎（maxStack/maxLocals 重算）+ smt_build（§10.4）+ mjbase spi/frame_rewire | 就绪 |
| tree ClassNode/MethodNode/指令节点 | ClassUnit/CodeDom/CodeEditor（InsnId 稳定句柄，gen 代次）+ MethodSeed/FieldSeed/NewClassSeed | 就绪（需 facade 内适配层） |
| Label 对象/跳转重定向 | LabelId + CodeEditor 重定向（ErasePolicy::kRetargetToFallthrough） | 就绪 |
| commons Remapper | mjbase asm/remapper（MemberRemapPlan 全覆盖） | 就绪 |
| analysis Analyzer/Verifier | mjbase asm/analyzer（def_use）+ jbc domain/frame/smt_read | 就绪 |
| util Textifier/ASMifier | mjt 自有 jasm 服务（非 ASM 文本形态，不迁移） | 不需要 |

### 1.3 版本 skew

- mjt/mjbase 内联 libjbc 包定义 pin **0.4.15**；libjbc 仓已发 **0.4.16**（typed
  属性挂载/BSM intern/MethodEditor 前置）、**0.4.17**（consumer 模块移除，能力
  归 mjbase consumer/renamer）；feat/method-editor（PR #9）已合入 main，
  fix/method-parameters-receiver-arity 分支在途未合。
- libjbytecode 0.0.2 被 mjt/mjbase pin；0.0.3 已发布（仅 Windows 路径/审计面，
  无 API 变化）。
- libjbc#10（CodeEditor move-range）open，阻塞 mjbase GotoRearranger。

## 二、目标与非目标

目标：
1. mjt/mjbase 依赖图中 libjbytecode 全退役（含传递依赖 libjblw）；
2. mjt-py Python API 契约零变化（bind 层签名不动，facade 内部换基座）；
3. mjbase 设施挂账收口（批次见 §四 B8）。

非目标：
- luiox-repo 中 libjbytecode 包条目删除（仓级治理，另有消费方时保留）；
- libjbytecode 仓自身后续演进。

## 三、迁移原则

- Python 契约先行：mjt_pysdk_facade_test 全绿为唯一行为门；
- 每批一个 PR，全构建/回归/CI/合并生命周期；堆叠推进；
- fail-closed：facade 语义中「旧栈宽容面」（如 COMPUTE_FRAMES 对尾插死代码
  的帧容忍）在 libjbc 显式失败码上如实暴露，行为差异要在测试面显式断言；
- 公开仓消毒纪律全程适用（commit/PR/文档）。

## 四、批次

- **B0 基线**：mjbase/mjt 基线构建 + 全回归绿；计划文档入库。
- **B1 libjbc 升级**：两仓内联包定义 add_versions 追加 0.4.16/0.4.17（只追加
  不修改纪律），add_requires bump；全回归。验收：mjbase 1809+ 全绿。
- **B2 libjbc receiver-arity fix**：merge fix/method-parameters-receiver-arity
  （libjbc 仓 PR 生命周期），随 B1 或紧随其后消费。
- **B3 mjbase libjhasm 0.3.0**：jvm_jasm_service 换 libjbc 后端（majorVersion/
  minorVersion 直收 u16，去 jblw/version 依赖）；mjbase 去 libjblw。
  验收：jasm 语料 golden + 全回归。
- **B4 test_helper 夹具 vendor**：asm-test test_resource 51 文件收进 mjt
  test_resource/asm-test/，路径改仓内。验收：JDK 门测试全绿（跳过语义不变）。
- **B5 mjt mcp test fixture 重写**：withClassNameAndSignature 换
  readClass/ClassSession/Signature 挂载/writeClass + frame_rewire。验收：
  mjt_mcp_unittest 全绿。
- **B6 mjt-py facade 重写**（最大件，切片）：
  - 6.1 容器/读写面：open/write_back 换 ClassSession + writeClass + 帧/栈重算；
  - 6.2 树模型适配层：ClassNode/MethodNode/指令节点 → ClassUnit/CodeUnit 映射，
    uid ↔ InsnId（slot+gen），标签 ↔ LabelId；
  - 6.3 emit/edit 指令族（19 种 kind 的 emitNode/字段读写）；
  - 6.4 异常表/LVT/LNT 行面（含 keep 过滤语义）；
  - 6.5 adoptEphemeralMethod 管线临时方法 + rename/shadow 面（已基本不依赖
    tree，核对收尾）；
  - 6.6 xmake 去 libjbytecode + 全链回归（pysdk 测试套全绿）。
- **B7 依赖图收尾**：mjt xmake.lua 去 libjbytecode/libjblw require/requireconf/
  链接闭包项；luiox-repo libjbytecode 条目加弃用注记（不删）；两仓最终回归。
  - 执行注记（B7 落地）：libjblw 全退役（libjhasm 0.3.2 换 libjbc 后端后
    零消费方，unittest/jblw 消费验证套件随目标删除）；libjbytecode 的
    require/requireconf/链接闭包**保留**——mjbase.lib 的 relaxed_verifier
    仍在消费（1969 个符号引用，dumpbin 实证），mjbase 被产品二进制链接，
    闭包兜底不可去，随 B8 8.7（relaxed_verifier 移植 jhasm::analyze）
    一并收口；luiox-repo 两包条目加弃用注记。
  - 执行注记（B8 8.7 兑现）：mjbase PR #72 后 relaxed_verifier 直驱
    libjbc FrameEngine，mjbase.lib jbytecode 符号归零；mjt 侧闭包兜底
    随之全摘（PR #22/#23：root require/requireconfs、三处链接闭包，
    libgarena 零消费一并出链；libjbytecode/libjblw 至此全线退役）。
- **B8 mjbase 设施完善**（迁移完成后继续，同会话）：
  - 8.1 dev_plan 台账校准（A3/A4/A5 已在 libjbc 落地——stale 条目修正）；
  - 8.2 libjbc#10 move-range 原语实现（libjbc 仓：段重定位/标签随迁/边界
    标签策略/原子性）+ mjbase GotoRearranger 消费；
  - 8.3 结构化泛型/TypeAnnotation 读写 pass（libjbc sig_attr/type_annotation
    _attr 消费面，mjbase pass 侧收口）；
  - 8.4 resolveHandleCp 下沉评估（B7 挂账）；
  - 8.5 FR 相对状态更新（s+=D 分发形态）；
  - 8.6 UPC 逐中继细化（DUP 族吸收）；
  - 8.7 #44 long 族（位级恒等窗口规则表扩展）。
  - 执行注记（B8 全收口，2026-10-28）：
    - 8.1 ✅ mjbase PR #69（dev_plan 台账校准：A3/A4/A5、B1/B2 挂账解除）。
    - 8.2 ✅ 上游 libjbc PR #12（CodeDom::moveRange 机械层 + CodeEditor
      策略层，kHeadFollows/kHeadStays、StillUsed 裁决面、原子性；closes
      libjbc#10）+ PR #13（0.4.19 对齐）；消费侧 mjbase PR #71
      （GotoRearranger：前向非邻接 GOTO 目标尾段搬移消跳转，保守门五条，
      13 单测生产管线真链验证）。
    - 8.3 ✅ mjbase PR #73（Signature 解析树 span 手术 + TypeAnnotations/
      ParameterAnnotations/AnnotationDefault typed 结构化读写；
      class_renamer 开 bytecodeAndMetadata；嵌套链部件数不一致按 ASM
      SignatureRemapper 同语义坍缩）。
    - 8.4 ✅ 评估结论：下沉 jbc/core 纯池自由函数（非 ClassSession 成员
      ——BSM 本体/实参是裸 CP 索引拿不到 Insn；统一基准取 reference 版
      零拷贝 view + 严格 kind）。libjbc PR #14（cp_resolve.hpp 三原语）+
      PR #15（0.4.20 对齐）；mjbase PR #74 消费收编（两处换装 +
      cp_refs.hpp 转发头，alias_analysis 零改动；行为收紧 readClass 读闸
      下不可达，remapper 污染接缝真红→绿钉死）。评估另澄清 dev_plan
      T13 fail-open 属 AliasAnalysis 数据流缺口，与句柄解析无关。
    - 8.5 ✅ mjbase PR #70 之一（FR 写点三态分类：绝对 K→相对形态→拒收；
      iinc 禁用解除，状态迹模拟求相对写点定值）。
    - 8.6 ✅ mjbase PR #70 之二（UPC 帧面逐中继细化：pop 回溯 + DUP/DUP2
      拷贝别名折叠 + 连通分组栈平衡门；X 族/SWAP/cat2 保持 fail-closed）。
    - 8.7 ✅ mjbase PR #72（relaxed_verifier 直驱 FrameEngine——不走纯
      转发 jhasm::analyze，refCompatible 硬编码破协变语义；#44 long 族
      规则 15-20 真 JVM 对拍，窗口形态不与 int 族互为转写；libjbytecode
      全摘，残留符号 0）。
  - 联动版本面：libjbc 0.4.18→0.4.19→0.4.20（两仓内联定义与中心仓并存，
    版本条目保持同步写入）；mjbase ref 随 #72/#73/#74 递进（ci.yml 同步）。

## 五、风险

- facade 重写是行为面最大的手术：uid/标签/帧三套纪律的等价性靠既有测试套
  （含 pybind 面用例）钉死；任何无测试覆盖的行为差异先补测试再动刀。
- COMPUTE_FRAMES 语义差异（ASM 的 FramesComputation 与 jbc smt_build 失败码
  映射）需逐用例核对。
- 并行会话协调：mjbase worktree 曾出现他方 libjhasm 迁移在途痕迹（已消失，
  本计划全量接管重做）。
