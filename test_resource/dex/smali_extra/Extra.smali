.class public Lcom/mjt/dexfix/Extra;
.super Ljava/lang/Object;
.source "Extra.java"
.field public static final magic:J = 0x1122334455667788L
.method public constructor <init>()V
    .registers 1
    invoke-direct {v0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public static wide()J
    .registers 4
    const-wide v0 0x1122334455667788L
    return-wide v0
.end method
.method public static kind(Lcom/mjt/dexfix/Base;)I
    .registers 2
    instance-of v0, v1, Lcom/mjt/dexfix/Subject;
    return v0
.end method
.method public static clazz()Ljava/lang/Class;
    .registers 1
    const-class v0, Lcom/mjt/dexfix/Base;
    return-object v0
.end method
