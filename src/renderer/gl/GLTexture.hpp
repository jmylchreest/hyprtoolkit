#pragma once

#include <hyprutils/math/Vector2D.hpp>
#include <hyprgraphics/resource/resources/AsyncResource.hpp>

#include "GL.hpp"

#include <cstdint>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "../RendererTexture.hpp"
#include "../../helpers/Memory.hpp"

namespace Hyprtoolkit {

    struct SDmaBufFrame;
    enum eGLTextureType : uint8_t {
        TEXTURE_INVALID,  // Invalid
        TEXTURE_RGBA,     // 4 channels
        TEXTURE_RGBX,     // discard A
        TEXTURE_EXTERNAL, // EGLImage
    };

    class CTimer;

    class CGLTexture : public IRendererTexture {
      public:
        CGLTexture(ASP<Hyprgraphics::IAsyncResource>);
        CGLTexture();
        virtual ~CGLTexture();

        virtual size_t                    id();
        virtual eTextureType              type();
        virtual void                      destroy();
        virtual eImageFitMode             fitMode();
        virtual Hyprutils::Math::Vector2D size();

        eGLTextureType                    m_type      = TEXTURE_RGBA;
        GLenum                            m_target    = GL_TEXTURE_2D;
        bool                              m_allocated = false;
        GLuint                            m_texID     = 0;
        eImageFitMode                     m_fitMode   = IMAGE_FIT_MODE_STRETCH;
        Hyprutils::Math::Vector2D         m_size      = {};
        EGLImageKHR                       m_eglImage  = EGL_NO_IMAGE_KHR;

        ASP<Hyprgraphics::IAsyncResource> m_resource;

        void                              upload();
        void                              allocate();
        void                              bind();

        // Create external texture from DMA-BUF frame (for zero-copy video)
        bool uploadFromDmaBuf(const SDmaBufFrame& frame);
    };
};