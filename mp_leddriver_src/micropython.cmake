# Create an INTERFACE library for our module
add_library(mp_leddriver_lib INTERFACE)

# Add source files
target_sources(mp_leddriver_lib INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mp_leddriver.c
    ${CMAKE_CURRENT_LIST_DIR}/bus_i8080.c
    ${CMAKE_CURRENT_LIST_DIR}/strip_encoder.c
)

# Add include directories
target_include_directories(mp_leddriver_lib INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Link to the main MicroPython target
target_link_libraries(usermod INTERFACE mp_leddriver_lib)

# ----------------------------------------------------------------
# FORCE INCLUDE ESP-IDF COMPONENT HEADERS
# ----------------------------------------------------------------
# Iterate over required components and force their include directories
# into the usermod target. This bypasses any linkage propagation issues.

foreach(comp esp_lcd driver esp_hw_support esp_common log heap)
    if(TARGET idf::${comp})
        # Link the component (standard way)
        target_link_libraries(usermod INTERFACE idf::${comp})
        
        # Force include directories (backup way)
        get_target_property(inc_dirs idf::${comp} INTERFACE_INCLUDE_DIRECTORIES)
        if(inc_dirs)
            target_include_directories(usermod INTERFACE ${inc_dirs})
        endif()
    endif()
endforeach()
