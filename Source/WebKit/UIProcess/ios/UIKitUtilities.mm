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

#import "config.h"
#import "UIKitUtilities.h"

#if PLATFORM(IOS_FAMILY)

#import "UIKitSPI.h"

@implementation UIScrollView (WebKitInternal)

- (BOOL)_wk_isInterruptingDeceleration
{
    return self.decelerating && self.tracking;
}

- (CGFloat)_wk_contentWidthIncludingInsets
{
    auto inset = self.adjustedContentInset;
    return self.contentSize.width + inset.left + inset.right;
}

- (CGFloat)_wk_contentHeightIncludingInsets
{
    auto inset = self.adjustedContentInset;
    return self.contentSize.height + inset.top + inset.bottom;
}

- (BOOL)_wk_isScrolledBeyondExtents
{
    auto contentOffset = self.contentOffset;
    auto inset = self.adjustedContentInset;
    if (contentOffset.x < -inset.left || contentOffset.y < -inset.top)
        return YES;

    auto contentSize = self.contentSize;
    auto boundsSize = self.bounds.size;
    auto maxScrollExtent = CGPointMake(
        contentSize.width + inset.right - std::min<CGFloat>(boundsSize.width, self._wk_contentWidthIncludingInsets),
        contentSize.height + inset.bottom - std::min<CGFloat>(boundsSize.height, self._wk_contentHeightIncludingInsets)
    );
    return contentOffset.x > maxScrollExtent.x || contentOffset.y > maxScrollExtent.y;
}

- (CGPoint)_wk_clampToScrollExtents:(CGPoint)contentOffset
{
    // See also: -[UIScrollView _adjustedContentOffsetForContentOffset:].
    auto bounds = CGRect { contentOffset, self.bounds.size };
    auto effectiveInset = self.adjustedContentInset;

    if (!self.zoomBouncing && self.zooming)
        return contentOffset;

    auto contentMinX = -effectiveInset.left;
    auto contentMinY = -effectiveInset.top;
    auto contentSize = self.contentSize;
    auto contentMaxX = contentSize.width + effectiveInset.right;
    auto contentMaxY = contentSize.height + effectiveInset.bottom;

    if (CGRectGetWidth(bounds) >= self._wk_contentWidthIncludingInsets || CGRectGetMinX(bounds) < contentMinX)
        contentOffset.x = contentMinX;
    else if (CGRectGetMaxX(bounds) > contentMaxX)
        contentOffset.x = contentMaxX - CGRectGetWidth(bounds);

    if (CGRectGetHeight(bounds) >= self._wk_contentHeightIncludingInsets || CGRectGetMinY(bounds) < contentMinY)
        contentOffset.y = contentMinY;
    else if (CGRectGetMaxY(bounds) > contentMaxY)
        contentOffset.y = contentMaxY - CGRectGetHeight(bounds);

    return contentOffset;
}

// Consistent with the value of `_UISmartEpsilon` in UIKit.
static constexpr auto epsilonForComputingScrollability = 0.0001;

- (BOOL)_wk_canScrollHorizontallyWithoutBouncing
{
    return self._wk_contentWidthIncludingInsets - CGRectGetWidth(self.bounds) > epsilonForComputingScrollability;
}

- (BOOL)_wk_canScrollVerticallyWithoutBouncing
{
    return self._wk_contentHeightIncludingInsets - CGRectGetHeight(self.bounds) > epsilonForComputingScrollability;
}

@end

@implementation UIView (WebKitInternal)

- (UIViewController *)_wk_viewControllerForFullScreenPresentation
{
    auto controller = self.window.rootViewController;
    UIViewController *nextPresentedController = nil;
    while ((nextPresentedController = controller.presentedViewController))
        controller = nextPresentedController;
    return controller.viewIfLoaded.window == self.window ? controller : nil;
}

@end

namespace WebKit {

RetainPtr<UIAlertController> createUIAlertController(NSString *title, NSString *message)
{
    auto *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert _setTitleMaximumLineCount:0]; // No limit, we need to make sure the title doesn't get truncated.
    return alert;
}

UIScrollView *scrollViewForTouches(NSSet<UITouch *> *touches)
{
    for (UITouch *touch in touches) {
        if (auto scrollView = dynamic_objc_cast<UIScrollView>(touch.view))
            return scrollView;
    }
    return nil;
}

} // namespace WebKit

#endif // PLATFORM(IOS_FAMILY)
