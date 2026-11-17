# Assemble with smali 2.5.2: assemble -a 28 -o invoke_custom.dex smali_dynamic
.class public Lcom/mjt/dexfix/Dynamic;
.super Ljava/lang/Object;

.field public static value:I

.method public static bootstrap(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;)Ljava/lang/invoke/CallSite;
    .registers 4
    new-instance p0, Ljava/lang/invoke/ConstantCallSite;
    invoke-direct {p0, p3}, Ljava/lang/invoke/ConstantCallSite;-><init>(Ljava/lang/invoke/MethodHandle;)V
    return-object p0
.end method

.method public static identity(I)I
    .registers 1
    return p0
.end method

.method public static run(I)I
    .registers 2
    const-method-handle v0, static-get@Lcom/mjt/dexfix/Dynamic;->value:I
    invoke-custom {p0}, call_site("apply", (I)I, invoke-static@Lcom/mjt/dexfix/Dynamic;->identity(I)I)@Lcom/mjt/dexfix/Dynamic;->bootstrap(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;)Ljava/lang/invoke/CallSite;
    move-result v0
    invoke-custom/range {v0 .. v0}, call_site("apply", (I)I, invoke-static@Lcom/mjt/dexfix/Dynamic;->identity(I)I)@Lcom/mjt/dexfix/Dynamic;->bootstrap(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;)Ljava/lang/invoke/CallSite;
    move-result v0
    return v0
.end method
