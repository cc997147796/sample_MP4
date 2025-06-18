#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <time.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sample_comm.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_vpss.h"
#include "ss_mpi_venc.h"
#include "ot_common_video.h"
#include "ot_common_aio.h"
#include "ss_audio_aac_adp.h"
#include "mp4v2/mp4v2.h"

#define CHN_NUM_MAX 2
#define OUTPUT_PATH "/mnt/MP4/"
#define MAX_FILENAME_LEN 256
#define MIN_FREE_SPACE_MB 100     // 最小保留空间100MB
#define MAX_FILE_SIZE_MB 500      // 单个文件最大500MB
#define MAX_RECORDING_TIME_SEC 600 // 最大录制时间10分钟

#ifdef OT_ACODEC_TYPE_INNER
#define SAMPLE_AUDIO_INNER_AI_DEV 0
#define SAMPLE_AUDIO_INNER_AO_DEV 0
#else
#define SAMPLE_AUDIO_EXTERN_AI_DEV 0
#define SAMPLE_AUDIO_EXTERN_AO_DEV 0
#endif

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

// 音频录制线程参数结构体
typedef struct {
    td_bool start;
    ot_ai_chn ai_chn;
    ot_aenc_chn ae_chn;
    MP4FileHandle mp4_file;
    MP4TrackId audio_track;
    pthread_t thread_id;
    td_u32 frame_count;
} audio_recorder_t;

// MP4录制相关结构体
typedef struct {
    MP4FileHandle mp4_file;
    MP4TrackId video_track;
    MP4TrackId audio_track;
    td_bool is_recording;
    td_u32 video_frame_count;
    td_u32 audio_frame_count;
    td_u32 fps;
    pthread_t video_thread;
    pthread_t audio_thread;
    ot_venc_chn venc_chn;
    ot_aenc_chn aenc_chn;
    char filename[MAX_FILENAME_LEN];
    time_t start_time;            // 录制开始时间
    td_u64 file_size;            // 当前文件大小
    audio_recorder_t audio_recorder;
} mp4_recorder_t;

static mp4_recorder_t g_recorder = {0};
static td_bool g_exit_flag = TD_FALSE;

static void signal_handler(int signo)
{
    printf("接收到信号: %d\n", signo);
    
    if (signo == SIGINT || signo == SIGTERM) {
        printf("接收到退出信号，正在清理资源...\n");
        g_exit_flag = TD_TRUE;
    } else if (signo == SIGSEGV) {
        printf("段错误发生！正在紧急清理...\n");
        // 紧急清理
        if (g_recorder.is_recording) {
            g_recorder.is_recording = TD_FALSE;
            if (g_recorder.mp4_file != MP4_INVALID_FILE_HANDLE) {
                MP4Close(g_recorder.mp4_file, 0);
            }
        }
        exit(1);
    } else if (signo == SIGABRT) {
        printf("程序异常终止！\n");
        exit(1);
    }
}

// 生成MP4文件名
static void generate_mp4_filename(char *filename, size_t size)
{
    time_t now;
    struct tm *tm_info;
    
    time(&now);
    tm_info = localtime(&now);
    
    snprintf(filename, size, "%svideo_%04d%02d%02d_%02d%02d%02d.mp4",
             OUTPUT_PATH,
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec);
}

// 查找H264 NALU起始码
static td_u8* find_nalu_start(td_u8* data, td_u32 len)
{
    td_u32 i;
    for (i = 0; i < len - 3; i++) {
        if (data[i] == 0x00 && data[i+1] == 0x00 && 
            data[i+2] == 0x00 && data[i+3] == 0x01) {
            return &data[i];
        }
        if (i < len - 2 && data[i] == 0x00 && 
            data[i+1] == 0x00 && data[i+2] == 0x01) {
            return &data[i];
        }
    }
    return NULL;
}

// 获取NALU类型
static td_u8 get_nalu_type(td_u8* nalu)
{
    // 跳过起始码
    if (nalu[0] == 0x00 && nalu[1] == 0x00 && 
        nalu[2] == 0x00 && nalu[3] == 0x01) {
        return nalu[4] & 0x1F;
    } else if (nalu[0] == 0x00 && nalu[1] == 0x00 && nalu[2] == 0x01) {
        return nalu[3] & 0x1F;
    }
    return 0;
}

// 前向声明
static td_bool ensure_disk_space(void);

// 创建MP4文件
static td_s32 create_mp4_file(mp4_recorder_t *recorder, ot_size *enc_size, td_u32 fps)
{
    // 检查磁盘空间
    if (!ensure_disk_space()) {
        return TD_FAILURE;
    }
    
    recorder->mp4_file = MP4Create(recorder->filename, 0);
    if (recorder->mp4_file == MP4_INVALID_FILE_HANDLE) {
        printf("创建MP4文件失败: %s, 错误: %s\n", recorder->filename, strerror(errno));
        return TD_FAILURE;
    }
    
    // 设置MP4文件时基
    MP4SetTimeScale(recorder->mp4_file, 90000);
    
    // 创建视频轨道
    recorder->video_track = MP4AddH264VideoTrack(
        recorder->mp4_file,
        90000,                    // timeScale
        90000 / fps,              // sampleDuration
        enc_size->width,          // width
        enc_size->height,         // height
        0x42,                     // AVCProfileIndication (Baseline)
        0x00,                     // profile_compat
        0x1E,                     // AVCLevelIndication
        3                         // lengthSizeMinusOne
    );
    
    if (recorder->video_track == MP4_INVALID_TRACK_ID) {
        printf("创建视频轨道失败\n");
        MP4Close(recorder->mp4_file, 0);
        return TD_FAILURE;
    }
    
    // 创建音频轨道（AAC）
    recorder->audio_track = MP4AddAudioTrack(
        recorder->mp4_file,
        48000,                   // timeScale (采样率)
        1024,                    // sampleDuration (每帧采样数)
        MP4_MPEG4_AUDIO_TYPE     // audioType
    );
    
    if (recorder->audio_track == MP4_INVALID_TRACK_ID) {
        printf("创建音频轨道失败\n");
        MP4Close(recorder->mp4_file, 0);
        return TD_FAILURE;
    }
    
    // 设置音频轨道参数
    MP4SetAudioProfileLevel(recorder->mp4_file, 0x29); // AAC LC Profile
    
    recorder->fps = fps;
    recorder->video_frame_count = 0;
    recorder->audio_frame_count = 0;
    recorder->file_size = 0;
    recorder->start_time = time(NULL);
    recorder->is_recording = TD_TRUE;
    
    printf("MP4文件创建成功: %s\n", recorder->filename);
    return TD_SUCCESS;
}

// 关闭MP4文件
static void close_mp4_file(mp4_recorder_t *recorder)
{
    if (recorder->mp4_file != MP4_INVALID_FILE_HANDLE) {
        MP4Close(recorder->mp4_file, 0);
        recorder->mp4_file = MP4_INVALID_FILE_HANDLE;
        recorder->is_recording = TD_FALSE;
        printf("MP4文件关闭: %s (视频帧数: %u, 音频帧数: %u)\n", 
               recorder->filename, recorder->video_frame_count, recorder->audio_frame_count);
    }
}

// 检查磁盘剩余空间
static td_u64 get_free_space_mb(const char *path)
{
    struct statvfs stat;
    
    if (statvfs(path, &stat) != 0) {
        printf("获取磁盘空间信息失败: %s\n", strerror(errno));
        return 0;
    }
    
    td_u64 free_bytes = (td_u64)stat.f_bavail * stat.f_frsize;
    return free_bytes / (1024 * 1024); // 转换为MB
}

// 删除最旧的MP4文件
static void delete_oldest_mp4_file(const char *dir_path)
{
    DIR *dir;
    struct dirent *entry;
    char oldest_file[MAX_FILENAME_LEN] = {0};
    time_t oldest_time = 0;
    char full_path[MAX_FILENAME_LEN];
    
    dir = opendir(dir_path);
    if (!dir) {
        printf("打开目录失败: %s\n", strerror(errno));
        return;
    }
    
    // 查找最旧的.mp4文件
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".mp4") != NULL) {
            snprintf(full_path, sizeof(full_path), "%s%s", dir_path, entry->d_name);
            
            struct stat st;
            if (stat(full_path, &st) == 0) {
                if (oldest_time == 0 || st.st_mtime < oldest_time) {
                    oldest_time = st.st_mtime;
                    strncpy(oldest_file, full_path, sizeof(oldest_file) - 1);
                    oldest_file[sizeof(oldest_file) - 1] = '\0';
                }
            }
        }
    }
    
    closedir(dir);
    
    // 删除最旧的文件
    if (strlen(oldest_file) > 0) {
        if (unlink(oldest_file) == 0) {
            printf("删除旧文件: %s\n", oldest_file);
        } else {
            printf("删除文件失败: %s\n", strerror(errno));
        }
    }
}

// 检查并清理磁盘空间
static td_bool ensure_disk_space(void)
{
    td_u64 free_space_mb;
    int retry_count = 0;
    const int max_retries = 5;
    
    while (retry_count < max_retries) {
        free_space_mb = get_free_space_mb(OUTPUT_PATH);
        
        if (free_space_mb >= MIN_FREE_SPACE_MB) {
            printf("可用空间: %lluMB\n", free_space_mb);
            return TD_TRUE;
        }
        
        printf("空间不足 (剩余: %lluMB, 需要: %dMB), 清理旧文件...\n", 
               free_space_mb, MIN_FREE_SPACE_MB);
        
        delete_oldest_mp4_file(OUTPUT_PATH);
        retry_count++;
        
        // 等待一下再检查
        usleep(100000); // 100ms
    }
    
    printf("清理后空间仍不足，无法继续录制\n");
    return TD_FALSE;
}

// 检查是否需要分割文件
static td_bool should_split_file(mp4_recorder_t *recorder)
{
    time_t current_time = time(NULL);
    td_u64 recording_duration = current_time - recorder->start_time;
    
    // 检查录制时间
    if (recording_duration >= MAX_RECORDING_TIME_SEC) {
        printf("达到最大录制时间，分割文件\n");
        return TD_TRUE;
    }
    
    // 检查文件大小（估算）
    if (recorder->file_size >= (MAX_FILE_SIZE_MB * 1024 * 1024)) {
        printf("达到最大文件大小，分割文件\n");
        return TD_TRUE;
    }
    
    return TD_FALSE;
}

// 写入H264数据到MP4
static td_s32 write_h264_to_mp4(mp4_recorder_t *recorder, td_u8 *data, td_u32 len, td_bool is_keyframe)
{
    if (!recorder->is_recording || recorder->mp4_file == MP4_INVALID_FILE_HANDLE) {
        return TD_FAILURE;
    }
    
    // 检查是否需要分割文件
    if (should_split_file(recorder)) {
        // 如果是关键帧，可以安全分割
        if (is_keyframe) {
            printf("在关键帧处分割文件\n");
            
            // 关闭当前文件
            close_mp4_file(recorder);
            
            // 检查磁盘空间
            if (!ensure_disk_space()) {
                return TD_FAILURE;
            }
            
            // 创建新文件
            ot_size enc_size = {1920, 1080};
            generate_mp4_filename(recorder->filename, sizeof(recorder->filename));
            
            if (create_mp4_file(recorder, &enc_size, recorder->fps) != TD_SUCCESS) {
                return TD_FAILURE;
            }
        }
    }
    
    // 转换为AVCC格式（长度+数据）
    td_u8 *avcc_data = malloc(len + 4);
    if (!avcc_data) {
        printf("内存分配失败\n");
        return TD_FAILURE;
    }
    
    // 跳过起始码，计算NALU长度
    td_u8 *nalu_start = data;
    td_u32 nalu_len = len;
    
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
        nalu_start = data + 4;
        nalu_len = len - 4;
    } else if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        nalu_start = data + 3;
        nalu_len = len - 3;
    }
    
    // 写入长度（大端序）
    avcc_data[0] = (nalu_len >> 24) & 0xFF;
    avcc_data[1] = (nalu_len >> 16) & 0xFF;
    avcc_data[2] = (nalu_len >> 8) & 0xFF;
    avcc_data[3] = nalu_len & 0xFF;
    
    // 复制NALU数据
    memcpy(avcc_data + 4, nalu_start, nalu_len);
    
    // 计算时间戳
    MP4Duration duration = (MP4Duration)(90000 / recorder->fps);
    
    // 写入MP4
    td_bool result = MP4WriteSample(
        recorder->mp4_file,
        recorder->video_track,
        avcc_data,
        nalu_len + 4,
        duration,
        0,                        // renderingOffset
        is_keyframe
    );
    
    free(avcc_data);
    
    if (result) {
        recorder->video_frame_count++;
        recorder->file_size += (nalu_len + 4);
        return TD_SUCCESS;
    } else {
        printf("写入MP4视频样本失败，错误码: %d\n", errno);
        return TD_FAILURE;
    }
}

// 写入AAC数据到MP4
static td_s32 write_aac_to_mp4(mp4_recorder_t *recorder, td_u8 *data, td_u32 len)
{
    if (!recorder->is_recording || recorder->mp4_file == MP4_INVALID_FILE_HANDLE) {
        return TD_FAILURE;
    }
    
    // AAC帧持续时间：1024样本 / 48000Hz = 约21.33ms
    MP4Duration duration = 1024;
    
    // 写入MP4
    td_bool result = MP4WriteSample(
        recorder->mp4_file,
        recorder->audio_track,
        data,
        len,
        duration,
        0,      // renderingOffset
        TD_FALSE // 音频不需要关键帧标识
    );
    
    if (result) {
        recorder->audio_frame_count++;
        recorder->file_size += len;
        return TD_SUCCESS;
    } else {
        printf("写入MP4音频样本失败，错误码: %d\n", errno);
        return TD_FAILURE;
    }
}

static FILE *g_aac_debug_file = NULL;
static td_bool g_save_aac_debug = TD_FALSE; // 设置为TD_TRUE开启AAC调试文件保存

// 打开AAC调试文件的函数
static FILE* open_aac_debug_file(ot_aenc_chn aenc_chn)
{
    char filename[256];
    time_t now;
    struct tm *tm_info;
    
    time(&now);
    tm_info = localtime(&now);
    
    snprintf(filename, sizeof(filename), "%saudio_debug_%04d%02d%02d_%02d%02d%02d_chn%d.aac",
             OUTPUT_PATH,
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec,
             aenc_chn);
    
    FILE *fp = fopen(filename, "wb");
    if (fp) {
        printf("创建AAC调试文件: %s\n", filename);
    } else {
        printf("创建AAC调试文件失败: %s, 错误: %s\n", filename, strerror(errno));
    }
    
    return fp;
}

// 音频编码线程
static void* audio_record_thread_proc(void *arg)
{
    mp4_recorder_t *recorder = (mp4_recorder_t*)arg;
    ot_audio_stream stream;  // 使用正确的音频流类型
    
    fd_set read_fds;
    td_s32 aenc_fd;
    td_s32 ret;
    struct timeval timeout;
    td_u32 error_count = 0;
    const td_u32 max_errors = 10;
    td_u32 frame_count = 0;
    td_u32 total_bytes = 0;
    
    // 获取AENC文件描述符
    aenc_fd = ss_mpi_aenc_get_fd(recorder->aenc_chn);
    if (aenc_fd < 0) {
        printf("获取AENC文件描述符失败\n");
        return NULL;
    }
    
    // 如果启用调试，打开AAC文件
    if (g_save_aac_debug) {
        g_aac_debug_file = open_aac_debug_file(recorder->aenc_chn);
    }

    printf("音频录制线程启动,AENC通道: %d\n", recorder->aenc_chn);
    
    while (recorder->is_recording && !g_exit_flag && error_count < max_errors) {
        FD_ZERO(&read_fds);
        FD_SET(aenc_fd, &read_fds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        ret = select(aenc_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            printf("音频select错误: %s\n", strerror(errno));
            error_count++;
            continue;
        } else if (ret == 0) {
            continue; // 超时，继续循环
        }
        
        if (FD_ISSET(aenc_fd, &read_fds)) {
            // 直接获取音频编码流
            ret = ss_mpi_aenc_get_stream(recorder->aenc_chn, &stream, TD_FALSE); // 非阻塞获取
            if (ret != TD_SUCCESS) {
                // 如果没有数据可获取，这是正常的，不算错误
                if (ret == OT_ERR_AENC_BUF_EMPTY) {
                    continue;
                }
                printf("获取音频编码流失败: 0x%x\n", ret);
                error_count++;
                continue;
            }
            
            
            // 检查流数据是否有效
            if (stream.len > 0 && stream.stream != NULL) {
                frame_count++;
                total_bytes += stream.len;
                // 写入MP4
                ret = write_aac_to_mp4(recorder, stream.stream, stream.len);
                if (ret != TD_SUCCESS) {
                    printf("写入音频MP4失败\n");
                    error_count++;
                } else {
                    error_count = 0; // 成功后重置错误计数
                }
            }
             // 保存到AAC调试文件
                if (g_save_aac_debug && g_aac_debug_file) {
                    size_t written = fwrite(stream.stream, 1, stream.len, g_aac_debug_file);
                    if (written != stream.len) {
                        printf("写入AAC调试文件失败: 期望%d字节, 实际%zu字节\n", 
                               stream.len, written);
                    } else {
                        // 每100帧刷新一次文件
                        if (frame_count % 100 == 0) {
                            fflush(g_aac_debug_file);
                        }
                    }
                }
            
            // 释放编码流
            ret = ss_mpi_aenc_release_stream(recorder->aenc_chn, &stream);
            if (ret != TD_SUCCESS) {
                printf("释放音频编码流失败: 0x%x\n", ret);
            }
        }
    }

    // 关闭AAC调试文件
    if (g_aac_debug_file) {
        fclose(g_aac_debug_file);
        g_aac_debug_file = NULL;
        printf("AAC调试文件已关闭，总共写入 %d 帧，%d 字节\n", frame_count, total_bytes);
    }
    
    if (error_count >= max_errors) {
        printf("音频录制线程因错误过多退出\n");
    }
    
    printf("音频录制线程退出\n");
    return NULL;
}


// 视频录制线程
static void* video_record_thread_proc(void *arg)
{
    mp4_recorder_t *recorder = (mp4_recorder_t*)arg;
    ot_venc_stream stream;
    ot_venc_chn_status status;
    fd_set read_fds;
    td_s32 venc_fd;
    td_s32 ret;
    struct timeval timeout;
    td_u32 error_count = 0;
    const td_u32 max_errors = 10;
    
    // 获取VENC文件描述符
    venc_fd = ss_mpi_venc_get_fd(recorder->venc_chn);
    if (venc_fd < 0) {
        printf("获取VENC文件描述符失败\n");
        return NULL;
    }
    
    printf("视频录制线程启动，VENC通道: %d\n", recorder->venc_chn);
    
    while (recorder->is_recording && !g_exit_flag && error_count < max_errors) {
        FD_ZERO(&read_fds);
        FD_SET(venc_fd, &read_fds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        ret = select(venc_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            printf("视频select错误: %s\n", strerror(errno));
            error_count++;
            continue;
        } else if (ret == 0) {
            continue; // 超时，继续循环
        }
        
        if (FD_ISSET(venc_fd, &read_fds)) {
            // 查询编码通道状态
            ret = ss_mpi_venc_query_status(recorder->venc_chn, &status);
            if (ret != TD_SUCCESS) {
                printf("查询VENC状态失败: 0x%x\n", ret);
                error_count++;
                continue;
            }
            
            if (status.cur_packs == 0) {
                continue;
            }
            
            // 获取编码流
            stream.pack = malloc(sizeof(ot_venc_pack) * status.cur_packs);
            if (!stream.pack) {
                printf("视频内存分配失败\n");
                error_count++;
                continue;
            }
            
            stream.pack_cnt = status.cur_packs;
            ret = ss_mpi_venc_get_stream(recorder->venc_chn, &stream, TD_TRUE);
            if (ret != TD_SUCCESS) {
                printf("获取视频编码流失败: 0x%x\n", ret);
                free(stream.pack);
                error_count++;
                continue;
            }
            
            // 处理每个包
            for (td_u32 i = 0; i < stream.pack_cnt; i++) {
                ot_venc_pack *pack = &stream.pack[i];
                td_u8 *data = (td_u8*)pack->addr + pack->offset;
                td_u32 len = pack->len - pack->offset;
                
                if (len > 0) {
                    td_u8 nalu_type = get_nalu_type(data);
                    td_bool is_keyframe = (nalu_type == 5) || (nalu_type == 7) || (nalu_type == 8);
                    
                    // 写入MP4
                    ret = write_h264_to_mp4(recorder, data, len, is_keyframe);
                    if (ret != TD_SUCCESS) {
                        printf("写入视频MP4失败\n");
                        error_count++;
                        
                        // 如果是磁盘空间问题，尝试清理
                        if (errno == 28) { // No space left on device
                            printf("磁盘空间不足，尝试清理...\n");
                            if (!ensure_disk_space()) {
                                printf("清理失败，停止录制\n");
                                recorder->is_recording = TD_FALSE;
                                break;
                            }
                        }
                    } else {
                        error_count = 0; // 成功后重置错误计数
                    }
                }
            }
            
            // 释放编码流
            ret = ss_mpi_venc_release_stream(recorder->venc_chn, &stream);
            if (ret != TD_SUCCESS) {
                printf("释放视频编码流失败: 0x%x\n", ret);
            }
            
            free(stream.pack);
        }
    }
    
    if (error_count >= max_errors) {
        printf("视频录制线程因错误过多退出\n");
    }
    
    printf("视频录制线程退出\n");
    return NULL;
}

// 创建目录（递归创建）
static td_s32 create_directory(const char *path)
{
    char tmp[MAX_FILENAME_LEN];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    // 去掉末尾的斜杠
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    // 递归创建目录
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                printf("创建目录失败: %s, 错误: %s\n", tmp, strerror(errno));
                return TD_FAILURE;
            }
            *p = '/';
        }
    }
    
    // 创建最后一级目录
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        printf("创建目录失败: %s, 错误: %s\n", tmp, strerror(errno));
        return TD_FAILURE;
    }
    
    return TD_SUCCESS;
}

// 检查目录是否存在
static td_bool directory_exists(const char *path)
{
    struct stat st;
    
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? TD_TRUE : TD_FALSE;
    }
    
    return TD_FALSE;
}

// 初始化音频系统
static td_s32 init_audio_system(void)
{
    td_s32 ret;
    
    // 初始化音频编解码器
    ret = ss_mpi_aenc_aac_init();
    if (ret != TD_SUCCESS) {
        printf("初始化AAC编码器失败: 0x%x\n", ret);
        return ret;
    }

     ret =ss_mpi_adec_aac_init();
    if (ret != TD_SUCCESS) {
        printf("初始化AAC解码器失败: 0x%x\n", ret);
        return ret;
    }

    ss_mpi_aenc_mp3_init();
    ss_mpi_adec_mp3_init();

    ss_mpi_aenc_opus_init();
    ss_mpi_adec_opus_init();
    
    printf("音频系统初始化成功\n");
    return TD_SUCCESS;
}

// 去初始化音频系统
static void deinit_audio_system(void)
{
    ss_mpi_aenc_aac_deinit();
    ss_mpi_adec_aac_deinit();

    ss_mpi_aenc_mp3_deinit();
    ss_mpi_adec_mp3_deinit();

    ss_mpi_aenc_opus_deinit();
    ss_mpi_adec_opus_deinit();
    printf("音频系统去初始化完成\n");
}

static ot_audio_sample_rate g_in_sample_rate  = OT_AUDIO_SAMPLE_RATE_BUTT;
static ot_audio_sample_rate g_out_sample_rate = OT_AUDIO_SAMPLE_RATE_BUTT;
static td_bool g_aio_resample  = TD_FALSE;

static td_void sample_audio_ai_aenc_init_param(ot_aio_attr *aio_attr, ot_audio_dev *ai_dev, ot_audio_dev *ao_dev)
{
    aio_attr->sample_rate  = OT_AUDIO_SAMPLE_RATE_48000;
    aio_attr->bit_width    = OT_AUDIO_BIT_WIDTH_16;
    aio_attr->work_mode    = OT_AIO_MODE_I2S_MASTER;
    aio_attr->snd_mode     = OT_AUDIO_SOUND_MODE_STEREO;
    aio_attr->expand_flag  = 0;
    aio_attr->frame_num    = SAMPLE_AUDIO_AI_USER_FRAME_DEPTH;
   
    aio_attr->point_num_per_frame = OT_AACLC_SAMPLES_PER_FRAME;
   
    aio_attr->chn_cnt      = 2; /* 2:chn num */
#ifdef OT_ACODEC_TYPE_INNER
    *ai_dev = SAMPLE_AUDIO_INNER_AI_DEV;
    *ao_dev = SAMPLE_AUDIO_INNER_AO_DEV;
    aio_attr->clk_share  = 1;
    aio_attr->i2s_type   = OT_AIO_I2STYPE_INNERCODEC;
#else
    *ai_dev = SAMPLE_AUDIO_EXTERN_AI_DEV;
    *ao_dev = SAMPLE_AUDIO_EXTERN_AO_DEV;
    aio_attr->clk_share  = 1;
    aio_attr->i2s_type   = OT_AIO_I2STYPE_EXTERN;
#endif

    g_aio_resample = TD_FALSE;
    g_in_sample_rate  = OT_AUDIO_SAMPLE_RATE_BUTT;
    g_out_sample_rate = OT_AUDIO_SAMPLE_RATE_BUTT;
}

static td_void sample_audio_set_ai_vqe_param(sample_comm_ai_vqe_param *ai_vqe_param,
    ot_audio_sample_rate out_sample_rate, td_bool resample_en, td_void *ai_vqe_attr, sample_audio_vqe_type ai_vqe_type)
{
    ai_vqe_param->out_sample_rate = out_sample_rate;
    ai_vqe_param->resample_en = resample_en;
    ai_vqe_param->ai_vqe_attr = ai_vqe_attr;
    ai_vqe_param->ai_vqe_type = ai_vqe_type;
}

// 启动AI
static td_s32 start_ai(void)
{
    td_s32 ret;
    ot_audio_dev ai_dev;
    ot_audio_dev ao_dev;
    td_u32 ai_chn_cnt;
    td_u32 aenc_chn_cnt;
    td_bool send_adec = TD_TRUE;
    ot_aio_attr aio_attr = {0};
    sample_comm_ai_vqe_param ai_vqe_param = {0};

    sample_audio_ai_aenc_init_param(&aio_attr, &ai_dev, &ao_dev);

    /* step 1: start ai */
    ai_chn_cnt = aio_attr.chn_cnt;
    sample_audio_set_ai_vqe_param(&ai_vqe_param, g_out_sample_rate, g_aio_resample, TD_NULL, 0);
   
    
    
    ret = sample_comm_audio_start_ai(ai_dev, ai_chn_cnt, &aio_attr, &ai_vqe_param, -1);
    if (ret != TD_SUCCESS) {
        printf("启动AI失败: 0x%x\n", ret);
        return ret;
    }
    
    printf("AI启动成功\n");
    return TD_SUCCESS;
}


static td_s32 init_audio_system_ljk(void)
{
    td_s32 ret;
    ot_aio_attr aio_attr;
    sample_comm_ai_vqe_param ai_vqe_param = {0};
    ot_audio_dev ai_dev;
    td_u32 ai_chn_cnt;
    td_u32 aenc_chn_cnt;

    printf("Initializing audio system...\n");

    // 配置音频属性
    aio_attr.sample_rate = OT_AUDIO_SAMPLE_RATE_48000;
    aio_attr.bit_width = OT_AUDIO_BIT_WIDTH_16;
    aio_attr.work_mode = OT_AIO_MODE_I2S_MASTER;
    aio_attr.snd_mode = OT_AUDIO_SOUND_MODE_STEREO;
    aio_attr.expand_flag = 0;
    aio_attr.frame_num = SAMPLE_AUDIO_AI_USER_FRAME_DEPTH;
    aio_attr.point_num_per_frame = OT_AACLC_SAMPLES_PER_FRAME;
    aio_attr.chn_cnt = 2; // 立体声：2个通道

#ifdef OT_ACODEC_TYPE_INNER
    ai_dev = SAMPLE_AUDIO_INNER_AI_DEV;
    aio_attr.clk_share = 1;
    aio_attr.i2s_type = OT_AIO_I2STYPE_INNERCODEC;
#else
    ai_dev = SAMPLE_AUDIO_EXTERN_AI_DEV;
    aio_attr.clk_share = 1;
    aio_attr.i2s_type = OT_AIO_I2STYPE_EXTERN;
#endif

    // 启动AI
    ai_chn_cnt = aio_attr.chn_cnt; // AI通道数 = 2
    ret = sample_comm_audio_start_ai(ai_dev, ai_chn_cnt, &aio_attr, &ai_vqe_param, -1);
    if (ret != TD_SUCCESS)
    {
        printf("start ai failed with %#x!\n", ret);
        return TD_FAILURE;
    }

    // 配置音频编解码器
    ret = sample_comm_audio_cfg_acodec(&aio_attr);
    if (ret != TD_SUCCESS)
    {
        printf("config audio codec failed with %#x!\n", ret);
        goto EXIT_AI_STOP;
    }

    // 使用标准的通道数计算方式
    aenc_chn_cnt = aio_attr.chn_cnt >> ((td_u32)aio_attr.snd_mode);

    printf("AI channels: %d, AENC channels: %d, Sound mode: %d\n",
           ai_chn_cnt, aenc_chn_cnt, aio_attr.snd_mode);

    // 启动AENC
    ret = sample_comm_audio_start_aenc(aenc_chn_cnt, &aio_attr, OT_PT_AAC);
    if (ret != TD_SUCCESS)
    {
        printf("start aenc failed with %#x!\n", ret);
        goto EXIT_AI_STOP;
    }

    // 使用标准的绑定函数
    ret = sample_audio_aenc_bind_ai(ai_dev, aenc_chn_cnt);
    if (ret != TD_SUCCESS)
    {
        printf("aenc bind ai failed with %#x!\n", ret);
        goto EXIT_AENC_STOP;
    }

    printf("Audio system initialized successfully\n");
    return TD_SUCCESS;

EXIT_AENC_STOP:
    sample_comm_audio_stop_aenc(aenc_chn_cnt);
EXIT_AI_STOP:
    sample_comm_audio_stop_ai(ai_dev, ai_chn_cnt, TD_FALSE, TD_FALSE);
    return TD_FAILURE;
}

#ifndef sample_dbg
#define sample_dbg(ret) printf("Error: %s:%d ret = %#x\n", __FUNCTION__, __LINE__, ret)
#endif

static td_bool g_user_get_mode = TD_FALSE; // 默认使用绑定模式
// 添加绑定函数
td_s32 sample_audio_aenc_bind_ai(ot_audio_dev ai_dev, td_u32 aenc_chn_cnt)
{
    td_s32 ret;
    td_u32 i, j;
    ot_ai_chn ai_chn;
    ot_aenc_chn ae_chn;

    for (i = 0; i < aenc_chn_cnt; i++)
    {
        ae_chn = i;
        ai_chn = i;

        if (g_user_get_mode == TD_TRUE)
        {
            ret = sample_comm_audio_create_thread_ai_aenc(ai_dev, ai_chn, ae_chn);
        }
        else
        {
            ret = sample_comm_audio_aenc_bind_ai(ai_dev, ai_chn, ae_chn);
        }
        if (ret != TD_SUCCESS)
        {
            sample_dbg(ret);
            goto aenc_bind_err;
        }
        printf("ai(%d,%d) bind to aenc_chn:%d ok!\n", ai_dev, ai_chn, ae_chn);
    }
    return TD_SUCCESS;

aenc_bind_err:
    for (j = 0; j < i; j++)
    {
        ae_chn = j;
        ai_chn = j;

        if (g_user_get_mode == TD_TRUE)
        {
            sample_comm_audio_destroy_thread_ai(ai_dev, ai_chn);
        }
        else
        {
            sample_comm_audio_aenc_unbind_ai(ai_dev, ai_chn, ae_chn);
        }
    }
    return TD_FAILURE;
}


// 停止AI
static void stop_ai(ot_audio_dev ai_dev, td_u32 ai_chn_cnt)
{
    sample_comm_audio_stop_ai(ai_dev, ai_chn_cnt, TD_FALSE, TD_FALSE);
    printf("AI停止完成\n");
}

// 启动AENC
static td_s32 start_aenc(td_u32 aenc_chn_cnt, ot_aio_attr *aio_attr)
{
    td_s32 ret;
    
    ret = sample_comm_audio_start_aenc(aenc_chn_cnt, aio_attr, OT_PT_AAC);
    if (ret != TD_SUCCESS) {
        printf("启动AENC失败: 0x%x\n", ret);
        return ret;
    }
    
    printf("AENC启动成功\n");
    return TD_SUCCESS;
}

// 停止AENC
static void stop_aenc(td_u32 aenc_chn_cnt)
{
    sample_comm_audio_stop_aenc(aenc_chn_cnt);
    printf("AENC停止完成\n");
}

// 绑定AI和AENC
static td_s32 bind_ai_aenc(ot_audio_dev ai_dev, ot_ai_chn ai_chn, ot_aenc_chn ae_chn)
{
    td_s32 ret;
    
    ret = sample_comm_audio_aenc_bind_ai(ai_dev, ai_chn, ae_chn);
    if (ret != TD_SUCCESS) {
        printf("绑定AI-AENC失败: 0x%x\n", ret);
        return ret;
    }
    
    printf("AI-AENC绑定成功\n");
    return TD_SUCCESS;
}

// 解绑AI和AENC
static void unbind_ai_aenc(ot_audio_dev ai_dev, ot_ai_chn ai_chn, ot_aenc_chn ae_chn)
{
    sample_comm_audio_aenc_unbind_ai(ai_dev, ai_chn, ae_chn);
    printf("AI-AENC解绑完成\n");
}

// 开始录制
static td_s32 start_recording(ot_venc_chn venc_chn, ot_aenc_chn aenc_chn, 
                             ot_size *enc_size, td_u32 fps)
{
    td_s32 ret;
    
    if (g_recorder.is_recording) {
        printf("录制已在进行中\n");
        return TD_FAILURE;
    }

    // 检查输出目录是否存在
    if (!directory_exists(OUTPUT_PATH)) {
        printf("输出目录不存在，正在创建: %s\n", OUTPUT_PATH);
        
        ret = create_directory(OUTPUT_PATH);
        if (ret != TD_SUCCESS) {
            printf("创建输出目录失败: %s\n", OUTPUT_PATH);
            return TD_FAILURE;
        }
        
        printf("输出目录创建成功: %s\n", OUTPUT_PATH);
    } else {
        printf("输出目录已存在: %s\n", OUTPUT_PATH);
    }
    
    // 生成文件名
    generate_mp4_filename(g_recorder.filename, sizeof(g_recorder.filename));
    g_recorder.venc_chn = venc_chn;
    g_recorder.aenc_chn = aenc_chn;
    
    // 创建MP4文件
    ret = create_mp4_file(&g_recorder, enc_size, fps);
    if (ret != TD_SUCCESS) {
        return ret;
    }
    
    // 创建视频录制线程
    ret = pthread_create(&g_recorder.video_thread, NULL, video_record_thread_proc, &g_recorder);
    if (ret != 0) {
        printf("创建视频录制线程失败: %s\n", strerror(ret));
        close_mp4_file(&g_recorder);
        return TD_FAILURE;
    }
    
    // 创建音频录制线程
    ret = pthread_create(&g_recorder.audio_thread, NULL, audio_record_thread_proc, &g_recorder);
    if (ret != 0) {
        printf("创建音频录制线程失败: %s\n", strerror(ret));
        g_recorder.is_recording = TD_FALSE;
        pthread_join(g_recorder.video_thread, NULL);
        close_mp4_file(&g_recorder);
        return TD_FAILURE;
    }
    
    printf("开始录制到: %s\n", g_recorder.filename);
    return TD_SUCCESS;
}

// 停止录制
static void stop_recording(void)
{
    if (!g_recorder.is_recording) {
        return;
    }
    
    printf("停止录制...\n");
    g_recorder.is_recording = TD_FALSE;
    
    // 等待录制线程退出
    pthread_join(g_recorder.video_thread, NULL);
    pthread_join(g_recorder.audio_thread, NULL);
    
    // 关闭MP4文件
    close_mp4_file(&g_recorder);
    
    printf("录制停止完成\n");
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

// 启动VENC
static int start_venc(ot_venc_chn venc_chn, ot_size *enc_size, 
                     int framerate, int bitrate)
{
    int ret;
    ot_venc_gop_mode gop_mode;
    ot_venc_gop_attr gop_attr;
    sample_comm_venc_chn_param chn_param = {0};
    
    // 获取GOP模式
    gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
    ret = sample_comm_venc_get_gop_attr(gop_mode, &gop_attr);
    if (ret != TD_SUCCESS) {
        printf("获取GOP属性失败: 0x%x\n", ret);
        return ret;
    }
    
    // 配置编码参数
    chn_param.type = OT_PT_H264;
    chn_param.size = sample_comm_sys_get_pic_enum(enc_size);
    chn_param.rc_mode = SAMPLE_RC_CBR;
    chn_param.profile = 0;
    chn_param.is_rcn_ref_share_buf = TD_FALSE;
    chn_param.gop_attr = gop_attr;
    chn_param.frame_rate = framerate;
    chn_param.gop = framerate * 2;
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
    int ret;
     // 🔧 修复1：先初始化音频系统
    ret = sample_comm_audio_init();
    if (ret != TD_SUCCESS)
    {
        printf("Failed to init audio comm with %#x\n", ret);
        return -1;
    }
    
    //AI_AENC();
    
    sample_sns_type sns_type = SENSOR0_TYPE;
    sample_vi_cfg vi_cfg;
    ot_size vi_size;
    sample_venc_vpss_chn_attr vpss_param;
    sample_venc_vb_attr vb_attr = { 0 };
    ot_size enc_size[CHN_NUM_MAX];
    ot_venc_chn venc_chn = 0;
    ot_aenc_chn aenc_chn = 0;
    ot_vpss_grp vpss_grp = 0;
    ot_vpss_chn vpss_chn = 0;
    ot_vi_pipe vi_pipe = 0;
    ot_vi_chn vi_chn = 0;
    ot_audio_dev ai_dev = SAMPLE_AUDIO_INNER_AI_DEV;
    ot_ai_chn ai_chn = 0;
    td_u32 framerate = 25; // 25fps
    td_u32 bitrate = 2000; // 2Mbps
    td_u32 ai_chn_cnt = 1;
    td_u32 aenc_chn_cnt = 1;
    ot_aio_attr aio_attr;
    
    printf("=== MP4录制示例程序启动 ===\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    
    // 1. 初始化系统
    ret = init_system();
    if (ret != TD_SUCCESS) {
        goto EXIT;
    }
    
    // 2. 初始化音频系统
    ret = init_audio_system();
    if (ret != TD_SUCCESS) {
        goto EXIT1;
    }
    
    // 3. 获取VI配置
    sample_comm_vi_get_default_vi_cfg(sns_type, &vi_cfg);
    sample_comm_vi_get_size_by_sns_type(sns_type, &vi_size);
    
    // 4. 启动VI
    ret = start_vi(&vi_cfg);
    if (ret != TD_SUCCESS) {
        goto EXIT2;
    }
    
    // 5. 初始化编码参数
    ret = sample_venc_init_param(enc_size, CHN_NUM_MAX, &vi_size, &vpss_param);
    if (ret != TD_SUCCESS) {
        goto EXIT3;
    }
    
    // 6. 启动VPSS
    ret = start_vpss(NULL, &vpss_param);
    if (ret != TD_SUCCESS) {
        goto EXIT3;
    }
    
    // 7. 启动VENC
    ret = start_venc(venc_chn, &enc_size[0], framerate, bitrate);
    if (ret != TD_SUCCESS) {
        goto EXIT4;
    }
    
    // 8. 配置音频属性
    aio_attr.sample_rate = OT_AUDIO_SAMPLE_RATE_48000;
    aio_attr.bit_width = OT_AUDIO_BIT_WIDTH_16;
    aio_attr.work_mode = OT_AIO_MODE_I2S_MASTER;
    aio_attr.snd_mode = OT_AUDIO_SOUND_MODE_STEREO;
    aio_attr.expand_flag = 0;
    aio_attr.frame_num = 30;
    aio_attr.point_num_per_frame = OT_AACLC_SAMPLES_PER_FRAME;
    aio_attr.chn_cnt = 2;
#ifdef OT_ACODEC_TYPE_INNER
    aio_attr.clk_share = 1;
    aio_attr.i2s_type = OT_AIO_I2STYPE_INNERCODEC;
#else
    aio_attr.clk_share = 1;
    aio_attr.i2s_type = OT_AIO_I2STYPE_EXTERN;
#endif
    
    // 9. 启动AI
    ret = start_ai();
    if (ret != TD_SUCCESS) {
        goto EXIT5;
    }
    
    // 10. 启动AENC
    ret = start_aenc(aenc_chn_cnt, &aio_attr);
    if (ret != TD_SUCCESS) {
        goto EXIT6;
    }
    
    // 11. 配置音频编解码器
    ret = sample_comm_audio_cfg_acodec(&aio_attr);
    if (ret != TD_SUCCESS) {
        goto EXIT7;
    }
    
    // 12. 绑定VI-VPSS
    ret = bind_vi_vpss(vi_pipe, vi_chn, vpss_grp, vpss_chn);
    if (ret != TD_SUCCESS) {
        goto EXIT7;
    }
    
    // 13. 绑定VPSS-VENC
    ret = bind_vpss_venc(vpss_grp, vpss_chn, venc_chn);
    if (ret != TD_SUCCESS) {
        goto EXIT8;
    }
    
    // 14. 绑定AI-AENC
    ret = bind_ai_aenc(ai_dev, ai_chn, aenc_chn);
    if (ret != TD_SUCCESS) {
        goto EXIT9;
    }
    
    // 15. 开始录制
    ret = start_recording(venc_chn, aenc_chn, &enc_size[0], framerate);
    if (ret != TD_SUCCESS) {
        goto EXIT10;
    }
    
    printf("录制进行中，按Ctrl+C停止...\n");
    
    // 16. 等待退出信号
    while (!g_exit_flag) {
        sleep(1);
    }
    
    // 清理资源
    stop_recording();
    
EXIT10:
    unbind_ai_aenc(ai_dev, ai_chn, aenc_chn);
EXIT9:
    unbind_vpss_venc(vpss_grp, vpss_chn, venc_chn);
EXIT8:
    unbind_vi_vpss(vi_pipe, vi_chn, vpss_grp, vpss_chn);
EXIT7:
    stop_aenc(aenc_chn_cnt);
EXIT6:
    stop_ai(ai_dev, ai_chn_cnt);
EXIT5:
    stop_venc(venc_chn);
EXIT4:
    stop_vpss(&vpss_param);
EXIT3:
    stop_vi(&vi_cfg);
EXIT2:
    deinit_audio_system();
EXIT1:
    deinit_system();
EXIT:
    printf("=== 程序退出 ===\n");
    return ret;
}



