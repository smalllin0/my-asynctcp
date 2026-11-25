idf_component_register(
    SRCS
        # 显式列出，不用 file(GLOB)：IDF 预扫描阶段 GLOB 会报
        # "invalid for script and find package modes"（加 CONFIGURE_DEPENDS 时），
        # 不加则新增 .cc 不重新 configure 就不进构建，症状是链接期 undefined reference。
        "src/AsyncConnection.cc"
        "src/AsyncServer.cc"
        "src/ConnPool.cc"
        "src/ConnRegistry.cc"
        "src/LwipWrapper.cc"
        "src/SendPool.cc"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS       #[[ 内部头：只有本组件的 .cc 能看到 ]]
        "src"
    REQUIRES
        "lwip"
        "freertos"
        "esp_netif"
        "esp_timer"
        "my-background"
    PRIV_REQUIRES           #[[ 只在.cc中用到的组件 ]]
        "my-sysinfo"
)
