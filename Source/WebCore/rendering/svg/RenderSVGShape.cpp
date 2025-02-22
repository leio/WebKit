/*
 * Copyright (C) 2004, 2005, 2007 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) 2004, 2005, 2008 Rob Buis <buis@kde.org>
 * Copyright (C) 2005, 2007 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2009 Google, Inc.
 * Copyright (C) 2009 Dirk Schulze <krit@webkit.org>
 * Copyright (C) Research In Motion Limited 2010. All rights reserved.
 * Copyright (C) 2009 Jeff Schiller <codedread@gmail.com>
 * Copyright (C) 2011 Renata Hodovan <reni@webkit.org>
 * Copyright (C) 2011 University of Szeged
 * Copyright (C) 2018 Adobe Systems Incorporated. All rights reserved.
 * Copyright (C) 2020, 2021, 2022 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "RenderSVGShape.h"

#if ENABLE(LAYER_BASED_SVG_ENGINE)
#include "FloatPoint.h"
#include "FloatQuad.h"
#include "GraphicsContext.h"
#include "HitTestRequest.h"
#include "HitTestResult.h"
#include "LayoutRepainter.h"
#include "PointerEventsHitRules.h"
#include "RenderSVGResourceMarker.h"
#include "RenderSVGResourceSolidColor.h"
#include "RenderSVGShapeInlines.h"
#include "RenderStyleInlines.h"
#include "SVGPathData.h"
#include "SVGRenderingContext.h"
#include "SVGResources.h"
#include "SVGResourcesCache.h"
#include "SVGURIReference.h"
#include <wtf/IsoMallocInlines.h>
#include <wtf/StackStats.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(RenderSVGShape);

RenderSVGShape::RenderSVGShape(Type type, SVGGraphicsElement& element, RenderStyle&& style)
    : RenderSVGModelObject(type, element, WTFMove(style))
{
}

RenderSVGShape::~RenderSVGShape() = default;

bool RenderSVGShape::isEmpty() const
{
    // This function should never be called before assigning a new Path to m_path.
    // But this bug can happen if this renderer was created and its layout was not
    // done before painting. Assert this did not happen but do not crash.
    ASSERT(hasPath());
    return !hasPath() || path().isEmpty();
}

void RenderSVGShape::fillShape(GraphicsContext& context) const
{
    context.fillPath(path());
}

void RenderSVGShape::strokeShape(GraphicsContext& context) const
{
    ASSERT(m_path);
    Path* usePath = m_path.get();

    if (hasNonScalingStroke())
        usePath = nonScalingStrokePath(usePath, nonScalingStrokeTransform());

    context.strokePath(*usePath);
}

bool RenderSVGShape::shapeDependentStrokeContains(const FloatPoint& point, PointCoordinateSpace pointCoordinateSpace)
{
    ASSERT(m_path);

    if (hasNonScalingStroke() && pointCoordinateSpace != LocalCoordinateSpace) {
        AffineTransform nonScalingTransform = nonScalingStrokeTransform();
        Path* usePath = nonScalingStrokePath(m_path.get(), nonScalingTransform);
        return usePath->strokeContains(nonScalingTransform.mapPoint(point), [this] (GraphicsContext& context) {
            SVGRenderSupport::applyStrokeStyleToContext(context, style(), *this);
        });
    }

    return m_path->strokeContains(point, [this] (GraphicsContext& context) {
        SVGRenderSupport::applyStrokeStyleToContext(context, style(), *this);
    });
}

bool RenderSVGShape::shapeDependentFillContains(const FloatPoint& point, const WindRule fillRule) const
{
    return path().contains(point, fillRule);
}

bool RenderSVGShape::fillContains(const FloatPoint& point, bool requiresFill, const WindRule fillRule)
{
    if (m_fillBoundingBox.isEmpty() || !m_fillBoundingBox.contains(point))
        return false;

    Color fallbackColor;
    if (requiresFill && !RenderSVGResource::fillPaintingResource(*this, style(), fallbackColor))
        return false;

    return shapeDependentFillContains(point, fillRule);
}

bool RenderSVGShape::strokeContains(const FloatPoint& point, bool requiresStroke)
{
    if (strokeBoundingBox().isEmpty() || !strokeBoundingBox().contains(point))
        return false;

    Color fallbackColor;
    if (requiresStroke && !RenderSVGResource::strokePaintingResource(*this, style(), fallbackColor))
        return false;

    return shapeDependentStrokeContains(point);
}

void RenderSVGShape::layout()
{
    StackStats::LayoutCheckPoint layoutCheckPoint;

    LayoutRepainter repainter(*this, checkForRepaintDuringLayout(), RepaintOutlineBounds::No);
    if (m_needsShapeUpdate) {
        // FIXME: [LBSE] Upstream SVGLengthContext changes
        // graphicsElement().updateLengthContext();
        updateShapeFromElement();

        m_needsShapeUpdate = false;
        setCurrentSVGLayoutRect(enclosingLayoutRect(m_fillBoundingBox));
    }

    updateLayerTransform();

    // Invalidate all resources of this client if our layout changed.
    if (everHadLayout() && selfNeedsLayout())
        SVGResourcesCache::clientLayoutChanged(*this);

    repainter.repaintAfterLayout();
    clearNeedsLayout();
}

Path* RenderSVGShape::nonScalingStrokePath(const Path* path, const AffineTransform& strokeTransform) const
{
    static NeverDestroyed<Path> tempPath;

    tempPath.get() = *path;
    tempPath.get().transform(strokeTransform);

    return &tempPath.get();
}

bool RenderSVGShape::setupNonScalingStrokeContext(AffineTransform& strokeTransform, GraphicsContextStateSaver& stateSaver)
{
    std::optional<AffineTransform> inverse = strokeTransform.inverse();
    if (!inverse)
        return false;

    stateSaver.save();
    stateSaver.context()->concatCTM(inverse.value());
    return true;
}

AffineTransform RenderSVGShape::nonScalingStrokeTransform() const
{
    return graphicsElement().getScreenCTM(SVGLocatable::DisallowStyleUpdate);
}

void RenderSVGShape::fillShape(const RenderStyle& style, GraphicsContext& originalContext)
{
    GraphicsContext* context = &originalContext;
    Color fallbackColor;
    if (RenderSVGResource* fillPaintingResource = RenderSVGResource::fillPaintingResource(*this, style, fallbackColor)) {
        if (fillPaintingResource->applyResource(*this, style, context, RenderSVGResourceMode::ApplyToFill))
            fillPaintingResource->postApplyResource(*this, context, RenderSVGResourceMode::ApplyToFill, nullptr, this);
        else if (fallbackColor.isValid()) {
            RenderSVGResourceSolidColor* fallbackResource = RenderSVGResource::sharedSolidPaintingResource();
            fallbackResource->setColor(fallbackColor);
            if (fallbackResource->applyResource(*this, style, context, RenderSVGResourceMode::ApplyToFill))
                fallbackResource->postApplyResource(*this, context, RenderSVGResourceMode::ApplyToFill, nullptr, this);
        }
    }
}

void RenderSVGShape::strokeShapeInternal(const RenderStyle& style, GraphicsContext& originalContext)
{
    GraphicsContext* context = &originalContext;
    Color fallbackColor;
    if (RenderSVGResource* strokePaintingResource = RenderSVGResource::strokePaintingResource(*this, style, fallbackColor)) {
        if (strokePaintingResource->applyResource(*this, style, context, RenderSVGResourceMode::ApplyToStroke))
            strokePaintingResource->postApplyResource(*this, context, RenderSVGResourceMode::ApplyToStroke, nullptr, this);
        else if (fallbackColor.isValid()) {
            RenderSVGResourceSolidColor* fallbackResource = RenderSVGResource::sharedSolidPaintingResource();
            fallbackResource->setColor(fallbackColor);
            if (fallbackResource->applyResource(*this, style, context, RenderSVGResourceMode::ApplyToStroke))
                fallbackResource->postApplyResource(*this, context, RenderSVGResourceMode::ApplyToStroke, nullptr, this);
        }
    }
}

void RenderSVGShape::strokeShape(const RenderStyle& style, GraphicsContext& context)
{
    if (!style.hasVisibleStroke())
        return;

    GraphicsContextStateSaver stateSaver(context, false);
    if (hasNonScalingStroke()) {
        AffineTransform nonScalingTransform = nonScalingStrokeTransform();
        if (!setupNonScalingStrokeContext(nonScalingTransform, stateSaver))
            return;
    }
    strokeShapeInternal(style, context);
}

void RenderSVGShape::fillStrokeMarkers(PaintInfo& childPaintInfo)
{
    for (auto type : RenderStyle::paintTypesForPaintOrder(style().paintOrder())) {
        switch (type) {
        case PaintType::Fill:
            fillShape(style(), childPaintInfo.context());
            break;
        case PaintType::Stroke:
            strokeShape(style(), childPaintInfo.context());
            break;
        case PaintType::Markers:
            drawMarkers(childPaintInfo);
            break;
        }
    }
}

void RenderSVGShape::paint(PaintInfo& paintInfo, const LayoutPoint& paintOffset)
{
    OptionSet<PaintPhase> relevantPaintPhases { PaintPhase::Foreground, PaintPhase::ClippingMask, PaintPhase::Mask, PaintPhase::Outline, PaintPhase::SelfOutline };
    if (!shouldPaintSVGRenderer(paintInfo, relevantPaintPhases) || isEmpty())
        return;

    if (paintInfo.phase == PaintPhase::ClippingMask) {
        // FIXME: [LBSE] Upstream SVGRenderSupport changes
        // SVGRenderSupport::paintSVGClippingMask(*this, paintInfo);
        return;
    }

    auto adjustedPaintOffset = paintOffset + currentSVGLayoutLocation();
    if (paintInfo.phase == PaintPhase::Mask) {
        // FIXME: [LBSE] Upstream SVGRenderSupport changes
        // SVGRenderSupport::paintSVGMask(*this, paintInfo, adjustedPaintOffset);
        return;
    }

    auto visualOverflowRect = visualOverflowRectEquivalent();
    visualOverflowRect.moveBy(adjustedPaintOffset);
    if (!visualOverflowRect.intersects(paintInfo.rect))
        return;

    if (paintInfo.phase == PaintPhase::Outline || paintInfo.phase == PaintPhase::SelfOutline) {
        // FIXME: [LBSE] Upstream outline painting
        // paintSVGOutline(paintInfo, adjustedPaintOffset);
        return;
    }

    ASSERT(paintInfo.phase == PaintPhase::Foreground);
    GraphicsContextStateSaver stateSaver(paintInfo.context());

    auto coordinateSystemOriginTranslation = adjustedPaintOffset - nominalSVGLayoutLocation();
    paintInfo.context().translate(coordinateSystemOriginTranslation.width(), coordinateSystemOriginTranslation.height());

    if (style().svgStyle().shapeRendering() == ShapeRendering::CrispEdges)
        paintInfo.context().setShouldAntialias(false);

    fillStrokeMarkers(paintInfo);
}

bool RenderSVGShape::isPointInFill(const FloatPoint& point)
{
    return shapeDependentFillContains(point, style().svgStyle().fillRule());
}

bool RenderSVGShape::isPointInStroke(const FloatPoint& point)
{
    if (!style().svgStyle().hasStroke())
        return false;

    return shapeDependentStrokeContains(point, LocalCoordinateSpace);
}

float RenderSVGShape::getTotalLength() const
{
    return hasPath() ? path().length() : createPath()->length();
}

FloatPoint RenderSVGShape::getPointAtLength(float distance) const
{
    return hasPath() ? path().pointAtLength(distance) : createPath()->pointAtLength(distance);
}

bool RenderSVGShape::nodeAtPoint(const HitTestRequest& request, HitTestResult& result, const HitTestLocation& locationInContainer, const LayoutPoint& accumulatedOffset, HitTestAction hitTestAction)
{
    if (hitTestAction != HitTestForeground)
        return false;

    auto adjustedLocation = accumulatedOffset + currentSVGLayoutLocation();

    auto localPoint = locationInContainer.point();
    auto coordinateSystemOriginTranslation = nominalSVGLayoutLocation() - adjustedLocation;
    localPoint.move(coordinateSystemOriginTranslation);

    if (!SVGRenderSupport::pointInClippingArea(*this, localPoint))
        return false;

    SVGHitTestCycleDetectionScope hitTestScope(*this);

    PointerEventsHitRules hitRules(PointerEventsHitRules::HitTestingTargetType::SVGPath, request, style().effectivePointerEvents());
    bool isVisible = (style().visibility() == Visibility::Visible);
    if (isVisible || !hitRules.requireVisible) {
        const SVGRenderStyle& svgStyle = style().svgStyle();
        WindRule fillRule = svgStyle.fillRule();
        if (request.svgClipContent())
            fillRule = svgStyle.clipRule();

        if (hitRules.canHitStroke && (svgStyle.hasStroke() || !hitRules.requireStroke) && strokeContains(localPoint, hitRules.requireStroke)) {
            updateHitTestResult(result, locationInContainer.point() - toLayoutSize(adjustedLocation));
            if (result.addNodeToListBasedTestResult(nodeForHitTest(), request, locationInContainer, strokeBoundingBox()) == HitTestProgress::Stop)
                return true;
            return false;
        }

        if ((hitRules.canHitFill && (svgStyle.hasFill() || !hitRules.requireFill) && fillContains(localPoint, hitRules.requireFill, fillRule))
            || (hitRules.canHitBoundingBox && m_fillBoundingBox.contains(localPoint))) {
            updateHitTestResult(result, locationInContainer.point() - toLayoutSize(adjustedLocation));
            if (result.addNodeToListBasedTestResult(nodeForHitTest(), request, locationInContainer, m_fillBoundingBox) == HitTestProgress::Stop)
                return true;
            return false;
        }
    }

    return false;
}

FloatRect RenderSVGShape::strokeBoundingBox() const
{
    if (m_shapeType == ShapeType::Empty)
        return { };
    if (!m_strokeBoundingBox)
        m_strokeBoundingBox = calculateStrokeBoundingBox();
    return *m_strokeBoundingBox;
}

FloatRect RenderSVGShape::calculateStrokeBoundingBox() const
{
    ASSERT(m_path);
    FloatRect strokeBoundingBox = m_fillBoundingBox;

    if (style().svgStyle().hasStroke()) {
        if (hasNonScalingStroke()) {
            AffineTransform nonScalingTransform = nonScalingStrokeTransform();
            if (std::optional<AffineTransform> inverse = nonScalingTransform.inverse()) {
                Path* usePath = nonScalingStrokePath(m_path.get(), nonScalingTransform);
                FloatRect strokeBoundingRect = usePath->strokeBoundingRect(Function<void(GraphicsContext&)> { [this] (GraphicsContext& context) {
                    SVGRenderSupport::applyStrokeStyleToContext(context, style(), *this);
                } });
                strokeBoundingRect = inverse.value().mapRect(strokeBoundingRect);
                strokeBoundingBox.unite(strokeBoundingRect);
            }
        } else {
            strokeBoundingBox.unite(path().strokeBoundingRect(Function<void(GraphicsContext&)> { [this] (GraphicsContext& context) {
                SVGRenderSupport::applyStrokeStyleToContext(context, style(), *this);
            } }));
        }
    }

    return adjustStrokeBoundingBoxForMarkersAndZeroLengthLinecaps(RepaintRectCalculation::Accurate, strokeBoundingBox);
}

float RenderSVGShape::strokeWidth() const
{
    SVGLengthContext lengthContext(&graphicsElement());
    return lengthContext.valueForLength(style().strokeWidth());
}

bool RenderSVGShape::hasSmoothStroke() const
{
    const SVGRenderStyle& svgStyle = style().svgStyle();
    return svgStyle.strokeDashArray().isEmpty()
        && style().strokeMiterLimit() == defaultMiterLimit
        && style().joinStyle() == LineJoin::Miter
        && style().capStyle() == LineCap::Butt;
}

Path& RenderSVGShape::ensurePath()
{
    if (!hasPath())
        m_path = createPath();
    return path();
}

std::unique_ptr<Path> RenderSVGShape::createPath() const
{
    return makeUnique<Path>(pathFromGraphicsElement(&graphicsElement()));
}

void RenderSVGShape::styleWillChange(StyleDifference diff, const RenderStyle& newStyle)
{
    auto* oldStyle = hasInitializedStyle() ? &style() : nullptr;
    if (oldStyle) {
        if (diff == StyleDifference::Layout)
            setNeedsShapeUpdate();
    }

    RenderSVGModelObject::styleWillChange(diff, newStyle);
}

bool RenderSVGShape::needsHasSVGTransformFlags() const
{
    return graphicsElement().hasTransformRelatedAttributes();
}

void RenderSVGShape::applyTransform(TransformationMatrix& transform, const RenderStyle& style, const FloatRect& boundingBox, OptionSet<RenderStyle::TransformOperationOption> options) const
{
    applySVGTransform(transform, graphicsElement(), style, boundingBox, std::nullopt, std::nullopt, options);
}

}

#endif // ENABLE(LAYER_BASED_SVG_ENGINE)
