#pragma once

#include <memory>
#include <string>
#include <functional>
#include <vector>

#include "libca/core/datatype.hpp"
#include "mbase/jar/jar_reader.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_model.hpp"

//── JarContext ─────────────────────────────────────────────────
//所有分析命令共用：打开 JAR → 遍历 class → 提取信息

struct JarContext
{
    std::unique_ptr<mjt::base::IResourceContainer> container;
    std::string                                        jarPath;

    ///@brief 打开 JAR 文件，失败返回 false 并设置 lastError
    ///@param decoder 条目名宽容解码回调（opt-in，族；空 = 原始字节直通，
    ///缺省零漂移。注入 text_codec::toCanonicalUtf8 后 GBK 无 flag 条目名规范化
    ///为合法 UTF-8 再进资源 URI，与 jar list/extract 的 --lenient-names 同口径）。
    bool open(const std::string& path,
              const mjt::base::IJarReader::EntryNameDecoder& decoder = {});

    ///@brief 只读遍历所有 class 资源
    void visitClasses(
        std::function<void(const std::string& name, const mjt::base::ClassResource&)> fn) const;

    ///@brief 可修改遍历（用于 jar fix 等）
    void transformClasses(
        std::function<void(const std::string& name, mjt::base::ClassResource&)> fn);

    ///@brief 获取错误信息
    const std::string& error() const { return lastError_; }

private:
    std::string lastError_;
};
