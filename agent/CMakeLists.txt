cmake_minimum_required(VERSION 3.20)

project(WebhookOpsAgent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(webhookops_agent_common
    src/common/channel.cpp
    src/common/config.cpp
    src/common/crypto.cpp
    src/common/log.cpp
    src/common/net.cpp
    src/common/protocol.cpp
)

target_include_directories(webhookops_agent_common PUBLIC src)
target_link_libraries(webhookops_agent_common PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

if(WIN32)
    target_link_libraries(webhookops_agent_common PUBLIC ws2_32)
endif()

add_executable(webhookops-tunnel
    src/tunnel/main.cpp
)
target_link_libraries(webhookops-tunnel PRIVATE webhookops_agent_common)

add_executable(webhookops-agent
    src/agent/main.cpp
)
target_link_libraries(webhookops-agent PRIVATE webhookops_agent_common)
