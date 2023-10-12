cmake_minimum_required(VERSION 3.0)
project(libdto)

# Build the shared library
add_library(dto SHARED dto.c)

# set gnu source everywhere
add_compile_definitions(_GNU_SOURCE)

# Add the -DDTO_STATS_SUPPORT preprocessor definition
target_compile_definitions(dto PRIVATE DTO_STATS_SUPPORT)

# Link libraries
target_link_libraries(dto accel-config dl)

# Install targets
install(
    TARGETS dto
    LIBRARY DESTINATION lib
)

# Build dto-test and dto-test-wodto
add_executable(dto-test dto-test.c)
target_link_libraries(dto-test PRIVATE dto pthread)

add_executable(dto-test-wodto dto-test.c)
target_link_libraries(dto-test-wodto PRIVATE pthread)

