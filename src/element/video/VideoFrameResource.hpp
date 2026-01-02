#pragma once

#include <hyprgraphics/resource/resources/AsyncResource.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <vector>
#include <cstdint>

namespace Hyprtoolkit {

    /*
        Video frame resource - wraps raw BGRA pixel data in a cairo surface.
        Used by CVideoElement to upload decoded video frames.

        TODO: This class should be moved to hyprgraphics as a generic
        raw pixel buffer resource (e.g., CRawPixelResource).
    */
    class CVideoFrameResource : public Hyprgraphics::IAsyncResource {
      public:
        CVideoFrameResource(std::vector<uint8_t>&& data, const Hyprutils::Math::Vector2D& size);
        virtual ~CVideoFrameResource() = default;

        virtual void render() override;

      private:
        std::vector<uint8_t>      m_data;
        Hyprutils::Math::Vector2D m_size;
    };

}
