#include <hyprtoolkit/element/Video.hpp>
#include <hyprtoolkit/core/Timer.hpp>

#include "../../helpers/Memory.hpp"
#include "../../resource/assetCache/AssetCacheEntry.hpp"
#include "FFmpegLoader.hpp"
#include "VideoFrameResource.hpp"

namespace Hyprtoolkit {
    struct SVideoData {
        std::string   path;
        eImageFitMode fitMode = IMAGE_FIT_MODE_COVER;
        CDynamicSize  size{CDynamicSize::HT_SIZE_PERCENT, CDynamicSize::HT_SIZE_PERCENT, {1, 1}};
        bool          loop = true;
        int           fps  = 0; // 0 = native fps, >0 = max fps (capped to video fps)
    };

    struct SVideoImpl {
        SVideoData                                                             data;

        WP<CVideoElement>                                                      self;

        UP<CFFmpegDecoder>                                                     decoder;
        Hyprutils::Memory::CAtomicSharedPointer<CVideoFrameResource>           resource;
        Hyprutils::Memory::CSharedPointer<Asset::CAssetCacheEntry>             cacheEntry;
        SP<IRendererTexture>                                                   texture;

        Hyprutils::Math::Vector2D                                             videoSize;
        ASP<CTimer>                                                           frameTimer;
        bool                                                                  playing = true;
        bool                                                                  failed  = false;

        double                                                                targetFps = 30.0;
        std::chrono::steady_clock::time_point                                 lastFrameTime;

        std::string                                                           getCacheString() const;
    };
}
