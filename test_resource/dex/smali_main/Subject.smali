.class public Lcom/mjt/dexfix/Subject;
.super Lcom/mjt/dexfix/Base;
.source "Subject.java"
.implements Lcom/mjt/dexfix/Hook;
.field private static sCache:Ljava/lang/String;
.method public constructor <init>()V
    .registers 1
    invoke-direct {v0}, Lcom/mjt/dexfix/Base;-><init>()V
    return-void
.end method
.method public describe()Ljava/lang/String;
    .registers 3
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    sget-object v1, Lcom/mjt/dexfix/Base;->sTag:Ljava/lang/String;
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    move-result-object v0
    const/16 v1 42
    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
.method public onEvent(I)V
    .registers 3
    invoke-static {}, Lcom/mjt/dexfix/Base;->tag()Ljava/lang/String;
    move-result-object v0
    instance-of v1, v0, Ljava/lang/String;
    if-eqz v1, :cond_skip
    const-string v0, "hook-fired"
    sput-object v0, Lcom/mjt/dexfix/Subject;->sCache:Ljava/lang/String;
    :cond_skip
    return-void
.end method
.method public factory()Lcom/mjt/dexfix/Base;
    .registers 2
    new-instance v0, Lcom/mjt/dexfix/Subject;
    invoke-direct {v0}, Lcom/mjt/dexfix/Subject;-><init>()V
    check-cast v0, Lcom/mjt/dexfix/Base;
    return-object v0
.end method
