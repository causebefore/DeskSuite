# 桥接组件：将旧版 IDF 内置组件名 json 映射到 managed component espressif__cjson。
# ESP-IDF v6.0 移除了内置 json 组件，esp-sr 等旧组件仍按 json 名引用。
idf_component_register(
    REQUIRES espressif__cjson
)
