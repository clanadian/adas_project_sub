cmake_minimum_required(VERSION 3.16)
project(ps_common CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# APU와 RPU가 함께 쓰는 계층.
#
# APU는 detection을 공유 메모리에 쓰고, RPU는 그것을 읽어 판단해 UART로
# 내보낸다. 양쪽이 같은 선언을 봐야 하므로 어느 한쪽에 둘 수 없다.
#
# 제약이 하나 있다. **이 디렉터리는 R5 베어메탈에서도 컴파일된다.**
# 그래서 힙 할당, 파일 I/O, std 컨테이너, 예외를 쓰지 않는다.
# OpenCV나 Linux 전용 헤더도 들어오면 안 된다.
# 새 파일을 넣을 때 이 조건을 지키는지 먼저 확인한다.
add_library(ps_common STATIC
    src/SafetyMessage.cpp
    src/SafetyJudge.cpp
    src/SafetyDebounce.cpp
    src/SafetyHazardLatch.cpp
    src/UartFrame.cpp
)

target_include_directories(ps_common PUBLIC include)
