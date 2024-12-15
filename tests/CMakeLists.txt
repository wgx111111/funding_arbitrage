# 测试配置
enable_testing()

# 查找 GTest
find_package(GTest REQUIRED)
find_package(GMock REQUIRED)

# 添加测试目录
add_subdirectory(unit)
add_subdirectory(integration)

# 单元测试可执行文件
set(UNIT_TEST_SOURCES
    unit/api/test_binance_api.cpp
    unit/api/test_websocket_client.cpp
    unit/core/test_arbitrage_engine.cpp
    unit/risk/test_risk_manager.cpp
    unit/utils/test_logger.cpp
)

# 集成测试可执行文件
set(INTEGRATION_TEST_SOURCES
    integration/test_market_data.cpp
    integration/test_trading.cpp
)

# 单元测试
add_executable(unit_tests ${UNIT_TEST_SOURCES})
target_link_libraries(unit_tests
    PRIVATE
        ${PROJECT_NAME}_lib
        GTest::GTest
        GTest::Main
        GMock::GMock
)

# 集成测试
add_executable(integration_tests ${INTEGRATION_TEST_SOURCES})
target_link_libraries(integration_tests
    PRIVATE
        ${PROJECT_NAME}_lib
        GTest::GTest
        GTest::Main
)

# 添加测试
add_test(NAME unit_tests COMMAND unit_tests)
add_test(NAME integration_tests COMMAND integration_tests)

# 代码覆盖率配置
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    include(CodeCoverage)
    append_coverage_compiler_flags()
    
    setup_target_for_coverage_gcovr_html(
        NAME coverage
        EXECUTABLE unit_tests
        DEPENDENCIES unit_tests
        BASE_DIRECTORY "${PROJECT_SOURCE_DIR}/src"
        EXCLUDE "tests/*" "build/*"
    )
endif()