#pragma once

#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <hyprutils/math/Vector2D.hpp>

namespace Hyprtoolkit {

    /*
        DMA-BUF frame descriptor for zero-copy GPU rendering.
        Exported from VAAPI surfaces for direct EGLImage import.
    */
    struct SDmaBufFrame {
        int                      fd       = -1; // DMA-BUF file descriptor (caller must close)
        uint32_t                 format   = 0;  // DRM fourcc format (e.g., DRM_FORMAT_NV12)
        uint64_t                 modifier = 0;  // DRM format modifier
        Hyprutils::Math::Vector2D size;
        int                      planes = 1;
        std::array<uint32_t, 4>  offsets{};
        std::array<uint32_t, 4>  strides{};

        bool                     valid() const { return fd >= 0; }
    };

    /*
        FFmpeg dlopen loader - provides zero compile-time dependency on FFmpeg.
        Loads libavcodec, libavformat, libavutil, libswscale at runtime.
        If unavailable, video support is gracefully disabled.
    */
    class CFFmpegLoader {
      public:
        static CFFmpegLoader& instance();

        bool                  available() const;
        std::string           hwAccelName() const;
        unsigned              versionMajor() const;
        bool                  dmaBufExportSupported() const;

        // Prevent copying
        CFFmpegLoader(const CFFmpegLoader&)            = delete;
        CFFmpegLoader& operator=(const CFFmpegLoader&) = delete;

      private:
        CFFmpegLoader();
        ~CFFmpegLoader();

        bool        load();
        bool        probeHwAccel();

        void*       m_avcodec  = nullptr;
        void*       m_avformat = nullptr;
        void*       m_avutil   = nullptr;
        void*       m_swscale  = nullptr;
        void*       m_libva    = nullptr; // For DMA-BUF export

        bool        m_available          = false;
        bool        m_dmaBufExportSupported = false;
        std::string m_hwAccelName        = "";
        unsigned    m_versionMajor       = 0;

        // Function pointers - opaque to avoid FFmpeg header dependency
        struct SFunctions;
        SFunctions* m_fn = nullptr;

        friend class CFFmpegDecoder;
    };

    /*
        Video decoder using FFmpeg (loaded via dlopen).
        Handles demuxing, decoding, and color conversion to RGBA.
    */
    class CFFmpegDecoder {
      public:
        explicit CFFmpegDecoder(const std::string& path);
        ~CFFmpegDecoder();

        bool                      valid() const;
        Hyprutils::Math::Vector2D size() const;
        double                    fps() const;
        double                    duration() const;

        // Decode next frame, returns true if frame available
        bool decodeNextFrame();

        // Get current frame as RGBA pixel data (CPU path)
        const std::vector<uint8_t>& frameData() const;

        // Export current frame as DMA-BUF for zero-copy GPU rendering (VAAPI only)
        // Returns invalid SDmaBufFrame if not supported or failed
        SDmaBufFrame exportFrameDmaBuf();

        // Check if DMA-BUF export is available for this decoder
        bool dmaBufExportAvailable() const;

        // Seek to position in seconds
        void seek(double seconds);

        // Check if at end of video
        bool atEnd() const;

        // Restart from beginning
        void rewind();

        // Prevent copying
        CFFmpegDecoder(const CFFmpegDecoder&)            = delete;
        CFFmpegDecoder& operator=(const CFFmpegDecoder&) = delete;

      private:
        bool                      openCodec();
        bool                      setupHwAccel();
        bool                      setupSwsContext();
        void                      cleanup();

        bool                      m_valid            = false;
        bool                      m_atEnd            = false;
        bool                      m_useHwDec         = false;
        bool                      m_dmaBufAvailable  = false;
        Hyprutils::Math::Vector2D m_size             = {};
        double                    m_fps              = 30.0;
        double                    m_duration         = 0.0;
        int                       m_pixelFormat      = -1; // AVPixelFormat from decoded frame

        std::vector<uint8_t>      m_frameBuffer;

        // Opaque FFmpeg handles
        void* m_formatCtx   = nullptr;
        void* m_codecCtx    = nullptr;
        void* m_hwDeviceCtx = nullptr;
        void* m_vaDisplay   = nullptr; // VADisplay for DMA-BUF export
        void* m_frame       = nullptr;
        void* m_swFrame     = nullptr; // For HW decode: holds CPU-transferred frame
        void* m_packet      = nullptr;
        void* m_swsCtx      = nullptr;
        int   m_streamIdx   = -1;
    };

}
