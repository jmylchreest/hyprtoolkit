#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <hyprutils/math/Vector2D.hpp>

namespace Hyprtoolkit {

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

        bool        m_available   = false;
        std::string m_hwAccelName = "";

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

        // Get current frame as RGBA pixel data
        const std::vector<uint8_t>& frameData() const;

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

        bool                      m_valid    = false;
        bool                      m_atEnd    = false;
        bool                      m_useHwDec = false;
        Hyprutils::Math::Vector2D m_size     = {};
        double                    m_fps      = 30.0;
        double                    m_duration = 0.0;

        std::vector<uint8_t>      m_frameBuffer;

        // Opaque FFmpeg handles
        void* m_formatCtx   = nullptr;
        void* m_codecCtx    = nullptr;
        void* m_hwDeviceCtx = nullptr;
        void* m_frame       = nullptr;
        void* m_frameRgb    = nullptr;
        void* m_packet      = nullptr;
        void* m_swsCtx      = nullptr;
        int   m_streamIdx   = -1;
    };

}
