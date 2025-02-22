/*
 * Copyright (C) 2018 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "GridBaselineAlignment.h"

#include "BaselineAlignmentInlines.h"
#include "RenderBoxInlines.h"
#include "RenderGrid.h"
#include "RenderStyleConstants.h"

namespace WebCore {

// This function gives the margin 'over' based on the baseline-axis, since in grid we can have 2-dimensional
// alignment by baseline. In horizontal writing-mode, the row-axis is the horizontal axis. When we use this
// axis to move the grid items so that they are baseline-aligned, we want their "horizontal" margin (right);
// the same will happen when using the column-axis under vertical writing mode, we also want in this case the
// 'right' margin.
LayoutUnit GridBaselineAlignment::marginOverForChild(const RenderBox& child, GridAxis axis) const
{
    return isHorizontalBaselineAxis(axis) ? child.marginRight() : child.marginTop();
}

// This function gives the margin 'under' based on the baseline-axis, since in grid we can can 2-dimensional
// alignment by baseline. In horizontal writing-mode, the row-axis is the horizontal axis. When we use this
// axis to move the grid items so that they are baseline-aligned, we want their "horizontal" margin (left);
// the same will happen when using the column-axis under vertical writing mode, we also want in this case the
// 'left' margin.
LayoutUnit GridBaselineAlignment::marginUnderForChild(const RenderBox& child, GridAxis axis) const
{
    return isHorizontalBaselineAxis(axis) ? child.marginLeft() : child.marginBottom();
}

LayoutUnit GridBaselineAlignment::logicalAscentForChild(const RenderBox& child, GridAxis baselineAxis, ItemPosition position) const
{
    LayoutUnit ascent = ascentForChild(child, baselineAxis, position);
    return (isDescentBaselineForChild(child, baselineAxis) || position == ItemPosition::LastBaseline) ? descentForChild(child, ascent, baselineAxis) : ascent;
}

LayoutUnit GridBaselineAlignment::ascentForChild(const RenderBox& child, GridAxis baselineAxis, ItemPosition position) const
{
    ASSERT(position == ItemPosition::Baseline || position == ItemPosition::LastBaseline);
    LayoutUnit margin = isDescentBaselineForChild(child, baselineAxis) ? marginUnderForChild(child, baselineAxis) : marginOverForChild(child, baselineAxis);
    std::optional<LayoutUnit> computedBaselineValue = position == ItemPosition::Baseline ? child.firstLineBaseline() : child.lastLineBaseline();
    LayoutUnit baseline(isParallelToBaselineAxisForChild(child, baselineAxis) ? computedBaselineValue.value_or(LayoutUnit(-1)) : LayoutUnit(-1));
    // We take border-box's under edge if no valid baseline.
    if (baseline == -1) {
        ASSERT(!child.needsLayout());
        if (isHorizontalBaselineAxis(baselineAxis))
            return isFlippedWritingMode(m_blockFlow) ? child.size().width().toInt() + margin : margin;
        return child.size().height() + margin;
    }

    auto subgridOffset = [&] {
        if (position == ItemPosition::LastBaseline || baselineAxis == GridAxis::GridRowAxis)
            return 0_lu;

        auto offset = 0_lu;
        const auto* currentSubgrid = dynamicDowncast<RenderGrid>(child.parent());
        while (currentSubgrid && currentSubgrid->isSubgridRows()) {
            auto isPlacedAtFirstTrackInSubgrid = [&] {
                return !currentSubgrid->gridSpanForChild(child, GridTrackSizingDirection::ForRows).startLine();
            }();

            if (!isPlacedAtFirstTrackInSubgrid || GridLayoutFunctions::isOrthogonalParent(*currentSubgrid, *currentSubgrid->parent()))
                return 0_lu;
            offset += currentSubgrid->marginAndBorderAndPaddingBefore();
            currentSubgrid = dynamicDowncast<RenderGrid>(currentSubgrid->parent());
        }
        return offset;
    }();

    return subgridOffset + margin + baseline;
}

LayoutUnit GridBaselineAlignment::descentForChild(const RenderBox& child, LayoutUnit ascent, GridAxis baselineAxis) const
{
    ASSERT(!child.needsLayout());
    if (isParallelToBaselineAxisForChild(child, baselineAxis))
        return child.marginLogicalHeight() + child.logicalHeight() - ascent;
    return child.marginLogicalWidth() + child.logicalWidth() - ascent;
}

bool GridBaselineAlignment::isDescentBaselineForChild(const RenderBox& child, GridAxis baselineAxis) const
{
    return isHorizontalBaselineAxis(baselineAxis)
        && ((child.style().isFlippedBlocksWritingMode() && !isFlippedWritingMode(m_blockFlow))
            || (child.style().isFlippedLinesWritingMode() && isFlippedWritingMode(m_blockFlow)));
}

bool GridBaselineAlignment::isHorizontalBaselineAxis(GridAxis axis) const
{
    return axis == GridAxis::GridRowAxis ? isHorizontalWritingMode(m_blockFlow) : !isHorizontalWritingMode(m_blockFlow);
}

bool GridBaselineAlignment::isOrthogonalChildForBaseline(const RenderBox& child) const
{
    return isHorizontalWritingMode(m_blockFlow) != child.isHorizontalWritingMode();
}

bool GridBaselineAlignment::isParallelToBaselineAxisForChild(const RenderBox& child, GridAxis axis) const
{
    return axis == GridAxis::GridColumnAxis ? !isOrthogonalChildForBaseline(child) : isOrthogonalChildForBaseline(child);
}

const BaselineGroup& GridBaselineAlignment::baselineGroupForChild(ItemPosition preference, unsigned sharedContext, const RenderBox& child, GridAxis baselineAxis) const
{
    ASSERT(isBaselinePosition(preference));
    bool isRowAxisContext = baselineAxis == GridAxis::GridColumnAxis;
    auto& baselineAlignmentStateMap = isRowAxisContext ? m_rowAxisBaselineAlignmentStates : m_colAxisBaselineAlignmentStates;
    auto* baselineAlignmentState = baselineAlignmentStateMap.get(sharedContext);
    ASSERT(baselineAlignmentState);
    return baselineAlignmentState->sharedGroup(child, preference);
}

void GridBaselineAlignment::updateBaselineAlignmentContext(ItemPosition preference, unsigned sharedContext, const RenderBox& child, GridAxis baselineAxis)
{
    ASSERT(isBaselinePosition(preference));
    ASSERT(!child.needsLayout());

    // Determine Ascent and Descent values of this child with respect to
    // its grid container.
    LayoutUnit ascent = logicalAscentForChild(child, baselineAxis, preference);
    // Looking up for a shared alignment context perpendicular to the
    // baseline axis.
    bool isRowAxisContext = baselineAxis == GridAxis::GridColumnAxis;
    auto& baselineAlignmentStateMap = isRowAxisContext ? m_rowAxisBaselineAlignmentStates : m_colAxisBaselineAlignmentStates;
    // Looking for a compatible baseline-sharing group.
    if (auto* baselineAlignmentStateSearch = baselineAlignmentStateMap.get(sharedContext))
        baselineAlignmentStateSearch->updateSharedGroup(child, preference, ascent);
    else
        baselineAlignmentStateMap.add(sharedContext, makeUnique<BaselineAlignmentState>(child, preference, ascent));
}

LayoutUnit GridBaselineAlignment::baselineOffsetForChild(ItemPosition preference, unsigned sharedContext, const RenderBox& child, GridAxis baselineAxis) const
{
    ASSERT(isBaselinePosition(preference));
    auto& group = baselineGroupForChild(preference, sharedContext, child, baselineAxis);
    if (group.computeSize() > 1)
        return group.maxAscent() - logicalAscentForChild(child, baselineAxis, preference);
    return LayoutUnit();
}

void GridBaselineAlignment::clear(GridAxis baselineAxis)
{
    if (baselineAxis == GridAxis::GridColumnAxis)
        m_rowAxisBaselineAlignmentStates.clear();
    else
        m_colAxisBaselineAlignmentStates.clear();
}

} // namespace WebCore
