# C1 manual driver: splat_model status|accept|fetch|import.
#
# The libcurl fetcher lives in platform/desktop/ (the PS5 gets its own
# implementation later). Its target is defined here, not in platform/desktop,
# so this CLI builds on a CPU-only desktop host as well.
find_package(CURL REQUIRED)

add_library(splat_http_curl STATIC
    "${PROJECT_SOURCE_DIR}/platform/desktop/curl_http_fetcher.cpp"
)
target_include_directories(splat_http_curl PUBLIC
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/platform/desktop"
)
target_link_libraries(splat_http_curl PUBLIC splat_core CURL::libcurl)

add_executable(splat_model main.cpp)
target_link_libraries(splat_model PRIVATE splat_http_curl)

# The real-network check needs libcurl, which only this directory links. It is
# a separate doctest binary; tests/model_manager_net_test.cpp compiles to an
# empty translation unit in splat_tests (the macro is set only here), so the
# normal suite keeps its CPU-only link set.
if(SPLAT_BUILD_TESTS)
    add_executable(splat_model_net_tests
        "${PROJECT_SOURCE_DIR}/tests/model_manager_net_test.cpp"
        "${PROJECT_SOURCE_DIR}/tests/test_main.cpp"
    )
    target_compile_definitions(splat_model_net_tests PRIVATE SPLAT_MODEL_NET_TESTS=1)
    target_include_directories(splat_model_net_tests PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/tests"
        "${PROJECT_SOURCE_DIR}/third_party"
    )
    target_link_libraries(splat_model_net_tests PRIVATE splat_http_curl)

    enable_testing()
    add_test(NAME model_manager_net COMMAND splat_model_net_tests)
    # Skipped (and passing) unless SPLAT_NET_TESTS=1 is in the environment.
    set_tests_properties(model_manager_net PROPERTIES LABELS "network")
endif()
