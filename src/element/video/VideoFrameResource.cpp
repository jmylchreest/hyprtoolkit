#include "VideoFrameResource.hpp"
#include "../../core/Logger.hpp"
#include "../../helpers/Memory.hpp"

#include <cairo/cairo.h>

using namespace Hyprtoolkit;

CVideoFrameResource::CVideoFrameResource(std::vector<uint8_t>&& data, const Hyprutils::Math::Vector2D& size) : m_data(std::move(data)), m_size(size) {
    //
}

void CVideoFrameResource::render() {
    // Create cairo surface from raw BGRA pixel data
    // Cairo expects data in ARGB32 format (which is BGRA in memory on little-endian)

    const int width  = static_cast<int>(m_size.x);
    const int height = static_cast<int>(m_size.y);
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    // Expected buffer size
    const size_t expectedSize = static_cast<size_t>(stride) * static_cast<size_t>(height);

    if (m_data.size() < expectedSize) {
        g_logger->log(HT_LOG_ERROR, "CVideoFrameResource: buffer size {} too small, expected {} ({}x{} stride={})", m_data.size(), expectedSize, width, height, stride);
        m_ready = true;
        return;
    }

    // Create surface wrapping our pixel data
    // IMPORTANT: The data must remain valid for the lifetime of the surface
    cairo_surface_t* surface = cairo_image_surface_create_for_data(m_data.data(), CAIRO_FORMAT_ARGB32, width, height, stride);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        g_logger->log(HT_LOG_ERROR, "CVideoFrameResource: failed to create cairo surface: {}", cairo_status_to_string(cairo_surface_status(surface)));
        cairo_surface_destroy(surface);
        m_ready = true;
        return;
    }

    m_asset.cairoSurface = makeShared<Hyprgraphics::CCairoSurface>(surface);
    m_asset.pixelSize    = m_size;

    m_ready = true;
    m_events.finished.emit();
}
