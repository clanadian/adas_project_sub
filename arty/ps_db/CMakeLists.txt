cmake_minimum_required(VERSION 3.16)
project(adas_arty_ps LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(adas_ps_core STATIC
    src/network/tcp_roi_server.c
    src/preprocess/roi_preprocessor.c
    src/accelerator/pl_mmio.c
    src/accelerator/classifier_buffers.c
    src/accelerator/classifier_accelerator.c
    src/postprocess/classifier_postprocess.c
    src/model/classifier_model.c
    src/driver/classifier_device.c
)

target_include_directories(adas_ps_core
    PUBLIC
        include
        ../../shared/include
)

target_compile_options(adas_ps_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(adas_ps_core PUBLIC m)

# ---------------------------------------------------------------------------
# 제어 계층 (안전 판단 + 터틀봇 UART 직접 송신)
# ---------------------------------------------------------------------------
# ps_classifier_server.c(C)에서 쓰기 위한 C++ 계층. common/은 순수 판단
# 로직(SafetyJudge/HazardLatch, OpenCV 등 외부 의존 없음)만 직접 쓰고,
# UART 송신 구현도 최종 소유 주체인 Arty PS 아래에 둔다.
add_library(adas_ps_safety STATIC
    src/control/ps_safety_bridge.cpp
    src/control/SafetyTransmitter.cpp
    src/control/UartPort.cpp
    ../../common/src/SafetyJudge.cpp
    ../../common/src/SafetyHazardLatch.cpp
    ../../common/src/UartFrame.cpp
)

target_include_directories(adas_ps_safety
    PUBLIC
        include
        ../../shared/include
        ../../jetson/include
        ../../common/include
)

target_compile_options(adas_ps_safety PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(adas_ps_safety PUBLIC Threads::Threads)

add_executable(ps_classifier_server tools/ps_classifier_server.c)
target_link_libraries(ps_classifier_server PRIVATE adas_ps_core adas_ps_safety)
target_compile_options(ps_classifier_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(ps_db_golden_test tools/ps_db_golden_test.c)
target_link_libraries(ps_db_golden_test PRIVATE adas_ps_core)
target_compile_options(ps_db_golden_test PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

function(add_adas_ps_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE adas_ps_core)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_adas_ps_test(test_roi_protocol tests/test_roi_protocol.c)
add_adas_ps_test(test_tcp_roi_server tests/test_tcp_roi_server.c)
add_adas_ps_test(test_roi_preprocessor tests/test_roi_preprocessor.c)
add_adas_ps_test(test_pl_mmio tests/test_pl_mmio.c)
add_adas_ps_test(test_classifier_buffers tests/test_classifier_buffers.c)
add_adas_ps_test(test_classifier_accelerator tests/test_classifier_accelerator.c)
add_adas_ps_test(test_classifier_postprocess tests/test_classifier_postprocess.c)
add_adas_ps_test(test_classifier_model tests/test_classifier_model.c)

# 제어 계층 시험. 판단 로직과 송신 주기·watchdog 은 하드웨어 없이 돈다 -
# UartPort/SafetyClock 이 그러라고 인터페이스로 분리돼 있다.
function(add_adas_ps_safety_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE adas_ps_safety)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_adas_ps_safety_test(test_safety_judge tests/test_safety_judge.cpp)
add_adas_ps_safety_test(test_safety_transmitter tests/test_safety_transmitter.cpp)
