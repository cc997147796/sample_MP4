#ifndef __SAMPLE_MP4_MUXER_H__
#define __SAMPLE_MP4_MUXER_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <mp4v2/mp4v2.h>
#include "ot_type.h"
#include "ot_common_video.h"


typedef struct {
    MP4FileHandle mp4_file;
    MP4TrackId video_track;
    MP4TrackId audio_track;
    td_bool audio_enabled;
    td_bool video_enabled;
    td_bool is_recording;
    pthread_mutex_t mux_mutex;
    td_u64 video_frame_count;
    td_u64 audio_frame_count;
    td_u32 video_fps;
    td_u32 audio_sample_rate;
    td_u32 video_width;
    td_u32 video_height;
    char filename[256];
    td_u8 *sps_data;
    td_u32 sps_len;
    td_u8 *pps_data;
    td_u32 pps_len;
    td_bool sps_pps_set;
} sample_mp4_muxer_ctx;

typedef struct {
    td_u8 *data;
    td_u32 len;
    td_u64 pts;
    td_bool is_keyframe;
} sample_mp4_frame_info;

// 函数声明
td_u64 sample_mp4_get_timestamp(void);
td_s32 sample_mp4_muxer_init(sample_mp4_muxer_ctx *ctx, const char *filename);
td_s32 sample_mp4_muxer_add_video_stream(sample_mp4_muxer_ctx *ctx, ot_payload_type type, td_u32 width, td_u32 height, td_u32 fps);
td_s32 sample_mp4_muxer_add_audio_stream(sample_mp4_muxer_ctx *ctx, ot_payload_type type, td_u32 sample_rate, td_u32 channels);
td_s32 sample_mp4_muxer_write_video_frame(sample_mp4_muxer_ctx *ctx, sample_mp4_frame_info *frame_info);
td_s32 sample_mp4_muxer_write_audio_frame(sample_mp4_muxer_ctx *ctx, sample_mp4_frame_info *frame_info);
td_s32 sample_mp4_muxer_close(sample_mp4_muxer_ctx *ctx);

#endif