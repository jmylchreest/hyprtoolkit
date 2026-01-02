#include "FFmpegLoader.hpp"
#include "../../core/Logger.hpp"

#include <dlfcn.h>
#include <cstring>
#include <algorithm>

using namespace Hyprtoolkit;

// FFmpeg constants (from libavutil, libavcodec, libavformat)
// These are stable ABI values that won't change
namespace {
    // AVPixelFormat
    constexpr int AV_PIX_FMT_NONE    = -1;
    constexpr int AV_PIX_FMT_YUV420P = 0;
    constexpr int AV_PIX_FMT_RGBA    = 26;
    constexpr int AV_PIX_FMT_VAAPI   = 53;
    constexpr int AV_PIX_FMT_NV12    = 23;

    // AVMediaType
    constexpr int AVMEDIA_TYPE_VIDEO = 0;

    // AVERROR
    constexpr int AVERROR_EOF = -541478725; // FFERRTAG('E','O','F',' ')

    // AV_HWDEVICE_TYPE
    constexpr int AV_HWDEVICE_TYPE_NONE  = 0;
    constexpr int AV_HWDEVICE_TYPE_VAAPI = 4;
    constexpr int AV_HWDEVICE_TYPE_CUDA  = 2;

    // Seek flags
    constexpr int AVSEEK_FLAG_BACKWARD = 1;

    // SWS flags
    constexpr int SWS_BILINEAR = 2;
}

// Minimal FFmpeg struct definitions (only what we need)
// These are stable across FFmpeg versions
struct AVRational {
    int num;
    int den;
};

struct AVCodecParameters;
struct AVCodec;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;
struct AVBufferRef;

// Function pointer types
using avformat_open_input_fn       = int (*)(AVFormatContext**, const char*, void*, void**);
using avformat_find_stream_info_fn = int (*)(AVFormatContext*, void**);
using avformat_close_input_fn      = void (*)(AVFormatContext**);
using av_find_best_stream_fn       = int (*)(AVFormatContext*, int, int, int, const AVCodec**, int);
using av_read_frame_fn             = int (*)(AVFormatContext*, AVPacket*);
using av_seek_frame_fn             = int (*)(AVFormatContext*, int, int64_t, int);

using avcodec_find_decoder_fn          = const AVCodec* (*)(int);
using avcodec_alloc_context3_fn        = AVCodecContext* (*)(const AVCodec*);
using avcodec_parameters_to_context_fn = int (*)(AVCodecContext*, const AVCodecParameters*);
using avcodec_open2_fn                 = int (*)(AVCodecContext*, const AVCodec*, void**);
using avcodec_send_packet_fn           = int (*)(AVCodecContext*, const AVPacket*);
using avcodec_receive_frame_fn         = int (*)(AVCodecContext*, AVFrame*);
using avcodec_free_context_fn          = void (*)(AVCodecContext**);
using avcodec_flush_buffers_fn         = void (*)(AVCodecContext*);

using av_frame_alloc_fn      = AVFrame* (*)();
using av_frame_free_fn       = void (*)(AVFrame**);
using av_frame_get_buffer_fn = int (*)(AVFrame*, int);
using av_packet_alloc_fn     = AVPacket* (*)();
using av_packet_free_fn      = void (*)(AVPacket**);
using av_packet_unref_fn     = void (*)(AVPacket*);

using av_hwdevice_ctx_create_fn    = int (*)(AVBufferRef**, int, const char*, void*, int);
using av_hwdevice_iterate_types_fn = int (*)(int);
using av_hwframe_transfer_data_fn  = int (*)(AVFrame*, const AVFrame*, int);
using av_buffer_unref_fn           = void (*)(AVBufferRef**);

using sws_getContext_fn  = SwsContext* (*)(int, int, int, int, int, int, int, void*, void*, void*);
using sws_scale_fn       = int (*)(SwsContext*, const uint8_t* const*, const int*, int, int, uint8_t* const*, const int*);
using sws_freeContext_fn = void (*)(SwsContext*);

using av_image_get_buffer_size_fn = int (*)(int, int, int, int);
using av_image_fill_arrays_fn     = int (*)(uint8_t**, int*, const uint8_t*, int, int, int, int);

// Store all function pointers
struct CFFmpegLoader::SFunctions {
    // avformat
    avformat_open_input_fn       avformat_open_input       = nullptr;
    avformat_find_stream_info_fn avformat_find_stream_info = nullptr;
    avformat_close_input_fn      avformat_close_input      = nullptr;
    av_find_best_stream_fn       av_find_best_stream       = nullptr;
    av_read_frame_fn             av_read_frame             = nullptr;
    av_seek_frame_fn             av_seek_frame             = nullptr;

    // avcodec
    avcodec_find_decoder_fn          avcodec_find_decoder          = nullptr;
    avcodec_alloc_context3_fn        avcodec_alloc_context3        = nullptr;
    avcodec_parameters_to_context_fn avcodec_parameters_to_context = nullptr;
    avcodec_open2_fn                 avcodec_open2                 = nullptr;
    avcodec_send_packet_fn           avcodec_send_packet           = nullptr;
    avcodec_receive_frame_fn         avcodec_receive_frame         = nullptr;
    avcodec_free_context_fn          avcodec_free_context          = nullptr;
    avcodec_flush_buffers_fn         avcodec_flush_buffers         = nullptr;

    // avutil
    av_frame_alloc_fn            av_frame_alloc            = nullptr;
    av_frame_free_fn             av_frame_free             = nullptr;
    av_frame_get_buffer_fn       av_frame_get_buffer       = nullptr;
    av_packet_alloc_fn           av_packet_alloc           = nullptr;
    av_packet_free_fn            av_packet_free            = nullptr;
    av_packet_unref_fn           av_packet_unref           = nullptr;
    av_hwdevice_ctx_create_fn    av_hwdevice_ctx_create    = nullptr;
    av_hwdevice_iterate_types_fn av_hwdevice_iterate_types = nullptr;
    av_hwframe_transfer_data_fn  av_hwframe_transfer_data  = nullptr;
    av_buffer_unref_fn           av_buffer_unref           = nullptr;
    av_image_get_buffer_size_fn  av_image_get_buffer_size  = nullptr;
    av_image_fill_arrays_fn      av_image_fill_arrays      = nullptr;

    // swscale
    sws_getContext_fn  sws_getContext  = nullptr;
    sws_scale_fn       sws_scale       = nullptr;
    sws_freeContext_fn sws_freeContext = nullptr;
};

// Helper macro for loading symbols
#define LOAD_SYM(lib, name)                                                                                                                                                        \
    m_fn->name = reinterpret_cast<name##_fn>(dlsym(lib, #name));                                                                                                                   \
    if (!m_fn->name) {                                                                                                                                                             \
        g_logger->log(HT_LOG_DEBUG, "FFmpeg: failed to load " #name);                                                                                                              \
        return false;                                                                                                                                                              \
    }

CFFmpegLoader& CFFmpegLoader::instance() {
    static CFFmpegLoader inst;
    return inst;
}

CFFmpegLoader::CFFmpegLoader() {
    m_fn        = new SFunctions();
    m_available = load();

    if (m_available) {
        probeHwAccel();
        g_logger->log(HT_LOG_DEBUG, "FFmpeg: loaded successfully, hw accel: {}", m_hwAccelName.empty() ? "none" : m_hwAccelName);
    } else {
        g_logger->log(HT_LOG_DEBUG, "FFmpeg: not available, video support disabled");
    }
}

CFFmpegLoader::~CFFmpegLoader() {
    if (m_swscale)
        dlclose(m_swscale);
    if (m_avcodec)
        dlclose(m_avcodec);
    if (m_avformat)
        dlclose(m_avformat);
    if (m_avutil)
        dlclose(m_avutil);

    delete m_fn;
}

bool CFFmpegLoader::load() {
    // Try to load libraries - try versioned names first, then unversioned
    const char* avutilNames[]   = {"libavutil.so.59", "libavutil.so.58", "libavutil.so.57", "libavutil.so", nullptr};
    const char* avcodecNames[]  = {"libavcodec.so.61", "libavcodec.so.60", "libavcodec.so.59", "libavcodec.so", nullptr};
    const char* avformatNames[] = {"libavformat.so.61", "libavformat.so.60", "libavformat.so.59", "libavformat.so", nullptr};
    const char* swscaleNames[]  = {"libswscale.so.8", "libswscale.so.7", "libswscale.so.6", "libswscale.so", nullptr};

    auto        tryLoad = [](const char** names) -> void* {
        for (int i = 0; names[i]; ++i) {
            void* h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
            if (h)
                return h;
        }
        return nullptr;
    };

    m_avutil = tryLoad(avutilNames);
    if (!m_avutil) {
        g_logger->log(HT_LOG_DEBUG, "FFmpeg: could not load libavutil");
        return false;
    }

    m_avcodec = tryLoad(avcodecNames);
    if (!m_avcodec) {
        g_logger->log(HT_LOG_DEBUG, "FFmpeg: could not load libavcodec");
        return false;
    }

    m_avformat = tryLoad(avformatNames);
    if (!m_avformat) {
        g_logger->log(HT_LOG_DEBUG, "FFmpeg: could not load libavformat");
        return false;
    }

    m_swscale = tryLoad(swscaleNames);
    if (!m_swscale) {
        g_logger->log(HT_LOG_DEBUG, "FFmpeg: could not load libswscale");
        return false;
    }

    // Load function pointers
    LOAD_SYM(m_avformat, avformat_open_input);
    LOAD_SYM(m_avformat, avformat_find_stream_info);
    LOAD_SYM(m_avformat, avformat_close_input);
    LOAD_SYM(m_avformat, av_find_best_stream);
    LOAD_SYM(m_avformat, av_read_frame);
    LOAD_SYM(m_avformat, av_seek_frame);

    LOAD_SYM(m_avcodec, avcodec_find_decoder);
    LOAD_SYM(m_avcodec, avcodec_alloc_context3);
    LOAD_SYM(m_avcodec, avcodec_parameters_to_context);
    LOAD_SYM(m_avcodec, avcodec_open2);
    LOAD_SYM(m_avcodec, avcodec_send_packet);
    LOAD_SYM(m_avcodec, avcodec_receive_frame);
    LOAD_SYM(m_avcodec, avcodec_free_context);
    LOAD_SYM(m_avcodec, avcodec_flush_buffers);

    LOAD_SYM(m_avutil, av_frame_alloc);
    LOAD_SYM(m_avutil, av_frame_free);
    LOAD_SYM(m_avutil, av_frame_get_buffer);
    LOAD_SYM(m_avutil, av_packet_alloc);
    LOAD_SYM(m_avutil, av_packet_free);
    LOAD_SYM(m_avutil, av_packet_unref);
    LOAD_SYM(m_avutil, av_hwdevice_ctx_create);
    LOAD_SYM(m_avutil, av_hwdevice_iterate_types);
    LOAD_SYM(m_avutil, av_hwframe_transfer_data);
    LOAD_SYM(m_avutil, av_buffer_unref);
    LOAD_SYM(m_avutil, av_image_get_buffer_size);
    LOAD_SYM(m_avutil, av_image_fill_arrays);

    LOAD_SYM(m_swscale, sws_getContext);
    LOAD_SYM(m_swscale, sws_scale);
    LOAD_SYM(m_swscale, sws_freeContext);

    return true;
}

bool CFFmpegLoader::probeHwAccel() {
    if (!m_fn->av_hwdevice_iterate_types)
        return false;

    int type = AV_HWDEVICE_TYPE_NONE;
    while ((type = m_fn->av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        if (type == AV_HWDEVICE_TYPE_VAAPI) {
            m_hwAccelName = "vaapi";
            return true;
        }
        if (type == AV_HWDEVICE_TYPE_CUDA) {
            m_hwAccelName = "cuda";
            return true;
        }
    }

    return false;
}

bool CFFmpegLoader::available() const {
    return m_available;
}

std::string CFFmpegLoader::hwAccelName() const {
    return m_hwAccelName;
}

#undef LOAD_SYM

// ============================================================================
// CFFmpegDecoder implementation
// ============================================================================

// We need access to some FFmpeg struct internals for decoding
// These offsets are stable across FFmpeg 5.x/6.x/7.x
namespace {
    // AVStream field offsets (approximate, we use av_find_best_stream result)
    struct AVStreamCompat {
        int        index;
        int        id;
        void*      codecpar; // AVCodecParameters*
        AVRational time_base;
        // ... more fields we don't need
    };

    // AVCodecParameters key fields
    struct AVCodecParametersCompat {
        int codec_type;
        int codec_id;
        // ... skip to dimensions
        char padding[8];
        int  width;
        int  height;
        // ... more fields
    };

    // AVFrame key fields for accessing data
    struct AVFrameCompat {
        uint8_t*  data[8];
        int       linesize[8];
        uint8_t** extended_data;
        int       width;
        int       height;
        int       nb_samples;
        int       format;
        // ... more fields we access via pointer arithmetic
    };

    // AVFormatContext - we need streams array
    struct AVFormatContextCompat {
        void*    av_class;
        void*    iformat;
        void*    oformat;
        void*    priv_data;
        void*    pb;
        int      ctx_flags;
        uint32_t nb_streams;
        void**   streams; // AVStream**
        // ... more fields, including duration
        char    padding[32];
        int64_t duration; // in AV_TIME_BASE units
    };

    // AVCodecContext - we need hw_device_ctx
    struct AVCodecContextCompat {
        void* av_class;
        int   log_level_offset;
        int   codec_type;
        void* codec;
        int   codec_id;
        // ... lots of fields, hw_device_ctx is far in
    };

    constexpr int64_t AV_TIME_BASE = 1000000;
}

CFFmpegDecoder::CFFmpegDecoder(const std::string& path) {
    auto& loader = CFFmpegLoader::instance();
    if (!loader.available()) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: FFmpeg not available");
        return;
    }

    auto* fn = loader.m_fn;

    // Open input file
    AVFormatContext* fmtCtx = nullptr;
    if (fn->avformat_open_input(&fmtCtx, path.c_str(), nullptr, nullptr) < 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not open '{}'", path);
        return;
    }
    m_formatCtx = fmtCtx;

    // Find stream info
    if (fn->avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not find stream info");
        cleanup();
        return;
    }

    // Find best video stream
    const AVCodec* codec = nullptr;
    m_streamIdx          = fn->av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (m_streamIdx < 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: no video stream found");
        cleanup();
        return;
    }

    // Get stream info
    auto* fmtCompat = reinterpret_cast<AVFormatContextCompat*>(fmtCtx);
    auto* stream    = reinterpret_cast<AVStreamCompat*>(fmtCompat->streams[m_streamIdx]);
    auto* codecpar  = reinterpret_cast<AVCodecParametersCompat*>(stream->codecpar);

    m_size.x   = codecpar->width;
    m_size.y   = codecpar->height;
    m_duration = static_cast<double>(fmtCompat->duration) / AV_TIME_BASE;

    // Calculate FPS from time_base
    if (stream->time_base.den > 0 && stream->time_base.num > 0)
        m_fps = static_cast<double>(stream->time_base.den) / stream->time_base.num;
    else
        m_fps = 30.0;

    // Clamp FPS to reasonable range
    m_fps = std::clamp(m_fps, 1.0, 240.0);

    // Allocate codec context
    AVCodecContext* codecCtx = fn->avcodec_alloc_context3(codec);
    if (!codecCtx) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not allocate codec context");
        cleanup();
        return;
    }
    m_codecCtx = codecCtx;

    if (fn->avcodec_parameters_to_context(codecCtx, reinterpret_cast<AVCodecParameters*>(stream->codecpar)) < 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not copy codec params");
        cleanup();
        return;
    }

    // Try hardware acceleration
    if (!loader.hwAccelName().empty())
        setupHwAccel();

    // Open codec
    if (fn->avcodec_open2(codecCtx, codec, nullptr) < 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not open codec");
        cleanup();
        return;
    }

    // Allocate frames
    m_frame    = fn->av_frame_alloc();
    m_frameRgb = fn->av_frame_alloc();
    m_packet   = fn->av_packet_alloc();

    if (!m_frame || !m_frameRgb || !m_packet) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not allocate frames/packet");
        cleanup();
        return;
    }

    // Setup color conversion
    if (!setupSwsContext()) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not setup color conversion");
        cleanup();
        return;
    }

    // Allocate RGBA buffer
    int bufSize = static_cast<int>(m_size.x * m_size.y * 4);
    m_frameBuffer.resize(bufSize);

    m_valid = true;
    g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: opened '{}' {}x{} @ {:.1f}fps, duration: {:.1f}s, hw: {}", path, static_cast<int>(m_size.x), static_cast<int>(m_size.y), m_fps,
                  m_duration, m_useHwDec ? "yes" : "no");
}

CFFmpegDecoder::~CFFmpegDecoder() {
    cleanup();
}

bool CFFmpegDecoder::setupHwAccel() {
    auto& loader = CFFmpegLoader::instance();
    auto* fn     = loader.m_fn;

    int   hwType = AV_HWDEVICE_TYPE_NONE;
    if (loader.hwAccelName() == "vaapi")
        hwType = AV_HWDEVICE_TYPE_VAAPI;
    else if (loader.hwAccelName() == "cuda")
        hwType = AV_HWDEVICE_TYPE_CUDA;
    else
        return false;

    AVBufferRef* hwCtx = nullptr;
    if (fn->av_hwdevice_ctx_create(&hwCtx, hwType, nullptr, nullptr, 0) < 0) {
        g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: hw accel init failed, using software decode");
        return false;
    }

    m_hwDeviceCtx = hwCtx;
    m_useHwDec    = true;

    // Set hw_device_ctx on codec context - this requires knowing the offset
    // For now, we'll skip direct hw context assignment and rely on auto-detection
    // TODO: properly set codecCtx->hw_device_ctx = av_buffer_ref(hwCtx)

    return true;
}

bool CFFmpegDecoder::setupSwsContext() {
    auto& loader = CFFmpegLoader::instance();
    auto* fn     = loader.m_fn;

    // Create software scaler for YUV->RGBA conversion
    m_swsCtx = fn->sws_getContext(static_cast<int>(m_size.x), static_cast<int>(m_size.y), AV_PIX_FMT_YUV420P, static_cast<int>(m_size.x), static_cast<int>(m_size.y),
                                  AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);

    return m_swsCtx != nullptr;
}

void CFFmpegDecoder::cleanup() {
    auto& loader = CFFmpegLoader::instance();
    if (!loader.available())
        return;

    auto* fn = loader.m_fn;

    if (m_swsCtx) {
        fn->sws_freeContext(reinterpret_cast<SwsContext*>(m_swsCtx));
        m_swsCtx = nullptr;
    }

    if (m_packet) {
        fn->av_packet_free(reinterpret_cast<AVPacket**>(&m_packet));
        m_packet = nullptr;
    }

    if (m_frameRgb) {
        fn->av_frame_free(reinterpret_cast<AVFrame**>(&m_frameRgb));
        m_frameRgb = nullptr;
    }

    if (m_frame) {
        fn->av_frame_free(reinterpret_cast<AVFrame**>(&m_frame));
        m_frame = nullptr;
    }

    if (m_hwDeviceCtx) {
        fn->av_buffer_unref(reinterpret_cast<AVBufferRef**>(&m_hwDeviceCtx));
        m_hwDeviceCtx = nullptr;
    }

    if (m_codecCtx) {
        fn->avcodec_free_context(reinterpret_cast<AVCodecContext**>(&m_codecCtx));
        m_codecCtx = nullptr;
    }

    if (m_formatCtx) {
        fn->avformat_close_input(reinterpret_cast<AVFormatContext**>(&m_formatCtx));
        m_formatCtx = nullptr;
    }

    m_valid = false;
}

bool CFFmpegDecoder::decodeNextFrame() {
    if (!m_valid || m_atEnd)
        return false;

    auto& loader = CFFmpegLoader::instance();
    auto* fn     = loader.m_fn;

    auto* fmtCtx   = reinterpret_cast<AVFormatContext*>(m_formatCtx);
    auto* codecCtx = reinterpret_cast<AVCodecContext*>(m_codecCtx);
    auto* frame    = reinterpret_cast<AVFrame*>(m_frame);
    auto* packet   = reinterpret_cast<AVPacket*>(m_packet);

    while (true) {
        int ret = fn->av_read_frame(fmtCtx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                m_atEnd = true;
            return false;
        }

        // Check if packet is from our video stream
        // AVPacket has stream_index at offset 4 (after pts which is int64_t at 0)
        int streamIdx = *reinterpret_cast<int*>(reinterpret_cast<char*>(packet) + 8);
        if (streamIdx != m_streamIdx) {
            fn->av_packet_unref(packet);
            continue;
        }

        // Send packet to decoder
        ret = fn->avcodec_send_packet(codecCtx, packet);
        fn->av_packet_unref(packet);

        if (ret < 0)
            continue;

        // Receive decoded frame
        ret = fn->avcodec_receive_frame(codecCtx, frame);
        if (ret < 0)
            continue;

        // Convert to RGBA
        auto*    frameCompat = reinterpret_cast<AVFrameCompat*>(frame);
        auto*    swsCtx      = reinterpret_cast<SwsContext*>(m_swsCtx);

        uint8_t* dstData[1]     = {m_frameBuffer.data()};
        int      dstLinesize[1] = {static_cast<int>(m_size.x) * 4};

        fn->sws_scale(swsCtx, frameCompat->data, frameCompat->linesize, 0, static_cast<int>(m_size.y), dstData, dstLinesize);

        return true;
    }
}

const std::vector<uint8_t>& CFFmpegDecoder::frameData() const {
    return m_frameBuffer;
}

void CFFmpegDecoder::seek(double seconds) {
    if (!m_valid)
        return;

    auto&   loader = CFFmpegLoader::instance();
    auto*   fn     = loader.m_fn;

    auto*   fmtCtx   = reinterpret_cast<AVFormatContext*>(m_formatCtx);
    auto*   codecCtx = reinterpret_cast<AVCodecContext*>(m_codecCtx);

    int64_t timestamp = static_cast<int64_t>(seconds * AV_TIME_BASE);
    fn->av_seek_frame(fmtCtx, -1, timestamp, AVSEEK_FLAG_BACKWARD);
    fn->avcodec_flush_buffers(codecCtx);

    m_atEnd = false;
}

bool CFFmpegDecoder::atEnd() const {
    return m_atEnd;
}

void CFFmpegDecoder::rewind() {
    seek(0.0);
}

bool CFFmpegDecoder::valid() const {
    return m_valid;
}

Hyprutils::Math::Vector2D CFFmpegDecoder::size() const {
    return m_size;
}

double CFFmpegDecoder::fps() const {
    return m_fps;
}

double CFFmpegDecoder::duration() const {
    return m_duration;
}
