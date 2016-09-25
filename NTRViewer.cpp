

#include <stdio.h>
#include <time.h>

#include <turbojpeg.h>

#include "intdef.h"
#ifndef INT64_C
#define INT64_C(c) (c ## LL)
#define UINT64_C(c) (c ## ULL)
#endif
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}


#ifdef WIN32
#pragma comment(lib,"ws2_32.lib")  
#include <winsock2.h>
#include <SDL.h>
#define PIX_FMT_RGB24 AV_PIX_FMT_RGB24
typedef int socklen_t;

#else
#include <unistd.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include <arpa/inet.h>
#include <SDL/SDL.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

#include "getopt.h"

#define PACKET_SIZE (1448)



float topScaleFactor = 1;
float botScaleFactor = 1;
int screenWidth;
int screenHeight;
int layoutMode = 1;
int logLevel = 1;

struct SwsContext *topSwsContext, *botSwsContext;



SDL_Surface* topSurface;
SDL_Surface* botSurface;
SDL_Surface* screenSurface;


tjhandle decompressHandle;

u8 recvBuffer[2][2][1444 * 140 * 3];

u8 topBuffer[400 * 240 * 3 * 3];
u8 botBuffer[320 * 240 * 3 * 3];
u8 decompressBuffer[2][400 * 240 * 3 * 3];

int totalCount = 0;
int badCount = 0;
int recoverCount = 0;
float totalCompressRatio = 0;
int compressCount = 0;
int lastTime = clock();

int topRequireUpdate = 0;
int botRequireUpdate = 0;
SDL_Rect topDrawRect = {0,0,400,240};
SDL_Rect botDrawRect = {0,0,320,240};
SDL_Rect topRect = {0, 0, 400, 240};
SDL_Rect botRect = {40, 240, 320, 240};
int lastFormat = -1;

u8 buf[2000];
u8 trackingId[2];
int newestBuf[2], bufCount[2][2], isFinished[2][2];
int bufTarget[2][2];

#define BUF_TARGET_UNKNOWN (9999)

int uncompressedTargetCount[2] = {107, 133};

char activateStreamingHost[256];


int fullScreenMode = 0;
int hideCursor = 0;

int priorityMode = 0;
int priorityFactor = 5;
int jpegQuality = 80;
float qosValue = 30.0;


SOCKET serSocket;

void printTime() {
    time_t rawtime;
    struct tm * timeinfo;
    char buffer[128];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, sizeof (buffer), "[%H:%M:%S] ", timeinfo);
    printf("%s", buffer);
}

int logI(const char *format, ...) {
    va_list ap;
    int retval;
    va_start(ap, format);
    printTime();
    retval = vprintf(format, ap);
    va_end(ap);
    return retval;
}

void closeSocket(SOCKET fd) {
#ifdef WIN32
	closesocket(fd);
#else 
	close(fd);
#endif
}

void doSleep(int sec) {
#ifdef WIN32
	Sleep(sec * 1000);
#else
	sleep(sec);
#endif
}

int logV(const char *format, ...) {
    va_list ap;
    if (logLevel < 2) {
        return 0;
    }
    int retval;
    va_start(ap, format);
    printTime();
    retval = vprintf(format, ap);
    va_end(ap);
    return retval;
}

void transBuffer(u16* dst, u16* src, int width, int height, int format) {
    int blockSize = 16;
    u16* blockStart;

    for (int x = 0; x < width; x += blockSize) {
        for (int y = 0; y < height; y += blockSize) {
            blockStart = dst + x * height + y;
            for (int i = 0; i < blockSize; i++) {
                for (int j = 0; j < blockSize; j++) {
                    *(blockStart + i * height + j) = *src;
                    src++;
                }
            }
        }
    }
}

void convertBuffer(u8* dst, u8* src, int width, int height, int format) {
    int x, y;
    u8 *dp, *sp;
    int bytesPerRow = width * 3;
    dp = dst;
    sp = src;

    for (x = 0; x < width; x++) {
        dp = dst + bytesPerRow * (height - 1) + 3 * x;
        for (y = 0; y < height; y++) {
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            sp += 3;
            dp -= bytesPerRow;
        }
    }
}

int getIdDiff(u8 now, u8 before) {
    if (now >= before) {
        return now - before;
    } else {
        return (u32) now + 256 - before;
    }
}

void advanceBuffer(u8 isTop) {
    totalCount += 1;
    if (!isFinished[isTop][!newestBuf[isTop]]) {
        logV("1 bad frame dropped\n");
        badCount += 1;
    }
    newestBuf[isTop] = !newestBuf[isTop];
    bufCount[isTop][newestBuf[isTop]] = 0;
    isFinished[isTop][newestBuf[isTop]] = 0;
    bufTarget[isTop][newestBuf[isTop]] = BUF_TARGET_UNKNOWN;


}

void resetBuffer(u8 isTop) {
    newestBuf[isTop] = 0;
    bufCount[isTop][0] = 0;
    bufCount[isTop][1] = 0;
    isFinished[isTop][0] = 0;
    isFinished[isTop][1] = 0;
    bufTarget[isTop][0] = BUF_TARGET_UNKNOWN;
    bufTarget[isTop][1] = BUF_TARGET_UNKNOWN;
    totalCount += 2;
    badCount += 2;
    logV("** reset buffer, 2 bad frames dropped\n");
}

void uncompressJpeg(u8* src, u8* dst, u32 srcSize, u32 dstSize, u32 width, u32 height) {
    tjDecompress2(decompressHandle, src, srcSize, dst, width, 3 * width, height, TJPF_RGB, 0);
}

void drawFrame(int isTop, int addr) {
    u8* src_data[4] = {0,0,0,0};
    u8* dst_data[4] = {0,0,0,0};
    int src_linesize[4] = {0,0,0,0};
    int dst_linesize[4] = {0,0,0,0};
    if (isTop) {
        
        if (topRect.w < 10) {
            return;
        }
        compressCount += 1;
        totalCompressRatio += ((float) bufTarget[isTop][addr]) / uncompressedTargetCount[isTop];
        uncompressJpeg(recvBuffer[isTop][addr], decompressBuffer[1], (PACKET_SIZE - 4) * bufTarget[isTop][addr], 400 * 240 * 3, 240, 400);
        SDL_LockSurface(topSurface);
        
        if (topSwsContext) {
            src_data[0] = topBuffer;
            dst_data[0] = (u8*) (topSurface->pixels);
            src_linesize[0] = 400 * 3;
            dst_linesize[0] = (topRect.w) * 3;
            convertBuffer((u8*) (topBuffer), decompressBuffer[1], 400, 240, lastFormat);
            sws_scale(topSwsContext,  src_data,src_linesize, 0, 240, dst_data,dst_linesize );
        } else {
            convertBuffer((u8*) (topSurface->pixels), decompressBuffer[1], 400, 240, lastFormat);
        }

        SDL_UnlockSurface(topSurface);
        topRequireUpdate = 1;
    } else {
        if (botRect.w < 10) {
            return;
        }
        compressCount += 1;
        totalCompressRatio += ((float) bufTarget[isTop][addr]) / uncompressedTargetCount[isTop];
        uncompressJpeg(recvBuffer[isTop][addr], decompressBuffer[0], (PACKET_SIZE - 4) * bufTarget[isTop][addr], 400 * 240 * 3, 240, 320);
        SDL_LockSurface(botSurface);
        if (botSwsContext) {
            src_data[0] = botBuffer;
            dst_data[0] = (u8*) (botSurface->pixels);
            
            src_linesize[0] = 320 * 3;
            dst_linesize[0] = (botRect.w) * 3;
            convertBuffer((u8*) (botBuffer), decompressBuffer[0], 320, 240, lastFormat);
            sws_scale(botSwsContext,  src_data,src_linesize, 0, 240, dst_data,dst_linesize );
        } else {
            convertBuffer((u8*) (botSurface->pixels), decompressBuffer[0], 320, 240, lastFormat);
        }

        SDL_UnlockSurface(botSurface);
        botRequireUpdate = 1;
    }
}

int recoverFrame(int isTop, int addr) {
    // never recover any frame
    return 0;
}

bool checkSDLEvent() {
    SDL_Event e;

    bool quit = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            quit = true;
        }
    }
    return quit;
}

int recvAndHandlePacket() {
    sockaddr_in remoteAddr;
    socklen_t nAddrLen = sizeof (remoteAddr);

    int ret = recvfrom(serSocket, (char*) buf, 2000, 0, (sockaddr *) & remoteAddr, &nAddrLen);
    if (ret <= 0) {
        return 2;
    }

    u8 id = buf[0];
    u8 isTop = buf[1] & 1;
    u8 format = buf[2];
    u8 cnt = buf[3];

    if (isTop > 1) {
        return 1;
    }

    if (format != lastFormat) {
        logI("format changed: %d\n", format);
        lastFormat = format;

    }

    if (getIdDiff(id, trackingId[isTop]) == 2) {
        if (!isFinished[isTop][newestBuf[isTop]]) {
            if (!isFinished[isTop][!newestBuf[isTop]]) {
                // try to recover the frame before dropping it
                recoverFrame(isTop, !newestBuf[isTop]);
            }
        }
        // drop a frame
        advanceBuffer(isTop);
        trackingId[isTop] += 1;
    } else if (getIdDiff(id, trackingId[isTop]) > 2) {
        int shouldTryOlderOne = 0;
        if (!isFinished[isTop][newestBuf[isTop]]) {
            if (!recoverFrame(isTop, newestBuf[isTop])) {
                // failed to recover the newest frame, try older one
                shouldTryOlderOne = 1;
            }
        }
        if (shouldTryOlderOne) {
            if (!isFinished[isTop][!newestBuf[isTop]]) {
                recoverFrame(isTop, !newestBuf[isTop]);
            }
        }
        if (getIdDiff(trackingId[isTop], id) <= 3) {
            // maybe it is from very previous frame, ignore them
            logI("ignoring previous frame: %d\n", id);
            return 0;
        }
        // drop all pending frames
        resetBuffer(isTop);
        trackingId[isTop] = id;
    }
    u32 offsetInBuffer = (u32) (cnt)* (PACKET_SIZE - 4);

    int bufAddr = 0;
    if (id == trackingId[isTop]) {
        bufAddr = !newestBuf[isTop];

    } else {
        bufAddr = newestBuf[isTop];

    }
    if (buf[1] & 0x10) {
        // the last compressed packet
        bufTarget[isTop][bufAddr] = cnt + 1;
    }
    memcpy(recvBuffer[isTop][bufAddr] + offsetInBuffer, buf + 4, (PACKET_SIZE - 4));
    bufCount[isTop][bufAddr] += 1;

    if (bufCount[isTop][bufAddr] >= bufTarget[isTop][bufAddr]) {
        if (bufCount[isTop][bufAddr] > bufTarget[isTop][bufAddr]) {
            // we have receive same packet multiple times?
            logI("wow\n");
        }
        isFinished[isTop][bufAddr] = 1;
        if ((isFinished[isTop][newestBuf[isTop]]) && (bufAddr != newestBuf[isTop])) {
            // the newest frame has already finished, do not draw
            logV("newest frame already finished: %d %d\n", isTop, id);
        }

        logV("good frame: %d %d\n", isTop, id);
        if (isTop) {
            logV("compression rate: %.2f\n", (float) (bufTarget[isTop][bufAddr]) / uncompressedTargetCount[isTop]);
        }
        drawFrame(isTop, bufAddr);
    }
    return 0;
}

int mainLoop() {

    int ret;


    serSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (serSocket == INVALID_SOCKET) {
        printf("socket error !");
        return 0;
    }

    sockaddr_in serAddr;
    serAddr.sin_family = AF_INET;
    serAddr.sin_port = htons(8001);
    serAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(serSocket, (sockaddr *) & serAddr, sizeof (serAddr)) == SOCKET_ERROR) {
        printf("bind error !");
        return 0;
    }


    int lastRecvCount = 0;

    int buff_size = 8 * 1024 * 1024;
    socklen_t tmp = 4;

    ret = setsockopt(serSocket, SOL_SOCKET, SO_RCVBUF, (char*) (&buff_size), sizeof (buff_size));
    buff_size = 0;
    ret = getsockopt(serSocket, SOL_SOCKET, SO_RCVBUF, (char*) (&buff_size), &tmp);
    logI("set buff size: %d\n", buff_size);
    if (ret) {
        printf("set buff size failed, ret: %d\n", ret);
        return 0;
    }
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    ret = setsockopt(serSocket, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout,
            sizeof (timeout));
        if (ret) {
        printf("set recv timeout failed, ret: %d\n", ret);
        return 0;
    }


    resetBuffer(0);
    resetBuffer(1);
    while (true) {

        if (totalCount >= 100) {
            float quality = ((totalCount - badCount) * 1.0f + (recoverCount) * 0.5f) / totalCount * 100;
            float compressRate = 0;
            float fps = 0;
            if (compressCount > 0) {
                compressRate = totalCompressRatio / compressCount;
            }
            logI("Quality: %.0f%% (total: %d, recover: %d, drop: %d)\n", quality, totalCount, recoverCount, badCount - recoverCount, compressRate);
            float timePassed = ((float) (clock() - lastTime) / CLOCKS_PER_SEC);
            if (timePassed > 0.1) {
                fps = compressCount / timePassed;
            }
            logI("fps: %.2f, compress rate: %.2f\n", fps, compressRate);
            lastTime = clock();
            totalCount = 0;
            badCount = 0;
            recoverCount = 0;
            compressCount = 0;
            totalCompressRatio = 0;

            char buf[512];
            sprintf(buf, "NTRViewer - Quality: %.0f%%, fps: %.2f", quality, fps);
            SDL_WM_SetCaption( buf, NULL);
        }

        bool requireCheckEvent = false;
        ret = recvAndHandlePacket();
        if (ret == 2) {
            logI("recv failed or timeout.\n");
            requireCheckEvent = true;
        }
        SDL_Rect destRect;
        
        if (topRequireUpdate) {
            topRequireUpdate = 0;
            destRect = topRect;
            SDL_BlitSurface(topSurface, &topDrawRect , screenSurface, &destRect);
   
            requireCheckEvent = true;
        }
        if (botRequireUpdate) {
            botRequireUpdate = 0;
            destRect = botRect;
            SDL_BlitSurface(botSurface, NULL, screenSurface, &destRect);
            requireCheckEvent = true;
        }

        if (requireCheckEvent) {
            
            SDL_UpdateRect(screenSurface, 0, 0, 0, 0);
            bool quit = checkSDLEvent();
            if (quit) {
                SDL_Quit();
                exit(0);
            }
        }

    }
	closeSocket(serSocket);

    return 0;
}
SDL_Surface* createScreenSurface(int width, int height) {
    /* Create a 32-bit surface with the bytes of each pixel in R,G,B,A order,
       as expected by OpenGL for textures */
    SDL_Surface *surface;
    Uint32 rmask, gmask, bmask, amask;


    rmask = 0x000000ff;
    gmask = 0x0000ff00;
    bmask = 0x00ff0000;
    amask = 0x0;

    surface = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 24,
                                   rmask, gmask, bmask, amask);
    if(surface == NULL) {
        printf( "CreateRGBSurface failed: %s\n", SDL_GetError());
        exit(1);
    }    
    return surface;
}

SDL_Rect createRect(int x, int y, int w, int h) {
    SDL_Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

int startViewer() {


    if (layoutMode == 0) {
        topRect = createRect(0, 0, (int) (400 * topScaleFactor), (int) (240 * topScaleFactor));
        botRect = createRect((int) (400 * topScaleFactor), 0,(int) (320 * botScaleFactor), (int) (240 * botScaleFactor));
    } else {
        topRect = createRect(0, 0, (int) (400 * topScaleFactor),(int) (240 * topScaleFactor));
        float indent = 400 * topScaleFactor - 320 * botScaleFactor;
        botRect = createRect((int)(indent / 2), (int) (240 * topScaleFactor),(int) (320 * botScaleFactor), (int) (240 * botScaleFactor));
    }
    
    topDrawRect = createRect(0,0,topRect.w, topRect.h);
    botDrawRect = createRect(0,0,botRect.w, botRect.h);
    
    printf("init SDL...\n");

    if( SDL_Init( SDL_INIT_VIDEO ) == -1 ) {
        return 1;
    }
    printf("SDL_Init done.\n");

    if (layoutMode == 0) {
        screenWidth = (int) (400 * topScaleFactor + 320 * botScaleFactor);
        screenHeight = (int) (240 * topScaleFactor);
    } else {
        screenWidth = (int) (400 * topScaleFactor);
        screenHeight = (int) (240 * topScaleFactor + 240 * botScaleFactor);
    }
    if (topRect.w != 400) {
        topSwsContext = sws_getContext(400, 240,PIX_FMT_RGB24, topRect.w, topRect.h, PIX_FMT_RGB24,  SWS_BICUBIC, NULL, NULL, NULL);
        logI("Top screen is scaled.\n");
        
    }
    if (botRect.w != 320) {
        botSwsContext = sws_getContext(300, 240,PIX_FMT_RGB24, botRect.w, botRect.h, PIX_FMT_RGB24,  SWS_BICUBIC, NULL, NULL, NULL);
        logI("Bottom screen is scaled.\n");
        
    }
    
    screenSurface = SDL_SetVideoMode( screenWidth, screenHeight, 24, SDL_SWSURFACE | (fullScreenMode?SDL_FULLSCREEN:0));
    if (hideCursor) {
        SDL_ShowCursor(SDL_DISABLE);
    }
    printf("screenSurface: %p\n", screenSurface);
    if (screenSurface == NULL) {
        printf("SDL_SetVideoMode failed.\n");
        return 1;
    }

    topSurface = createScreenSurface(topRect.w, topRect.h + 20);

    botSurface = createScreenSurface(botRect.w, botRect.h + 20);

    mainLoop();
}

int buildPacket(u32* buf, u32 type, u32 cmd, u32 arg0, u32 arg1, u32 arg2, u32 arg3) {
    int packetSize = 84;
    memset((void*) buf, 0, packetSize);
    buf[0] = 0x12345678;
    buf[1] = 1;
    buf[2] = type;
    buf[3] = cmd;
    buf[4] = arg0;
    buf[5] = arg1;
    buf[6] = arg2;
    buf[7] = arg3;
    return packetSize;
}
void activateStreaming(char* host) {
    u32 buf[64];
    
    struct sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;   
    client_addr.sin_addr.s_addr = htons(INADDR_ANY);
    client_addr.sin_port = htons(0);    
    
    SOCKET client_socket = socket(AF_INET,SOCK_STREAM,0);
    if( client_socket < 0)
    {
        printf("Create Socket Failed!\n");
        exit(1);
    }
    
    if( bind(client_socket,(struct sockaddr*)&client_addr,sizeof(client_addr)))
    {
        printf("Client Bind Port Failed!\n"); 
        exit(1);
    }
 
	struct sockaddr_in server_addr = { 0 };
    server_addr.sin_family = AF_INET;
#ifdef WIN32
	server_addr.sin_addr.s_addr = inet_addr(host);
#else
	if (inet_aton(host, &server_addr.sin_addr) == 0)
	{
		printf("Server IP Address Error!\n");
		exit(1);
	}
#endif
	

    server_addr.sin_port = htons(8000);
    socklen_t server_addr_length = sizeof(server_addr);
    if(connect(client_socket,(struct sockaddr*)&server_addr, server_addr_length) < 0)
    {
        printf("Connect To %s failed!\n",host);
        exit(1);
    }
    printf("connected. \n");
    int mode = 1;
    if (priorityMode) {
        mode = 0;
    }
    int qosInByte = (int) (qosValue * 1024 * 1024 / 8);
    int packetSize = buildPacket(buf, 0, 901,  (mode << 8) | priorityFactor, jpegQuality, qosInByte, 0);
    send(client_socket, (const char*) buf, packetSize, 0);
    doSleep(1);
    for (int i = 0; i < 5; i++ ){
        
        int packetSize = buildPacket(buf, 0, 0, 0, 0, 0, 0);
		if (send(client_socket, (const char*)buf, packetSize, 0) < 0) {
            printf("send ping packet failed!\n");
            exit(1);
        } else {
            printf("sent ping packet.\n");
        }
        doSleep(1);
    }
	closeSocket(client_socket);
}


void parseOpts(int argc, char* argv[]) {
    const char* optString = "l:t:b:Dd:fa:h";
    int opt = getopt(argc, argv, optString);;
    while (opt != (int) (-1)) {
        
        switch (opt) {
            case 'l':
                layoutMode = atoi(optarg);
                break;
            case 't':
                topScaleFactor = (float) atof(optarg);
                break;
            case 'b':
                botScaleFactor = (float) atof(optarg);
                break;
            case 'D':
                exit(0);
                break;
            case 'f':
                fullScreenMode = 1;
                break;
            case 'h':
                hideCursor = 1;
                break;
            case 'a':
                strcpy(activateStreamingHost, optarg);
                break;
            default:
                break;
        }

        opt = getopt(argc, argv, optString);
    }
}

#undef main

int main(int argc, char* argv[]) {
#ifdef WIN32
    WSADATA wsaData;
    WORD sockVersion = MAKEWORD(2, 2);
    if (WSAStartup(sockVersion, &wsaData) != 0) {
        return 0;
    }
#endif
    decompressHandle = tjInitDecompress();
    parseOpts(argc, argv);
    
    if (strlen(activateStreamingHost) > 0) {
        activateStreaming(activateStreamingHost);
    }

   
    startViewer();

#ifdef WIN32
    WSACleanup();
#endif

    return 0;
}

