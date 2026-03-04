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

# Add dependency on ESP-IDF components
# This ensures header paths for esp_lcd, driver, and heap_caps are available
# We link them to OUR library, which then propagates to usermod
target_link_libraries(mp_leddriver_lib INTERFACE 
    idf::esp_lcd 
    idf::driver
    idf::esp_hw_support 
    idf::esp_common
)

# Link to the main MicroPython target
target_link_libraries(usermod INTERFACE mp_leddriver_lib)
