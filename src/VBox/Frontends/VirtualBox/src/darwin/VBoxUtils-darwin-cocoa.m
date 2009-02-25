/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * Declarations of utility classes and functions for handling Darwin Cocoa
 * specific tasks
 */

/*
 * Copyright (C) 2009 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

#include "VBoxUtils-darwin.h"

#import <AppKit/NSWindow.h>
#import <AppKit/NSView.h>
#import <AppKit/NSEvent.h>

NativeWindowRef darwinToNativeWindowImpl (NativeViewRef aView)
{
    if (aView)
        return [aView window];
    return 0;
}

void darwinSetShowsToolbarButtonImpl (NativeWindowRef aWindow, bool aEnabled)
{
    [aWindow setShowsToolbarButton:aEnabled];
}

/**
 * Calls the + (void)setMouseCoalescingEnabled:(BOOL)flag class method.
 *
 * @param   fEnabled    Whether to enable or disable coalescing.
 */
void darwinSetMouseCoalescingEnabled (bool aEnabled)
{
    [NSEvent setMouseCoalescingEnabled:aEnabled];
}

