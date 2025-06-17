/******************************************************************************
 RTSP SERVER.
Copyright (C), 2023-2033, KUDIANWANJIA Tech. Co., Ltd.
******************************************************************************
	Modification:  2023-3 Created By KODO
******************************************************************************/
#ifdef __cplusplus
#if __cplusplus
extern "C"{
#endif
#endif /* End of #ifdef __cplusplus */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#define RTSP_ENABLE 0

#if RTSP_ENABLE

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>

#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <netinet/if_ether.h>
#include <net/if.h>

#include <linux/if_ether.h>
#include <linux/sockios.h>
#include <netinet/in.h> 
#include <arpa/inet.h> 

#include <RtspServer.h>

RTP_FIXED_HEADER	*rtp_hdr;
NALU_HEADER			*nalu_hdr;
FU_INDICATOR		*fu_ind;
FU_HEADER			*fu_hdr;
RTSP_CLIENT			g_rtspClients[MAX_RTSP_CLIENT];

int g_nSendDataChn = -1;
pthread_mutex_t g_mutex;		//互斥锁创建
pthread_cond_t  g_cond;			//线程同步
pthread_mutex_t g_sendmutex;	//互斥锁创建

pthread_t g_SendDataThreadId = 0;
char g_rtp_playload[20];
int  g_audio_rate = 8000;
int  g_nframerate;

int exitok = 0;
int udpfd;
int count=0;

static pthread_t gs_RtpPid;
struct list_head RTPbuf_head = LIST_HEAD_INIT(RTPbuf_head);//创建并初始化链表

static char const* dateHeader()
{
	static char buf[200];
#if !defined(_WIN32_WCE)
	time_t tt = time(NULL);
	strftime(buf, sizeof buf, "Date: %a, %b %d %Y %H:%M:%S GMT\r\n", gmtime(&tt));
#endif
	return buf;
}

static char* GetLocalIP(int sock)
{
	struct ifreq ifreq;
	struct sockaddr_in *sin;
	char * LocalIP = malloc(20);

	strcpy(ifreq.ifr_name,"wlan");
	if (!(ioctl (sock, SIOCGIFADDR,&ifreq)))
	{
		sin = (struct sockaddr_in *)&ifreq.ifr_addr;
		sin->sin_family = AF_INET;
		strcpy(LocalIP,inet_ntoa(sin->sin_addr));
		//inet_ntop(AF_INET, &sin->sin_addr,LocalIP, 16);
	}
	printf("-------------------------%s\n",LocalIP);
	return LocalIP;
}

char* strDupSize(char const* str)
{
	if (str == NULL)
		return NULL;
	size_t len = strlen(str) + 1;
	char* copy = malloc(len);
	return copy;
}

/* 解析 RTSP 客户端请求
*
* 格式：
* 方法Method		URI			RTSP版本	CR+LF(回车换行)
* 消息头			CR			LF
* ......
* CR(回车)			LF(换行)
* 消息体			CR			LF
*
* RTSP 关键字段说明：
* ① 关键字OPTION：得到服务器提供的可用方法（OPTION、DESCRIBE、SETUP、TEARDOWN、PLAY、PAUSE、SCALE、GET_PARAMETER、SET_PARAMETER）
* ② 关键字DESCRIBE：请求流的 SDP 信息。注解：此处需要了解 H264 Law Data 如何生成 SPS PPS 信息。
* ③ 关键字SETUP：客户端提醒服务器建立会话，并建立传输模式。注解：此处确定了 RTP 传输交互式采用 TCP（面向连接）还是 UDP（无连接）模式。
* ④ 关键字PLAY：客户端发送播放请求。注解：此处引入 RTP 协议及 RTCP 协议。
* ⑤ 关键字PAUSE：播放暂停请求。注解：此关键字经常用在录像回放当中，实时视频流几乎用不到。
* ⑥ 关键字TEARDOWN:客户端发送关闭请求
* ⑦ 关键字GET_PARAMETER:从服务器获取参数，目前主要获取时间参数（可扩展）
* ⑧ 关键字SET_PARAMETER：给指定的 URL 或者流设置参数（可扩展）
*
* 参数：reqStr - 要解析的字符串
*		reqStrSize - 字符串大小
*		resultCmdName - 解析出来的方法的存储指针
*		resultCmdNameMaxSize - resultCmdName 的长度
*		resultURLPreSuffix - URL 的前缀存储指针
*		resultURLPreSuffixMaxSize - URLPreSuffix 的长度
*		resultURLSuffix - URL 的存储指针
*		resultURLSuffixMaxSize - URLSuffix 的长度
*		resultCSeq - CSeq 的存储指针
*		resultCSeqMaxSize - CSeq 大小
*/
int ParseRequestString(char const* reqStr,
			unsigned reqStrSize,
			char* resultCmdName,
			unsigned resultCmdNameMaxSize,
			char* resultURLPreSuffix,
			unsigned resultURLPreSuffixMaxSize,
			char* resultURLSuffix,
			unsigned resultURLSuffixMaxSize,
			char* resultCSeq,
			unsigned resultCSeqMaxSize)
{
	// This parser is currently rather dumb; it should be made smarter #####
	// Read everything up to the first space as the command name:
	int parseSucceeded = FALSE;
	unsigned i;

	for (i = 0; i < resultCmdNameMaxSize-1 && i < reqStrSize; ++i)
	{
		char c = reqStr[i];
		if (c == ' ' || c == '\t') {
			parseSucceeded = TRUE;
			break;
		}
		resultCmdName[i] = c;
	}
	resultCmdName[i] = '\0';
	if (!parseSucceeded) return FALSE;

	// Skip over the prefix of any "rtsp://" or "rtsp:/" URL that follows:
	unsigned j = i+1;
	while (j < reqStrSize && (reqStr[j] == ' ' || reqStr[j] == '\t')) ++j; // skip over any additional white space
	for (j = i+1; j < reqStrSize-8; ++j)
	{
		if ((reqStr[j] == 'r' || reqStr[j] == 'R')
			&& (reqStr[j+1] == 't' || reqStr[j+1] == 'T')
			&& (reqStr[j+2] == 's' || reqStr[j+2] == 'S')
			&& (reqStr[j+3] == 'p' || reqStr[j+3] == 'P')
			&& reqStr[j+4] == ':' && reqStr[j+5] == '/')
		{
			j += 6;
			if (reqStr[j] == '/') {
				// This is a "rtsp://" URL; skip over the host:port part that follows:
				++j;
				while (j < reqStrSize && reqStr[j] != '/' && reqStr[j] != ' ') ++j;
			} else {
				// This is a "rtsp:/" URL; back up to the "/":
				--j;
			}
			i = j;
			break;
		}
	}

	// Look for the URL suffix (before the following "RTSP/"):
	parseSucceeded = FALSE;
	unsigned k;
	for (k = i+1; k < reqStrSize-5; ++k)
	{
		if (reqStr[k] == 'R' && reqStr[k+1] == 'T' &&
			reqStr[k+2] == 'S' && reqStr[k+3] == 'P' && reqStr[k+4] == '/')
		{
			while (--k >= i && reqStr[k] == ' ') {} // go back over all spaces before "RTSP/"
			unsigned k1 = k;
			while (k1 > i && reqStr[k1] != '/' && reqStr[k1] != ' ') --k1;
			// the URL suffix comes from [k1+1,k]

			// Copy "resultURLSuffix":
			if (k - k1 + 1 > resultURLSuffixMaxSize) return FALSE; // there's no room
			unsigned n = 0, k2 = k1+1;
			while (k2 <= k) resultURLSuffix[n++] = reqStr[k2++];
			resultURLSuffix[n] = '\0';

			// Also look for the URL 'pre-suffix' before this:
			unsigned k3 = --k1;
			while (k3 > i && reqStr[k3] != '/' && reqStr[k3] != ' ') --k3;
			// the URL pre-suffix comes from [k3+1,k1]

			// Copy "resultURLPreSuffix":
			if (k1 - k3 + 1 > resultURLPreSuffixMaxSize) return FALSE; // there's no room
			n = 0; k2 = k3+1;
			while (k2 <= k1) resultURLPreSuffix[n++] = reqStr[k2++];
			resultURLPreSuffix[n] = '\0';

			i = k + 7; // to go past " RTSP/"
			parseSucceeded = TRUE;
			break;
		}
	}
	if (!parseSucceeded) return FALSE;

	// Look for "CSeq:", skip whitespace,
	// then read everything up to the next \r or \n as 'CSeq':
	parseSucceeded = FALSE;
	for (j = i; j < reqStrSize-5; ++j)
	{
		if (reqStr[j] == 'C' && reqStr[j+1] == 'S' && reqStr[j+2] == 'e' &&
			reqStr[j+3] == 'q' && reqStr[j+4] == ':')
		{
			j += 5;
			unsigned n;
			while (j < reqStrSize && (reqStr[j] ==  ' ' || reqStr[j] == '\t')) ++j;
			for (n = 0; n < resultCSeqMaxSize-1 && j < reqStrSize; ++n,++j)
			{
				char c = reqStr[j];
				if (c == '\r' || c == '\n') {
					parseSucceeded = TRUE;
					break;
				}
				resultCSeq[n] = c;
			}
			resultCSeq[n] = '\0';
			break;
		}
	}
	if (!parseSucceeded) return FALSE;
	return TRUE;
}

/* 应答 RTSP 客户端请求
* 格式：
* RTSP版本			状态码		解释	CR+LF(回车换行)
* 消息头			CR			LF
* ......
* CR(回车)			LF(换行)
* 消息体			CR			LF
*/

/* Describe 应答 - 得到服务器提供的可用方法 */
int OptionAnswer(char *cseq, int sock)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sPublic: %s\r\n\r\n",
						cseq,dateHeader(),"OPTIONS,DESCRIBE,SETUP,PLAY,PAUSE,TEARDOWN");

		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s\n",buf);
		}
		return TRUE;
	}
	return FALSE;
}

/* Describe 应答 - 请求流的 SDP 信息 */
int DescribeAnswer(char *cseq,int sock,char * urlSuffix,char* recvbuf)
{
	if (sock != 0)
	{
		char sdpMsg[1024];
		char buf[2048];
		memset(buf,0,2048);
		memset(sdpMsg,0,1024);
		char*localip;
		localip = GetLocalIP(sock);

		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n",cseq);
		pTemp += sprintf(pTemp,"%s",dateHeader());
		pTemp += sprintf(pTemp,"Content-Type: application/sdp\r\n");

		char *pTemp2 = sdpMsg;
		pTemp2 += sprintf(pTemp2,"v=0\r\n");
		pTemp2 += sprintf(pTemp2,"o=StreamingServer 3331435948 1116907222000 IN IP4 %s\r\n",localip);
		pTemp2 += sprintf(pTemp2,"s=H.264\r\n");
		pTemp2 += sprintf(pTemp2,"c=IN IP4 0.0.0.0\r\n");
		pTemp2 += sprintf(pTemp2,"t=0 0\r\n");
		pTemp2 += sprintf(pTemp2,"a=control:*\r\n");

		/*H264 TrackID=0 RTP_PT 96*/
		pTemp2 += sprintf(pTemp2,"m=video 0 RTP/AVP 96\r\n");
		pTemp2 += sprintf(pTemp2,"a=control:trackID=0\r\n");
		pTemp2 += sprintf(pTemp2,"a=rtpmap:96 H264/90000\r\n");
		pTemp2 += sprintf(pTemp2,"a=fmtp:96 packetization-mode=1; sprop-parameter-sets=%s\r\n", "AAABBCCC");
#if 1
		/*G726*/
		pTemp2 += sprintf(pTemp2,"m=audio 0 RTP/AVP 97\r\n");
		pTemp2 += sprintf(pTemp2,"a=control:trackID=1\r\n");
		if(strcmp(g_rtp_playload,"AAC")==0)
		{
			pTemp2 += sprintf(pTemp2,"a=rtpmap:97 MPEG4-GENERIC/%d/2\r\n",16000);
			pTemp2 += sprintf(pTemp2,"a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1410\r\n");
		}
		else
		{
			pTemp2 += sprintf(pTemp2,"a=rtpmap:97 G726-32/%d/1\r\n",8000);
			pTemp2 += sprintf(pTemp2,"a=fmtp:97 packetization-mode=1\r\n");
		}
#endif
		pTemp += sprintf(pTemp,"Content-length: %d\r\n", strlen(sdpMsg));
		pTemp += sprintf(pTemp,"Content-Base: rtsp://%s/%s/\r\n\r\n",localip,urlSuffix);

		//printf("mem ready\n");
		strcat(pTemp, sdpMsg);
		free(localip);
		//printf("Describe ready sent\n");
		int re = send(sock, buf, strlen(buf),0);
		if(re <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s\n",buf);
		}
	}
	return TRUE;
}

void ParseTransportHeader(char const* buf,
						StreamingMode* streamingMode,
						char**streamingModeString,
						char**destinationAddressStr,
						u_int8_t* destinationTTL,
						portNumBits* clientRTPPortNum, // if UDP
						portNumBits* clientRTCPPortNum,// if UDP
						unsigned char* rtpChannelId,	// if TCP
						unsigned char* rtcpChannelId	// if TCP
						)
{
	// Initialize the result parameters to default values:
	*streamingMode = RTP_UDP;
	*streamingModeString = NULL;
	*destinationAddressStr = NULL;
	*destinationTTL = 255;
	*clientRTPPortNum = 0;
	*clientRTCPPortNum = 1;
	*rtpChannelId = *rtcpChannelId = 0xFF;

	portNumBits p1, p2;
	unsigned ttl, rtpCid, rtcpCid;

	// First, find "Transport:"
	while (1) {
		if (*buf == '\0') return; // not found
		if (strncasecmp(buf, "Transport: ", 11) == 0) break;
		++buf;
	}

	// Then, run through each of the fields, looking for ones we handle:
	char const* fields = buf + 11;
	char* field = strDupSize(fields);
	while (sscanf(fields, "%[^;]", field) == 1) {
		if (strcmp(field, "RTP/AVP/TCP") == 0) {
			*streamingMode = RTP_TCP;
		} else if (strcmp(field, "RAW/RAW/UDP") == 0 ||
			strcmp(field, "MP2T/H2221/UDP") == 0) {
			*streamingMode = RAW_UDP;
			//*streamingModeString = strDup(field);
		} else if (strncasecmp(field, "destination=", 12) == 0)
		{
			//delete[] destinationAddressStr;
			free(destinationAddressStr);
			//destinationAddressStr = strDup(field+12);
		} else if (sscanf(field, "ttl%u", &ttl) == 1) {
			destinationTTL = (u_int8_t)ttl;
		} else if (sscanf(field, "client_port=%hu-%hu", &p1, &p2) == 2) {
			*clientRTPPortNum = p1;
			*clientRTCPPortNum = p2;
		} else if (sscanf(field, "client_port=%hu", &p1) == 1) {
			*clientRTPPortNum = p1;
			*clientRTCPPortNum = streamingMode == RAW_UDP ? 0 : p1 + 1;
		} else if (sscanf(field, "interleaved=%u-%u", &rtpCid, &rtcpCid) == 2) {
			*rtpChannelId = (unsigned char)rtpCid;
			*rtcpChannelId = (unsigned char)rtcpCid;
		}

		fields += strlen(field);
		while (*fields == ';') ++fields; // skip over separating ';' chars
		if (*fields == '\0' || *fields == '\r' || *fields == '\n') break;
	}
	free(field);
}

/* Setup 应答 - 客户端提醒服务器建立会话，并建立传输模式 */
int SetupAnswer(char *cseq,int sock,int SessionId,char * urlSuffix,char* recvbuf,int* rtpport, int* rtcpport)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);

		StreamingMode streamingMode;
		char* streamingModeString; // set when RAW_UDP streaming is specified
		char* clientsDestinationAddressStr;
		u_int8_t clientsDestinationTTL;
		portNumBits clientRTPPortNum, clientRTCPPortNum;
		unsigned char rtpChannelId, rtcpChannelId;
		ParseTransportHeader(recvbuf,&streamingMode, &streamingModeString,
					&clientsDestinationAddressStr, &clientsDestinationTTL,
					&clientRTPPortNum, &clientRTCPPortNum,
					&rtpChannelId, &rtcpChannelId);

		//Port clientRTPPort(clientRTPPortNum);
		//Port clientRTCPPort(clientRTCPPortNum);
		*rtpport = clientRTPPortNum;
		*rtcpport = clientRTCPPortNum;

		char *pTemp = buf;
		char*localip;
		localip = GetLocalIP(sock);
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sTransport: RTP/AVP;unicast;destination=%s;client_port=%d-%d;server_port=%d-%d\r\nSession: %d\r\n\r\n",
						cseq,dateHeader(),localip,
						ntohs(htons(clientRTPPortNum)),
						ntohs(htons(clientRTCPPortNum)),
						ntohs(2000),
						ntohs(2001),
						SessionId);

		free(localip);
		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
		}
		return TRUE;
	}
	return FALSE;
}
/* Play 应答 - 客户端发送播放请求 */
int PlayAnswer(char *cseq, int sock,int SessionId,char* urlPre,char* recvbuf)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		char*localip;
		localip = GetLocalIP(sock);
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sRange: npt=0.000-\r\nSession: %d\r\nRTP-Info: url=rtsp://%s/%s;seq=0\r\n\r\n",
			cseq,dateHeader(),SessionId,localip,urlPre);

		free(localip);

		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
			udpfd = socket(AF_INET,SOCK_DGRAM,0);//UDP
			struct sockaddr_in server;
			server.sin_family=AF_INET;
			server.sin_port=htons(g_rtspClients[0].rtpport[0]);
			server.sin_addr.s_addr=inet_addr(g_rtspClients[0].IP);
			connect(udpfd,(struct sockaddr *)&server,sizeof(server));
			printf("udp up\n");
		}
		return TRUE;
	}
	return FALSE;
}
/* Pause 应答 - 播放暂停请求 */
int PauseAnswer(char *cseq,int sock,char *recvbuf)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%s\r\n\r\n",
			cseq,dateHeader());

		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
		}
		return TRUE;
	}
	return FALSE;
}
/* Teardown 应答 - 客户端发送关闭请求 */
int TeardownAnswer(char *cseq,int sock,int SessionId,char *recvbuf)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sSession: %d\r\n\r\n",
			cseq,dateHeader(),SessionId);

		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
			close(udpfd);
		}
		return TRUE;
	}
	return FALSE;
}

/* 服务器响应客户端消息线程 */
void * RtspClientMsg(void*pParam)
{
	pthread_detach(pthread_self());
	int nRes;
	char pRecvBuf[RTSP_RECV_SIZE];
	RTSP_CLIENT * pClient = (RTSP_CLIENT*)pParam;

	memset(pRecvBuf,0,sizeof(pRecvBuf));
	printf("RTSP:-----Create Client %s\n",pClient->IP);
	while(pClient->status != RTSP_IDLE)
	{
		nRes = recv(pClient->socket, pRecvBuf, RTSP_RECV_SIZE,0);
		//printf("--------- %d\n",nRes);
		if(nRes < 1)
		{
			//usleep(1000);
			printf("RTSP:Recv Error--- %d\n",nRes);
			g_rtspClients[pClient->index].status = RTSP_IDLE;
			g_rtspClients[pClient->index].seqnum = 0;
			g_rtspClients[pClient->index].tsvid = 0;
			g_rtspClients[pClient->index].tsaud = 0;
			close(pClient->socket);
			break;
		}

		/* 解析 RTSP 客户端请求 */
		char cmdName[PARAM_STRING_MAX];
		char urlPreSuffix[PARAM_STRING_MAX];
		char urlSuffix[PARAM_STRING_MAX];
		char cseq[PARAM_STRING_MAX];
		ParseRequestString(pRecvBuf,nRes,cmdName,sizeof(cmdName),urlPreSuffix,sizeof(urlPreSuffix),
							urlSuffix,sizeof(urlSuffix),cseq,sizeof(cseq));

		char *p = pRecvBuf;
		printf("<<<<<%s\n",p);
		//printf("---------- %s %s ----------\n",urlPreSuffix,urlSuffix);

		/* 应答 RTSP 客户端请求 */
		if(strstr(cmdName, "OPTIONS"))
		{
			OptionAnswer(cseq,pClient->socket);
		}
		else if(strstr(cmdName, "DESCRIBE"))
		{
			DescribeAnswer(cseq,pClient->socket,urlSuffix,p);
			//printf("---------DescribeAnswer %s %s\n", urlPreSuffix,urlSuffix);
		}
		else if(strstr(cmdName, "SETUP"))
		{
			int rtpport,rtcpport;
			int trackID=0;
			SetupAnswer(cseq,pClient->socket,pClient->sessionid,urlPreSuffix,p,&rtpport,&rtcpport);

			sscanf(urlSuffix, "trackID=%u", &trackID);
			//printf("---------TrackId %d\n",trackID);
			if(trackID<0 || trackID>=2)trackID=0;
			g_rtspClients[pClient->index].rtpport[trackID] = rtpport;
			g_rtspClients[pClient->index].rtcpport= rtcpport;
			g_rtspClients[pClient->index].reqchn = atoi(urlPreSuffix);
			if(strlen(urlPreSuffix)<100)
				strcpy(g_rtspClients[pClient->index].urlPre,urlPreSuffix);
			//printf("---------SetupAnswer %s-%d-%d\n", urlPreSuffix,g_rtspClients[pClient->index].reqchn,rtpport);
		}
		else if(strstr(cmdName, "PLAY"))
		{
			PlayAnswer(cseq,pClient->socket,pClient->sessionid,g_rtspClients[pClient->index].urlPre,p);
			g_rtspClients[pClient->index].status = RTSP_SENDING;
			printf("Start Play\n",pClient->index);
			//printf("----------------PlayAnswer %d %d\n",pClient->index);
			//usleep(100);
		}
		else if(strstr(cmdName, "PAUSE"))
		{
			PauseAnswer(cseq,pClient->socket,p);
		}
		else if(strstr(cmdName, "TEARDOWN"))
		{
			TeardownAnswer(cseq,pClient->socket,pClient->sessionid,p);
			g_rtspClients[pClient->index].status = RTSP_IDLE;
			g_rtspClients[pClient->index].seqnum = 0;
			g_rtspClients[pClient->index].tsvid = 0;
			g_rtspClients[pClient->index].tsaud = 0;
			close(pClient->socket);
		}
	}
	printf("RTSP:-----Exit Client %s\n",pClient->IP);
	return NULL;
}

/* RTSP服务器监听客户端线程 */
void * RtspServerListen(void*pParam)
{
	int s32Socket;
	struct sockaddr_in servaddr;
	int s32CSocket;
	int s32Rtn;
	int s32Socket_opt_value = 1;
	int nAddrLen;
	struct sockaddr_in addrAccept;
	int bResult;

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port = htons(RTSP_SERVER_PORT);

	s32Socket = socket(AF_INET, SOCK_STREAM, 0);

	/* 设置 socket 属性 */
	if (setsockopt(s32Socket ,SOL_SOCKET,SO_REUSEADDR,&s32Socket_opt_value,sizeof(int)) == -1)
	{
		return (void *)(-1);
	}

	s32Rtn = bind(s32Socket, (struct sockaddr *)&servaddr, sizeof(struct sockaddr_in));
	if(s32Rtn < 0)
	{
		return (void *)(-2);
	}

	s32Rtn = listen(s32Socket, 50);
	if(s32Rtn < 0)
	{
		return (void *)(-2);
	}

	nAddrLen = sizeof(struct sockaddr_in);
	int nSessionId = 1000;
	/* 阻塞等待客服端连接 */
	while ((s32CSocket = accept(s32Socket, (struct sockaddr*)&addrAccept, &nAddrLen)) >= 0)
	{
		printf("<<<<RTSP Client %s Connected...\n", inet_ntoa(addrAccept.sin_addr));

		int nMaxBuf = 10 * 1024; //
		if(setsockopt(s32CSocket, SOL_SOCKET, SO_SNDBUF, (char*)&nMaxBuf, sizeof(nMaxBuf)) == -1)
			printf("RTSP:!!!!!! Enalarge socket sending buffer error !!!!!!\n");

		int i;
		int bAdd=FALSE;
		for(i=0;i<MAX_RTSP_CLIENT;i++)
		{
			if(g_rtspClients[i].status == RTSP_IDLE)
			{
				memset(&g_rtspClients[i],0,sizeof(RTSP_CLIENT));
				g_rtspClients[i].index = i;
				g_rtspClients[i].socket = s32CSocket;
				g_rtspClients[i].status = RTSP_CONNECTED ;//RTSP_SENDING;
				g_rtspClients[i].sessionid = nSessionId++;
				strcpy(g_rtspClients[i].IP,inet_ntoa(addrAccept.sin_addr));
				pthread_t threadIdlsn = 0;

				struct sched_param sched;
				sched.sched_priority = 1;
				pthread_create(&threadIdlsn, NULL, RtspClientMsg, &g_rtspClients[i]);
				//pthread_setschedparam(threadIdlsn,SCHED_RR,&sched);
				bAdd = TRUE;
				break;
			}
		}
		if(bAdd==FALSE) //当客服端连接数等于 MAX_RTSP_CLIENT 时再有客服端尝试连接就会被执行
		{
			memset(&g_rtspClients[0],0,sizeof(RTSP_CLIENT));
			g_rtspClients[0].index = 0;
			g_rtspClients[0].socket = s32CSocket;
			g_rtspClients[0].status = RTSP_CONNECTED ;//RTSP_SENDING;
			g_rtspClients[0].sessionid = nSessionId++;
			strcpy(g_rtspClients[0].IP,inet_ntoa(addrAccept.sin_addr));

			pthread_t threadIdlsn = 0;
			struct sched_param sched;
			sched.sched_priority = 1;
			pthread_create(&threadIdlsn, NULL, RtspClientMsg, &g_rtspClients[0]);
			//pthread_setschedparam(threadIdlsn,SCHED_RR,&sched);
			bAdd = TRUE;
		}
	}
	if(s32CSocket < 0)
	{
		//printf(0, "RTSP listening on port %d,accept err, %d\n", RTSP_SERVER_PORT, s32CSocket);
	}
	printf("----- INIT_RTSP_Listen() Exit !! \n");
	return NULL;
}

/* RTP视频流发送函数 - UDP发送视频流 */
td_s32 VENC_Sent(char *buffer,int buflen)
{
	td_s32 i;
	int is=0;
	int nChanNum=0;

	for(is=0;is<MAX_RTSP_CLIENT;is++)
	{
		if(g_rtspClients[is].status!=RTSP_SENDING)
		{
			continue;
		}
		int heart = g_rtspClients[is].seqnum % 10000;

		char* nalu_payload;
		int nAvFrmLen = 0;
		int nIsIFrm = 0;
		int nNaluType = 0;
		char sendbuf[500*1024+32];

		nAvFrmLen = buflen;

		struct sockaddr_in server;
		server.sin_family=AF_INET;
		server.sin_port=htons(g_rtspClients[is].rtpport[0]);
		server.sin_addr.s_addr=inet_addr(g_rtspClients[is].IP);
		int	bytes=0;
		unsigned int timestamp_increse=0;

		timestamp_increse=(unsigned int)(90000.0 / 25);

		rtp_hdr =(RTP_FIXED_HEADER*)&sendbuf[0];

		rtp_hdr->payload     = RTP_H264;
		rtp_hdr->version     = 2;
		rtp_hdr->marker    = 0;
		rtp_hdr->ssrc      = htonl(10);

		if(nAvFrmLen<=nalu_sent_len)
		{
			rtp_hdr->marker=1;
			rtp_hdr->seq_no     = htons(g_rtspClients[is].seqnum++);
			nalu_hdr =(NALU_HEADER*)&sendbuf[12];
			nalu_hdr->F=0;
			nalu_hdr->NRI=  nIsIFrm;
			nalu_hdr->TYPE=  nNaluType;
			nalu_payload=&sendbuf[13];
			memcpy(nalu_payload,buffer,nAvFrmLen);
					g_rtspClients[is].tsvid = g_rtspClients[is].tsvid+timestamp_increse;
			rtp_hdr->timestamp=htonl(g_rtspClients[is].tsvid);
			bytes=nAvFrmLen+ 13 ;
			sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
		}
		else if(nAvFrmLen>nalu_sent_len)
		{
			int k=0,l=0;
			k=nAvFrmLen/nalu_sent_len;
			l=nAvFrmLen%nalu_sent_len;
			int t=0;

			g_rtspClients[is].tsvid = g_rtspClients[is].tsvid+timestamp_increse;
			rtp_hdr->timestamp=htonl(g_rtspClients[is].tsvid);
			while(t<=k)
			{
				rtp_hdr->seq_no = htons(g_rtspClients[is].seqnum++);
				if(t==0)
				{
					rtp_hdr->marker=0;
					fu_ind =(FU_INDICATOR*)&sendbuf[12];
					fu_ind->F= 0;
					fu_ind->NRI= nIsIFrm;
					fu_ind->TYPE=28;

					fu_hdr =(FU_HEADER*)&sendbuf[13];
					fu_hdr->E=0;
					fu_hdr->R=0;
					fu_hdr->S=1;
					fu_hdr->TYPE=nNaluType;

					nalu_payload=&sendbuf[14];
					memcpy(nalu_payload,buffer,nalu_sent_len);

					bytes=nalu_sent_len+14;
					sendto( udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
					t++;
				}
				else if(k==t)
				{
					rtp_hdr->marker=1;
					fu_ind =(FU_INDICATOR*)&sendbuf[12];
					fu_ind->F= 0 ;
					fu_ind->NRI= nIsIFrm ;
					fu_ind->TYPE=28;

					fu_hdr =(FU_HEADER*)&sendbuf[13];
					fu_hdr->R=0;
					fu_hdr->S=0;
					fu_hdr->TYPE= nNaluType;
					fu_hdr->E=1;
					nalu_payload=&sendbuf[14];
					memcpy(nalu_payload,buffer+t*nalu_sent_len,l);
					bytes=l+14;
					sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
					t++;
				}
				else if(t<k && t!=0)
				{
					rtp_hdr->marker=0;

					fu_ind =(FU_INDICATOR*)&sendbuf[12];
					fu_ind->F=0; 
					fu_ind->NRI=nIsIFrm;
					fu_ind->TYPE=28;
					fu_hdr =(FU_HEADER*)&sendbuf[13];
					//fu_hdr->E=0;
					fu_hdr->R=0;
					fu_hdr->S=0;
					fu_hdr->E=0;
					fu_hdr->TYPE=nNaluType;
					nalu_payload=&sendbuf[14];
					memcpy(nalu_payload,buffer+t*nalu_sent_len,nalu_sent_len);
					bytes=nalu_sent_len+14;
					sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
					t++;
				}
			}
		}
	}
}

/* 功能 : 视频流发送 - RTP直接方式发送 */
td_s32 SAMPLE_COMM_VENC_Sentjin(ot_venc_stream *pstStream)
{
	td_s32 i,flag=0;

	for(i=0;i<MAX_RTSP_CLIENT;i++)//客服端连接个数判断
	{
		if(g_rtspClients[i].status == RTSP_SENDING)  //RTSP客户端状态,由RtspClientMsg的PlayMsg改变
		{
			flag = 1;
			break;
		}
	}
	if(flag)
	{
		for (i = 0; i < pstStream->pack_cnt; i++)
		{
			td_s32 lens=0,j,lastadd=0,newadd=0,showflap=0;
			char sendbuf[320*1024];
			//char tmp[640*1024];
			lens = pstStream->pack[i].len-pstStream->pack[i].offset;
			memcpy(&sendbuf[0],pstStream->pack[i].addr+pstStream->pack[i].offset,lens);
			//printf("lens = %d, count= %d\n",lens,count++);
			VENC_Sent(sendbuf,lens);
			lens = 0;
		}
	}
	return TD_SUCCESS;
}
/* 功能 : 视频流发送 - RTP缓存方式发送 */
td_void* vdRTPSendThread(td_void *p)
{
	while(1)
	{
		if(!list_empty(&RTPbuf_head))
		{
			RTPbuf_s *p = get_first_item(&RTPbuf_head,RTPbuf_s,list);
			VENC_Sent(p->buf,p->len);
			list_del(&(p->list));
			free(p->buf);
			free(p);
			p = NULL;
			count--;
			//printf("count = %d\n",count);
		}
		usleep(5000);
	}
}
/* 功能 : 视频流缓存 - 存入链表 */
td_s32 saveStream(ot_venc_stream *pstStream)
{
	td_s32 i,j,lens=0;

	for(j=0;j<MAX_RTSP_CLIENT;j++)//客服端连接个数判断
	{
		if(g_rtspClients[j].status == RTSP_SENDING)
		{
			for (i = 0; i < pstStream->pack_cnt; i++)
			{
				RTPbuf_s *p = (RTPbuf_s *)malloc(sizeof(RTPbuf_s));
				INIT_LIST_HEAD(&(p->list));

				lens = pstStream->pack[i].len-pstStream->pack[i].offset;
				p->buf = (char *)malloc(lens);
				p->len = lens;
				memcpy(p->buf,pstStream->pack[i].addr+pstStream->pack[i].offset,lens);

				list_add_tail(&(p->list),&RTPbuf_head);
				count++;
				//printf("count = %d\n",count);
			}
		}
	}
	return TD_SUCCESS;
}

void RtspServer_init(void)
{
	int i;
	pthread_t threadId = 0;

	/* RTSP音频相关 */
	memset(&g_rtp_playload,0,sizeof(g_rtp_playload));
	strcpy(&g_rtp_playload,"G726-32");
	g_audio_rate = 8000;
	/* RTSP音频相关 */

	/* 变量初始化 */
	pthread_mutex_init(&g_sendmutex,NULL);
	pthread_mutex_init(&g_mutex,NULL);
	pthread_cond_init(&g_cond,NULL);
	memset(&g_rtspClients,0,sizeof(RTSP_CLIENT)*MAX_RTSP_CLIENT); //初始化客户端信息存储变量
	/* 变量初始化 */

	//pthread_create(&g_SendDataThreadId, NULL, SendDataThread, NULL);

	/* RTSP服务器监听线程、线程调度策略设置 */
	struct sched_param thdsched;
	thdsched.sched_priority = 2;   //线程优先级
	//to listen visiting
	pthread_create(&threadId, NULL, RtspServerListen, NULL);
	//pthread_setschedparam(threadId,SCHED_RR,&thdsched);  //设置线程的调度策略
	printf("RTSP:-----Init Rtsp server\n");
	printf("RTSP:-----[rtsp://192.168.xxx.xxx:%d/0]\n", RTSP_SERVER_PORT);
	/* RTSP服务器监听线程、线程调度策略设置 */

	/* RTP视频流发送线程 - 缓存方式发送 */
	pthread_create(&gs_RtpPid, 0, vdRTPSendThread, NULL);
}
void RtspServer_exit(void)
{
	return;
}
#endif

/*************************************
* function    : main()
* Description : Test
**************************************/
/*
int main(int argc, char *argv[])
{
	td_s32 s32Ret;

	RtspServer_init();

	return s32Ret;
}
*/

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* End of #ifdef __cplusplus */
