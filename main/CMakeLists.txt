idf_component_register(
        SRC_DIRS
        "."


        INCLUDE_DIRS
        "."
        "tusb"
)

idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
        set(tinyusb_name tinyusb)        # 本地组件
else()
        set(tinyusb_name espressif__tinyusb)    # 托管组件
endif()
idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/tusb")
target_sources(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/tusb/usb_descriptors.c")
