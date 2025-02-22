/*
 * Copyright (C) 2018-2023 Apple Inc. All rights reserved.
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
#include "TextUtil.h"

#include "BreakLines.h"
#include "FontCascade.h"
#include "InlineLineTypes.h"
#include "InlineTextItem.h"
#include "Latin1TextIterator.h"
#include "LayoutInlineTextBox.h"
#include "RenderBox.h"
#include "RenderStyleInlines.h"
#include "SurrogatePairAwareTextIterator.h"
#include "TextRun.h"
#include "WidthIterator.h"
#include <unicode/ubidi.h>
#include <wtf/text/TextBreakIterator.h>

namespace WebCore {
namespace Layout {

static inline InlineLayoutUnit spaceWidth(const FontCascade& fontCascade, bool canUseSimplifiedContentMeasuring)
{
    if (canUseSimplifiedContentMeasuring)
        return fontCascade.primaryFont().spaceWidth();
    return fontCascade.widthOfSpaceString();
}

InlineLayoutUnit TextUtil::width(const InlineTextBox& inlineTextBox, const FontCascade& fontCascade, unsigned from, unsigned to, InlineLayoutUnit contentLogicalLeft, UseTrailingWhitespaceMeasuringOptimization useTrailingWhitespaceMeasuringOptimization)
{
    if (from == to)
        return 0;

    if (inlineTextBox.isCombined())
        return fontCascade.size();

    auto text = inlineTextBox.content();
    ASSERT(to <= text.length());
    auto hasKerningOrLigatures = fontCascade.enableKerning() || fontCascade.requiresShaping();
    // The "non-whitespace" + "whitespace" pattern is very common for inline content and since most of the "non-whitespace" runs end up with
    // their "whitespace" pair on the line (notable exception is when trailing whitespace is trimmed).
    // Including the trailing whitespace here enables us to cut the number of text measures when placing content on the line.
    auto extendedMeasuring = useTrailingWhitespaceMeasuringOptimization == UseTrailingWhitespaceMeasuringOptimization::Yes && hasKerningOrLigatures && to < text.length() && text[to] == space;
    if (extendedMeasuring)
        ++to;
    auto width = 0.f;
    auto useSimplifiedContentMeasuring = inlineTextBox.canUseSimplifiedContentMeasuring();
    if (useSimplifiedContentMeasuring)
        width = fontCascade.widthForSimpleText(StringView(text).substring(from, to - from));
    else {
        auto& style = inlineTextBox.style();
        auto directionalOverride = style.unicodeBidi() == UnicodeBidi::Override;
        auto run = WebCore::TextRun { StringView(text).substring(from, to - from), contentLogicalLeft, { }, ExpansionBehavior::defaultBehavior(), directionalOverride ? style.direction() : TextDirection::LTR, directionalOverride };
        if (!style.collapseWhiteSpace() && style.tabSize())
            run.setTabSize(true, style.tabSize());
        width = fontCascade.width(run);
    }

    if (extendedMeasuring)
        width -= (spaceWidth(fontCascade, useSimplifiedContentMeasuring) + fontCascade.wordSpacing());

    return std::isnan(width) ? 0.0f : std::isinf(width) ? maxInlineLayoutUnit() : width;
}

InlineLayoutUnit TextUtil::width(const InlineTextItem& inlineTextItem, const FontCascade& fontCascade, InlineLayoutUnit contentLogicalLeft)
{
    return TextUtil::width(inlineTextItem, fontCascade, inlineTextItem.start(), inlineTextItem.end(), contentLogicalLeft);
}

InlineLayoutUnit TextUtil::width(const InlineTextItem& inlineTextItem, const FontCascade& fontCascade, unsigned from, unsigned to, InlineLayoutUnit contentLogicalLeft, UseTrailingWhitespaceMeasuringOptimization useTrailingWhitespaceMeasuringOptimization)
{
    RELEASE_ASSERT(from >= inlineTextItem.start());
    RELEASE_ASSERT(to <= inlineTextItem.end());

    if (inlineTextItem.isWhitespace()) {
        auto& inlineTextBox = inlineTextItem.inlineTextBox();
        auto useSimplifiedContentMeasuring = inlineTextBox.canUseSimplifiedContentMeasuring();
        auto length = from - to;
        auto singleWhiteSpace = length == 1 || !TextUtil::shouldPreserveSpacesAndTabs(inlineTextBox);

        if (singleWhiteSpace) {
            auto width = spaceWidth(fontCascade, useSimplifiedContentMeasuring);
            return std::isnan(width) ? 0.0f : std::isinf(width) ? maxInlineLayoutUnit() : width;
        }
    }
    return width(inlineTextItem.inlineTextBox(), fontCascade, from, to, contentLogicalLeft, useTrailingWhitespaceMeasuringOptimization);
}

InlineLayoutUnit TextUtil::trailingWhitespaceWidth(const InlineTextBox& inlineTextBox, const FontCascade& fontCascade, size_t startPosition, size_t endPosition)
{
    auto text = inlineTextBox.content();
    ASSERT(endPosition > startPosition + 1);
    ASSERT(text[endPosition - 1] == space);
    return width(inlineTextBox, fontCascade, startPosition, endPosition, { }, UseTrailingWhitespaceMeasuringOptimization::Yes) - 
        width(inlineTextBox, fontCascade, startPosition, endPosition - 1, { }, UseTrailingWhitespaceMeasuringOptimization::No);
}

template <typename TextIterator>
static void fallbackFontsForRunWithIterator(WeakHashSet<const Font>& fallbackFonts, const FontCascade& fontCascade, const TextRun& run, TextIterator& textIterator)
{
    auto isRTL = run.rtl();
    auto isSmallCaps = fontCascade.isSmallCaps();
    auto& primaryFont = fontCascade.primaryFont();

    UChar32 currentCharacter = 0;
    unsigned clusterLength = 0;
    while (textIterator.consume(currentCharacter, clusterLength)) {

        auto addFallbackFontForCharacterIfApplicable = [&](auto character) {
            if (isSmallCaps && character != u_toupper(character))
                character = u_toupper(character);

            auto glyphData = fontCascade.glyphDataForCharacter(character, isRTL);
            if (glyphData.glyph && glyphData.font && glyphData.font != &primaryFont) {
                auto isNonSpacingMark = U_MASK(u_charType(character)) & U_GC_MN_MASK;

                // https://drafts.csswg.org/css-text-3/#white-space-processing
                // "Unsupported Default_ignorable characters must be ignored for text rendering."
                auto isIgnored = isDefaultIgnorableCodePoint(character);

                // If we include the synthetic bold expansion, then even zero-width glyphs will have their fonts added.
                if (isNonSpacingMark || glyphData.font->widthForGlyph(glyphData.glyph, Font::SyntheticBoldInclusion::Exclude))
                    if (!isIgnored)
                        fallbackFonts.add(*glyphData.font);
            }
        };
        addFallbackFontForCharacterIfApplicable(currentCharacter);
        textIterator.advance(clusterLength);
    }
}

TextUtil::FallbackFontList TextUtil::fallbackFontsForText(StringView textContent, const RenderStyle& style, IncludeHyphen includeHyphen)
{
    TextUtil::FallbackFontList fallbackFonts;

    auto collectFallbackFonts = [&](const auto& textRun) {
        if (textRun.text().isEmpty())
            return;

        if (textRun.is8Bit()) {
            auto textIterator = Latin1TextIterator { textRun.data8(0), 0, textRun.length(), textRun.length() };
            fallbackFontsForRunWithIterator(fallbackFonts, style.fontCascade(), textRun, textIterator);
            return;
        }
        auto textIterator = SurrogatePairAwareTextIterator { textRun.data16(0), 0, textRun.length(), textRun.length() };
        fallbackFontsForRunWithIterator(fallbackFonts, style.fontCascade(), textRun, textIterator);
    };

    if (includeHyphen == IncludeHyphen::Yes)
        collectFallbackFonts(TextRun { StringView(style.hyphenString().string()), { }, { }, ExpansionBehavior::defaultBehavior(), style.direction() });
    collectFallbackFonts(TextRun { textContent, { }, { }, ExpansionBehavior::defaultBehavior(), style.direction() });
    return fallbackFonts;
}

template <typename TextIterator>
static TextUtil::EnclosingAscentDescent enclosingGlyphBoundsForRunWithIterator(const FontCascade& fontCascade, bool isRTL, TextIterator& textIterator)
{
    auto enclosingAscent = std::optional<InlineLayoutUnit> { };
    auto enclosingDescent = std::optional<InlineLayoutUnit> { };
    auto isSmallCaps = fontCascade.isSmallCaps();
    auto& primaryFont = fontCascade.primaryFont();

    UChar32 currentCharacter = 0;
    unsigned clusterLength = 0;
    while (textIterator.consume(currentCharacter, clusterLength)) {

        auto computeTopAndBottomForCharacter = [&](auto character) {
            if (isSmallCaps && character != u_toupper(character))
                character = u_toupper(character);

            auto glyphData = fontCascade.glyphDataForCharacter(character, isRTL);
            auto& font = glyphData.font ? *glyphData.font : primaryFont;
            // FIXME: This may need some adjustment for ComplexTextController. See glyphOrigin.
            auto bounds = font.boundsForGlyph(glyphData.glyph);

            enclosingAscent = std::min(enclosingAscent.value_or(bounds.y()), bounds.y());
            enclosingDescent = std::max(enclosingDescent.value_or(bounds.maxY()), bounds.maxY());
        };
        computeTopAndBottomForCharacter(currentCharacter);
        textIterator.advance(clusterLength);
    }
    return { enclosingAscent.value_or(0.f), enclosingDescent.value_or(0.f) };
}

TextUtil::EnclosingAscentDescent TextUtil::enclosingGlyphBoundsForText(StringView textContent, const RenderStyle& style)
{
    if (textContent.isEmpty())
        return { };

    if (textContent.is8Bit()) {
        auto textIterator = Latin1TextIterator { textContent.characters8(), 0, textContent.length(), textContent.length() };
        return enclosingGlyphBoundsForRunWithIterator(style.fontCascade(), !style.isLeftToRightDirection(), textIterator);
    }
    auto textIterator = SurrogatePairAwareTextIterator { textContent.characters16(), 0, textContent.length(), textContent.length() };
    return enclosingGlyphBoundsForRunWithIterator(style.fontCascade(), !style.isLeftToRightDirection(), textIterator);
}

TextUtil::WordBreakLeft TextUtil::breakWord(const InlineTextItem& inlineTextItem, const FontCascade& fontCascade, InlineLayoutUnit textWidth, InlineLayoutUnit availableWidth, InlineLayoutUnit contentLogicalLeft)
{
    return breakWord(inlineTextItem.inlineTextBox(), inlineTextItem.start(), inlineTextItem.length(), textWidth, availableWidth, contentLogicalLeft, fontCascade);
}

TextUtil::WordBreakLeft TextUtil::breakWord(const InlineTextBox& inlineTextBox, size_t startPosition, size_t length, InlineLayoutUnit textWidth, InlineLayoutUnit availableWidth, InlineLayoutUnit contentLogicalLeft, const FontCascade& fontCascade)
{
    ASSERT(availableWidth >= 0);
    ASSERT(length);
    auto text = inlineTextBox.content();

    if (inlineTextBox.canUseSimpleFontCodePath()) {

        auto findBreakingPositionInSimpleText = [&] {
            auto userPerceivedCharacterBoundaryAlignedIndex = [&] (auto index) -> size_t {
                if (text.is8Bit())
                    return index;
                auto alignedStartIndex = index;
                U16_SET_CP_START(text, startPosition, alignedStartIndex);
                ASSERT(alignedStartIndex >= startPosition);
                return alignedStartIndex;
            };

            auto nextUserPerceivedCharacterIndex = [&] (auto index) -> size_t {
                if (text.is8Bit())
                    return index + 1;
                U16_FWD_1(text, index, startPosition + length);
                return index;
            };

            auto trySimplifiedBreakingPosition = [&] (auto start) -> std::optional<WordBreakLeft> {
                auto mayUseSimplifiedBreakingPositionForFixedPitch = fontCascade.isFixedPitch() && inlineTextBox.canUseSimplifiedContentMeasuring();
                if (!mayUseSimplifiedBreakingPositionForFixedPitch)
                    return { };
                // FIXME: Check if we could bring webkit.org/b/221581 back for system monospace fonts.
                auto monospaceCharacterWidth = fontCascade.widthOfSpaceString();
                size_t estimatedCharacterCount = floorf(availableWidth / monospaceCharacterWidth);
                auto end = userPerceivedCharacterBoundaryAlignedIndex(std::min(start + estimatedCharacterCount, start + length - 1));
                auto underflowWidth = TextUtil::width(inlineTextBox, fontCascade, start, end, contentLogicalLeft);
                if (underflowWidth > availableWidth || underflowWidth + monospaceCharacterWidth < availableWidth) {
                    // This does not look like a real fixed pitch font. Let's just fall back to regular bisect.
                    // In some edge cases (float precision) using monospaceCharacterWidth here may produce an incorrect off-by-one visual overflow.
                    return { };
                }
                return { WordBreakLeft { end - start, underflowWidth } };
            };
            if (auto leftSide = trySimplifiedBreakingPosition(startPosition))
                return *leftSide;

            auto left = startPosition;
            auto right = left + length - 1;
            // Pathological case of (extremely)long string and narrow lines.
            // Adjust the range so that we can pick a reasonable midpoint.
            auto averageCharacterWidth = InlineLayoutUnit { textWidth / length };
            // Overshot the midpoint so that biscection starts at the left side of the content.
            size_t startOffset = 2 * availableWidth / averageCharacterWidth;
            right = userPerceivedCharacterBoundaryAlignedIndex(std::min(left + startOffset, right));
            // Preserve the left width for the final split position so that we don't need to remeasure the left side again.
            auto leftSideWidth = InlineLayoutUnit { 0 };
            while (left < right) {
                auto middle = userPerceivedCharacterBoundaryAlignedIndex((left + right) / 2);
                ASSERT(middle >= left && middle < right);
                auto endOfMiddleCharacter = nextUserPerceivedCharacterIndex(middle);
                auto width = TextUtil::width(inlineTextBox, fontCascade, startPosition, endOfMiddleCharacter, contentLogicalLeft);
                if (width < availableWidth) {
                    left = endOfMiddleCharacter;
                    leftSideWidth = width;
                } else if (width > availableWidth)
                    right = middle;
                else {
                    right = endOfMiddleCharacter;
                    leftSideWidth = width;
                    break;
                }
            }
            RELEASE_ASSERT(right >= startPosition);
            return WordBreakLeft { right - startPosition, leftSideWidth };
        };
        return findBreakingPositionInSimpleText();
    }

    auto graphemeClusterIterator = NonSharedCharacterBreakIterator { StringView { text }.substring(startPosition, length) };
    auto leftSide = TextUtil::WordBreakLeft { };
    for (auto clusterStartPosition = ubrk_next(graphemeClusterIterator); clusterStartPosition != UBRK_DONE; clusterStartPosition = ubrk_next(graphemeClusterIterator)) {
        auto width = TextUtil::width(inlineTextBox, fontCascade, startPosition, startPosition + clusterStartPosition, contentLogicalLeft);
        if (width > availableWidth)
            return leftSide;
        leftSide = { static_cast<size_t>(clusterStartPosition), width };
    }
    // This content is not supposed to fit availableWidth.
    ASSERT_NOT_REACHED();
    return leftSide;
}

bool TextUtil::mayBreakInBetween(const InlineTextItem& previousInlineItem, const InlineTextItem& nextInlineItem)
{
    // Check if these 2 adjacent non-whitespace inline items are connected at a breakable position.
    ASSERT(!previousInlineItem.isWhitespace() && !nextInlineItem.isWhitespace());

    auto previousContent = previousInlineItem.inlineTextBox().content();
    auto nextContent = nextInlineItem.inlineTextBox().content();
    // Now we need to collect at least 3 adjacent characters to be able to make a decision whether the previous text item ends with breaking opportunity.
    // [ex-][ample] <- second to last[x] last[-] current[a]
    // We need at least 1 character in the current inline text item and 2 more from previous inline items.
    if (!previousContent.is8Bit()) {
        // FIXME: Remove this workaround when we move over to a better way of handling prior-context with Unicode.
        // See the templated CharacterType in nextBreakablePosition for last and lastlast characters.
        nextContent.convertTo16Bit();
    }
    auto& previousContentStyle = previousInlineItem.style();
    auto& nextContentStyle = nextInlineItem.style();
    auto lineBreakIteratorFactory = CachedLineBreakIteratorFactory { nextContent, nextContentStyle.computedLocale(), TextUtil::lineBreakIteratorMode(nextContentStyle.lineBreak()), TextUtil::contentAnalysis(nextContentStyle.wordBreak()) };
    auto previousContentLength = previousContent.length();
    // FIXME: We should look into the entire uncommitted content for more text context.
    UChar lastCharacter = previousContentLength ? previousContent[previousContentLength - 1] : 0;
    if (lastCharacter == softHyphen && previousContentStyle.hyphens() == Hyphens::None)
        return false;
    UChar secondToLastCharacter = previousContentLength > 1 ? previousContent[previousContentLength - 2] : 0;
    lineBreakIteratorFactory.priorContext().set({ secondToLastCharacter, lastCharacter });
    // Now check if we can break right at the inline item boundary.
    // With the [ex-ample], findNextBreakablePosition should return the startPosition (0).
    // FIXME: Check if there's a more correct way of finding breaking opportunities.
    return !findNextBreakablePosition(lineBreakIteratorFactory, 0, nextContentStyle);
}

unsigned TextUtil::findNextBreakablePosition(CachedLineBreakIteratorFactory& lineBreakIteratorFactory, unsigned startPosition, const RenderStyle& style)
{
    auto keepAllWordsForCJK = style.wordBreak() == WordBreak::KeepAll;
    auto breakNBSP = style.autoWrap() && style.nbspMode() == NBSPMode::Space;

    if (keepAllWordsForCJK) {
        if (breakNBSP)
            return nextBreakablePositionKeepingAllWords(lineBreakIteratorFactory, startPosition);
        return nextBreakablePositionKeepingAllWordsIgnoringNBSP(lineBreakIteratorFactory, startPosition);
    }

    if (lineBreakIteratorFactory.mode() == TextBreakIterator::LineMode::Behavior::Default) {
        if (breakNBSP)
            return WebCore::nextBreakablePosition(lineBreakIteratorFactory, startPosition);
        return nextBreakablePositionIgnoringNBSP(lineBreakIteratorFactory, startPosition);
    }

    if (breakNBSP)
        return nextBreakablePositionWithoutShortcut(lineBreakIteratorFactory, startPosition);
    return nextBreakablePositionIgnoringNBSPWithoutShortcut(lineBreakIteratorFactory, startPosition);
}

bool TextUtil::shouldPreserveSpacesAndTabs(const Box& layoutBox)
{
    // https://www.w3.org/TR/css-text-4/#white-space-collapsing
    auto whitespaceCollapse = layoutBox.style().whiteSpaceCollapse();
    return whitespaceCollapse == WhiteSpaceCollapse::Preserve || whitespaceCollapse == WhiteSpaceCollapse::BreakSpaces;
}

bool TextUtil::shouldPreserveNewline(const Box& layoutBox)
{
    // https://www.w3.org/TR/css-text-4/#white-space-collapsing
    auto whitespaceCollapse = layoutBox.style().whiteSpaceCollapse();
    return whitespaceCollapse == WhiteSpaceCollapse::Preserve || whitespaceCollapse == WhiteSpaceCollapse::PreserveBreaks || whitespaceCollapse == WhiteSpaceCollapse::BreakSpaces;
}

bool TextUtil::isWrappingAllowed(const RenderStyle& style)
{
    // https://www.w3.org/TR/css-text-4/#text-wrap
    return style.textWrap() != TextWrap::NoWrap;
}

bool TextUtil::shouldTrailingWhitespaceHang(const RenderStyle& style)
{
    // https://www.w3.org/TR/css-text-4/#white-space-phase-2
    return style.whiteSpaceCollapse() == WhiteSpaceCollapse::Preserve && style.textWrap() != TextWrap::NoWrap;
}

TextBreakIterator::LineMode::Behavior TextUtil::lineBreakIteratorMode(LineBreak lineBreak)
{
    switch (lineBreak) {
    case LineBreak::Auto:
    case LineBreak::AfterWhiteSpace:
    case LineBreak::Anywhere:
        return TextBreakIterator::LineMode::Behavior::Default;
    case LineBreak::Loose:
        return TextBreakIterator::LineMode::Behavior::Loose;
    case LineBreak::Normal:
        return TextBreakIterator::LineMode::Behavior::Normal;
    case LineBreak::Strict:
        return TextBreakIterator::LineMode::Behavior::Strict;
    }
    ASSERT_NOT_REACHED();
    return TextBreakIterator::LineMode::Behavior::Default;
}

TextBreakIterator::ContentAnalysis TextUtil::contentAnalysis(WordBreak wordBreak)
{
    switch (wordBreak) {
    case WordBreak::Normal:
    case WordBreak::BreakAll:
    case WordBreak::KeepAll:
    case WordBreak::BreakWord:
        return TextBreakIterator::ContentAnalysis::Mechanical;
    case WordBreak::Auto:
        return TextBreakIterator::ContentAnalysis::Linguistic;
    }
    return TextBreakIterator::ContentAnalysis::Mechanical;
}

bool TextUtil::containsStrongDirectionalityText(StringView text)
{
    if (text.is8Bit())
        return false;

    auto length = text.length();
    for (size_t position = 0; position < length;) {
        UChar32 character;
        U16_NEXT(text.characters16(), position, length, character);

        auto bidiCategory = u_charDirection(character);
        bool hasBidiContent = bidiCategory == U_RIGHT_TO_LEFT
            || bidiCategory == U_RIGHT_TO_LEFT_ARABIC
            || bidiCategory == U_RIGHT_TO_LEFT_EMBEDDING
            || bidiCategory == U_RIGHT_TO_LEFT_OVERRIDE
            || bidiCategory == U_LEFT_TO_RIGHT_EMBEDDING
            || bidiCategory == U_LEFT_TO_RIGHT_OVERRIDE
            || bidiCategory == U_POP_DIRECTIONAL_FORMAT;
        if (hasBidiContent)
            return true;
    }

    return false;
}

size_t TextUtil::firstUserPerceivedCharacterLength(const InlineTextBox& inlineTextBox, size_t startPosition, size_t length)
{
    auto textContent = inlineTextBox.content();
    RELEASE_ASSERT(!textContent.isEmpty());

    if (textContent.is8Bit())
        return 1;
    if (inlineTextBox.canUseSimpleFontCodePath()) {
        UChar32 character;
        size_t endOfCodePoint = startPosition;
        U16_NEXT(textContent.characters16(), endOfCodePoint, textContent.length(), character);
        ASSERT(endOfCodePoint > startPosition);
        return endOfCodePoint - startPosition;
    }
    auto graphemeClustersIterator = NonSharedCharacterBreakIterator { textContent };
    auto nextPosition = ubrk_following(graphemeClustersIterator, startPosition);
    if (nextPosition == UBRK_DONE)
        return length;
    return nextPosition - startPosition;
}

size_t TextUtil::firstUserPerceivedCharacterLength(const InlineTextItem& inlineTextItem)
{
    auto length = firstUserPerceivedCharacterLength(inlineTextItem.inlineTextBox(), inlineTextItem.start(), inlineTextItem.length());
    return std::min<size_t>(inlineTextItem.length(), length);
}

TextDirection TextUtil::directionForTextContent(StringView content)
{
    if (content.is8Bit())
        return TextDirection::LTR;
    return ubidi_getBaseDirection(content.characters16(), content.length()) == UBIDI_RTL ? TextDirection::RTL : TextDirection::LTR;
}

TextRun TextUtil::ellipsisTextRun(bool isHorizontal)
{
    if (isHorizontal) {
        static MainThreadNeverDestroyed<const AtomString> horizontalEllipsisStr(&horizontalEllipsis, 1);
        return TextRun { horizontalEllipsisStr->string() };
    }
    static MainThreadNeverDestroyed<const AtomString> verticalEllipsisStr(&verticalEllipsis, 1);
    return TextRun { verticalEllipsisStr->string() };
}

bool TextUtil::hasHangablePunctuationStart(const InlineTextItem& inlineTextItem, const RenderStyle& style)
{
    if (!inlineTextItem.length() || !style.hangingPunctuation().contains(HangingPunctuation::First))
        return false;
    auto leadingCharacter = inlineTextItem.inlineTextBox().content()[inlineTextItem.start()];
    return U_GET_GC_MASK(leadingCharacter) & (U_GC_PS_MASK | U_GC_PI_MASK | U_GC_PF_MASK);
}

float TextUtil::hangablePunctuationStartWidth(const InlineTextItem& inlineTextItem, const RenderStyle& style)
{
    if (!hasHangablePunctuationStart(inlineTextItem, style))
        return { };
    ASSERT(inlineTextItem.length());
    auto leadingPosition = inlineTextItem.start();
    return width(inlineTextItem, style.fontCascade(), leadingPosition, leadingPosition + 1, { });
}

bool TextUtil::hasHangablePunctuationEnd(const InlineTextItem& inlineTextItem, const RenderStyle& style)
{
    if (!inlineTextItem.length() || !style.hangingPunctuation().contains(HangingPunctuation::Last))
        return false;
    auto trailingCharacter = inlineTextItem.inlineTextBox().content()[inlineTextItem.end() - 1];
    return U_GET_GC_MASK(trailingCharacter) & (U_GC_PE_MASK | U_GC_PI_MASK | U_GC_PF_MASK);
}

float TextUtil::hangablePunctuationEndWidth(const InlineTextItem& inlineTextItem, const RenderStyle& style)
{
    if (!hasHangablePunctuationEnd(inlineTextItem, style))
        return { };
    ASSERT(inlineTextItem.length());
    auto trailingPosition = inlineTextItem.end() - 1;
    return width(inlineTextItem, style.fontCascade(), trailingPosition, trailingPosition + 1, { });
}

bool TextUtil::hasHangableStopOrCommaEnd(const InlineTextItem& inlineTextItem, const RenderStyle& style)
{
    if (!inlineTextItem.length() || !style.hangingPunctuation().containsAny({ HangingPunctuation::AllowEnd, HangingPunctuation::ForceEnd }))
        return false;
    auto trailingPosition = inlineTextItem.end() - 1;
    auto trailingCharacter = inlineTextItem.inlineTextBox().content()[trailingPosition];
    auto isHangableStopOrComma = trailingCharacter == 0x002C
        || trailingCharacter == 0x002E || trailingCharacter == 0x060C
        || trailingCharacter == 0x06D4 || trailingCharacter == 0x3001
        || trailingCharacter == 0x3002 || trailingCharacter == 0xFF0C
        || trailingCharacter == 0xFF0E || trailingCharacter == 0xFE50
        || trailingCharacter == 0xFE51 || trailingCharacter == 0xFE52
        || trailingCharacter == 0xFF61 || trailingCharacter == 0xFF64;
    return isHangableStopOrComma;
}

float TextUtil::hangableStopOrCommaEndWidth(const InlineTextItem& inlineTextItem, const RenderStyle& style)
{
    if (!hasHangableStopOrCommaEnd(inlineTextItem, style))
        return { };
    ASSERT(inlineTextItem.length());
    auto trailingPosition = inlineTextItem.end() - 1;
    return width(inlineTextItem, style.fontCascade(), trailingPosition, trailingPosition + 1, { });
}

template<typename CharacterType>
static bool canUseSimplifiedTextMeasuringForCharacters(const CharacterType* characters, unsigned length, const FontCascade& fontCascade, const Font& primaryFont, bool whitespaceIsCollapsed)
{
    for (unsigned i = 0; i < length; ++i) {
        auto character = characters[i];
        if (!WidthIterator::characterCanUseSimplifiedTextMeasuring(character, whitespaceIsCollapsed))
            return false;
        auto glyphData = fontCascade.glyphDataForCharacter(character, false);
        if (!glyphData.isValid() || glyphData.font != &primaryFont)
            return false;
    }
    return true;
}

bool TextUtil::canUseSimplifiedTextMeasuring(StringView textContent, const RenderStyle& style, const RenderStyle* firstLineStyle)
{
    ASSERT(textContent.is8Bit() || FontCascade::characterRangeCodePath(textContent.characters16(), textContent.length()) == FontCascade::CodePath::Simple);
    // FIXME: All these checks should be more fine-grained at the inline item level.
    auto& fontCascade = style.fontCascade();
    if (fontCascade.wordSpacing() || fontCascade.letterSpacing())
        return false;

    // Additional check on the font codepath.
    auto run = TextRun { textContent };
    run.setCharacterScanForCodePath(false);
    if (fontCascade.codePath(run) != FontCascade::CodePath::Simple)
        return false;

    if (firstLineStyle && fontCascade != firstLineStyle->fontCascade())
        return false;

    auto& primaryFont = fontCascade.primaryFont();
    if (primaryFont.syntheticBoldOffset())
        return false;

    auto whitespaceIsCollapsed = style.collapseWhiteSpace();
    if (textContent.is8Bit())
        return canUseSimplifiedTextMeasuringForCharacters(textContent.characters8(), textContent.length(), fontCascade, primaryFont, whitespaceIsCollapsed);
    return canUseSimplifiedTextMeasuringForCharacters(textContent.characters16(), textContent.length(), fontCascade, primaryFont, whitespaceIsCollapsed);
}

void TextUtil::computedExpansions(const Line::RunList& runs, WTF::Range<size_t> runRange, size_t hangingTrailingWhitespaceLength, ExpansionInfo& expansionInfo)
{
    // Collect and distribute the expansion opportunities.
    expansionInfo.opportunityCount = 0;
    auto rangeSize = runRange.end() - runRange.begin();
    if (rangeSize > runs.size()) {
        ASSERT_NOT_REACHED();
        return;
    }
    expansionInfo.opportunityList.resizeToFit(rangeSize);
    expansionInfo.behaviorList.resizeToFit(rangeSize);
    auto lastExpansionIndexWithContent = std::optional<size_t> { };

    // Line start behaves as if we had an expansion here (i.e. fist runs should not start with allowing left expansion).
    auto runIsAfterExpansion = true;
    auto lastTextRunIndexForTrimming = [&]() -> std::optional<size_t> {
        if (!hangingTrailingWhitespaceLength)
            return { };
        for (auto index = runs.size(); index--;) {
            if (runs[index].isText())
                return index;
        }
        return { };
    }();
    for (size_t index = 0; index < rangeSize; ++index) {
        auto runIndex = runRange.begin() + index;
        auto& run = runs[runIndex];
        auto expansionBehavior = ExpansionBehavior::defaultBehavior();
        size_t expansionOpportunitiesInRun = 0;

        // According to the CSS3 spec, a UA can determine whether or not
        // it wishes to apply text-align: justify to text with collapsible spaces (and this behavior matches Blink).
        auto mayAlterSpacingWithinText = !TextUtil::shouldPreserveSpacesAndTabs(run.layoutBox()) || hangingTrailingWhitespaceLength;
        if (run.isText() && mayAlterSpacingWithinText) {
            if (run.hasTextCombine())
                expansionBehavior = ExpansionBehavior::forbidAll();
            else {
                expansionBehavior.left = runIsAfterExpansion ? ExpansionBehavior::Behavior::Forbid : ExpansionBehavior::Behavior::Allow;
                expansionBehavior.right = ExpansionBehavior::Behavior::Allow;
                auto& textContent = *run.textContent();
                auto length = textContent.length;
                if (lastTextRunIndexForTrimming && runIndex == *lastTextRunIndexForTrimming) {
                    // Trailing hanging whitespace sequence is ignored when computing the expansion opportunities.
                    length -= hangingTrailingWhitespaceLength;
                }
                std::tie(expansionOpportunitiesInRun, runIsAfterExpansion) = FontCascade::expansionOpportunityCount(StringView(downcast<InlineTextBox>(run.layoutBox()).content()).substring(textContent.start, length), run.inlineDirection(), expansionBehavior);
            }
        } else if (run.isBox())
            runIsAfterExpansion = false;

        expansionInfo.behaviorList[index] = expansionBehavior;
        expansionInfo.opportunityList[index] = expansionOpportunitiesInRun;
        expansionInfo.opportunityCount += expansionOpportunitiesInRun;

        if (run.isText() || run.isBox())
            lastExpansionIndexWithContent = index;
    }
    // Forbid right expansion in the last run to prevent trailing expansion at the end of the line.
    if (lastExpansionIndexWithContent && expansionInfo.opportunityList[*lastExpansionIndexWithContent]) {
        expansionInfo.behaviorList[*lastExpansionIndexWithContent].right = ExpansionBehavior::Behavior::Forbid;
        if (runIsAfterExpansion) {
            // When the last run has an after expansion (e.g. CJK ideograph) we need to remove this trailing expansion opportunity.
            // Note that this is not about trailing collapsible whitespace as at this point we trimmed them all.
            ASSERT(expansionInfo.opportunityCount && expansionInfo.opportunityList[*lastExpansionIndexWithContent]);
            --expansionInfo.opportunityCount;
            --expansionInfo.opportunityList[*lastExpansionIndexWithContent];
        }
    }
}

}
}
