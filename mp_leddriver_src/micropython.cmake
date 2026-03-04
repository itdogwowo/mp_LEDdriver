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

# Add dependency on ESP-IDF components
# This ensures header paths for esp_lcd and driver are available
target_link_libraries(usermod INTERFACE idf::esp_lcd idf::driver)
