cmake_minimum_required(VERSION 3.10)
project(adas_jetson LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(Threads REQUIRED)

find_path(TENSORRT_INCLUDE_DIR NvInfer.h PATH_SUFFIXES aarch64-linux-gnu)
find_library(TENSORRT_LIBRARY nvinfer)
find_library(CUDART_LIBRARY cudart PATHS /usr/local/cuda/lib64)

add_library(adas_jetson_pipeline STATIC
    src/capture/V4L2Capture.cpp
    src/capture/FrameSource.cpp
    src/roi/RoiCropper.cpp
    src/roi/RoiProposer.cpp
    src/roi/TensorRtProposalEngine.cpp
    src/preprocess/RoiPreprocessor.cpp
    src/network/TcpRoiClient.cpp
    src/stream/MjpegStreamServer.cpp
)

target_include_directories(adas_jetson_pipeline
    PUBLIC
        include
        ../shared/include
)

target_link_libraries(adas_jetson_pipeline
    PUBLIC
        ${OpenCV_LIBS}
        Threads::Threads
)

target_compile_options(adas_jetson_pipeline
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(TENSORRT_INCLUDE_DIR AND TENSORRT_LIBRARY AND CUDART_LIBRARY)
    target_include_directories(adas_jetson_pipeline PRIVATE
        ${TENSORRT_INCLUDE_DIR}
        /usr/local/cuda/include
    )
    target_link_libraries(adas_jetson_pipeline PUBLIC
        ${TENSORRT_LIBRARY}
        ${CUDART_LIBRARY}
    )
    target_compile_definitions(adas_jetson_pipeline PRIVATE ADAS_HAS_TENSORRT=1)

    add_executable(proposal_golden_check tools/proposal_golden_check.cpp)
    target_link_libraries(proposal_golden_check PRIVATE adas_jetson_pipeline)
    target_compile_options(proposal_golden_check PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(camera_roi_check tools/camera_roi_check.cpp)
    target_link_libraries(camera_roi_check PRIVATE adas_jetson_pipeline)
    target_compile_options(camera_roi_check PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 제어 계층
# ---------------------------------------------------------------------------
# common/ 은 KR260 저장소와 공유하는 안전 판단 계층이다. 힙·STL 컨테이너·
# 예외를 쓰지 않는 제한된 C++17 이라 그대로 링크된다. 여기서 쓰는 것만
# 골라 넣는다 - SafetyMessage(코어 간 공유 메모리)는 이 구조에 필요 없다.
add_library(adas_safety STATIC
    ../common/src/SafetyJudge.cpp
    ../common/src/SafetyHazardLatch.cpp
    ../common/src/UartFrame.cpp
)

target_include_directories(adas_safety PUBLIC ../common/include)
target_compile_options(adas_safety PRIVATE -Wall -Wextra -Wpedantic)

add_library(adas_jetson_control STATIC
    src/control/DetectionAdapter.cpp
    src/control/SafetyDecider.cpp
    src/control/SafetyTransmitter.cpp
    src/control/UartPort.cpp
)

target_link_libraries(adas_jetson_control
    PUBLIC
        adas_safety
        adas_jetson_pipeline
        Threads::Threads
)

target_compile_options(adas_jetson_control PRIVATE -Wall -Wextra -Wpedantic)

add_executable(jetson_roi_client tools/jetson_roi_client.cpp)
target_link_libraries(jetson_roi_client PRIVATE adas_jetson_control)
target_compile_options(jetson_roi_client PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

add_executable(test_roi_preprocessor
    tests/test_roi_preprocessor.cpp
)

target_link_libraries(test_roi_preprocessor
    PRIVATE
        adas_jetson_pipeline
)

target_compile_options(test_roi_preprocessor
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(
    NAME roi_preprocessor
    COMMAND test_roi_preprocessor
)

add_executable(test_detection_adapter tests/test_detection_adapter.cpp)
target_link_libraries(test_detection_adapter PRIVATE adas_jetson_control)
target_compile_options(test_detection_adapter PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME detection_adapter COMMAND test_detection_adapter)

add_executable(test_safety_decider tests/test_safety_decider.cpp)
target_link_libraries(test_safety_decider PRIVATE adas_jetson_control)
target_compile_options(test_safety_decider PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME safety_decider COMMAND test_safety_decider)

add_executable(test_safety_transmitter tests/test_safety_transmitter.cpp)
target_link_libraries(test_safety_transmitter PRIVATE adas_jetson_control)
target_compile_options(test_safety_transmitter PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME safety_transmitter COMMAND test_safety_transmitter)

add_executable(test_latency_stats
    tests/test_latency_stats.cpp
)

# 헤더 전용이라 파이프라인 라이브러리(OpenCV/TensorRT)에 의존하지 않는다.
target_include_directories(test_latency_stats
    PRIVATE
        include
)

target_compile_options(test_latency_stats
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(
    NAME latency_stats
    COMMAND test_latency_stats
)

add_executable(test_roi_cropper
    tests/test_roi_cropper.cpp
)

target_link_libraries(test_roi_cropper
    PRIVATE
        adas_jetson_pipeline
)

target_compile_options(test_roi_cropper
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_test(
    NAME roi_cropper
    COMMAND test_roi_cropper
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../arty/ps_db/src/network/tcp_roi_server.c")
    add_executable(test_tcp_roi_client
        tests/test_tcp_roi_client.cpp
        ../arty/ps_db/src/network/tcp_roi_server.c
    )

    target_include_directories(test_tcp_roi_client PRIVATE ../arty/ps_db/include)
    target_link_libraries(test_tcp_roi_client PRIVATE adas_jetson_pipeline Threads::Threads)
    target_compile_options(test_tcp_roi_client PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME tcp_roi_client COMMAND test_tcp_roi_client)
endif()
