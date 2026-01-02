#include "GLTexture.hpp"
#include "OpenGL.hpp"

#include "../../core/InternalBackend.hpp"
#include "../../element/video/FFmpegLoader.hpp"

#include <aquamarine/buffer/Buffer.hpp>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

using namespace Hyprtoolkit;

CGLTexture::CGLTexture(ASP<Hyprgraphics::IAsyncResource> resource) {
    if (resource->m_ready) {
        m_resource = resource;
        upload();
        return;
    }

    // not ready yet, add a timer when it is and do it
    // FIXME: could UAF. Maybe keep wref?
    resource->m_events.finished.listenStatic([this, resource] {
        g_backend->addIdle([this, resource]() {
            m_resource = resource;
            upload();
        });
    });
}

CGLTexture::CGLTexture() {
    ;
}

CGLTexture::~CGLTexture() {
    destroy();
}

void CGLTexture::upload() {
    const cairo_status_t SURFACESTATUS = (cairo_status_t)m_resource->m_asset.cairoSurface->status();
    const auto           CAIROFORMAT   = cairo_image_surface_get_format(m_resource->m_asset.cairoSurface->cairo());
    const GLint          glIFormat     = CAIROFORMAT == CAIRO_FORMAT_RGB96F ? GL_RGB32F : GL_RGBA;
    const GLint          glFormat      = CAIROFORMAT == CAIRO_FORMAT_RGB96F ? GL_RGB : GL_RGBA;
    const GLint          glType        = CAIROFORMAT == CAIRO_FORMAT_RGB96F ? GL_FLOAT : GL_UNSIGNED_BYTE;

    allocate();

    if (SURFACESTATUS != CAIRO_STATUS_SUCCESS) {
        g_logger->log(HT_LOG_ERROR, "Resource {} invalid: failed to load, renderer will ignore");
        m_type = TEXTURE_INVALID;
        return;
    }

    m_type = TEXTURE_RGBA;
    m_size = m_resource->m_asset.pixelSize;

    GLCALL(glBindTexture(GL_TEXTURE_2D, m_texID));
    GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    if (CAIROFORMAT != CAIRO_FORMAT_RGB96F) {
        GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE));
        GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED));
    }
    GLCALL(glTexImage2D(GL_TEXTURE_2D, 0, glIFormat, m_size.x, m_size.y, 0, glFormat, glType, m_resource->m_asset.cairoSurface->data()));

    m_resource.reset();
}

size_t CGLTexture::id() {
    return m_texID;
}

IRendererTexture::eTextureType CGLTexture::type() {
    return TEXTURE_GL;
}

void CGLTexture::destroy() {
    if (g_openGL)
        g_openGL->makeEGLCurrent();

    if (m_eglImage != EGL_NO_IMAGE_KHR) {
        g_openGL->destroyEGLImage(m_eglImage);
        m_eglImage = EGL_NO_IMAGE_KHR;
    }

    if (m_allocated) {
        GLCALL(glDeleteTextures(1, &m_texID));
        m_texID = 0;
    }
    m_allocated = false;
}

void CGLTexture::allocate() {
    if (!m_allocated)
        GLCALL(glGenTextures(1, &m_texID));
    m_allocated = true;
}

void CGLTexture::bind() {
    GLCALL(glBindTexture(m_target, m_texID));
}

eImageFitMode CGLTexture::fitMode() {
    return m_fitMode;
}

Vector2D CGLTexture::size() {
    return m_size;
}

bool CGLTexture::uploadFromDmaBuf(const SDmaBufFrame& frame) {
    if (!frame.valid())
        return false;

    // Destroy old EGLImage if exists
    if (m_eglImage != EGL_NO_IMAGE_KHR) {
        g_openGL->destroyEGLImage(m_eglImage);
        m_eglImage = EGL_NO_IMAGE_KHR;
    }

    // Convert SDmaBufFrame to Aquamarine::SDMABUFAttrs
    Aquamarine::SDMABUFAttrs attrs;
    attrs.size     = {static_cast<int32_t>(frame.size.x), static_cast<int32_t>(frame.size.y)};
    attrs.format   = frame.format;
    attrs.modifier = frame.modifier;
    attrs.planes   = frame.planes;
    for (int i = 0; i < frame.planes && i < 4; ++i) {
        attrs.fds[i]     = frame.fd; // All planes share the same fd for COMPOSED_LAYERS
        attrs.offsets[i] = frame.offsets[i];
        attrs.strides[i] = frame.strides[i];
    }

    // Create new EGLImage from DMA-BUF
    m_eglImage = g_openGL->createEGLImage(attrs);

    if (m_eglImage == EGL_NO_IMAGE_KHR) {
        g_logger->log(HT_LOG_ERROR, "CGLTexture: failed to create EGLImage from DMA-BUF (format=0x{:x} modifier=0x{:x})", attrs.format, attrs.modifier);
        return false;
    }

    // Allocate texture if needed
    allocate();

    // Use external texture target for EGLImage
    m_target = GL_TEXTURE_EXTERNAL_OES;
    m_type   = TEXTURE_EXTERNAL;
    m_size   = frame.size;

    // Bind and attach EGLImage to texture
    GLCALL(glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texID));
    GLCALL(glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GLCALL(glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GLCALL(glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GLCALL(glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

    // Bind EGLImage to texture
    g_openGL->m_proc.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, m_eglImage);

    return true;
}
