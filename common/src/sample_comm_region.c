/*
  Copyright (c), 2001-2024, Shenshu Tech. Co., Ltd.
 */

#include "sample_comm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "loadbmp.h"

#define OVERLAY_MIN_HANDLE 0
#define COVER_MIN_HANDLE 20
#define COVEREX_MIN_HANDLE 40
#define LINEEX_MIN_HANDLE 60
#define CORNER_RECT_MIN_HANDLE 80
#define CORNER_RECTEX_MIN_HANDLE 100

#define DEFAULT_CANVAS_NUM 2

#define BITS_NUM_PER_BYTE 8
#define BYTE_PER_PIX_1555 2
#define BYTE_PER_PIX_8888 4
#define PIX_PER_BYTE_CLUT2 4
#define PIX_PER_BYTE_CLUT4 2
#define OFFSET_NUM0 200
#define OFFSET_NUM2 100
#define OFFSET_NUM3 50
#define OFFSET_NUM4 16

#define MAX_BIT_COUNT 32

#define RGN_DEFAULT_WIDTH 180
#define RGN_DEFAULT_HEIGHT 140
#define RGN_ALPHA 128
#define RGN_RGB888_BLUE 0x0000ff
#define RGN_RGB888_RED  0xff0000

#define RGN_OVERLAY_X0 200
#define RGN_OVERLAY_X1 400
#define RGN_OVERLAY_X2 600
#define RGN_OVERLAY_X3 800
#define RGN_OVERLAY_X4 1000

#define RGN_OVERLAY_Y0 40
#define RGN_OVERLAY_Y1 200
#define RGN_OVERLAY_Y2 500

#define RGN_NUM_2 2
#define RGN_NUM_4 4

static td_bool g_venc_osd_drawline;

typedef struct {
    const td_char *filename;
    ot_bmp bmp;
    td_bool fil;
    td_u32 fil_color;
    ot_pixel_format pixel_fmt;
} rgn_load_bmp_info;

static osd_color_format region_mst_get_color_format_by_pixel_format(ot_pixel_format pixel_format)
{
    switch (pixel_format) {
        /* is the same with argb1555 */
        case OT_PIXEL_FORMAT_ARGB_1555:
            return OSD_COLOR_FORMAT_RGB1555;
        case OT_PIXEL_FORMAT_ARGB_4444:
        case OT_PIXEL_FORMAT_ARGB_CLUT2:
        case OT_PIXEL_FORMAT_ARGB_CLUT2_2X2:
        case OT_PIXEL_FORMAT_ARGB_CLUT2_4X4:
        case OT_PIXEL_FORMAT_ARGB_CLUT4:
        case OT_PIXEL_FORMAT_ARGB_CLUT4_2X2:
        case OT_PIXEL_FORMAT_ARGB_CLUT4_4X4:
            return OSD_COLOR_FORMAT_RGB4444;
        case OT_PIXEL_FORMAT_ARGB_8888:
            return OSD_COLOR_FORMAT_RGB8888;
        default:
            printf("pixel format is not support!\n");
            return OSD_COLOR_FORMAT_BUTT;
    }
}

static td_u8 *region_mst_get_clut2_data_from_bmp_data(ot_bmp *bmp)
{
    td_u8 *clut_data = NULL;
    td_u16 *temp = TD_NULL;
    td_u8 *c_temp = TD_NULL;
    td_u32 i, j, k;
    td_s32 value;
    td_u8 value_temp;

    temp = (td_u16 *)bmp->data;

    clut_data = malloc(bmp->height * bmp->width / PIX_PER_BYTE_CLUT2);
    if (clut_data == NULL) {
        printf("malloc osd memory err!\n");
        return TD_NULL;
    }

    c_temp = (td_u8 *)clut_data;
    for (i = 0; i < bmp->height; i++) {
        for (j = 0; j < bmp->width / PIX_PER_BYTE_CLUT2; j++) {
            value = 0;
            for (k = 0; k < PIX_PER_BYTE_CLUT2; k++) {
                value_temp = ((*temp & 0x001f) * 28 + ((*temp >> 5) & 0x001f) * 58 + /* 0x001f:28:5:58:color modulus */
                    ((*temp >> 10) & 0x001f) * 14) / 800; /* 10:0x001f:14:800:color modulus */
                value_temp = value_temp << (2 * (PIX_PER_BYTE_CLUT2 - k - 1)); /* 2:color modulus */
                value += value_temp;
                temp++;
            }
            *c_temp = value;
            c_temp++;
        }
    }

    return clut_data;
}

static td_u8 *region_mst_get_clut4_data_from_bmp_data(ot_bmp *bmp)
{
    td_u8 *clut_data = NULL;
    td_u16 *temp = TD_NULL;
    td_u8 *c_temp = TD_NULL;
    td_u32 i, j, k;
    td_s32 value;
    td_u8 value_temp;

    temp = (td_u16 *)bmp->data;

    clut_data = malloc(bmp->height * bmp->width / PIX_PER_BYTE_CLUT4);
    if (clut_data == NULL) {
        printf("malloc osd memory err!\n");
        return TD_NULL;
    }

    c_temp = (td_u8 *)clut_data;
    for (i = 0; i < bmp->height; i++) {
        for (j = 0; j < bmp->width / PIX_PER_BYTE_CLUT4; j++) {
            value = 0;
            for (k = j; k < j + PIX_PER_BYTE_CLUT4; k++) {
                value_temp = ((*temp & 0x001f) * 28 + ((*temp >> 5) & 0x001f) * 58 + /* 0x001f:28:5:58:color modulus */
                    ((*temp >> 10) & 0x001f) * 14) / 200; /* 10:0x001f:14:200:color modulus */
                temp++;
                value = (value_temp << 4) + value_temp; /* 4:color modulus */
            }
            *c_temp = value;
            c_temp++;
        }
    }

    return clut_data;
}

td_void fil_bmp(ot_bmp *bmp, td_bool fil, td_u32 fil_color)
{
    td_u16 *temp = TD_NULL;
    td_u32 i, j;

    if (!fil) {
        return;
    }

    temp = (td_u16 *)bmp->data;
    for (i = 0; i < bmp->height; i++) {
        for (j = 0; j < bmp->width; j++) {
            if (*temp == fil_color) {
                *temp &= 0x7FFF;
            }
            temp++;
        }
    }
    return;
}

td_s32 region_mst_load_bmp(rgn_load_bmp_info *load_bmp_info)
{
    osd_surface surface;
    osd_bit_map_file_header bmp_file_header;
    osd_bit_map_info bmp_info;
    td_u32 bpp;
    td_u8 *clut_data = TD_NULL;
    ot_bmp *bmp = TD_NULL;
    if (get_bmp_info(load_bmp_info->filename, &bmp_file_header, &bmp_info) < 0) {
        return TD_FAILURE;
    }

    if (bmp_info.bmp_header.bi_bit_count > MAX_BIT_COUNT || bmp_info.bmp_header.bi_width > OT_RGN_OVERLAY_MAX_WIDTH ||
        bmp_info.bmp_header.bi_height > OT_RGN_OVERLAY_MAX_HEIGHT) {
        printf("bmp info error!");
        return TD_FAILURE;
    }
    surface.color_format = region_mst_get_color_format_by_pixel_format(load_bmp_info->pixel_fmt);
    if (surface.color_format == OSD_COLOR_FORMAT_BUTT) {
        return TD_FAILURE;
    }
    bpp = bmp_info.bmp_header.bi_bit_count / BITS_NUM_PER_BYTE;
    bmp = &load_bmp_info->bmp;
    bmp->data = malloc(bmp_info.bmp_header.bi_width * bpp * abs(bmp_info.bmp_header.bi_height));
    if (bmp->data == TD_NULL) {
        printf("malloc osd memory err!\n");
        return TD_FAILURE;
    }

    if (create_surface_by_bit_map(load_bmp_info->filename, &surface, (td_u8 *)(bmp->data)) != TD_SUCCESS) {
        free(bmp->data);
        return TD_FAILURE;
    }
    bmp->width = surface.width;
    bmp->height = surface.height;
    bmp->pixel_format = load_bmp_info->pixel_fmt;

    if (load_bmp_info->pixel_fmt == OT_PIXEL_FORMAT_ARGB_CLUT2 ||
        load_bmp_info->pixel_fmt == OT_PIXEL_FORMAT_ARGB_CLUT2_2X2 ||
        load_bmp_info->pixel_fmt == OT_PIXEL_FORMAT_ARGB_CLUT2_4X4) {
        clut_data = region_mst_get_clut2_data_from_bmp_data(bmp);
    } else if (load_bmp_info->pixel_fmt == OT_PIXEL_FORMAT_ARGB_CLUT4 ||
               load_bmp_info->pixel_fmt == OT_PIXEL_FORMAT_ARGB_CLUT4_2X2 ||
               load_bmp_info->pixel_fmt == OT_PIXEL_FORMAT_ARGB_CLUT4_4X4) {
        clut_data = region_mst_get_clut4_data_from_bmp_data(bmp);
    } else {
        fil_bmp(bmp, load_bmp_info->fil, load_bmp_info->fil_color);
    }
    if (clut_data != TD_NULL) {
        free(bmp->data);
        bmp->data = clut_data;
    }
    return TD_SUCCESS;
}

td_s32 region_mst_update_canvas(rgn_load_bmp_info *load_bmp_info, ot_size *size, td_u32 stride)
{
    osd_surface surface;
    osd_bit_map_file_header bmp_file_header;
    osd_bit_map_info bmp_info;
    canvas_size_info canvas_size;

    if (get_bmp_info(load_bmp_info->filename, &bmp_file_header, &bmp_info) < 0) {
        printf("get_bmp_info err!\n");
        return TD_FAILURE;
    }

    if (OT_PIXEL_FORMAT_ARGB_1555 == load_bmp_info->pixel_fmt) {
        surface.color_format = OSD_COLOR_FORMAT_RGB1555;
    } else if (OT_PIXEL_FORMAT_ARGB_4444 == load_bmp_info->pixel_fmt) {
        surface.color_format = OSD_COLOR_FORMAT_RGB4444;
    } else if (OT_PIXEL_FORMAT_ARGB_8888 == load_bmp_info->pixel_fmt) {
        surface.color_format = OSD_COLOR_FORMAT_RGB8888;
    } else if (OT_PIXEL_FORMAT_ARGB_CLUT2 == load_bmp_info->pixel_fmt) {
        surface.color_format = OSD_COLOR_FORMAT_CLUT4;
    } else if (OT_PIXEL_FORMAT_ARGB_CLUT4 == load_bmp_info->pixel_fmt) {
        surface.color_format = OSD_COLOR_FORMAT_CLUT4;
    } else {
        printf("pixel format is not support!\n");
        return TD_FAILURE;
    }

    if (load_bmp_info->bmp.data == NULL) {
        printf("malloc osd memory err!\n");
        return TD_FAILURE;
    }

    canvas_size.width = size->width;
    canvas_size.height = size->height;
    canvas_size.stride = stride;
    create_surface_by_canvas(load_bmp_info->filename, &surface, (td_u8 *)(load_bmp_info->bmp.data), &canvas_size);

    load_bmp_info->bmp.width = surface.width;
    load_bmp_info->bmp.height = surface.height;

    if (OT_PIXEL_FORMAT_ARGB_1555 == load_bmp_info->pixel_fmt) {
        load_bmp_info->bmp.pixel_format = OT_PIXEL_FORMAT_ARGB_1555;
    } else if (OT_PIXEL_FORMAT_ARGB_4444 == load_bmp_info->pixel_fmt) {
        load_bmp_info->bmp.pixel_format = OT_PIXEL_FORMAT_ARGB_4444;
    } else if (OT_PIXEL_FORMAT_ARGB_8888 == load_bmp_info->pixel_fmt) {
        load_bmp_info->bmp.pixel_format = OT_PIXEL_FORMAT_ARGB_8888;
    } else if (OT_PIXEL_FORMAT_ARGB_CLUT2 == load_bmp_info->pixel_fmt) {
        load_bmp_info->bmp.pixel_format = OT_PIXEL_FORMAT_ARGB_CLUT2;
    } else if (OT_PIXEL_FORMAT_ARGB_CLUT4 == load_bmp_info->pixel_fmt) {
        load_bmp_info->bmp.pixel_format = OT_PIXEL_FORMAT_ARGB_CLUT4;
    }

    return TD_SUCCESS;
}

td_s32 sample_comm_region_get_min_handle(ot_rgn_type type)
{
    td_s32 min_handle;
    switch (type) {
        case OT_RGN_OVERLAY:
            min_handle = OVERLAY_MIN_HANDLE;
            break;
        case OT_RGN_COVER:
            min_handle = COVER_MIN_HANDLE;
            break;
        case OT_RGN_COVEREX:
            min_handle = COVEREX_MIN_HANDLE;
            break;
        case OT_RGN_LINEEX:
            min_handle = LINEEX_MIN_HANDLE;
            break;
        case OT_RGN_CORNER_RECT:
            min_handle = CORNER_RECT_MIN_HANDLE;
            break;
        case OT_RGN_CORNER_RECTEX:
            min_handle = CORNER_RECTEX_MIN_HANDLE;
            break;
        default:
            min_handle = -1;
            break;
    }
    return min_handle;
}

static ot_pixel_format sample_comm_region_get_pixel_format_by_handle(ot_rgn_handle handle)
{
    switch (handle) {
        case 0: /* 0: argb1555 */
            return OT_PIXEL_FORMAT_ARGB_1555;
        case 1: /* 1: argb4444 */
            return OT_PIXEL_FORMAT_ARGB_4444;
        case 2: /* 2: clut2 */
            return OT_PIXEL_FORMAT_ARGB_CLUT2;
        case 3: /* 3: clut4 */
            return OT_PIXEL_FORMAT_ARGB_CLUT4;
        case 4: /* 4: clut2_2x2 */
            return OT_PIXEL_FORMAT_ARGB_CLUT2_2X2;
        case 5: /* 5: clut4_2x2 */
            return OT_PIXEL_FORMAT_ARGB_CLUT4_2X2;
        case 6: /* 6: clut2_4x4 */
            return OT_PIXEL_FORMAT_ARGB_CLUT2_4X4;
        case 7: /* 7: clut4_4x4 */
            return OT_PIXEL_FORMAT_ARGB_CLUT4_4X4;
    }
    return OT_PIXEL_FORMAT_BUTT;
}

static td_u32 sample_region_get_overlay_bg_color(ot_pixel_format pixel_format)
{
    if (pixel_format == OT_PIXEL_FORMAT_ARGB_1555 || pixel_format == OT_PIXEL_FORMAT_ARGB_4444) {
        return 0xffff;
    }
    return 1;
}

static td_u32 sample_region_get_overlay_default_width(ot_pixel_format pixel_format)
{
    switch (pixel_format) {
        case OT_PIXEL_FORMAT_ARGB_1555:
        case OT_PIXEL_FORMAT_ARGB_4444:
            return RGN_DEFAULT_WIDTH;
        case OT_PIXEL_FORMAT_ARGB_CLUT2:
        case OT_PIXEL_FORMAT_ARGB_CLUT4:
            return RGN_DEFAULT_WIDTH;
        case OT_PIXEL_FORMAT_ARGB_CLUT2_2X2:
        case OT_PIXEL_FORMAT_ARGB_CLUT4_2X2:
            return RGN_DEFAULT_WIDTH * 2; /* 2: 2xwidth */
        case OT_PIXEL_FORMAT_ARGB_CLUT2_4X4:
        case OT_PIXEL_FORMAT_ARGB_CLUT4_4X4:
            return RGN_DEFAULT_WIDTH * 4; /* 4: 4xwidth */
        default:
            break;
    }
    return RGN_DEFAULT_WIDTH;
}

static td_u32 sample_region_get_overlay_default_height(ot_pixel_format pixel_format)
{
    switch (pixel_format) {
        case OT_PIXEL_FORMAT_ARGB_1555:
        case OT_PIXEL_FORMAT_ARGB_4444:
            return RGN_DEFAULT_HEIGHT;
        case OT_PIXEL_FORMAT_ARGB_CLUT2:
        case OT_PIXEL_FORMAT_ARGB_CLUT4:
            return RGN_DEFAULT_HEIGHT;
        case OT_PIXEL_FORMAT_ARGB_CLUT2_2X2:
        case OT_PIXEL_FORMAT_ARGB_CLUT4_2X2:
            return RGN_DEFAULT_HEIGHT * 2; /* 2: 2xheight */
        case OT_PIXEL_FORMAT_ARGB_CLUT2_4X4:
        case OT_PIXEL_FORMAT_ARGB_CLUT4_4X4:
            return RGN_DEFAULT_HEIGHT * 4; /* 4: 4xheight */
        default:
            break;
    }
    return RGN_DEFAULT_HEIGHT;
}

static td_void sample_region_set_drawline_clut_color_table(td_u32 *clut)
{
    clut[0] = 0x0;  // Full Transparency
    clut[1] = 0xffff0000; /* 1: clut index */
    clut[2] = 0xff00ff00; /* 2: clut index */
    clut[3] = 0xff0000ff; /* 3: clut index */
}

td_s32 sample_region_create_overlay(td_s32 handle_num)
{
    td_s32 ret;
    td_s32 i;
    td_s32 min_handle;
    ot_rgn_attr region;

    region.type = OT_RGN_OVERLAY;
    region.attr.overlay.canvas_num = DEFAULT_CANVAS_NUM;
    region.attr.overlay.clut[0] = 0xffffffff;
    for (i = 1; i < OT_RGN_CLUT_NUM; i++) {
        region.attr.overlay.clut[i] = (td_u32)(0x000fff0f * (i + 1) * (i + 1));
        region.attr.overlay.clut[i] |= 0xff000000;
    }

    if (g_venc_osd_drawline) {
        sample_region_set_drawline_clut_color_table(region.attr.overlay.clut);
    }

    min_handle = sample_comm_region_get_min_handle(region.type);
    for (i = min_handle; i < min_handle + handle_num; i++) {
        region.attr.overlay.pixel_format = sample_comm_region_get_pixel_format_by_handle(i - min_handle);
        region.attr.overlay.size.width = sample_region_get_overlay_default_width(region.attr.overlay.pixel_format);
        region.attr.overlay.size.height = sample_region_get_overlay_default_height(region.attr.overlay.pixel_format);
        region.attr.overlay.bg_color = sample_region_get_overlay_bg_color(region.attr.overlay.pixel_format);
        ret = ss_mpi_rgn_create(i, &region);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_create failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    }
    return TD_SUCCESS;
}

td_s32 sample_region_create_cover(td_s32 handle_num)
{
    td_s32 ret;
    td_s32 i;
    ot_rgn_attr region;

    region.type = OT_RGN_COVER;

    for (i = COVER_MIN_HANDLE; i < COVER_MIN_HANDLE + handle_num; i++) {
        ret = ss_mpi_rgn_create(i, &region);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_create failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    }

    return TD_SUCCESS;
}

td_s32 sample_region_create_coverex(td_s32 handle_num)
{
    td_s32 ret;
    td_s32 i;
    ot_rgn_attr region;

    region.type = OT_RGN_COVEREX;

    for (i = COVEREX_MIN_HANDLE; i < COVEREX_MIN_HANDLE + handle_num; i++) {
        ret = ss_mpi_rgn_create(i, &region);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_create failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    }

    return TD_SUCCESS;
}

td_s32 sample_region_create_line(td_s32 handle_num)
{
    td_s32 ret;
    td_s32 i;
    ot_rgn_attr region;

    region.type = OT_RGN_LINEEX;

    for (i = LINEEX_MIN_HANDLE; i < LINEEX_MIN_HANDLE + handle_num; i++) {
        ret = ss_mpi_rgn_create(i, &region);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_create failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    }

    return TD_SUCCESS;
}

td_s32 sample_region_create_corner_rect(td_s32 handle_num)
{
    td_s32 ret;
    td_s32 i;
    ot_rgn_attr region;

    region.type = OT_RGN_CORNER_RECT;

    for (i = CORNER_RECT_MIN_HANDLE; i < CORNER_RECT_MIN_HANDLE + handle_num; i++) {
        ret = ss_mpi_rgn_create(i, &region);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_create failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    }

    return TD_SUCCESS;
}

td_s32 sample_region_create_corner_rectex(td_s32 handle_num)
{
    td_s32 ret;
    td_s32 i;
    ot_rgn_attr region;

    region.type = OT_RGN_CORNER_RECTEX;

    for (i = CORNER_RECTEX_MIN_HANDLE; i < CORNER_RECTEX_MIN_HANDLE + handle_num; i++) {
        ret = ss_mpi_rgn_create(i, &region);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_create failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    }

    return TD_SUCCESS;
}

td_s32 sample_region_destroy(ot_rgn_handle handle)
{
    td_s32 ret;

    ret = ss_mpi_rgn_destroy(handle);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_rgn_destroy failed with %#x!\n", ret);
        return TD_FAILURE;
    }

    return TD_SUCCESS;
}

td_s32 sample_region_attach(ot_rgn_handle handle, ot_mpp_chn *chn, ot_rgn_chn_attr *chn_attr,
    region_op_flag op_flag)
{
    td_s32 ret;

    if (op_flag & REGION_OP_CHN) {
        ret = ss_mpi_rgn_attach_to_chn(handle, chn, chn_attr);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_attach_to_chn failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    } else if (op_flag & REGION_OP_DEV) {
        ret = ss_mpi_rgn_attach_to_dev(handle, chn, chn_attr);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_attach_to_dev failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    }

    return TD_SUCCESS;
}

td_s32 sample_region_detach(ot_rgn_handle handle, ot_mpp_chn *chn, region_op_flag op_flag)
{
    td_s32 ret;

    if (op_flag & REGION_OP_CHN) {
        ret = ss_mpi_rgn_detach_from_chn(handle, chn);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_detach_from_chn failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    } else if (op_flag & REGION_OP_DEV) {
        ret = ss_mpi_rgn_detach_from_dev(handle, chn);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_rgn_detach_from_dev failed with %#x!\n", ret);
            return TD_FAILURE;
        }
    }

    return TD_SUCCESS;
}

td_s32 sample_region_set_bit_map(ot_rgn_handle handle, ot_bmp *bitmap)
{
    td_s32 ret;
    if (bitmap->pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT2_2X2 ||
        bitmap->pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT4_2X2) {
        bitmap->width  *= 2; /* 2: width x 2 */
        bitmap->height *= 2; /* 2: height x 2 */
    } else if (bitmap->pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT2_4X4 ||
        bitmap->pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT4_4X4) {
        bitmap->width  *= 4; /* 4: width x 4 */
        bitmap->height *= 4; /* 4: height x 4 */
    }

    ret = ss_mpi_rgn_set_bmp(handle, bitmap);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_rgn_set_bit_map failed with %#x!\n", ret);
        return TD_FAILURE;
    }
    return TD_SUCCESS;
}

td_s32 sample_region_get_up_canvas_info(ot_rgn_handle handle, ot_rgn_canvas_info *canvas_info)
{
    td_s32 ret;
    ret = ss_mpi_rgn_get_canvas_info(handle, canvas_info);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_rgn_get_canvas_info failed with %#x!\n", ret);
        return TD_FAILURE;
    }

    ret = ss_mpi_rgn_update_canvas(handle);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_rgn_update_canvas failed with %#x!\n", ret);
        return TD_FAILURE;
    }
    return TD_SUCCESS;
}

td_s32 sample_comm_region_create(td_s32 handle_num, ot_rgn_type type)
{
    td_s32 ret = TD_SUCCESS;
    if (handle_num <= 0 || handle_num > 16) { /* 16:max_num */
        sample_print("handle_num is illegal %d!\n", handle_num);
        return TD_FAILURE;
    }
    if (type < 0 || type >= OT_RGN_BUTT) {
        sample_print("type is illegal %d!\n", type);
        return TD_FAILURE;
    }
    switch (type) {
        case OT_RGN_OVERLAY:
            ret = sample_region_create_overlay(handle_num);
            break;
        case OT_RGN_COVER:
            ret = sample_region_create_cover(handle_num);
            break;
        case OT_RGN_COVEREX:
            ret = sample_region_create_coverex(handle_num);
            break;
        case OT_RGN_LINEEX:
            ret = sample_region_create_line(handle_num);
            break;
        case OT_RGN_CORNER_RECT:
            ret = sample_region_create_corner_rect(handle_num);
            break;
        case OT_RGN_CORNER_RECTEX:
            ret = sample_region_create_corner_rectex(handle_num);
            break;
        default:
            break;
    }
    if (ret != TD_SUCCESS) {
        sample_print("sample_comm_region_create failed! handle_num%d,type:%d!\n", handle_num, type);
        return TD_FAILURE;
    }
    return ret;
}

td_s32 sample_comm_region_destroy(td_s32 handle_num, ot_rgn_type type)
{
    td_s32 i;
    td_s32 ret;
    td_s32 min_handle;

    if (handle_num <= 0 || handle_num > 16) { /* 16 max_num */
        sample_print("handle_num is illegal %d!\n", handle_num);
        return TD_FAILURE;
    }
    if (type < 0 || type >= OT_RGN_BUTT) {
        sample_print("type is illegal %d!\n", type);
        return TD_FAILURE;
    }
    min_handle = sample_comm_region_get_min_handle(type);
    for (i = min_handle; i < min_handle + handle_num; i++) {
        ret = sample_region_destroy(i);
        if (ret != TD_SUCCESS) {
            sample_print("sample_comm_region_destroy failed!\n");
        }
    }
    return TD_SUCCESS;
}

td_s32 sample_comm_region_attach_check(td_s32 handle_num, ot_rgn_type type, ot_mpp_chn *mpp_chn)
{
    if (handle_num <= 0 || handle_num > 16) { /* 16 max_num */
        sample_print("handle_num is illegal %d!\n", handle_num);
        return TD_FAILURE;
    }
    if (type < 0 || type >= OT_RGN_BUTT) {
        sample_print("type is illegal %d!\n", type);
        return TD_FAILURE;
    }
    if (mpp_chn == TD_NULL) {
        sample_print("mpp_chn is NULL !\n");
        return TD_FAILURE;
    }
    return TD_SUCCESS;
}

#define rgn_check_handle_min_ret(handle, min_value) \
    do { \
        if ((handle) < (min_value)) { \
            sample_print("handle(%d) invalid\n", (handle)); \
            return; \
        } \
    } while (0)

static td_void sample_region_get_overlay_point(td_u32 handle, ot_point *point)
{
    ot_pixel_format pixel_fmt = sample_comm_region_get_pixel_format_by_handle(handle);

    switch (pixel_fmt) {
        case OT_PIXEL_FORMAT_ARGB_1555:
            point->x = RGN_OVERLAY_X0;
            point->y = RGN_OVERLAY_Y0;
            break;
        case OT_PIXEL_FORMAT_ARGB_4444:
            point->x = RGN_OVERLAY_X1;
            point->y = RGN_OVERLAY_Y0;
            break;
        case OT_PIXEL_FORMAT_ARGB_CLUT2:
            point->x = RGN_OVERLAY_X2;
            point->y = RGN_OVERLAY_Y0;
            break;
        case OT_PIXEL_FORMAT_ARGB_CLUT4:
            point->x = RGN_OVERLAY_X3;
            point->y = RGN_OVERLAY_Y0;
            break;
        case OT_PIXEL_FORMAT_ARGB_CLUT2_2X2:
            point->x = RGN_OVERLAY_X0;
            point->y = RGN_OVERLAY_Y1;
            break;
        case OT_PIXEL_FORMAT_ARGB_CLUT4_2X2:
            point->x = RGN_OVERLAY_X2;
            point->y = RGN_OVERLAY_Y1;
            break;
        case OT_PIXEL_FORMAT_ARGB_CLUT2_4X4:
            point->x = RGN_OVERLAY_X0;
            point->y = RGN_OVERLAY_Y2;
            break;
        case OT_PIXEL_FORMAT_ARGB_CLUT4_4X4:
            point->x = RGN_OVERLAY_X4;
            point->y = RGN_OVERLAY_Y2;
            break;
        default:
            point->x = RGN_OVERLAY_X0;
            point->y = RGN_OVERLAY_Y0;
            break;
    }
}

static td_void sample_region_get_overlay_chn_attr(td_s32 handle, ot_rgn_overlay_chn_attr *overlay_chn)
{
    rgn_check_handle_min_ret(handle, OVERLAY_MIN_HANDLE);

    overlay_chn->bg_alpha = RGN_ALPHA;
    overlay_chn->fg_alpha = RGN_ALPHA;
    overlay_chn->qp_info.enable = TD_TRUE;
    overlay_chn->qp_info.is_abs_qp = TD_TRUE;
    overlay_chn->qp_info.qp_val = 30; /* 30: default qp value */
    overlay_chn->dst = OT_RGN_ATTACH_JPEG_MAIN;
    if (g_venc_osd_drawline == TD_TRUE) {
        overlay_chn->bg_alpha = 0;
        overlay_chn->qp_info.enable = TD_FALSE;
    }
    sample_region_get_overlay_point(handle, &overlay_chn->point);
    overlay_chn->layer = (td_u32)(handle - OVERLAY_MIN_HANDLE);
}

td_s32 sample_region_set_overlay_chn_attr(td_s32 handle_num,
    ot_rgn_chn_attr *chn_attr, ot_mpp_chn *mpp_chn, region_op_flag op_flag)
{
    td_s32 i;
    td_s32 ret;

    rgn_check_handle_num_return(handle_num);

    chn_attr->type = OT_RGN_OVERLAY;

    for (i = OVERLAY_MIN_HANDLE; i < OVERLAY_MIN_HANDLE + handle_num; i++) {
        sample_region_get_overlay_chn_attr(i, &chn_attr->attr.overlay_chn);
        ret = sample_region_attach(i, mpp_chn, chn_attr, op_flag);
        if (ret != TD_SUCCESS) {
            sample_print("sample_region_attach failed!\n");
            sample_comm_region_detach(i - OVERLAY_MIN_HANDLE + 1, OT_RGN_OVERLAY, mpp_chn, op_flag);
            return ret;
        }
    }
    return TD_SUCCESS;
}

static td_void sample_region_get_rect_cover_chn_attr(td_s32 handle, ot_rgn_cover_chn_attr *cover_chn)
{
    rgn_check_handle_min_ret(handle, COVER_MIN_HANDLE);

    cover_chn->coord = OT_COORD_ABS;
    cover_chn->layer = (td_u32)(handle - COVER_MIN_HANDLE);

    cover_chn->cover.type = OT_COVER_RECT;
    cover_chn->cover.color = RGN_RGB888_BLUE;
    cover_chn->cover.rect_attr.rect.height = RGN_DEFAULT_HEIGHT;
    cover_chn->cover.rect_attr.rect.width = RGN_DEFAULT_WIDTH;
    cover_chn->cover.rect_attr.rect.x = OFFSET_NUM0 * (handle - COVER_MIN_HANDLE) + OFFSET_NUM0;
    cover_chn->cover.rect_attr.rect.y = OFFSET_NUM0;
    cover_chn->cover.rect_attr.is_solid = (handle % 2 == 0) ? /* 2: solid or unsolid by remainder */
        TD_TRUE : TD_FALSE;
    cover_chn->cover.rect_attr.thick = OT_RGN_COVER_MIN_THICK;
}

td_s32 sample_region_set_cover_chn_attr(td_s32 handle_num,
    ot_rgn_chn_attr *chn_attr, ot_mpp_chn *mpp_chn, region_op_flag op_flag)
{
    td_s32 i;
    td_s32 ret;

    rgn_check_handle_num_return(handle_num);

    chn_attr->type = OT_RGN_COVER;

    for (i = COVER_MIN_HANDLE; i < COVER_MIN_HANDLE + handle_num; i++) {
        sample_region_get_rect_cover_chn_attr(i, &chn_attr->attr.cover_chn);
        ret = sample_region_attach(i, mpp_chn, chn_attr, op_flag);
        if (ret != TD_SUCCESS) {
            sample_print("sample_region_attach failed!\n");
            sample_comm_region_detach(i - COVER_MIN_HANDLE + 1, OT_RGN_COVER, mpp_chn, op_flag);
            return ret;
        }
    }
    return TD_SUCCESS;
}

static td_void sample_region_get_quad_coverex_chn_attr(td_s32 handle,
    ot_rgn_coverex_chn_attr *coverex_chn)
{
    rgn_check_handle_min_ret(handle, COVEREX_MIN_HANDLE);

    coverex_chn->coord = OT_COORD_ABS;
    coverex_chn->layer = (td_u32)(handle - COVEREX_MIN_HANDLE);
    coverex_chn->coverex.type = OT_COVER_QUAD;
    coverex_chn->coverex.color = RGN_RGB888_RED;
    coverex_chn->coverex.quad_attr.is_solid = TD_FALSE;
    coverex_chn->coverex.quad_attr.thick = (td_u32)(OT_RGN_COVEREX_MIN_THICK + (handle - COVEREX_MIN_HANDLE) %
        (OT_RGN_COVEREX_MAX_THICK - OT_RGN_COVEREX_MIN_THICK + 1));
    coverex_chn->coverex.quad_attr.point[0].x = OFFSET_NUM0 * (handle - COVEREX_MIN_HANDLE);
    coverex_chn->coverex.quad_attr.point[0].y = OFFSET_NUM3 + OFFSET_NUM3;
    coverex_chn->coverex.quad_attr.point[1].x = OFFSET_NUM0 * (handle - COVEREX_MIN_HANDLE) + OFFSET_NUM2;
    coverex_chn->coverex.quad_attr.point[1].y = OFFSET_NUM3;
    coverex_chn->coverex.quad_attr.point[2].x = /* 2:point num */
        OFFSET_NUM0 * (handle - COVEREX_MIN_HANDLE) + OFFSET_NUM0;
    coverex_chn->coverex.quad_attr.point[2].y = OFFSET_NUM3 + OFFSET_NUM3; /* 2:point num */
    coverex_chn->coverex.quad_attr.point[3].x =                            /* 3:point num */
        OFFSET_NUM0 * (handle - COVEREX_MIN_HANDLE) + OFFSET_NUM2;
    coverex_chn->coverex.quad_attr.point[3].y = OFFSET_NUM3 + OFFSET_NUM2; /* 3:point num */
}

static td_void sample_region_get_rect_coverex_chn_attr(td_s32 handle,
    ot_rgn_coverex_chn_attr *coverex_chn)
{
    rgn_check_handle_min_ret(handle, COVEREX_MIN_HANDLE);

    coverex_chn->coord = OT_COORD_ABS;
    coverex_chn->layer = (td_u32)(handle - COVEREX_MIN_HANDLE);
    coverex_chn->coverex.type = OT_COVER_RECT;
    coverex_chn->coverex.color = RGN_RGB888_BLUE;
    coverex_chn->coverex.rect_attr.rect.height = RGN_DEFAULT_HEIGHT;
    coverex_chn->coverex.rect_attr.rect.width = RGN_DEFAULT_WIDTH;
    coverex_chn->coverex.rect_attr.rect.x = OFFSET_NUM0 * (handle - COVEREX_MIN_HANDLE) + OFFSET_NUM0;
    coverex_chn->coverex.rect_attr.rect.y = OFFSET_NUM0;
    coverex_chn->coverex.rect_attr.is_solid = (handle % 2 == 0) ? /* 2: solid or unsolid by remainder */
        TD_FALSE : TD_TRUE;
    coverex_chn->coverex.rect_attr.thick = OT_RGN_COVEREX_MIN_THICK;
}

td_s32 sample_region_set_coverex_chn_attr(td_s32 handle_num,
    ot_rgn_chn_attr *chn_attr, ot_mpp_chn *mpp_chn, region_op_flag op_flag)
{
    td_s32 i;
    td_s32 ret;

    rgn_check_handle_num_return(handle_num);

    chn_attr->type = OT_RGN_COVEREX;

    for (i = COVEREX_MIN_HANDLE; i < COVEREX_MIN_HANDLE + handle_num; i++) {
        if (i - COVEREX_MIN_HANDLE < 1) { /* 1:quad num */
            sample_region_get_quad_coverex_chn_attr(i, &chn_attr->attr.coverex_chn);
        } else {
            sample_region_get_rect_coverex_chn_attr(i, &chn_attr->attr.coverex_chn);
        }
        ret = sample_region_attach(i, mpp_chn, chn_attr, op_flag);
        if (ret != TD_SUCCESS) {
            sample_print("sample_region_attach failed!\n");
            sample_comm_region_detach(i - COVEREX_MIN_HANDLE + 1, OT_RGN_COVEREX, mpp_chn, op_flag);
            return ret;
        }
    }
    return TD_SUCCESS;
}

static td_void sample_region_get_corner_rect_chn_attr(td_s32 handle,
    ot_rgn_corner_rect_chn_attr *corner_rect_chn)
{
    rgn_check_handle_min_ret(handle, CORNER_RECT_MIN_HANDLE);

    corner_rect_chn->corner_rect.rect.height = RGN_DEFAULT_HEIGHT;
    corner_rect_chn->corner_rect.rect.width = RGN_DEFAULT_WIDTH;
    corner_rect_chn->corner_rect.thick = OT_RGN_CORNER_RECT_MIN_THICK;
    corner_rect_chn->corner_rect.hor_len = OFFSET_NUM4;
    corner_rect_chn->corner_rect.ver_len = OFFSET_NUM4;
    corner_rect_chn->corner_rect_attr.color = RGN_RGB888_RED;
    corner_rect_chn->corner_rect_attr.corner_rect_type = OT_CORNER_RECT_TYPE_CORNER;
    corner_rect_chn->layer = (td_u32)(handle - CORNER_RECT_MIN_HANDLE);
    corner_rect_chn->corner_rect.rect.x = OFFSET_NUM0 * (handle - CORNER_RECT_MIN_HANDLE) + OFFSET_NUM3;
    corner_rect_chn->corner_rect.rect.y = OFFSET_NUM2 * (handle - CORNER_RECT_MIN_HANDLE) + OFFSET_NUM3;
}

static td_void sample_region_get_corner_rectex_chn_attr(td_s32 handle,
    ot_rgn_corner_rectex_chn_attr *corner_rectex_chn)
{
    rgn_check_handle_min_ret(handle, CORNER_RECTEX_MIN_HANDLE);

    corner_rectex_chn->corner_rect.rect.height = RGN_DEFAULT_HEIGHT;
    corner_rectex_chn->corner_rect.rect.width = RGN_DEFAULT_WIDTH;
    corner_rectex_chn->corner_rect.thick = OT_RGN_CORNER_RECTEX_MIN_THICK;
    corner_rectex_chn->corner_rect.hor_len = OFFSET_NUM4;
    corner_rectex_chn->corner_rect.ver_len = OFFSET_NUM4;
    corner_rectex_chn->corner_rect_attr.color = RGN_RGB888_RED;
    corner_rectex_chn->corner_rect_attr.corner_rect_type = OT_CORNER_RECT_TYPE_CORNER;
    corner_rectex_chn->layer = (td_u32)(handle - CORNER_RECTEX_MIN_HANDLE);
    corner_rectex_chn->corner_rect.rect.x = OFFSET_NUM0 * (handle - CORNER_RECTEX_MIN_HANDLE) + OFFSET_NUM3;
    corner_rectex_chn->corner_rect.rect.y = OFFSET_NUM2 * (handle - CORNER_RECTEX_MIN_HANDLE) + OFFSET_NUM3;
}

td_s32 sample_region_set_corner_rect_chn_attr(td_s32 handle_num,
    ot_rgn_chn_attr *chn_attr, ot_mpp_chn *mpp_chn, region_op_flag op_flag)
{
    td_s32 i;
    td_s32 ret;

    rgn_check_handle_num_return(handle_num);

    chn_attr->type = OT_RGN_CORNER_RECT;

    for (i = CORNER_RECT_MIN_HANDLE; i < CORNER_RECT_MIN_HANDLE + handle_num; i++) {
        sample_region_get_corner_rect_chn_attr(i, &chn_attr->attr.corner_rect_chn);
        if (i - CORNER_RECT_MIN_HANDLE < 1) { /* 1:corner type number */
            chn_attr->attr.corner_rect_chn.corner_rect_attr.corner_rect_type = OT_CORNER_RECT_TYPE_CORNER;
        } else {
            chn_attr->attr.corner_rect_chn.corner_rect_attr.corner_rect_type = OT_CORNER_RECT_TYPE_FULL_LINE;
        }
        ret = sample_region_attach(i, mpp_chn, chn_attr, op_flag);
        if (ret != TD_SUCCESS) {
            sample_print("sample_region_attach failed!\n");
            sample_comm_region_detach(i - CORNER_RECT_MIN_HANDLE + 1, OT_RGN_CORNER_RECT, mpp_chn, op_flag);
            return ret;
        }
    }
    return TD_SUCCESS;
}

td_s32 sample_region_set_corner_rectex_chn_attr(td_s32 handle_num,
    ot_rgn_chn_attr *chn_attr, ot_mpp_chn *mpp_chn, region_op_flag op_flag)
{
    td_s32 i;
    td_s32 ret;

    rgn_check_handle_num_return(handle_num);

    chn_attr->type = OT_RGN_CORNER_RECTEX;

    for (i = CORNER_RECTEX_MIN_HANDLE; i < CORNER_RECTEX_MIN_HANDLE + handle_num; i++) {
        sample_region_get_corner_rectex_chn_attr(i, &chn_attr->attr.corner_rectex_chn);
        if (i - CORNER_RECTEX_MIN_HANDLE < 1) { /* 1:corner type number */
            chn_attr->attr.corner_rectex_chn.corner_rect_attr.corner_rect_type = OT_CORNER_RECT_TYPE_CORNER;
        } else {
            chn_attr->attr.corner_rectex_chn.corner_rect_attr.corner_rect_type = OT_CORNER_RECT_TYPE_FULL_LINE;
        }
        ret = sample_region_attach(i, mpp_chn, chn_attr, op_flag);
        if (ret != TD_SUCCESS) {
            sample_print("sample_region_attach failed!\n");
            sample_comm_region_detach(i - CORNER_RECTEX_MIN_HANDLE + 1, OT_RGN_CORNER_RECTEX, mpp_chn, op_flag);
            return ret;
        }
    }
    return TD_SUCCESS;
}

static td_void sample_region_get_line_chn_attr(td_s32 handle, ot_rgn_lineex_chn_attr *line_chn)
{
    rgn_check_handle_min_ret(handle, LINEEX_MIN_HANDLE);

    line_chn->color = RGN_RGB888_BLUE;
    line_chn->points[0].x = OFFSET_NUM0 + OFFSET_NUM2 * (handle - LINEEX_MIN_HANDLE);
    line_chn->points[0].y = OFFSET_NUM0 + OFFSET_NUM0 * (handle - LINEEX_MIN_HANDLE);
    line_chn->points[1].x = OFFSET_NUM3 + OFFSET_NUM0 * (handle - LINEEX_MIN_HANDLE);
    line_chn->points[1].y = OFFSET_NUM3 + OFFSET_NUM2 * (handle - LINEEX_MIN_HANDLE);
    /* align 2 */
    line_chn->thick = (td_u32)(((handle - LINEEX_MIN_HANDLE + OT_RGN_LINEEX_MIN_THICK) * 2) % OT_RGN_LINEEX_MAX_THICK);
}

static td_s32 sample_region_set_lineex_chn_attr(td_s32 handle_num,
    ot_rgn_chn_attr *chn_attr, ot_mpp_chn *mpp_chn, region_op_flag op_flag)
{
    td_s32 i;
    td_s32 ret;

    rgn_check_handle_num_return(handle_num);

    chn_attr->type = OT_RGN_LINEEX;

    for (i = LINEEX_MIN_HANDLE; i < LINEEX_MIN_HANDLE + handle_num; i++) {
        sample_region_get_line_chn_attr(i, &chn_attr->attr.lineex_chn);
        ret = sample_region_attach(i, mpp_chn, chn_attr, op_flag);
        if (ret != TD_SUCCESS) {
            sample_print("sample_region_attach failed!\n");
            sample_comm_region_detach(i - LINEEX_MIN_HANDLE + 1, OT_RGN_LINEEX, mpp_chn, op_flag);
            return ret;
        }
    }
    return TD_SUCCESS;
}

td_s32 sample_region_set_chn_attr(td_s32 handle_num, ot_rgn_type type, ot_rgn_chn_attr *chn_attr,
    ot_mpp_chn *chn, region_op_flag op_flag)
{
    td_s32 ret = TD_SUCCESS;
    switch (type) {
        case OT_RGN_OVERLAY:
            ret = sample_region_set_overlay_chn_attr(handle_num, chn_attr, chn, op_flag);
            break;
        case OT_RGN_COVER:
            ret = sample_region_set_cover_chn_attr(handle_num, chn_attr, chn, op_flag);
            break;
        case OT_RGN_COVEREX:
            ret = sample_region_set_coverex_chn_attr(handle_num, chn_attr, chn, op_flag);
            break;
        case OT_RGN_LINEEX:
            ret = sample_region_set_lineex_chn_attr(handle_num, chn_attr, chn, op_flag);
            break;
        case OT_RGN_CORNER_RECT:
            ret = sample_region_set_corner_rect_chn_attr(handle_num, chn_attr, chn, op_flag);
            break;
        case OT_RGN_CORNER_RECTEX:
            ret = sample_region_set_corner_rectex_chn_attr(handle_num, chn_attr, chn, op_flag);
            break;
        default:
            break;
    }
    return ret;
}

td_s32 sample_comm_region_attach(td_s32 handle_num, ot_rgn_type type, ot_mpp_chn *mpp_chn,
    region_op_flag op_flag)
{
    td_s32 ret;
    ot_rgn_chn_attr chn_attr;
    ret = sample_comm_region_attach_check(handle_num, type, mpp_chn);
    if (ret != TD_SUCCESS) {
        return ret;
    }
    /* set the chn config */
    chn_attr.is_show = TD_TRUE;
    ret = sample_region_set_chn_attr(handle_num, type, &chn_attr, mpp_chn, op_flag);
    if (ret != TD_SUCCESS) {
        sample_print("sample_region_attach failed!\n");
    }
    return ret;
}

td_s32 sample_comm_check_min(td_s32 min_handle)
{
    if ((min_handle <= OT_INVALID_HANDLE) || (min_handle > OT_RGN_HANDLE_MAX)) {
        return TD_FAILURE;
    }

    return TD_SUCCESS;
}

td_s32 sample_comm_region_detach(td_s32 handle_num, ot_rgn_type type, ot_mpp_chn *mpp_chn,
    region_op_flag op_flag)
{
    td_s32 i;
    td_s32 ret;
    td_s32 min_handle;

    if (handle_num <= 0 || handle_num > 16) { /* 16:max region num */
        sample_print("handle_num is illegal %d!\n", handle_num);
        return TD_FAILURE;
    }
    if (type < 0 || type >= OT_RGN_BUTT) {
        sample_print("type is illegal %d!\n", type);
        return TD_FAILURE;
    }
    if (mpp_chn == TD_NULL) {
        sample_print("mpp_chn is NULL !\n");
        return TD_FAILURE;
    }
    min_handle = sample_comm_region_get_min_handle(type);
    if (sample_comm_check_min(min_handle) != TD_SUCCESS) {
        sample_print("min_handle(%d) should be in [0, %d).\n", min_handle, OT_RGN_HANDLE_MAX);
        return TD_FAILURE;
    }
    for (i = min_handle; i < min_handle + handle_num; i++) {
        ret = sample_region_detach(i, mpp_chn, op_flag);
        if (ret != TD_SUCCESS) {
            sample_print("sample_region_detach failed! handle:%d\n", i);
        }
    }
    return TD_SUCCESS;
}

td_s32 sample_comm_region_set_bit_map(ot_rgn_handle handle, const td_char *bmp_path)
{
    td_s32 ret;
    rgn_load_bmp_info load_bmp_info = {0};

    load_bmp_info.fil = TD_FALSE;
    load_bmp_info.fil_color = 0;
    load_bmp_info.pixel_fmt = sample_comm_region_get_pixel_format_by_handle(handle);
    if (load_bmp_info.pixel_fmt == OT_PIXEL_FORMAT_BUTT) {
        sample_print("handle(%u) pixel format error!\n", handle);
        return TD_FAILURE;
    }
    load_bmp_info.filename = bmp_path;
    if (load_bmp_info.filename == TD_NULL) {
        sample_print("handle(%u) file path is null!\n", handle);
        return TD_FAILURE;
    }

    ret = region_mst_load_bmp(&load_bmp_info);
    if (ret != TD_SUCCESS) {
        sample_print("region_mst_load_bmp failed!handle\n");
        return ret;
    }
    ret = sample_region_set_bit_map(handle, &load_bmp_info.bmp);
    if (ret != TD_SUCCESS) {
        sample_print("sample_region_set_bit_map failed!handle:%u\n", handle);
    }
    free(load_bmp_info.bmp.data);
    return ret;
}

static td_void sample_region_reset_line_color(sample_osd_line *line, ot_rgn_handle handle)
{
    ot_pixel_format pixel_format;
    pixel_format = sample_comm_region_get_pixel_format_by_handle(handle);
    if (pixel_format == OT_PIXEL_FORMAT_ARGB_1555 || pixel_format == OT_PIXEL_FORMAT_ARGB_4444) {
        line[0].color = 0xf777; /* 0: osd line index */
        line[1].color = 0xffff; /* 1: osd line index */
        line[2].color = 0xff0f; /* 2: osd line index */
        line[3].color = 0xff00; /* 3: osd line index */
        line[4].color = 0xffff; /* 4: osd line index */
        line[5].color = 0xf77f; /* 5: osd line index */
        line[6].color = 0xf7f7; /* 6: osd line index */
        line[7].color = 0xf7ff; /* 7: osd line index */
    }
}

td_s32 sample_comm_region_drawline_get_canvas(ot_rgn_handle handle, sample_osd_line *line, td_u32 line_num)
{
    td_s32 ret;
    ot_pixel_format pixel_format;
    ot_rgn_canvas_info canvas_info = {0};
    td_u32 i;
    sample_drawline_param drawline_param = {0};
    td_u32 mem_len;

    sample_region_reset_line_color(line, handle);

    ret = ss_mpi_rgn_get_canvas_info(handle, &canvas_info);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_rgn_get_canvas_info failed with %#x!\n", ret);
        return TD_FAILURE;
    }

    pixel_format = canvas_info.pixel_format;
    mem_len = canvas_info.stride * canvas_info.size.height;
    if (pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT2_2X2 || pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT4_2X2) {
        mem_len /= 2; /* 2: 2x2: height need ÷2 */
    } else if (pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT2_4X4 || pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT4_4X4) {
        mem_len /= 4; /* 4: 4x4: height need ÷4 */
    }
    (td_void)memset_s(canvas_info.virt_addr, mem_len, 0, mem_len);

    drawline_param.pixel_format = pixel_format;
    drawline_param.data = canvas_info.virt_addr;
    (td_void)memcpy_s(&drawline_param.size, sizeof(ot_size), &canvas_info.size, sizeof(ot_size));
    drawline_param.stride = canvas_info.stride;
    drawline_param.is_set_bmp = TD_FALSE;
    for (i = 0; i < line_num; ++i) {
        drawline_param.line = &line[i];
        ret = sample_comm_osd_drawline(&drawline_param);
        if (ret != TD_SUCCESS) {
            sample_print("sample_comm_osd_drawline failed! point1(%d, %d) point2(%d, %d)\n",
                line[i].point1.x, line[i].point1.y, line[i].point2.x, line[i].point2.y);
            break;
        }
    }

    ret = ss_mpi_rgn_update_canvas(handle);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_rgn_update_canvas failed with %#x!\n", ret);
        return TD_FAILURE;
    }

    return TD_SUCCESS;
}

static td_s32 sample_comm_region_drawline_get_stride_height(ot_bmp *bmp, td_u32 *stride, td_u32 *h)
{
    ot_pixel_format pixel_format = bmp->pixel_format;
    td_u32 width = bmp->width;
    td_u32 height = bmp->height;

    if (pixel_format == OT_PIXEL_FORMAT_ARGB_1555 || pixel_format == OT_PIXEL_FORMAT_ARGB_4444) {
        *stride = width * RGN_NUM_2;
        *h = height;
    } else if (pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT2) {
        *stride = OT_DIV_UP(width, RGN_NUM_4);
        *h = height;
    } else if (pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT2_2X2) {
        *stride = OT_DIV_UP(OT_DIV_UP(width, RGN_NUM_2), RGN_NUM_4);
        *h = OT_DIV_UP(height, RGN_NUM_2);
    } else if (pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT2_4X4) {
        *stride = OT_DIV_UP(OT_DIV_UP(width, RGN_NUM_4), RGN_NUM_4);
        *h = OT_DIV_UP(height, RGN_NUM_4);
    } else if (pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT4) {
        *stride = OT_ALIGN_UP(width, RGN_NUM_2) / RGN_NUM_2;
        *h = height;
    } else if (pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT4_2X2) {
        *stride = OT_DIV_UP(OT_DIV_UP(width, RGN_NUM_2), RGN_NUM_2);
        *h = OT_DIV_UP(height, RGN_NUM_2);
    } else if (pixel_format == OT_PIXEL_FORMAT_ARGB_CLUT4_4X4) {
        *stride = OT_DIV_UP(OT_DIV_UP(width, RGN_NUM_4), RGN_NUM_2);
        *h = OT_DIV_UP(height, RGN_NUM_4);
    } else {
        return TD_FAILURE;
    }
    return TD_SUCCESS;
}

td_s32 sample_comm_region_drawline_set_bit_map(ot_rgn_handle handle, sample_osd_line *line, td_u32 line_num)
{
    td_s32 ret;
    ot_bmp bmp;
    td_u32 i;
    td_u32 stride, height;
    sample_drawline_param drawline_param = {0};

    sample_region_reset_line_color(line, handle);
    bmp.pixel_format = sample_comm_region_get_pixel_format_by_handle(handle);
    if (bmp.pixel_format == OT_PIXEL_FORMAT_BUTT) {
        sample_print("handle(%u) pixel format error!\n", handle);
        return TD_FAILURE;
    }
    bmp.width = sample_region_get_overlay_default_width(bmp.pixel_format);
    bmp.height = sample_region_get_overlay_default_height(bmp.pixel_format);

    if (sample_comm_region_drawline_get_stride_height(&bmp, &stride, &height) != TD_SUCCESS) {
        sample_print("handle(%u) pixel format(%d) error!\n", handle, bmp.pixel_format);
        return TD_FAILURE;
    }

    bmp.data = malloc(stride * height);
    if (bmp.data == TD_NULL) {
        sample_print("malloc bmp failed!\n");
        return TD_FAILURE;
    }
    (td_void)memset_s(bmp.data, stride * height, 0, stride * height);

    drawline_param.pixel_format = bmp.pixel_format;
    drawline_param.data = bmp.data;
    drawline_param.size.width = bmp.width;
    drawline_param.size.height = bmp.height;
    drawline_param.is_set_bmp = TD_TRUE;

    for (i = 0; i < line_num; i++) {
        drawline_param.line = &line[i];
        ret = sample_comm_osd_drawline(&drawline_param);
        if (ret != TD_SUCCESS) {
            sample_print("sample_comm_osd_drawline failed! point1(%d, %d) point2(%d, %d)\n",
                line[i].point1.x, line[i].point1.y, line[i].point2.x, line[i].point2.y);
            free(bmp.data);
            return ret;
        }
    }

    ret = ss_mpi_rgn_set_bmp(handle, &bmp);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_rgn_set_bmp failed!handle:%u\n", handle);
    }
    free(bmp.data);
    return ret;
}

td_s32 sample_comm_region_get_up_canvas(ot_rgn_handle handle, const td_char *bmp_path)
{
    td_s32 ret;
    ot_size size;
    ot_rgn_canvas_info canvas_info;
    rgn_load_bmp_info load_bmp_info = {0};

    ret = ss_mpi_rgn_get_canvas_info(handle, &canvas_info);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_rgn_get_canvas_info failed with %#x!\n", ret);
        return TD_FAILURE;
    }

    size.width = canvas_info.size.width;
    size.height = canvas_info.size.height;
    load_bmp_info.bmp.data = canvas_info.virt_addr;
    load_bmp_info.pixel_fmt = canvas_info.pixel_format;
    load_bmp_info.fil = TD_FALSE;
    load_bmp_info.fil_color = 0;
    load_bmp_info.filename = bmp_path;
    ret = region_mst_update_canvas(&load_bmp_info, &size, canvas_info.stride);
    if (ret != TD_SUCCESS) {
        sample_print("region_mst_update_canvas failed with %#x!\n", ret);
        return TD_FAILURE;
    }

    ret = ss_mpi_rgn_update_canvas(handle);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_rgn_update_canvas failed with %#x!\n", ret);
        return TD_FAILURE;
    }

    return ret;
}

td_void sample_comm_set_osd_drawline(td_bool osd_drawline)
{
    g_venc_osd_drawline = osd_drawline;
}