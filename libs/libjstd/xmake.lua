target("libjstd")
    set_kind("headeronly")
    set_group("libs")

     -- libca 链接经包 public 传播（去垫片）
    add_headerfiles("src/jstd/**.hpp")
    add_includedirs("src", {public = true})

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

