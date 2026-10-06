file(GLOB SOURCE_CODE
    "src/*.cc"
)

idf_component_register(
    SRCS 
        ${SOURCE_CODE}
    INCLUDE_DIRS 
        "include"
    REQUIRES 
        "lwip"
        "freertos"
        "esp_netif"
        "esp-tls"
        "my-background"
    PRIV_REQUIRES           #[[ 只在.cc中用到的组件 ]]
        "my-sysinfo"
)