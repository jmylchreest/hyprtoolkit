#include "Video.hpp"

#include "../../layout/Positioner.hpp"
#include "../../renderer/Renderer.hpp"
#include "../../core/InternalBackend.hpp"
#include "../../window/ToolkitWindow.hpp"
#include "../../resource/assetCache/AssetCache.hpp"
#include "../../renderer/RendererTexture.hpp"

#include "../Element.hpp"

using namespace Hyprtoolkit;

bool Hyprtoolkit::videoSupported() {
    return CFFmpegLoader::instance().available();
}

std::string Hyprtoolkit::videoHwAccelName() {
    return CFFmpegLoader::instance().hwAccelName();
}

SP<CVideoElement> CVideoElement::create(const SVideoData& data) {
    auto p          = SP<CVideoElement>(new CVideoElement(data));
    p->impl->self   = p;
    p->m_impl->self = p;
    return p;
}

CVideoElement::CVideoElement(const SVideoData& data) : IElement(), m_impl(makeUnique<SVideoImpl>()) {
    m_impl->data = data;

    if (!videoSupported()) {
        g_logger->log(HT_LOG_ERROR, "CVideoElement: video not supported, FFmpeg unavailable");
        m_impl->failed = true;
        return;
    }

    // Create decoder
    m_impl->decoder = makeUnique<CFFmpegDecoder>(data.path);
    if (!m_impl->decoder->valid()) {
        g_logger->log(HT_LOG_ERROR, "CVideoElement: failed to open video '{}'", data.path);
        m_impl->failed = true;
        return;
    }

    m_impl->videoSize = m_impl->decoder->size();
    m_impl->targetFps = data.fps > 0 ? static_cast<double>(data.fps) : m_impl->decoder->fps();

    // Decode first frame immediately
    if (m_impl->decoder->decodeNextFrame())
        decodeAndUploadFrame();

    // Schedule frame updates
    if (m_impl->playing)
        scheduleNextFrame();
}

void CVideoElement::scheduleNextFrame() {
    auto frameInterval = std::chrono::milliseconds(static_cast<int>(1000.0 / m_impl->targetFps));

    m_impl->frameTimer = g_backend->addTimer(
        frameInterval,
        [this, s = m_impl->self](ASP<CTimer> timer, void*) {
            if (auto locked = s.lock())
                locked->onFrameTimer();
        },
        nullptr);
}

void CVideoElement::paint() {
    if (m_impl->failed || !m_impl->texture)
        return;

    g_renderer->renderTexture({
        .box      = impl->position,
        .texture  = m_impl->texture,
        .a        = 1.F,
        .rounding = 0,
    });
}

void CVideoElement::decodeAndUploadFrame() {
    if (!m_impl->decoder || !m_impl->decoder->valid())
        return;

    const auto& frameData = m_impl->decoder->frameData();
    if (frameData.empty())
        return;

    // Create image resource from RGBA frame data
    std::vector<uint8_t> dataCopy = frameData;
    m_impl->resource              = makeAtomicShared<Hyprgraphics::CImageResource>(std::move(dataCopy), m_impl->videoSize);

    // Process synchronously since we have the data ready
    g_asyncResourceGatherer->enqueue(ASP<Hyprgraphics::IAsyncResource>(m_impl->resource));
    g_asyncResourceGatherer->await(ASP<Hyprgraphics::IAsyncResource>(m_impl->resource));

    if (m_impl->resource->m_asset.cairoSurface) {
        ASP<Hyprgraphics::IAsyncResource> resourceGeneric(m_impl->resource);
        m_impl->texture = g_renderer->uploadTexture({.resource = resourceGeneric, .fitMode = m_impl->data.fitMode});
    }

    // Trigger repaint
    impl->damageEntire();
}

void CVideoElement::onFrameTimer() {
    if (!m_impl->playing || m_impl->failed)
        return;

    // Decode next frame
    bool hasFrame = m_impl->decoder->decodeNextFrame();

    if (!hasFrame) {
        if (m_impl->decoder->atEnd()) {
            if (m_impl->data.loop) {
                m_impl->decoder->rewind();
                hasFrame = m_impl->decoder->decodeNextFrame();
            } else {
                m_impl->playing = false;
                return;
            }
        }
    }

    if (hasFrame)
        decodeAndUploadFrame();

    // Schedule next frame
    if (m_impl->playing)
        scheduleNextFrame();
}

std::string SVideoImpl::getCacheString() const {
    return std::format("video-{}", data.path);
}

SP<CVideoBuilder> CVideoElement::rebuild() {
    auto p       = SP<CVideoBuilder>(new CVideoBuilder());
    p->m_self    = p;
    p->m_data    = makeUnique<SVideoData>(m_impl->data);
    p->m_element = m_impl->self;
    return p;
}

void CVideoElement::replaceData(const SVideoData& data) {
    // Stop current playback
    if (m_impl->frameTimer && !m_impl->frameTimer->passed())
        m_impl->frameTimer->cancel();

    m_impl->data    = data;
    m_impl->failed  = false;
    m_impl->playing = true;

    // Re-create decoder if path changed
    m_impl->decoder = makeUnique<CFFmpegDecoder>(data.path);
    if (!m_impl->decoder->valid()) {
        g_logger->log(HT_LOG_ERROR, "CVideoElement: failed to open video '{}'", data.path);
        m_impl->failed = true;
        return;
    }

    m_impl->videoSize = m_impl->decoder->size();
    m_impl->targetFps = data.fps > 0 ? static_cast<double>(data.fps) : m_impl->decoder->fps();

    // Decode first frame
    if (m_impl->decoder->decodeNextFrame())
        decodeAndUploadFrame();

    // Schedule frame updates
    if (m_impl->playing)
        scheduleNextFrame();

    if (impl->window)
        impl->window->scheduleReposition(impl->self);
}

void CVideoElement::reposition(const Hyprutils::Math::CBox& box, const Hyprutils::Math::Vector2D& maxSize) {
    IElement::reposition(box);

    g_positioner->positionChildren(impl->self.lock());
}

Hyprutils::Math::Vector2D CVideoElement::size() {
    return impl->position.size();
}

std::optional<Vector2D> CVideoElement::preferredSize(const Hyprutils::Math::Vector2D& parent) {
    auto s = m_impl->data.size.calculate(parent);
    if (s.x != -1 && s.y != -1)
        return s;

    const float SCALE = impl->window ? impl->window->scale() : 1.F;

    if (s.x == -1 && s.y == -1)
        return m_impl->videoSize / SCALE;

    if (m_impl->videoSize.y == 0)
        return impl->getPreferredSizeGeneric(m_impl->data.size, parent);

    const double ASPECT_RATIO = m_impl->videoSize.x / m_impl->videoSize.y;

    if (s.y == -1)
        return Vector2D{s.x, s.x * (1 / ASPECT_RATIO)};

    return Vector2D{ASPECT_RATIO * s.y, s.y};
}

std::optional<Vector2D> CVideoElement::minimumSize(const Hyprutils::Math::Vector2D& parent) {
    auto s = m_impl->data.size.calculate(parent);
    if (s.x != -1 && s.y != -1)
        return s;
    return Vector2D{0, 0};
}

std::optional<Vector2D> CVideoElement::maximumSize(const Hyprutils::Math::Vector2D& parent) {
    auto s = m_impl->data.size.calculate(parent);
    if (s.x != -1 && s.y != -1)
        return s;
    return std::nullopt;
}

bool CVideoElement::positioningDependsOnChild() {
    return m_impl->data.size.hasAuto();
}

void CVideoElement::play() {
    if (m_impl->failed || m_impl->playing)
        return;

    m_impl->playing = true;
    scheduleNextFrame();
}

void CVideoElement::pause() {
    m_impl->playing = false;
    if (m_impl->frameTimer && !m_impl->frameTimer->passed())
        m_impl->frameTimer->cancel();
}

bool CVideoElement::playing() const {
    return m_impl->playing;
}
