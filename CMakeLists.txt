cmake_minimum_required(VERSION 3.14)
project(funding_arbitrage VERSION 1.0)

# C++标准设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 查找依赖包
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(prometheus-cpp REQUIRED)

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CURL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)

# 源文件
set(SOURCES
    src/api/binance_api.cpp
    src/api/websocket_client.cpp
    src/config/config.cpp
    src/core/arbitrage_engine.cpp
    src/risk/risk_manager.cpp
    src/monitor/monitor_service.cpp
    src/utils/logger.cpp
)

# 可执行文件
add_executable(${PROJECT_NAME} 
    src/main.cpp
    ${SOURCES}
)

# 链接库
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${CURL_LIBRARIES}
        ${OPENSSL_LIBRARIES}
        Threads::Threads
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Boost::system
        prometheus-cpp::core
        prometheus-cpp::pull
)

# 安装配置
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

install(FILES 
    ${CMAKE_SOURCE_DIR}/config/config.json
    DESTINATION etc/${PROJECT_NAME}
)

# 测试配置
enable_testing()
add_subdirectory(tests)

# Debug配置
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${PROJECT_NAME}
        PRIVATE
            DEBUG
    )
endif()

# 版本信息
configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.h.in
    ${CMAKE_BINARY_DIR}/version.h
)