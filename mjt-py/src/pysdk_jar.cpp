#include "pysdk_jar.hpp"

#include "mjbase/jar/manifest.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

#include "jbytecode/jbytecode.hpp"

#include <cstring>
#include <filesystem>

namespace mjt::pysdk {

using jbytecode::tree::ClassNode;
using mjbase::model::ClassResource;

// ── 合成演示 jar（unittest + 新样本演示共用；仅写 %TEMP%/测试目录） ─────────

namespace {

using ca::str::Utf8StringRef;
using jbytecode::core::Label;
using jbytecode::core::Opcodes;
using namespace jbytecode::tree;

struct SynthClass
{
    std::unique_ptr<ClassNode> node;
    ca::str::Utf8StringArena   arena;   // 类字符串归属（节点视图寿命=本对象）

    explicit SynthClass(const char* name)
        : node(std::make_unique<ClassNode>())
    {
        node->version   = (0 << 16) | 52;   // major 52 (Java 8)
        node->access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
        node->name      = arena.intern(Utf8StringRef::from_cstr(name));
        node->superName = arena.intern(Utf8StringRef::from_cstr("java/lang/Object"));
    }

    MethodNode& addMethod(const char* name, const char* desc, int access, int maxStack,
                          int maxLocals)
    {
        static const std::vector<Utf8StringRef> kNoExceptions;
        // MethodNode 本体是堆对象（ClassNode 析构统一 delete，nodes.hpp 契约）；
        // 类级 create<T> 仅用于注解/innerClasses 等类内容物。
        auto* m = new MethodNode(access, arena.intern(Utf8StringRef::from_cstr(name)),
                                 arena.intern(Utf8StringRef::from_cstr(desc)),
                                 Utf8StringRef{}, kNoExceptions);
        m->maxStack  = maxStack;
        m->maxLocals = maxLocals;
        node->methods.push_back(m);
        return *m;
    }

    std::vector<ca::u8> bytes()
    {
        jbytecode::core::ClassWriter writer(arena, 0);
        node->accept(&writer);
        return writer.toByteArray();
    }
};

/// 恒等 rethrow handler（静态恒等包装形态）：
///   L0: <protected>            ; try 体
///   L1: goto Lend              ; 跳板（handler 前驱不可 fallthrough）
///   Lh: astore 1; aload 1;
///       invokestatic wrap; athrow
///   Lend: return
static void appendIdentityHandlerMethod(SynthClass&  cls,
                                        const char*  name,
                                        const char*  desc,
                                        const char*  wrapOwner,
                                        const char*  wrapName,
                                        const char*  wrapDesc)
{
    auto& m = cls.addMethod(name, desc, Opcodes::ACC_PUBLIC, 2, 2);
    auto* l0 = m.create<LabelNode>(new Label());
    auto* l1 = m.create<LabelNode>(new Label());
    auto* lh = m.create<LabelNode>(new Label());
    auto* le = m.create<LabelNode>(new Label());

    m.instructions.add(l0);
    m.instructions.add(m.create<InsnNode>(Opcodes::ICONST_0));
    m.instructions.add(m.create<InsnNode>(Opcodes::POP));
    m.instructions.add(l1);
    m.instructions.add(m.create<JumpInsnNode>(Opcodes::GOTO, le->label));
    m.instructions.add(lh);
    m.instructions.add(m.create<VarInsnNode>(Opcodes::ASTORE, 1));
    m.instructions.add(m.create<VarInsnNode>(Opcodes::ALOAD, 1));
    m.instructions.add(m.create<MethodInsnNode>(Opcodes::INVOKESTATIC,
                                                cls.arena.intern(Utf8StringRef::from_cstr(wrapOwner)),
                                                cls.arena.intern(Utf8StringRef::from_cstr(wrapName)),
                                                cls.arena.intern(Utf8StringRef::from_cstr(wrapDesc)),
                                                false));
    m.instructions.add(m.create<InsnNode>(Opcodes::ATHROW));
    m.instructions.add(le);
    m.instructions.add(m.create<InsnNode>(Opcodes::RETURN));

    m.tryCatchBlocks.push_back(m.create<TryCatchBlockNode>(
        l0->label, l1->label, lh->label,
        cls.arena.intern(Utf8StringRef::from_cstr("java/io/IOException"))));
}

}   // namespace

void writeSyntheticDemoJar(const std::string& outJarPath)
{
    // me/demo/Api：接口（class_hierarchy 闭包查询的容器内接口样本）。
    SynthClass api("me/demo/Api");
    api.node->access = Opcodes::ACC_PUBLIC | Opcodes::ACC_INTERFACE | Opcodes::ACC_ABSTRACT;
    api.node->superName = api.arena.intern(Utf8StringRef::from_cstr("java/lang/Object"));
    {
        // 抽象方法：接口不写方法体（ClassWriter 对 ACC_INTERFACE 抽象方法直出无 code）。
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_ABSTRACT,
                                 api.arena.intern(Utf8StringRef::from_cstr("id")),
                                 api.arena.intern(
                                     Utf8StringRef::from_cstr("(Ljava/lang/Object;)Ljava/lang/Object;")),
                                 Utf8StringRef{}, std::vector<Utf8StringRef>{});
        api.node->methods.push_back(m);
    }

    // me/demo/Util：普通工具类。
    SynthClass util("me/demo/Util");
    {
        auto& m = util.addMethod("add", "(II)I", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, 2, 2);
        m.instructions.add(m.create<VarInsnNode>(Opcodes::ILOAD, 0));
        m.instructions.add(m.create<VarInsnNode>(Opcodes::ILOAD, 1));
        m.instructions.add(m.create<InsnNode>(Opcodes::IADD));
        m.instructions.add(m.create<InsnNode>(Opcodes::IRETURN));
    }
    auto& utilWrap =
        util.addMethod("wrap", "(Ljava/io/IOException;)Ljava/io/IOException;",
                       Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, 1, 1);
    utilWrap.instructions.add(utilWrap.create<VarInsnNode>(Opcodes::ALOAD, 0));
    utilWrap.instructions.add(utilWrap.create<InsnNode>(Opcodes::ARETURN));

    // me/demo/Crypto：解密器形态站点（invokestatic (II)Ljava/lang/String;）+ 自身 handler。
    SynthClass crypto("me/demo/Crypto");
    {
        auto& site = crypto.addMethod("decode", "(II)Ljava/lang/String;",
                                      Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, 2, 2);
        site.instructions.add(site.create<InsnNode>(Opcodes::ICONST_1));
        site.instructions.add(site.create<IntInsnNode>(Opcodes::BIPUSH, 7));
        site.instructions.add(site.create<MethodInsnNode>(
            Opcodes::INVOKESTATIC,
            crypto.arena.intern(Utf8StringRef::from_cstr("me/demo/Crypto")),
            crypto.arena.intern(Utf8StringRef::from_cstr("dec")),
            crypto.arena.intern(Utf8StringRef::from_cstr("(II)Ljava/lang/String;")), false));
        site.instructions.add(site.create<InsnNode>(Opcodes::ARETURN));
    }
    {
        auto& dec = crypto.addMethod("dec", "(II)Ljava/lang/String;",
                                     Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, 1, 2);
        dec.instructions.add(
            dec.create<LdcInsnNode>(ca::core::Any(Utf8StringRef::from_cstr("HIDDEN"))));
        dec.instructions.add(dec.create<InsnNode>(Opcodes::ARETURN));
    }
    appendIdentityHandlerMethod(crypto, "guarded", "()V", "me/demo/Crypto", "wrap",
                                "(Ljava/io/IOException;)Ljava/io/IOException;");
    {
        // wrap（crypto 侧同名恒等包装，保证样例自洽）。
        auto& wrap =
            crypto.addMethod("wrap", "(Ljava/io/IOException;)Ljava/io/IOException;",
                             Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, 1, 1);
        wrap.instructions.add(wrap.create<VarInsnNode>(Opcodes::ALOAD, 0));
        wrap.instructions.add(wrap.create<InsnNode>(Opcodes::ARETURN));
    }

    // me/demo/App：入口 + 恒等 handler + 对 Crypto/Util 的引用。
    SynthClass app("me/demo/App");
    {
        auto& init = app.addMethod("<init>", "()V", Opcodes::ACC_PUBLIC, 1, 1);
        init.instructions.add(init.create<VarInsnNode>(Opcodes::ALOAD, 0));
        init.instructions.add(init.create<MethodInsnNode>(
            Opcodes::INVOKESPECIAL,
            app.arena.intern(Utf8StringRef::from_cstr("java/lang/Object")),
            app.arena.intern(Utf8StringRef::from_cstr("<init>")),
            app.arena.intern(Utf8StringRef::from_cstr("()V")), false));
        init.instructions.add(init.create<InsnNode>(Opcodes::RETURN));
    }
    {
        auto& main = app.addMethod(
            "main", "([Ljava/lang/String;)V", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, 2, 1);
        main.instructions.add(main.create<FieldInsnNode>(
            Opcodes::GETSTATIC,
            app.arena.intern(Utf8StringRef::from_cstr("java/lang/System")),
            app.arena.intern(Utf8StringRef::from_cstr("out")),
            app.arena.intern(Utf8StringRef::from_cstr("Ljava/io/PrintStream;"))));
        main.instructions.add(
            main.create<LdcInsnNode>(ca::core::Any(Utf8StringRef::from_cstr("demo-payload"))));
        main.instructions.add(main.create<MethodInsnNode>(
            Opcodes::INVOKEVIRTUAL,
            app.arena.intern(Utf8StringRef::from_cstr("java/io/PrintStream")),
            app.arena.intern(Utf8StringRef::from_cstr("println")),
            app.arena.intern(Utf8StringRef::from_cstr("(Ljava/lang/String;)V")), false));
        main.instructions.add(main.create<MethodInsnNode>(
            Opcodes::INVOKESTATIC,
            app.arena.intern(Utf8StringRef::from_cstr("me/demo/Crypto")),
            app.arena.intern(Utf8StringRef::from_cstr("decode")),
            app.arena.intern(Utf8StringRef::from_cstr("(II)Ljava/lang/String;")), false));
        main.instructions.add(main.create<InsnNode>(Opcodes::POP));
        main.instructions.add(main.create<InsnNode>(Opcodes::RETURN));
    }
    appendIdentityHandlerMethod(app, "guarded", "()V", "me/demo/Util", "wrap",
                                "(Ljava/io/IOException;)Ljava/io/IOException;");

    // me/demo/Color：enum values() 形态样本（混淆罐数组型 owner 悬空的合成复现位，
    // 零样本字节——minzi 姿态自造）。$VALUES 静态字段 + values() 经
    // `invokevirtual [Lme/demo/Color;.clone()` 克隆数组：类改名时该数组型 owner
    // 必须随元素类名同步改写，否则 receiver 与 owner 失配、真机一调 values() 即炸。
    SynthClass color("me/demo/Color");
    color.node->interfaces.push_back(
        color.arena.intern(Utf8StringRef::from_cstr("me/demo/Api")));
    {
        auto* values = new FieldNode(Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC | Opcodes::ACC_FINAL,
                                     color.arena.intern(Utf8StringRef::from_cstr("$VALUES")),
                                     color.arena.intern(Utf8StringRef::from_cstr("[Lme/demo/Color;")),
                                     Utf8StringRef{}, ca::core::Any());
        color.node->fields.push_back(values);
    }
    {
        auto& vals = color.addMethod("values", "()[Lme/demo/Color;",
                                     Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, 1, 0);
        vals.instructions.add(vals.create<FieldInsnNode>(
            Opcodes::GETSTATIC,
            color.arena.intern(Utf8StringRef::from_cstr("me/demo/Color")),
            color.arena.intern(Utf8StringRef::from_cstr("$VALUES")),
            color.arena.intern(Utf8StringRef::from_cstr("[Lme/demo/Color;"))));
        vals.instructions.add(vals.create<MethodInsnNode>(
            Opcodes::INVOKEVIRTUAL,
            color.arena.intern(Utf8StringRef::from_cstr("[Lme/demo/Color;")),
            color.arena.intern(Utf8StringRef::from_cstr("clone")),
            color.arena.intern(Utf8StringRef::from_cstr("()Ljava/lang/Object;")), false));
        vals.instructions.add(vals.create<TypeInsnNode>(
            Opcodes::CHECKCAST,
            color.arena.intern(Utf8StringRef::from_cstr("[Lme/demo/Color;"))));
        vals.instructions.add(vals.create<InsnNode>(Opcodes::ARETURN));
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
        auto& m = app.addMethod("run", "()V", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, 0, 0);
        m.instructions.add(m.create<InsnNode>(Opcodes::RETURN));
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
