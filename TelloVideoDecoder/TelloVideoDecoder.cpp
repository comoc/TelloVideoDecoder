#include "PlatformBase.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <cstdlib>
#include <assert.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#ifdef UNITY_WIN
#include <winsock2.h>
#include <ws2tcpip.h>

#define _CRTDBG_MAP_ALLOC #include <stdlib.h> #include <crtdbg.h>
#define new ::new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define malloc(X) _malloc_dbg(X,_NORMAL_BLOCK,__FILE__,__LINE__)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif

#include "Unity/IUnityInterface.h"
#include "RenderAPI.h"
#include "PlatformBase.h"

using namespace std;

extern "C" void debug_log(const char* msg);

class GlobalObject
{
public:
	GlobalObject()
	{
#ifdef UNITY_WIN
		WSADATA data;
		WSAStartup(MAKEWORD(2, 0), &data);
#endif
	}
	~GlobalObject()
	{
#ifdef UNITY_WIN
		WSACleanup();
#endif
	}
};
GlobalObject _globalObject;

class MyUdpClient
{
public:
	MyUdpClient()
		:
#ifdef UNITY_WIN
		sock(INVALID_SOCKET)
#else
		sock(-1)
#endif
	{
	}
	~MyUdpClient()
	{
		close();
	}

	bool open(int port)
	{
		isRunning = true;

		sock = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef UNITY_WIN
		if (sock != INVALID_SOCKET) {
#else
		if (sock != -1) {
#endif
			sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons(port);
			addr.sin_addr.s_addr = htonl(INADDR_ANY);

			int status = ::bind(sock, (sockaddr*)&addr, sizeof(addr));
#ifdef UNITY_WIN
			if (status != SOCKET_ERROR) {
				u_long val = 1;
				ioctlsocket(sock, FIONBIO, &val);
				return true;
			}
			else {
				closesocket(sock);
				sock = INVALID_SOCKET;
			}
#else
			if (status != -1) {
				int val = 1;
				ioctl(sock, FIONBIO, &val);
				return true;
			}
			else {
				close(sock);
				sock = -1;
			}
#endif
		}
		return false;
	}

	void close()
	{
		isRunning = false;

#ifdef UNITY_WIN
		if (sock != INVALID_SOCKET) {
			closesocket(sock);
			sock = INVALID_SOCKET;
		}
#else
		if (sock != INVALID_SOCKET) {
			close(sock);
			sock = -1;
		}
#endif
	}

	int read(char* buffer, int buffer_size)
	{
#ifdef UNITY_WIN
		if (sock == INVALID_SOCKET)
#else
		if (sock == -1)
#endif
			return -1;

		int read_size = -1;
		while (isRunning) {
			read_size = recvfrom(sock, buffer, buffer_size, 0, nullptr, nullptr);
#ifdef UNITY_WIN
			if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
			if (errno == EAGAIN) {
#endif
				this_thread::yield();
				continue;
			}
			else {
				break;
			}
		}
		return read_size;
	}
private:
#ifdef UNITY_WIN
	SOCKET sock;
#else
	int sock;
#endif
	volatile bool isRunning;
};

//#define USE_FILE
class MyAVIOContext
{
public:
#ifdef USE_FILE
	MyAVIOContext(const char* input_filename)
		: avio_ctx(nullptr), recvSocekt(INVALID_SOCKET) {
		errno_t error = fopen_s(&f_, input_filename, "rb");
		if (error != 0)
			f_ = nullptr;
#else
	MyAVIOContext(const char* address, int port)
		: avio_ctx(nullptr), buffer2(nullptr){
#endif
		//avformat_network_init(); // not necessary

		buffer = static_cast<unsigned char*>(av_malloc(BUFFER_SIZE));
		avio_ctx = avio_alloc_context(buffer, BUFFER_SIZE, 0, this, &MyAVIOContext::read, nullptr, nullptr);

		if (!udp.open(port)) {
			debug_log("UDP open failed");
		}
	}

	~MyAVIOContext() {
		av_freep(&avio_ctx->buffer);
		av_freep(&avio_ctx);
#ifdef USE_FILE
		if (f_ != nullptr)
			fclose(f_);
#endif
		if (buffer2 != nullptr)
			delete[] buffer2;

		udp.close();
	}

	static int read(void *opaque, unsigned char *buf, int buf_size) {
		MyAVIOContext* h = static_cast<MyAVIOContext*>(opaque);

		stringstream ss;

#ifdef USE_FILE
		int read_size = static_cast<int>(fread(buf, 1, buf_size, h->f_));
		ss << "buf_size: " << buf_size << ", pos: " << ftell(h->f_) << ", read_size:" << read_size;
#else
		if (buf_size + 2 > h->buffer2_size) {
			if ( h->buffer2 != nullptr)
				delete[]  h->buffer2;
			 h->buffer2_size = buf_size + 2;
			 h->buffer2 = new char[ h->buffer2_size];
		}
		int read_size =	h->udp.read(h->buffer2, h->buffer2_size);
		if (read_size <= 0) {
			return 0;
		}

		uint8_t b0 = (uint8_t)h->buffer2[0];
		uint8_t b1 = (uint8_t)h->buffer2[1];
		ss << "buf_size: " << buf_size << ", read_size:" << read_size << " [0]:" << (int)b0 << " [1]:" << (int)b1;
		debug_log(ss.str().c_str());

		read_size -= 2;
		memcpy(buf, h->buffer2 + 2, read_size);
#endif	
		return read_size;
	}

	AVIOContext* get() { return avio_ctx; }

private:
	static const int BUFFER_SIZE = 32768;
	unsigned char* buffer;
	AVIOContext * avio_ctx;
	MyUdpClient udp;

	char* buffer2;
	int buffer2_size;
#ifdef USE_FILE
	FILE * f_;
#else
#endif
	};

class MyVideoDecoder {
public:
	MyVideoDecoder() :
		avioContext(nullptr),
		fmt_ctx(nullptr),
		video_stream(nullptr),
		codec(nullptr),
		codec_context(nullptr),
		frame(nullptr) {

	}
	~MyVideoDecoder() {
	}

public:
	bool run(uint8_t* destination, size_t size, mutex* mtx, bool* isRunning) {
		if (!open())
			return false;

		// Read frame
		while ((*isRunning) && av_read_frame(fmt_ctx, &packet) == 0) {
			if (packet.stream_index == video_stream->index) {
				if (avcodec_send_packet(codec_context, &packet) != 0) {
					debug_log("avcodec_send_packet failed\n");
				}
				while ((*isRunning) && avcodec_receive_frame(codec_context, frame) == 0) {
					sws_scale(convert_context, (const uint8_t* const*)frame->data, frame->linesize, 0, codec_context->height, frame_rgb->data, frame_rgb->linesize);
					stringstream ss;
					ss << "Decoded frame: " << (unsigned long)frameCounter << " width: " << codec_context->width << " height: " <<  codec_context->height;
					debug_log(ss.str().c_str());
					mtx->lock();
					memcpy(destination, frame_rgb->data[0], size);
					mtx->unlock();
					frameCounter++;
				}
			}
			av_packet_unref(&packet);
			this_thread::yield();
		}


		close();
		return true;
	}

private:
	bool open() {
		avioContext = new MyAVIOContext(TELLO_ADDRESS.c_str(), TELLO_VIDEO_PORT);
		fmt_ctx = avformat_alloc_context();
		fmt_ctx->pb = avioContext->get();

		/// using ctx
		int ret;
		ret = avformat_open_input(&fmt_ctx, nullptr, nullptr, nullptr);
		if (ret < 0) {
			debug_log("Could not open input\n");
			close();
			return false;
		}

		ret = avformat_find_stream_info(fmt_ctx, nullptr);
		if (ret < 0) {
			debug_log("Could not find stream information\n");
			close();
			return false;
		}
#ifdef USE_FILE
		av_dump_format(fmt_ctx, 0, input_filename, 0);
#else
		av_dump_format(fmt_ctx, 0, TELLO_ADDRESS.c_str(), 0);
#endif

		// decode
		for (int i = 0; i < (int)fmt_ctx->nb_streams; ++i) {
			if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				video_stream = fmt_ctx->streams[i];
				break;
			}
		}
		if (video_stream == nullptr) {
			debug_log("No video stream ...\n");
			close();
			return false;
		}
		if (video_stream->codecpar == nullptr) {
			debug_log("No codec parameter ...\n");
			close();
			return false;
		}

		codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
		if (codec == nullptr) {
			debug_log("No supported decoder ...\n");
			close();
			return false;
		}

		codec_context = avcodec_alloc_context3(codec);
		if (codec_context == nullptr) {
			debug_log("avcodec_alloc_context3 failed\n");
			close();
			return false;
		}

		if (avcodec_parameters_to_context(codec_context, video_stream->codecpar) < 0) {
			debug_log("avcodec_parameters_to_context failed\n");
			close();
			return false;
		}

		if (avcodec_open2(codec_context, codec, nullptr) != 0) {
			debug_log("avcodec_open2 failed\n");
			close();
			return false;
		}

		frame = av_frame_alloc();

		// Color converter
		frame_rgb = av_frame_alloc();
		buffer = (uint8_t*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGBA, codec_context->width, codec_context->height, 1));
		av_image_fill_arrays(frame_rgb->data, frame_rgb->linesize, buffer, AV_PIX_FMT_RGBA, codec_context->width, codec_context->height, 1);
		//av_image_fill_arrays(frame->data, frame->linesize, buffer, AV_PIX_FMT_RGBA, codec_context->width, codec_context->height, 1);
		convert_context = sws_getContext(codec_context->width, codec_context->height, codec_context->pix_fmt, codec_context->width, codec_context->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		return true;
	}

	void close() {
		// flush decoder
		if (codec_context != nullptr && avcodec_send_packet(codec_context, nullptr) != 0) {
			debug_log("avcodec_send_packet failed");
		}

		if (codec_context != nullptr && frame != nullptr && convert_context != nullptr && frame_rgb != nullptr) {
			while (avcodec_receive_frame(codec_context, frame) == 0) {
				sws_scale(convert_context, (const uint8_t* const*)frame->data, frame->linesize, 0, codec_context->height, frame_rgb->data, frame_rgb->linesize);
				//debug_log("%u width: %d, height: %d\n", (unsigned long)frameCounter, codec_context->width, codec_context->height);
				frameCounter++;
			}
		}

		if (convert_context != nullptr) {
			sws_freeContext(convert_context);
			convert_context = nullptr;
		}

		if (buffer != nullptr) {
			av_free(buffer);
			buffer = nullptr;
		}

		if (frame_rgb != nullptr) {
			av_frame_free(&frame_rgb);
			frame_rgb = nullptr;
		}

		if (frame != nullptr) {
			av_frame_free(&frame);
			frame = nullptr;
		}

		if (frame_rgb != nullptr) {
			av_frame_free(&frame_rgb);
			frame_rgb = nullptr;
		}
		if (codec_context != nullptr) {
			avcodec_free_context(&codec_context);
			codec_context = nullptr;
		}

		if (fmt_ctx != nullptr) {
			avformat_close_input(&fmt_ctx);
		}

		if (fmt_ctx != nullptr) {
			avformat_free_context(fmt_ctx);
			fmt_ctx = nullptr;
		}

		if (avioContext != nullptr) {
			delete avioContext;
			avioContext = nullptr;
		}
	}
private:
	static const string TELLO_ADDRESS;
	static const string TELLO_URL;
	static const int TELLO_VIDEO_PORT;

	MyAVIOContext* avioContext;
	AVFormatContext* fmt_ctx;
	AVStream* video_stream;
	AVCodec* codec;
	AVCodecContext* codec_context = nullptr;
	AVFrame* frame = nullptr;
	AVPacket packet = AVPacket();

	AVFrame* frame_rgb = nullptr;
	uint8_t* buffer = nullptr;
	SwsContext* convert_context = nullptr;

	uint64_t frameCounter = 0;

};



const string MyVideoDecoder::TELLO_ADDRESS("192.168.0.1");
const string MyVideoDecoder::TELLO_URL("udp://192.168.0.1:6038");
const int MyVideoDecoder::TELLO_VIDEO_PORT(6038);


namespace {
	const size_t PIXELS = 1280 * 720;
	const size_t BPP = 4;
	const size_t IMAGE_SIZE_IN_BYTE = PIXELS * BPP;
}

struct GlobalData {
	bool s_isRunning = true;
	uint8_t* s_imageBuffer = nullptr;
	thread* s_thread = nullptr;
	mutex s_mutex;
};
GlobalData* global = nullptr;


void decode()
{
	MyVideoDecoder decoder;
	while (global->s_isRunning) {
		decoder.run(global->s_imageBuffer, IMAGE_SIZE_IN_BYTE, &global->s_mutex, &global->s_isRunning);
		this_thread::yield();
	}
}


void TelloVideoDecoder_Open()
{
	global = new GlobalData();

	global->s_imageBuffer = new uint8_t[IMAGE_SIZE_IN_BYTE];
	for (int i = 0; i < IMAGE_SIZE_IN_BYTE; i += BPP) {
		for (int j = 0; j < BPP; j++) {
			if (j == 0 || j == 3)
				global->s_imageBuffer[i + j] = 0xff;
			else
				global->s_imageBuffer[i + j] = 0;
		}
	}

	global->s_isRunning = true;
	global->s_thread = new thread(decode);
}

void TelloVideoDecoder_Close()
{
	global->s_isRunning = false;
	if (global->s_thread != nullptr) {
		if (global->s_thread->joinable()) {
			global->s_thread->join();
			delete global->s_thread;
			global->s_thread = nullptr;
		}
	}

	if (global->s_imageBuffer != nullptr) {
		delete[] global->s_imageBuffer;
		global->s_imageBuffer = nullptr;
	}
	
	delete global;
	global = nullptr;
}

void TelloVideoDecoder_ModifyTexturePixels(void* data, int width, int height)
{
	//debug_log("TelloVideoDecoder_ModifyTexturePixels begin");

	int size = width * height * 4;
	size = min(size, IMAGE_SIZE_IN_BYTE);

	if (global != nullptr) {
		global->s_mutex.lock();
		if (global->s_imageBuffer != nullptr)
			memcpy(data, global->s_imageBuffer, size);
		global->s_mutex.unlock();
	}
	//debug_log("TelloVideoDecoder_ModifyTexturePixels end");
}
