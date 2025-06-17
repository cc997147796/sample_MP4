/******************************************************************************
 RTSP SERVER.

Copyright (C), 2023-2033, KUDIANWANJIA Tech. Co., Ltd.
******************************************************************************
	Modification:  2023-3 Created By KODO
******************************************************************************/
#ifndef __RTSP_SERVER_H__
#define __RTSP_SERVER_H__

#include "sample_comm.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* End of #ifdef __cplusplus */

#if RTSP_ENABLE

#include "list.h"

#if !defined(WIN32)
	#define __PACKED__ __attribute__ ((__packed__))
#else
	#define __PACKED__
#endif

typedef enum
{
	RTSP_VIDEO=0,
	RTSP_VIDEOSUB=1,
	RTSP_AUDIO=2,
	RTSP_YUV422=3,
	RTSP_RGB=4,
	RTSP_VIDEOPS=5,
	RTSP_VIDEOSUBPS=6
}enRTSP_MonBlockType;

struct _RTP_FIXED_HEADER
{
	/**//* byte 0 */
	unsigned char csrc_len:4;       /**//* expect 0 */
	unsigned char extension:1;      /**//* expect 1, see RTP_OP below */
	unsigned char padding:1;        /**//* expect 0 */
	unsigned char version:2;        /**//* expect 2 */
	/**//* byte 1 */
	unsigned char payload:7;        /**//* RTP_PAYLOAD_RTSP */
	unsigned char marker:1;         /**//* expect 1 */
	/**//* bytes 2, 3 */
	unsigned short seq_no;
	/**//* bytes 4-7 */
	unsigned  long timestamp;
	/**//* bytes 8-11 */
	unsigned long ssrc;             /**//* stream number is used here. */
} __PACKED__;

typedef struct _RTP_FIXED_HEADER RTP_FIXED_HEADER;

struct _NALU_HEADER
{
	//byte 0
	unsigned char TYPE:5;
	unsigned char NRI:2;
	unsigned char F:1;
}__PACKED__; /**//* 1 BYTES */

typedef struct _NALU_HEADER NALU_HEADER;

struct _FU_INDICATOR
{
	//byte 0
	unsigned char TYPE:5;
	unsigned char NRI:2;
	unsigned char F:1;
}__PACKED__; /**//* 1 BYTES */

typedef struct _FU_INDICATOR FU_INDICATOR;

struct _FU_HEADER
{
	//byte 0
	unsigned char TYPE:5;
	unsigned char R:1;
	unsigned char E:1;
	unsigned char S:1;
} __PACKED__; /**//* 1 BYTES */
typedef struct _FU_HEADER FU_HEADER;

struct _AU_HEADER
{
	//byte 0, 1
	unsigned short au_len;
	//byte 2,3
	unsigned  short frm_len:13;
	unsigned char au_index:3;
} __PACKED__; /**//* 1 BYTES */
typedef struct _AU_HEADER AU_HEADER;

extern void RtspServer_init(void);
extern void RtspServer_exit(void);

int AddFrameToRtspBuf(int nChanNum,enRTSP_MonBlockType eType, char * pData, unsigned int nSize, unsigned int nVidFrmNum,int bIFrm);

extern td_s32 SAMPLE_COMM_VENC_Sentjin(ot_venc_stream *pstStream);
extern td_s32 saveStream(ot_venc_stream *pstStream);
extern td_void* vdRTPSendThread(td_void *p);

#define nalu_sent_len           1400
//#define nalu_sent_len         1400
#define RTP_H264                96
#define RTP_AUDIO               97
#define MAX_RTSP_CLIENT         1
#define RTSP_SERVER_PORT        8554
#define RTSP_RECV_SIZE          1024
#define RTSP_MAX_VID            (640*1024)
#define RTSP_MAX_AUD            (15*1024)

#define AU_HEADER_SIZE          4
#define PARAM_STRING_MAX        100

typedef unsigned int u_int32_t;
typedef unsigned short u_int16_t;
typedef unsigned char u_int8_t;
typedef u_int16_t portNumBits;
typedef u_int32_t netAddressBits;
typedef long long _int64;
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif
#define AUDIO_RATE    8000
#define PACKET_BUFFER_END            (unsigned int)0x00000000

typedef struct
{
	int startblock;         //
	int endblock;           //
	int BlockFileNum;       //
}IDXFILEHEAD_INFO;          //

typedef struct
{
	_int64 starttime;       //
	_int64 endtime;         //
	int startblock;         //
	int endblock;           //
	int stampnum;           //
}IDXFILEBLOCK_INFO;         //

typedef struct
{
	int blockindex;         //
	int pos;                //
	_int64 time;            //
}IDXSTAMP_INFO;             //

typedef struct
{
	char filename[150];     //
	int pos;                //
	_int64 time;            //
}FILESTAMP_INFO;            //

typedef struct
{
	char channelid[9];
	_int64 starttime;       //
	_int64 endtime;         //
	_int64 session;
	int type;               //
	int encodetype;         //
}FIND_INFO;                 //

typedef enum
{
	RTP_UDP,
	RTP_TCP,
	RAW_UDP
}StreamingMode;

extern char g_rtp_playload[20]; //
extern int   g_audio_rate;      //

typedef enum
{
	RTSP_IDLE = 0,
	RTSP_CONNECTED = 1,
	RTSP_SENDING = 2,
}RTSP_STATUS;

typedef struct
{
	int  nVidLen;
	int  nAudLen;
	int bIsIFrm;
	int bWaitIFrm;
	int bIsFree;
	char vidBuf[RTSP_MAX_VID];
	char audBuf[RTSP_MAX_AUD];
}RTSP_PACK;

typedef struct
{
	int index;
	int socket;
	int reqchn;
	int seqnum;
	int seqnum2;
	unsigned int tsvid;
	unsigned int tsaud;
	int status;
	int sessionid;
	int rtpport[2];
	int rtcpport;
	char IP[20];
	char urlPre[PARAM_STRING_MAX];
}RTSP_CLIENT;

typedef struct
{
	int  vidLen;
	int  audLen;
	int  nFrameID;
	char vidBuf[RTSP_MAX_VID];
	char audBuf[RTSP_MAX_AUD];
}FRAME_PACK;

typedef struct
{
	int startcodeprefix_len;      //! 4 for parameter sets and first slice in picture, 3 for everything else (suggested)
	unsigned len;                 //! Length of the NAL unit (Excluding the start code, which does not belong to the NALU)
	unsigned max_size;            //! Nal Unit Buffer size
	int forbidden_bit;            //! should be always FALSE
	int nal_reference_idc;        //! NALU_PRIORITY_xxxx
	int nal_unit_type;            //! NALU_TYPE_xxxx
	char *buf;                    //! contains the first byte followed by the EBSP
	unsigned short lost_packets;  //! true, if packet loss is detected
} NALU_t;

extern int udpfd;
extern RTSP_CLIENT g_rtspClients[MAX_RTSP_CLIENT];

typedef struct _rtpbuf
{
	struct list_head list;
	td_s32 len;
	char * buf;
}RTPbuf_s;
extern struct list_head RTPbuf_head;

#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* End of #ifdef __cplusplus */

#endif /* End of #ifndef __SAMPLE_COMMON_H__ */
