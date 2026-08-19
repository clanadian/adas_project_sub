# Arty Z7-20 (Cortex-A9) 크로스컴파일용 CMake toolchain file.
#
# ps_classifier_server, ps_eb_golden_test 처럼 rootfs 레시피에 아직 편입되지
# 않은 실행파일을 PC에서 만들어 보드로 scp할 때 쓴다. 자세한 절차는
# ../../docs/ARTY_SD_BOOT_USAGE.md 참고.
#
# 사전 조건: arty/classifier_linux_db 를 한 번 이상 petalinux-build 했어야
# 한다 (SD 부팅 이미지를 만들 때 이미 했다면 충분하다). DB 빌드 트리가 만든
# 컴파일러·sysroot를 그대로 쓴다 — DB/EB 둘 다 같은 Cortex-A9 하드플로트
# ABI라 EB용 바이너리를 만들 때도 이걸 쓴다. (petalinux-build를 새로 실행할
# 필요는 없다. 이미 있는 결과물만 가리킨다.)
#
# 이 SDK가 내부적으로 만들어 두는 toolchain.cmake(OEToolchainConfig 류)를
# 직접 -DCMAKE_TOOLCHAIN_FILE로 쓰면 안 된다 — 그 파일은 컴파일러를 plain
# set()으로 박아 두어서 CMakeCache보다 우선하며, 명령줄 -D로 넘긴 값을
# 조용히 덮어써 버린다(오늘 실제로 걸린 문제). 그래서 필요한 값만 CACHE
# FILEPATH로 새로 선언하는 이 파일을 대신 쓴다.
#
# 이 파일을 가리킬 때는 상대경로 대신 절대경로(또는 "$(pwd)/...")를 쓴다 —
# CMake는 CMAKE_TOOLCHAIN_FILE 상대경로를 호출한 디렉터리가 아니라 빌드
# 디렉터리 기준으로 찾아서, -B로 새 디렉터리를 만드는 첫 configure에서
# "toolchain file을 못 찾는다"로 실패한다(오늘 실제로 걸린 문제).

get_filename_component(ADAS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ADAS_DB_BUILD "${ADAS_REPO_ROOT}/arty/classifier_linux_db/build")
set(ADAS_DB_RECIPE
    "${ADAS_DB_BUILD}/tmp/work/cortexa9t2hf-neon-xilinx-linux-gnueabi/ps-db-golden-test/1.0")
# gcc-cross-arm(sysroots-components 아래 공용 사본)이 아니라 반드시 이
# 레시피 전용 recipe-sysroot-native 안의 컴파일러를 쓴다 — 공용 사본은
# 이 sysroot의 binutils(as)와 -march 기본값이 안 맞아 어셈블 단계에서
# "invalid -march= option"으로 실패한다(오늘 실제로 걸린 문제).
set(ADAS_DB_NATIVE_BIN
    "${ADAS_DB_RECIPE}/recipe-sysroot-native/usr/bin/arm-xilinx-linux-gnueabi")
set(ADAS_DB_SYSROOT "${ADAS_DB_RECIPE}/recipe-sysroot")

if(NOT EXISTS "${ADAS_DB_NATIVE_BIN}/arm-xilinx-linux-gnueabi-gcc")
    message(FATAL_ERROR
        "크로스컴파일러가 없다: ${ADAS_DB_NATIVE_BIN}\n"
        "arty/classifier_linux_db 에서 petalinux-build를 한 번 이상 실행했는지 확인한다"
        "(SD 부팅 이미지를 만들 때 이미 했다면 존재해야 한다).")
endif()
if(NOT EXISTS "${ADAS_DB_SYSROOT}")
    message(FATAL_ERROR "sysroot가 없다: ${ADAS_DB_SYSROOT}")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   "${ADAS_DB_NATIVE_BIN}/arm-xilinx-linux-gnueabi-gcc"       CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${ADAS_DB_NATIVE_BIN}/arm-xilinx-linux-gnueabi-g++"       CACHE FILEPATH "")
set(CMAKE_AR            "${ADAS_DB_NATIVE_BIN}/arm-xilinx-linux-gnueabi-gcc-ar"   CACHE FILEPATH "")
set(CMAKE_RANLIB        "${ADAS_DB_NATIVE_BIN}/arm-xilinx-linux-gnueabi-gcc-ranlib" CACHE FILEPATH "")

set(CMAKE_SYSROOT "${ADAS_DB_SYSROOT}")
set(CMAKE_C_FLAGS_INIT   "-mthumb -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a9 --sysroot=${ADAS_DB_SYSROOT}")
set(CMAKE_CXX_FLAGS_INIT "-mthumb -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a9 --sysroot=${ADAS_DB_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH "${ADAS_DB_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
