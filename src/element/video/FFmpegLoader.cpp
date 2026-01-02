#include "FFmpegLoader.hpp"
#include "../../core/Logger.hpp"

#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

using namespace Hyprtoolkit;

#ifndef HT_VIDEO_SUPPORT
// ============================================================================
// Stub implementations when FFmpeg headers not available at compile time
// ============================================================================

struct CFFmpegLoader::SFunctions {};

CFFmpegLoader& CFFmpegLoader::instance() {
    static CFFmpegLoader inst;
    return inst;
}

CFFmpegLoader::CFFmpegLoader() : m_fn(new SFunctions()) {
    g_logger->log(HT_LOG_DEBUG, "FFmpeg: video support not compiled in");
}

CFFmpegLoader::~CFFmpegLoader() {
    delete m_fn;
}

bool CFFmpegLoader::load() { return false; }
bool CFFmpegLoader::probeHwAccel() { return false; }
bool CFFmpegLoader::available() const { return false; }
std::string CFFmpegLoader::hwAccelName() const { return ""; }
unsigned CFFmpegLoader::versionMajor() const { return 0; }
bool CFFmpegLoader::dmaBufExportSupported() const { return false; }

CFFmpegDecoder::CFFmpegDecoder(const std::string&) {
    g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: video support not compiled in");
}
CFFmpegDecoder::~CFFmpegDecoder() {}
bool CFFmpegDecoder::valid() const { return false; }
Hyprutils::Math::Vector2D CFFmpegDecoder::size() const { return {}; }
double CFFmpegDecoder::fps() const { return 0; }
double CFFmpegDecoder::duration() const { return 0; }
bool CFFmpegDecoder::decodeNextFrame() { return false; }
const std::vector<uint8_t>& CFFmpegDecoder::frameData() const { return m_frameBuffer; }
SDmaBufFrame CFFmpegDecoder::exportFrameDmaBuf() { return {}; }
bool CFFmpegDecoder::dmaBufExportAvailable() const { return false; }
void CFFmpegDecoder::seek(double) {}
bool CFFmpegDecoder::atEnd() const { return true; }
void CFFmpegDecoder::rewind() {}
bool CFFmpegDecoder::setupHwAccel() { return false; }
bool CFFmpegDecoder::setupSwsContext() { return false; }
void CFFmpegDecoder::cleanup() {}

#else
// ============================================================================
// Full FFmpeg implementation when headers are available
// ============================================================================

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// DRM format definitions for DMA-BUF
#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564E
#endif
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 0x30313050
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

// VAAPI function types for DMA-BUF export
using VADisplay = void*;
using VASurfaceID = unsigned int;

// VADRMPRIMESurfaceDescriptor for vaExportSurfaceHandle
struct VADRMPRIMESurfaceDescriptor {
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint32_t num_objects;
    struct {
        int      fd;
        uint32_t size;
        uint64_t drm_format_modifier;
    } objects[4];
    uint32_t num_layers;
    struct {
        uint32_t drm_format;
        uint32_t num_planes;
        uint32_t object_index[4];
        uint32_t offset[4];
        uint32_t pitch[4];
    } layers[4];
};

#define VA_EXPORT_SURFACE_READ_ONLY        0x0001
#define VA_EXPORT_SURFACE_WRITE_ONLY       0x0002
#define VA_EXPORT_SURFACE_READ_WRITE       0x0003
#define VA_EXPORT_SURFACE_SEPARATE_LAYERS  0x0004
#define VA_EXPORT_SURFACE_COMPOSED_LAYERS  0x0008

#define VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 0x40000000

using vaExportSurfaceHandle_fn = int (*)(VADisplay, VASurfaceID, uint32_t, uint32_t, void*);
using vaSyncSurface_fn = int (*)(VADisplay, VASurfaceID);

// Function pointer types
using avformat_open_input_fn       = int (*)(AVFormatContext**, const char*, void*, void**);
using avformat_find_stream_info_fn = int (*)(AVFormatContext*, void**);
using avformat_close_input_fn      = void (*)(AVFormatContext**);
using av_find_best_stream_fn       = int (*)(AVFormatContext*, int, int, int, const AVCodec**, int);
using av_read_frame_fn             = int (*)(AVFormatContext*, AVPacket*);
using av_seek_frame_fn             = int (*)(AVFormatContext*, int, int64_t, int);

using avcodec_version_fn               = unsigned (*)();
using avcodec_alloc_context3_fn        = AVCodecContext* (*)(const AVCodec*);
using avcodec_parameters_to_context_fn = int (*)(AVCodecContext*, const AVCodecParameters*);
using avcodec_open2_fn                 = int (*)(AVCodecContext*, const AVCodec*, void**);
using avcodec_send_packet_fn           = int (*)(AVCodecContext*, const AVPacket*);
using avcodec_receive_frame_fn         = int (*)(AVCodecContext*, AVFrame*);
using avcodec_free_context_fn          = void (*)(AVCodecContext**);
using avcodec_flush_buffers_fn         = void (*)(AVCodecContext*);
using avcodec_get_hw_config_fn = const AVCodecHWConfig* (*)(const AVCodec*, int);
using av_codec_iterate_fn      = const AVCodec* (*)(void**);
using av_codec_is_decoder_fn   = int (*)(const AVCodec*);

using av_frame_alloc_fn  = AVFrame* (*)();
using av_frame_free_fn   = void (*)(AVFrame**);
using av_frame_unref_fn  = void (*)(AVFrame*);
using av_packet_alloc_fn = AVPacket* (*)();
using av_packet_free_fn  = void (*)(AVPacket**);
using av_packet_unref_fn = void (*)(AVPacket*);

using av_hwdevice_ctx_create_fn    = int (*)(AVBufferRef**, int, const char*, void*, int);
using av_hwdevice_iterate_types_fn = int (*)(int);
using av_buffer_ref_fn             = AVBufferRef* (*)(const AVBufferRef*);
using av_buffer_unref_fn           = void (*)(AVBufferRef**);
using av_hwframe_transfer_data_fn  = int (*)(AVFrame*, const AVFrame*, int);

using sws_getContext_fn  = SwsContext* (*)(int, int, int, int, int, int, int, void*, void*, void*);
using sws_scale_fn       = int (*)(SwsContext*, const uint8_t* const*, const int*, int, int, uint8_t* const*, const int*);
using sws_freeContext_fn = void (*)(SwsContext*);

// Minimum supported libavcodec major version (FFmpeg 6.0+)
constexpr unsigned MIN_LIBAVCODEC_VERSION_MAJOR = 60;

// Store all function pointers
struct CFFmpegLoader::SFunctions {
    // version
    avcodec_version_fn avcodec_version = nullptr;

    // avformat
    avformat_open_input_fn       avformat_open_input       = nullptr;
    avformat_find_stream_info_fn avformat_find_stream_info = nullptr;
    avformat_close_input_fn      avformat_close_input      = nullptr;
    av_find_best_stream_fn       av_find_best_stream       = nullptr;
    av_read_frame_fn             av_read_frame             = nullptr;
    av_seek_frame_fn             av_seek_frame             = nullptr;

    // avcodec
    avcodec_alloc_context3_fn        avcodec_alloc_context3        = nullptr;
    avcodec_parameters_to_context_fn avcodec_parameters_to_context = nullptr;
    avcodec_open2_fn                 avcodec_open2                 = nullptr;
    avcodec_send_packet_fn           avcodec_send_packet           = nullptr;
    avcodec_receive_frame_fn         avcodec_receive_frame         = nullptr;
    avcodec_free_context_fn          avcodec_free_context          = nullptr;
    avcodec_flush_buffers_fn         avcodec_flush_buffers         = nullptr;
    avcodec_get_hw_config_fn         avcodec_get_hw_config         = nullptr;
    av_codec_iterate_fn              av_codec_iterate              = nullptr;
    av_codec_is_decoder_fn           av_codec_is_decoder           = nullptr;
    av_packet_alloc_fn               av_packet_alloc               = nullptr;
    av_packet_free_fn                av_packet_free                = nullptr;
    av_packet_unref_fn               av_packet_unref               = nullptr;

    // avutil
    av_frame_alloc_fn            av_frame_alloc            = nullptr;
    av_frame_free_fn             av_frame_free             = nullptr;
    av_frame_unref_fn            av_frame_unref            = nullptr;
    av_hwdevice_ctx_create_fn    av_hwdevice_ctx_create    = nullptr;
    av_hwdevice_iterate_types_fn av_hwdevice_iterate_types = nullptr;
    av_buffer_ref_fn             av_buffer_ref             = nullptr;
    av_buffer_unref_fn           av_buffer_unref           = nullptr;
    av_hwframe_transfer_data_fn  av_hwframe_transfer_data  = nullptr;

    // swscale
    sws_getContext_fn  sws_getContext  = nullptr;
    sws_scale_fn       sws_scale       = nullptr;
    sws_freeContext_fn sws_freeContext = nullptr;

    // libva (for DMA-BUF export)
    vaExportSurfaceHandle_fn vaExportSurfaceHandle = nullptr;
    vaSyncSurface_fn         vaSyncSurface         = nullptr;
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
    if (m_libva)
        dlclose(m_libva);
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

    // Load version function first to check compatibility
    LOAD_SYM(m_avcodec, avcodec_version);

    // Check FFmpeg version - we need libavcodec >= 60 (FFmpeg 6.0+)
    unsigned version      = m_fn->avcodec_version();
    unsigned versionMajor = version >> 16;
    unsigned versionMinor = (version >> 8) & 0xFF;
    m_versionMajor        = versionMajor;

    if (versionMajor < MIN_LIBAVCODEC_VERSION_MAJOR) {
        g_logger->log(HT_LOG_WARNING, "FFmpeg: libavcodec {}.{} too old, need >= {}.0 (FFmpeg 6.0+)", versionMajor, versionMinor, MIN_LIBAVCODEC_VERSION_MAJOR);
        return false;
    }

    g_logger->log(HT_LOG_DEBUG, "FFmpeg: libavcodec version {}.{}", versionMajor, versionMinor);

    // Load function pointers
    LOAD_SYM(m_avformat, avformat_open_input);
    LOAD_SYM(m_avformat, avformat_find_stream_info);
    LOAD_SYM(m_avformat, avformat_close_input);
    LOAD_SYM(m_avformat, av_find_best_stream);
    LOAD_SYM(m_avformat, av_read_frame);
    LOAD_SYM(m_avformat, av_seek_frame);

    LOAD_SYM(m_avcodec, avcodec_alloc_context3);
    LOAD_SYM(m_avcodec, avcodec_parameters_to_context);
    LOAD_SYM(m_avcodec, avcodec_open2);
    LOAD_SYM(m_avcodec, avcodec_send_packet);
    LOAD_SYM(m_avcodec, avcodec_receive_frame);
    LOAD_SYM(m_avcodec, avcodec_free_context);
    LOAD_SYM(m_avcodec, avcodec_flush_buffers);
    LOAD_SYM(m_avcodec, av_packet_alloc);
    LOAD_SYM(m_avcodec, av_packet_free);
    LOAD_SYM(m_avcodec, av_packet_unref);

    // Optional: needed for HW decoder selection
    m_fn->avcodec_get_hw_config = reinterpret_cast<avcodec_get_hw_config_fn>(dlsym(m_avcodec, "avcodec_get_hw_config"));
    m_fn->av_codec_iterate      = reinterpret_cast<av_codec_iterate_fn>(dlsym(m_avcodec, "av_codec_iterate"));
    m_fn->av_codec_is_decoder   = reinterpret_cast<av_codec_is_decoder_fn>(dlsym(m_avcodec, "av_codec_is_decoder"));

    LOAD_SYM(m_avutil, av_frame_alloc);
    LOAD_SYM(m_avutil, av_frame_free);
    LOAD_SYM(m_avutil, av_frame_unref);
    LOAD_SYM(m_avutil, av_hwdevice_ctx_create);
    LOAD_SYM(m_avutil, av_hwdevice_iterate_types);
    LOAD_SYM(m_avutil, av_buffer_ref);
    LOAD_SYM(m_avutil, av_buffer_unref);
    LOAD_SYM(m_avutil, av_hwframe_transfer_data);

    LOAD_SYM(m_swscale, sws_getContext);
    LOAD_SYM(m_swscale, sws_scale);
    LOAD_SYM(m_swscale, sws_freeContext);

    // Try to load libva for DMA-BUF export (optional)
    const char* libvaNames[] = {"libva.so.2", "libva.so", nullptr};
    m_libva                  = tryLoad(libvaNames);
    if (m_libva) {
        m_fn->vaExportSurfaceHandle = reinterpret_cast<vaExportSurfaceHandle_fn>(dlsym(m_libva, "vaExportSurfaceHandle"));
        m_fn->vaSyncSurface         = reinterpret_cast<vaSyncSurface_fn>(dlsym(m_libva, "vaSyncSurface"));

        if (m_fn->vaExportSurfaceHandle && m_fn->vaSyncSurface) {
            m_dmaBufExportSupported = true;
            g_logger->log(HT_LOG_DEBUG, "FFmpeg: libva loaded, DMA-BUF export available");
        } else {
            g_logger->log(HT_LOG_DEBUG, "FFmpeg: libva loaded but vaExportSurfaceHandle not available");
        }
    } else {
        g_logger->log(HT_LOG_DEBUG, "FFmpeg: libva not available, DMA-BUF export disabled");
    }

    return true;
}

bool CFFmpegLoader::probeHwAccel() {
    // Try VAAPI first (Intel/AMD), then CUDA (NVIDIA)
    static const std::pair<int, const char*> hwTypes[] = {
        {AV_HWDEVICE_TYPE_VAAPI, "vaapi"},
        {AV_HWDEVICE_TYPE_CUDA, "cuda"},
    };

    for (const auto& [hwType, name] : hwTypes) {
        AVBufferRef* testCtx = nullptr;
        if (m_fn->av_hwdevice_ctx_create(&testCtx, hwType, nullptr, nullptr, 0) >= 0) {
            m_fn->av_buffer_unref(&testCtx);
            m_hwAccelName = name;
            g_logger->log(HT_LOG_DEBUG, "FFmpeg: found hw accel: {}", name);
            return true;
        }
    }

    g_logger->log(HT_LOG_DEBUG, "FFmpeg: no hw accel available, using software decode");
    return false;
}

bool CFFmpegLoader::available() const {
    return m_available;
}

std::string CFFmpegLoader::hwAccelName() const {
    return m_hwAccelName;
}

unsigned CFFmpegLoader::versionMajor() const {
    return m_versionMajor;
}

bool CFFmpegLoader::dmaBufExportSupported() const {
    return m_dmaBufExportSupported;
}

#undef LOAD_SYM

// Hardware accelerated get_format callback
// Returns VAAPI format if available, otherwise falls back to software formats
static AVPixelFormat getHwFormat(AVCodecContext* ctx, const AVPixelFormat* pixFmts) {
    for (const AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return AV_PIX_FMT_VAAPI;
    }
    return pixFmts[0];
}

// Check if codec supports VAAPI hardware acceleration via hw_device_ctx
static bool codecSupportsVaapi(const AVCodec* codec, avcodec_get_hw_config_fn getHwConfig) {
    if (!getHwConfig)
        return false;

    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = getHwConfig(codec, i);
        if (!config)
            break;

        // Check for VAAPI support via hw_device_ctx
        if (config->device_type == AV_HWDEVICE_TYPE_VAAPI && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            return true;
    }
    return false;
}

// Find a decoder that supports VAAPI hardware acceleration for the given codec ID
// Iterates through all registered codecs to find one with VAAPI support
// Returns the original codec if no HW-capable alternative is found
static const AVCodec* findHwCapableDecoder(AVCodecID codecId, const AVCodec* originalCodec,
                                           av_codec_iterate_fn codecIterate,
                                           av_codec_is_decoder_fn isDecoder,
                                           avcodec_get_hw_config_fn getHwConfig) {
    // If the original codec supports VAAPI, use it
    if (codecSupportsVaapi(originalCodec, getHwConfig)) {
        g_logger->log(HT_LOG_DEBUG, "findHwCapableDecoder: '{}' supports VAAPI", originalCodec->name);
        return originalCodec;
    }

    if (!codecIterate || !isDecoder || !getHwConfig) {
        g_logger->log(HT_LOG_DEBUG, "findHwCapableDecoder: codec iteration not available");
        return originalCodec;
    }

    // Iterate through all codecs to find a decoder with VAAPI support
    void*          opaque = nullptr;
    const AVCodec* codec  = nullptr;
    while ((codec = codecIterate(&opaque)) != nullptr) {
        // Skip if not our codec ID or not a decoder
        if (codec->id != codecId || !isDecoder(codec))
            continue;

        // Skip the original codec (already checked)
        if (codec == originalCodec)
            continue;

        // Check if this decoder supports VAAPI
        if (codecSupportsVaapi(codec, getHwConfig)) {
            g_logger->log(HT_LOG_DEBUG, "findHwCapableDecoder: using '{}' instead of '{}' for VAAPI support",
                          codec->name, originalCodec->name);
            return codec;
        }
    }

    g_logger->log(HT_LOG_DEBUG, "findHwCapableDecoder: no VAAPI-capable decoder found for codec id {}", static_cast<int>(codecId));
    return originalCodec;
}

// ============================================================================
// CFFmpegDecoder implementation
// ============================================================================

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

    // Find best video stream - this also returns the codec
    const AVCodec* codec = nullptr;
    m_streamIdx          = fn->av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (m_streamIdx < 0 || !codec) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: no video stream found");
        cleanup();
        return;
    }
    g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: stream codec '{}' (id={})", codec->name, static_cast<int>(codec->id));

    // If HW accel is available, try to find a decoder that supports it
    // (e.g., prefer native 'av1' over 'libdav1d' for VAAPI support)
    if (!loader.hwAccelName().empty()) {
        AVCodecID codecId = fmtCtx->streams[m_streamIdx]->codecpar->codec_id;
        codec             = findHwCapableDecoder(codecId, codec, fn->av_codec_iterate, fn->av_codec_is_decoder, fn->avcodec_get_hw_config);
    }
    g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: using codec '{}'", codec->name);

    // Allocate codec context
    AVCodecContext* codecCtx = fn->avcodec_alloc_context3(codec);
    if (!codecCtx) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not allocate codec context");
        cleanup();
        return;
    }
    m_codecCtx = codecCtx;

    // Get codec parameters from stream (needed for SPS/PPS extradata etc)
    AVStream* stream = fmtCtx->streams[m_streamIdx];

    if (fn->avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not copy codec parameters");
        cleanup();
        return;
    }

    // Try to set up hardware acceleration
    if (setupHwAccel()) {
        codecCtx->hw_device_ctx = fn->av_buffer_ref(reinterpret_cast<AVBufferRef*>(m_hwDeviceCtx));
        codecCtx->get_format    = getHwFormat;
        g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: using hw accel: {}", loader.hwAccelName());
    }

    // Open codec
    if (fn->avcodec_open2(codecCtx, codec, nullptr) < 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not open codec");
        cleanup();
        return;
    }

    // Allocate frames and packet
    m_frame  = fn->av_frame_alloc();
    m_packet = fn->av_packet_alloc();

    if (!m_frame || !m_packet) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not allocate frame/packet");
        cleanup();
        return;
    }

    // Decode first frame to get actual dimensions
    // This is the safest way to get video size without accessing struct internals
    AVFrame*  frame  = reinterpret_cast<AVFrame*>(m_frame);
    AVPacket* packet = reinterpret_cast<AVPacket*>(m_packet);

    bool gotFrame = false;
    while (!gotFrame) {
        int ret = fn->av_read_frame(fmtCtx, packet);
        if (ret < 0) {
            g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not read first frame");
            cleanup();
            return;
        }

        if (packet->stream_index != m_streamIdx) {
            fn->av_packet_unref(packet);
            continue;
        }

        ret = fn->avcodec_send_packet(codecCtx, packet);
        fn->av_packet_unref(packet);

        if (ret < 0) {
            g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: error sending packet");
            cleanup();
            return;
        }

        ret = fn->avcodec_receive_frame(codecCtx, frame);
        if (ret == 0) {
            gotFrame = true;
        } else if (ret != AVERROR_EOF && ret != -11 /* EAGAIN */) {
            g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: error receiving frame");
            cleanup();
            return;
        }
    }

    // Get dimensions from decoded frame
    m_size.x = frame->width;
    m_size.y = frame->height;

    if (m_size.x <= 0 || m_size.y <= 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: invalid video dimensions {}x{}", static_cast<int>(m_size.x), static_cast<int>(m_size.y));
        cleanup();
        return;
    }

    // Check if we actually got hardware frames
    if (m_useHwDec && (frame->format == AV_PIX_FMT_VAAPI || frame->format == AV_PIX_FMT_CUDA)) {
        // HW decoded - transfer first frame to get CPU pixel format
        m_swFrame = fn->av_frame_alloc();
        auto* swFrame = reinterpret_cast<AVFrame*>(m_swFrame);
        if (fn->av_hwframe_transfer_data(swFrame, frame, 0) < 0) {
            g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: failed to transfer initial HW frame");
            cleanup();
            return;
        }
        m_pixelFormat = swFrame->format;
        g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: using HW decode, frame format={}", static_cast<int>(frame->format));
    } else {
        // Codec doesn't actually produce HW frames (e.g., libdav1d with VAAPI device set)
        // Disable DMA-BUF export since we won't have VAAPI surfaces
        if (m_dmaBufAvailable) {
            g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: codec '{}' doesn't produce HW frames (format={}), disabling DMA-BUF",
                          codec->name, static_cast<int>(frame->format));
            m_dmaBufAvailable = false;
        }
        m_pixelFormat = frame->format;
    }

    // Default FPS - we'll use 30fps as a fallback
    m_fps      = 30.0;
    m_duration = 0.0;

    // If DMA-BUF export is available, skip CPU color conversion setup
    if (!m_dmaBufAvailable) {
        // Setup color conversion
        if (!setupSwsContext()) {
            g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: could not setup color conversion");
            cleanup();
            return;
        }

        // Allocate RGBA buffer
        int bufSize = static_cast<int>(m_size.x * m_size.y * 4);
        m_frameBuffer.resize(bufSize);

        // Convert the first frame we already decoded
        // Use swFrame for HW decode (contains CPU data), otherwise use frame directly
        AVFrame* srcFrame = m_useHwDec ? reinterpret_cast<AVFrame*>(m_swFrame) : frame;

        auto*    swsCtx         = reinterpret_cast<SwsContext*>(m_swsCtx);
        uint8_t* dstData[4]     = {m_frameBuffer.data(), nullptr, nullptr, nullptr};
        int      dstLinesize[4] = {static_cast<int>(m_size.x) * 4, 0, 0, 0};

        fn->sws_scale(swsCtx, srcFrame->data, srcFrame->linesize, 0, static_cast<int>(m_size.y), dstData, dstLinesize);
    }

    // Seek back to start for normal playback
    fn->av_seek_frame(fmtCtx, -1, 0, AVSEEK_FLAG_BACKWARD);
    fn->avcodec_flush_buffers(codecCtx);

    m_valid = true;
    g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: opened '{}' {}x{} @ {:.1f}fps", path, static_cast<int>(m_size.x), static_cast<int>(m_size.y), m_fps);
}

CFFmpegDecoder::~CFFmpegDecoder() {
    cleanup();
}

bool CFFmpegDecoder::setupHwAccel() {
    auto& loader = CFFmpegLoader::instance();
    auto* fn     = loader.m_fn;

    int   hwType = AV_HWDEVICE_TYPE_NONE;
    bool  isVaapi = false;
    if (loader.hwAccelName() == "vaapi") {
        hwType  = AV_HWDEVICE_TYPE_VAAPI;
        isVaapi = true;
    } else if (loader.hwAccelName() == "cuda") {
        hwType = AV_HWDEVICE_TYPE_CUDA;
    } else {
        return false;
    }

    AVBufferRef* hwCtx = nullptr;
    if (fn->av_hwdevice_ctx_create(&hwCtx, hwType, nullptr, nullptr, 0) < 0) {
        g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: hw accel init failed, using software decode");
        return false;
    }

    m_hwDeviceCtx = hwCtx;
    m_useHwDec    = true;

    // For VAAPI, extract VADisplay for DMA-BUF export
    if (isVaapi && loader.dmaBufExportSupported()) {
        auto* hwDeviceCtx = reinterpret_cast<AVHWDeviceContext*>(hwCtx->data);
        if (hwDeviceCtx && hwDeviceCtx->type == AV_HWDEVICE_TYPE_VAAPI) {
            auto* vaapiDeviceCtx = reinterpret_cast<AVVAAPIDeviceContext*>(hwDeviceCtx->hwctx);
            if (vaapiDeviceCtx) {
                m_vaDisplay       = reinterpret_cast<void*>(vaapiDeviceCtx->display);
                m_dmaBufAvailable = true;
                g_logger->log(HT_LOG_DEBUG, "FFmpegDecoder: VAAPI DMA-BUF export enabled");
            }
        }
    }

    return true;
}

bool CFFmpegDecoder::setupSwsContext() {
    auto& loader = CFFmpegLoader::instance();
    auto* fn     = loader.m_fn;

    // Use actual pixel format from decoded frame (default to YUV420P if not set)
    int srcFormat = m_pixelFormat >= 0 ? m_pixelFormat : AV_PIX_FMT_YUV420P;

    // Create software scaler for pixel format -> BGRA conversion
    // Cairo expects ARGB32 which is BGRA on little-endian systems
    m_swsCtx = fn->sws_getContext(static_cast<int>(m_size.x), static_cast<int>(m_size.y), static_cast<AVPixelFormat>(srcFormat), static_cast<int>(m_size.x),
                                  static_cast<int>(m_size.y), AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!m_swsCtx)
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: failed to create sws context for pixel format {}", srcFormat);

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

    if (m_swFrame) {
        fn->av_frame_free(reinterpret_cast<AVFrame**>(&m_swFrame));
        m_swFrame = nullptr;
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
        if (packet->stream_index != m_streamIdx) {
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

        // If DMA-BUF export is available, skip CPU conversion - just keep frame decoded
        if (m_dmaBufAvailable)
            return true;

        // Determine which frame to use for scaling
        AVFrame* srcFrame = frame;

        // If HW decoded, transfer from GPU to CPU
        if (m_useHwDec && (frame->format == AV_PIX_FMT_VAAPI || frame->format == AV_PIX_FMT_CUDA)) {
            if (!m_swFrame)
                m_swFrame = fn->av_frame_alloc();

            auto* swFrame = reinterpret_cast<AVFrame*>(m_swFrame);
            if (fn->av_hwframe_transfer_data(swFrame, frame, 0) < 0) {
                g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: failed to transfer HW frame to CPU");
                continue;
            }
            srcFrame = swFrame;

            // Update pixel format from transferred frame if needed
            if (m_pixelFormat != srcFrame->format) {
                m_pixelFormat = srcFrame->format;
                if (m_swsCtx) {
                    fn->sws_freeContext(reinterpret_cast<SwsContext*>(m_swsCtx));
                    m_swsCtx = nullptr;
                }
                setupSwsContext();
            }
        }

        auto*    swsCtx         = reinterpret_cast<SwsContext*>(m_swsCtx);
        uint8_t* dstData[1]     = {m_frameBuffer.data()};
        int      dstLinesize[1] = {static_cast<int>(m_size.x) * 4};

        fn->sws_scale(swsCtx, srcFrame->data, srcFrame->linesize, 0, static_cast<int>(m_size.y), dstData, dstLinesize);

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

bool CFFmpegDecoder::dmaBufExportAvailable() const {
    return m_dmaBufAvailable;
}

SDmaBufFrame CFFmpegDecoder::exportFrameDmaBuf() {
    SDmaBufFrame result;

    if (!m_valid || !m_dmaBufAvailable || !m_vaDisplay) {
        g_logger->log(HT_LOG_DEBUG, "exportFrameDmaBuf: early return - valid={} dmaBuf={} vaDisplay={}",
                      m_valid, m_dmaBufAvailable, m_vaDisplay != nullptr);
        return result;
    }

    auto& loader = CFFmpegLoader::instance();
    auto* fn     = loader.m_fn;

    // Get the current frame - it should be a VAAPI surface
    auto* frame = reinterpret_cast<AVFrame*>(m_frame);
    if (!frame) {
        g_logger->log(HT_LOG_DEBUG, "exportFrameDmaBuf: frame is null");
        return result;
    }
    if (frame->format != AV_PIX_FMT_VAAPI) {
        g_logger->log(HT_LOG_DEBUG, "exportFrameDmaBuf: frame format {} is not VAAPI ({})", static_cast<int>(frame->format), static_cast<int>(AV_PIX_FMT_VAAPI));
        return result;
    }

    // For VAAPI frames, data[3] contains the VASurfaceID
    VASurfaceID surface = reinterpret_cast<uintptr_t>(frame->data[3]);
    VADisplay   display = reinterpret_cast<VADisplay>(m_vaDisplay);

    // Sync the surface before export
    if (fn->vaSyncSurface(display, surface) != 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: vaSyncSurface failed");
        return result;
    }

    // Export surface as DRM PRIME (DMA-BUF)
    VADRMPRIMESurfaceDescriptor desc{};
    int status = fn->vaExportSurfaceHandle(display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                           VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc);
    if (status != 0) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: vaExportSurfaceHandle failed with status {}", status);
        return result;
    }

    // We requested COMPOSED_LAYERS, so should have single object with all planes
    if (desc.num_objects < 1 || desc.num_layers < 1) {
        g_logger->log(HT_LOG_ERROR, "FFmpegDecoder: unexpected DRM PRIME descriptor: objects={} layers={}", desc.num_objects, desc.num_layers);
        // Close any fds we got
        for (uint32_t i = 0; i < desc.num_objects; ++i) {
            if (desc.objects[i].fd >= 0)
                close(desc.objects[i].fd);
        }
        return result;
    }

    // Fill in the result
    result.fd       = desc.objects[0].fd;
    result.format   = desc.layers[0].drm_format;
    result.modifier = desc.objects[0].drm_format_modifier;
    result.size     = m_size;
    result.planes   = static_cast<int>(desc.layers[0].num_planes);

    for (uint32_t i = 0; i < desc.layers[0].num_planes && i < 4; ++i) {
        result.offsets[i] = desc.layers[0].offset[i];
        result.strides[i] = desc.layers[0].pitch[i];
    }

    return result;
}

#endif // HT_VIDEO_SUPPORT
