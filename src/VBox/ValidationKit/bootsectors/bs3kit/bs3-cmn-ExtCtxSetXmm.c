/* $Id$ */
/** @file
 * BS3Kit - Bs3ExtCtxSetXmm
 */

/*
 * Copyright (C) 2007-2022 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include "bs3kit-template-header.h"


#undef Bs3ExtCtxSetXmm
BS3_CMN_DEF(bool, Bs3ExtCtxSetXmm,(PBS3EXTCTX pExtCtx, uint8_t iReg, PCRTUINT128U pValue))
{
    AssertCompileMembersAtSameOffset(BS3EXTCTX, Ctx.x87.aXMM, BS3EXTCTX, Ctx.x.x87.aXMM);
    switch (pExtCtx->enmMethod)
    {
        case BS3EXTCTXMETHOD_FXSAVE:
        case BS3EXTCTXMETHOD_XSAVE:
            if (iReg < RT_ELEMENTS(pExtCtx->Ctx.x87.aXMM))
            {
                pExtCtx->Ctx.x87.aXMM[iReg].xmm = pValue->u;
                return true;
            }
            break;
    }
    return false;
}

