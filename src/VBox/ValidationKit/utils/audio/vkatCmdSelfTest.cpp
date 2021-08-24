/* $Id$ */
/** @file
 * Validation Kit Audio Test (VKAT) - Self test.
 *
 * Self-test which does a complete audio testing framework run without the need
 * of a VM or other infrastructure, i.e. all required parts are running locally
 * on the same machine.
 *
 * This self-test does the following:
 * - 1. a) Creates an ATS instance to emulate the guest mode ("--mode guest")
 *         at port 6042 (ATS_TCP_DEF_BIND_PORT_GUEST).
 *      or
 *      b) Connect to an already existing guest ATS instance if "--guest-ats-address" is specified.
 *      This makes it more flexible in terms of testing / debugging.
 * - 2. Uses the Validation Kit audio backend, which in turn creates an ATS instance
 *      listening at port 6062 (ATS_TCP_DEF_BIND_PORT_VALKIT).
 * - 3. Uses the host test environment which creates an ATS instance
 *      listening at port 6052 (ATS_TCP_DEF_BIND_PORT_HOST).
 * - 4. Executes a complete test run locally (e.g. without any guest (VM) involved).
 */

/*
 * Copyright (C) 2021 Oracle Corporation
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

#include <iprt/ctype.h>
#include <iprt/errcore.h>
#include <iprt/getopt.h>
#include <iprt/message.h>
#include <iprt/test.h>

#include "Audio/AudioHlp.h"
#include "Audio/AudioTest.h"
#include "Audio/AudioTestService.h"
#include "Audio/AudioTestServiceClient.h"

#include "vkatInternal.h"


/**
 * Thread callback for mocking the guest (VM) side of things.
 *
 * @returns VBox status code.
 * @param   hThread             Thread handle.
 * @param   pvUser              Pointer to user-supplied data.
 */
static DECLCALLBACK(int) audioTestSelftestGuestAtsThread(RTTHREAD hThread, void *pvUser)
{
    RT_NOREF(hThread);
    PSELFTESTCTX pCtx = (PSELFTESTCTX)pvUser;

    PAUDIOTESTENV pTstEnv = &pCtx->Guest.TstEnv;

    /* Flag the environment for self test mode. */
    pTstEnv->fSelftest = true;

    /* Tweak the address the guest ATS is trying to connect to the host if anything else is specified.
     * Note: The host also runs on the same host (this self-test is completely self-contained and does not need a VM). */
    if (!pTstEnv->u.Guest.TcpOpts.szTcpConnectAddr[0])
        RTStrCopy(pTstEnv->u.Guest.TcpOpts.szTcpConnectAddr, sizeof(pTstEnv->u.Guest.TcpOpts.szTcpConnectAddr), "127.0.0.1");

    int rc = AudioTestSvcCreate(&pTstEnv->u.Guest.Srv);
    AssertRCReturn(rc, rc);

    /* Generate tag for guest side. */
    rc = RTStrCopy(pTstEnv->szTag, sizeof(pTstEnv->szTag), pCtx->szTag);
    AssertRCReturn(rc, rc);

    rc = AudioTestPathCreateTemp(pTstEnv->szPathTemp, sizeof(pTstEnv->szPathTemp), "selftest-guest");
    AssertRCReturn(rc, rc);

    rc = AudioTestPathCreateTemp(pTstEnv->szPathOut, sizeof(pTstEnv->szPathOut), "selftest-out");
    AssertRCReturn(rc, rc);

    pTstEnv->enmMode = AUDIOTESTMODE_GUEST;

    /** @todo Make this customizable. */
    PDMAudioPropsInit(&pTstEnv->Props,
                      2 /* 16-bit */, true  /* fSigned */, 2 /* cChannels */, 44100 /* uHz */);

    rc = audioTestEnvInit(pTstEnv, &pCtx->DrvStack);
    if (RT_SUCCESS(rc))
    {
        RTThreadUserSignal(hThread);

        audioTestWorker(pTstEnv);
        audioTestEnvDestroy(pTstEnv);
    }

    return rc;
}

/**
 * Main function for performing the self test.
 *
 * @returns RTEXITCODE
 * @param   pCtx                Self test context to use.
 */
RTEXITCODE audioTestDoSelftest(PSELFTESTCTX pCtx)
{
    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS,  "Running self test ...\n");

    /* Generate a common tag for guest and host side. */
    int rc = AudioTestGenTag(pCtx->szTag, sizeof(pCtx->szTag));
    AssertRCReturn(rc, RTEXITCODE_FAILURE);

    PAUDIOTESTENV pTstEnvHst = &pCtx->Host.TstEnv;

    /* Flag the environment for self test mode. */
    pTstEnvHst->fSelftest = true;

    /* Generate tag for host side. */
    rc = RTStrCopy(pTstEnvHst->szTag, sizeof(pTstEnvHst->szTag), pCtx->szTag);
    AssertRCReturn(rc, RTEXITCODE_FAILURE);

    rc = AudioTestPathCreateTemp(pTstEnvHst->szPathTemp, sizeof(pTstEnvHst->szPathTemp), "selftest-tmp");
    AssertRCReturn(rc, RTEXITCODE_FAILURE);

    rc = AudioTestPathCreateTemp(pTstEnvHst->szPathOut, sizeof(pTstEnvHst->szPathOut), "selftest-out");
    AssertRCReturn(rc, RTEXITCODE_FAILURE);

    /* Initialize the PCM properties to some sane values. */
    PDMAudioPropsInit(&pTstEnvHst->Props,
                      2 /* 16-bit */, true /* fPcmSigned */, 2 /* cPcmChannels */, 44100 /* uPcmHz */);

    /*
     * Step 1.
     */
    RTTHREAD hThreadGstAts = NIL_RTTHREAD;

    bool const fStartGuestAts = RTStrNLen(pCtx->Host.szGuestAtsAddr, sizeof(pCtx->Host.szGuestAtsAddr)) == 0;
    if (fStartGuestAts)
    {
        /* Step 1b. */
        rc = RTThreadCreate(&hThreadGstAts, audioTestSelftestGuestAtsThread, pCtx, 0, RTTHREADTYPE_IO, RTTHREADFLAGS_WAITABLE,
                            "VKATGstAts");
        if (RT_SUCCESS(rc))
            rc = RTThreadUserWait(hThreadGstAts, RT_MS_30SEC);
    }
    /* else Step 1a later. */

    RTThreadSleep(2000); /* Fudge: Wait until guest ATS is up. 2 seconds should be enough (tm). */

    if (RT_SUCCESS(rc))
    {
        /*
         * Steps 2 + 3.
         */
        pTstEnvHst->enmMode = AUDIOTESTMODE_HOST;

        rc = audioTestEnvInit(pTstEnvHst, &pCtx->DrvStack);
        if (RT_SUCCESS(rc))
        {
            /*
             * Step 4.
             */
            rc = audioTestWorker(pTstEnvHst);
            if (RT_SUCCESS(rc))
            {

            }

            audioTestEnvDestroy(pTstEnvHst);
        }
    }

    /*
     * Shutting down.
     */
    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS,  "Shutting down self test\n");

    /* If we started the guest ATS ourselves, wait for it to terminate properly. */
    if (fStartGuestAts)
    {
        int rcThread;
        int rc2 = RTThreadWait(hThreadGstAts, RT_MS_30SEC, &rcThread);
        if (RT_SUCCESS(rc2))
            rc2 = rcThread;
        if (RT_FAILURE(rc2))
            RTTestFailed(g_hTest, "Shutting down guest ATS failed with %Rrc\n", rc2);
        if (RT_SUCCESS(rc))
            rc = rc2;
    }

    if (RT_FAILURE(rc))
        RTTestFailed(g_hTest, "Self test failed with %Rrc\n", rc);

    return RT_SUCCESS(rc) ?  RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}


/*********************************************************************************************************************************
*   Command: selftest                                                                                                            *
*********************************************************************************************************************************/

/**
 * Long option values for the 'selftest' command.
 */
enum
{
    VKAT_SELFTEST_OPT_GUEST_ATS_ADDR = 900,
    VKAT_SELFTEST_OPT_GUEST_ATS_PORT,
    VKAT_SELFTEST_OPT_HOST_ATS_ADDR,
    VKAT_SELFTEST_OPT_HOST_ATS_PORT
};

/**
 * Command line parameters for self-test mode.
 */
static const RTGETOPTDEF s_aCmdSelftestOptions[] =
{
    { "--guest-ats-addr",   VKAT_SELFTEST_OPT_GUEST_ATS_ADDR,   RTGETOPT_REQ_STRING  },
    { "--guest-ats-port",   VKAT_SELFTEST_OPT_GUEST_ATS_PORT,   RTGETOPT_REQ_UINT32  },
    { "--host-ats-addr",    VKAT_SELFTEST_OPT_HOST_ATS_ADDR,    RTGETOPT_REQ_STRING  },
    { "--host-ats-port",    VKAT_SELFTEST_OPT_HOST_ATS_PORT,    RTGETOPT_REQ_UINT32  },
    { "--exclude-all",      'a',                                RTGETOPT_REQ_NOTHING },
    { "--backend",          'b',                                RTGETOPT_REQ_STRING  },
    { "--with-drv-audio",   'd',                                RTGETOPT_REQ_NOTHING },
    { "--exclude",          'e',                                RTGETOPT_REQ_UINT32  },
    { "--include",          'i',                                RTGETOPT_REQ_UINT32  }
};

/** the 'selftest' command option help. */
static DECLCALLBACK(const char *) audioTestCmdSelftestHelp(PCRTGETOPTDEF pOpt)
{
    switch (pOpt->iShort)
    {
        case 'a':                              return "Exclude all tests from the list (useful to enable single tests later with --include)";
        case 'b':                              return "The audio backend to use";
        case 'd':                              return "Go via DrvAudio instead of directly interfacing with the backend";
        case 'e':                              return "Exclude the given test id from the list";
        case 'i':                              return "Include the given test id in the list";
        case VKAT_SELFTEST_OPT_GUEST_ATS_ADDR: return "Address of guest ATS to connect to\n"
                                                      "    Default: 127.0.0.1; will start own guest ATS";
        case VKAT_SELFTEST_OPT_GUEST_ATS_PORT: return "Port of guest ATS to connect to\n"
                                                      "    Default: 6042"; /* ATS_TCP_DEF_CONNECT_PORT_GUEST */
        case VKAT_SELFTEST_OPT_HOST_ATS_ADDR:  return "Address of host ATS to connect to\n"
                                                      "    Default: " ATS_TCP_DEF_CONNECT_HOST_ADDR_STR;
        case VKAT_SELFTEST_OPT_HOST_ATS_PORT:  return "Port of host ATS to connect to\n"
                                                      "    Default: 6052"; /* ATS_TCP_DEF_BIND_PORT_VALKIT */
        default:  return NULL;
    }
}

/**
 * The 'selftest' command handler.
 *
 * @returns Program exit code.
 * @param   pGetState   RTGetOpt state.
 */
DECLCALLBACK(RTEXITCODE) audioTestCmdSelftestHandler(PRTGETOPTSTATE pGetState)
{
    SELFTESTCTX Ctx;
    RT_ZERO(Ctx);

    /* Argument processing loop: */
    int           rc;
    RTGETOPTUNION ValueUnion;
    while ((rc = RTGetOpt(pGetState, &ValueUnion)) != 0)
    {
        switch (rc)
        {
            case VKAT_SELFTEST_OPT_GUEST_ATS_ADDR:
                rc = RTStrCopy(Ctx.Host.szGuestAtsAddr, sizeof(Ctx.Host.szGuestAtsAddr), ValueUnion.psz);
                break;

            case VKAT_SELFTEST_OPT_GUEST_ATS_PORT:
                Ctx.Host.uGuestAtsPort = ValueUnion.u32;
                break;

            case VKAT_SELFTEST_OPT_HOST_ATS_ADDR:
                rc = RTStrCopy(Ctx.Host.szValKitAtsAddr, sizeof(Ctx.Host.szValKitAtsAddr), ValueUnion.psz);
                break;

            case VKAT_SELFTEST_OPT_HOST_ATS_PORT:
                Ctx.Host.uValKitAtsPort = ValueUnion.u32;
                break;

            case 'a':
                for (unsigned i = 0; i < g_cTests; i++)
                    g_aTests[i].fExcluded = true;
                break;

            case 'b':
                Ctx.pDrvReg = AudioTestFindBackendOpt(ValueUnion.psz);
                if (Ctx.pDrvReg == NULL)
                    return RTEXITCODE_SYNTAX;
                break;

            case 'd':
                Ctx.fWithDrvAudio = true;
                break;

            case 'e':
                if (ValueUnion.u32 >= g_cTests)
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Invalid test number %u passed to --exclude", ValueUnion.u32);
                g_aTests[ValueUnion.u32].fExcluded = true;
                break;

            case 'i':
                if (ValueUnion.u32 >= g_cTests)
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Invalid test number %u passed to --include", ValueUnion.u32);
                g_aTests[ValueUnion.u32].fExcluded = false;
                break;

            AUDIO_TEST_COMMON_OPTION_CASES(ValueUnion);

            default:
                return RTGetOptPrintError(rc, &ValueUnion);
        }
    }

    /* Go with the Validation Kit audio backend if nothing else is specified. */
    if (Ctx.pDrvReg == NULL)
        Ctx.pDrvReg = AudioTestFindBackendOpt("valkit");

    /*
     * In self-test mode the guest and the host side have to share the same driver stack,
     * as we don't have any device emulation between the two sides.
     *
     * This is necessary to actually get the played/recorded audio to from/to the guest
     * and host respectively.
     *
     * Choosing any other backend than the Validation Kit above *will* break this self-test!
     */
    rc = audioTestDriverStackInitEx(&Ctx.DrvStack, Ctx.pDrvReg,
                                    true /* fEnabledIn */, true /* fEnabledOut */, Ctx.fWithDrvAudio);
    if (RT_FAILURE(rc))
        return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Unable to init driver stack: %Rrc\n", rc);

    /*
     * Start testing.
     */
    RTTestBanner(g_hTest);

    int rc2 = audioTestDoSelftest(&Ctx);
    if (RT_FAILURE(rc2))
        RTTestFailed(g_hTest, "Self test failed with rc=%Rrc", rc2);

    audioTestDriverStackDelete(&Ctx.DrvStack);

    /*
     * Print summary and exit.
     */
    return RTTestSummaryAndDestroy(g_hTest);
}

/**
 * Command table entry for 'selftest'.
 */
const VKATCMD g_CmdSelfTest =
{
    "selftest",
    audioTestCmdSelftestHandler,
    "Performs self-tests.",
    s_aCmdSelftestOptions,
    RT_ELEMENTS(s_aCmdSelftestOptions),
    audioTestCmdSelftestHelp,
    true /* fNeedsTransport */
};

