/*
 * Copyright (C) 2004, 2005 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) 2004, 2005, 2006 Rob Buis <buis@kde.org>
 * Copyright (C) 2018 Apple Inc. All rights reserved.
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
#include "SVGSymbolElement.h"

#include "LegacyRenderSVGHiddenContainer.h"
#include "RenderSVGHiddenContainer.h"
#include "SVGFitToViewBox.h"
#include "SVGNames.h"
#include <wtf/IsoMallocInlines.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(SVGSymbolElement);

inline SVGSymbolElement::SVGSymbolElement(const QualifiedName& tagName, Document& document)
    : SVGGraphicsElement(tagName, document, makeUniqueRef<PropertyRegistry>(*this))
    , SVGFitToViewBox(this)
{
    ASSERT(hasTagName(SVGNames::symbolTag));
}

Ref<SVGSymbolElement> SVGSymbolElement::create(const QualifiedName& tagName, Document& document)
{
    return adoptRef(*new SVGSymbolElement(tagName, document));
}

void SVGSymbolElement::attributeChanged(const QualifiedName& name, const AtomString& oldValue, const AtomString& newValue, AttributeModificationReason attributeModificationReason)
{
    SVGFitToViewBox::parseAttribute(name, newValue);
    SVGGraphicsElement::attributeChanged(name, oldValue, newValue, attributeModificationReason);
}

bool SVGSymbolElement::selfHasRelativeLengths() const
{
    return hasAttribute(SVGNames::viewBoxAttr);
}

RenderPtr<RenderElement> SVGSymbolElement::createElementRenderer(RenderStyle&& style, const RenderTreePosition&)
{
#if ENABLE(LAYER_BASED_SVG_ENGINE)
    if (document().settings().layerBasedSVGEngineEnabled())
        return createRenderer<RenderSVGHiddenContainer>(*this, WTFMove(style));
#endif
    return createRenderer<LegacyRenderSVGHiddenContainer>(RenderObject::Type::LegacySVGHiddenContainer, *this, WTFMove(style));
}

}
