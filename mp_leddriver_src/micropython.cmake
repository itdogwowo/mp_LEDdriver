# Create an INTERFACE library for our module
add_library(mp_leddriver_lib INTERFACE)

# Add source files
target_sources(mp_leddriver_lib INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mp_leddriver.c
    ${CMAKE_CURRENT_LIST_DIR}/bus_i8080.c
    ${CMAKE_CURRENT_LIST_DIR}/strip_encoder.c
)

# Add local include directories
target_include_directories(mp_leddriver_lib INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Link to usermod
target_link_libraries(usermod INTERFACE mp_leddriver_lib)

# ----------------------------------------------------------------
# HARDCODED IDF INCLUDE PATHS (The "Nuclear Option")
# ----------------------------------------------------------------
# Since automatic dependency propagation failed, we manually add 
# the include paths for required ESP-IDF components.

if(DEFINED IDF_PATH)
    # Helper macro to safely add include directories
    macro(safe_add_include path)
        if(EXISTS "${path}")
            target_include_directories(usermod INTERFACE "${path}")
        endif()
    endmacro()

    # Add standard component paths for ESP-IDF v5.x
    safe_add_include("${IDF_PATH}/components/esp_lcd/include")
    safe_add_include("${IDF_PATH}/components/esp_lcd/interface")
    safe_add_include("${IDF_PATH}/components/driver/include")
    safe_add_include("${IDF_PATH}/components/driver/gpio/include")
    safe_add_include("${IDF_PATH}/components/esp_driver_gpio/include") # v5.x specific
    safe_add_include("${IDF_PATH}/components/esp_hw_support/include")
    safe_add_include("${IDF_PATH}/components/esp_common/include")
    safe_add_include("${IDF_PATH}/components/heap/include")
    safe_add_include("${IDF_PATH}/components/log/include")
    safe_add_include("${IDF_PATH}/components/soc/include")
    safe_add_include("${IDF_PATH}/components/hal/include")
else()
    message(WARNING "IDF_PATH not defined in micropython.cmake! Falling back to target detection.")
    # Fallback to target detection
    foreach(comp esp_lcd driver esp_hw_support esp_common log heap)
        if(TARGET idf::${comp})
            target_link_libraries(usermod INTERFACE idf::${comp})
            get_target_property(inc_dirs idf::${comp} INTERFACE_INCLUDE_DIRECTORIES)
            if(inc_dirs)
                target_include_directories(usermod INTERFACE ${inc_dirs})
            endif()
        endif()
    endforeach()
endif()
