#pragma once

#include "Element.hpp"
#include "../types/SizeType.hpp"
#include "../types/ImageTypes.hpp"

namespace Hyprtoolkit {

    struct SVideoImpl;
    struct SVideoData;
    class CVideoElement;

    /*
        Check if video playback is available at runtime.
        Returns false if FFmpeg libraries are not found.
    */
    bool videoSupported();

    /*
        Get the name of the hardware acceleration backend in use.
        Returns empty string if no hardware acceleration or video not supported.
    */
    std::string videoHwAccelName();

    class CVideoBuilder {
      public:
        ~CVideoBuilder() = default;

        static Hyprutils::Memory::CSharedPointer<CVideoBuilder> begin();
        Hyprutils::Memory::CSharedPointer<CVideoBuilder>        path(std::string&&);
        Hyprutils::Memory::CSharedPointer<CVideoBuilder>        fitMode(eImageFitMode);
        Hyprutils::Memory::CSharedPointer<CVideoBuilder>        size(CDynamicSize&&);
        Hyprutils::Memory::CSharedPointer<CVideoBuilder>        loop(bool);
        Hyprutils::Memory::CSharedPointer<CVideoBuilder>        fps(int);

        Hyprutils::Memory::CSharedPointer<CVideoElement>        commence();

      private:
        Hyprutils::Memory::CWeakPointer<CVideoBuilder> m_self;
        Hyprutils::Memory::CUniquePointer<SVideoData>  m_data;
        Hyprutils::Memory::CWeakPointer<CVideoElement> m_element;

        CVideoBuilder() = default;

        friend class CVideoElement;
    };

    class CVideoElement : public IElement {
      public:
        virtual ~CVideoElement() = default;

        Hyprutils::Memory::CSharedPointer<CVideoBuilder> rebuild();
        virtual Hyprutils::Math::Vector2D                size();

        void                                             play();
        void                                             pause();
        bool                                             playing() const;

      private:
        CVideoElement(const SVideoData& data);
        static Hyprutils::Memory::CSharedPointer<CVideoElement> create(const SVideoData& data);

        void                                                    replaceData(const SVideoData& data);

        //
        virtual void                                     paint();
        virtual void                                     reposition(const Hyprutils::Math::CBox& box, const Hyprutils::Math::Vector2D& maxSize = {-1, -1});
        virtual std::optional<Hyprutils::Math::Vector2D> preferredSize(const Hyprutils::Math::Vector2D& parent);
        virtual std::optional<Hyprutils::Math::Vector2D> minimumSize(const Hyprutils::Math::Vector2D& parent);
        virtual std::optional<Hyprutils::Math::Vector2D> maximumSize(const Hyprutils::Math::Vector2D& parent);
        virtual bool                                     positioningDependsOnChild();

        void                                             onFrameTimer();
        void                                             decodeAndUploadFrame();
        void                                             scheduleNextFrame();

        Hyprutils::Memory::CUniquePointer<SVideoImpl>    m_impl;

        friend class CVideoBuilder;
    };
};
