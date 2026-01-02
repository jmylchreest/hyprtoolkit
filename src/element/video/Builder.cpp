#include "Video.hpp"

using namespace Hyprtoolkit;

SP<CVideoBuilder> CVideoBuilder::begin() {
    SP<CVideoBuilder> p = SP<CVideoBuilder>(new CVideoBuilder());
    p->m_data           = makeUnique<SVideoData>();
    p->m_self           = p;
    return p;
}

SP<CVideoBuilder> CVideoBuilder::path(std::string&& s) {
    m_data->path = std::move(s);
    return m_self.lock();
}

SP<CVideoBuilder> CVideoBuilder::fitMode(eImageFitMode x) {
    m_data->fitMode = x;
    return m_self.lock();
}

SP<CVideoBuilder> CVideoBuilder::size(CDynamicSize&& s) {
    m_data->size = std::move(s);
    return m_self.lock();
}

SP<CVideoBuilder> CVideoBuilder::loop(bool x) {
    m_data->loop = x;
    return m_self.lock();
}

SP<CVideoBuilder> CVideoBuilder::fps(int x) {
    m_data->fps = x;
    return m_self.lock();
}

SP<CVideoElement> CVideoBuilder::commence() {
    if (m_element) {
        m_element->replaceData(*m_data);
        return m_element.lock();
    }

    return CVideoElement::create(*m_data);
}
