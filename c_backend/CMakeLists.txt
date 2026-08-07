cmake_minimum_required(VERSION 3.10)
project(NovaCompiler C)

set(CMAKE_C_STANDARD 99)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(COMMON_SOURCES
    lexer.c
    parser.c
    semantic.c
    tac.c
    optimizer.c
    bytecode.c
    vm.c
    json.c
)

add_executable(nova_compiler main.c ${COMMON_SOURCES})

add_executable(nova_server server.c ${COMMON_SOURCES})
if(WIN32)
    target_link_libraries(nova_server ws2_32)
endif()
