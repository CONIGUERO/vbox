/* $Id$ */
/** @file
 * VBox audio devices: Mac OS X CoreAudio audio driver.
 */

/*
 * Copyright (C) 2010-2016 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#define LOG_GROUP LOG_GROUP_DRV_HOST_AUDIO
#include <VBox/log.h>
#include <VBox/vmm/pdmaudioifs.h>

#include "DrvAudio.h"
#include "AudioMixBuffer.h"

#include "VBoxDD.h"

#include <iprt/asm.h>
#include <iprt/cdefs.h>
#include <iprt/circbuf.h>
#include <iprt/mem.h>

#include <iprt/uuid.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreServices/CoreServices.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioConverter.h>

/* TODO:
 * - Maybe make sure the threads are immediately stopped if playing/recording stops.
 */

/*
 * Most of this is based on:
 * http://developer.apple.com/mac/library/technotes/tn2004/tn2097.html
 * http://developer.apple.com/mac/library/technotes/tn2002/tn2091.html
 * http://developer.apple.com/mac/library/qa/qa2007/qa1533.html
 * http://developer.apple.com/mac/library/qa/qa2001/qa1317.html
 * http://developer.apple.com/mac/library/documentation/AudioUnit/Reference/AUComponentServicesReference/Reference/reference.html
 */

/**
 * Host Coreaudio driver instance data.
 * @implements PDMIAUDIOCONNECTOR
 */
typedef struct DRVHOSTCOREAUDIO
{
    /** Pointer to the driver instance structure. */
    PPDMDRVINS    pDrvIns;
    /** Pointer to host audio interface. */
    PDMIHOSTAUDIO IHostAudio;
} DRVHOSTCOREAUDIO, *PDRVHOSTCOREAUDIO;

/*******************************************************************************
 *
 * Helper function section
 *
 ******************************************************************************/

static void coreAudioPrintASBDesc(const char *pszDesc, const AudioStreamBasicDescription *pStreamDesc)
{
    char pszSampleRate[32];
    LogRel2(("CoreAudio: %s description:\n", pszDesc));
    LogRel2(("CoreAudio: Format ID: %RU32 (%c%c%c%c)\n", pStreamDesc->mFormatID,
             RT_BYTE4(pStreamDesc->mFormatID), RT_BYTE3(pStreamDesc->mFormatID),
             RT_BYTE2(pStreamDesc->mFormatID), RT_BYTE1(pStreamDesc->mFormatID)));
    LogRel2(("CoreAudio: Flags: %RU32", pStreamDesc->mFormatFlags));
    if (pStreamDesc->mFormatFlags & kAudioFormatFlagIsFloat)
        LogRel2((" Float"));
    if (pStreamDesc->mFormatFlags & kAudioFormatFlagIsBigEndian)
        LogRel2((" BigEndian"));
    if (pStreamDesc->mFormatFlags & kAudioFormatFlagIsSignedInteger)
        LogRel2((" SignedInteger"));
    if (pStreamDesc->mFormatFlags & kAudioFormatFlagIsPacked)
        LogRel2((" Packed"));
    if (pStreamDesc->mFormatFlags & kAudioFormatFlagIsAlignedHigh)
        LogRel2((" AlignedHigh"));
    if (pStreamDesc->mFormatFlags & kAudioFormatFlagIsNonInterleaved)
        LogRel2((" NonInterleaved"));
    if (pStreamDesc->mFormatFlags & kAudioFormatFlagIsNonMixable)
        LogRel2((" NonMixable"));
    if (pStreamDesc->mFormatFlags & kAudioFormatFlagsAreAllClear)
        LogRel2((" AllClear"));
    LogRel2(("\n"));
    snprintf(pszSampleRate, 32, "%.2f", (float)pStreamDesc->mSampleRate); /** @todo r=andy Use RTStrPrint*. */
    LogRel2(("CoreAudio: SampleRate      : %s\n", pszSampleRate));
    LogRel2(("CoreAudio: ChannelsPerFrame: %RU32\n", pStreamDesc->mChannelsPerFrame));
    LogRel2(("CoreAudio: FramesPerPacket : %RU32\n", pStreamDesc->mFramesPerPacket));
    LogRel2(("CoreAudio: BitsPerChannel  : %RU32\n", pStreamDesc->mBitsPerChannel));
    LogRel2(("CoreAudio: BytesPerFrame   : %RU32\n", pStreamDesc->mBytesPerFrame));
    LogRel2(("CoreAudio: BytesPerPacket  : %RU32\n", pStreamDesc->mBytesPerPacket));
}

static void coreAudioPCMInfoToASBDesc(PDMPCMPROPS *pPcmProperties, AudioStreamBasicDescription *pStreamDesc)
{
    pStreamDesc->mFormatID         = kAudioFormatLinearPCM;
    pStreamDesc->mFormatFlags      = kAudioFormatFlagIsPacked;
    pStreamDesc->mFramesPerPacket  = 1;
    pStreamDesc->mSampleRate       = (Float64)pPcmProperties->uHz;
    pStreamDesc->mChannelsPerFrame = pPcmProperties->cChannels;
    pStreamDesc->mBitsPerChannel   = pPcmProperties->cBits;
    if (pPcmProperties->fSigned)
        pStreamDesc->mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    pStreamDesc->mBytesPerFrame    = pStreamDesc->mChannelsPerFrame * (pStreamDesc->mBitsPerChannel / 8);
    pStreamDesc->mBytesPerPacket   = pStreamDesc->mFramesPerPacket * pStreamDesc->mBytesPerFrame;
}

static OSStatus coreAudioSetFrameBufferSize(AudioDeviceID deviceID, bool fInput, UInt32 cReqSize, UInt32 *pcActSize)
{
    AudioObjectPropertyScope propScope = fInput
                                       ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
    AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyBufferFrameSize, propScope,
                                           kAudioObjectPropertyElementMaster };

    /* First try to set the new frame buffer size. */
    OSStatus err = AudioObjectSetPropertyData(deviceID, &propAdr, 0, NULL, sizeof(cReqSize), &cReqSize);

    /* Check if it really was set. */
    UInt32 cSize = sizeof(*pcActSize);
    err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &cSize, pcActSize);
    if (RT_UNLIKELY(err != noErr))
        return err;

    /* If both sizes are the same, we are done. */
    if (cReqSize == *pcActSize)
        return noErr;

    /* If not we have to check the limits of the device. First get the size of
       the buffer size range property. */
    propAdr.mSelector = kAudioDevicePropertyBufferSizeRange;
    err = AudioObjectGetPropertyDataSize(deviceID, &propAdr, 0, NULL, &cSize);
    if (RT_UNLIKELY(err != noErr))
        return err;

    Assert(cSize);
    AudioValueRange *pRange = (AudioValueRange *)RTMemAllocZ(cSize);
    if (pRange)
    {
        err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &cSize, pRange);
        if (err == noErr)
        {
            Float64 cMin = -1;
            Float64 cMax = -1;
            for (size_t a = 0; a < cSize / sizeof(AudioValueRange); a++)
            {
                /* Search for the absolute minimum. */
                if (   pRange[a].mMinimum < cMin
                    || cMin == -1)
                    cMin = pRange[a].mMinimum;

                /* Search for the best maximum which isn't bigger than cReqSize. */
                if (pRange[a].mMaximum < cReqSize)
                {
                    if (pRange[a].mMaximum > cMax)
                        cMax = pRange[a].mMaximum;
                }
            }
            if (cMax == -1)
                cMax = cMin;
            cReqSize = cMax;

            /* First try to set the new frame buffer size. */
            propAdr.mSelector = kAudioDevicePropertyBufferFrameSize;
            err = AudioObjectSetPropertyData(deviceID, &propAdr, 0, NULL, sizeof(cReqSize), &cReqSize);
            if (err == noErr)
            {
                /* Check if it really was set. */
                cSize = sizeof(*pcActSize);
                err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &cSize, pcActSize);
            }
        }

        RTMemFree(pRange);
    }
    else
        err = notEnoughMemoryErr;

    return err;
}

DECL_FORCE_INLINE(bool) coreAudioIsRunning(AudioDeviceID deviceID)
{
    AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMaster };
    UInt32 uFlag = 0;
    UInt32 uSize = sizeof(uFlag);
    OSStatus err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &uSize, &uFlag);
    if (err != kAudioHardwareNoError)
        LogRel(("CoreAudio: Could not determine whether the device is running (%RI32)\n", err));

    return (uFlag >= 1);
}

static int coreAudioCFStringToCString(const CFStringRef pCFString, char **ppszString)
{
    CFIndex cLen = CFStringGetLength(pCFString) + 1;
    char *pszResult = (char *)RTMemAllocZ(cLen * sizeof(char));
    if (!CFStringGetCString(pCFString, pszResult, cLen, kCFStringEncodingUTF8))
    {
        RTMemFree(pszResult);
        return VERR_NOT_FOUND;
    }

    *ppszString = pszResult;
    return VINF_SUCCESS;
}

static AudioDeviceID coreAudioDeviceUIDtoID(const char* pszUID)
{
    /* Create a CFString out of our CString. */
    CFStringRef strUID = CFStringCreateWithCString(NULL, pszUID, kCFStringEncodingMacRoman);

    /* Fill the translation structure. */
    AudioDeviceID deviceID;

    AudioValueTranslation translation;
    translation.mInputData      = &strUID;
    translation.mInputDataSize  = sizeof(CFStringRef);
    translation.mOutputData     = &deviceID;
    translation.mOutputDataSize = sizeof(AudioDeviceID);

    /* Fetch the translation from the UID to the device ID. */
    AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMaster };

    UInt32 uSize = sizeof(AudioValueTranslation);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdr, 0, NULL, &uSize, &translation);

    /* Release the temporary CFString */
    CFRelease(strUID);

    if (RT_LIKELY(err == noErr))
        return deviceID;

    /* Return the unknown device on error. */
    return kAudioDeviceUnknown;
}

/*******************************************************************************
 *
 * Global structures section
 *
 ******************************************************************************/

/* Initialization status indicator used for the recreation of the AudioUnits. */
#define CA_STATUS_UNINIT    UINT32_C(0) /* The device is uninitialized */
#define CA_STATUS_IN_INIT   UINT32_C(1) /* The device is currently initializing */
#define CA_STATUS_INIT      UINT32_C(2) /* The device is initialized */
#define CA_STATUS_IN_UNINIT UINT32_C(3) /* The device is currently uninitializing */
#define CA_STATUS_REINIT    UINT32_C(4) /* The device has to be reinitialized */

/* Error code which indicates "End of data" */
static const OSStatus caConverterEOFDErr = 0x656F6664; /* 'eofd' */

/* Prototypes needed for COREAUDIOSTREAMCBCTX. */
struct COREAUDIOSTREAMIN;
typedef struct COREAUDIOSTREAMIN *PCOREAUDIOSTREAMIN;
struct COREAUDIOSTREAMOUT;
typedef struct COREAUDIOSTREAMOUT *PCOREAUDIOSTREAMOUT;

/**
 * Simple structure for maintaining a stream's callback context.
 ** @todo Remove this as soon as we have unified input/output streams in this backend.
 */
typedef struct COREAUDIOSTREAMCBCTX
{
    /** Pointer to driver instance. */
    PDRVHOSTCOREAUDIO       pThis;
    /** The stream's direction. */
    PDMAUDIODIR             enmDir;
    union
    {
        /** Pointer to self, if it's an input stream. */
        PCOREAUDIOSTREAMIN  pIn;
        /** Pointer to self, if it's an output stream. */
        PCOREAUDIOSTREAMOUT pOut;
    };
} COREAUDIOSTREAMCBCTX, *PCOREAUDIOSTREAMCBCTX;

/** @todo Unify COREAUDIOSTREAMOUT / COREAUDIOSTREAMIN. */
typedef struct COREAUDIOSTREAMOUT
{
    /** Host output stream.
     *  Note: Always must come first in this structure! */
    PDMAUDIOSTREAM              Stream;
    /** Stream description which is default on the device. */
    AudioStreamBasicDescription deviceFormat;
    /** Stream description which is selected for using with VBox. */
    AudioStreamBasicDescription streamFormat;
    /** The audio device ID of the currently used device. */
    AudioDeviceID               deviceID;
    /** The AudioUnit being used. */
    AudioUnit                   audioUnit;
    /** A ring buffer for transferring data to the playback thread. */
    PRTCIRCBUF                  pBuf;
    /** Initialization status tracker. Used when some of the device parameters
     *  or the device itself is changed during the runtime. */
    volatile uint32_t           status;
    /** Flag whether the "default device changed" listener was registered. */
    bool                        fDefDevChgListReg;
    /** Flag whether the "device state changed" listener was registered. */
    bool                        fDevStateChgListReg;
    /** Callback context for this stream for handing this stream in to
     *  a CoreAudio callback.
     ** @todo Remove this as soon as we have unified input/output streams in this backend. */
    COREAUDIOSTREAMCBCTX        cbCtx;
} COREAUDIOSTREAMOUT, *PCOREAUDIOSTREAMOUT;

typedef struct COREAUDIOSTREAMIN
{
    /** Host input stream.
     *  Note: Always must come first in this structure! */
    PDMAUDIOSTREAM              Stream;
    /** Stream description which is default on the device. */
    AudioStreamBasicDescription deviceFormat;
    /** Stream description which is selected for using with VBox. */
    AudioStreamBasicDescription streamFormat;
    /** The audio device ID of the currently used device. */
    AudioDeviceID               deviceID;
    /** The AudioUnit used. */
    AudioUnit                   audioUnit;
    /** The audio converter if necessary. */
    AudioConverterRef           pConverter;
    /** Native buffer used for render the audio data in the recording thread. */
    AudioBufferList             bufferList;
    /** Reading offset for the bufferList's buffer. */
    uint32_t                    offBufferRead;
    /** The ratio between the device & the stream sample rate. */
    Float64                     sampleRatio;
    /** A ring buffer for transferring data from the recording thread. */
    PRTCIRCBUF                  pBuf;
    /** Initialization status tracker. Used when some of the device parameters
     *  or the device itself is changed during the runtime. */
    volatile uint32_t           status;
    /** Flag whether the "default device changed" listener was registered. */
    bool                        fDefDevChgListReg;
    /** Flag whether the "device state changed" listener was registered. */
    bool                        fDevStateChgListReg;
    /** Callback context for this stream for handing this stream in to
     *  a CoreAudio callback.
     ** @todo Remove this as soon as we have unified input/output streams in this backend. */
    COREAUDIOSTREAMCBCTX        cbCtx;
} COREAUDIOSTREAMIN, *PCOREAUDIOSTREAMIN;


static int coreAudioInitIn(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream, uint32_t *pcSamples);
static int coreAudioInitOut(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream, uint32_t *pcSamples);
static int coreAudioControlStreamIn(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream, PDMAUDIOSTREAMCMD enmStreamCmd);
static int coreAudioControlStreamOut(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream, PDMAUDIOSTREAMCMD enmStreamCmd);
static int coreAudioCreateStreamIn(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream, PPDMAUDIOSTREAMCFG pCfg, uint32_t *pcSamples);
static int coreAudioCreateStreamOut(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream, PPDMAUDIOSTREAMCFG pCfg, uint32_t *pcSamples);
static int coreAudioDestroyStreamIn(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream);
static int coreAudioDestroyStreamOut(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream);

static OSStatus coreAudioPlaybackAudioDevicePropertyChanged(AudioObjectID propertyID, UInt32 nAddresses, const AudioObjectPropertyAddress properties[], void *pvUser);
static OSStatus coreAudioPlaybackCb(void *pvUser, AudioUnitRenderActionFlags *pActionFlags, const AudioTimeStamp *pAudioTS, UInt32 uBusID, UInt32 cFrames, AudioBufferList* pBufData);

/**
 * Does a (Re-)enumeration of the host's playback + capturing devices.
 *
 * @return  IPRT status code.
 * @param   pThis               Host audio driver instance.
 * @param   pCfg                Where to store the enumeration results.
 * @param   fEnum               Enumeration flags.
 */
static int coreAudioDevicesEnumerate(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOBACKENDCFG pCfg, bool fIn, uint32_t fEnum)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    /* pCfg is optional. */

    int rc = VINF_SUCCESS;

    uint8_t cDevs = 0;

    do
    {
        AudioObjectPropertyAddress propAdrDevList = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                                      kAudioObjectPropertyElementMaster };
        UInt32 uSize = 0;
        OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propAdrDevList, 0, NULL, &uSize);
        if (err != kAudioHardwareNoError)
            break;

        AudioDeviceID *pDevIDs = (AudioDeviceID *)alloca(uSize);
        if (pDevIDs == NULL)
            break;

        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdrDevList, 0, NULL, &uSize, pDevIDs);
        if (err != kAudioHardwareNoError)
            break;

        UInt32 cDevices = uSize / sizeof (AudioDeviceID);
        for (UInt32 i = 0; i < cDevices; i++)
        {
            AudioDeviceID curDevID = pDevIDs[i];

            /* Check if the device is valid. */
            AudioObjectPropertyAddress propAddrCfg = { kAudioDevicePropertyStreamConfiguration,
                                                       fIn ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                                                       kAudioObjectPropertyElementMaster };

            err = AudioObjectGetPropertyDataSize(curDevID, &propAddrCfg, 0, NULL, &uSize);
            if (err != noErr)
                continue;

            AudioBufferList *pBufList = (AudioBufferList *)RTMemAlloc(uSize);
            if (!pBufList)
                continue;

            bool fIsValid = false;

            err = AudioObjectGetPropertyData(curDevID, &propAddrCfg, 0, NULL, &uSize, pBufList);
            if (err == noErr)
            {
                for (UInt32 a = 0; a < pBufList->mNumberBuffers; a++)
                {
                    fIsValid = pBufList->mBuffers[a].mNumberChannels > 0;
                    if (fIsValid)
                        break;
                }
            }

            if (pBufList)
            {
                RTMemFree(pBufList);
                pBufList = NULL;
            }

            if (!fIsValid)
                continue;

            /* Resolve the device's name. */
            AudioObjectPropertyAddress propAddrName = { kAudioObjectPropertyName,
                                                        fIn ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                                                        kAudioObjectPropertyElementMaster };
            uSize = sizeof(CFStringRef);
            CFStringRef pcfstrName = NULL;

            err = AudioObjectGetPropertyData(curDevID, &propAddrName, 0, NULL, &uSize, &pcfstrName);
            if (err != kAudioHardwareNoError)
                continue;

            CFIndex uMax = CFStringGetMaximumSizeForEncoding(CFStringGetLength(pcfstrName), kCFStringEncodingUTF8) + 1;
            if (uMax)
            {
                char *pszName = (char *)RTStrAlloc(uMax);
                if (   pszName
                    && CFStringGetCString(pcfstrName, pszName, uMax, kCFStringEncodingUTF8))
                {
                    LogRel2(("CoreAudio: Found %s device '%s'\n", fIn ? "recording" : "playback", pszName));
                    cDevs++;
                }

                if (pszName)
                {
                    RTStrFree(pszName);
                    pszName = NULL;
                }
            }

            CFRelease(pcfstrName);
        }

    } while (0);

    if (fIn)
        LogRel2(("CoreAudio: Found %RU8 capturing device(s)\n", cDevs));
    else
        LogRel2(("CoreAudio: Found %RU8 playback device(s)\n", cDevs));

    if (pCfg)
    {
        if (fIn)
            pCfg->cSources = cDevs;
        else
            pCfg->cSinks   = cDevs;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Updates this host driver's internal status, according to the global, overall input/output
 * state and all connected (native) audio streams.
 *
 * @param   pThis               Host audio driver instance.
 * @param   pCfg                Where to store the backend configuration. Optional.
 * @param   fEnum               Enumeration flags.
 */
int coreAudioUpdateStatusInternalEx(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOBACKENDCFG pCfg, uint32_t fEnum)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    /* pCfg is optional. */

    PDMAUDIOBACKENDCFG Cfg;
    RT_ZERO(Cfg);

    Cfg.cbStreamOut    = sizeof(COREAUDIOSTREAMOUT);
    Cfg.cbStreamIn     = sizeof(COREAUDIOSTREAMIN);
    Cfg.cMaxStreamsIn  = UINT32_MAX;
    Cfg.cMaxStreamsOut = UINT32_MAX;

    int rc = coreAudioDevicesEnumerate(pThis, &Cfg, false /* fIn */, 0 /* fEnum */);
    AssertRC(rc);
    rc = coreAudioDevicesEnumerate(pThis, &Cfg, true /* fIn */, 0 /* fEnum */);
    AssertRC(rc);

    if (pCfg)
        memcpy(pCfg, &Cfg, sizeof(PDMAUDIOBACKENDCFG));

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(OSStatus) drvHostCoreAudioDeviceStateChanged(AudioObjectID propertyID,
                                                                 UInt32 nAddresses,
                                                                 const AudioObjectPropertyAddress properties[],
                                                                 void *pvUser)
{
    LogFlowFunc(("propertyID=%u nAddresses=%u pvUser=%p\n", propertyID, nAddresses, pvUser));

    PCOREAUDIOSTREAMCBCTX pCbCtx = (PCOREAUDIOSTREAMCBCTX)pvUser;
    AssertPtr(pCbCtx);

    UInt32 uAlive = 1;
    UInt32 uSize  = sizeof(UInt32);

    AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMaster };

    AudioDeviceID deviceID = pCbCtx->enmDir == PDMAUDIODIR_IN
                           ? pCbCtx->pIn->deviceID : pCbCtx->pOut->deviceID;

    OSStatus err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &uSize, &uAlive);

    bool fIsDead = false;

    if (err == kAudioHardwareBadDeviceError)
        fIsDead = true; /* Unplugged. */
    else if ((err == kAudioHardwareNoError) && (!RT_BOOL(uAlive)))
        fIsDead = true; /* Something else happened. */

    if (fIsDead)
    {
        switch (pCbCtx->enmDir)
        {
            case PDMAUDIODIR_IN:
            {
                PCOREAUDIOSTREAMIN pStreamIn = pCbCtx->pIn;

                /* We move the reinitialization to the next output event.
                 * This make sure this thread isn't blocked and the
                 * reinitialization is done when necessary only. */
                ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_REINIT);

                LogRel(("CoreAudio: Capturing device stopped functioning\n"));
                break;
            }

            case PDMAUDIODIR_OUT:
            {
                PCOREAUDIOSTREAMOUT pStreamOut = pCbCtx->pOut;

                /* We move the reinitialization to the next output event.
                 * This make sure this thread isn't blocked and the
                 * reinitialization is done when necessary only. */
                ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_REINIT);

                LogRel(("CoreAudio: Playback device stopped functioning\n"));
                break;
            }

            default:
                AssertMsgFailed(("Not implemented\n"));
                break;
        }
    }

    int rc2 = coreAudioDevicesEnumerate(pCbCtx->pThis, NULL /* pCfg */, false /* fIn */, 0 /* fEnum */);
    AssertRC(rc2);
    rc2 = coreAudioDevicesEnumerate(pCbCtx->pThis, NULL /* pCfg */, true /* fIn */, 0 /* fEnum */);
    AssertRC(rc2);

    return noErr;
}

/* Callback for getting notified when the default recording/playback device has been changed. */
static DECLCALLBACK(OSStatus) coreAudioDefaultDeviceChanged(AudioObjectID propertyID,
                                                            UInt32 nAddresses,
                                                            const AudioObjectPropertyAddress properties[],
                                                            void *pvUser)
{
    OSStatus err = noErr;

    LogFlowFunc(("propertyID=%u nAddresses=%u pvUser=%p\n", propertyID, nAddresses, pvUser));

    PCOREAUDIOSTREAMCBCTX pCbCtx = (PCOREAUDIOSTREAMCBCTX)pvUser;
    AssertPtr(pCbCtx);

    for (UInt32 idxAddress = 0; idxAddress < nAddresses; idxAddress++)
    {
        const AudioObjectPropertyAddress *pProperty = &properties[idxAddress];

        switch (pProperty->mSelector)
        {
            case kAudioHardwarePropertyDefaultInputDevice:
            {
                PCOREAUDIOSTREAMIN pStreamIn = pCbCtx->pIn;
                AssertPtr(pStreamIn);

                /* This listener is called on every change of the hardware
                 * device. So check if the default device has really changed. */
                UInt32 uSize = sizeof(pStreamIn->deviceID);
                UInt32 uResp;
                err = AudioObjectGetPropertyData(kAudioObjectSystemObject, pProperty, 0, NULL, &uSize, &uResp);
                if (err == noErr)
                {
                    if (pStreamIn->deviceID != uResp)
                    {
                        LogRel2(("CoreAudio: Default device for recording has changed\n"));

                        /* We move the reinitialization to the next input event.
                         * This make sure this thread isn't blocked and the
                         * reinitialization is done when necessary only. */
                        ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_REINIT);
                    }
                }
                break;
            }

            case kAudioHardwarePropertyDefaultOutputDevice:
            {
                PCOREAUDIOSTREAMOUT pStreamOut = pCbCtx->pOut;
                AssertPtr(pStreamOut);

                /* This listener is called on every change of the hardware
                 * device. So check if the default device has really changed. */
                AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultOutputDevice,
                                                       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };

                UInt32 uSize = sizeof(pStreamOut->deviceID);
                UInt32 uResp;
                err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdr, 0, NULL, &uSize, &uResp);
                if (err == noErr)
                {
                    if (pStreamOut->deviceID != uResp)
                    {
                        LogRel2(("CoreAudio: Default device for playback has changed\n"));

                        /* We move the reinitialization to the next input event.
                         * This make sure this thread isn't blocked and the
                         * reinitialization is done when necessary only. */
                        ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_REINIT);
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    int rc2 = coreAudioDevicesEnumerate(pCbCtx->pThis, NULL /* pCfg */, false /* fIn */, 0 /* fEnum */);
    AssertRC(rc2);
    rc2 = coreAudioDevicesEnumerate(pCbCtx->pThis, NULL /* pCfg */, true /* fIn */, 0 /* fEnum */);
    AssertRC(rc2);

    /** @todo Implement callback notification here to let the audio connector / device emulation
     *        know that something has changed. */

    return noErr;
}

/* Callback for getting notified when some of the properties of an audio device has changed. */
static DECLCALLBACK(OSStatus) coreAudioRecordingAudioDevicePropertyChanged(AudioObjectID                     propertyID,
                                                                           UInt32                            cAdresses,
                                                                           const AudioObjectPropertyAddress  aProperties[],
                                                                           void                             *pvUser)
{
    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pvUser;

    switch (propertyID)
    {
#ifdef DEBUG
        case kAudioDeviceProcessorOverload:
        {
            LogFunc(("Processor overload detected!\n"));
            break;
        }
#endif /* DEBUG */
        case kAudioDevicePropertyNominalSampleRate:
        {
            LogRel(("CoreAudio: Recording sample rate changed\n"));

            /* We move the reinitialization to the next input event.
             * This make sure this thread isn't blocked and the
             * reinitialization is done when necessary only. */
            ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_REINIT);
            break;
        }

        default:
            break;
    }

    return noErr;
}

/* Callback to convert audio input data from one format to another. */
static DECLCALLBACK(OSStatus) coreAudioConverterCb(AudioConverterRef              converterID,
                                                   UInt32                        *pcPackets,
                                                   AudioBufferList               *pBufData,
                                                   AudioStreamPacketDescription **ppPacketDesc,
                                                   void                          *pvUser)
{
    /** @todo Check incoming pointers. */

    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pvUser;

    /** @todo Check converter ID? */

    const AudioBufferList *pBufferList = &pStreamIn->bufferList;

    if (ASMAtomicReadU32(&pStreamIn->status) != CA_STATUS_INIT)
        return noErr;

    /** @todo In principle we had to check here if the source is non interleaved, and if so,
     *        so go through all buffers not only the first one like now. */

    /* Use the lower one of the packets to process & the available packets in the buffer. */
    Assert(pBufferList->mBuffers[0].mDataByteSize >= pStreamIn->offBufferRead);
    UInt32 cSize = RT_MIN(*pcPackets * pStreamIn->deviceFormat.mBytesPerPacket,
                          pBufferList->mBuffers[0].mDataByteSize - pStreamIn->offBufferRead);

    /* Set the new size on output, so the caller know what we have processed. */
    Assert(pStreamIn->deviceFormat.mBytesPerPacket);
    *pcPackets = cSize / pStreamIn->deviceFormat.mBytesPerPacket;

    OSStatus err;

    /* If no data is available anymore we return with an error code. This error code will be returned
     * from AudioConverterFillComplexBuffer. */
    if (*pcPackets == 0)
    {
        pBufData->mBuffers[0].mDataByteSize = 0;
        pBufData->mBuffers[0].mData         = NULL;

        err = caConverterEOFDErr;
    }
    else
    {
        pBufData->mBuffers[0].mNumberChannels = pBufferList->mBuffers[0].mNumberChannels;
        pBufData->mBuffers[0].mDataByteSize   = cSize;
        pBufData->mBuffers[0].mData           = (uint8_t *)pBufferList->mBuffers[0].mData + pStreamIn->offBufferRead;

        pStreamIn->offBufferRead += cSize;

        err = noErr;
    }

    return err;
}

/* Callback to feed audio input buffer. */
static DECLCALLBACK(OSStatus) coreAudioRecordingCb(void *pvUser,
                                                   AudioUnitRenderActionFlags *pActionFlags,
                                                   const AudioTimeStamp       *pAudioTS,
                                                   UInt32                      uBusID,
                                                   UInt32                      cFrames,
                                                   AudioBufferList            *pBufData)
{
    PCOREAUDIOSTREAMIN pStreamIn  = (PCOREAUDIOSTREAMIN)pvUser;
    PPDMAUDIOSTREAM pStream = &pStreamIn->Stream;

    if (ASMAtomicReadU32(&pStreamIn->status) != CA_STATUS_INIT)
        return noErr;

    /* If nothing is pending return immediately. */
    if (cFrames == 0)
        return noErr;

    OSStatus err = noErr;
    int rc = VINF_SUCCESS;

    do
    {
        /* Are we using a converter? */
        if (pStreamIn->pConverter)
        {
            /* First, render the data as usual. */
            pStreamIn->bufferList.mBuffers[0].mNumberChannels = pStreamIn->deviceFormat.mChannelsPerFrame;
            pStreamIn->bufferList.mBuffers[0].mDataByteSize   = pStreamIn->deviceFormat.mBytesPerFrame * cFrames;
            AssertBreakStmt(pStreamIn->bufferList.mBuffers[0].mDataByteSize, rc = VERR_INVALID_PARAMETER);
            pStreamIn->bufferList.mBuffers[0].mData           = RTMemAlloc(pStreamIn->bufferList.mBuffers[0].mDataByteSize);
            if (!pStreamIn->bufferList.mBuffers[0].mData)
            {
                rc = VERR_NO_MEMORY;
                break;
            }

            err = AudioUnitRender(pStreamIn->audioUnit, pActionFlags, pAudioTS, uBusID, cFrames, &pStreamIn->bufferList);
            if (err != noErr)
            {
                LogRel2(("CoreAudio: Failed rendering converted audio input data (%RI32)\n", err));
                rc = VERR_IO_GEN_FAILURE; /** @todo Improve this. */
                break;
            }

            size_t cbAvail = RT_MIN(RTCircBufFree(pStreamIn->pBuf), pStreamIn->bufferList.mBuffers[0].mDataByteSize);

            /* Initialize the temporary output buffer */
            AudioBufferList tmpList;
            tmpList.mNumberBuffers = 1;
            tmpList.mBuffers[0].mNumberChannels = pStreamIn->streamFormat.mChannelsPerFrame;

            /* Set the read position to zero. */
            pStreamIn->offBufferRead = 0;

            /* Iterate as long as data is available. */
            uint8_t *puDst = NULL;
            while (cbAvail)
            {
                /* Try to acquire the necessary space from the ring buffer. */
                size_t cbToWrite = 0;
                RTCircBufAcquireWriteBlock(pStreamIn->pBuf, cbAvail, (void **)&puDst, &cbToWrite);
                if (!cbToWrite)
                    break;

                /* Now set how much space is available for output. */
                Assert(pStreamIn->streamFormat.mBytesPerPacket);

                UInt32 ioOutputDataPacketSize = cbToWrite / pStreamIn->streamFormat.mBytesPerPacket;

                /* Set our ring buffer as target. */
                tmpList.mBuffers[0].mDataByteSize = cbToWrite;
                tmpList.mBuffers[0].mData         = puDst;

                AudioConverterReset(pStreamIn->pConverter);

                err = AudioConverterFillComplexBuffer(pStreamIn->pConverter, coreAudioConverterCb, pStreamIn,
                                                      &ioOutputDataPacketSize, &tmpList, NULL);
                if(   err != noErr
                   && err != caConverterEOFDErr)
                {
                    LogFunc(("Failed to convert audio data (%RI32:%c%c%c%c)\n", err,
                             RT_BYTE4(err), RT_BYTE3(err), RT_BYTE2(err), RT_BYTE1(err)));
                    rc = VERR_IO_GEN_FAILURE;
                    break;
                }

                /* Check in any case what processed size is returned. It could be less than we expected. */
                cbToWrite = ioOutputDataPacketSize * pStreamIn->streamFormat.mBytesPerPacket;

                Log3Func(("cbToWrite=%RU32\n", cbToWrite));

                /* Release the ring buffer, so the main thread could start reading this data. */
                RTCircBufReleaseWriteBlock(pStreamIn->pBuf, cbToWrite);

                /* If the error is "End of Data" it means there is no data anymore
                 * which could be converted. So end here now. */
                if (err == caConverterEOFDErr)
                    break;

                Assert(cbAvail >= cbToWrite);
                cbAvail -= cbToWrite;
            }
        }
        else /* No converter being used. */
        {
            AssertBreakStmt(pStreamIn->streamFormat.mChannelsPerFrame >= 1,    rc = VERR_INVALID_PARAMETER);
            AssertBreakStmt(pStreamIn->streamFormat.mBytesPerFrame >= 1,       rc = VERR_INVALID_PARAMETER);

            AssertBreakStmt(pStreamIn->bufferList.mNumberBuffers >= 1,         rc = VERR_INVALID_PARAMETER);
            AssertBreakStmt(pStreamIn->bufferList.mBuffers[0].mNumberChannels, rc = VERR_INVALID_PARAMETER);

            pStreamIn->bufferList.mBuffers[0].mNumberChannels = pStreamIn->streamFormat.mChannelsPerFrame;
            pStreamIn->bufferList.mBuffers[0].mDataByteSize   = pStreamIn->streamFormat.mBytesPerFrame * cFrames;
            pStreamIn->bufferList.mBuffers[0].mData           = RTMemAlloc(pStreamIn->bufferList.mBuffers[0].mDataByteSize);
            if (!pStreamIn->bufferList.mBuffers[0].mData)
            {
                rc = VERR_NO_MEMORY;
                break;
            }

            err = AudioUnitRender(pStreamIn->audioUnit, pActionFlags, pAudioTS, uBusID, cFrames, &pStreamIn->bufferList);
            if (err != noErr)
            {
                LogRel2(("CoreAudio: Failed rendering non-coverted audio input data (%RI32)\n", err));
                rc = VERR_IO_GEN_FAILURE; /** @todo Improve this. */
                break;
            }

            const uint32_t cbDataSize = pStreamIn->bufferList.mBuffers[0].mDataByteSize;
            const size_t   cbBufFree  = RTCircBufFree(pStreamIn->pBuf);
                  size_t   cbAvail    = RT_MIN(cbDataSize, cbBufFree);

            Log3Func(("cbDataSize=%RU32, cbBufFree=%zu, cbAvail=%zu\n", cbDataSize, cbBufFree, cbAvail));

            /* Iterate as long as data is available. */
            uint8_t *puDst = NULL;
            uint32_t cbWrittenTotal = 0;
            while (cbAvail)
            {
                /* Try to acquire the necessary space from the ring buffer. */
                size_t cbToWrite = 0;
                RTCircBufAcquireWriteBlock(pStreamIn->pBuf, cbAvail, (void **)&puDst, &cbToWrite);
                if (!cbToWrite)
                    break;

                /* Copy the data from the Core Audio buffer to the ring buffer. */
                memcpy(puDst, (uint8_t *)pStreamIn->bufferList.mBuffers[0].mData + cbWrittenTotal, cbToWrite);

                /* Release the ring buffer, so the main thread could start reading this data. */
                RTCircBufReleaseWriteBlock(pStreamIn->pBuf, cbToWrite);

                cbWrittenTotal += cbToWrite;

                Assert(cbAvail >= cbToWrite);
                cbAvail -= cbToWrite;
            }

            Log3Func(("cbWrittenTotal=%RU32, cbLeft=%zu\n", cbWrittenTotal, cbAvail));
        }

    } while (0);

    if (pStreamIn->bufferList.mBuffers[0].mData)
    {
        RTMemFree(pStreamIn->bufferList.mBuffers[0].mData);
        pStreamIn->bufferList.mBuffers[0].mData = NULL;
    }

    if (RT_FAILURE(rc))
        LogFunc(("Failed with rc=%Rrc\n", rc));

    return err;
}

# define CA_BREAK_STMT(stmt) \
    stmt; \
    break;

/** @todo Eventually split up this function, as this already is huge! */
static int coreAudioInitIn(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream, uint32_t *pcSamples)
{
    int rc = VINF_SUCCESS;

    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;
    UInt32 cSamples = 0;

    do
    {
        ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_IN_INIT);

        OSStatus err;

        UInt32 uSize = 0;
        if (pStreamIn->deviceID == kAudioDeviceUnknown)
        {
            /* Fetch the default audio recording device currently in use. */
            AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultInputDevice,
                                                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
            uSize = sizeof(pStreamIn->deviceID);
            err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdr, 0, NULL, &uSize, &pStreamIn->deviceID);
            if (err != noErr)
            {
                LogRel2(("CoreAudio: Unable to determine default recording device (%RI32)\n", err));
                return VERR_AUDIO_NO_FREE_INPUT_STREAMS;
            }
        }

        if (pStreamIn->deviceID == kAudioDeviceUnknown)
        {
            LogFlowFunc(("No default recording device found\n"));
            return VERR_AUDIO_NO_FREE_INPUT_STREAMS;
        }

        /*
         * Try to get the name of the recording device and log it. It's not fatal if it fails.
         */
        CFStringRef strTemp;

        AudioObjectPropertyAddress propAdr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMaster };
        uSize = sizeof(CFStringRef);
        err = AudioObjectGetPropertyData(pStreamIn->deviceID, &propAdr, 0, NULL, &uSize, &strTemp);
        if (err == noErr)
        {
            char *pszDevName = NULL;
            err = coreAudioCFStringToCString(strTemp, &pszDevName);
            if (err == noErr)
            {
                CFRelease(strTemp);

                /* Get the device' UUID. */
                propAdr.mSelector = kAudioDevicePropertyDeviceUID;
                err = AudioObjectGetPropertyData(pStreamIn->deviceID, &propAdr, 0, NULL, &uSize, &strTemp);
                if (err == noErr)
                {
                    char *pszUID = NULL;
                    err = coreAudioCFStringToCString(strTemp, &pszUID);
                    if (err == noErr)
                    {
                        CFRelease(strTemp);
                        LogRel(("CoreAudio: Using recording device: %s (UID: %s)\n", pszDevName, pszUID));

                        RTMemFree(pszUID);
                    }
                }

                RTMemFree(pszDevName);
            }
        }
        else
        {
            /* This is not fatal, can happen for some Macs. */
            LogRel2(("CoreAudio: Unable to determine recording device name (%RI32)\n", err));
        }

        /* Get the default frames buffer size, so that we can setup our internal buffers. */
        UInt32 cFrames;
        uSize = sizeof(cFrames);
        propAdr.mSelector = kAudioDevicePropertyBufferFrameSize;
        propAdr.mScope    = kAudioDevicePropertyScopeInput;
        err = AudioObjectGetPropertyData(pStreamIn->deviceID, &propAdr, 0, NULL, &uSize, &cFrames);
        if (err != noErr)
        {
            /* Can happen if no recording device is available by default. Happens on some Macs,
             * so don't log this by default to not scare people. */
            LogRel2(("CoreAudio: Failed to determine frame buffer size of the audio recording device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Set the frame buffer size and honor any minimum/maximum restrictions on the device. */
        err = coreAudioSetFrameBufferSize(pStreamIn->deviceID, true /* fInput */, cFrames, &cFrames);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set frame buffer size for the audio recording device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        LogFlowFunc(("cFrames=%RU32\n", cFrames));

        ComponentDescription cd;
        RT_ZERO(cd);
        cd.componentType         = kAudioUnitType_Output;
        cd.componentSubType      = kAudioUnitSubType_HALOutput;
        cd.componentManufacturer = kAudioUnitManufacturer_Apple;

        /* Try to find the default HAL output component. */
        Component cp = FindNextComponent(NULL, &cd);
        if (cp == 0)
        {
            LogRel(("CoreAudio: Failed to find HAL output component\n")); /** @todo Return error value? */
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Open the default HAL output component. */
        err = OpenAComponent(cp, &pStreamIn->audioUnit);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to open output component (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Switch the I/O mode for input to on. */
        UInt32 uFlag = 1;
        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                                   1, &uFlag, sizeof(uFlag));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to disable input I/O mode for input stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Switch the I/O mode for output to off. This is important, as this is a pure input stream. */
        uFlag = 0;
        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                                   0, &uFlag, sizeof(uFlag));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to disable output I/O mode for input stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Set the default audio recording device as the device for the new AudioUnit. */
        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                                   0, &pStreamIn->deviceID, sizeof(pStreamIn->deviceID));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set current device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /*
         * CoreAudio will inform us on a second thread for new incoming audio data.
         * Therefore register a callback function which will process the new data.
         */
        AURenderCallbackStruct cb;
        RT_ZERO(cb);
        cb.inputProc       = coreAudioRecordingCb;
        cb.inputProcRefCon = pStreamIn;

        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
                                   0, &cb, sizeof(cb));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to register input callback (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Fetch the current stream format of the device. */
        uSize = sizeof(pStreamIn->deviceFormat);
        err = AudioUnitGetProperty(pStreamIn->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                   1, &pStreamIn->deviceFormat, &uSize);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to get device format (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Create an AudioStreamBasicDescription based on our required audio settings. */
        coreAudioPCMInfoToASBDesc(&pStreamIn->Stream.Props, &pStreamIn->streamFormat);

        coreAudioPrintASBDesc("CoreAudio: Recording device", &pStreamIn->deviceFormat);
        coreAudioPrintASBDesc("CoreAudio: Recording stream", &pStreamIn->streamFormat);

        /* If the frequency of the device is different from the requested one we
         * need a converter. The same count if the number of channels is different. */
        if (   pStreamIn->deviceFormat.mSampleRate       != pStreamIn->streamFormat.mSampleRate
            || pStreamIn->deviceFormat.mChannelsPerFrame != pStreamIn->streamFormat.mChannelsPerFrame)
        {
            LogRel(("CoreAudio: Input converter is active\n"));

            err = AudioConverterNew(&pStreamIn->deviceFormat, &pStreamIn->streamFormat, &pStreamIn->pConverter);
            if (RT_UNLIKELY(err != noErr))
            {
                LogRel(("CoreAudio: Failed to create the audio converter (%RI32)\n", err));
                CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
            }

            if (   pStreamIn->deviceFormat.mChannelsPerFrame == 1 /* Mono */
                && pStreamIn->streamFormat.mChannelsPerFrame == 2 /* Stereo */)
            {
                /*
                 * If the channel count is different we have to tell this the converter
                 * and supply a channel mapping. For now we only support mapping
                 * from mono to stereo. For all other cases the core audio defaults
                 * are used, which means dropping additional channels in most
                 * cases.
                 */
                const SInt32 channelMap[2] = {0, 0}; /* Channel map for mono -> stereo, */

                err = AudioConverterSetProperty(pStreamIn->pConverter, kAudioConverterChannelMap, sizeof(channelMap), channelMap);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to set channel mapping (mono -> stereo) for the audio input converter (%RI32)\n", err));
                    CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
                }
            }
#if 0
            /* Set sample rate converter quality to maximum. */
            uFlag = kAudioConverterQuality_Max;
            err = AudioConverterSetProperty(pStreamIn->converter, kAudioConverterSampleRateConverterQuality,
                                            sizeof(uFlag), &uFlag);
            if (err != noErr)
                LogRel(("CoreAudio: Failed to set input audio converter quality to the maximum (%RI32)\n", err));
#endif
            /* Set the new format description for the stream. */
            err = AudioUnitSetProperty(pStreamIn->audioUnit,
                                       kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Output,
                                       1,
                                       &pStreamIn->deviceFormat,
                                       sizeof(pStreamIn->deviceFormat));
            if (RT_UNLIKELY(err != noErr))
            {
                LogRel(("CoreAudio: Failed to set input stream output format (%RI32)\n", err));
                CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
            }

            err = AudioUnitSetProperty(pStreamIn->audioUnit,
                                       kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Input,
                                       1,
                                       &pStreamIn->deviceFormat,
                                       sizeof(pStreamIn->deviceFormat));
            if (RT_UNLIKELY(err != noErr))
            {
                LogRel(("CoreAudio: Failed to set stream input format (%RI32)\n", err));
                CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
            }
        }
        else
        {

            /* Set the new output format description for the input stream. */
            err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                       1, &pStreamIn->streamFormat, sizeof(pStreamIn->streamFormat));
            if (err != noErr)
            {
                LogRel(("CoreAudio: Failed to set output format for input stream (%RI32)\n", err));
                CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
            }
        }

        /*
         * Also set the frame buffer size off the device on our AudioUnit. This
         * should make sure that the frames count which we receive in the render
         * thread is as we like.
         */
        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                                   1, &cFrames, sizeof(cFrames));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set maximum frame buffer size for input stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Finally initialize the new AudioUnit. */
        err = AudioUnitInitialize(pStreamIn->audioUnit);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to initialize audio unit for input stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        uSize = sizeof(pStreamIn->deviceFormat);
        err = AudioUnitGetProperty(pStreamIn->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                   1, &pStreamIn->deviceFormat, &uSize);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to get recording device format (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /*
         * There are buggy devices (e.g. my Bluetooth headset) which doesn't honor
         * the frame buffer size set in the previous calls. So finally get the
         * frame buffer size after the AudioUnit was initialized.
         */
        uSize = sizeof(cFrames);
        err = AudioUnitGetProperty(pStreamIn->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                                   0, &cFrames, &uSize);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to get maximum frame buffer size from input audio device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Destroy any former internal ring buffer. */
        if (pStreamIn->pBuf)
        {
            RTCircBufDestroy(pStreamIn->pBuf);
            pStreamIn->pBuf = NULL;
        }

        /* Calculate the ratio between the device and the stream sample rate. */
        pStreamIn->sampleRatio = pStreamIn->streamFormat.mSampleRate / pStreamIn->deviceFormat.mSampleRate;

        /* Create the AudioBufferList structure with one buffer. */
        pStreamIn->bufferList.mNumberBuffers = 1;
        /* Initialize the buffer to nothing. */
        pStreamIn->bufferList.mBuffers[0].mNumberChannels = pStreamIn->streamFormat.mChannelsPerFrame;
        pStreamIn->bufferList.mBuffers[0].mDataByteSize = 0;
        pStreamIn->bufferList.mBuffers[0].mData = NULL;

        /*
         * Make sure that the ring buffer is big enough to hold the recording
         * data. Compare the maximum frames per slice value with the frames
         * necessary when using the converter where the sample rate could differ.
         * The result is always multiplied by the channels per frame to get the
         * samples count.
         */
        cSamples = RT_MAX(cFrames,
                          (cFrames * pStreamIn->deviceFormat.mBytesPerFrame * pStreamIn->sampleRatio)
                           / pStreamIn->streamFormat.mBytesPerFrame)
                           * pStreamIn->streamFormat.mChannelsPerFrame;
        if (!cSamples)
        {
            LogRel(("CoreAudio: Failed to determine samples buffer count input stream\n"));
            CA_BREAK_STMT(rc = VERR_INVALID_PARAMETER);
        }

        /* Create the internal ring buffer. */
        rc = RTCircBufCreate(&pStreamIn->pBuf, cSamples << pStream->Props.cShift);

    } while (0);

    if (RT_SUCCESS(rc))
    {
        ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_INIT);

        LogFunc(("cSamples=%RU32\n", cSamples));

        if (pcSamples)
            *pcSamples = cSamples;
    }
    else
    {
        AudioUnitUninitialize(pStreamIn->audioUnit);

        if (pStreamIn->pBuf)
        {
            RTCircBufDestroy(pStreamIn->pBuf);
            pStreamIn->pBuf = NULL;
        }

        ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_UNINIT);
    }

    LogFunc(("rc=%Rrc\n", rc));
    return rc;
}

/** @todo Eventually split up this function, as this already is huge! */
static int coreAudioInitOut(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream, uint32_t *pcSamples)
{
    PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;

    ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_IN_INIT);

    OSStatus err = noErr;

    UInt32 uSize = 0;
    if (pStreamOut->deviceID == kAudioDeviceUnknown)
    {
        /* Fetch the default audio recording device currently in use. */
        AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultOutputDevice,
                                               kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
        uSize = sizeof(pStreamOut->deviceID);
        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdr, 0, NULL, &uSize, &pStreamOut->deviceID);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Unable to determine default playback device (%RI32)\n", err));
            return VERR_NOT_FOUND;
        }
    }

    /*
     * Try to get the name of the playback device and log it. It's not fatal if it fails.
     */
    CFStringRef strTemp;

    AudioObjectPropertyAddress propAdr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMaster };
    uSize = sizeof(CFStringRef);
    err = AudioObjectGetPropertyData(pStreamOut->deviceID, &propAdr, 0, NULL, &uSize, &strTemp);
    if (err == noErr)
    {
        char *pszDevName = NULL;
        err = coreAudioCFStringToCString(strTemp, &pszDevName);
        if (err == noErr)
        {
            CFRelease(strTemp);

            /* Get the device' UUID. */
            propAdr.mSelector = kAudioDevicePropertyDeviceUID;
            err = AudioObjectGetPropertyData(pStreamOut->deviceID, &propAdr, 0, NULL, &uSize, &strTemp);
            if (err == noErr)
            {
                char *pszUID = NULL;
                err = coreAudioCFStringToCString(strTemp, &pszUID);
                if (err == noErr)
                {
                    CFRelease(strTemp);
                    LogRel(("CoreAudio: Using playback device: %s (UID: %s)\n", pszDevName, pszUID));

                    RTMemFree(pszUID);
                }
            }

            RTMemFree(pszDevName);
        }
    }
    else
        LogRel(("CoreAudio: Unable to determine playback device name (%RI32)\n", err));

    /* Get the default frames buffer size, so that we can setup our internal buffers. */
    UInt32 cFrames;
    uSize = sizeof(cFrames);
    propAdr.mSelector = kAudioDevicePropertyBufferFrameSize;
    propAdr.mScope    = kAudioDevicePropertyScopeInput;
    err = AudioObjectGetPropertyData(pStreamOut->deviceID, &propAdr, 0, NULL, &uSize, &cFrames);
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to determine frame buffer size of the audio playback device (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /* Set the frame buffer size and honor any minimum/maximum restrictions on the device. */
    err = coreAudioSetFrameBufferSize(pStreamOut->deviceID, false /* fInput */, cFrames, &cFrames);
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to set frame buffer size for the audio playback device (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    ComponentDescription cd;
    RT_ZERO(cd);
    cd.componentType         = kAudioUnitType_Output;
    cd.componentSubType      = kAudioUnitSubType_HALOutput;
    cd.componentManufacturer = kAudioUnitManufacturer_Apple;

    /* Try to find the default HAL output component. */
    Component cp = FindNextComponent(NULL, &cd);
    if (cp == 0)
    {
        LogRel(("CoreAudio: Failed to find HAL output component\n")); /** @todo Return error value? */
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /* Open the default HAL output component. */
    err = OpenAComponent(cp, &pStreamOut->audioUnit);
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to open output component (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /* Switch the I/O mode for output to on. */
    UInt32 uFlag = 1;
    err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                               0, &uFlag, sizeof(uFlag));
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to disable I/O mode for output stream (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /* Set the default audio playback device as the device for the new AudioUnit. */
    err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                               0, &pStreamOut->deviceID, sizeof(pStreamOut->deviceID));
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to set current device for output stream (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /*
     * CoreAudio will inform us on a second thread for new incoming audio data.
     * Therefor register a callback function which will process the new data.
     */
    AURenderCallbackStruct cb;
    RT_ZERO(cb);
    cb.inputProc       = coreAudioPlaybackCb; /* pvUser */
    cb.inputProcRefCon = pStreamOut;

    err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                               0, &cb, sizeof(cb));
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to register output callback (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /* Fetch the current stream format of the device. */
    uSize = sizeof(pStreamOut->deviceFormat);
    err = AudioUnitGetProperty(pStreamOut->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                               0, &pStreamOut->deviceFormat, &uSize);
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to get device format (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /* Create an AudioStreamBasicDescription based on our required audio settings. */
    coreAudioPCMInfoToASBDesc(&pStreamOut->Stream.Props, &pStreamOut->streamFormat);

    coreAudioPrintASBDesc("CoreAudio: playback device", &pStreamOut->deviceFormat);
    coreAudioPrintASBDesc("CoreAudio: Output format", &pStreamOut->streamFormat);

    /* Set the new output format description for the stream. */
    err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                               0, &pStreamOut->streamFormat, sizeof(pStreamOut->streamFormat));
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to set stream format for output stream (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    uSize = sizeof(pStreamOut->deviceFormat);
    err = AudioUnitGetProperty(pStreamOut->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                               0, &pStreamOut->deviceFormat, &uSize);
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to retrieve device format for output stream (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /*
     * Also set the frame buffer size off the device on our AudioUnit. This
     * should make sure that the frames count which we receive in the render
     * thread is as we like.
     */
    err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                               0, &cFrames, sizeof(cFrames));
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to set maximum frame buffer size for output AudioUnit (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /* Finally initialize the new AudioUnit. */
    err = AudioUnitInitialize(pStreamOut->audioUnit);
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to initialize the output audio device (%RI32)\n", err));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /*
     * There are buggy devices (e.g. my Bluetooth headset) which doesn't honor
     * the frame buffer size set in the previous calls. So finally get the
     * frame buffer size after the AudioUnit was initialized.
     */
    uSize = sizeof(cFrames);
    err = AudioUnitGetProperty(pStreamOut->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                               0, &cFrames, &uSize);
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to get maximum frame buffer size from output audio device (%RI32)\n", err));

        AudioUnitUninitialize(pStreamOut->audioUnit);
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /*
     * Make sure that the ring buffer is big enough to hold the recording
     * data. Compare the maximum frames per slice value with the frames
     * necessary when using the converter where the sample rate could differ.
     * The result is always multiplied by the channels per frame to get the
     * samples count.
     */
    int rc = VINF_SUCCESS;

    UInt32 cSamples = cFrames * pStreamOut->streamFormat.mChannelsPerFrame;
    if (!cSamples)
    {
        LogRel(("CoreAudio: Failed to determine samples buffer count output stream\n"));
        rc = VERR_INVALID_PARAMETER;
    }

    /* Destroy any former internal ring buffer. */
    if (pStreamOut->pBuf)
    {
        RTCircBufDestroy(pStreamOut->pBuf);
        pStreamOut->pBuf = NULL;
    }

    /* Create the internal ring buffer. */
    rc = RTCircBufCreate(&pStreamOut->pBuf, cSamples << pStream->Props.cShift);
    if (RT_SUCCESS(rc))
    {
        /*
         * Register callbacks.
         */
#ifdef DEBUG
        propAdr.mSelector = kAudioDeviceProcessorOverload;
        propAdr.mScope    = kAudioUnitScope_Global;
        err = AudioObjectAddPropertyListener(pStreamOut->deviceID, &propAdr,
                                             coreAudioPlaybackAudioDevicePropertyChanged, (void *)pStreamOut);
        if (err != noErr)
            LogRel(("CoreAudio: Failed to register processor overload listener for output stream (%RI32)\n", err));
#endif /* DEBUG */

        propAdr.mSelector = kAudioDevicePropertyNominalSampleRate;
        propAdr.mScope    = kAudioUnitScope_Global;
        err = AudioObjectAddPropertyListener(pStreamOut->deviceID, &propAdr,
                                             coreAudioPlaybackAudioDevicePropertyChanged, (void *)pStreamOut);
        /* Not fatal. */
        if (err != noErr)
            LogRel(("CoreAudio: Failed to register sample rate changed listener for output stream (%RI32)\n", err));
    }

    if (RT_SUCCESS(rc))
    {
        ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_INIT);

        if (pcSamples)
            *pcSamples = cSamples;
    }
    else
    {
        AudioUnitUninitialize(pStreamOut->audioUnit);

        if (pStreamOut->pBuf)
        {
            RTCircBufDestroy(pStreamOut->pBuf);
            pStreamOut->pBuf = NULL;
        }
    }

    LogFunc(("cSamples=%RU32, rc=%Rrc\n", cSamples, rc));
    return rc;
}

/* Callback for getting notified when some of the properties of an audio device has changed. */
static DECLCALLBACK(OSStatus) coreAudioPlaybackAudioDevicePropertyChanged(AudioObjectID propertyID,
                                                                          UInt32 nAddresses,
                                                                          const AudioObjectPropertyAddress properties[],
                                                                          void *pvUser)
{
    switch (propertyID)
    {
#ifdef DEBUG
#endif /* DEBUG */
        default:
            break;
    }

    return noErr;
}

/* Callback to feed audio output buffer. */
static DECLCALLBACK(OSStatus) coreAudioPlaybackCb(void *pvUser,
                                                  AudioUnitRenderActionFlags *pActionFlags,
                                                  const AudioTimeStamp       *pAudioTS,
                                                  UInt32                      uBusID,
                                                  UInt32                      cFrames,
                                                  AudioBufferList            *pBufData)
{
    PCOREAUDIOSTREAMOUT pStreamOut  = (PCOREAUDIOSTREAMOUT)pvUser;
    PPDMAUDIOSTREAM     pStream     = &pStreamOut->Stream;

    if (ASMAtomicReadU32(&pStreamOut->status) != CA_STATUS_INIT)
    {
        pBufData->mBuffers[0].mDataByteSize = 0;
        return noErr;
    }

    /* How much space is used in the ring buffer? */
    size_t cbToRead = RT_MIN(RTCircBufUsed(pStreamOut->pBuf), pBufData->mBuffers[0].mDataByteSize);
    if (!cbToRead)
    {
        pBufData->mBuffers[0].mDataByteSize = 0;
        return noErr;
    }

    uint8_t *pbSrc = NULL;
    size_t cbRead  = 0;

    size_t cbLeft  = cbToRead;
    while (cbLeft)
    {
        /* Try to acquire the necessary block from the ring buffer. */
        RTCircBufAcquireReadBlock(pStreamOut->pBuf, cbLeft, (void **)&pbSrc, &cbToRead);

        /* Break if nothing is used anymore. */
        if (!cbToRead)
            break;

        /* Copy the data from our ring buffer to the core audio buffer. */
        memcpy((uint8_t *)pBufData->mBuffers[0].mData + cbRead, pbSrc, cbToRead);

        /* Release the read buffer, so it could be used for new data. */
        RTCircBufReleaseReadBlock(pStreamOut->pBuf, cbToRead);

        /* Move offset. */
        cbRead += cbToRead;
        Assert(pBufData->mBuffers[0].mDataByteSize >= cbRead);

        Assert(cbToRead <= cbLeft);
        cbLeft -= cbToRead;
    }

    /* Write the bytes to the core audio buffer which where really written. */
    pBufData->mBuffers[0].mDataByteSize = cbRead;

    LogFlowFunc(("Read %zu / %zu bytes\n", cbRead, cbToRead));

    return noErr;
}

static DECLCALLBACK(int) drvHostCoreAudioInit(PPDMIHOSTAUDIO pInterface)
{
    NOREF(pInterface);

    LogFlowFuncEnter();

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvHostCoreAudioStreamCapture(PPDMIHOSTAUDIO pInterface, PPDMAUDIOSTREAM pStream,
                                                       uint32_t *pcSamplesCaptured)
{
    PPDMDRVINS pDrvIns      = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTCOREAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);

    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;

    size_t csReads = 0;
    char *pcSrc;
    PPDMAUDIOSAMPLE psDst;

    if (ASMAtomicReadU32(&pStreamIn->status) != CA_STATUS_INIT)
    {
        if (pcSamplesCaptured)
            *pcSamplesCaptured = 0;
        return VINF_SUCCESS;
    }

    int rc = VINF_SUCCESS;
    uint32_t cbWrittenTotal = 0;

    do
    {
        size_t cbBuf = AudioMixBufSizeBytes(&pStream->MixBuf);
        size_t cbToWrite = RT_MIN(cbBuf, RTCircBufUsed(pStreamIn->pBuf));

        uint32_t cWritten, cbWritten;
        uint8_t *puBuf;
        size_t   cbToRead;

        Log3Func(("cbBuf=%zu, cbToWrite=%zu\n", cbBuf, cbToWrite));

        while (cbToWrite)
        {
            /* Try to acquire the necessary block from the ring buffer. */
            RTCircBufAcquireReadBlock(pStreamIn->pBuf, cbToWrite, (void **)&puBuf, &cbToRead);
            if (!cbToRead)
            {
                RTCircBufReleaseReadBlock(pStreamIn->pBuf, cbToRead);
                break;
            }

            rc = AudioMixBufWriteCirc(&pStream->MixBuf, puBuf, cbToRead, &cWritten);
            if (   RT_FAILURE(rc)
                || !cWritten)
            {
                RTCircBufReleaseReadBlock(pStreamIn->pBuf, cbToRead);
                break;
            }

            cbWritten = AUDIOMIXBUF_S2B(&pStream->MixBuf, cWritten);

            /* Release the read buffer, so it could be used for new data. */
            RTCircBufReleaseReadBlock(pStreamIn->pBuf, cbWritten);

            Assert(cbToWrite >= cbWritten);
            cbToWrite      -= cbWritten;
            cbWrittenTotal += cbWritten;
        }

        Log3Func(("cbToWrite=%zu, cbToRead=%zu, cbWrittenTotal=%RU32, rc=%Rrc\n", cbToWrite, cbToRead, cbWrittenTotal, rc));
    }
    while (0);

    if (RT_SUCCESS(rc))
    {
        uint32_t cCaptured     = 0;
        uint32_t cWrittenTotal = AUDIOMIXBUF_B2S(&pStream->MixBuf, cbWrittenTotal);
        if (cWrittenTotal)
            rc = AudioMixBufMixToParent(&pStream->MixBuf, cWrittenTotal, &cCaptured);

        Log3Func(("cWrittenTotal=%RU32 (%RU32 bytes), cCaptured=%RU32, rc=%Rrc\n", cWrittenTotal, cbWrittenTotal, cCaptured, rc));

        if (cCaptured)
            LogFlowFunc(("%RU32 samples captured\n", cCaptured));

        if (pcSamplesCaptured)
            *pcSamplesCaptured = cCaptured;
    }

    if (RT_FAILURE(rc))
        LogFunc(("Failed with rc=%Rrc\n", rc));

    return rc;
}

static DECLCALLBACK(int) drvHostCoreAudioStreamPlay(PPDMIHOSTAUDIO pInterface, PPDMAUDIOSTREAM pStream,
                                                    uint32_t *pcSamplesPlayed)
{
    PPDMDRVINS pDrvIns      = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTCOREAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);

    PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;

    int rc = VINF_SUCCESS;

    uint32_t cLive = AudioMixBufLive(&pStream->MixBuf);
    if (!cLive) /* Not live samples to play? Bail out. */
    {
        if (pcSamplesPlayed)
            *pcSamplesPlayed = 0;
        return VINF_SUCCESS;
    }

    size_t cbLive  = AUDIOMIXBUF_S2B(&pStream->MixBuf, cLive);

    uint32_t cbReadTotal = 0;

    size_t cbToRead = RT_MIN(cbLive, RTCircBufFree(pStreamOut->pBuf));
    LogFlowFunc(("cbToRead=%zu\n", cbToRead));

    while (cbToRead)
    {
        uint32_t cRead, cbRead;
        uint8_t *puBuf;
        size_t   cbCopy;

        /* Try to acquire the necessary space from the ring buffer. */
        RTCircBufAcquireWriteBlock(pStreamOut->pBuf, cbToRead, (void **)&puBuf, &cbCopy);
        if (!cbCopy)
        {
            RTCircBufReleaseWriteBlock(pStreamOut->pBuf, cbCopy);
            break;
        }

        Assert(cbCopy <= cbToRead);

        rc = AudioMixBufReadCirc(&pStream->MixBuf,
                                 puBuf, cbCopy, &cRead);

        if (   RT_FAILURE(rc)
            || !cRead)
        {
            RTCircBufReleaseWriteBlock(pStreamOut->pBuf, 0);
            break;
        }

        cbRead = AUDIOMIXBUF_S2B(&pStream->MixBuf, cRead);

        /* Release the ring buffer, so the read thread could start reading this data. */
        RTCircBufReleaseWriteBlock(pStreamOut->pBuf, cbRead);

        Assert(cbToRead >= cbRead);
        cbToRead -= cbRead;
        cbReadTotal += cbRead;
    }

    if (RT_SUCCESS(rc))
    {
        uint32_t cReadTotal = AUDIOMIXBUF_B2S(&pStream->MixBuf, cbReadTotal);
        if (cReadTotal)
            AudioMixBufFinish(&pStream->MixBuf, cReadTotal);

        LogFlowFunc(("cReadTotal=%RU32 (%RU32 bytes)\n", cReadTotal, cbReadTotal));

        if (pcSamplesPlayed)
            *pcSamplesPlayed = cReadTotal;
    }

    return rc;
}

static DECLCALLBACK(int) coreAudioControlStreamOut(PDRVHOSTCOREAUDIO pThis,
                                                   PPDMAUDIOSTREAM pStream, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;

    LogFlowFunc(("enmStreamCmd=%RU32\n", enmStreamCmd));

    uint32_t uStatus = ASMAtomicReadU32(&pStreamOut->status);
    if (!(   uStatus == CA_STATUS_INIT
          || uStatus == CA_STATUS_REINIT))
    {
        return VINF_SUCCESS;
    }

    int rc = VINF_SUCCESS;
    OSStatus err;

    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        case PDMAUDIOSTREAMCMD_RESUME:
        {
            /* Only start the device if it is actually stopped */
            if (!coreAudioIsRunning(pStreamOut->deviceID))
            {
                err = AudioUnitReset(pStreamOut->audioUnit, kAudioUnitScope_Input, 0);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to reset AudioUnit (%RI32)\n", err));
                    /* Keep going. */
                }
                RTCircBufReset(pStreamOut->pBuf);

                err = AudioOutputUnitStart(pStreamOut->audioUnit);
                if (RT_UNLIKELY(err != noErr))
                {
                    LogRel(("CoreAudio: Failed to start playback (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                }
            }
            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
        case PDMAUDIOSTREAMCMD_PAUSE:
        {
            /* Only stop the device if it is actually running */
            if (coreAudioIsRunning(pStreamOut->deviceID))
            {
                err = AudioOutputUnitStop(pStreamOut->audioUnit);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to stop playback (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                    break;
                }

                err = AudioUnitReset(pStreamOut->audioUnit, kAudioUnitScope_Input, 0);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to reset AudioUnit (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                }
            }
            break;
        }

        default:
            rc = VERR_NOT_SUPPORTED;
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioControlStreamIn(PDRVHOSTCOREAUDIO pThis,
                                    PPDMAUDIOSTREAM pStream, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;

    LogFlowFunc(("enmStreamCmd=%RU32\n", enmStreamCmd));

    uint32_t uStatus = ASMAtomicReadU32(&pStreamIn->status);
    if (!(   uStatus == CA_STATUS_INIT
          || uStatus == CA_STATUS_REINIT))
    {
        return VINF_SUCCESS;
    }

    int rc = VINF_SUCCESS;
    OSStatus err = noErr;

    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        case PDMAUDIOSTREAMCMD_RESUME:
        {
            /* Only start the device if it is actually stopped */
            if (!coreAudioIsRunning(pStreamIn->deviceID))
            {
                RTCircBufReset(pStreamIn->pBuf);
                err = AudioOutputUnitStart(pStreamIn->audioUnit);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to start recording (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                    break;
                }
            }

            if (err != noErr)
            {
                LogRel(("CoreAudio: Failed to start recording (%RI32)\n", err));
                rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
            }
            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
        case PDMAUDIOSTREAMCMD_PAUSE:
        {
            /* Only stop the device if it is actually running */
            if (coreAudioIsRunning(pStreamIn->deviceID))
            {
                err = AudioOutputUnitStop(pStreamIn->audioUnit);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to stop recording (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                    break;
                }

                err = AudioUnitReset(pStreamIn->audioUnit, kAudioUnitScope_Input, 0);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to reset AudioUnit (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                    break;
                }
            }
            break;
        }

        default:
            rc = VERR_NOT_SUPPORTED;
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioDestroyStreamIn(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream)
{
    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;

    LogFlowFuncEnter();

    uint32_t status = ASMAtomicReadU32(&pStreamIn->status);
    if (!(   status == CA_STATUS_INIT
          || status == CA_STATUS_REINIT))
    {
        return VINF_SUCCESS;
    }

    OSStatus err = noErr;

    int rc = coreAudioControlStreamIn(pThis, &pStreamIn->Stream, PDMAUDIOSTREAMCMD_DISABLE);
    if (RT_SUCCESS(rc))
    {
        ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_IN_UNINIT);

        /*
         * Unregister recording device callbacks.
         */
        AudioObjectPropertyAddress propAdr = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMaster };
#ifdef DEBUG
        err = AudioObjectRemovePropertyListener(pStreamIn->deviceID, &propAdr,
                                                coreAudioRecordingAudioDevicePropertyChanged, &pStreamIn->cbCtx);
        if (   err != noErr
            && err != kAudioHardwareBadObjectError)
        {
            LogRel(("CoreAudio: Failed to remove the recording processor overload listener (%RI32)\n", err));
        }
#endif /* DEBUG */

        propAdr.mSelector = kAudioDevicePropertyNominalSampleRate;
        err = AudioObjectRemovePropertyListener(pStreamIn->deviceID, &propAdr,
                                                coreAudioRecordingAudioDevicePropertyChanged, &pStreamIn->cbCtx);
        if (   err != noErr
            && err != kAudioHardwareBadObjectError)
        {
            LogRel(("CoreAudio: Failed to remove the recording sample rate changed listener (%RI32)\n", err));
        }

        if (pStreamIn->fDefDevChgListReg)
        {
            propAdr.mSelector = kAudioHardwarePropertyDefaultInputDevice;
            err = AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &propAdr,
                                                    coreAudioDefaultDeviceChanged, &pStreamIn->cbCtx);
            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to remove the default recording device changed listener (%RI32)\n", err));
            }

            pStreamIn->fDefDevChgListReg = false;
        }

        if (pStreamIn->fDevStateChgListReg)
        {
            Assert(pStreamIn->deviceID != kAudioDeviceUnknown);

            AudioObjectPropertyAddress propAdr2 = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMaster };
            err = AudioObjectRemovePropertyListener(pStreamIn->deviceID, &propAdr2,
                                                    drvHostCoreAudioDeviceStateChanged, &pStreamIn->cbCtx);
            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to remove the recording device state changed listener (%RI32)\n", err));
            }

            pStreamIn->fDevStateChgListReg = false;
        }

        if (pStreamIn->pConverter)
        {
            AudioConverterDispose(pStreamIn->pConverter);
            pStreamIn->pConverter = NULL;
        }

        err = AudioUnitUninitialize(pStreamIn->audioUnit);
        if (err == noErr)
            err = CloseComponent(pStreamIn->audioUnit);

        if (   err != noErr
            && err != kAudioHardwareBadObjectError)
        {
            LogRel(("CoreAudio: Failed to uninit the recording device (%RI32)\n", err));
        }

        pStreamIn->deviceID      = kAudioDeviceUnknown;
        pStreamIn->audioUnit     = NULL;
        pStreamIn->offBufferRead = 0;
        pStreamIn->sampleRatio   = 1;
        if (pStreamIn->pBuf)
        {
            RTCircBufDestroy(pStreamIn->pBuf);
            pStreamIn->pBuf = NULL;
        }

        ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_UNINIT);
    }
    else
        LogRel(("CoreAudio: Failed to stop recording on uninit, rc=%Rrc\n", rc));

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioDestroyStreamOut(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOSTREAM pStream)
{
    PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;

    LogFlowFuncEnter();

    uint32_t status = ASMAtomicReadU32(&pStreamOut->status);
    if (!(   status == CA_STATUS_INIT
          || status == CA_STATUS_REINIT))
    {
        return VINF_SUCCESS;
    }

    int rc = coreAudioControlStreamOut(pThis, &pStreamOut->Stream, PDMAUDIOSTREAMCMD_DISABLE);
    if (RT_SUCCESS(rc))
    {
        ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_IN_UNINIT);

        OSStatus err;

        /*
         * Unregister playback device callbacks.
         */
        AudioObjectPropertyAddress propAdr = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMaster };
#ifdef DEBUG
        err = AudioObjectRemovePropertyListener(pStreamOut->deviceID, &propAdr,
                                                coreAudioPlaybackAudioDevicePropertyChanged, &pStreamOut->cbCtx);
        if (   err != noErr
            && err != kAudioHardwareBadObjectError)
        {
            LogRel(("CoreAudio: Failed to remove the playback processor overload listener (%RI32)\n", err));
        }
#endif /* DEBUG */

        propAdr.mSelector = kAudioDevicePropertyNominalSampleRate;
        err = AudioObjectRemovePropertyListener(pStreamOut->deviceID, &propAdr,
                                                coreAudioPlaybackAudioDevicePropertyChanged, &pStreamOut->cbCtx);
        if (   err != noErr
            && err != kAudioHardwareBadObjectError)
        {
            LogRel(("CoreAudio: Failed to remove the playback sample rate changed listener (%RI32)\n", err));
        }

        if (pStreamOut->fDefDevChgListReg)
        {
            propAdr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
            propAdr.mScope    = kAudioObjectPropertyScopeGlobal;
            propAdr.mElement  = kAudioObjectPropertyElementMaster;
            err = AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &propAdr,
                                                    coreAudioDefaultDeviceChanged, &pStreamOut->cbCtx);
            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to remove the default playback device changed listener (%RI32)\n", err));
            }

            pStreamOut->fDefDevChgListReg = false;
        }

        if (pStreamOut->fDevStateChgListReg)
        {
            Assert(pStreamOut->deviceID != kAudioDeviceUnknown);

            AudioObjectPropertyAddress propAdr2 = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMaster };
            err = AudioObjectRemovePropertyListener(pStreamOut->deviceID, &propAdr2,
                                                    drvHostCoreAudioDeviceStateChanged, &pStreamOut->cbCtx);
            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to remove the playback device state changed listener (%RI32)\n", err));
            }

            pStreamOut->fDevStateChgListReg = false;
        }

        err = AudioUnitUninitialize(pStreamOut->audioUnit);
        if (err == noErr)
            err = CloseComponent(pStreamOut->audioUnit);

        if (   err != noErr
            && err != kAudioHardwareBadObjectError)
        {
            LogRel(("CoreAudio: Failed to uninit the playback device (%RI32)\n", err));
        }

        pStreamOut->deviceID  = kAudioDeviceUnknown;
        pStreamOut->audioUnit = NULL;
        if (pStreamOut->pBuf)
        {
            RTCircBufDestroy(pStreamOut->pBuf);
            pStreamOut->pBuf = NULL;
        }

        ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_UNINIT);
    }
    else
        LogRel(("CoreAudio: Failed to stop playback on uninit, rc=%Rrc\n", rc));

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioCreateStreamIn(PDRVHOSTCOREAUDIO pThis,
                                   PPDMAUDIOSTREAM pStream, PPDMAUDIOSTREAMCFG pCfg, uint32_t *pcSamples)
{
    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;

    pStreamIn->deviceID                  = kAudioDeviceUnknown;
    pStreamIn->audioUnit                 = NULL;
    pStreamIn->pConverter                = NULL;
    pStreamIn->bufferList.mNumberBuffers = 0;
    pStreamIn->offBufferRead             = 0;
    pStreamIn->sampleRatio               = 1;
    pStreamIn->pBuf                      = NULL;
    pStreamIn->status                    = CA_STATUS_UNINIT;
    pStreamIn->fDefDevChgListReg         = false;
    pStreamIn->fDevStateChgListReg       = false;

    /* Set callback context. */
    pStreamIn->cbCtx.pThis  = pThis;
    pStreamIn->cbCtx.enmDir = PDMAUDIODIR_IN;
    pStreamIn->cbCtx.pIn    = pStreamIn;

    bool fDeviceByUser = false; /* Do we use a device which was set by the user? */

    LogFlowFunc(("enmRecSource=%RU32\n", pCfg->DestSource.Source));

    int rc = DrvAudioHlpStreamCfgToProps(pCfg, &pStreamIn->Stream.Props);
    if (RT_SUCCESS(rc))
    {
#if 0
        /* Try to find the audio device set by the user */
        if (DeviceUID.pszInputDeviceUID)
        {
            pStreamIn->deviceID = drvHostCoreAudioDeviceUIDtoID(DeviceUID.pszInputDeviceUID);
            /* Not fatal */
            if (pStreamIn->deviceID == kAudioDeviceUnknown)
                LogRel(("CoreAudio: Unable to find recording device %s. Falling back to the default audio device. \n", DeviceUID.pszInputDeviceUID));
            else
                fDeviceByUser = true;
        }
#endif
        rc = coreAudioInitIn(pThis, &pStreamIn->Stream, pcSamples);
    }

    OSStatus err;

    /* When the devices isn't forced by the user, we want default device change notifications. */
    if (!fDeviceByUser)
    {
        if (!pStreamIn->fDefDevChgListReg)
        {
            AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMaster };
            err = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &propAdr,
                                                 coreAudioDefaultDeviceChanged, &pStreamIn->cbCtx);
            if (   err == noErr
                || err == kAudioHardwareIllegalOperationError)
            {
                pStreamIn->fDefDevChgListReg = true;
            }
            else
                LogRel(("CoreAudio: Failed to add the default recording device changed listener (%RI32)\n", err));
        }
    }

    if (   !pStreamIn->fDevStateChgListReg
        && (pStreamIn->deviceID != kAudioDeviceUnknown))
    {
        /* Register callback for being notified if the device stops being alive. */
        AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMaster };
        err = AudioObjectAddPropertyListener(pStreamIn->deviceID, &propAdr, drvHostCoreAudioDeviceStateChanged,
                                             &pStreamIn->cbCtx);
        if (err == noErr)
        {
            pStreamIn->fDevStateChgListReg = true;
        }
        else
            LogRel(("CoreAudio: Failed to add the recording device state changed listener (%RI32)\n", err));
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioCreateStreamOut(PDRVHOSTCOREAUDIO pThis,
                                    PPDMAUDIOSTREAM pStream, PPDMAUDIOSTREAMCFG pCfg,
                                    uint32_t *pcSamples)
{
    PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;

    LogFlowFuncEnter();

    pStreamOut->deviceID                  = kAudioDeviceUnknown;
    pStreamOut->audioUnit                 = NULL;
    pStreamOut->pBuf                      = NULL;
    pStreamOut->status                    = CA_STATUS_UNINIT;
    pStreamOut->fDefDevChgListReg         = false;
    pStreamOut->fDevStateChgListReg       = false;

    /* Set callback context. */
    pStreamOut->cbCtx.pThis  = pThis;
    pStreamOut->cbCtx.enmDir = PDMAUDIODIR_OUT;
    pStreamOut->cbCtx.pOut   = pStreamOut;

    bool fDeviceByUser = false; /* Do we use a device which was set by the user? */

    /* If a stream configuration is given, apply that to the stream. */
    int rc = DrvAudioHlpStreamCfgToProps(pCfg, &pStreamOut->Stream.Props);
    if (RT_SUCCESS(rc))
    {
#if 0
        /* Try to find the audio device set by the user. Use
         * export VBOX_COREAUDIO_OUTPUT_DEVICE_UID=AppleHDAEngineOutput:0
         * to set it. */
        if (DeviceUID.pszOutputDeviceUID)
        {
            pStreamOut->audioDeviceId = drvHostCoreAudioDeviceUIDtoID(DeviceUID.pszOutputDeviceUID);
            /* Not fatal */
            if (pStreamOut->audioDeviceId == kAudioDeviceUnknown)
                LogRel(("CoreAudio: Unable to find playback device %s. Falling back to the default audio device. \n", DeviceUID.pszOutputDeviceUID));
            else
                fDeviceByUser = true;
        }
#endif
        rc = coreAudioInitOut(pThis, pStream, pcSamples);
    }

    OSStatus err;

    /* When the devices isn't forced by the user, we want default device change notifications. */
    if (!fDeviceByUser)
    {
        AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMaster };
        err = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &propAdr,
                                             coreAudioDefaultDeviceChanged, &pStreamOut->cbCtx);
        if (err == noErr)
        {
            pStreamOut->fDefDevChgListReg = true;
        }
        else
            LogRel(("CoreAudio: Failed to add the default playback device changed listener (%RI32)\n", err));
    }

    if (   !pStreamOut->fDevStateChgListReg
        && (pStreamOut->deviceID != kAudioDeviceUnknown))
    {
        /* Register callback for being notified if the device stops being alive. */
        AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMaster };
        err = AudioObjectAddPropertyListener(pStreamOut->deviceID, &propAdr, drvHostCoreAudioDeviceStateChanged,
                                             &pStreamOut->cbCtx);
        if (err == noErr)
        {
            pStreamOut->fDevStateChgListReg = true;
        }
        else
            LogRel(("CoreAudio: Failed to add the playback device state changed listener (%RI32)\n", err));
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) drvHostCoreAudioGetConfig(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDCFG pCfg)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg,       VERR_INVALID_POINTER);

    PPDMDRVINS        pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTCOREAUDIO pThis   = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);

    return coreAudioUpdateStatusInternalEx(pThis, pCfg, 0 /* fEnum */);
}

static DECLCALLBACK(PDMAUDIOBACKENDSTS) drvHostCoreAudioGetStatus(PPDMIHOSTAUDIO pInterface, PDMAUDIODIR enmDir)
{
    AssertPtrReturn(pInterface, PDMAUDIOBACKENDSTS_UNKNOWN);

    return PDMAUDIOBACKENDSTS_RUNNING;
}

static DECLCALLBACK(int) drvHostCoreAudioStreamCreate(PPDMIHOSTAUDIO pInterface,
                                                       PPDMAUDIOSTREAM pStream, PPDMAUDIOSTREAMCFG pCfg, uint32_t *pcSamples)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg,       VERR_INVALID_POINTER);

    PPDMDRVINS        pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTCOREAUDIO pThis   = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);

    int rc;
    if (pCfg->enmDir == PDMAUDIODIR_IN)
        rc = coreAudioCreateStreamIn(pThis,  pStream, pCfg, pcSamples);
    else
        rc = coreAudioCreateStreamOut(pThis, pStream, pCfg, pcSamples);

    LogFlowFunc(("%s: rc=%Rrc\n", pStream->szName, rc));
    return rc;
}

static DECLCALLBACK(int) drvHostCoreAudioStreamDestroy(PPDMIHOSTAUDIO pInterface, PPDMAUDIOSTREAM pStream)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);

    PPDMDRVINS        pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTCOREAUDIO pThis   = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);

    int rc;
    if (pStream->enmDir == PDMAUDIODIR_IN)
        rc = coreAudioDestroyStreamIn(pThis,  pStream);
    else
        rc = coreAudioDestroyStreamOut(pThis, pStream);

    return rc;
}

static DECLCALLBACK(int) drvHostCoreAudioStreamControl(PPDMIHOSTAUDIO pInterface,
                                                       PPDMAUDIOSTREAM pStream, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);

    Assert(pStream->enmCtx == PDMAUDIOSTREAMCTX_HOST);

    PPDMDRVINS        pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTCOREAUDIO pThis   = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);

    int rc;
    if (pStream->enmDir == PDMAUDIODIR_IN)
        rc = coreAudioControlStreamIn(pThis,  pStream, enmStreamCmd);
    else
        rc = coreAudioControlStreamOut(pThis, pStream, enmStreamCmd);

    return rc;
}

static DECLCALLBACK(PDMAUDIOSTRMSTS) drvHostCoreAudioStreamGetStatus(PPDMIHOSTAUDIO pInterface, PPDMAUDIOSTREAM pStream)
{
    NOREF(pInterface);
    AssertPtrReturn(pStream, VERR_INVALID_POINTER);

    Assert(pStream->enmCtx == PDMAUDIOSTREAMCTX_HOST);

    PDMAUDIOSTRMSTS strmSts = PDMAUDIOSTRMSTS_FLAG_NONE;

    if (pStream->enmDir == PDMAUDIODIR_IN)
    {
        PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;

        if (ASMAtomicReadU32(&pStreamIn->status) == CA_STATUS_INIT)
            strmSts |= PDMAUDIOSTRMSTS_FLAG_INITIALIZED | PDMAUDIOSTRMSTS_FLAG_ENABLED;

        if (strmSts & PDMAUDIOSTRMSTS_FLAG_ENABLED)
            strmSts |= PDMAUDIOSTRMSTS_FLAG_DATA_READABLE;
    }
    else if (pStream->enmDir == PDMAUDIODIR_OUT)
    {
        PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;

        if (ASMAtomicReadU32(&pStreamOut->status) == CA_STATUS_INIT)
            strmSts |= PDMAUDIOSTRMSTS_FLAG_INITIALIZED | PDMAUDIOSTRMSTS_FLAG_ENABLED;

        if (strmSts & PDMAUDIOSTRMSTS_FLAG_ENABLED)
            strmSts |= PDMAUDIOSTRMSTS_FLAG_DATA_WRITABLE;
    }
    else
        AssertFailed();

    return strmSts;
}

static DECLCALLBACK(int) drvHostCoreAudioStreamIterate(PPDMIHOSTAUDIO pInterface, PPDMAUDIOSTREAM pStream)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);

    /* Nothing to do here for Core Audio. */
    return VINF_SUCCESS;
}

static DECLCALLBACK(void) drvHostCoreAudioShutdown(PPDMIHOSTAUDIO pInterface)
{
    NOREF(pInterface);
}

static DECLCALLBACK(void *) drvHostCoreAudioQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS pDrvIns       = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTCOREAUDIO  pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIHOSTAUDIO, &pThis->IHostAudio);

    return NULL;
}

 /* Construct a DirectSound Audio driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
static DECLCALLBACK(int) drvHostCoreAudioConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    PDRVHOSTCOREAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);
    LogRel(("Audio: Initializing Core Audio driver\n"));

    /*
     * Init the static parts.
     */
    pThis->pDrvIns                   = pDrvIns;
    /* IBase */
    pDrvIns->IBase.pfnQueryInterface = drvHostCoreAudioQueryInterface;
    /* IHostAudio */
    PDMAUDIO_IHOSTAUDIO_CALLBACKS(drvHostCoreAudio);

    return VINF_SUCCESS;
}

/**
 * Char driver registration record.
 */
const PDMDRVREG g_DrvHostCoreAudio =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "CoreAudio",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Core Audio host driver",
    /* fFlags */
     PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_AUDIO,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVHOSTCOREAUDIO),
    /* pfnConstruct */
    drvHostCoreAudioConstruct,
    /* pfnDestruct */
    NULL,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};

