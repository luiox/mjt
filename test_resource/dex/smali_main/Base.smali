.class public Lcom/mjt/dexfix/Base;
.super Ljava/lang/Object;
.source "Base.java"
.field private mValue:I
.field public static final sTag:Ljava/lang/String; = "dexfix-base"
.method public constructor <init>()V
    .registers 1
    invoke-direct {v0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public getValue()I
    .registers 1
    iget v0, v0, Lcom/mjt/dexfix/Base;->mValue:I
    return v0
.end method
.method public setValue(I)V
    .registers 2
    iput v1, v0, Lcom/mjt/dexfix/Base;->mValue:I
    return-void
.end method
.method public static tag()Ljava/lang/String;
    .registers 1
    sget-object v0, Lcom/mjt/dexfix/Base;->sTag:Ljava/lang/String;
    return-object v0
.end method
