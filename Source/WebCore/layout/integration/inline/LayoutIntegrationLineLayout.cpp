/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "LayoutIntegrationLineLayout.h"

#include "BlockFormattingState.h"
#include "BlockLayoutState.h"
#include "DeprecatedGlobalSettings.h"
#include "EventRegion.h"
#include "HitTestLocation.h"
#include "HitTestRequest.h"
#include "HitTestResult.h"
#include "InlineContentCache.h"
#include "InlineDamage.h"
#include "InlineFormattingContext.h"
#include "InlineInvalidation.h"
#include "InlineWalker.h"
#include "LayoutBoxGeometry.h"
#include "LayoutIntegrationCoverage.h"
#include "LayoutIntegrationInlineContentBuilder.h"
#include "LayoutIntegrationInlineContentPainter.h"
#include "LayoutIntegrationPagination.h"
#include "LayoutTreeBuilder.h"
#include "PaintInfo.h"
#include "PlacedFloats.h"
#include "RenderAttachment.h"
#include "RenderBlockFlow.h"
#include "RenderBoxInlines.h"
#include "RenderButton.h"
#include "RenderChildIterator.h"
#include "RenderDeprecatedFlexibleBox.h"
#include "RenderDescendantIterator.h"
#include "RenderElementInlines.h"
#include "RenderFlexibleBox.h"
#include "RenderGrid.h"
#include "RenderImage.h"
#include "RenderInline.h"
#include "RenderLayer.h"
#include "RenderLayoutState.h"
#include "RenderLineBreak.h"
#include "RenderListBox.h"
#include "RenderListItem.h"
#include "RenderListMarker.h"
#include "RenderMathMLBlock.h"
#include "RenderSlider.h"
#include "RenderTable.h"
#include "RenderTextControlMultiLine.h"
#include "RenderTheme.h"
#include "RenderView.h"
#include "Settings.h"
#include "ShapeOutsideInfo.h"
#include <wtf/Assertions.h>
#include <wtf/Range.h>

namespace WebCore {
namespace LayoutIntegration {

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(LayoutIntegration_LineLayout);

LineLayout::LineLayout(RenderBlockFlow& flow)
    : m_boxTree(flow)
    , m_layoutState(flow.view().layoutState())
    , m_blockFormattingState(layoutState().ensureBlockFormattingState(rootLayoutBox()))
    , m_inlineContentCache(layoutState().inlineContentCache(rootLayoutBox()))
{
}

LineLayout::~LineLayout()
{
    clearInlineContent();
    layoutState().destroyInlineContentCache(rootLayoutBox());
    layoutState().destroyBlockFormattingState(rootLayoutBox());
}

static inline bool isContentRenderer(const RenderObject& renderer)
{
    // FIXME: These fake renderers have their parent set but are not actually in the tree.
    return !renderer.isReplica() && !renderer.isRenderScrollbarPart();
}

RenderBlockFlow* LineLayout::blockContainer(const RenderObject& renderer)
{
    if (!isContentRenderer(renderer))
        return nullptr;

    for (auto* parent = renderer.parent(); parent; parent = parent->parent()) {
        if (!parent->childrenInline())
            return nullptr;
        if (is<RenderBlockFlow>(*parent))
            return downcast<RenderBlockFlow>(parent);
    }

    return nullptr;
}

LineLayout* LineLayout::containing(RenderObject& renderer)
{
    if (!isContentRenderer(renderer))
        return nullptr;

    if (!renderer.isInline()) {

        if (renderer.isRenderSVGBlock()) {
            // SVG content inside svg root shows up as block (see RenderSVGBlock). We only support inline root svg as "atomic content".
            return nullptr;
        }

        // IFC may contain block level boxes (floats and out-of-flow boxes).
        auto adjustedContainingBlock = [&] {
            RenderElement* containingBlock = nullptr;
            if (renderer.isOutOfFlowPositioned()) {
                // Here we are looking for the containing block as if the out-of-flow box was inflow (for static position purpose).
                containingBlock = renderer.parent();
                if (is<RenderInline>(containingBlock))
                    containingBlock = containingBlock->containingBlock();
            } else if (renderer.isFloating())
                containingBlock = renderer.containingBlock();
            return dynamicDowncast<RenderBlockFlow>(containingBlock);
        };
        if (auto* blockContainer = adjustedContainingBlock())
            return blockContainer->modernLineLayout();
        return nullptr;
    }

    if (auto* container = blockContainer(renderer))
        return container->modernLineLayout();

    return nullptr;
}

const LineLayout* LineLayout::containing(const RenderObject& renderer)
{
    return containing(const_cast<RenderObject&>(renderer));
}

bool LineLayout::isEnabled()
{
    return DeprecatedGlobalSettings::inlineFormattingContextIntegrationEnabled();
}

bool LineLayout::canUseFor(const RenderBlockFlow& flow)
{
    if (!isEnabled())
        return false;

    return canUseForLineLayout(flow);
}

bool LineLayout::canUseForPreferredWidthComputation(const RenderBlockFlow& flow)
{
    ASSERT(isEnabled());
    return LayoutIntegration::canUseForPreferredWidthComputation(flow);
}

bool LineLayout::shouldInvalidateLineLayoutPathAfterContentChange(const RenderBlockFlow& parent, const RenderObject& rendererWithNewContent, const LineLayout& lineLayout)
{
    ASSERT(isEnabled());
    return shouldInvalidateLineLayoutPathAfterChangeFor(parent, rendererWithNewContent, lineLayout, TypeOfChangeForInvalidation::NodeMutation);
}

bool LineLayout::shouldInvalidateLineLayoutPathAfterTreeMutation(const RenderBlockFlow& parent, const RenderObject& renderer, const LineLayout& lineLayout, bool isRemoval)
{
    ASSERT(isEnabled());
    return shouldInvalidateLineLayoutPathAfterChangeFor(parent, renderer, lineLayout, isRemoval ? TypeOfChangeForInvalidation::NodeRemoval : TypeOfChangeForInvalidation::NodeInsertion);
}

bool LineLayout::shouldSwitchToLegacyOnInvalidation() const
{
    constexpr size_t maximimumBoxTreeSizeForInvalidation = 128;
    if (m_boxTree.boxCount() <= maximimumBoxTreeSizeForInvalidation)
        return false;
    auto isOutOfFlowPositionedContentOnly = [&] {
        auto renderers = m_boxTree.renderers();
        if (renderers.isEmpty())
            return false;
        for (size_t index = 0; index < renderers.size() - 1; ++index) {
            if (!renderers[index]->isOutOfFlowPositioned())
                return false;
        }
        return true;
    };
    return isOutOfFlowPositionedContentOnly();
}

void LineLayout::updateReplacedDimensions(const RenderBox& replaced)
{
    updateLayoutBoxDimensions(replaced);
}

void LineLayout::updateInlineBlockDimensions(const RenderBlock& inlineBlock)
{
    updateLayoutBoxDimensions(inlineBlock);
}

void LineLayout::updateInlineTableDimensions(const RenderTable& inlineTable)
{
    updateLayoutBoxDimensions(inlineTable);
}

void LineLayout::updateListItemDimensions(const RenderListItem& listItem)
{
    updateLayoutBoxDimensions(listItem);
}

void LineLayout::updateListMarkerDimensions(const RenderListMarker& listMarker)
{
    updateLayoutBoxDimensions(listMarker);

    auto& layoutBox = m_boxTree.layoutBoxForRenderer(listMarker);
    if (layoutBox.isListMarkerOutside()) {
        auto* ancestor = listMarker.containingBlock();
        auto offsetFromParentListItem = [&] {
            auto hasAccountedForBorderAndPadding = false;
            auto offset = LayoutUnit { };
            for (; ancestor; ancestor = ancestor->containingBlock()) {
                if (!hasAccountedForBorderAndPadding)
                    offset -= (ancestor->borderStart() + ancestor->paddingStart());
                if (is<RenderListItem>(*ancestor))
                    break;
                if (ancestor->isFlexItem()) {
                    offset -= ancestor->logicalLeft();
                    hasAccountedForBorderAndPadding = true;
                    continue;
                }
                hasAccountedForBorderAndPadding = false;
            }
            return offset;
        }();
        auto offsetFromAssociatedListItem = [&] {
            auto* associatedListItem = listMarker.listItem();
            if (ancestor == associatedListItem || !ancestor) {
                // FIXME: Handle column spanner case when ancestor is null_ptr here.
                return offsetFromParentListItem;
            }
            auto offset = offsetFromParentListItem;
            for (ancestor = ancestor->containingBlock(); ancestor; ancestor = ancestor->containingBlock()) {
                offset -= (ancestor->borderStart() + ancestor->paddingStart());
                if (ancestor == associatedListItem)
                    break;
            }
            return offset;
        }();
        if (offsetFromAssociatedListItem) {
            auto& listMarkerGeometry = layoutState().ensureGeometryForBox(layoutBox);
            // Make sure that the line content does not get pulled in to logical left direction due to
            // the large negative margin (i.e. this ensures that logical left of the list content stays at the line start)
            listMarkerGeometry.setHorizontalMargin({ listMarkerGeometry.marginStart() + offsetFromParentListItem, listMarkerGeometry.marginEnd() - offsetFromParentListItem });
            if (auto nestedOffset = offsetFromAssociatedListItem - offsetFromParentListItem)
                m_nestedListMarkerOffsets.set(&layoutBox, nestedOffset);
        }
    }
}

static inline LayoutUnit contentLogicalWidthForRenderer(const RenderBox& renderer)
{
    return renderer.parent()->style().isHorizontalWritingMode() ? renderer.contentWidth() : renderer.contentHeight();
}

static inline LayoutUnit contentLogicalHeightForRenderer(const RenderBox& renderer)
{
    return renderer.parent()->style().isHorizontalWritingMode() ? renderer.contentHeight() : renderer.contentWidth();
}

static inline Layout::BoxGeometry::HorizontalMargin horizontalLogicalMargin(const RenderBoxModelObject& renderer, bool isLeftToRightInlineDirection, bool isHorizontalWritingMode, bool retainMarginStart = true, bool retainMarginEnd = true)
{
    auto marginLeft = renderer.marginLeft();
    auto marginRight = renderer.marginRight();
    if (isHorizontalWritingMode) {
        if (isLeftToRightInlineDirection)
            return { retainMarginStart ? marginLeft : 0_lu, retainMarginEnd ? marginRight : 0_lu };
        return { retainMarginStart ? marginRight : 0_lu, retainMarginEnd ? marginLeft : 0_lu };
    }

    auto marginTop = renderer.marginTop();
    auto marginBottom = renderer.marginBottom();
    if (isLeftToRightInlineDirection)
        return { retainMarginStart ? marginTop : 0_lu, retainMarginEnd ? marginBottom : 0_lu };
    return { retainMarginStart ? marginBottom : 0_lu, retainMarginEnd ? marginTop : 0_lu };
}

static inline Layout::BoxGeometry::VerticalMargin verticalLogicalMargin(const RenderBoxModelObject& renderer, BlockFlowDirection blockFlowDirection)
{
    switch (blockFlowDirection) {
    case BlockFlowDirection::TopToBottom:
    case BlockFlowDirection::BottomToTop:
        return { renderer.marginTop(), renderer.marginBottom() };
    case BlockFlowDirection::LeftToRight:
    case BlockFlowDirection::RightToLeft:
        return { renderer.marginRight(), renderer.marginLeft() };
    default:
        ASSERT_NOT_REACHED();
        return { renderer.marginTop(), renderer.marginBottom() };
    }
}

enum class IsPartOfFormattingContext : bool { No, Yes };
static inline Layout::Edges logicalBorder(const RenderBoxModelObject& renderer, bool isLeftToRightInlineDirection, BlockFlowDirection blockFlowDirection, IsPartOfFormattingContext isPartOfFormattingContext = IsPartOfFormattingContext::No, bool retainBorderStart = true, bool retainBorderEnd = true)
{
    auto borderLeft = renderer.borderLeft();
    auto borderRight = renderer.borderRight();
    auto borderTop = renderer.borderTop();
    auto borderBottom = renderer.borderBottom();

    if (blockFlowDirection == BlockFlowDirection::TopToBottom || blockFlowDirection == BlockFlowDirection::BottomToTop) {
        if (isLeftToRightInlineDirection)
            return { { retainBorderStart ? borderLeft : 0_lu, retainBorderEnd ? borderRight : 0_lu }, { borderTop, borderBottom } };
        return { { retainBorderStart ? borderRight : 0_lu, retainBorderEnd ? borderLeft : 0_lu }, { borderTop, borderBottom } };
    }

    auto borderLogicalLeft = retainBorderStart ? isLeftToRightInlineDirection ? borderTop : borderBottom : 0_lu;
    auto borderLogicalRight = retainBorderEnd ? isLeftToRightInlineDirection ? borderBottom : borderTop : 0_lu;
    // For boxes inside the formatting context, right border (padding) always points up, while when converting the formatting context root's border (padding) the directionality matters.
    auto borderLogicalTop = isPartOfFormattingContext == IsPartOfFormattingContext::Yes ? borderRight : blockFlowDirection == BlockFlowDirection::LeftToRight ? borderLeft : borderRight;
    auto borderLogicalBottom = isPartOfFormattingContext == IsPartOfFormattingContext::Yes ? borderLeft : blockFlowDirection == BlockFlowDirection::LeftToRight ? borderRight : borderLeft;
    return { { borderLogicalLeft, borderLogicalRight }, { borderLogicalTop, borderLogicalBottom } };
}

static inline Layout::Edges logicalPadding(const RenderBoxModelObject& renderer, bool isLeftToRightInlineDirection, BlockFlowDirection blockFlowDirection, IsPartOfFormattingContext isPartOfFormattingContext = IsPartOfFormattingContext::No, bool retainPaddingStart = true, bool retainPaddingEnd = true)
{
    auto paddingLeft = renderer.paddingLeft();
    auto paddingRight = renderer.paddingRight();
    auto paddingTop = renderer.paddingTop();
    auto paddingBottom = renderer.paddingBottom();

    if (blockFlowDirection == BlockFlowDirection::TopToBottom || blockFlowDirection == BlockFlowDirection::BottomToTop) {
        if (isLeftToRightInlineDirection)
            return { { retainPaddingStart ? paddingLeft : 0_lu, retainPaddingEnd ? paddingRight : 0_lu }, { paddingTop, paddingBottom } };
        return { { retainPaddingStart ? paddingRight : 0_lu, retainPaddingEnd ? paddingLeft : 0_lu }, { paddingTop, paddingBottom } };
    }

    auto paddingLogicalLeft = retainPaddingStart ? isLeftToRightInlineDirection ? paddingTop : paddingBottom : 0_lu;
    auto paddingLogicalRight = retainPaddingEnd ? isLeftToRightInlineDirection ? paddingBottom : paddingTop : 0_lu;
    // For boxes inside the formatting context, right padding (border) always points up, while when converting the formatting context root's padding (border) the directionality matters.
    auto paddingLogicalTop = isPartOfFormattingContext == IsPartOfFormattingContext::Yes ? paddingRight : blockFlowDirection == BlockFlowDirection::LeftToRight ? paddingLeft : paddingRight;
    auto paddingLogicalBottom = isPartOfFormattingContext == IsPartOfFormattingContext::Yes ? paddingLeft : blockFlowDirection == BlockFlowDirection::LeftToRight ? paddingRight : paddingLeft;
    return { { paddingLogicalLeft, paddingLogicalRight }, { paddingLogicalTop, paddingLogicalBottom } };
}

static inline LayoutSize scrollbarLogicalSize(const RenderBox& renderer)
{
    // Scrollbars eat into the padding box area. They never stretch the border box but they may shrink the padding box.
    // In legacy render tree, RenderBox::contentWidth/contentHeight values are adjusted to accommodate the scrollbar width/height.
    // e.g. <div style="width: 10px; overflow: scroll;">content</div>, RenderBox::contentWidth() won't be returning the value of 10px but instead 0px (10px - 15px).
    auto horizontalSpaceReservedForScrollbar = std::max(0_lu, renderer.paddingBoxRectIncludingScrollbar().width() - renderer.paddingBoxWidth());
    auto verticalSpaceReservedForScrollbar = std::max(0_lu, renderer.paddingBoxRectIncludingScrollbar().height() - renderer.paddingBoxHeight());
    return { horizontalSpaceReservedForScrollbar, verticalSpaceReservedForScrollbar };
}

void LineLayout::updateLayoutBoxDimensions(const RenderBox& replacedOrInlineBlock)
{
    auto& layoutBox = m_boxTree.layoutBoxForRenderer(replacedOrInlineBlock);

    auto& replacedBoxGeometry = layoutState().ensureGeometryForBox(layoutBox);
    auto scrollbarSize = scrollbarLogicalSize(replacedOrInlineBlock);
    replacedBoxGeometry.setHorizontalSpaceForScrollbar(scrollbarSize.width());
    replacedBoxGeometry.setVerticalSpaceForScrollbar(scrollbarSize.height());

    replacedBoxGeometry.setContentBoxWidth(contentLogicalWidthForRenderer(replacedOrInlineBlock));
    replacedBoxGeometry.setContentBoxHeight(contentLogicalHeightForRenderer(replacedOrInlineBlock));

    auto isLeftToRightInlineDirection = replacedOrInlineBlock.parent()->style().isLeftToRightDirection();
    auto blockFlowDirection = writingModeToBlockFlowDirection(replacedOrInlineBlock.parent()->style().writingMode());

    replacedBoxGeometry.setVerticalMargin(verticalLogicalMargin(replacedOrInlineBlock, blockFlowDirection));
    replacedBoxGeometry.setHorizontalMargin(horizontalLogicalMargin(replacedOrInlineBlock, isLeftToRightInlineDirection, blockFlowDirection == BlockFlowDirection::TopToBottom || blockFlowDirection == BlockFlowDirection::BottomToTop));
    replacedBoxGeometry.setBorder(logicalBorder(replacedOrInlineBlock, isLeftToRightInlineDirection, blockFlowDirection));
    replacedBoxGeometry.setPadding(logicalPadding(replacedOrInlineBlock, isLeftToRightInlineDirection, blockFlowDirection));

    auto hasNonSyntheticBaseline = [&] {
        if (is<RenderListMarker>(replacedOrInlineBlock))
            return !downcast<RenderListMarker>(replacedOrInlineBlock).isImage();

        if ((is<RenderReplaced>(replacedOrInlineBlock) && replacedOrInlineBlock.style().display() == DisplayType::Inline)
            || is<RenderListBox>(replacedOrInlineBlock)
            || is<RenderSlider>(replacedOrInlineBlock)
            || is<RenderTextControlMultiLine>(replacedOrInlineBlock)
            || is<RenderTable>(replacedOrInlineBlock)
            || is<RenderGrid>(replacedOrInlineBlock)
            || is<RenderFlexibleBox>(replacedOrInlineBlock)
            || is<RenderDeprecatedFlexibleBox>(replacedOrInlineBlock)
#if ENABLE(ATTACHMENT_ELEMENT)
            || is<RenderAttachment>(replacedOrInlineBlock)
#endif
#if ENABLE(MATHML)
            || is<RenderMathMLBlock>(replacedOrInlineBlock)
#endif
            || is<RenderButton>(replacedOrInlineBlock)) {
            // These are special RenderBlock renderers that override the default baseline position behavior of the inline block box.
            return true;
        }
        if (!is<RenderBlockFlow>(replacedOrInlineBlock))
            return false;
        auto& blockFlow = downcast<RenderBlockFlow>(replacedOrInlineBlock);
        auto hasAppareance = blockFlow.style().hasEffectiveAppearance() && !blockFlow.theme().isControlContainer(blockFlow.style().effectiveAppearance());
        return hasAppareance || !blockFlow.childrenInline() || blockFlow.hasLines() || blockFlow.hasLineIfEmpty();
    }();
    if (hasNonSyntheticBaseline) {
        auto baseline = replacedOrInlineBlock.baselinePosition(AlphabeticBaseline, false /* firstLine */, blockFlowDirection == BlockFlowDirection::TopToBottom || blockFlowDirection == BlockFlowDirection::BottomToTop ? HorizontalLine : VerticalLine, PositionOnContainingLine);
        layoutBox.setBaselineForIntegration(roundToInt(baseline));
    }

    if (auto* shapeOutsideInfo = replacedOrInlineBlock.shapeOutsideInfo())
        layoutBox.setShape(&shapeOutsideInfo->computedShape());
}

void LineLayout::updateLineBreakBoxDimensions(const RenderLineBreak& lineBreakBox)
{
    // This is just a box geometry reset (see InlineFormattingContext::layoutInFlowContent).
    auto& boxGeometry = layoutState().ensureGeometryForBox(m_boxTree.layoutBoxForRenderer(lineBreakBox));

    boxGeometry.setHorizontalMargin({ });
    boxGeometry.setBorder({ });
    boxGeometry.setPadding({ });
    boxGeometry.setContentBoxWidth({ });
    boxGeometry.setVerticalMargin({ });
    if (lineBreakBox.style().hasOutOfFlowPosition())
        boxGeometry.setContentBoxHeight({ });
}

void LineLayout::updateInlineBoxDimensions(const RenderInline& renderInline)
{
    auto& boxGeometry = layoutState().ensureGeometryForBox(m_boxTree.layoutBoxForRenderer(renderInline));

    // Check if this renderer is part of a continuation and adjust horizontal margin/border/padding accordingly.
    auto shouldNotRetainBorderPaddingAndMarginStart = renderInline.isContinuation();
    auto shouldNotRetainBorderPaddingAndMarginEnd = !renderInline.isContinuation() && renderInline.inlineContinuation();

    boxGeometry.setVerticalMargin({ });
    auto isLeftToRightInlineDirection = renderInline.style().isLeftToRightDirection();
    auto blockFlowDirection = writingModeToBlockFlowDirection(renderInline.style().writingMode());

    boxGeometry.setHorizontalMargin(horizontalLogicalMargin(renderInline, isLeftToRightInlineDirection, blockFlowDirection == BlockFlowDirection::TopToBottom || blockFlowDirection == BlockFlowDirection::BottomToTop, !shouldNotRetainBorderPaddingAndMarginStart, !shouldNotRetainBorderPaddingAndMarginEnd));
    boxGeometry.setVerticalMargin(verticalLogicalMargin(renderInline, blockFlowDirection));
    boxGeometry.setBorder(logicalBorder(renderInline, isLeftToRightInlineDirection, blockFlowDirection, IsPartOfFormattingContext::Yes, !shouldNotRetainBorderPaddingAndMarginStart, !shouldNotRetainBorderPaddingAndMarginEnd));
    boxGeometry.setPadding(logicalPadding(renderInline, isLeftToRightInlineDirection, blockFlowDirection, IsPartOfFormattingContext::Yes, !shouldNotRetainBorderPaddingAndMarginStart, !shouldNotRetainBorderPaddingAndMarginEnd));
}

void LineLayout::updateInlineContentDimensions()
{
    for (auto walker = InlineWalker(flow()); !walker.atEnd(); walker.advance()) {
        auto& renderer = *walker.current();

        if (is<RenderReplaced>(renderer)) {
            updateReplacedDimensions(downcast<RenderReplaced>(renderer));
            continue;
        }
        if (is<RenderTable>(renderer)) {
            updateInlineTableDimensions(downcast<RenderTable>(renderer));
            continue;
        }
        if (is<RenderListMarker>(renderer)) {
            updateListMarkerDimensions(downcast<RenderListMarker>(renderer));
            continue;
        }
        if (is<RenderListItem>(renderer)) {
            updateListItemDimensions(downcast<RenderListItem>(renderer));
            continue;
        }
        if (is<RenderBlock>(renderer)) {
            updateInlineBlockDimensions(downcast<RenderBlock>(renderer));
            continue;
        }
        if (is<RenderLineBreak>(renderer)) {
            updateLineBreakBoxDimensions(downcast<RenderLineBreak>(renderer));
            continue;
        }
        if (is<RenderInline>(renderer)) {
            updateInlineBoxDimensions(downcast<RenderInline>(renderer));
            continue;
        }
    }
}

void LineLayout::updateStyle(const RenderBoxModelObject& renderer, const RenderStyle& oldStyle)
{
    if (m_inlineContent) {
        auto invalidation = Layout::InlineInvalidation { ensureLineDamage(), m_inlineContentCache.inlineItems().content(), m_inlineContent->displayContent() };
        invalidation.styleChanged(m_boxTree.layoutBoxForRenderer(renderer), oldStyle);
    }
    m_boxTree.updateStyle(renderer);
}

void LineLayout::updateOverflow()
{
    InlineContentBuilder { flow(), m_boxTree }.updateLineOverflow(*m_inlineContent);
}

std::pair<LayoutUnit, LayoutUnit> LineLayout::computeIntrinsicWidthConstraints()
{
    auto parentBlockLayoutState = Layout::BlockLayoutState { m_blockFormattingState.placedFloats() };
    auto constraints = Layout::InlineFormattingContext { rootLayoutBox(), layoutState(), parentBlockLayoutState }.computedIntrinsicSizes(m_lineDamage.get());
    return { constraints.minimum, constraints.maximum };
}

static inline std::optional<Layout::BlockLayoutState::LineClamp> lineClamp(const RenderBlockFlow& rootRenderer)
{
    auto& layoutState = *rootRenderer.view().frameView().layoutContext().layoutState();
    if (auto lineClamp = layoutState.lineClamp())
        return Layout::BlockLayoutState::LineClamp { lineClamp->maximumLineCount, lineClamp->currentLineCount };
    return { };
}

static inline Layout::BlockLayoutState::TextBoxTrim textBoxTrim(const RenderBlockFlow& rootRenderer)
{
    auto* layoutState = rootRenderer.view().frameView().layoutContext().layoutState();
    if (!layoutState)
        return { };
    auto textBoxTrimForIFC = Layout::BlockLayoutState::TextBoxTrim { };
    if (layoutState->hasTextBoxTrimStart())
        textBoxTrimForIFC.add(Layout::BlockLayoutState::TextBoxTrimSide::Start);
    if (layoutState->hasTextBoxTrimEnd(rootRenderer))
        textBoxTrimForIFC.add(Layout::BlockLayoutState::TextBoxTrimSide::End);
    return textBoxTrimForIFC;
}

static inline std::optional<Layout::BlockLayoutState::LineGrid> lineGrid(const RenderBlockFlow& rootRenderer)
{
    auto& layoutState = *rootRenderer.view().frameView().layoutContext().layoutState();
    if (auto* lineGrid = layoutState.lineGrid()) {
        if (lineGrid->style().writingMode() != rootRenderer.style().writingMode())
            return { };

        auto layoutOffset = layoutState.layoutOffset();
        auto lineGridOffset = layoutState.lineGridOffset();
        if (lineGrid->style().isVerticalWritingMode()) {
            layoutOffset = layoutOffset.transposedSize();
            lineGridOffset = lineGridOffset.transposedSize();
        }

        auto columnWidth = lineGrid->style().fontCascade().primaryFont().maxCharWidth();
        auto rowHeight = lineGrid->style().computedLineHeight();
        auto topRowOffset = lineGrid->borderAndPaddingBefore();

        std::optional<LayoutSize> paginationOrigin;
        auto pageLogicalTop = 0_lu;
        if (layoutState.isPaginated()) {
            paginationOrigin = layoutState.lineGridPaginationOrigin();
            if (lineGrid->style().isVerticalWritingMode())
                paginationOrigin = paginationOrigin->transposedSize();
            pageLogicalTop = rootRenderer.pageLogicalTopForOffset(0_lu);
        }

        return Layout::BlockLayoutState::LineGrid { layoutOffset, lineGridOffset, columnWidth, rowHeight, topRowOffset, lineGrid->style().fontCascade().primaryFont(), paginationOrigin, pageLogicalTop };
    }

    return { };
}

std::optional<LayoutRect> LineLayout::layout()
{
    prepareLayoutState();
    preparePlacedFloats();

    // FIXME: Partial layout should not rely on inline display content, but instead InlineContentCache
    // should retain all the pieces of data required -and then we can destroy damaged content here instead of after
    // layout in constructContent.
    auto isPartialLayout = isDamaged() && m_lineDamage->start();
    if (!isPartialLayout) {
        m_lineDamage = { };
        clearInlineContent();
    }
    ASSERT(m_inlineContentConstraints);
    auto intrusiveInitialLetterBottom = [&]() -> std::optional<LayoutUnit> {
        if (auto lowestInitialLetterLogicalBottom = flow().lowestInitialLetterLogicalBottom())
            return { *lowestInitialLetterLogicalBottom - m_inlineContentConstraints->logicalTop() };
        return { };
    };
    auto inlineContentConstraints = [&]() -> Layout::ConstraintsForInlineContent {
        if (!isPartialLayout || !m_inlineContent)
            return *m_inlineContentConstraints;
        auto damagedLineIndex = m_lineDamage->start()->lineIndex;
        if (!damagedLineIndex)
            return *m_inlineContentConstraints;
        if (damagedLineIndex >= m_inlineContent->displayContent().lines.size()) {
            ASSERT_NOT_REACHED();
            return *m_inlineContentConstraints;
        }
        auto partialContentTop = LayoutUnit { m_inlineContent->displayContent().lines[damagedLineIndex - 1].lineBoxLogicalRect().maxY() };
        auto constraintsForInFlowContent = Layout::ConstraintsForInFlowContent { m_inlineContentConstraints->horizontal(), partialContentTop };
        return { constraintsForInFlowContent, m_inlineContentConstraints->visualLeft() };
    };

    auto parentBlockLayoutState = Layout::BlockLayoutState {
        m_blockFormattingState.placedFloats(),
        lineClamp(flow()),
        textBoxTrim(flow()),
        intrusiveInitialLetterBottom(),
        lineGrid(flow())
    };
    auto inlineFormattingContext = Layout::InlineFormattingContext { rootLayoutBox(), layoutState(), parentBlockLayoutState };
    // Temporary, integration only.
    inlineFormattingContext.layoutState().setNestedListMarkerOffsets(WTFMove(m_nestedListMarkerOffsets));
    auto layoutResult = inlineFormattingContext.layout(inlineContentConstraints(), m_lineDamage.get());
    auto repaintRect = LayoutRect { constructContent(inlineFormattingContext.layoutState(), WTFMove(layoutResult)) };
    auto adjustments = adjustContent(parentBlockLayoutState);

    updateRenderTreePositions(adjustments);

    m_lineDamage = { };

    return isPartialLayout ? std::make_optional(repaintRect) : std::nullopt;
}

FloatRect LineLayout::constructContent(const Layout::InlineLayoutState& inlineLayoutState, Layout::InlineLayoutResult&& layoutResult)
{
    auto damagedRect = InlineContentBuilder { flow(), m_boxTree }.build(WTFMove(layoutResult), ensureInlineContent(), m_lineDamage.get());

    m_inlineContent->clearGapBeforeFirstLine = inlineLayoutState.clearGapBeforeFirstLine();
    m_inlineContent->clearGapAfterLastLine = inlineLayoutState.clearGapAfterLastLine();
    m_inlineContent->shrinkToFit();

    m_inlineContentCache.inlineItems().shrinkToFit();
    m_blockFormattingState.shrinkToFit();

    // FIXME: These needs to be incorporated into the partial damage.
    auto additionalHeight = m_inlineContent->firstLinePaginationOffset + m_inlineContent->clearGapBeforeFirstLine + m_inlineContent->clearGapAfterLastLine;
    damagedRect.expand({ 0, additionalHeight });
    return damagedRect;
}

void LineLayout::updateRenderTreePositions(const Vector<LineAdjustment>& lineAdjustments)
{
    if (!m_inlineContent)
        return;

    auto& blockFlow = flow();
    auto& rootStyle = blockFlow.style();
    auto isLeftToRightPlacedFloatsInlineDirection = m_blockFormattingState.placedFloats().isLeftToRightDirection();
    auto writingMode = rootStyle.writingMode();
    auto isHorizontalWritingMode = WebCore::isHorizontalWritingMode(writingMode);

    auto visualAdjustmentOffset = [&](auto lineIndex) {
        if (lineAdjustments.isEmpty())
            return LayoutSize { };
        if (!isHorizontalWritingMode)
            return LayoutSize { lineAdjustments[lineIndex].offset, 0_lu };
        return LayoutSize { 0_lu, lineAdjustments[lineIndex].offset };
    };

    for (auto& box : m_inlineContent->displayContent().boxes) {
        if (box.isInlineBox() || box.isText())
            continue;

        auto& layoutBox = box.layoutBox();
        if (!layoutBox.isAtomicInlineLevelBox())
            continue;

        auto& renderer = downcast<RenderBox>(m_boxTree.rendererForLayoutBox(layoutBox));
        if (auto* layer = renderer.layer())
            layer->setIsHiddenByOverflowTruncation(box.isFullyTruncated());

        auto& logicalGeometry = layoutState().geometryForBox(layoutBox);
        auto adjustmentOffset = visualAdjustmentOffset(box.lineIndex());

        renderer.setLocation(Layout::BoxGeometry::borderBoxRect(logicalGeometry).topLeft() + adjustmentOffset);
        auto relayoutRubyAnnotationIfNeeded = [&] {
            // Annotation inline-block may get resized during inline layout (when base is wider) and
            // we need to apply this new size on the annotation content by running layout.
            if (!layoutBox.isRubyAnnotationBox())
                return;
            auto usedMarginBoxSize = Layout::BoxGeometry::marginBoxRect(logicalGeometry).size();
            if (usedMarginBoxSize == renderer.size())
                return;
            renderer.setSize(usedMarginBoxSize);
            renderer.setOverridingLogicalWidthLength({ usedMarginBoxSize.width(), LengthType::Fixed });
            renderer.setNeedsLayout(MarkOnlyThis);
            renderer.layoutIfNeeded();
            renderer.clearOverridingLogicalWidthLength();
        };
        relayoutRubyAnnotationIfNeeded();
    }

    HashMap<CheckedRef<const Layout::Box>, LayoutSize> floatPaginationOffsetMap;
    if (!lineAdjustments.isEmpty()) {
        for (auto& floatItem : m_blockFormattingState.placedFloats().list()) {
            if (!floatItem.layoutBox() || !floatItem.placedByLine())
                continue;
            auto adjustmentOffset = visualAdjustmentOffset(*floatItem.placedByLine());
            floatPaginationOffsetMap.add(*floatItem.layoutBox(), adjustmentOffset);
        }
    }

    // FIXME: Loop through the floating state?
    for (auto& renderObject : m_boxTree.renderers()) {
        auto& layoutBox = *renderObject->layoutBox();
        if (!layoutBox.isFloatingPositioned() && !layoutBox.isOutOfFlowPositioned())
            continue;
        if (layoutBox.isLineBreakBox())
            continue;
        auto& renderer = downcast<RenderBox>(m_boxTree.rendererForLayoutBox(layoutBox));
        auto& logicalGeometry = layoutState().geometryForBox(layoutBox);

        if (layoutBox.isFloatingPositioned()) {
            auto& floatingObject = flow().insertFloatingObjectForIFC(renderer);

            ASSERT(m_inlineContentConstraints);
            auto rootBorderBoxWidth = m_inlineContentConstraints->visualLeft() + m_inlineContentConstraints->horizontal().logicalWidth + m_inlineContentConstraints->horizontal().logicalLeft;

            auto visualGeometry = logicalGeometry.geometryForWritingModeAndDirection(writingMode, isLeftToRightPlacedFloatsInlineDirection, rootBorderBoxWidth);
            auto visualMarginBoxRect = LayoutRect { Layout::BoxGeometry::marginBoxRect(visualGeometry) };
            auto visualBorderBoxRect = LayoutRect { Layout::BoxGeometry::borderBoxRect(visualGeometry) };

            auto paginationOffset = floatPaginationOffsetMap.getOptional(layoutBox);
            if (paginationOffset) {
                visualMarginBoxRect.move(*paginationOffset);
                visualBorderBoxRect.move(*paginationOffset);
            }

            floatingObject.setFrameRect(visualMarginBoxRect);
            floatingObject.setMarginOffset({ visualGeometry.marginStart(), visualGeometry.marginBefore() });
            floatingObject.setIsPlaced(true);

            auto oldRect = renderer.frameRect();
            renderer.setLocation(visualBorderBoxRect.location());

            if (renderer.checkForRepaintDuringLayout()) {
                auto hasMoved = oldRect.location() != renderer.location();
                if (hasMoved)
                    renderer.repaintDuringLayoutIfMoved(oldRect);
                else
                    renderer.repaint();
            }

            if (paginationOffset) {
                // Float content may be affected by the new position.
                renderer.markForPaginationRelayoutIfNeeded();
                renderer.layoutIfNeeded();
            }

            continue;
        }

        if (layoutBox.isOutOfFlowPositioned()) {
            ASSERT(renderer.layer());
            auto& layer = *renderer.layer();
            auto logicalBorderBoxRect = LayoutRect { Layout::BoxGeometry::borderBoxRect(logicalGeometry) };
            auto previousStaticPosition = LayoutPoint { layer.staticInlinePosition(), layer.staticBlockPosition() };
            auto delta = logicalBorderBoxRect.location() - previousStaticPosition;
            auto hasStaticInlinePositioning = layoutBox.style().hasStaticInlinePosition(renderer.isHorizontalWritingMode());

            if (layoutBox.style().isOriginalDisplayInlineType()) {
                blockFlow.setStaticInlinePositionForChild(renderer, logicalBorderBoxRect.y(), logicalBorderBoxRect.x());
                if (hasStaticInlinePositioning)
                    renderer.move(delta.width(), delta.height());
            }

            layer.setStaticBlockPosition(logicalBorderBoxRect.y());
            layer.setStaticInlinePosition(logicalBorderBoxRect.x());

            if (!delta.isZero() && hasStaticInlinePositioning)
                renderer.setChildNeedsLayout(MarkOnlyThis);
            continue;
        }
    }
}

void LineLayout::updateInlineContentConstraints()
{
    auto& flow = this->flow();
    auto isLeftToRightInlineDirection = flow.style().isLeftToRightDirection();
    auto writingMode = flow.style().writingMode();
    auto blockFlowDirection = writingModeToBlockFlowDirection(writingMode);
    auto padding = logicalPadding(flow, isLeftToRightInlineDirection, blockFlowDirection, IsPartOfFormattingContext::No);
    auto border = logicalBorder(flow, isLeftToRightInlineDirection, blockFlowDirection, IsPartOfFormattingContext::No);
    auto scrollbarSize = scrollbarLogicalSize(flow);
    auto shouldPlaceVerticalScrollbarOnLeft = flow.shouldPlaceVerticalScrollbarOnLeft();

    auto contentBoxWidth = WebCore::isHorizontalWritingMode(writingMode) ? flow.contentWidth() : flow.contentHeight();
    auto contentBoxLeft = border.horizontal.left + padding.horizontal.left + (isLeftToRightInlineDirection && shouldPlaceVerticalScrollbarOnLeft ? scrollbarSize.width() : 0_lu);
    auto contentBoxTop = border.vertical.top + padding.vertical.top;

    auto horizontalConstraints = Layout::HorizontalConstraints { contentBoxLeft, contentBoxWidth };
    auto visualLeft = !isLeftToRightInlineDirection || shouldPlaceVerticalScrollbarOnLeft ? border.horizontal.right + scrollbarSize.width() + padding.horizontal.right : contentBoxLeft;

    m_inlineContentConstraints = { { horizontalConstraints, contentBoxTop }, visualLeft };

    auto createRootGeometryIfNeeded = [&] {
        // FIXME: BFC should be responsible for creating the box geometry for this block box (IFC root) as part of the block layout.
        auto& rootGeometry = layoutState().ensureGeometryForBox(rootLayoutBox());
        rootGeometry.setContentBoxWidth(contentBoxWidth);
        rootGeometry.setPadding(padding);
        rootGeometry.setBorder(border);
        rootGeometry.setHorizontalSpaceForScrollbar(scrollbarSize.width());
        rootGeometry.setVerticalSpaceForScrollbar(scrollbarSize.height());
        rootGeometry.setHorizontalMargin({ });
        rootGeometry.setVerticalMargin({ });
    };
    createRootGeometryIfNeeded();
}

void LineLayout::prepareLayoutState()
{
}

void LineLayout::preparePlacedFloats()
{
    auto& placedFloats = m_blockFormattingState.placedFloats();
    placedFloats.clear();

    if (!flow().containsFloats())
        return;

    auto isHorizontalWritingMode = flow().containingBlock() ? flow().containingBlock()->style().isHorizontalWritingMode() : true;
    auto placedFloatsIsLeftToRightInlineDirection = flow().containingBlock() ? flow().containingBlock()->style().isLeftToRightDirection() : true;
    placedFloats.setIsLeftToRightDirection(placedFloatsIsLeftToRightInlineDirection);
    for (auto& floatingObject : *flow().floatingObjectSet()) {
        auto& visualRect = floatingObject->frameRect();
        auto logicalPosition = [&] {
            switch (floatingObject->renderer().style().floating()) {
            case Float::Left:
                return placedFloatsIsLeftToRightInlineDirection ? Layout::PlacedFloats::Item::Position::Left : Layout::PlacedFloats::Item::Position::Right;
            case Float::Right:
                return placedFloatsIsLeftToRightInlineDirection ? Layout::PlacedFloats::Item::Position::Right : Layout::PlacedFloats::Item::Position::Left;
            case Float::InlineStart: {
                auto* floatBoxContainingBlock = floatingObject->renderer().containingBlock();
                if (floatBoxContainingBlock)
                    return floatBoxContainingBlock->style().isLeftToRightDirection() == placedFloatsIsLeftToRightInlineDirection ? Layout::PlacedFloats::Item::Position::Left : Layout::PlacedFloats::Item::Position::Right;
                return Layout::PlacedFloats::Item::Position::Left;
            }
            case Float::InlineEnd: {
                auto* floatBoxContainingBlock = floatingObject->renderer().containingBlock();
                if (floatBoxContainingBlock)
                    return floatBoxContainingBlock->style().isLeftToRightDirection() == placedFloatsIsLeftToRightInlineDirection ? Layout::PlacedFloats::Item::Position::Right : Layout::PlacedFloats::Item::Position::Left;
                return Layout::PlacedFloats::Item::Position::Right;
            }
            default:
                ASSERT_NOT_REACHED();
                return Layout::PlacedFloats::Item::Position::Left;
            }
        };

        auto boxGeometry = Layout::BoxGeometry { };
        auto logicalRect = [&] {
            // FIXME: We are flooring here for legacy compatibility. See FloatingObjects::intervalForFloatingObject and RenderBlockFlow::clearFloats.
            auto logicalTop = isHorizontalWritingMode ? LayoutUnit(visualRect.y().floor()) : visualRect.x();
            auto logicalLeft = isHorizontalWritingMode ? visualRect.x() : LayoutUnit(visualRect.y().floor());
            auto logicalHeight = (isHorizontalWritingMode ? LayoutUnit(visualRect.maxY().floor()) : visualRect.maxX()) - logicalTop;
            auto logicalWidth = (isHorizontalWritingMode ? visualRect.maxX() : LayoutUnit(visualRect.maxY().floor())) - logicalLeft;
            if (!placedFloatsIsLeftToRightInlineDirection) {
                auto rootBorderBoxWidth = m_inlineContentConstraints->visualLeft() + m_inlineContentConstraints->horizontal().logicalWidth + m_inlineContentConstraints->horizontal().logicalLeft;
                logicalLeft = rootBorderBoxWidth - (logicalLeft + logicalWidth);
            }
            return LayoutRect { logicalLeft, logicalTop, logicalWidth, logicalHeight };
        }();

        boxGeometry.setTopLeft(logicalRect.location());
        boxGeometry.setContentBoxWidth(logicalRect.width());
        boxGeometry.setContentBoxHeight(logicalRect.height());
        boxGeometry.setBorder({ });
        boxGeometry.setPadding({ });
        boxGeometry.setHorizontalMargin({ });
        boxGeometry.setVerticalMargin({ });

        auto shapeOutsideInfo = floatingObject->renderer().shapeOutsideInfo();
        auto* shape = shapeOutsideInfo ? &shapeOutsideInfo->computedShape() : nullptr;

        placedFloats.append({ logicalPosition(), boxGeometry, logicalRect.location(), shape });
    }
}

bool LineLayout::isPaginated() const
{
    return m_inlineContent && m_inlineContent->isPaginated;
}

std::optional<LayoutUnit> LineLayout::clampedContentLogicalHeight() const
{
    if (!m_inlineContent)
        return { };

    auto& lines = m_inlineContent->displayContent().lines;
    if (lines.isEmpty()) {
        // Out-of-flow only content (and/or with floats) may produce blank inline content.
        return { };
    }

    auto firstTruncatedLineIndex = [&]() -> std::optional<size_t> {
        for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            if (lines[lineIndex].isTruncatedInBlockDirection())
                return lineIndex;
        }
        return { };
    }();
    if (!firstTruncatedLineIndex)
        return { };
    if (!*firstTruncatedLineIndex) {
        // This content is fully truncated in the block direction.
        return LayoutUnit { };
    }

    auto contentHeight = lines[*firstTruncatedLineIndex - 1].lineBoxLogicalRect().maxY() - lines.first().lineBoxLogicalRect().y();
    auto additionalHeight = m_inlineContent->firstLinePaginationOffset + m_inlineContent->clearGapBeforeFirstLine + m_inlineContent->clearGapAfterLastLine;
    return LayoutUnit { contentHeight + additionalHeight };
}

LayoutUnit LineLayout::contentBoxLogicalHeight() const
{
    if (!m_inlineContent)
        return { };

    auto& lines = m_inlineContent->displayContent().lines;
    if (lines.isEmpty()) {
        // Out-of-flow only content (and/or with floats) may produce blank inline content.
        return { };
    }

    auto lastLineWithInlineContent = [&] {
        // Out-of-flow/float content only don't produce lines with inline content. They should not be taken into
        // account when computing content box height.
        for (auto& line : makeReversedRange(lines)) {
            ASSERT(line.boxCount());
            if (line.boxCount() > 1)
                return line;
        }
        return lines.first();
    };
    auto contentHeight = lastLineWithInlineContent().lineBoxLogicalRect().maxY() - lines.first().lineBoxLogicalRect().y();
    auto additionalHeight = m_inlineContent->firstLinePaginationOffset + m_inlineContent->clearGapBeforeFirstLine + m_inlineContent->clearGapAfterLastLine;
    return LayoutUnit { contentHeight + additionalHeight };
}

size_t LineLayout::lineCount() const
{
    if (!m_inlineContent)
        return 0;
    if (!m_inlineContent->hasContent())
        return 0;

    return m_inlineContent->displayContent().lines.size();
}

bool LineLayout::hasVisualOverflow() const
{
    return m_inlineContent && m_inlineContent->hasVisualOverflow();
}

LayoutUnit LineLayout::firstLinePhysicalBaseline() const
{
    if (!m_inlineContent || m_inlineContent->displayContent().boxes.isEmpty()) {
        ASSERT_NOT_REACHED();
        return { };
    }

    auto& firstLine = m_inlineContent->displayContent().lines.first();
    return physicalBaselineForLine(firstLine); 
}

LayoutUnit LineLayout::lastLinePhysicalBaseline() const
{
    if (!m_inlineContent || m_inlineContent->displayContent().lines.isEmpty()) {
        ASSERT_NOT_REACHED();
        return { };
    }

    auto lastLine = m_inlineContent->displayContent().lines.last();
    return physicalBaselineForLine(lastLine);
}

LayoutUnit LineLayout::physicalBaselineForLine(const InlineDisplay::Line& line) const
{
    switch (writingModeToBlockFlowDirection(rootLayoutBox().style().writingMode())) {
    case BlockFlowDirection::TopToBottom:
    case BlockFlowDirection::BottomToTop:
        return LayoutUnit { line.lineBoxTop() + line.baseline() };
    case BlockFlowDirection::LeftToRight:
        return LayoutUnit { line.lineBoxLeft() + (line.lineBoxWidth() - line.baseline()) };
    case BlockFlowDirection::RightToLeft:
        return LayoutUnit { line.lineBoxLeft() + line.baseline() };
    default:
        ASSERT_NOT_REACHED();
        return { };
    }
}

LayoutUnit LineLayout::lastLineLogicalBaseline() const
{
    if (!m_inlineContent || m_inlineContent->displayContent().lines.isEmpty()) {
        ASSERT_NOT_REACHED();
        return { };
    }

    auto& lastLine = m_inlineContent->displayContent().lines.last();
    switch (writingModeToBlockFlowDirection(rootLayoutBox().style().writingMode())) {
    case BlockFlowDirection::TopToBottom:
    case BlockFlowDirection::BottomToTop:
        return LayoutUnit { lastLine.lineBoxTop() + lastLine.baseline() };
    case BlockFlowDirection::LeftToRight: {
        // FIXME: We should set the computed height on the root's box geometry (in RenderBlockFlow) so that
        // we could call m_layoutState.geometryForRootBox().borderBoxHeight() instead.

        // Line is always visual coordinates while logicalHeight is not (i.e. this translate to "box visual width" - "line visual right")
        auto lineLogicalTop = flow().logicalHeight() - lastLine.lineBoxRight();
        return LayoutUnit { lineLogicalTop + lastLine.baseline() };
    }
    case BlockFlowDirection::RightToLeft:
        return LayoutUnit { lastLine.lineBoxLeft() + lastLine.baseline() };
    default:
        ASSERT_NOT_REACHED();
        return { };
    }
}

Vector<LineAdjustment> LineLayout::adjustContent(const Layout::BlockLayoutState& blockLayoutState)
{
    if (!m_inlineContent)
        return { };

    auto& layoutState = *flow().view().frameView().layoutContext().layoutState();
    if (!layoutState.isPaginated())
        return { };

    auto adjustments = computeAdjustmentsForPagination(*m_inlineContent, m_blockFormattingState.placedFloats(), blockLayoutState, flow());
    adjustLinePositionsForPagination(*m_inlineContent, adjustments);

    return adjustments;
}

void LineLayout::collectOverflow()
{
    if (!m_inlineContent)
        return;

    for (auto& line : m_inlineContent->displayContent().lines) {
        flow().addLayoutOverflow(Layout::toLayoutRect(line.scrollableOverflow()));
        if (!flow().hasNonVisibleOverflow())
            flow().addVisualOverflow(Layout::toLayoutRect(line.inkOverflow()));
    }
}

InlineContent& LineLayout::ensureInlineContent()
{
    if (!m_inlineContent)
        m_inlineContent = makeUnique<InlineContent>(*this);
    return *m_inlineContent;
}

InlineIterator::TextBoxIterator LineLayout::textBoxesFor(const RenderText& renderText) const
{
    if (!m_inlineContent)
        return { };

    auto& layoutBox = m_boxTree.layoutBoxForRenderer(renderText);
    auto firstIndex = m_inlineContent->firstBoxIndexForLayoutBox(layoutBox);
    if (!firstIndex)
        return { };

    return InlineIterator::textBoxFor(*m_inlineContent, *firstIndex);
}

InlineIterator::LeafBoxIterator LineLayout::boxFor(const RenderElement& renderElement) const
{
    if (!m_inlineContent)
        return { };

    auto& layoutBox = m_boxTree.layoutBoxForRenderer(renderElement);
    auto firstIndex = m_inlineContent->firstBoxIndexForLayoutBox(layoutBox);
    if (!firstIndex)
        return { };

    return InlineIterator::boxFor(*m_inlineContent, *firstIndex);
}

InlineIterator::InlineBoxIterator LineLayout::firstInlineBoxFor(const RenderInline& renderInline) const
{
    if (!m_inlineContent)
        return { };

    auto& layoutBox = m_boxTree.layoutBoxForRenderer(renderInline);
    auto* box = m_inlineContent->firstBoxForLayoutBox(layoutBox);
    if (!box)
        return { };

    return InlineIterator::inlineBoxFor(*m_inlineContent, *box);
}

InlineIterator::InlineBoxIterator LineLayout::firstRootInlineBox() const
{
    if (!m_inlineContent || !m_inlineContent->hasContent())
        return { };

    return InlineIterator::inlineBoxFor(*m_inlineContent, m_inlineContent->displayContent().boxes[0]);
}

InlineIterator::LineBoxIterator LineLayout::firstLineBox() const
{
    if (!m_inlineContent)
        return { };

    return { InlineIterator::LineBoxIteratorModernPath(*m_inlineContent, 0) };
}

InlineIterator::LineBoxIterator LineLayout::lastLineBox() const
{
    if (!m_inlineContent)
        return { };

    return { InlineIterator::LineBoxIteratorModernPath(*m_inlineContent, m_inlineContent->displayContent().lines.isEmpty() ? 0 : m_inlineContent->displayContent().lines.size() - 1) };
}

LayoutRect LineLayout::firstInlineBoxRect(const RenderInline& renderInline) const
{
    if (!m_inlineContent)
        return { };

    auto& layoutBox = m_boxTree.layoutBoxForRenderer(renderInline);
    auto* firstBox = m_inlineContent->firstBoxForLayoutBox(layoutBox);
    if (!firstBox)
        return { };

    // FIXME: We should be able to flip the display boxes soon after the root block
    // is finished sizing in one go.
    auto firstBoxRect = Layout::toLayoutRect(firstBox->visualRectIgnoringBlockDirection());
    switch (writingModeToBlockFlowDirection(rootLayoutBox().style().writingMode())) {
    case BlockFlowDirection::TopToBottom:
    case BlockFlowDirection::BottomToTop:
    case BlockFlowDirection::LeftToRight:
        return firstBoxRect;
    case BlockFlowDirection::RightToLeft:
        firstBoxRect.setX(flow().width() - firstBoxRect.maxX());
        return firstBoxRect;
    default:
        ASSERT_NOT_REACHED();
        return firstBoxRect;
    }
}

LayoutRect LineLayout::enclosingBorderBoxRectFor(const RenderInline& renderInline) const
{
    if (!m_inlineContent)
        return { };

    // FIXME: This keeps the existing output.
    if (!m_inlineContent->hasContent())
        return { };

    return Layout::BoxGeometry::borderBoxRect(layoutState().geometryForBox(m_boxTree.layoutBoxForRenderer(renderInline)));
}

LayoutRect LineLayout::visualOverflowBoundingBoxRectFor(const RenderInline& renderInline) const
{
    if (!m_inlineContent)
        return { };

    auto& layoutBox = m_boxTree.layoutBoxForRenderer(renderInline);

    LayoutRect result;
    m_inlineContent->traverseNonRootInlineBoxes(layoutBox, [&](auto& inlineBox) {
        result.unite(Layout::toLayoutRect(inlineBox.inkOverflow()));
    });

    return result;
}

Vector<FloatRect> LineLayout::collectInlineBoxRects(const RenderInline& renderInline) const
{
    if (!m_inlineContent)
        return { };

    auto& layoutBox = m_boxTree.layoutBoxForRenderer(renderInline);

    Vector<FloatRect> result;
    m_inlineContent->traverseNonRootInlineBoxes(layoutBox, [&](auto& inlineBox) {
        result.append(inlineBox.visualRectIgnoringBlockDirection());
    });

    return result;
}

const RenderObject& LineLayout::rendererForLayoutBox(const Layout::Box& layoutBox) const
{
    return m_boxTree.rendererForLayoutBox(layoutBox);
}

const Layout::ElementBox& LineLayout::rootLayoutBox() const
{
    return m_boxTree.rootLayoutBox();
}

Layout::ElementBox& LineLayout::rootLayoutBox()
{
    return m_boxTree.rootLayoutBox();
}

static LayoutPoint flippedContentOffsetIfNeeded(const RenderBlockFlow& root, const RenderBox& childRenderer, LayoutPoint contentOffset)
{
    if (root.style().isFlippedBlocksWritingMode())
        return root.flipForWritingModeForChild(childRenderer, contentOffset);
    return contentOffset;
}

static LayoutRect flippedRectForWritingMode(const RenderBlockFlow& root, const FloatRect& rect)
{
    auto flippedRect = LayoutRect { rect };
    root.flipForWritingMode(flippedRect);
    return flippedRect;
}

void LineLayout::paint(PaintInfo& paintInfo, const LayoutPoint& paintOffset, const RenderInline* layerRenderer)
{
    if (!m_inlineContent)
        return;

    auto shouldPaintForPhase = [&] {
        switch (paintInfo.phase) {
        case PaintPhase::Accessibility:
        case PaintPhase::Foreground:
        case PaintPhase::EventRegion:
        case PaintPhase::TextClip:
        case PaintPhase::Mask:
        case PaintPhase::Selection:
        case PaintPhase::Outline:
        case PaintPhase::ChildOutlines:
        case PaintPhase::SelfOutline:
            return true;
        default:
            return false;
        }
    };
    if (!shouldPaintForPhase())
        return;

    InlineContentPainter { paintInfo, paintOffset, layerRenderer, *m_inlineContent, m_boxTree }.paint();
}

bool LineLayout::hitTest(const HitTestRequest& request, HitTestResult& result, const HitTestLocation& locationInContainer, const LayoutPoint& accumulatedOffset, HitTestAction hitTestAction, const RenderInline* layerRenderer)
{
    if (hitTestAction != HitTestForeground)
        return false;

    if (!m_inlineContent)
        return false;

    auto hitTestBoundingBox = locationInContainer.boundingBox();
    hitTestBoundingBox.moveBy(-accumulatedOffset);
    auto boxRange = m_inlineContent->boxesForRect(hitTestBoundingBox);

    LayerPaintScope layerPaintScope(m_boxTree, layerRenderer);

    for (auto& box : makeReversedRange(boxRange)) {
        if (!box.isVisible())
            continue;

        auto& renderer = m_boxTree.rendererForLayoutBox(box.layoutBox());

        if (!layerPaintScope.includes(box))
            continue;

        if (box.isAtomicInlineLevelBox()) {
            if (renderer.hitTest(request, result, locationInContainer, flippedContentOffsetIfNeeded(flow(), downcast<RenderBox>(renderer), accumulatedOffset)))
                return true;
            continue;
        }

        auto& currentLine = m_inlineContent->displayContent().lines[box.lineIndex()];
        auto boxRect = flippedRectForWritingMode(flow(), InlineDisplay::Box::visibleRectIgnoringBlockDirection(box, currentLine.visibleRectIgnoringBlockDirection()));
        boxRect.moveBy(accumulatedOffset);

        if (!locationInContainer.intersects(boxRect))
            continue;

        auto& elementRenderer = is<RenderElement>(renderer) ? downcast<RenderElement>(renderer) : *renderer.parent();
        if (!elementRenderer.visibleToHitTesting(request))
            continue;
        
        renderer.updateHitTestResult(result, flow().flipForWritingMode(locationInContainer.point() - toLayoutSize(accumulatedOffset)));
        if (result.addNodeToListBasedTestResult(renderer.nodeForHitTest(), request, locationInContainer, boxRect) == HitTestProgress::Stop)
            return true;
    }

    return false;
}

void LineLayout::insertedIntoTree(const RenderElement& parent, RenderObject& child)
{
    if (!m_inlineContent) {
        // This should only be called on partial layout.
        ASSERT_NOT_REACHED();
        return;
    }

    auto& childLayoutBox = m_boxTree.insert(parent, child, child.previousSibling());
    if (is<Layout::InlineTextBox>(childLayoutBox)) {
        auto invalidation = Layout::InlineInvalidation { ensureLineDamage(), m_inlineContentCache.inlineItems().content(), m_inlineContent->displayContent() };
        invalidation.textInserted(downcast<Layout::InlineTextBox>(childLayoutBox));
        return;
    }

    if (childLayoutBox.isLineBreakBox() || childLayoutBox.isReplacedBox() || childLayoutBox.isInlineBox()) {
        auto invalidation = Layout::InlineInvalidation { ensureLineDamage(), m_inlineContentCache.inlineItems().content(), m_inlineContent->displayContent() };
        invalidation.inlineLevelBoxInserted(childLayoutBox);
        return;
    }

    ASSERT_NOT_IMPLEMENTED_YET();
}

void LineLayout::removedFromTree(const RenderElement& parent, RenderObject& child)
{
    if (!m_inlineContent) {
        // This should only be called on partial layout.
        ASSERT_NOT_REACHED();
        return;
    }

    auto& childLayoutBox = m_boxTree.layoutBoxForRenderer(child);
    auto invalidation = Layout::InlineInvalidation { ensureLineDamage(), m_inlineContentCache.inlineItems().content(), m_inlineContent->displayContent() };
    auto boxIsInvalidated = is<Layout::InlineTextBox>(childLayoutBox) ? invalidation.textWillBeRemoved(downcast<Layout::InlineTextBox>(childLayoutBox)) : childLayoutBox.isLineBreakBox() ? invalidation.inlineLevelBoxWillBeRemoved(childLayoutBox) : false;
    if (boxIsInvalidated)
        m_lineDamage->addDetachedBox(m_boxTree.remove(parent, child));
}

void LineLayout::updateTextContent(const RenderText& textRenderer, size_t offset, int delta)
{
    if (!m_inlineContent) {
        // This should only be called on partial layout.
        ASSERT_NOT_REACHED();
        return;
    }

    m_boxTree.updateContent(textRenderer);
    auto invalidation = Layout::InlineInvalidation { ensureLineDamage(), m_inlineContentCache.inlineItems().content(), m_inlineContent->displayContent() };
    auto& inlineTextBox = downcast<Layout::InlineTextBox>(m_boxTree.layoutBoxForRenderer(textRenderer));
    if (delta >= 0) {
        invalidation.textInserted(inlineTextBox, offset);
        return;
    }
    invalidation.textWillBeRemoved(inlineTextBox, offset);
}

void LineLayout::releaseCaches(RenderView& view)
{
    if (!isEnabled())
        return;

    for (auto& renderer : descendantsOfType<RenderBlockFlow>(view)) {
        if (auto* lineLayout = renderer.modernLineLayout())
            lineLayout->releaseCaches();
    }
}

void LineLayout::releaseCaches()
{
    m_inlineContentCache.inlineItems().content().clear();
    if (m_inlineContent)
        m_inlineContent->releaseCaches();
}

void LineLayout::clearInlineContent()
{
    if (!m_inlineContent)
        return;
    m_inlineContent = nullptr;
}

Layout::InlineDamage& LineLayout::ensureLineDamage()
{
    if (!m_lineDamage)
        m_lineDamage = makeUnique<Layout::InlineDamage>();
    return *m_lineDamage;
}

bool LineLayout::contentNeedsVisualReordering() const
{
    return m_inlineContentCache.inlineItems().requiresVisualReordering();
}

#if ENABLE(TREE_DEBUGGING)
void LineLayout::outputLineTree(WTF::TextStream& stream, size_t depth) const
{
    showInlineContent(stream, *m_inlineContent, depth);
}
#endif

}
}

