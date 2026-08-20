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

add_library(adas_ps_dummy STATIC src/app/dummy_roi_service.c)
target_link_libraries(adas_ps_dummy PUBLIC adas_ps_core)
target_compile_options(adas_ps_dummy PRIVATE -Wall -Wextra -Wpedantic)

add_executable(ps_dummy_server tools/ps_dummy_server.c)
target_link_libraries(ps_dummy_server PRIVATE adas_ps_dummy)
target_compile_options(ps_dummy_server PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# 제어 계층 (안전 판단 + 터틀봇 UART 직접 송신)
# ---------------------------------------------------------------------------
# ps_classifier_server.c(C)에서 쓰기 위한 C++ 계층. common/은 순수 판단
# 로직(SafetyJudge/HazardLatch, OpenCV 등 외부 의존 없음)만 직접 쓰고,
# jetson/include/control/의 SafetyTransmitter·UartPort는 Jetson과 동일한
# POSIX termios 구현이라 그대로 재사용한다(DetectionAdapter/SafetyDecider는
# RoiObservation을 통해 OpenCV까지 딸려오는 Jetson 전용 접착 타입이라 안
# 쓴다 - ps_safety_bridge.cpp 참고).
add_library(adas_ps_safety STATIC
    src/control/ps_safety_bridge.cpp
    ../../jetson/src/control/SafetyTransmitter.cpp
    ../../jetson/src/control/UartPort.cpp
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

add_executable(test_dummy_roi_service tests/test_dummy_roi_service.c)
target_link_libraries(test_dummy_roi_service PRIVATE adas_ps_dummy)
target_compile_options(test_dummy_roi_service PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME test_dummy_roi_service COMMAND test_dummy_roi_service)
