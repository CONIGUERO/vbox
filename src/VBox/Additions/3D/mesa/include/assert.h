/* $Id$ */
/** @file
 * Replaces C runtime assert with a simplified version which just hits breakpoint.
 *
 * Mesa code uses assert.h a lot, which is inconvenient because the C runtime
 * implementation wants to open a message box and it does not work in the
 * graphics driver.
 */

/*
 * Copyright (C) 2017-2022 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef GA_INCLUDED_3D_MESA_assert_h
#define GA_INCLUDED_3D_MESA_assert_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <iprt/asm.h>

#undef assert
#ifdef DEBUG
#define assert(_e) (void)( (!!(_e)) || (ASMBreakpoint(), 0) )
#else
#define assert(_e) (void)(0)
#endif

#endif /* !GA_INCLUDED_3D_MESA_assert_h */
