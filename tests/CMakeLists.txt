# Native test + benchmark targets. Build only in the non-Vita configuration.

add_executable(vitaiptv_test test_m3u.c)
target_link_libraries(vitaiptv_test PRIVATE vicore)
if(NOT MSVC)
    target_compile_options(vitaiptv_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME vitaiptv_test COMMAND vitaiptv_test)
set_tests_properties(vitaiptv_test PROPERTIES
    ENVIRONMENT "VITAIPTV_TEST_DATA=${CMAKE_SOURCE_DIR}/tests/data")

add_executable(bench_m3u bench_m3u.c)
target_link_libraries(bench_m3u PRIVATE vicore)
