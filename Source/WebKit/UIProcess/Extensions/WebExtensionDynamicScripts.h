/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)

#include "APIContentWorld.h"
#include "WebExtensionFrameIdentifier.h"
#include "WebExtensionScriptInjectionResultParameters.h"
#include <wtf/Forward.h>

OBJC_CLASS WKWebView;
OBJC_CLASS WKFrameInfo;
OBJC_CLASS _WKFrameTreeNode;

namespace WebKit {

class WebExtension;
class WebExtensionContext;

namespace WebExtensionDynamicScripts {

using InjectionResults = Vector<WebExtensionScriptInjectionResultParameters>;
using Error = std::optional<String>;

using SourcePair = std::pair<String, std::optional<URL>>;
using SourcePairs = Vector<std::optional<SourcePair>>;

class InjectionResultHolder : public RefCounted<InjectionResultHolder> {
    WTF_MAKE_NONCOPYABLE(InjectionResultHolder);
    WTF_MAKE_FAST_ALLOCATED;

public:
    template<typename... Args>
    static Ref<InjectionResultHolder> create(Args&&... args)
    {
        return adoptRef(*new InjectionResultHolder(std::forward<Args>(args)...));
    }

    InjectionResultHolder() { };

    InjectionResults results;
};

std::optional<SourcePair> sourcePairForResource(String path, RefPtr<WebExtension>);
SourcePairs getSourcePairsForResource(std::optional<Vector<String>> files, std::optional<String> code, RefPtr<WebExtension>);
Vector<RetainPtr<_WKFrameTreeNode>> getFrames(_WKFrameTreeNode *, std::optional<Vector<WebExtensionFrameIdentifier>>);

void injectStyleSheets(SourcePairs, WKWebView *, API::ContentWorld&, WebCore::UserContentInjectedFrames, WebExtensionContext&);
void removeStyleSheets(SourcePairs, WebCore::UserContentInjectedFrames, WebExtensionContext&);

WebExtensionScriptInjectionResultParameters toInjectionResultParameters(id resultOfExecution, WKFrameInfo *, NSString *errorMessage);

} // namespace WebExtensionDynamicScripts

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
