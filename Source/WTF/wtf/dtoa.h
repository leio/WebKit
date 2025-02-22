/*
 *  Copyright (C) 2003-2019 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <array>
#include <wtf/ASCIICType.h>
#include <wtf/FastFloat.h>
#include <wtf/dtoa/double-conversion.h>
#include <wtf/text/StringView.h>

namespace WTF {

// Only toFixed() can use all the 124 positions. The format is:
// <-> + <21 digits> + decimal point + <100 digits> + null char = 124.
using NumberToStringBuffer = std::array<char, 124>;


// <-> + <320 digits> + decimal point + <6 digits> + null char = 329
using NumberToCSSStringBuffer = std::array<char, 329>;

WTF_EXPORT_PRIVATE const char* numberToString(float, NumberToStringBuffer&);
WTF_EXPORT_PRIVATE const char* numberToFixedPrecisionString(float, unsigned significantFigures, NumberToStringBuffer&, bool truncateTrailingZeros = false);
WTF_EXPORT_PRIVATE const char* numberToFixedWidthString(float, unsigned decimalPlaces, NumberToStringBuffer&);

WTF_EXPORT_PRIVATE const char* numberToString(double, NumberToStringBuffer&);
WTF_EXPORT_PRIVATE const char* numberToStringWithTrailingPoint(double, NumberToStringBuffer&);
WTF_EXPORT_PRIVATE const char* numberToFixedPrecisionString(double, unsigned significantFigures, NumberToStringBuffer&, bool truncateTrailingZeros = false);
WTF_EXPORT_PRIVATE const char* numberToFixedWidthString(double, unsigned decimalPlaces, NumberToStringBuffer&);

// Fixed width with up to 6 decimal places, trailing zeros truncated.
WTF_EXPORT_PRIVATE const char* numberToCSSString(double, NumberToCSSStringBuffer&);

inline double parseDouble(StringView string, size_t& parsedLength)
{
    if (string.is8Bit())
        return parseDouble(string.characters8(), string.length(), parsedLength);
    return parseDouble(string.characters16(), string.length(), parsedLength);
}

} // namespace WTF

using WTF::NumberToStringBuffer;
using WTF::numberToString;
using WTF::numberToFixedPrecisionString;
using WTF::numberToFixedWidthString;
using WTF::parseDouble;
