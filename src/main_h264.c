#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>

#include "sample_comm.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_vpss.h"
#include "ss_mpi_venc.h"
#include "ot_common_video.h"
#define CHN_NUM_MAX 2

typedef struct {
    ot_size max_size;
    ot_pixel_format pixel_format;
    ot_size in_size;
    ot_size output_size[OT_VPSS_MAX_PHYS_CHN_NUM];
    ot_compress_mode compress_mode[OT_VPSS_MAX_PHYS_CHN_NUM];
    td_bool enable[OT_VPSS_MAX_PHYS_CHN_NUM];
    ot_fmu_mode fmu_mode[OT_VPSS_MAX_PHYS_CHN_NUM];
} sample_venc_vpss_chn_attr;

typedef struct {
    td_u32 valid_num;
    td_u64 blk_size[OT_VB_MAX_COMMON_POOLS];
    td_u32 blk_cnt[OT_VB_MAX_COMMON_POOLS];
    td_u32 supplement_config;
} sample_venc_vb_attr;

typedef struct {
    ot_vpss_chn vpss_chn[CHN_NUM_MAX];
    ot_venc_chn venc_chn[CHN_NUM_MAX];
} sample_venc_vpss_chn;

typedef struct {
    td_s32 venc_chn_num;
    ot_size enc_size[CHN_NUM_MAX];
    td_s32 vpss_chn_depth;
} sample_venc_param;

// 录制控制
static volatile int g_recording = 0;
static volatile int g_exit_flag = 0;

// 录制参数结构
typedef struct {
    ot_venc_chn venc_chn;
    char filename[256];
    int duration_sec;
    int bitrate;
    int framerate;
} record_params_t;

// 信号处理函数
static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        printf("\n接收到退出信号 %d，正在停止录制...\n", signo);
        g_recording = 0;
        g_exit_flag = 1;
    }
}

// 获取当前时间戳（毫秒）
static uint64_t get_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 创建输出文件名
static void create_filename(char *filename, size_t size, const char *extension)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    snprintf(filename, size, "video_%04d%02d%02d_%02d%02d%02d.%s",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, extension);
}

// 修正H264编码录制线程
static void* h264_record_thread(void *arg)
{
    record_params_t *params = (record_params_t*)arg;
    FILE *fp = NULL;
    ot_venc_stream stream;
    uint64_t start_time, current_time;
    uint32_t frame_count = 0;
    uint32_t total_bytes = 0;
    int ret;
    fd_set read_fds;
    int venc_fd;
    struct timeval timeout;
    time_t last_print_time = 0;
    
    printf("开始录制H264视频到文件: %s\n", params->filename);
    printf("录制时长: %d 秒，码率: %d kbps，帧率: %d fps\n", 
           params->duration_sec, params->bitrate, params->framerate);
    
    // 打开输出文件
    fp = fopen(params->filename, "wb");
    if (fp == NULL) {
        printf("打开文件失败: %s\n", params->filename);
        return NULL;
    }
    
    // 获取VENC文件描述符
    venc_fd = ss_mpi_venc_get_fd(params->venc_chn);
    if (venc_fd < 0) {
        printf("获取VENC文件描述符失败\n");
        fclose(fp);
        return NULL;
    }
    
    start_time = get_timestamp_ms();
    last_print_time = time(NULL);
    
    printf("录制开始，目标时长: %d 秒\n", params->duration_sec);
    
    while (g_recording) {
        // 检查录制时长 - 移到循环开始，确保时间检查准确
        current_time = get_timestamp_ms();
        uint64_t elapsed_ms = current_time - start_time;
        
        if (elapsed_ms >= (uint64_t)params->duration_sec * 1000) {
            printf("录制时间到达 %d 秒，停止录制\n", params->duration_sec);
            break;
        }
        
        // 设置select超时 - 缩短超时时间，提高响应性
        FD_ZERO(&read_fds);
        FD_SET(venc_fd, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms超时
        
        ret = select(venc_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) {
                continue; // 被信号中断，继续
            }
            printf("select错误: %s\n", strerror(errno));
            break;
        } else if (ret == 0) {
            // 超时，继续循环检查时间
            continue;
        }
        
        if (FD_ISSET(venc_fd, &read_fds)) {
            // 查询编码状态
            ot_venc_chn_status stat;
            ret = ss_mpi_venc_query_status(params->venc_chn, &stat);
            if (ret != TD_SUCCESS) {
                printf("查询编码状态失败: 0x%x\n", ret);
                usleep(10000); // 10ms延时
                continue;
            }
            
            if (stat.cur_packs == 0) {
                usleep(1000); // 1ms延时
                continue;
            }
            
            // 分配内存
            stream.pack = (ot_venc_pack *)malloc(sizeof(ot_venc_pack) * stat.cur_packs);
            if (stream.pack == NULL) {
                printf("分配内存失败\n");
                continue;
            }
            
            // 获取编码流
            stream.pack_cnt = stat.cur_packs;
            ret = ss_mpi_venc_get_stream(params->venc_chn, &stream, TD_FALSE); // 非阻塞
            if (ret != TD_SUCCESS) {
                free(stream.pack);
                if (ret != OT_ERR_VENC_BUF_EMPTY) {
                    printf("获取编码流失败: 0x%x\n", ret);
                }
                continue;
            }
            
            // 保存编码数据
            for (uint32_t i = 0; i < stream.pack_cnt; i++) {
                ot_venc_pack *pack = &stream.pack[i];
                if (pack->len > 0 && pack->addr != 0) {
                    size_t written = fwrite((void*)(uintptr_t)pack->addr, 1, pack->len, fp);
                    if (written != pack->len) {
                        printf("写入文件失败: %zu/%u\n", written, pack->len);
                    } else {
                        total_bytes += pack->len;
                    }
                }
            }
            
            frame_count += stream.pack_cnt;
            
            // 释放编码流
            ss_mpi_venc_release_stream(params->venc_chn, &stream);
            free(stream.pack);
            
            // 每秒打印一次状态
            time_t now = time(NULL);
            if (now != last_print_time) {
                double elapsed_sec = elapsed_ms / 1000.0;
                double current_fps = frame_count / elapsed_sec;
                double current_bitrate = (total_bytes * 8.0) / (elapsed_sec * 1000.0); // kbps
                
                printf("录制中: %.1f/%.0f 秒, %u 帧, FPS: %.1f, 码率: %.1f kbps, 文件大小: %.1f KB\n", 
                       elapsed_sec, (double)params->duration_sec, frame_count, 
                       current_fps, current_bitrate, total_bytes / 1024.0);
                
                last_print_time = now;
            }
        }
    }
    
    if (fp) {
        fflush(fp);
        fclose(fp);
    }
    
    current_time = get_timestamp_ms();
    double actual_duration = (current_time - start_time) / 1000.0;
    double actual_fps = frame_count / actual_duration;
    double actual_bitrate = (total_bytes * 8.0) / (actual_duration * 1000.0);
    
    printf("H264录制完成:\n");
    printf("  实际时长: %.1f 秒\n", actual_duration);
    printf("  总帧数: %u 帧\n", frame_count);
    printf("  平均帧率: %.1f fps\n", actual_fps);
    printf("  平均码率: %.1f kbps\n", actual_bitrate);
    printf("  文件大小: %.1f KB\n", total_bytes / 1024.0);
    printf("  视频文件: %s\n", params->filename);
    
    return NULL;
}

// 修改初始化系统函数
static int init_system(void)
{
    int ret;
    sample_sns_type sns_type = SENSOR0_TYPE;
    sample_vi_cfg vi_cfg;
    ot_size vi_size;
    sample_venc_vpss_chn_attr vpss_param;
    sample_venc_vb_attr vb_attr = { 0 };
    ot_size enc_size[CHN_NUM_MAX];
    
    // 获取VI配置和尺寸
    sample_comm_vi_get_default_vi_cfg(sns_type, &vi_cfg);
    sample_comm_vi_get_size_by_sns_type(sns_type, &vi_size);
    
    // 初始化编码参数
    ret = sample_venc_init_param(enc_size, CHN_NUM_MAX, &vi_size, &vpss_param);
    if (ret != TD_SUCCESS) {
        printf("初始化VENC参数失败: 0x%x\n", ret);
        return ret;
    }
    
    // 获取VB属性
    get_vb_attr(&vi_size, &vpss_param, &vb_attr);
    
    // 使用sample_venc_sys_init初始化系统
    ret = sample_venc_sys_init(&vb_attr);
    if (ret != TD_SUCCESS) {
        printf("系统初始化失败: 0x%x\n", ret);
        return ret;
    }
    
    printf("系统初始化成功\n");
    return TD_SUCCESS;
}

// 去初始化系统函数也需要相应修改
static void deinit_system(void)
{
    sample_comm_sys_exit();
    printf("系统去初始化完成\n");
}

// 修改启动VI函数，使用更标准的方式
static int start_vi(sample_vi_cfg *vi_cfg)
{
    int ret;
    
    // 使用sample_venc_vi_init函数
    ret = sample_venc_vi_init(vi_cfg);
    if (ret != TD_SUCCESS) {
        printf("启动VI失败: 0x%x\n", ret);
        return ret;
    }
    
    printf("VI启动成功\n");
    return TD_SUCCESS;
}

// 修改停止VI函数
static void stop_vi(sample_vi_cfg *vi_cfg)
{
    sample_venc_vi_deinit(vi_cfg);
    printf("VI停止完成\n");
}

// 修改启动VPSS函数
static int start_vpss(sample_vpss_cfg *vpss_cfg, sample_venc_vpss_chn_attr *vpss_param)
{
    int ret;
    ot_vpss_grp vpss_grp = 0;
    
    // 使用sample_venc_vpss_init函数
    ret = sample_venc_vpss_init(vpss_grp, vpss_param);
    if (ret != TD_SUCCESS) {
        printf("启动VPSS失败: 0x%x\n", ret);
        return ret;
    }
    
    printf("VPSS启动成功\n");
    return TD_SUCCESS;
}

// 修改停止VPSS函数
static void stop_vpss(sample_venc_vpss_chn_attr *vpss_param)
{
    ot_vpss_grp vpss_grp = 0;
    sample_venc_vpss_deinit(vpss_grp, vpss_param);
    printf("VPSS停止完成\n");
}

// 修改绑定VI和VPSS
static int bind_vi_vpss(ot_vi_pipe vi_pipe, ot_vi_chn vi_chn, 
                       ot_vpss_grp vpss_grp, ot_vpss_chn vpss_chn)
{
    int ret;
    
    // 使用sample_comm_vi_bind_vpss
    ret = sample_comm_vi_bind_vpss(vi_pipe, vi_chn, vpss_grp, vpss_chn);
    if (ret != TD_SUCCESS) {
        printf("绑定VI-VPSS失败: 0x%x\n", ret);
        return ret;
    }
    
    printf("VI-VPSS绑定成功\n");
    return TD_SUCCESS;
}

// 修改解绑VI和VPSS
static void unbind_vi_vpss(ot_vi_pipe vi_pipe, ot_vi_chn vi_chn, 
                          ot_vpss_grp vpss_grp, ot_vpss_chn vpss_chn)
{
    sample_comm_vi_un_bind_vpss(vi_pipe, vi_chn, vpss_grp, vpss_chn);
    printf("VI-VPSS解绑完成\n");
}

// 修改启动VENC函数，使用sample_comm接口
static int start_venc(ot_venc_chn venc_chn, ot_size *enc_size, 
                     int framerate, int bitrate)
{
    int ret;
    ot_venc_gop_mode gop_mode;
    ot_venc_gop_attr gop_attr;
    sample_comm_venc_chn_param chn_param = {0};
    
    // 获取GOP模式
    gop_mode = OT_VENC_GOP_MODE_NORMAL_P;  // 使用默认模式
    ret = sample_comm_venc_get_gop_attr(gop_mode, &gop_attr);
    if (ret != TD_SUCCESS) {
        printf("获取GOP属性失败: 0x%x\n", ret);
        return ret;
    }
    
    // 配置编码参数
    chn_param.type = OT_PT_H264;
    chn_param.size = sample_comm_sys_get_pic_enum(enc_size);
    chn_param.rc_mode = SAMPLE_RC_CBR;
    chn_param.profile = 0;  // baseline
    chn_param.is_rcn_ref_share_buf = TD_FALSE;
    chn_param.gop_attr = gop_attr;
    chn_param.frame_rate = framerate;
    chn_param.gop = framerate * 2;  // 2秒一个I帧
    chn_param.stats_time = 1;
    
    // 启用小缓冲模式
    ret = sample_comm_venc_mini_buf_en(&chn_param, 1);
    if (ret != TD_SUCCESS) {
        printf("启用VENC小缓冲失败: 0x%x\n", ret);
        return ret;
    }
    
    // 启动VENC通道
    ret = sample_comm_venc_start(venc_chn, &chn_param);
    if (ret != TD_SUCCESS) {
        printf("启动VENC通道失败: 0x%x\n", ret);
        return ret;
    }
    
    printf("VENC通道 %d 启动成功 (%dx%d, %dfps, %dkbps)\n", 
           venc_chn, enc_size->width, enc_size->height, framerate, bitrate);
    return TD_SUCCESS;
}

// 修改停止VENC
static void stop_venc(ot_venc_chn venc_chn)
{
    sample_comm_venc_stop(venc_chn);
    printf("VENC通道 %d 停止完成\n", venc_chn);
}

// 修改绑定VPSS和VENC
static int bind_vpss_venc(ot_vpss_grp vpss_grp, ot_vpss_chn vpss_chn, 
                         ot_venc_chn venc_chn)
{
    int ret;
    
    // 使用sample_comm_vpss_bind_venc
    ret = sample_comm_vpss_bind_venc(vpss_grp, vpss_chn, venc_chn);
    if (ret != TD_SUCCESS) {
        printf("绑定VPSS-VENC失败: 0x%x\n", ret);
        return ret;
    }
    
    printf("VPSS-VENC绑定成功\n");
    return TD_SUCCESS;
}

// 修改解绑VPSS和VENC
static void unbind_vpss_venc(ot_vpss_grp vpss_grp, ot_vpss_chn vpss_chn, 
                            ot_venc_chn venc_chn)
{
    sample_comm_vpss_un_bind_venc(vpss_grp, vpss_chn, venc_chn);
    printf("VPSS-VENC解绑完成\n");
}

// 修改主函数
int main(int argc, char *argv[])
{
    int ret = 0;
    sample_sns_type sns_type = SENSOR0_TYPE;
    sample_vi_cfg vi_cfg;
    ot_size vi_size;
    sample_venc_vpss_chn_attr vpss_param;
    ot_size enc_size[CHN_NUM_MAX];
    record_params_t record_params;
    pthread_t record_thread;
    int record_duration = 30; // 默认录制30秒
    int bitrate = 2048; // 默认码率2Mbps
    int framerate = 25; // 默认帧率25fps
    ot_venc_chn venc_chn = 0;
    ot_vpss_grp vpss_grp = 0;
    ot_vi_pipe vi_pipe = 0;
    ot_vi_chn vi_chn = 0;
    ot_vpss_chn vpss_chn = 0;
    
    printf("==== 海思H264视频录制程序 ====\n");
    
    // 检查命令行参数
    if (argc > 1) {
        record_duration = atoi(argv[1]);
        if (record_duration <= 0) {
            record_duration = 30;
        }
    }
    if (argc > 2) {
        bitrate = atoi(argv[2]);
        if (bitrate <= 0) {
            bitrate = 2048;
        }
    }
    if (argc > 3) {
        framerate = atoi(argv[3]);
        if (framerate <= 0) {
            framerate = 25;
        }
    }
    
    // 注册信号处理函数
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 获取VI配置和尺寸
    sample_comm_vi_get_default_vi_cfg(sns_type, &vi_cfg);
    sample_comm_vi_get_size_by_sns_type(sns_type, &vi_size);
    
    // 初始化编码参数
    ret = sample_venc_init_param(enc_size, CHN_NUM_MAX, &vi_size, &vpss_param);
    if (ret != TD_SUCCESS) {
        printf("初始化VENC参数失败: 0x%x\n", ret);
        goto EXIT;
    }
    
    // 初始化系统
    ret = init_system();
    if (ret != TD_SUCCESS) {
        goto EXIT;
    }
    
    // 启动VI
    ret = start_vi(&vi_cfg);
    if (ret != TD_SUCCESS) {
        goto EXIT_SYS;
    }
    
    // 启动VPSS
    ret = start_vpss(NULL, &vpss_param);
    if (ret != TD_SUCCESS) {
        goto EXIT_VI;
    }
    
    // 启动VENC
    ret = start_venc(venc_chn, &enc_size[0], framerate, bitrate);
    if (ret != TD_SUCCESS) {
        goto EXIT_VPSS;
    }
    
    // 绑定VI和VPSS
    ret = bind_vi_vpss(vi_pipe, vi_chn, vpss_grp, vpss_chn);
    if (ret != TD_SUCCESS) {
        goto EXIT_VENC;
    }
    
    // 绑定VPSS和VENC
    ret = bind_vpss_venc(vpss_grp, vpss_chn, venc_chn);
    if (ret != TD_SUCCESS) {
        goto EXIT_BIND_VI_VPSS;
    }
    
    // 等待系统稳定
    sleep(2);
    
    // 配置录制参数
    record_params.venc_chn = venc_chn;
    record_params.duration_sec = record_duration;
    record_params.bitrate = bitrate;
    record_params.framerate = framerate;
    create_filename(record_params.filename, sizeof(record_params.filename), "h264");
    
    // 开始录制
    g_recording = 1;
    ret = pthread_create(&record_thread, NULL, h264_record_thread, &record_params);
    if (ret != 0) {
        printf("创建录制线程失败: %s\n", strerror(ret));
        goto EXIT_BIND_VPSS_VENC;
    }
    
    printf("H264录制开始，目标时长: %d 秒，按Ctrl+C提前停止录制\n", record_duration);
    
    // 主线程监控录制状态
    time_t start_monitor = time(NULL);
    while (g_recording && !g_exit_flag) {
        sleep(1);
        time_t now = time(NULL);
        int elapsed = now - start_monitor;
        
        if (elapsed >= record_duration) {
            printf("主线程检测到录制时间到达 %d 秒，停止录制\n", record_duration);
            g_recording = 0;
            break;
        }
        
        // 每5秒打印一次主线程状态
        if (elapsed % 5 == 0 && elapsed > 0) {
            printf("主线程监控: 已运行 %d/%d 秒\n", elapsed, record_duration);
        }
    }
    
    // 等待录制完成或用户中断
    pthread_join(record_thread, NULL);
    
    printf("H264录制停止\n");
    
EXIT_BIND_VPSS_VENC:
    unbind_vpss_venc(vpss_grp, vpss_chn, venc_chn);
    
EXIT_BIND_VI_VPSS:
    unbind_vi_vpss(vi_pipe, vi_chn, vpss_grp, vpss_chn);
    
EXIT_VENC:
    stop_venc(venc_chn);
    
EXIT_VPSS:
    stop_vpss(&vpss_param);
    
EXIT_VI:
    stop_vi(&vi_cfg);
    
EXIT_SYS:
    deinit_system();
    
EXIT:
    printf("程序退出，返回值: %d\n", ret);
    return ret;
}



