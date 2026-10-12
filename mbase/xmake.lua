target("mbase")
    set_kind("static")
     add_deps("libjhasm")
     add_packages("libjstd")
     add_packages("libjbytecode", {public = true})
     add_packages("libjstr", {public = true})
     add_packages("libjzip", {public = true})
     add_packages("libjclassfile", {public = true})
    add_packages("libarchive")
    add_files("src/mbase/**.cpp")
    add_headerfiles("src/mbase/**.hpp")
    add_includedirs("src", {public = true})
    -- ArgParser 框架文案单源表(i18n_setup.cpp 静态注册)。
    add_files("src/mbase/cli/translations/*.lang", {rules = "libca.i18n.embed-lang"})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        add_links("bcrypt")
        -- xmllite：libarchive 静态库内含 xar 格式读取（依赖 CreateXmlReader），
        -- 链接 mbase 的可执行（mjt-mcp / mjt_mcp_unittest）需解析该符号。public 传递给下游。
        add_syslinks("xmllite", {public = true})
    end

