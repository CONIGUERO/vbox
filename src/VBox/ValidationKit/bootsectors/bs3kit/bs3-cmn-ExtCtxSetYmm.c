/* $Id$ */
/** @file
 * BS3Kit - Bs3ExtCtxSetYmm
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


#undef Bs3ExtCtxSetYmm
BS3_CMN_DEF(bool, Bs3ExtCtxSetYmm,(PBS3EXTCTX pExtCtx, uint8_t iReg, PCRTUINT256U pValue, uint8_t cbValue))
{
    BS3_ASSERT(cbValue == 16 || cbValue == 32);
    switch (pExtCtx->enmMethod)
    {
        case BS3EXTCTXMETHOD_FXSAVE:
            if (iReg < RT_ELEMENTS(pExtCtx->Ctx.x87.aXMM))
            {
                pExtCtx->Ctx.x87.aXMM[iReg].uXmm = pValue->DQWords.dqw0;
                return true;
            }
            break;

        case BS3EXTCTXMETHOD_XSAVE:
            if (iReg < RT_ELEMENTS(pExtCtx->Ctx.x.x87.aXMM))
            {
                pExtCtx->Ctx.x87.aXMM[iReg].uXmm = pValue->DQWords.dqw0;
                if (pExtCtx->fXcr0Nominal & XSAVE_C_YMM)
                {
                    if (cbValue >= 32)
                        pExtCtx->Ctx.x.u.YmmHi.aYmmHi[iReg].uXmm = pValue->DQWords.dqw1;
                    else
                    {
                        pExtCtx->Ctx.x.u.YmmHi.aYmmHi[iReg].au64[0] = 0;
                        pExtCtx->Ctx.x.u.YmmHi.aYmmHi[iReg].au64[1] = 0;
                    }
                    /** @todo zero high ZMM part. */
                }
                return true;
            }
            break;
    }
    return false;
}

