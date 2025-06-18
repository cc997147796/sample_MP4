cmake_minimum_required(VERSION 3.16)
project(sample_mp4 VERSION 1.0.0)

set(CMAKE_C_STANDARD 11)

set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SRC_FILES
    "include/*.h"
    "MP4v2/include/*.h"
    "MP4v2/src/*.c"
    "common/*.c"
    "common/include/*.h"
    "src/*.h"
    "src/main.c"
)

#add_subdirectory(common)

add_executable(${PROJECT_NAME} ${SRC_FILES})

# 设置预处理宏定义
add_definitions(-DSENSOR0_TYPE=SC4336P_MIPI_4M_30FPS_10BIT -DSC4336P_MIPI_4M_30FPS_10BIT_SELECT -DRTSP_ENABLE=0)

add_definitions(
-DAUDIO_MICIN_GAIN_OPEN=1 
-DOT_ACODEC_TYPE_INNER=1
-DAUDIO_OUTPUT_VOLUME_OPEN=1)
#-DOT_ACODEC_TYPE_INNER

# 调整为自己的海思SDK路径
set(HI_SDK_DIR ~/YouYan-HI3516CV610-V1.0.1.0-master/smp/a7_linux/source)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/MP4v2/include
    ${HI_SDK_DIR}/out/include
    ${HI_SDK_DIR}/mpp/cbb/isp/include
    ${HI_SDK_DIR}/mpp/cbb/isp/user/3a/include

    /home/cc/lib-cc/src
    /home/cc/YouYan-HI3516CV610-V1.0.1.0-master/open_source/optee/optee_os-3.20.0/bsp/mmz/drivers/src/include
    /usr/local/include
)

#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

# 添加库文件目录
target_link_directories(${PROJECT_NAME} PRIVATE 
    ${HI_SDK_DIR}/out/lib
   #/usr/lib/mp4v2/lib
   ${HI_SDK_DIR}/mpp/sample/3516cv610_app/thirdlibrary/mp4v2/lib) 

    

target_link_libraries(${PROJECT_NAME} PRIVATE
    securec
    ss_mpi
    ss_mpi_audio
    ss_mpi_sysbind
    ss_mpi_ae
    ss_mpi_awb
    ss_mpi_isp
    ss_mpi_sysmem
    ot_mpi_isp
    mp4v2
    
    voice_engine
    extend_stats
    drc
    upvqe
    ldci
    dnvqe
    ir_auto
    calcflicker
    acs
    dehaze
    bnr
     # 添加 OPUS 相关库
    opus
    # 添加其他可能需要的音频库
    mp3_dec
    #${HI_SDK_DIR}/out/lib/libmp3_enc.so
    mp3_enc
    mp3_lame
    #/home/cc/lib-cc/mp4v2/.libs/libmp4v2.a 
    stdc++
    # 系统库
    pthread
    m
    dl
    rt
    
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    target_compile_options(sample_mp4 PRIVATE -mcpu=cortex-a7 -mfloat-abi=softfp -mfpu=neon-vfpv4)
endif()

install(TARGETS sample_mp4 DESTINATION bin)