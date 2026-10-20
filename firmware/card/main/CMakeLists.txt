set(priv_requires kscore ksbridge esp_timer)
if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND priv_requires usb) # USB PHY lives in the usb component before IDF 6
endif()

idf_component_register(
    SRCS "main.c" "ccid_usb.c"
    INCLUDE_DIRS "." "tusb"
    PRIV_REQUIRES ${priv_requires}
)

# TinyUSB compiles against our tusb_config.h (same approach as espressif's
# usb_device_uac component). Local copy or component-manager copy both work.
idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tinyusb_name tinyusb)
else()
    set(tinyusb_name espressif__tinyusb)
endif()
idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/tusb")
target_link_libraries(${COMPONENT_LIB} PRIVATE ${tusb_lib})
