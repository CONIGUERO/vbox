/* $Id$ */
/** @file
 * Shared Clipboard Service - Host service entry points.
 */

/*
 * Copyright (C) 2006-2019 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/** @page pg_hostclip       The Shared Clipboard Host Service
 *
 * The shared clipboard host service provides a proxy between the host's
 * clipboard and a similar proxy running on a guest.  The service is split
 * into a platform-independent core and platform-specific backends.  The
 * service defines two communication protocols - one to communicate with the
 * clipboard service running on the guest, and one to communicate with the
 * backend.  These will be described in a very skeletal fashion here.
 *
 * @section sec_hostclip_guest_proto  The guest communication protocol
 *
 * The guest clipboard service communicates with the host service via HGCM
 * (the host service runs as an HGCM service).  The guest clipboard must
 * connect to the host service before all else (Windows hosts currently only
 * support one simultaneous connection).  Once it has connected, it can send
 * HGCM messages to the host services, some of which will receive replies from
 * the host.  The host can only reply to a guest message, it cannot initiate
 * any communication.  The guest can in theory send any number of messages in
 * parallel (see the descriptions of the messages for the practice), and the
 * host will receive these in sequence, and may reply to them at once
 * (releasing the caller in the guest) or defer the reply until later.
 *
 * There are currently four messages defined.  The first is
 * VBOX_SHARED_CLIPBOARD_FN_GET_HOST_MSG, which waits for a message from the
 * host.  Host messages currently defined are
 * VBOX_SHARED_CLIPBOARD_HOST_MSG_QUIT (unused),
 * VBOX_SHARED_CLIPBOARD_HOST_MSG_READ_DATA (request that the guest send the
 * contents of its clipboard to the host) and
 * VBOX_SHARED_CLIPBOARD_HOST_MSG_REPORT_FORMATS (to notify the guest that new
 * clipboard data is available).  If a host message is sent while the guest is
 * not waiting, it will be queued until the guest requests it.  At most one
 * host message of each type will be kept in the queue.  The host code only
 * supports a single simultaneous VBOX_SHARED_CLIPBOARD_FN_GET_HOST_MSG call
 * from the guest.
 *
 * The second guest message is VBOX_SHARED_CLIPBOARD_FN_FORMATS, which tells
 * the host that the guest has new clipboard data available.  The third is
 * VBOX_SHARED_CLIPBOARD_FN_READ_DATA, which asks the host to send its
 * clipboard data and waits until it arrives.  The host supports at most one
 * simultaneous VBOX_SHARED_CLIPBOARD_FN_READ_DATA call from the guest - if a
 * second call is made before the first has returned, the first will be
 * aborted.
 *
 * The last guest message is VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA, which is
 * used to send the contents of the guest clipboard to the host.  This call
 * should be used after the host has requested data from the guest.
 *
 * @section sec_hostclip_backend_proto  The communication protocol with the
 *                                      platform-specific backend
 *
 * This section may be written in the future :)
 *
 * @section sec_uri_intro               Transferring files.
 *
 * Since VBox x.x.x transferring files via Shared Clipboard is supported.
 * See the VBOX_WITH_SHARED_CLIPBOARD_URI_LIST define for supported / enabled
 * platforms.
 *
 * Copying files / directories from guest A to guest B requires the host
 * service to act as a proxy and cache, as we don't allow direct VM-to-VM
 * communication. Copying from / to the host also is taken into account.
 *
 * At the moment a transfer is a all-or-nothing operation, e.g. it either
 * completes orfails completely. There might be callbacks in the future
 * to e.g. skip failing entries.
 *
 * Known limitations:
 *
 * - Support for VRDE (VRDP) is not implemented yet (see #9498).
 * - Unicode support on Windows hosts / guests is not enabled (yet).
 * - Symbolic links are not yet handled.
 * - No support for ACLs yet.
 * - No (maybe never) support for NT4.
 *
 * @section sec_uri_areas               Clipboard areas.
 *
 * For larger / longer transfers there might be file data
 * temporarily cached on the host, which has not been transferred to the
 * destination yet. Such a cache is called a "Shared Clipboard Area", which
 * in turn is identified by a unique ID across all VMs running on the same
 * host. To control the access (and needed cleanup) of such clipboard areas,
 * VBoxSVC (Main) is used for this task. A Shared Clipboard client can register,
 * unregister, attach to and detach from a clipboard area. If all references
 * to a clipboard area are released, a clipboard area gets detroyed automatically
 * by VBoxSVC.
 *
 * By default a clipboard area lives in the user's temporary directory in the
 * sub folder "VirtualBox Shared Clipboards/clipboard-<ID>". VBoxSVC does not
 * do any file locking in a clipboard area, but keeps the clipboard areas's
 * directory open to prevent deletion by third party processes.
 *
 * @todo We might use some VFS / container (IPRT?) for this instead of the
 *       host's file system directly?
 *
 * @section sec_uri_structure           URI handling structure.
 *
 * All structures / classes are designed for running on both, on the guest
 * (via VBoxTray / VBoxClient) or on the host (host service) to avoid code
 * duplication where applicable.
 *
 * Per HGCM client there is a so-called "URI context", which in turn can have
 * one or mulitple so-called "URI transfer" objects. At the moment we only support
 * on concurrent URI transfer per URI context. It's being used for reading from a
 * source or writing to destination, depening on its direction. An URI transfer
 * can have optional callbacks which might be needed by various implementations.
 * Also, transfers optionally can run in an asynchronous thread to prevent
 * blocking the UI while running.
 *
 * An URI transfer can maintain its own clipboard area; for the host service such
 * a clipboard area is coupled to a clipboard area registered or attached with
 * VBoxSVC. This is needed because multiple transfers from multiple VMs (n:n) can
 * rely on the same clipboard area, so there needs a master keeping tracking of
 * a clipboard area. To minimize IPC traffic only the minimum de/attaching is done
 * at the moment. A clipboard area gets cleaned up (i.e. physically deleted) if
 * no references are held to it  anymore, or if VBoxSVC goes down.
 *
 * @section sec_uri_providers           URI providers.
 *
 * For certain implementations (for example on Windows guests / hosts, using
 * IDataObject and IStream objects) a more flexible approach reqarding reading /
 * writing is needed. For this so-called URI providers abstract the way of how
 * data is being read / written in the current context (host / guest), while
 * the rest of the code stays the same.
 *
 * @section sec_uri_protocol            URI protocol.
 *
 * The host service issues commands which the guest has to respond with an own
 * message to. The protocol itself is designed so that it has primitives to list
 * directories and open/close/read/write file system objects.
 *
 * The protocol does not rely on the old ReportMsg() / ReturnMsg() mechanism anymore
 * and uses a (per-client) message queue instead (see VBOX_SHARED_CLIPBOARD_GUEST_FN_GET_HOST_MSG_OLD
 * vs. VBOX_SHARED_CLIPBOARD_GUEST_FN_GET_HOST_MSG).
 *
 * Note that this is different from the DnD approach, as Shared Clipboard transfers
 * need to be deeper integrated within the host / guest OS (i.e. for progress UI),
 * and this might require non-monolithic / random access APIs to achieve.
 *
 * One transfer always is handled by an own (HGCM) client, so for multiple transfers
 * at the same time, multiple clients (client IDs) are being used. How this transfer
 * is implemented on the guest (and / or host) side depends upon the actual
 * implementation, e.g. via an own thread per transfer (see ClipboardStreamImpl-win.cpp
 * for example).
 *
 * As there can be multiple file system objects (fs objects) selected for transfer,
 * a transfer can be queried for its root entries, which then contains the top-level
 * elements. Based on these elements, (a) (recursive) listing(s) can be performed
 * to (partially) walk down into directories and query fs object information. The
 * provider provides appropriate interface for this, even if not all implementations
 * might need this mechanism.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_SHARED_CLIPBOARD
#include <VBox/log.h>

#include <VBox/AssertGuest.h>
#include <VBox/GuestHost/clipboard-helper.h>
#include <VBox/HostServices/Service.h>
#include <VBox/HostServices/VBoxClipboardSvc.h>
#include <VBox/HostServices/VBoxClipboardExt.h>

#include <iprt/alloc.h>
#include <iprt/string.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/rand.h>

#include <VBox/err.h>
#include <VBox/vmm/ssm.h>

#include "VBoxSharedClipboardSvc-internal.h"
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
# include "VBoxSharedClipboardSvc-uri.h"
#endif

using namespace HGCM;


/*********************************************************************************************************************************
*   Prototypes                                                                                                                   *
*********************************************************************************************************************************/
static int vboxSvcClipboardClientStateInit(PVBOXCLIPBOARDCLIENTSTATE pClientState, uint32_t uClientID);
static int vboxSvcClipboardClientStateDestroy(PVBOXCLIPBOARDCLIENTSTATE pClientState);
static void vboxSvcClipboardClientStateReset(PVBOXCLIPBOARDCLIENTSTATE pClientState);


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
PVBOXHGCMSVCHELPERS g_pHelpers;

static RTCRITSECT g_CritSect;
static uint32_t g_uMode;

PFNHGCMSVCEXT g_pfnExtension;
void *g_pvExtension;

/* Serialization of data reading and format announcements from the RDP client. */
static bool g_fReadingData = false;
static bool g_fDelayedAnnouncement = false;
static uint32_t g_u32DelayedFormats = 0;

/** Is the clipboard running in headless mode? */
static bool g_fHeadless = false;

/** Global map of all connected clients. */
ClipboardClientMap g_mapClients;

/** Global map of all registered event sources. */
ClipboardEventSourceMap g_mapEventSources;

/** Global list of all clients which are queued up (deferred return) and ready
 *  to process new commands. The key is the (unique) client ID. */
ClipboardClientQueue g_listClientsDeferred;


/**
 * Creates a (unique) event source ID.
 *
 * @returns VBox status code, or VERR_NOT_FOUND on error.
 * @param   puID                Where to store the created event source ID on success.
 */
int vboxSvcClipboardEventSourceCreateID(PVBOXCLIPBOARDEVENTSOURCEID puID)
{
    AssertPtrReturn(puID, VERR_INVALID_POINTER);

    for (uint32_t i = 0; i < 32; i++) /* Don't try too hard. */
    {
        VBOXCLIPBOARDEVENTSOURCEID uID = RTRandU32() % VBOX_SHARED_CLIPBOARD_MAX_EVENT_SOURCES;
        if (g_mapEventSources.find(uID) == g_mapEventSources.end())
        {
            *puID = uID;
            return VINF_SUCCESS;
        }
    }

    return VERR_NOT_FOUND;
}

uint32_t vboxSvcClipboardGetMode(void)
{
    return g_uMode;
}

#ifdef UNIT_TEST
/** Testing interface, getter for clipboard mode */
uint32_t TestClipSvcGetMode(void)
{
    return vboxSvcClipboardGetMode();
}
#endif

/** Getter for headless setting. Also needed by testcase. */
bool VBoxSvcClipboardGetHeadless(void)
{
    return g_fHeadless;
}

static void vboxSvcClipboardModeSet(uint32_t u32Mode)
{
    switch (u32Mode)
    {
        case VBOX_SHARED_CLIPBOARD_MODE_OFF:
            RT_FALL_THROUGH();
        case VBOX_SHARED_CLIPBOARD_MODE_HOST_TO_GUEST:
            RT_FALL_THROUGH();
        case VBOX_SHARED_CLIPBOARD_MODE_GUEST_TO_HOST:
            RT_FALL_THROUGH();
        case VBOX_SHARED_CLIPBOARD_MODE_BIDIRECTIONAL:
            g_uMode = u32Mode;
            break;

        default:
            g_uMode = VBOX_SHARED_CLIPBOARD_MODE_OFF;
            break;
    }
}

bool VBoxSvcClipboardLock(void)
{
    return RT_SUCCESS(RTCritSectEnter(&g_CritSect));
}

void VBoxSvcClipboardUnlock(void)
{
    int rc2 = RTCritSectLeave(&g_CritSect);
    AssertRC(rc2);
}

/**
 * Resets a client's state message queue.
 *
 * @param   pClientData         Pointer to the client data structure to reset message queue for.
 */
void vboxSvcClipboardMsgQueueReset(PVBOXCLIPBOARDCLIENT pClient)
{
    LogFlowFuncEnter();

    while (!pClient->queueMsg.isEmpty())
    {
        RTMemFree(pClient->queueMsg.last());
        pClient->queueMsg.removeLast();
    }
}

/**
 * Allocates a new clipboard message.
 *
 * @returns Allocated clipboard message, or NULL on failure.
 * @param   uMsg                Message type of message to allocate.
 * @param   cParms              Number of HGCM parameters to allocate.
 */
PVBOXCLIPBOARDCLIENTMSG vboxSvcClipboardMsgAlloc(uint32_t uMsg, uint32_t cParms)
{
    PVBOXCLIPBOARDCLIENTMSG pMsg = (PVBOXCLIPBOARDCLIENTMSG)RTMemAlloc(sizeof(VBOXCLIPBOARDCLIENTMSG));
    if (pMsg)
    {
        pMsg->m_paParms = (PVBOXHGCMSVCPARM)RTMemAllocZ(sizeof(VBOXHGCMSVCPARM) * cParms);
        if (pMsg->m_paParms)
        {
            pMsg->m_cParms = cParms;
            pMsg->m_uMsg   = uMsg;

            return pMsg;
        }
    }

    RTMemFree(pMsg);
    return NULL;
}

/**
 * Frees a formerly allocated clipboard message.
 *
 * @param   pMsg                Clipboard message to free.
 *                              The pointer will be invalid after calling this function.
 */
void vboxSvcClipboardMsgFree(PVBOXCLIPBOARDCLIENTMSG pMsg)
{
    if (!pMsg)
        return;

    if (pMsg->m_paParms)
        RTMemFree(pMsg->m_paParms);

    RTMemFree(pMsg);
    pMsg = NULL;
}

/**
 * Sets the VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_WAIT and VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_NOWAIT
 * return parameters.
 *
 * @param   pMsg        Message to set return parameters to.
 * @param   paDstParms  The peek parameter vector.
 * @param   cDstParms   The number of peek parameters (at least two).
 * @remarks ASSUMES the parameters has been cleared by clientMsgPeek.
 */
void vboxSvcClipboardMsgSetPeekReturn(PVBOXCLIPBOARDCLIENTMSG pMsg, PVBOXHGCMSVCPARM paDstParms, uint32_t cDstParms)
{
    Assert(cDstParms >= 2);
    if (paDstParms[0].type == VBOX_HGCM_SVC_PARM_32BIT)
        paDstParms[0].u.uint32 = pMsg->m_uMsg;
    else
        paDstParms[0].u.uint64 = pMsg->m_uMsg;
    paDstParms[1].u.uint32 = pMsg->m_cParms;

    uint32_t i = RT_MIN(cDstParms, pMsg->m_cParms + 2);
    while (i-- > 2)
        switch (pMsg->m_paParms[i - 2].type)
        {
            case VBOX_HGCM_SVC_PARM_32BIT: paDstParms[i].u.uint32 = ~(uint32_t)sizeof(uint32_t); break;
            case VBOX_HGCM_SVC_PARM_64BIT: paDstParms[i].u.uint32 = ~(uint32_t)sizeof(uint64_t); break;
            case VBOX_HGCM_SVC_PARM_PTR:   paDstParms[i].u.uint32 = pMsg->m_paParms[i - 2].u.pointer.size; break;
        }
}

/**
 * Adds a new message to a client'S message queue.
 *
 * @returns IPRT status code.
 * @param   pClientData         Pointer to the client data structure to add new message to.
 * @param   pMsg                Pointer to message to add. The queue then owns the pointer.
 * @param   fAppend             Whether to append or prepend the message to the queue.
 */
int vboxSvcClipboardMsgAdd(PVBOXCLIPBOARDCLIENT pClient, PVBOXCLIPBOARDCLIENTMSG pMsg, bool fAppend)
{
    AssertPtrReturn(pMsg, VERR_INVALID_POINTER);

    LogFlowFunc(("uMsg=%RU32 (%s), cParms=%RU32, fAppend=%RTbool\n",
                 pMsg->m_uMsg, VBoxClipboardHostMsgToStr(pMsg->m_uMsg), pMsg->m_cParms, fAppend));

    if (fAppend)
        pClient->queueMsg.append(pMsg);
    else
        pClient->queueMsg.prepend(pMsg);

    /** @todo Catch / handle OOM? */

    return VINF_SUCCESS;
}

/**
 * Implements VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_WAIT and VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_NOWAIT.
 *
 * @returns VBox status code.
 * @retval  VINF_SUCCESS if a message was pending and is being returned.
 * @retval  VERR_TRY_AGAIN if no message pending and not blocking.
 * @retval  VERR_RESOURCE_BUSY if another read already made a waiting call.
 * @retval  VINF_HGCM_ASYNC_EXECUTE if message wait is pending.
 *
 * @param   pClient     The client state.
 * @param   hCall       The client's call handle.
 * @param   cParms      Number of parameters.
 * @param   paParms     Array of parameters.
 * @param   fWait       Set if we should wait for a message, clear if to return
 *                      immediately.
 */
int vboxSvcClipboardMsgPeek(PVBOXCLIPBOARDCLIENT pClient, VBOXHGCMCALLHANDLE hCall, uint32_t cParms, VBOXHGCMSVCPARM paParms[],
                            bool fWait)
{
    /*
     * Validate the request.
     */
    ASSERT_GUEST_MSG_RETURN(cParms >= 2, ("cParms=%u!\n", cParms), VERR_WRONG_PARAMETER_COUNT);

    uint64_t idRestoreCheck = 0;
    uint32_t i              = 0;
    if (paParms[i].type == VBOX_HGCM_SVC_PARM_64BIT)
    {
        idRestoreCheck = paParms[0].u.uint64;
        paParms[0].u.uint64 = 0;
        i++;
    }
    for (; i < cParms; i++)
    {
        ASSERT_GUEST_MSG_RETURN(paParms[i].type == VBOX_HGCM_SVC_PARM_32BIT, ("#%u type=%u\n", i, paParms[i].type),
                                VERR_WRONG_PARAMETER_TYPE);
        paParms[i].u.uint32 = 0;
    }

    /*
     * Check restore session ID.
     */
    if (idRestoreCheck != 0)
    {
        uint64_t idRestore = g_pHelpers->pfnGetVMMDevSessionId(g_pHelpers);
        if (idRestoreCheck != idRestore)
        {
            paParms[0].u.uint64 = idRestore;
            LogFlowFunc(("[Client %RU32] VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_XXX -> VERR_VM_RESTORED (%#RX64 -> %#RX64)\n",
                         pClient->uClientID, idRestoreCheck, idRestore));
            return VERR_VM_RESTORED;
        }
        Assert(!g_pHelpers->pfnIsCallRestored(hCall));
    }

    /*
     * Return information about the first message if one is pending in the list.
     */
    if (!pClient->queueMsg.isEmpty())
    {
        PVBOXCLIPBOARDCLIENTMSG pFirstMsg = pClient->queueMsg.first();
        if (pFirstMsg)
        {
            vboxSvcClipboardMsgSetPeekReturn(pFirstMsg, paParms, cParms);
            LogFlowFunc(("[Client %RU32] VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_XXX -> VINF_SUCCESS (idMsg=%u (%s), cParms=%u)\n",
                         pClient->uClientID, pFirstMsg->m_uMsg, VBoxClipboardHostMsgToStr(pFirstMsg->m_uMsg),
                         pFirstMsg->m_cParms));
            return VINF_SUCCESS;
        }
    }

    /*
     * If we cannot wait, fail the call.
     */
    if (!fWait)
    {
        LogFlowFunc(("[Client %RU32] GUEST_MSG_PEEK_NOWAIT -> VERR_TRY_AGAIN\n", pClient->uClientID));
        return VERR_TRY_AGAIN;
    }

    /*
     * Wait for the host to queue a message for this client.
     */
    ASSERT_GUEST_MSG_RETURN(pClient->Pending.uType == 0, ("Already pending! (idClient=%RU32)\n",
                                                           pClient->uClientID), VERR_RESOURCE_BUSY);
    pClient->Pending.hHandle = hCall;
    pClient->Pending.cParms  = cParms;
    pClient->Pending.paParms = paParms;
    pClient->Pending.uType   = VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_WAIT;
    LogFlowFunc(("[Client %RU32] Is now in pending mode...\n", pClient->uClientID));
    return VINF_HGCM_ASYNC_EXECUTE;
}

/**
 * Implements VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_GET.
 *
 * @returns VBox status code.
 * @retval  VINF_SUCCESS if message retrieved and removed from the pending queue.
 * @retval  VERR_TRY_AGAIN if no message pending.
 * @retval  VERR_BUFFER_OVERFLOW if a parmeter buffer is too small.  The buffer
 *          size was updated to reflect the required size, though this isn't yet
 *          forwarded to the guest.  (The guest is better of using peek with
 *          parameter count + 2 parameters to get the sizes.)
 * @retval  VERR_MISMATCH if the incoming message ID does not match the pending.
 * @retval  VINF_HGCM_ASYNC_EXECUTE if message was completed already.
 *
 * @param   pClient      The client state.
 * @param   hCall        The client's call handle.
 * @param   cParms       Number of parameters.
 * @param   paParms      Array of parameters.
 */
int vboxSvcClipboardMsgGet(PVBOXCLIPBOARDCLIENT pClient, VBOXHGCMCALLHANDLE hCall, uint32_t cParms, VBOXHGCMSVCPARM paParms[])
{
    /*
     * Validate the request.
     */
    uint32_t const idMsgExpected = cParms > 0 && paParms[0].type == VBOX_HGCM_SVC_PARM_32BIT ? paParms[0].u.uint32
                                 : cParms > 0 && paParms[0].type == VBOX_HGCM_SVC_PARM_64BIT ? paParms[0].u.uint64
                                 : UINT32_MAX;

    /*
     * Return information about the first message if one is pending in the list.
     */
    if (!pClient->queueMsg.isEmpty())
    {
        PVBOXCLIPBOARDCLIENTMSG pFirstMsg = pClient->queueMsg.first();
        if (pFirstMsg)
        {
            LogFlowFunc(("First message is: %RU32 (%s), cParms=%RU32\n",
                         pFirstMsg->m_uMsg, VBoxClipboardHostMsgToStr(pFirstMsg->m_uMsg), pFirstMsg->m_cParms));

            ASSERT_GUEST_MSG_RETURN(pFirstMsg->m_uMsg == idMsgExpected || idMsgExpected == UINT32_MAX,
                                    ("idMsg=%u (%s) cParms=%u, caller expected %u (%s) and %u\n",
                                     pFirstMsg->m_uMsg, VBoxClipboardHostMsgToStr(pFirstMsg->m_uMsg), pFirstMsg->m_cParms,
                                     idMsgExpected, VBoxClipboardHostMsgToStr(idMsgExpected), cParms),
                                    VERR_MISMATCH);
            ASSERT_GUEST_MSG_RETURN(pFirstMsg->m_cParms == cParms,
                                    ("idMsg=%u (%s) cParms=%u, caller expected %u (%s) and %u\n",
                                     pFirstMsg->m_uMsg, VBoxClipboardHostMsgToStr(pFirstMsg->m_uMsg), pFirstMsg->m_cParms,
                                     idMsgExpected, VBoxClipboardHostMsgToStr(idMsgExpected), cParms),
                                    VERR_WRONG_PARAMETER_COUNT);

            /* Check the parameter types. */
            for (uint32_t i = 0; i < cParms; i++)
                ASSERT_GUEST_MSG_RETURN(pFirstMsg->m_paParms[i].type == paParms[i].type,
                                        ("param #%u: type %u, caller expected %u (idMsg=%u %s)\n", i, pFirstMsg->m_paParms[i].type,
                                         paParms[i].type, pFirstMsg->m_uMsg, VBoxClipboardHostMsgToStr(pFirstMsg->m_uMsg)),
                                        VERR_WRONG_PARAMETER_TYPE);
            /*
             * Copy out the parameters.
             *
             * No assertions on buffer overflows, and keep going till the end so we can
             * communicate all the required buffer sizes.
             */
            int rc = VINF_SUCCESS;
            for (uint32_t i = 0; i < cParms; i++)
                switch (pFirstMsg->m_paParms[i].type)
                {
                    case VBOX_HGCM_SVC_PARM_32BIT:
                        paParms[i].u.uint32 = pFirstMsg->m_paParms[i].u.uint32;
                        break;

                    case VBOX_HGCM_SVC_PARM_64BIT:
                        paParms[i].u.uint64 = pFirstMsg->m_paParms[i].u.uint64;
                        break;

                    case VBOX_HGCM_SVC_PARM_PTR:
                    {
                        uint32_t const cbSrc = pFirstMsg->m_paParms[i].u.pointer.size;
                        uint32_t const cbDst = paParms[i].u.pointer.size;
                        paParms[i].u.pointer.size = cbSrc; /** @todo Check if this is safe in other layers...
                                                            * Update: Safe, yes, but VMMDevHGCM doesn't pass it along. */
                        if (cbSrc <= cbDst)
                            memcpy(paParms[i].u.pointer.addr, pFirstMsg->m_paParms[i].u.pointer.addr, cbSrc);
                        else
                        {
                            AssertMsgFailed(("#%u: cbSrc=%RU32 is bigger than cbDst=%RU32\n", i, cbSrc, cbDst));
                            rc = VERR_BUFFER_OVERFLOW;
                        }
                        break;
                    }

                    default:
                        AssertMsgFailed(("#%u: %u\n", i, pFirstMsg->m_paParms[i].type));
                        rc = VERR_INTERNAL_ERROR;
                        break;
                }
            if (RT_SUCCESS(rc))
            {
                /*
                 * Complete the message and remove the pending message unless the
                 * guest raced us and cancelled this call in the meantime.
                 */
                AssertPtr(g_pHelpers);
                rc = g_pHelpers->pfnCallComplete(hCall, rc);

                LogFlowFunc(("[Client %RU32] pfnCallComplete -> %Rrc\n", pClient->uClientID, rc));

                if (rc != VERR_CANCELLED)
                {
                    pClient->queueMsg.removeFirst();
                    vboxSvcClipboardMsgFree(pFirstMsg);
                }

                return VINF_HGCM_ASYNC_EXECUTE; /* The caller must not complete it. */
            }

            LogFlowFunc(("[Client %RU32] Returning %Rrc\n", pClient->uClientID, rc));
            return rc;
        }
    }

    paParms[0].u.uint32 = 0;
    paParms[1].u.uint32 = 0;
    LogFlowFunc(("[Client %RU32] -> VERR_TRY_AGAIN\n", pClient->uClientID));
    return VERR_TRY_AGAIN;
}

int vboxSvcClipboardClientWakeup(PVBOXCLIPBOARDCLIENT pClient)
{
    int rc = VINF_NO_CHANGE;

    if (pClient->Pending.uType)
    {
        LogFlowFunc(("[Client %RU32] Waking up ...\n", pClient->uClientID));

        rc = VINF_SUCCESS;

        if (!pClient->queueMsg.isEmpty())
        {
            PVBOXCLIPBOARDCLIENTMSG pFirstMsg = pClient->queueMsg.first();
            if (pFirstMsg)
            {
                LogFlowFunc(("[Client %RU32] Current host message is %RU32 (%s), cParms=%RU32\n",
                             pClient->uClientID, pFirstMsg->m_uMsg, VBoxClipboardHostMsgToStr(pFirstMsg->m_uMsg),
                             pFirstMsg->m_cParms));

                if (pClient->Pending.uType == VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_WAIT)
                {
                    vboxSvcClipboardMsgSetPeekReturn(pFirstMsg, pClient->Pending.paParms, pClient->Pending.cParms);
                    rc = g_pHelpers->pfnCallComplete(pClient->Pending.hHandle, VINF_SUCCESS);

                    pClient->Pending.hHandle = NULL;
                    pClient->Pending.paParms = NULL;
                    pClient->Pending.cParms  = 0;
                    pClient->Pending.uType   = false;
                }
            }
            else
                AssertFailed();
        }
        else
            AssertMsgFailed(("Waking up client ID=%RU32 with no host message in queue is a bad idea\n", pClient->uClientID));

        return rc;
    }

    return VINF_NO_CHANGE;
}

/**
 * Set the HGCM parameters according to pending messages.
 * Executed under the clipboard lock.
 *
 * Legacy protocol, do not use anymore.
 */
static bool vboxSvcClipboardOldReturnMsg(PVBOXCLIPBOARDCLIENT pClient, uint32_t cParms, VBOXHGCMSVCPARM paParms[])
{
    /** @todo r=andy The client at the moment supplies two parameters, which we can
     *        use by filling in the next message type sent by the host service.
     *        Make this more flexible later, as I don't want to break the existing protocol right now. */
    if (cParms < 2)
    {
        AssertFailed(); /* Should never happen. */
        return false;
    }

    /* Message priority is taken into account. */
    if (pClient->State.Old.fHostMsgQuit)
    {
        LogFlowFunc(("VBOX_SHARED_CLIPBOARD_HOST_MSG_QUIT\n"));
        HGCMSvcSetU32(&paParms[0], VBOX_SHARED_CLIPBOARD_HOST_MSG_QUIT);
        HGCMSvcSetU32(&paParms[1], 0);
        pClient->State.Old.fHostMsgQuit = false;
    }
    else if (pClient->State.Old.fHostMsgReadData)
    {
        uint32_t fFormat = 0;

        LogFlowFunc(("VBOX_SHARED_CLIPBOARD_HOST_MSG_READ_DATA: u32RequestedFormat=%02X\n",
                     pClient->State.Old.u32RequestedFormat));

        if (pClient->State.Old.u32RequestedFormat & VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT)
            fFormat = VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT;
        else if (pClient->State.Old.u32RequestedFormat & VBOX_SHARED_CLIPBOARD_FMT_BITMAP)
            fFormat = VBOX_SHARED_CLIPBOARD_FMT_BITMAP;
        else if (pClient->State.Old.u32RequestedFormat & VBOX_SHARED_CLIPBOARD_FMT_HTML)
            fFormat = VBOX_SHARED_CLIPBOARD_FMT_HTML;
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
        else if (pClient->State.Old.u32RequestedFormat & VBOX_SHARED_CLIPBOARD_FMT_URI_LIST)
            fFormat = VBOX_SHARED_CLIPBOARD_FMT_URI_LIST;
#endif
        else
        {
            LogRel2(("Clipboard: Unsupported format from guest (0x%x), skipping\n", fFormat));
            pClient->State.Old.u32RequestedFormat = 0;
        }
        pClient->State.Old.u32RequestedFormat &= ~fFormat;
        HGCMSvcSetU32(&paParms[0], VBOX_SHARED_CLIPBOARD_HOST_MSG_READ_DATA);
        HGCMSvcSetU32(&paParms[1], fFormat);
        if (pClient->State.Old.u32RequestedFormat == 0)
            pClient->State.Old.fHostMsgReadData = false;
    }
    else if (pClient->State.Old.fHostMsgFormats)
    {
        LogFlowFunc(("VBOX_SHARED_CLIPBOARD_HOST_MSG_REPORT_FORMATS: u32AvailableFormats=%02X\n",
                     pClient->State.Old.u32AvailableFormats));

        HGCMSvcSetU32(&paParms[0], VBOX_SHARED_CLIPBOARD_HOST_MSG_FORMATS_WRITE);
        HGCMSvcSetU32(&paParms[1], pClient->State.Old.u32AvailableFormats);
        pClient->State.Old.fHostMsgFormats = false;
    }
    else
    {
        /* No pending messages. */
        LogFlowFunc(("No pending message\n"));
        return false;
    }

    /* Message information assigned. */
    return true;
}

int vboxSvcClipboardSendFormatsWrite(PVBOXCLIPBOARDCLIENT pClient, PSHAREDCLIPBOARDFORMATDATA pFormats)
{
    AssertPtrReturn(pClient, VERR_INVALID_POINTER);

    int rc;

    PVBOXCLIPBOARDCLIENTMSG pMsg = vboxSvcClipboardMsgAlloc(VBOX_SHARED_CLIPBOARD_HOST_MSG_FORMATS_WRITE, 3);
    if (pMsg)
    {
        VBOXCLIPBOARDEVENTID uEvent = SharedClipboardEventIDGenerate(&pClient->Events);

        HGCMSvcSetU32(&pMsg->m_paParms[0], VBOX_SHARED_CLIPBOARD_CONTEXTID_MAKE(pClient->Events.uID, uEvent));
        HGCMSvcSetU32(&pMsg->m_paParms[1], pFormats->uFormats);
        HGCMSvcSetU32(&pMsg->m_paParms[2], 0 /* fFlags */);

        rc = vboxSvcClipboardMsgAdd(pClient, pMsg, true /* fAppend */);
        if (RT_SUCCESS(rc))
        {
            rc = vboxSvcClipboardClientWakeup(pClient);
        }
    }
    else
        rc = VERR_NO_MEMORY;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int vboxSvcClipboardGetDataWrite(PVBOXCLIPBOARDCLIENT pClient, uint32_t cParms, VBOXHGCMSVCPARM paParms[])
{
    LogFlowFuncEnter();

    if (   vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_GUEST_TO_HOST
        && vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_BIDIRECTIONAL)
    {
        return VERR_NOT_SUPPORTED;
    }

    int rc;

    SHAREDCLIPBOARDDATABLOCK dataBlock;
    RT_ZERO(dataBlock);

    VBOXCLIPBOARDCLIENTCMDCTX cmdCtx;
    RT_ZERO(cmdCtx);

    if (pClient->State.uProtocolVer == 0) /* Legacy protocol */
    {
        if (cParms < 2)
        {
            rc = VERR_INVALID_PARAMETER;
        }
        else if (   paParms[0].type != VBOX_HGCM_SVC_PARM_32BIT   /* format */
                 || paParms[1].type != VBOX_HGCM_SVC_PARM_PTR)    /* ptr */
        {
            rc = VERR_INVALID_PARAMETER;
        }
        else
        {
            rc = HGCMSvcGetU32(&paParms[0], &dataBlock.uFormat);
            if (RT_SUCCESS(rc))
                rc = HGCMSvcGetBuf(&paParms[1], &dataBlock.pvData, &dataBlock.cbData);
        }
    }
    else
    {
        if (cParms < VBOX_SHARED_CLIPBOARD_CPARMS_WRITE_DATA)
        {
            rc = VERR_INVALID_PARAMETER;
        }
        else if (   paParms[0].type != VBOX_HGCM_SVC_PARM_32BIT   /* uContext */
                 || paParms[1].type != VBOX_HGCM_SVC_PARM_32BIT   /* uFormat */
                 || paParms[2].type != VBOX_HGCM_SVC_PARM_32BIT   /* cbData */
                 || paParms[3].type != VBOX_HGCM_SVC_PARM_PTR)    /* pvData */
        {
            rc = VERR_INVALID_PARAMETER;
        }
        else
        {
            rc = HGCMSvcGetU32(&paParms[0], &cmdCtx.uContextID);
            if (RT_SUCCESS(rc))
                rc = HGCMSvcGetU32(&paParms[1], &dataBlock.uFormat);
            if (RT_SUCCESS(rc))
                rc = HGCMSvcGetBuf(&paParms[3], &dataBlock.pvData, &dataBlock.cbData);

            /** @todo Handle the rest. */
        }
    }

    if (RT_SUCCESS(rc))
    {
        if (g_pfnExtension)
        {
            VBOXCLIPBOARDEXTPARMS parms;
            RT_ZERO(parms);

            parms.u32Format = dataBlock.uFormat;
            parms.u.pvData  = dataBlock.pvData;
            parms.cbData    = dataBlock.cbData;

            g_pfnExtension(g_pvExtension, VBOX_CLIPBOARD_EXT_FN_DATA_WRITE, &parms, sizeof(parms));
        }

        rc = VBoxClipboardSvcImplWriteData(pClient, &cmdCtx, &dataBlock);
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Reports a cached message back to the guest.
 *
 * Legacy protocol, do not use anymore.
 */
int vboxSvcClipboardOldReportMsg(PVBOXCLIPBOARDCLIENT pClient, uint32_t uMsg, uint32_t uFormats)
{
    AssertPtrReturn(pClient, VERR_INVALID_POINTER);

    int rc = VINF_SUCCESS;

    LogFlowFunc(("uMsg=%RU32 (%s), fIsAsync=%RTbool\n",
                 uMsg, VBoxClipboardHostMsgToStr(uMsg), pClient->State.Old.fAsync));

    if (VBoxSvcClipboardLock())
    {
        switch (uMsg)
        {
            case VBOX_SHARED_CLIPBOARD_HOST_MSG_QUIT:
            {
                pClient->State.Old.fHostMsgQuit = true;
            } break;

            case VBOX_SHARED_CLIPBOARD_HOST_MSG_READ_DATA:
            {
                if (   vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_GUEST_TO_HOST
                    && vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_BIDIRECTIONAL)
                {
                    /* Skip the message. */
                    break;
                }

                LogFlowFunc(("uFormats=%02X\n", uFormats));

                pClient->State.Old.u32RequestedFormat = uFormats;
                pClient->State.Old.fHostMsgReadData = true;
            } break;

            case VBOX_SHARED_CLIPBOARD_HOST_MSG_FORMATS_WRITE:
            {
                if (   vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_HOST_TO_GUEST
                    && vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_BIDIRECTIONAL)
                {
                    /* Skip the message. */
                    break;
                }

                LogFlowFunc(("uFormats=%02X\n", uFormats));

                pClient->State.Old.u32AvailableFormats = uFormats;
                pClient->State.Old.fHostMsgFormats = true;
            } break;

            default:
            {
                AssertMsgFailed(("Invalid message %RU32\n", uMsg));
                rc = VERR_INVALID_PARAMETER;
                break;
            }
        }

        if (RT_SUCCESS(rc))
        {
            if (pClient->State.Old.fAsync)
            {
                /* The client waits for a response. */
                bool fMessageReturned = vboxSvcClipboardOldReturnMsg(pClient,
                                                                  pClient->State.Old.async.cParms,
                                                                  pClient->State.Old.async.paParms);

                /* Make a copy of the handle. */
                VBOXHGCMCALLHANDLE callHandle = pClient->State.Old.async.callHandle;

                if (fMessageReturned)
                {
                    /* There is a response. */
                    pClient->State.Old.fAsync = false;
                }

                VBoxSvcClipboardUnlock();

                if (fMessageReturned)
                {
                    LogFlowFunc(("CallComplete\n"));
                    g_pHelpers->pfnCallComplete(callHandle, VINF_SUCCESS);
                }
            }
            else
                VBoxSvcClipboardUnlock();
        }
        else
            VBoxSvcClipboardUnlock();
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}


int vboxSvcClipboardSetSource(PVBOXCLIPBOARDCLIENT pClient, SHAREDCLIPBOARDSOURCE enmSource)
{
    if (!pClient) /* If no client connected (anymore), bail out. */
        return VINF_SUCCESS;

    int rc = VINF_SUCCESS;

    if (VBoxSvcClipboardLock())
    {
        pClient->State.enmSource = enmSource;

        LogFlowFunc(("Source of client %RU32 is now %RU32\n", pClient->State.u32ClientID, pClient->State.enmSource));

        VBoxSvcClipboardUnlock();
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int svcInit(void)
{
    int rc = RTCritSectInit(&g_CritSect);

    if (RT_SUCCESS(rc))
    {
        vboxSvcClipboardModeSet(VBOX_SHARED_CLIPBOARD_MODE_OFF);

        rc = VBoxClipboardSvcImplInit();

        /* Clean up on failure, because 'svnUnload' will not be called
         * if the 'svcInit' returns an error.
         */
        if (RT_FAILURE(rc))
        {
            RTCritSectDelete(&g_CritSect);
        }
    }

    return rc;
}

static DECLCALLBACK(int) svcUnload(void *)
{
    LogFlowFuncEnter();

    VBoxClipboardSvcImplDestroy();

    RTCritSectDelete(&g_CritSect);

    return VINF_SUCCESS;
}

/**
 * Disconnect the host side of the shared clipboard and send a "host disconnected" message
 * to the guest side.
 */
static DECLCALLBACK(int) svcDisconnect(void *, uint32_t u32ClientID, void *pvClient)
{
    RT_NOREF(u32ClientID, pvClient);

    LogFunc(("u32ClientID=%RU32\n", u32ClientID));

    PVBOXCLIPBOARDCLIENT pClient = (PVBOXCLIPBOARDCLIENT)pvClient;
    AssertPtr(pClient);

    vboxSvcClipboardOldReportMsg(pClient, VBOX_SHARED_CLIPBOARD_HOST_MSG_QUIT, 0); /** @todo r=andy Why is this necessary? The client already disconnected ... */
    vboxSvcClipboardOldCompleteReadData(pClient, VERR_NO_DATA, 0);

#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
    PSHAREDCLIPBOARDURITRANSFER pTransfer = SharedClipboardURICtxGetTransfer(&pClient->URI, 0 /* Index*/);
    if (pTransfer)
        vboxSvcClipboardURIAreaDetach(&pClient->State, pTransfer);

    SharedClipboardURICtxDestroy(&pClient->URI);
#endif

    VBoxClipboardSvcImplDisconnect(pClient);

    vboxSvcClipboardClientStateReset(&pClient->State);
    vboxSvcClipboardClientStateDestroy(&pClient->State);

    ClipboardClientMap::iterator itClient = g_mapClients.find(u32ClientID);
    if (itClient != g_mapClients.end())
    {
        g_mapClients.erase(itClient);
    }
    else
        AssertFailed();

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) svcConnect(void *, uint32_t u32ClientID, void *pvClient, uint32_t fRequestor, bool fRestoring)
{
    RT_NOREF(fRequestor, fRestoring);

    PVBOXCLIPBOARDCLIENT pClient = (PVBOXCLIPBOARDCLIENT)pvClient;
    AssertPtr(pvClient);

    /* Assign the client ID. */
    pClient->uClientID = u32ClientID;

    /* Create the client's own event source. */
    VBOXCLIPBOARDEVENTSOURCEID uEventSourceID;
    int rc = vboxSvcClipboardEventSourceCreateID(&uEventSourceID);
    if (RT_SUCCESS(rc))
        rc = SharedClipboardEventSourceCreate(&pClient->Events, uEventSourceID);
    if (RT_SUCCESS(rc))
    {
        LogFlowFunc(("[Client %RU32] Using event source %RU32\n", u32ClientID, pClient->Events.uID));

        /* Reset the client state. */
        vboxSvcClipboardClientStateReset(&pClient->State);

        /* (Re-)initialize the client state. */
        rc = vboxSvcClipboardClientStateInit(&pClient->State, u32ClientID);
        if (RT_SUCCESS(rc))
        {
            rc = VBoxClipboardSvcImplConnect(pClient, VBoxSvcClipboardGetHeadless());
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
            if (RT_SUCCESS(rc))
                rc = SharedClipboardURICtxInit(&pClient->URI);
#endif
            if (RT_SUCCESS(rc))
            {
                VBOXCLIPBOARDCLIENTMAPENTRY ClientEntry;
                RT_ZERO(ClientEntry);

                g_mapClients[u32ClientID] = ClientEntry; /** @todo Handle OOM / collisions? */
            }
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(void) svcCall(void *,
                                  VBOXHGCMCALLHANDLE callHandle,
                                  uint32_t u32ClientID,
                                  void *pvClient,
                                  uint32_t u32Function,
                                  uint32_t cParms,
                                  VBOXHGCMSVCPARM paParms[],
                                  uint64_t tsArrival)
{
    RT_NOREF(u32ClientID, pvClient, tsArrival);

    int rc = VINF_SUCCESS;

    PVBOXCLIPBOARDCLIENT pClient = (PVBOXCLIPBOARDCLIENT)pvClient;
    AssertPtr(pClient);

    LogFunc(("u32ClientID=%RU32 (proto %RU32), fn=%RU32 (%s), cParms=%RU32, paParms=%p\n",
             u32ClientID, pClient->State.uProtocolVer, u32Function, VBoxClipboardGuestMsgToStr(u32Function), cParms, paParms));

#ifdef DEBUG
    uint32_t i;

    for (i = 0; i < cParms; i++)
    {
        /** @todo parameters other than 32 bit */
        LogFunc(("    paParms[%d]: type %RU32 - value %RU32\n", i, paParms[i].type, paParms[i].u.uint32));
    }
#endif

    bool fDefer = false;

    switch (u32Function)
    {
        case VBOX_SHARED_CLIPBOARD_GUEST_FN_GET_HOST_MSG_OLD:
        {
            if (cParms != VBOX_SHARED_CLIPBOARD_CPARMS_GET_HOST_MSG_OLD)
            {
                rc = VERR_INVALID_PARAMETER;
            }
            else if (   paParms[0].type != VBOX_HGCM_SVC_PARM_32BIT  /* msg */
                     || paParms[1].type != VBOX_HGCM_SVC_PARM_32BIT) /* formats */
            {
                rc = VERR_INVALID_PARAMETER;
            }
            else
            {
                /* Atomically verify the client's state. */
                if (VBoxSvcClipboardLock())
                {
                    bool fMessageReturned = vboxSvcClipboardOldReturnMsg(pClient, cParms, paParms);
                    if (fMessageReturned)
                    {
                        /* Just return to the caller. */
                        pClient->State.Old.fAsync = false;
                    }
                    else
                    {
                        /* No event available at the time. Process asynchronously. */
                        fDefer = true;

                        pClient->State.Old.fAsync           = true;
                        pClient->State.Old.async.callHandle = callHandle;
                        pClient->State.Old.async.cParms     = cParms;
                        pClient->State.Old.async.paParms    = paParms;
                    }

                    VBoxSvcClipboardUnlock();
                }
                else
                {
                    rc = VERR_NOT_SUPPORTED;
                }
            }

            break;
        }

        case VBOX_SHARED_CLIPBOARD_GUEST_FN_CONNECT:
        {
            if (cParms != VBOX_SHARED_CLIPBOARD_CPARMS_CONNECT)
            {
                rc = VERR_INVALID_PARAMETER;
            }
            else if (   paParms[0].type != VBOX_HGCM_SVC_PARM_32BIT  /* uProtocolVer */
                     || paParms[1].type != VBOX_HGCM_SVC_PARM_32BIT  /* uProtocolFlags */
                     || paParms[2].type != VBOX_HGCM_SVC_PARM_32BIT  /* cbChunkSize */
                     || paParms[3].type != VBOX_HGCM_SVC_PARM_32BIT  /* enmCompression */
                     || paParms[4].type != VBOX_HGCM_SVC_PARM_32BIT) /* enmChecksumType */
            {
                rc = VERR_INVALID_PARAMETER;
            }
            else if (vboxSvcClipboardGetMode() == VBOX_SHARED_CLIPBOARD_MODE_OFF)
            {
                rc = VERR_ACCESS_DENIED;
            }
            else
            {
                /* Update the protocol version and tell the guest. */
                pClient->State.uProtocolVer = 1;

                LogFlowFunc(("Now using protocol v%RU32\n", pClient->State.uProtocolVer));

                HGCMSvcSetU32(&paParms[0], pClient->State.uProtocolVer);
                HGCMSvcSetU32(&paParms[1], 0 /* Procotol flags, not used yet */);
                HGCMSvcSetU32(&paParms[2], pClient->State.cbChunkSize);
                HGCMSvcSetU32(&paParms[3], 0 /* Compression type, not used yet */);
                HGCMSvcSetU32(&paParms[4], 0 /* Checksum type, not used yet */);

                rc = VINF_SUCCESS;
            }

            break;
        }

        case VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_NOWAIT:
        {
            rc = vboxSvcClipboardMsgPeek(pClient, callHandle, cParms, paParms, false /*fWait*/);
            break;
        }

        case VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_PEEK_WAIT:
        {
            rc = vboxSvcClipboardMsgPeek(pClient, callHandle, cParms, paParms, true /*fWait*/);
            if (rc == VINF_HGCM_ASYNC_EXECUTE)
                fDefer = true;
            break;
        }

        case VBOX_SHARED_CLIPBOARD_GUEST_FN_MSG_GET:
        {
            rc = vboxSvcClipboardMsgGet(pClient, callHandle, cParms, paParms);
            if (RT_SUCCESS(rc)) /* vboxSvcClipboardMsgGet did the completion already. */
                fDefer = true;
            break;
        }

        case VBOX_SHARED_CLIPBOARD_GUEST_FN_FORMATS_WRITE:
        {
            uint32_t u32Formats = 0;

            if (pClient->State.uProtocolVer == 0)
            {
                if (cParms != 1)
                {
                    rc = VERR_INVALID_PARAMETER;
                }
                else if (paParms[0].type != VBOX_HGCM_SVC_PARM_32BIT) /* uFormats */
                {
                    rc = VERR_INVALID_PARAMETER;
                }
                else
                {
                    rc = HGCMSvcGetU32(&paParms[0], &u32Formats);
                }
            }
            else
            {
                if (cParms != 3)
                {
                    rc = VERR_INVALID_PARAMETER;
                }
                else if (   paParms[0].type != VBOX_HGCM_SVC_PARM_32BIT  /* uContextID */
                         || paParms[1].type != VBOX_HGCM_SVC_PARM_32BIT  /* uFormats */
                         || paParms[2].type != VBOX_HGCM_SVC_PARM_32BIT) /* fFlags */
                {
                    rc = VERR_INVALID_PARAMETER;
                }
                else
                {
                    rc = HGCMSvcGetU32(&paParms[1], &u32Formats);

                    /** @todo Handle rest. */
                }
            }

            if (RT_SUCCESS(rc))
            {
                if (   vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_GUEST_TO_HOST
                    && vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_BIDIRECTIONAL)
                {
                    rc = VERR_ACCESS_DENIED;
                }
                else
                {
                    rc = vboxSvcClipboardSetSource(pClient, SHAREDCLIPBOARDSOURCE_REMOTE);
                    if (RT_SUCCESS(rc))
                    {
            #if 0
                        if (u32Formats & VBOX_SHARED_CLIPBOARD_FMT_URI_LIST)
                        {
                            /* Tell the guest that we want to start a reading transfer
                             * (from guest to the host). */
                            rc = vboxSvcClipboardReportMsg(pClient, VBOX_SHARED_CLIPBOARD_HOST_MSG_URI_TRANSFER_START,
                                                           0 /* u32Formats == 0 means reading data */);

                            /* Note: Announcing the actual format will be done in the
                                     host service guest call URI handler (vboxSvcClipboardURIHandler). */
                        }
                        else /* Announce simple formats to the OS-specific service implemenation. */
            #endif /* VBOX_WITH_SHARED_CLIPBOARD_URI_LIST */
                        {
                            if (g_pfnExtension)
                            {
                                VBOXCLIPBOARDEXTPARMS parms;
                                RT_ZERO(parms);
                                parms.u32Format = u32Formats;

                                g_pfnExtension(g_pvExtension, VBOX_CLIPBOARD_EXT_FN_FORMAT_ANNOUNCE, &parms, sizeof (parms));
                            }

                            VBOXCLIPBOARDCLIENTCMDCTX cmdCtx;
                            RT_ZERO(cmdCtx);

                            SHAREDCLIPBOARDFORMATDATA formatData;
                            RT_ZERO(formatData);

                            formatData.uFormats = u32Formats;

                            rc = VBoxClipboardSvcImplFormatAnnounce(pClient, &cmdCtx, &formatData);
                        }
                    }
                }
            }

            break;
        }

        case VBOX_SHARED_CLIPBOARD_GUEST_FN_DATA_READ:
        {
            if (cParms != VBOX_SHARED_CLIPBOARD_CPARMS_READ_DATA)
            {
                rc = VERR_INVALID_PARAMETER;
            }
            else if (   paParms[0].type != VBOX_HGCM_SVC_PARM_32BIT   /* format */
                     || paParms[1].type != VBOX_HGCM_SVC_PARM_PTR     /* ptr */
                     || paParms[2].type != VBOX_HGCM_SVC_PARM_32BIT   /* size */
                    )
            {
                rc = VERR_INVALID_PARAMETER;
            }
            else
            {
                if (   vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_HOST_TO_GUEST
                    && vboxSvcClipboardGetMode() != VBOX_SHARED_CLIPBOARD_MODE_BIDIRECTIONAL)
                {
                    rc = VERR_ACCESS_DENIED;
                    break;
                }

                uint32_t u32Format;
                rc = HGCMSvcGetU32(&paParms[0], &u32Format);
                if (RT_SUCCESS(rc))
                {
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
                    if (u32Format == VBOX_SHARED_CLIPBOARD_FMT_URI_LIST)
                    {
                        if (!SharedClipboardURICtxTransfersMaximumReached(&pClient->URI))
                        {
                            SharedClipboardURICtxTransfersCleanup(&pClient->URI);

                            PSHAREDCLIPBOARDURITRANSFER pTransfer;
                            rc = SharedClipboardURITransferCreate(SHAREDCLIPBOARDURITRANSFERDIR_WRITE,
                                                                  pClient->State.enmSource,
                                                                  &pTransfer);
                            if (RT_SUCCESS(rc))
                            {
                                /* Attach to the most recent clipboard area. */
                                rc = vboxSvcClipboardURIAreaAttach(&pClient->State, pTransfer, 0 /* Area ID */);
                                if (RT_SUCCESS(rc))
                                {
                                    SHAREDCLIPBOARDPROVIDERCREATIONCTX creationCtx;
                                    RT_ZERO(creationCtx);

                                    creationCtx.enmSource = SharedClipboardURITransferGetSource(pTransfer);

                                    RT_ZERO(creationCtx.Interface);

                                    creationCtx.Interface.pfnListHdrWrite    = vboxSvcClipboardURIListHdrWrite;
                                    creationCtx.Interface.pfnListEntryWrite  = vboxSvcClipboardURIListEntryWrite;
                                    creationCtx.Interface.pfnObjWrite        = vboxSvcClipboardURIObjWrite;

                                    creationCtx.pvUser = pClient;

                                    rc = SharedClipboardURITransferSetInterface(pTransfer, &creationCtx);
                                    if (RT_SUCCESS(rc))
                                        rc = SharedClipboardURICtxTransferAdd(&pClient->URI, pTransfer);
                                }

                                if (RT_SUCCESS(rc))
                                {
                                    rc = VBoxClipboardSvcImplURITransferCreate(pClient, pTransfer);
                                }
                                else
                                {
                                    VBoxClipboardSvcImplURITransferDestroy(pClient, pTransfer);
                                    SharedClipboardURITransferDestroy(pTransfer);
                                }
                            }
                        }
                        else
                            rc = VERR_SHCLPB_MAX_TRANSFERS_REACHED;

                        if (RT_FAILURE(rc))
                            LogRel(("Shared Clipboard: Initializing URI host to guest write transfer failed with %Rrc\n", rc));
                    }
                    else
                    {
#endif /* VBOX_WITH_SHARED_CLIPBOARD_URI_LIST */
                        void    *pv;
                        uint32_t cb;
                        rc = HGCMSvcGetBuf(&paParms[1], &pv, &cb);
                        if (RT_SUCCESS(rc))
                        {
                            uint32_t cbActual = 0;

                            if (g_pfnExtension)
                            {
                                VBOXCLIPBOARDEXTPARMS parms;
                                RT_ZERO(parms);

                                parms.u32Format = u32Format;
                                parms.u.pvData  = pv;
                                parms.cbData    = cb;

                                g_fReadingData = true;

                                rc = g_pfnExtension(g_pvExtension, VBOX_CLIPBOARD_EXT_FN_DATA_READ, &parms, sizeof (parms));
                                LogFlowFunc(("DATA: g_fDelayedAnnouncement = %d, g_u32DelayedFormats = 0x%x\n", g_fDelayedAnnouncement, g_u32DelayedFormats));

                                if (g_fDelayedAnnouncement)
                                {
                                    vboxSvcClipboardOldReportMsg(pClient, VBOX_SHARED_CLIPBOARD_HOST_MSG_FORMATS_WRITE, g_u32DelayedFormats);
                                    g_fDelayedAnnouncement = false;
                                    g_u32DelayedFormats = 0;
                                }

                                g_fReadingData = false;

                                if (RT_SUCCESS (rc))
                                {
                                    cbActual = parms.cbData;
                                }
                            }

                            /* Release any other pending read, as we only
                             * support one pending read at one time. */
                            rc = vboxSvcClipboardOldCompleteReadData(pClient, VERR_NO_DATA, 0);
                            if (RT_SUCCESS(rc))
                            {
                                VBOXCLIPBOARDCLIENTCMDCTX cmdCtx;
                                RT_ZERO(cmdCtx);

                                SHAREDCLIPBOARDDATABLOCK dataBlock;
                                RT_ZERO(dataBlock);

                                dataBlock.pvData  = pv;
                                dataBlock.cbData  = cb;
                                dataBlock.uFormat = u32Format;

                                rc = VBoxClipboardSvcImplReadData(pClient, &cmdCtx, &dataBlock, &cbActual);
                            }

                            /* Remember our read request until it is completed.
                             * See the protocol description above for more
                             * information. */
                            if (rc == VINF_HGCM_ASYNC_EXECUTE)
                            {
                                if (VBoxSvcClipboardLock())
                                {
                                    pClient->State.Old.asyncRead.callHandle = callHandle;
                                    pClient->State.Old.asyncRead.cParms     = cParms;
                                    pClient->State.Old.asyncRead.paParms    = paParms;
                                    pClient->State.Old.fReadPending         = true;
                                    fDefer = true;
                                    VBoxSvcClipboardUnlock();
                                }
                                else
                                    rc = VERR_NOT_SUPPORTED;
                            }
                            else if (RT_SUCCESS (rc))
                            {
                                HGCMSvcSetU32(&paParms[2], cbActual);
                            }
                        }
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
                    }
#endif
                }
            }

            break;
        }

        case VBOX_SHARED_CLIPBOARD_GUEST_FN_DATA_WRITE:
        {
            rc = vboxSvcClipboardGetDataWrite(pClient, cParms, paParms);
            break;
        }

        default:
        {
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
            rc = vboxSvcClipboardURIHandler(pClient, callHandle, u32Function, cParms, paParms, tsArrival);

            /* The URI handler does deferring on its own, so never do any call completion here. */
            fDefer = true;
#else
            rc = VERR_NOT_IMPLEMENTED;
#endif
            break;
        }
    }

    LogFlowFunc(("u32ClientID=%RU32, fDefer=%RTbool\n", pClient->uClientID, fDefer));

    if (!fDefer)
        g_pHelpers->pfnCallComplete(callHandle, rc);

    LogFlowFuncLeaveRC(rc);
}

/** If the client in the guest is waiting for a read operation to complete
 * then complete it, otherwise return.  See the protocol description in the
 * shared clipboard module description. */
int vboxSvcClipboardOldCompleteReadData(PVBOXCLIPBOARDCLIENT pClient, int rc, uint32_t cbActual)
{
    VBOXHGCMCALLHANDLE callHandle = NULL;
    VBOXHGCMSVCPARM *paParms = NULL;
    bool fReadPending = false;
    if (VBoxSvcClipboardLock())  /* if not can we do anything useful? */
    {
        callHandle   = pClient->State.Old.asyncRead.callHandle;
        paParms      = pClient->State.Old.asyncRead.paParms;
        fReadPending = pClient->State.Old.fReadPending;
        pClient->State.Old.fReadPending = false;
        VBoxSvcClipboardUnlock();
    }
    if (fReadPending)
    {
        HGCMSvcSetU32(&paParms[2], cbActual);
        g_pHelpers->pfnCallComplete(callHandle, rc);
    }

    return VINF_SUCCESS;
}

/**
 * Initializes a Shared Clipboard service's client state.
 *
 * @returns VBox status code.
 * @param   pClientState        Client state to initialize.
 * @param   uClientID           Client ID (HGCM) to use for this client state.
 */
static int vboxSvcClipboardClientStateInit(PVBOXCLIPBOARDCLIENTSTATE pClientState, uint32_t uClientID)
{
    LogFlowFuncEnter();

    vboxSvcClipboardClientStateReset(pClientState);

    /* Register the client.
     * Note: Do *not* memset the struct, as it contains classes (for caching). */
    pClientState->u32ClientID    = uClientID;
    pClientState->uProtocolVer   = 0;

    return VINF_SUCCESS;
}

/**
 * Destroys a Shared Clipboard service's client state.
 *
 * @returns VBox status code.
 * @param   pClientState        Client state to destroy.
 */
static int vboxSvcClipboardClientStateDestroy(PVBOXCLIPBOARDCLIENTSTATE pClientState)
{
    RT_NOREF(pClientState);

    LogFlowFuncEnter();

    return VINF_SUCCESS;
}

/**
 * Resets a Shared Clipboard service's old client state.
 * Legacy protocol, do not use anymore.
 *
 * @param   pClientState        Client state to reset.
 */
static void vboxSvcClipboardOldClientStateReset(PVBOXCLIPBOARDCLIENTSTATE pClientState)
{
    LogFlowFuncEnter();

    pClientState->Old.fAsync                       = false;
    pClientState->Old.fReadPending                 = false;

    pClientState->Old.fHostMsgQuit                 = false;
    pClientState->Old.fHostMsgReadData             = false;
    pClientState->Old.fHostMsgFormats              = false;

    pClientState->Old.u32AvailableFormats = 0;
    pClientState->Old.u32RequestedFormat  = 0;
}

/**
 * Resets a Shared Clipboard service's client state.
 *
 * @param   pClient         Client state to reset.
 */
static void vboxSvcClipboardClientStateReset(PVBOXCLIPBOARDCLIENTSTATE pClientState)
{
    LogFlowFuncEnter();

#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
    pClientState->URI.fTransferStart = false;
    pClientState->URI.enmTransferDir = SHAREDCLIPBOARDURITRANSFERDIR_UNKNOWN;
#endif

    vboxSvcClipboardOldClientStateReset(pClientState);
}

/*
 * We differentiate between a function handler for the guest and one for the host.
 */
static DECLCALLBACK(int) svcHostCall(void *,
                                     uint32_t u32Function,
                                     uint32_t cParms,
                                     VBOXHGCMSVCPARM paParms[])
{
    int rc = VINF_SUCCESS;

    LogFlowFunc(("u32Function=%RU32, cParms=%RU32, paParms=%p\n", u32Function, cParms, paParms));

    switch (u32Function)
    {
        case VBOX_SHARED_CLIPBOARD_HOST_FN_SET_MODE:
        {
            LogFunc(("VBOX_SHARED_CLIPBOARD_HOST_FN_SET_MODE\n"));

            if (cParms != 1)
            {
                rc = VERR_INVALID_PARAMETER;
            }
            else if (   paParms[0].type != VBOX_HGCM_SVC_PARM_32BIT   /* mode */
                    )
            {
                rc = VERR_INVALID_PARAMETER;
            }
            else
            {
                uint32_t u32Mode = VBOX_SHARED_CLIPBOARD_MODE_OFF;

                rc = HGCMSvcGetU32(&paParms[0], &u32Mode);

                /* The setter takes care of invalid values. */
                vboxSvcClipboardModeSet(u32Mode);
            }
        } break;

        case VBOX_SHARED_CLIPBOARD_HOST_FN_SET_HEADLESS:
        {
            uint32_t u32Headless = g_fHeadless;

            rc = VERR_INVALID_PARAMETER;
            if (cParms != 1)
                break;

            rc = HGCMSvcGetU32(&paParms[0], &u32Headless);
            if (RT_SUCCESS(rc))
                LogFlowFunc(("VBOX_SHARED_CLIPBOARD_HOST_FN_SET_HEADLESS, u32Headless=%u\n",
                            (unsigned) u32Headless));

            g_fHeadless = RT_BOOL(u32Headless);

        } break;

        default:
        {
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
            rc = vboxSvcClipboardURIHostHandler(u32Function, cParms, paParms);
#else
            rc = VERR_NOT_IMPLEMENTED;
#endif
        } break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

#ifndef UNIT_TEST
/**
 * SSM descriptor table for the VBOXCLIPBOARDCLIENTDATA structure.
 */
static SSMFIELD const g_aClipboardClientDataFields[] =
{
    SSMFIELD_ENTRY(VBOXCLIPBOARDCLIENTSTATE, u32ClientID),  /* for validation purposes */
    SSMFIELD_ENTRY(VBOXCLIPBOARDCLIENTSTATE, Old.fHostMsgQuit),
    SSMFIELD_ENTRY(VBOXCLIPBOARDCLIENTSTATE, Old.fHostMsgReadData),
    SSMFIELD_ENTRY(VBOXCLIPBOARDCLIENTSTATE, Old.fHostMsgFormats),
    SSMFIELD_ENTRY(VBOXCLIPBOARDCLIENTSTATE, Old.u32RequestedFormat),
    SSMFIELD_ENTRY_TERM()
};
#endif

static DECLCALLBACK(int) svcSaveState(void *, uint32_t u32ClientID, void *pvClient, PSSMHANDLE pSSM)
{
    RT_NOREF(u32ClientID);

#ifndef UNIT_TEST
    /*
     * When the state will be restored, pending requests will be reissued
     * by VMMDev. The service therefore must save state as if there were no
     * pending request.
     * Pending requests, if any, will be completed in svcDisconnect.
     */
    LogFunc(("u32ClientID=%RU32\n", u32ClientID));

    PVBOXCLIPBOARDCLIENT pClient = (PVBOXCLIPBOARDCLIENT)pvClient;

    /* This field used to be the length. We're using it as a version field
       with the high bit set. */
    SSMR3PutU32(pSSM, UINT32_C(0x80000002));
    int rc = SSMR3PutStructEx(pSSM, pClient, sizeof(*pClient), 0 /*fFlags*/, &g_aClipboardClientDataFields[0], NULL);
    AssertRCReturn(rc, rc);

#else  /* UNIT_TEST */
    RT_NOREF3(u32ClientID, pvClient, pSSM);
#endif /* UNIT_TEST */
    return VINF_SUCCESS;
}

static DECLCALLBACK(int) svcLoadState(void *, uint32_t u32ClientID, void *pvClient, PSSMHANDLE pSSM, uint32_t uVersion)
{
#ifndef UNIT_TEST
    RT_NOREF(u32ClientID, uVersion);

    LogFunc(("u32ClientID=%RU32\n", u32ClientID));

    PVBOXCLIPBOARDCLIENT pClient = (PVBOXCLIPBOARDCLIENT)pvClient;
    AssertPtr(pClient);

    /* Existing client can not be in async state yet. */
    Assert(!pClient->State.Old.fAsync);

    /* Save the client ID for data validation. */
    /** @todo isn't this the same as u32ClientID? Playing safe for now... */
    uint32_t const u32ClientIDOld = pClient->State.u32ClientID;

    /* Restore the client data. */
    uint32_t lenOrVer;
    int rc = SSMR3GetU32(pSSM, &lenOrVer);
    AssertRCReturn(rc, rc);
    if (lenOrVer == UINT32_C(0x80000002))
    {
        rc = SSMR3GetStructEx(pSSM, pClient, sizeof(*pClient), 0 /*fFlags*/, &g_aClipboardClientDataFields[0], NULL);
        AssertRCReturn(rc, rc);
    }
    else
    {
        LogFunc(("Client data size mismatch: got %#x\n", lenOrVer));
        return VERR_SSM_DATA_UNIT_FORMAT_CHANGED;
    }

    /* Verify the client ID. */
    if (pClient->State.u32ClientID != u32ClientIDOld)
    {
        LogFunc(("Client ID mismatch: expected %d, got %d\n", u32ClientIDOld, pClient->State.u32ClientID));
        pClient->State.u32ClientID = u32ClientIDOld;
        return VERR_SSM_DATA_UNIT_FORMAT_CHANGED;
    }

    /* Actual host data are to be reported to guest (SYNC). */
    VBoxClipboardSvcImplSync(pClient);

#else  /* UNIT_TEST*/
    RT_NOREF(u32ClientID, pvClient, pSSM, uVersion);
#endif /* UNIT_TEST */
    return VINF_SUCCESS;
}

static DECLCALLBACK(int) extCallback(uint32_t u32Function, uint32_t u32Format, void *pvData, uint32_t cbData)
{
    RT_NOREF(pvData, cbData);

    LogFlowFunc(("u32Function=%RU32\n", u32Function));

    int rc = VINF_SUCCESS;

    PVBOXCLIPBOARDCLIENT pClient = NULL; /** @todo FIX !!! */

    if (pClient != NULL)
    {
        switch (u32Function)
        {
            case VBOX_CLIPBOARD_EXT_FN_FORMAT_ANNOUNCE:
            {
                LogFlowFunc(("VBOX_CLIPBOARD_EXT_FN_FORMAT_ANNOUNCE: g_fReadingData=%RTbool\n", g_fReadingData));
                if (g_fReadingData)
                {
                    g_fDelayedAnnouncement = true;
                    g_u32DelayedFormats = u32Format;
                }
            #if 0
                else
                {
                    vboxSvcClipboardReportMsg(g_pClient, VBOX_SHARED_CLIPBOARD_HOST_MSG_REPORT_FORMATS, u32Format);
                }
            #endif
            } break;

#if 0
            case VBOX_CLIPBOARD_EXT_FN_DATA_READ:
            {
                vboxSvcClipboardReportMsg(g_pClient, VBOX_SHARED_CLIPBOARD_HOST_MSG_READ_DATA, u32Format);
            } break;
#endif

            default:
                /* Just skip other messages. */
                break;
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) svcRegisterExtension(void *, PFNHGCMSVCEXT pfnExtension, void *pvExtension)
{
    LogFlowFunc(("pfnExtension=%p\n", pfnExtension));

    VBOXCLIPBOARDEXTPARMS parms;
    RT_ZERO(parms);

    if (pfnExtension)
    {
        /* Install extension. */
        g_pfnExtension = pfnExtension;
        g_pvExtension = pvExtension;

        parms.u.pfnCallback = extCallback;
        g_pfnExtension(g_pvExtension, VBOX_CLIPBOARD_EXT_FN_SET_CALLBACK, &parms, sizeof(parms));
    }
    else
    {
        if (g_pfnExtension)
            g_pfnExtension(g_pvExtension, VBOX_CLIPBOARD_EXT_FN_SET_CALLBACK, &parms, sizeof(parms));

        /* Uninstall extension. */
        g_pfnExtension = NULL;
        g_pvExtension = NULL;
    }

    return VINF_SUCCESS;
}

extern "C" DECLCALLBACK(DECLEXPORT(int)) VBoxHGCMSvcLoad(VBOXHGCMSVCFNTABLE *ptable)
{
    int rc = VINF_SUCCESS;

    LogFlowFunc(("ptable=%p\n", ptable));

    if (!ptable)
    {
        rc = VERR_INVALID_PARAMETER;
    }
    else
    {
        LogFunc(("ptable->cbSize = %d, ptable->u32Version = 0x%08X\n", ptable->cbSize, ptable->u32Version));

        if (   ptable->cbSize     != sizeof (VBOXHGCMSVCFNTABLE)
            || ptable->u32Version != VBOX_HGCM_SVC_VERSION)
        {
            rc = VERR_INVALID_PARAMETER;
        }
        else
        {
            g_pHelpers = ptable->pHelpers;

            ptable->cbClient = sizeof(VBOXCLIPBOARDCLIENT);

            ptable->pfnUnload     = svcUnload;
            ptable->pfnConnect    = svcConnect;
            ptable->pfnDisconnect = svcDisconnect;
            ptable->pfnCall       = svcCall;
            ptable->pfnHostCall   = svcHostCall;
            ptable->pfnSaveState  = svcSaveState;
            ptable->pfnLoadState  = svcLoadState;
            ptable->pfnRegisterExtension  = svcRegisterExtension;
            ptable->pfnNotify     = NULL;
            ptable->pvService     = NULL;

            /* Service specific initialization. */
            rc = svcInit();
        }
    }

    return rc;
}
