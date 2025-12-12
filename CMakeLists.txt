cmake_minimum_required(VERSION 3.22.1)
project(queue)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


include_directories(lib)
add_compile_options(-mwaitpkg)

add_executable(main src/main.cpp)
add_executable(client src/client.cpp)





if (CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(
            -g   # 启动调试符号
            -Ofast
            -fno-stack-protector
            -falign-functions=64
            -falign-loops=64
            -falign-labels=64
            -falign-jumps=64
            -funroll-loops
            -flto  # 启动lto
            -mfma
            -ffp-contract=fast
            -fno-trapping-math
            -fomit-frame-pointer
            -mtune=sapphirerapids
            -march=sapphirerapids
            -mavx2
            -mfma
            -mavx512f
            -mavx512dq
            -mavx512cd
            -mavx512bw
            -mavx512vl
            -mavx512vnni
            -mavx512vbmi
            -mavx512vbmi2
            -mavx512bitalg
            -mavx512vpopcntdq
            -mavx512bf16
            -mvaes
            -mvpclmulqdq
            -mamx-tile
            -mamx-int8
            -mamx-bf16
    )
endif()

