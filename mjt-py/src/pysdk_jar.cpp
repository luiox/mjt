#include "pysdk_jar.hpp"

#include "jbc/jbc.hpp"

#include "mjbase/jar/manifest.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace mjt::pysdk {

using mjbase::model::ClassResource;

// ── 合成演示 jar（unittest + 新样本演示共用；仅写 %TEMP%/测试目录） ─────────
//
// RFC 0002 迁移批 B6：合成面从 jbytecode tree 换 libjbc 会话（A5 种子建类 +
// CodeDom 直排 + lowerMethod 落盘）。指令 CP 操作数经会话语义 intern（去重
// 落位）；标签/异常表用 owner-scoped LabelId；写回 lowerMethod + writeClass。

namespace {

using ca::str::Utf8StringRef;
namespace ir = jbc::ir;
namespace core = jbc::core;

/// Insn 直排便利件：kNoPc = 合成指令（无源布局），CP 槽由调用方 intern 后填。
ir::Insn insnOf(core::Op op)
{
    ir::Insn in;
    in.op    = op;
    in.srcPc = ir::kNoPc;
    return in;
}
/// 局部变量访问的专用码基座（_0 形态）；非局部变量族原样返回。
constexpr core::Op dedicatedLoadStoreBase(core::Op op)
{
    switch (op) {
        case core::Op::ILOAD:  return core::Op::ILOAD_0;
        case core::Op::LLOAD:  return core::Op::LLOAD_0;
        case core::Op::FLOAD:  return core::Op::FLOAD_0;
        case core::Op::DLOAD:  return core::Op::DLOAD_0;
        case core::Op::ALOAD:  return core::Op::ALOAD_0;
        case core::Op::ISTORE: return core::Op::ISTORE_0;
        case core::Op::LSTORE: return core::Op::LSTORE_0;
        case core::Op::FSTORE: return core::Op::FSTORE_0;
        case core::Op::DSTORE: return core::Op::DSTORE_0;
        case core::Op::ASTORE: return core::Op::ASTORE_0;
        default:               return op;
    }
}

ir::Insn varInsnOf(core::Op op, int var)
{
    // 局部变量折叠：index ≤ 3 落 JVM 专用码（iload_0 族）——javac 口径，也是
    // 旧树管线写出口径。libjbc writeClass 忠实序列化不折叠，合成语料在此
    // 折叠以保证 jasm 文本往返恒等（render(assemble(render(x))) 稳定）。
    const core::Op base = dedicatedLoadStoreBase(op);
    core::Op chosen     = op;
    if (base != op && var >= 0 && var <= 3) {
        chosen = static_cast<core::Op>(static_cast<int>(base) + var);
    }
    auto in = insnOf(chosen);
    in.a    = var;
    return in;
}
ir::Insn cpInsnOf(core::Op op, ca::u32 cp)
{
    auto in   = insnOf(op);
    in.cp     = cp;
    return in;
}

/// 会话建类的公共尾：commit 类级元数据 + lower 全部带体方法 + 写字节。
std::vector<ca::u8> finishClass(ir::ClassSession& s)
{
    auto& unit = s.unit();
    // unit 上的后置类级修改（access/interfaces/super）不经 commit 不进
    // backing——MethodUnit::code 同族的会话契约：unit 视图 ≠ backing。
    if (s.commitClassMetadata().is_err())
        throw std::runtime_error("demo jar: commitClassMetadata failed");
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (unit.methods[i].code == nullptr) continue;
        const auto lowered = ir::lowerMethod(s, unit.methods[i].id);
        if (lowered.is_err())
            throw std::runtime_error(std::string("demo jar: lowerMethod failed: ") +
                                     ir::to_string(lowered.unwrap_err()));
    }
    auto written = core::writeClass(s.backing());
    if (!written.is_ok())
        throw std::runtime_error(std::string("demo jar: writeClass failed: ") +
                                 core::to_string(written.unwrap_err()));
    return std::move(written).unwrap();
}

struct SynthClass
{
    std::shared_ptr<ir::ClassSession> session;

    explicit SynthClass(const char* name)
    {
        ir::NewClassSeed seed;
        seed.access    = 0x0021;   // ACC_PUBLIC | ACC_SUPER
        seed.thisName  = name;
        seed.superName = "java/lang/Object";
        auto made      = ir::makeSession(seed);
        if (!made.is_ok()) throw std::runtime_error("demo jar: seed session failed");
        session = std::move(made).unwrap();
    }

    /// 带体方法（空 Code 体 + maxes；指令由调用方经 dom() 直排）。
    ir::MethodId addMethod(const char* name, const char* desc, int access, int maxStack,
                           int maxLocals)
    {
        ir::MethodSeed seed;
        seed.name      = name;
        seed.desc      = desc;
        seed.access    = static_cast<ca::u32>(access);
        seed.withCode  = true;
        seed.maxStack  = static_cast<ca::u16>(maxStack);
        seed.maxLocals = static_cast<ca::u16>(maxLocals);
        auto added     = session->addMethod(seed);
        if (added.is_err())
            throw std::runtime_error(std::string("demo jar: addMethod failed: ") +
                                     ir::to_string(added.unwrap_err()));
        const auto id = added.unwrap();
        // withCode 只落空 Code 属性：IR CodeUnit 在 promoteMethod 后才非空
        // （MethodUnit::code 契约）。fresh 槽位恰好 promote 一次，无重入失效面。
        const auto promoted = ir::promoteMethod(*session, id);
        if (promoted.is_err())
            throw std::runtime_error(std::string("demo jar: promoteMethod failed: ") +
                                     ir::to_string(promoted.unwrap_err()));
        return id;
    }

    /// 无体方法（接口抽象形态）。
    ir::MethodId addAbstractMethod(const char* name, const char* desc, int access)
    {
        ir::MethodSeed seed;
        seed.name     = name;
        seed.desc     = desc;
        seed.access   = static_cast<ca::u32>(access);
        seed.withCode = false;
        auto added    = session->addMethod(seed);
        if (added.is_err())
            throw std::runtime_error(std::string("demo jar: addAbstractMethod failed: ") +
                                     ir::to_string(added.unwrap_err()));
        return added.unwrap();
    }

    /// 已带体方法的 promote 视图（appendIdentityHandlerMethod 的直排面）。
    ir::CodeUnit& body(ir::MethodId id) { return *session->unit().method(id)->code; }

    ca::u32 internClass(const char* n) { return session->internClass(n); }
    ca::u32 internFieldRef(const char* o, const char* n, const char* d)
    {
        return session->internFieldRef(o, n, d);
    }
    ca::u32 internMethodRef(const char* o, const char* n, const char* d, bool itf)
    {
        return itf ? session->internInterfaceMethodRef(o, n, d)
                   : session->internMethodRef(o, n, d);
    }
    ca::u32 internString(const char* v) { return session->internString(v); }

    std::vector<ca::u8> bytes() { return finishClass(*session); }
};

/// 恒等 rethrow handler（静态恒等包装形态）：
///   L0: <protected>            ; try 体
///   L1: goto Lend              ; 跳板（handler 前驱不可 fallthrough）
///   Lh: astore 1; aload 1;
///       invokestatic wrap; athrow
///   Lend: return
void appendIdentityHandlerMethod(SynthClass&  cls,
                                 const char*  name,
                                 const char*  desc,
                                 const char*  wrapOwner,
                                 const char*  wrapName,
                                 const char*  wrapDesc)
{
    const auto id   = cls.addMethod(name, desc, 0x0001 /*ACC_PUBLIC*/, 2, 2);
    auto&      dom  = cls.body(id).dom;

    // 前向分支（goto Lend → 方法尾 return）：源顺序直排时目标指令尚未
    // 落位，append 期「控制目标须已锚定」防呆机制性不可满足——整体方法体
    // 直排进入受控延后绑定构建态（design.md §34.3），全部落位后收尾补验。
    dom.beginDeferredTargets();

    auto put = [&dom](ir::Insn in) -> ir::InsnId {
        auto made = dom.append(std::move(in));
        if (made.is_err())
            throw std::runtime_error(std::string("demo jar: append failed: ") +
                                     ir::to_string(made.unwrap_err()));
        return made.unwrap();
    };
    const auto iconst = put(insnOf(core::Op::ICONST_0));
    const auto pop    = put(insnOf(core::Op::POP));
    const auto ggoto  = put(insnOf(core::Op::GOTO));
    const auto astore = put(varInsnOf(core::Op::ASTORE, 1));
    const auto aload  = put(varInsnOf(core::Op::ALOAD, 1));
    const auto invoke = put(cpInsnOf(core::Op::INVOKESTATIC,
                                     cls.internMethodRef(wrapOwner, wrapName, wrapDesc, false)));
    const auto athrow = put(insnOf(core::Op::ATHROW));
    const auto ret    = put(insnOf(core::Op::RETURN));

    auto endBinding = [&](ir::LabelId& out, ir::InsnId anchor) {
        auto made = dom.createLabel(anchor);
        if (made.is_err())
            throw std::runtime_error(std::string("demo jar: createLabel failed: ") +
                                     ir::to_string(made.unwrap_err()));
        out = made.unwrap();
    };
    ir::LabelId l0, l1, lh, le;
    endBinding(l0, iconst);
    endBinding(l1, ggoto);
    endBinding(lh, astore);
    endBinding(le, ret);   // Lend 锚在 return 前沿（旧树序里 le 在 return 之前）

    dom.get(ggoto).unwrap()->target = le;

    auto endState = dom.endDeferredTargets();
    if (endState.is_err())
        throw std::runtime_error(std::string("demo jar: dangling branch target: ") +
                                 ir::to_string(endState.unwrap_err()));

    auto& cu   = cls.body(id);
    cu.exc.push_back({l0, l1, lh, cls.internClass("java/io/IOException")});
}

}   // namespace

void writeSyntheticDemoJar(const std::string& outJarPath)
{
    // me/demo/Api：接口（class_hierarchy 闭包查询的容器内接口样本）。
    SynthClass api("me/demo/Api");
    api.session->unit().access = 0x0001 | 0x0200 | 0x0400;   // PUBLIC | INTERFACE | ABSTRACT
    {
        // 抽象方法：接口不写方法体。
        api.addAbstractMethod("id", "(Ljava/lang/Object;)Ljava/lang/Object;",
                              0x0001 | 0x0400);
    }

    // me/demo/Util：普通工具类。
    SynthClass util("me/demo/Util");
    {
        const auto id  = util.addMethod("add", "(II)I", 0x0001 | 0x0008, 2, 2);
        auto&      dom = util.body(id).dom;
        dom.append(varInsnOf(core::Op::ILOAD, 0));
        dom.append(varInsnOf(core::Op::ILOAD, 1));
        dom.append(insnOf(core::Op::IADD));
        dom.append(insnOf(core::Op::IRETURN));
    }
    {
        const auto id  = util.addMethod("wrap", "(Ljava/io/IOException;)Ljava/io/IOException;",
                                        0x0001 | 0x0008, 1, 1);
        auto&      dom = util.body(id).dom;
        dom.append(varInsnOf(core::Op::ALOAD, 0));
        dom.append(insnOf(core::Op::ARETURN));
    }

    // me/demo/Crypto：解密器形态站点（invokestatic (II)Ljava/lang/String;）+ 自身 handler。
    SynthClass crypto("me/demo/Crypto");
    {
        const auto id  = crypto.addMethod("decode", "(II)Ljava/lang/String;",
                                          0x0001 | 0x0008, 2, 2);
        auto&      dom = crypto.body(id).dom;
        dom.append(insnOf(core::Op::ICONST_1));
        auto bipush = insnOf(core::Op::BIPUSH);
        bipush.a    = 7;
        dom.append(bipush);
        dom.append(cpInsnOf(core::Op::INVOKESTATIC,
                            crypto.internMethodRef("me/demo/Crypto", "dec",
                                                   "(II)Ljava/lang/String;", false)));
        dom.append(insnOf(core::Op::ARETURN));
    }
    {
        const auto id  = crypto.addMethod("dec", "(II)Ljava/lang/String;",
                                          0x0001 | 0x0008, 1, 2);
        auto&      dom = crypto.body(id).dom;
        dom.append(cpInsnOf(core::Op::LDC, crypto.internString("HIDDEN")));
        dom.append(insnOf(core::Op::ARETURN));
    }
    appendIdentityHandlerMethod(crypto, "guarded", "()V", "me/demo/Crypto", "wrap",
                                "(Ljava/io/IOException;)Ljava/io/IOException;");
    {
        // wrap（crypto 侧同名恒等包装，保证样例自洽）。
        const auto id  = crypto.addMethod("wrap", "(Ljava/io/IOException;)Ljava/io/IOException;",
                                          0x0001 | 0x0008, 1, 1);
        auto&      dom = crypto.body(id).dom;
        dom.append(varInsnOf(core::Op::ALOAD, 0));
        dom.append(insnOf(core::Op::ARETURN));
    }

    // me/demo/App：入口 + 恒等 handler + 对 Crypto/Util 的引用。
    SynthClass app("me/demo/App");
    {
        const auto id  = app.addMethod("<init>", "()V", 0x0001, 1, 1);
        auto&      dom = app.body(id).dom;
        dom.append(varInsnOf(core::Op::ALOAD, 0));
        dom.append(cpInsnOf(core::Op::INVOKESPECIAL,
                            app.internMethodRef("java/lang/Object", "<init>", "()V", false)));
        dom.append(insnOf(core::Op::RETURN));
    }
    {
        const auto id  = app.addMethod("main", "([Ljava/lang/String;)V",
                                       0x0001 | 0x0008, 2, 1);
        auto&      dom = app.body(id).dom;
        dom.append(cpInsnOf(core::Op::GETSTATIC,
                            app.internFieldRef("java/lang/System", "out",
                                               "Ljava/io/PrintStream;")));
        dom.append(cpInsnOf(core::Op::LDC, app.internString("demo-payload")));
        dom.append(cpInsnOf(core::Op::INVOKEVIRTUAL,
                            app.internMethodRef("java/io/PrintStream", "println",
                                                "(Ljava/lang/String;)V", false)));
        // decode(II) 需要真参压栈：旧树版夹具空栈调用（线性块无合并点，
        // 旧 COMPUTE_FRAMES 不校验栈深侥幸过），libjbc FrameEngine 真验证
        // fail-closed——夹具字节码本身修成合法形态。
        dom.append(insnOf(core::Op::ICONST_1));
        dom.append(insnOf(core::Op::ICONST_2));
        dom.append(cpInsnOf(core::Op::INVOKESTATIC,
                            app.internMethodRef("me/demo/Crypto", "decode",
                                                "(II)Ljava/lang/String;", false)));
        dom.append(insnOf(core::Op::POP));
        dom.append(insnOf(core::Op::RETURN));
    }
    appendIdentityHandlerMethod(app, "guarded", "()V", "me/demo/Util", "wrap",
                                "(Ljava/io/IOException;)Ljava/io/IOException;");

    // me/demo/Color：enum values() 形态样本（混淆罐数组型 owner 悬空的合成复现位，
    // 零样本字节——minzi 姿态自造）。$VALUES 静态字段 + values() 经
    // `invokevirtual [Lme/demo/Color;.clone()` 克隆数组：类改名时该数组型 owner
    // 必须随元素类名同步改写，否则 receiver 与 owner 失配、真机一调 values() 即炸。
    SynthClass color("me/demo/Color");
    color.session->unit().interfaces.push_back(color.session->unit().symbols().intern("me/demo/Api"));
    {
        ir::FieldSeed seed;
        seed.name   = "$VALUES";
        seed.desc   = "[Lme/demo/Color;";
        seed.access = 0x0002 | 0x0008 | 0x0010;   // PRIVATE | STATIC | FINAL
        const auto fieldAdded = color.session->addField(seed);
        if (fieldAdded.is_err())
            throw std::runtime_error(std::string("demo jar: addField failed: ") +
                                     ir::to_string(fieldAdded.unwrap_err()));
    }
    {
        const auto id  = color.addMethod("values", "()[Lme/demo/Color;",
                                         0x0001 | 0x0008, 1, 0);
        auto&      dom = color.body(id).dom;
        dom.append(cpInsnOf(core::Op::GETSTATIC,
                            color.internFieldRef("me/demo/Color", "$VALUES",
                                                 "[Lme/demo/Color;")));
        dom.append(cpInsnOf(core::Op::INVOKEVIRTUAL,
                            color.internMethodRef("[Lme/demo/Color;", "clone",
                                                  "()Ljava/lang/Object;", false)));
        dom.append(cpInsnOf(core::Op::CHECKCAST, color.internClass("[Lme/demo/Color;")));
        dom.append(insnOf(core::Op::ARETURN));
    }

    mjbase::model::ResourceContainer container;
    const std::pair<const char*, SynthClass*> entries[] = {
        {"me/demo/App.class", &app},     {"me/demo/Api.class", &api},
        {"me/demo/Color.class", &color}, {"me/demo/Crypto.class", &crypto},
        {"me/demo/Util.class", &util},
    };
    for (auto& [uri, cls] : entries) {
        container.addResource(uri,
                              std::make_unique<mjbase::model::ClassResource>(
                                  std::string(uri), cls->bytes()));
    }
    const auto exported = mjbase::model::ResourceHelper::exportToJar(container, outJarPath);
    if (exported.is_err()) throw std::runtime_error("export demo jar failed: " + outJarPath);
}

void writeSyntheticManifestJar(const std::string& outJarPath)
{
    SynthClass app("me/demo/App");
    {
        const auto id  = app.addMethod("run", "()V", 0x0001 | 0x0008, 0, 0);
        auto&      dom = app.body(id).dom;
        dom.append(insnOf(core::Op::RETURN));
    }

    // 可解析的最小 MANIFEST：CRLF 续行结构 + class 形态主属性（供 token 改名
    // 的 manifest 重写面断言）。
    const char* manifestText =
        "Manifest-Version: 1.0\r\nCreated-By: mjt-py synthetic\r\n"
        "Main-Class: me.demo.App\r\n\r\n";
    const std::vector<ca::u8> manifestBytes(manifestText,
                                            manifestText + std::strlen(manifestText));

    mjbase::model::ResourceContainer container;
    container.addResource("META-INF/MANIFEST.MF",
                          std::make_unique<mjbase::model::ManifestResource>(
                              mjbase::jar::Manifest::fromBytes(manifestBytes)));
    container.addResource("me/demo/App.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              std::string("me/demo/App.class"), app.bytes()));
    const auto exported = mjbase::model::ResourceHelper::exportToJar(container, outJarPath);
    if (exported.is_err()) throw std::runtime_error("export demo jar failed: " + outJarPath);
}

}   // namespace mjt::pysdk
