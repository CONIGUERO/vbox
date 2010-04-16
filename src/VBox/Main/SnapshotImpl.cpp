/* $Id$ */

/** @file
 *
 * COM class implementation for Snapshot and SnapshotMachine in VBoxSVC.
 */

/*
 * Copyright (C) 2006-2010 Sun Microsystems, Inc.
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

#include "Logging.h"
#include "SnapshotImpl.h"

#include "MachineImpl.h"
#include "MediumImpl.h"
#include "Global.h"
#include "ProgressImpl.h"

// @todo these three includes are required for about one or two lines, try
// to remove them and put that code in shared code in MachineImplcpp
#include "SharedFolderImpl.h"
#include "USBControllerImpl.h"
#include "VirtualBoxImpl.h"

#include "AutoCaller.h"

#include <iprt/path.h>
#include <VBox/param.h>
#include <VBox/err.h>

#include <VBox/settings.h>

////////////////////////////////////////////////////////////////////////////////
//
// Globals
//
////////////////////////////////////////////////////////////////////////////////

/**
 *  Progress callback handler for lengthy operations
 *  (corresponds to the FNRTPROGRESS typedef).
 *
 *  @param uPercentage  Completetion precentage (0-100).
 *  @param pvUser       Pointer to the Progress instance.
 */
static DECLCALLBACK(int) progressCallback(unsigned uPercentage, void *pvUser)
{
    IProgress *progress = static_cast<IProgress*>(pvUser);

    /* update the progress object */
    if (progress)
        progress->SetCurrentOperationProgress(uPercentage);

    return VINF_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
//
// Snapshot private data definition
//
////////////////////////////////////////////////////////////////////////////////

typedef std::list< ComObjPtr<Snapshot> > SnapshotsList;

struct Snapshot::Data
{
    Data()
        : pVirtualBox(NULL)
    {
        RTTimeSpecSetMilli(&timeStamp, 0);
    };

    ~Data()
    {}

    const Guid                  uuid;
    Utf8Str                     strName;
    Utf8Str                     strDescription;
    RTTIMESPEC                  timeStamp;
    ComObjPtr<SnapshotMachine>  pMachine;

    /** weak VirtualBox parent */
    VirtualBox * const          pVirtualBox;

    // pParent and llChildren are protected by Machine::snapshotsTreeLockHandle()
    ComObjPtr<Snapshot>         pParent;
    SnapshotsList               llChildren;
};

////////////////////////////////////////////////////////////////////////////////
//
// Constructor / destructor
//
////////////////////////////////////////////////////////////////////////////////

HRESULT Snapshot::FinalConstruct()
{
    LogFlowThisFunc(("\n"));
    return S_OK;
}

void Snapshot::FinalRelease()
{
    LogFlowThisFunc(("\n"));
    uninit();
}

/**
 *  Initializes the instance
 *
 *  @param  aId            id of the snapshot
 *  @param  aName          name of the snapshot
 *  @param  aDescription   name of the snapshot (NULL if no description)
 *  @param  aTimeStamp     timestamp of the snapshot, in ms since 1970-01-01 UTC
 *  @param  aMachine       machine associated with this snapshot
 *  @param  aParent        parent snapshot (NULL if no parent)
 */
HRESULT Snapshot::init(VirtualBox *aVirtualBox,
                       const Guid &aId,
                       const Utf8Str &aName,
                       const Utf8Str &aDescription,
                       const RTTIMESPEC &aTimeStamp,
                       SnapshotMachine *aMachine,
                       Snapshot *aParent)
{
    LogFlowThisFunc(("uuid=%s aParent->uuid=%s\n", aId.toString().c_str(), (aParent) ? aParent->m->uuid.toString().c_str() : ""));

    ComAssertRet(!aId.isEmpty() && !aName.isEmpty() && aMachine, E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    m = new Data;

    /* share parent weakly */
    unconst(m->pVirtualBox) = aVirtualBox;

    m->pParent = aParent;

    unconst(m->uuid) = aId;
    m->strName = aName;
    m->strDescription = aDescription;
    m->timeStamp = aTimeStamp;
    m->pMachine = aMachine;

    if (aParent)
        aParent->m->llChildren.push_back(this);

    /* Confirm a successful initialization when it's the case */
    autoInitSpan.setSucceeded();

    return S_OK;
}

/**
 *  Uninitializes the instance and sets the ready flag to FALSE.
 *  Called either from FinalRelease(), by the parent when it gets destroyed,
 *  or by a third party when it decides this object is no more valid.
 *
 *  Since this manipulates the snapshots tree, the caller must hold the
 *  machine lock in write mode (which protects the snapshots tree)!
 */
void Snapshot::uninit()
{
    LogFlowThisFunc(("\n"));

    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    Assert(m->pMachine->isWriteLockOnCurrentThread());

    // uninit all children
    SnapshotsList::iterator it;
    for (it = m->llChildren.begin();
         it != m->llChildren.end();
         ++it)
    {
        Snapshot *pChild = *it;
        pChild->m->pParent.setNull();
        pChild->uninit();
    }
    m->llChildren.clear();          // this unsets all the ComPtrs and probably calls delete

    if (m->pParent)
        deparent();

    if (m->pMachine)
    {
        m->pMachine->uninit();
        m->pMachine.setNull();
    }

    delete m;
    m = NULL;
}

/**
 *  Delete the current snapshot by removing it from the tree of snapshots
 *  and reparenting its children.
 *
 *  After this, the caller must call uninit() on the snapshot. We can't call
 *  that from here because if we do, the AutoUninitSpan waits forever for
 *  the number of callers to become 0 (it is 1 because of the AutoCaller in here).
 *
 *  NOTE: this does NOT lock the snapshot, it is assumed that the machine state
 *  (and the snapshots tree) is protected by the caller having requested the machine
 *  lock in write mode AND the machine state must be DeletingSnapshot.
 */
void Snapshot::beginSnapshotDelete()
{
    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc()))
        return;

    // caller must have acquired the machine's write lock
    Assert(m->pMachine->mData->mMachineState == MachineState_DeletingSnapshot);
    Assert(m->pMachine->isWriteLockOnCurrentThread());

    // the snapshot must have only one child when being deleted or no children at all
    AssertReturnVoid(m->llChildren.size() <= 1);

    ComObjPtr<Snapshot> parentSnapshot = m->pParent;

    /// @todo (dmik):
    //  when we introduce clones later, deleting the snapshot will affect
    //  the current and first snapshots of clones, if they are direct children
    //  of this snapshot. So we will need to lock machines associated with
    //  child snapshots as well and update mCurrentSnapshot and/or
    //  mFirstSnapshot fields.

    if (this == m->pMachine->mData->mCurrentSnapshot)
    {
        m->pMachine->mData->mCurrentSnapshot = parentSnapshot;

        /* we've changed the base of the current state so mark it as
            * modified as it no longer guaranteed to be its copy */
        m->pMachine->mData->mCurrentStateModified = TRUE;
    }

    if (this == m->pMachine->mData->mFirstSnapshot)
    {
        if (m->llChildren.size() == 1)
        {
            ComObjPtr<Snapshot> childSnapshot = m->llChildren.front();
            m->pMachine->mData->mFirstSnapshot = childSnapshot;
        }
        else
            m->pMachine->mData->mFirstSnapshot.setNull();
    }

    // reparent our children
    for (SnapshotsList::const_iterator it = m->llChildren.begin();
         it != m->llChildren.end();
         ++it)
    {
        ComObjPtr<Snapshot> child = *it;
        // no need to lock, snapshots tree is protected by machine lock
        child->m->pParent = m->pParent;
        if (m->pParent)
            m->pParent->m->llChildren.push_back(child);
    }

    // clear our own children list (since we reparented the children)
    m->llChildren.clear();
}

/**
 * Internal helper that removes "this" from the list of children of its
 * parent. Used in uninit() and other places when reparenting is necessary.
 *
 * The caller must hold the machine lock in write mode (which protects the snapshots tree)!
 */
void Snapshot::deparent()
{
    Assert(m->pMachine->isWriteLockOnCurrentThread());

    SnapshotsList &llParent = m->pParent->m->llChildren;
    for (SnapshotsList::iterator it = llParent.begin();
         it != llParent.end();
         ++it)
    {
        Snapshot *pParentsChild = *it;
        if (this == pParentsChild)
        {
            llParent.erase(it);
            break;
        }
    }

    m->pParent.setNull();
}

////////////////////////////////////////////////////////////////////////////////
//
// ISnapshot public methods
//
////////////////////////////////////////////////////////////////////////////////

STDMETHODIMP Snapshot::COMGETTER(Id)(BSTR *aId)
{
    CheckComArgOutPointerValid(aId);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    m->uuid.toUtf16().cloneTo(aId);
    return S_OK;
}

STDMETHODIMP Snapshot::COMGETTER(Name)(BSTR *aName)
{
    CheckComArgOutPointerValid(aName);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    m->strName.cloneTo(aName);
    return S_OK;
}

/**
 *  @note Locks this object for writing, then calls Machine::onSnapshotChange()
 *  (see its lock requirements).
 */
STDMETHODIMP Snapshot::COMSETTER(Name)(IN_BSTR aName)
{
    CheckComArgStrNotEmptyOrNull(aName);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    Utf8Str strName(aName);

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (m->strName != strName)
    {
        m->strName = strName;

        alock.leave(); /* Important! (child->parent locks are forbidden) */

        return m->pMachine->onSnapshotChange(this);
    }

    return S_OK;
}

STDMETHODIMP Snapshot::COMGETTER(Description)(BSTR *aDescription)
{
    CheckComArgOutPointerValid(aDescription);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    m->strDescription.cloneTo(aDescription);
    return S_OK;
}

STDMETHODIMP Snapshot::COMSETTER(Description)(IN_BSTR aDescription)
{
    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    Utf8Str strDescription(aDescription);

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (m->strDescription != strDescription)
    {
        m->strDescription = strDescription;

        alock.leave(); /* Important! (child->parent locks are forbidden) */

        return m->pMachine->onSnapshotChange(this);
    }

    return S_OK;
}

STDMETHODIMP Snapshot::COMGETTER(TimeStamp)(LONG64 *aTimeStamp)
{
    CheckComArgOutPointerValid(aTimeStamp);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aTimeStamp = RTTimeSpecGetMilli(&m->timeStamp);
    return S_OK;
}

STDMETHODIMP Snapshot::COMGETTER(Online)(BOOL *aOnline)
{
    CheckComArgOutPointerValid(aOnline);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aOnline = !stateFilePath().isEmpty();
    return S_OK;
}

STDMETHODIMP Snapshot::COMGETTER(Machine)(IMachine **aMachine)
{
    CheckComArgOutPointerValid(aMachine);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    m->pMachine.queryInterfaceTo(aMachine);
    return S_OK;
}

STDMETHODIMP Snapshot::COMGETTER(Parent)(ISnapshot **aParent)
{
    CheckComArgOutPointerValid(aParent);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    m->pParent.queryInterfaceTo(aParent);
    return S_OK;
}

STDMETHODIMP Snapshot::COMGETTER(Children)(ComSafeArrayOut(ISnapshot *, aChildren))
{
    CheckComArgOutSafeArrayPointerValid(aChildren);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    // snapshots tree is protected by machine lock
    AutoReadLock alock(m->pMachine COMMA_LOCKVAL_SRC_POS);

    SafeIfaceArray<ISnapshot> collection(m->llChildren);
    collection.detachTo(ComSafeArrayOutArg(aChildren));

    return S_OK;
}

////////////////////////////////////////////////////////////////////////////////
//
// Snapshot public internal methods
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Returns the parent snapshot or NULL if there's none.  Must have caller + locking!
 * @return
 */
const ComObjPtr<Snapshot>& Snapshot::getParent() const
{
    return m->pParent;
}

/**
 *  @note
 *      Must be called from under the object's lock!
 */
const Utf8Str& Snapshot::stateFilePath() const
{
    return m->pMachine->mSSData->mStateFilePath;
}

/**
 * Returns the number of direct child snapshots, without grandchildren.
 * Does not recurse.
 * @return
 */
ULONG Snapshot::getChildrenCount()
{
    AutoCaller autoCaller(this);
    AssertComRC(autoCaller.rc());

    // snapshots tree is protected by machine lock
    AutoReadLock alock(m->pMachine COMMA_LOCKVAL_SRC_POS);

    return (ULONG)m->llChildren.size();
}

/**
 * Implementation method for getAllChildrenCount() so we request the
 * tree lock only once before recursing. Don't call directly.
 * @return
 */
ULONG Snapshot::getAllChildrenCountImpl()
{
    AutoCaller autoCaller(this);
    AssertComRC(autoCaller.rc());

    ULONG count = (ULONG)m->llChildren.size();
    for (SnapshotsList::const_iterator it = m->llChildren.begin();
         it != m->llChildren.end();
         ++it)
    {
        count += (*it)->getAllChildrenCountImpl();
    }

    return count;
}

/**
 * Returns the number of child snapshots including all grandchildren.
 * Recurses into the snapshots tree.
 * @return
 */
ULONG Snapshot::getAllChildrenCount()
{
    AutoCaller autoCaller(this);
    AssertComRC(autoCaller.rc());

    // snapshots tree is protected by machine lock
    AutoReadLock alock(m->pMachine COMMA_LOCKVAL_SRC_POS);

    return getAllChildrenCountImpl();
}

/**
 * Returns the SnapshotMachine that this snapshot belongs to.
 * Caller must hold the snapshot's object lock!
 * @return
 */
const ComObjPtr<SnapshotMachine>& Snapshot::getSnapshotMachine() const
{
    return m->pMachine;
}

/**
 * Returns the UUID of this snapshot.
 * Caller must hold the snapshot's object lock!
 * @return
 */
Guid Snapshot::getId() const
{
    return m->uuid;
}

/**
 * Returns the name of this snapshot.
 * Caller must hold the snapshot's object lock!
 * @return
 */
const Utf8Str& Snapshot::getName() const
{
    return m->strName;
}

/**
 * Returns the time stamp of this snapshot.
 * Caller must hold the snapshot's object lock!
 * @return
 */
RTTIMESPEC Snapshot::getTimeStamp() const
{
    return m->timeStamp;
}

/**
 *  Searches for a snapshot with the given ID among children, grand-children,
 *  etc. of this snapshot. This snapshot itself is also included in the search.
 *
 *  Caller must hold the machine lock (which protects the snapshots tree!)
 */
ComObjPtr<Snapshot> Snapshot::findChildOrSelf(IN_GUID aId)
{
    ComObjPtr<Snapshot> child;

    AutoCaller autoCaller(this);
    AssertComRC(autoCaller.rc());

    // no need to lock, uuid is const
    if (m->uuid == aId)
        child = this;
    else
    {
        for (SnapshotsList::const_iterator it = m->llChildren.begin();
             it != m->llChildren.end();
             ++it)
        {
            if ((child = (*it)->findChildOrSelf(aId)))
                break;
        }
    }

    return child;
}

/**
 *  Searches for a first snapshot with the given name among children,
 *  grand-children, etc. of this snapshot. This snapshot itself is also included
 *  in the search.
 *
 *  Caller must hold the machine lock (which protects the snapshots tree!)
 */
ComObjPtr<Snapshot> Snapshot::findChildOrSelf(const Utf8Str &aName)
{
    ComObjPtr<Snapshot> child;
    AssertReturn(!aName.isEmpty(), child);

    AutoCaller autoCaller(this);
    AssertComRC(autoCaller.rc());

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (m->strName == aName)
        child = this;
    else
    {
        alock.release();
        for (SnapshotsList::const_iterator it = m->llChildren.begin();
             it != m->llChildren.end();
             ++it)
        {
            if ((child = (*it)->findChildOrSelf(aName)))
                break;
        }
    }

    return child;
}

/**
 * Internal implementation for Snapshot::updateSavedStatePaths (below).
 * @param aOldPath
 * @param aNewPath
 */
void Snapshot::updateSavedStatePathsImpl(const char *aOldPath, const char *aNewPath)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    const Utf8Str &path = m->pMachine->mSSData->mStateFilePath;
    LogFlowThisFunc(("Snap[%s].statePath={%s}\n", m->strName.c_str(), path.c_str()));

    /* state file may be NULL (for offline snapshots) */
    if (    path.length()
         && RTPathStartsWith(path.c_str(), aOldPath)
       )
    {
        m->pMachine->mSSData->mStateFilePath = Utf8StrFmt("%s%s", aNewPath, path.raw() + strlen(aOldPath));

        LogFlowThisFunc(("-> updated: {%s}\n", path.raw()));
    }

    for (SnapshotsList::const_iterator it = m->llChildren.begin();
         it != m->llChildren.end();
         ++it)
    {
        Snapshot *pChild = *it;
        pChild->updateSavedStatePathsImpl(aOldPath, aNewPath);
    }
}

/**
 *  Checks if the specified path change affects the saved state file path of
 *  this snapshot or any of its (grand-)children and updates it accordingly.
 *
 *  Intended to be called by Machine::openConfigLoader() only.
 *
 *  @param aOldPath old path (full)
 *  @param aNewPath new path (full)
 *
 *  @note Locks the machine (for the snapshots tree) +  this object + children for writing.
 */
void Snapshot::updateSavedStatePaths(const char *aOldPath, const char *aNewPath)
{
    LogFlowThisFunc(("aOldPath={%s} aNewPath={%s}\n", aOldPath, aNewPath));

    AssertReturnVoid(aOldPath);
    AssertReturnVoid(aNewPath);

    AutoCaller autoCaller(this);
    AssertComRC(autoCaller.rc());

    // snapshots tree is protected by machine lock
    AutoWriteLock alock(m->pMachine COMMA_LOCKVAL_SRC_POS);

    // call the implementation under the tree lock
    updateSavedStatePathsImpl(aOldPath, aNewPath);
}

/**
 * Internal implementation for Snapshot::saveSnapshot (below). Caller has
 * requested the snapshots tree (machine) lock.
 *
 * @param aNode
 * @param aAttrsOnly
 * @return
 */
HRESULT Snapshot::saveSnapshotImpl(settings::Snapshot &data, bool aAttrsOnly)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    data.uuid = m->uuid;
    data.strName = m->strName;
    data.timestamp = m->timeStamp;
    data.strDescription = m->strDescription;

    if (aAttrsOnly)
        return S_OK;

    /* stateFile (optional) */
    if (!stateFilePath().isEmpty())
        /* try to make the file name relative to the settings file dir */
        m->pMachine->calculateRelativePath(stateFilePath(), data.strStateFile);
    else
        data.strStateFile.setNull();

    HRESULT rc = m->pMachine->saveHardware(data.hardware);
    if (FAILED(rc)) return rc;

    rc = m->pMachine->saveStorageControllers(data.storage);
    if (FAILED(rc)) return rc;

    alock.release();

    data.llChildSnapshots.clear();

    if (m->llChildren.size())
    {
        for (SnapshotsList::const_iterator it = m->llChildren.begin();
             it != m->llChildren.end();
             ++it)
        {
            settings::Snapshot snap;
            rc = (*it)->saveSnapshotImpl(snap, aAttrsOnly);
            if (FAILED(rc)) return rc;

            data.llChildSnapshots.push_back(snap);
        }
    }

    return S_OK;
}

/**
 *  Saves the given snapshot and all its children (unless \a aAttrsOnly is true).
 *  It is assumed that the given node is empty (unless \a aAttrsOnly is true).
 *
 *  @param aNode        <Snapshot> node to save the snapshot to.
 *  @param aSnapshot    Snapshot to save.
 *  @param aAttrsOnly   If true, only updatge user-changeable attrs.
 */
HRESULT Snapshot::saveSnapshot(settings::Snapshot &data, bool aAttrsOnly)
{
    // snapshots tree is protected by machine lock
    AutoReadLock alock(m->pMachine COMMA_LOCKVAL_SRC_POS);

    return saveSnapshotImpl(data, aAttrsOnly);
}

////////////////////////////////////////////////////////////////////////////////
//
// SnapshotMachine implementation
//
////////////////////////////////////////////////////////////////////////////////

DEFINE_EMPTY_CTOR_DTOR(SnapshotMachine)

HRESULT SnapshotMachine::FinalConstruct()
{
    LogFlowThisFunc(("\n"));

    return S_OK;
}

void SnapshotMachine::FinalRelease()
{
    LogFlowThisFunc(("\n"));

    uninit();
}

/**
 *  Initializes the SnapshotMachine object when taking a snapshot.
 *
 *  @param aSessionMachine  machine to take a snapshot from
 *  @param aSnapshotId      snapshot ID of this snapshot machine
 *  @param aStateFilePath   file where the execution state will be later saved
 *                          (or NULL for the offline snapshot)
 *
 *  @note The aSessionMachine must be locked for writing.
 */
HRESULT SnapshotMachine::init(SessionMachine *aSessionMachine,
                              IN_GUID aSnapshotId,
                              const Utf8Str &aStateFilePath)
{
    LogFlowThisFuncEnter();
    LogFlowThisFunc(("mName={%ls}\n", aSessionMachine->mUserData->mName.raw()));

    AssertReturn(aSessionMachine && !Guid(aSnapshotId).isEmpty(), E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    AssertReturn(aSessionMachine->isWriteLockOnCurrentThread(), E_FAIL);

    mSnapshotId = aSnapshotId;

    /* memorize the primary Machine instance (i.e. not SessionMachine!) */
    unconst(mPeer) = aSessionMachine->mPeer;
    /* share the parent pointer */
    unconst(mParent) = mPeer->mParent;

    /* take the pointer to Data to share */
    mData.share(mPeer->mData);

    /* take the pointer to UserData to share (our UserData must always be the
     * same as Machine's data) */
    mUserData.share(mPeer->mUserData);
    /* make a private copy of all other data (recent changes from SessionMachine) */
    mHWData.attachCopy(aSessionMachine->mHWData);
    mMediaData.attachCopy(aSessionMachine->mMediaData);

    /* SSData is always unique for SnapshotMachine */
    mSSData.allocate();
    mSSData->mStateFilePath = aStateFilePath;

    HRESULT rc = S_OK;

    /* create copies of all shared folders (mHWData after attiching a copy
     * contains just references to original objects) */
    for (HWData::SharedFolderList::iterator it = mHWData->mSharedFolders.begin();
         it != mHWData->mSharedFolders.end();
         ++it)
    {
        ComObjPtr<SharedFolder> folder;
        folder.createObject();
        rc = folder->initCopy(this, *it);
        if (FAILED(rc)) return rc;
        *it = folder;
    }

    /* associate hard disks with the snapshot
     * (Machine::uninitDataAndChildObjects() will deassociate at destruction) */
    for (MediaData::AttachmentList::const_iterator it = mMediaData->mAttachments.begin();
         it != mMediaData->mAttachments.end();
         ++it)
    {
        MediumAttachment *pAtt = *it;
        Medium *pMedium = pAtt->getMedium();
        if (pMedium) // can be NULL for non-harddisk
        {
            rc = pMedium->attachTo(mData->mUuid, mSnapshotId);
            AssertComRC(rc);
        }
    }

    /* create copies of all storage controllers (mStorageControllerData
     * after attaching a copy contains just references to original objects) */
    mStorageControllers.allocate();
    for (StorageControllerList::const_iterator
         it = aSessionMachine->mStorageControllers->begin();
         it != aSessionMachine->mStorageControllers->end();
         ++it)
    {
        ComObjPtr<StorageController> ctrl;
        ctrl.createObject();
        ctrl->initCopy(this, *it);
        mStorageControllers->push_back(ctrl);
    }

    /* create all other child objects that will be immutable private copies */

    unconst(mBIOSSettings).createObject();
    mBIOSSettings->initCopy(this, mPeer->mBIOSSettings);

#ifdef VBOX_WITH_VRDP
    unconst(mVRDPServer).createObject();
    mVRDPServer->initCopy(this, mPeer->mVRDPServer);
#endif

    unconst(mAudioAdapter).createObject();
    mAudioAdapter->initCopy(this, mPeer->mAudioAdapter);

    unconst(mUSBController).createObject();
    mUSBController->initCopy(this, mPeer->mUSBController);

    for (ULONG slot = 0; slot < RT_ELEMENTS(mNetworkAdapters); slot++)
    {
        unconst(mNetworkAdapters[slot]).createObject();
        mNetworkAdapters[slot]->initCopy(this, mPeer->mNetworkAdapters[slot]);
    }

    for (ULONG slot = 0; slot < RT_ELEMENTS(mSerialPorts); slot++)
    {
        unconst(mSerialPorts[slot]).createObject();
        mSerialPorts[slot]->initCopy(this, mPeer->mSerialPorts[slot]);
    }

    for (ULONG slot = 0; slot < RT_ELEMENTS(mParallelPorts); slot++)
    {
        unconst(mParallelPorts[slot]).createObject();
        mParallelPorts[slot]->initCopy(this, mPeer->mParallelPorts[slot]);
    }

    /* Confirm a successful initialization when it's the case */
    autoInitSpan.setSucceeded();

    LogFlowThisFuncLeave();
    return S_OK;
}

/**
 *  Initializes the SnapshotMachine object when loading from the settings file.
 *
 *  @param aMachine machine the snapshot belngs to
 *  @param aHWNode          <Hardware> node
 *  @param aHDAsNode        <HardDiskAttachments> node
 *  @param aSnapshotId      snapshot ID of this snapshot machine
 *  @param aStateFilePath   file where the execution state is saved
 *                          (or NULL for the offline snapshot)
 *
 *  @note Doesn't lock anything.
 */
HRESULT SnapshotMachine::init(Machine *aMachine,
                              const settings::Hardware &hardware,
                              const settings::Storage &storage,
                              IN_GUID aSnapshotId,
                              const Utf8Str &aStateFilePath)
{
    LogFlowThisFuncEnter();
    LogFlowThisFunc(("mName={%ls}\n", aMachine->mUserData->mName.raw()));

    AssertReturn(aMachine &&  !Guid(aSnapshotId).isEmpty(), E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    /* Don't need to lock aMachine when VirtualBox is starting up */

    mSnapshotId = aSnapshotId;

    /* memorize the primary Machine instance */
    unconst(mPeer) = aMachine;
    /* share the parent pointer */
    unconst(mParent) = mPeer->mParent;

    /* take the pointer to Data to share */
    mData.share(mPeer->mData);
    /*
     *  take the pointer to UserData to share
     *  (our UserData must always be the same as Machine's data)
     */
    mUserData.share(mPeer->mUserData);
    /* allocate private copies of all other data (will be loaded from settings) */
    mHWData.allocate();
    mMediaData.allocate();
    mStorageControllers.allocate();

    /* SSData is always unique for SnapshotMachine */
    mSSData.allocate();
    mSSData->mStateFilePath = aStateFilePath;

    /* create all other child objects that will be immutable private copies */

    unconst(mBIOSSettings).createObject();
    mBIOSSettings->init(this);

#ifdef VBOX_WITH_VRDP
    unconst(mVRDPServer).createObject();
    mVRDPServer->init(this);
#endif

    unconst(mAudioAdapter).createObject();
    mAudioAdapter->init(this);

    unconst(mUSBController).createObject();
    mUSBController->init(this);

    for (ULONG slot = 0; slot < RT_ELEMENTS(mNetworkAdapters); slot++)
    {
        unconst(mNetworkAdapters[slot]).createObject();
        mNetworkAdapters[slot]->init(this, slot);
    }

    for (ULONG slot = 0; slot < RT_ELEMENTS(mSerialPorts); slot++)
    {
        unconst(mSerialPorts[slot]).createObject();
        mSerialPorts[slot]->init(this, slot);
    }

    for (ULONG slot = 0; slot < RT_ELEMENTS(mParallelPorts); slot++)
    {
        unconst(mParallelPorts[slot]).createObject();
        mParallelPorts[slot]->init(this, slot);
    }

    /* load hardware and harddisk settings */

    HRESULT rc = loadHardware(hardware);
    if (SUCCEEDED(rc))
        rc = loadStorageControllers(storage, &mSnapshotId);

    if (SUCCEEDED(rc))
        /* commit all changes made during the initialization */
        commit();   // @todo r=dj why do we need a commit in init?!? this is very expensive

    /* Confirm a successful initialization when it's the case */
    if (SUCCEEDED(rc))
        autoInitSpan.setSucceeded();

    LogFlowThisFuncLeave();
    return rc;
}

/**
 *  Uninitializes this SnapshotMachine object.
 */
void SnapshotMachine::uninit()
{
    LogFlowThisFuncEnter();

    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    uninitDataAndChildObjects();

    /* free the essential data structure last */
    mData.free();

    unconst(mParent) = NULL;
    unconst(mPeer) = NULL;

    LogFlowThisFuncLeave();
}

/**
 *  Overrides VirtualBoxBase::lockHandle() in order to share the lock handle
 *  with the primary Machine instance (mPeer).
 */
RWLockHandle *SnapshotMachine::lockHandle() const
{
    AssertReturn(mPeer != NULL, NULL);
    return mPeer->lockHandle();
}

////////////////////////////////////////////////////////////////////////////////
//
// SnapshotMachine public internal methods
//
////////////////////////////////////////////////////////////////////////////////

/**
 *  Called by the snapshot object associated with this SnapshotMachine when
 *  snapshot data such as name or description is changed.
 *
 *  @note Locks this object for writing.
 */
HRESULT SnapshotMachine::onSnapshotChange(Snapshot *aSnapshot)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    //     mPeer->saveAllSnapshots();  @todo

    /* inform callbacks */
    mParent->onSnapshotChange(mData->mUuid, aSnapshot->getId());

    return S_OK;
}

////////////////////////////////////////////////////////////////////////////////
//
// SessionMachine task records
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Abstract base class for SessionMachine::RestoreSnapshotTask and
 * SessionMachine::DeleteSnapshotTask. This is necessary since
 * RTThreadCreate cannot call a method as its thread function, so
 * instead we have it call the static SessionMachine::taskHandler,
 * which can then call the handler() method in here (implemented
 * by the children).
 */
struct SessionMachine::SnapshotTask
{
    SnapshotTask(SessionMachine *m,
                 Progress *p,
                 Snapshot *s)
        : pMachine(m),
          pProgress(p),
          machineStateBackup(m->mData->mMachineState), // save the current machine state
          pSnapshot(s)
    {}

    void modifyBackedUpState(MachineState_T s)
    {
        *const_cast<MachineState_T*>(&machineStateBackup) = s;
    }

    virtual void handler() = 0;

    ComObjPtr<SessionMachine>       pMachine;
    ComObjPtr<Progress>             pProgress;
    const MachineState_T            machineStateBackup;
    ComObjPtr<Snapshot>             pSnapshot;
};

/** Restore snapshot state task */
struct SessionMachine::RestoreSnapshotTask
    : public SessionMachine::SnapshotTask
{
    RestoreSnapshotTask(SessionMachine *m,
                        Progress *p,
                        Snapshot *s,
                        ULONG ulStateFileSizeMB)
        : SnapshotTask(m, p, s),
          m_ulStateFileSizeMB(ulStateFileSizeMB)
    {}

    void handler()
    {
        pMachine->restoreSnapshotHandler(*this);
    }

    ULONG       m_ulStateFileSizeMB;
};

/** Delete snapshot task */
struct SessionMachine::DeleteSnapshotTask
    : public SessionMachine::SnapshotTask
{
    DeleteSnapshotTask(SessionMachine *m,
                       Progress *p,
                       Snapshot *s)
        : SnapshotTask(m, p, s)
    {}

    void handler()
    {
        pMachine->deleteSnapshotHandler(*this);
    }

private:
    DeleteSnapshotTask(const SnapshotTask &task)
        : SnapshotTask(task)
    {}
};

/**
 * Static SessionMachine method that can get passed to RTThreadCreate to
 * have a thread started for a SnapshotTask. See SnapshotTask above.
 *
 * This calls either RestoreSnapshotTask::handler() or DeleteSnapshotTask::handler().
 */

/* static */ DECLCALLBACK(int) SessionMachine::taskHandler(RTTHREAD /* thread */, void *pvUser)
{
    AssertReturn(pvUser, VERR_INVALID_POINTER);

    SnapshotTask *task = static_cast<SnapshotTask*>(pvUser);
    task->handler();

    // it's our responsibility to delete the task
    delete task;

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
//
// TakeSnapshot methods (SessionMachine and related tasks)
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Implementation for IInternalMachineControl::beginTakingSnapshot().
 *
 * Gets called indirectly from Console::TakeSnapshot, which creates a
 * progress object in the client and then starts a thread
 * (Console::fntTakeSnapshotWorker) which then calls this.
 *
 * In other words, the asynchronous work for taking snapshots takes place
 * on the _client_ (in the Console). This is different from restoring
 * or deleting snapshots, which start threads on the server.
 *
 * This does the server-side work of taking a snapshot: it creates diffencing
 * images for all hard disks attached to the machine and then creates a
 * Snapshot object with a corresponding SnapshotMachine to save the VM settings.
 *
 * The client's fntTakeSnapshotWorker() blocks while this takes place.
 * After this returns successfully, fntTakeSnapshotWorker() will begin
 * saving the machine state to the snapshot object and reconfigure the
 * hard disks.
 *
 * When the console is done, it calls SessionMachine::EndTakingSnapshot().
 *
 *  @note Locks mParent + this object for writing.
 *
 * @param aInitiator in: The console on which Console::TakeSnapshot was called.
 * @param aName  in: The name for the new snapshot.
 * @param aDescription  in: A description for the new snapshot.
 * @param aConsoleProgress  in: The console's (client's) progress object.
 * @param fTakingSnapshotOnline  in: True if an online snapshot is being taken (i.e. machine is running).
 * @param aStateFilePath out: name of file in snapshots folder to which the console should write the VM state.
 * @return
 */
STDMETHODIMP SessionMachine::BeginTakingSnapshot(IConsole *aInitiator,
                                                 IN_BSTR aName,
                                                 IN_BSTR aDescription,
                                                 IProgress *aConsoleProgress,
                                                 BOOL fTakingSnapshotOnline,
                                                 BSTR *aStateFilePath)
{
    LogFlowThisFuncEnter();

    AssertReturn(aInitiator && aName, E_INVALIDARG);
    AssertReturn(aStateFilePath, E_POINTER);

    LogFlowThisFunc(("aName='%ls' fTakingSnapshotOnline=%RTbool\n", aName, fTakingSnapshotOnline));

    AutoCaller autoCaller(this);
    AssertComRCReturn(autoCaller.rc(), autoCaller.rc());

    // if this becomes true, we need to call VirtualBox::saveSettings() in the end
    bool fNeedsSaveSettings = false;

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    AssertReturn(    !Global::IsOnlineOrTransient(mData->mMachineState)
                  || mData->mMachineState == MachineState_Running
                  || mData->mMachineState == MachineState_Paused, E_FAIL);
    AssertReturn(mSnapshotData.mLastState == MachineState_Null, E_FAIL);
    AssertReturn(mSnapshotData.mSnapshot.isNull(), E_FAIL);

    if (    !fTakingSnapshotOnline
         && mData->mMachineState != MachineState_Saved
       )
    {
        /* save all current settings to ensure current changes are committed and
         * hard disks are fixed up */
        HRESULT rc = saveSettings(NULL);
                // no need to check for whether VirtualBox.xml needs changing since
                // we can't have a machine XML rename pending at this point
        if (FAILED(rc)) return rc;
    }

    /* create an ID for the snapshot */
    Guid snapshotId;
    snapshotId.create();

    Utf8Str strStateFilePath;
    /* stateFilePath is null when the machine is not online nor saved */
    if (    fTakingSnapshotOnline
         || mData->mMachineState == MachineState_Saved)
    {
        strStateFilePath = Utf8StrFmt("%ls%c{%RTuuid}.sav",
                                      mUserData->mSnapshotFolderFull.raw(),
                                      RTPATH_DELIMITER,
                                      snapshotId.ptr());
        /* ensure the directory for the saved state file exists */
        HRESULT rc = VirtualBox::ensureFilePathExists(strStateFilePath);
        if (FAILED(rc)) return rc;
    }

    /* create a snapshot machine object */
    ComObjPtr<SnapshotMachine> snapshotMachine;
    snapshotMachine.createObject();
    HRESULT rc = snapshotMachine->init(this, snapshotId, strStateFilePath);
    AssertComRCReturn(rc, rc);

    /* create a snapshot object */
    RTTIMESPEC time;
    ComObjPtr<Snapshot> pSnapshot;
    pSnapshot.createObject();
    rc = pSnapshot->init(mParent,
                         snapshotId,
                         aName,
                         aDescription,
                         *RTTimeNow(&time),
                         snapshotMachine,
                         mData->mCurrentSnapshot);
    AssertComRCReturnRC(rc);

    /* fill in the snapshot data */
    mSnapshotData.mLastState = mData->mMachineState;
    mSnapshotData.mSnapshot = pSnapshot;

    try
    {
        LogFlowThisFunc(("Creating differencing hard disks (online=%d)...\n",
                         fTakingSnapshotOnline));

        // backup the media data so we can recover if things goes wrong along the day;
        // the matching commit() is in fixupMedia() during endSnapshot()
        setModified(IsModified_Storage);
        mMediaData.backup();

        /* Console::fntTakeSnapshotWorker and friends expects this. */
        if (mSnapshotData.mLastState == MachineState_Running)
            setMachineState(MachineState_LiveSnapshotting);
        else
            setMachineState(MachineState_Saving); /** @todo Confusing! Saving is used for both online and offline snapshots. */

        /* create new differencing hard disks and attach them to this machine */
        rc = createImplicitDiffs(mUserData->mSnapshotFolderFull,
                                 aConsoleProgress,
                                 1,            // operation weight; must be the same as in Console::TakeSnapshot()
                                 !!fTakingSnapshotOnline,
                                 &fNeedsSaveSettings);
        if (FAILED(rc))
            throw rc;

        if (mSnapshotData.mLastState == MachineState_Saved)
        {
            Utf8Str stateFrom = mSSData->mStateFilePath;
            Utf8Str stateTo = mSnapshotData.mSnapshot->stateFilePath();

            LogFlowThisFunc(("Copying the execution state from '%s' to '%s'...\n",
                             stateFrom.raw(), stateTo.raw()));

            aConsoleProgress->SetNextOperation(Bstr(tr("Copying the execution state")),
                                               1);        // weight

            /* Leave the lock before a lengthy operation (machine is protected
             * by "Saving" machine state now) */
            alock.release();

            /* copy the state file */
            int vrc = RTFileCopyEx(stateFrom.c_str(),
                                   stateTo.c_str(),
                                   0,
                                   progressCallback,
                                   aConsoleProgress);
            alock.acquire();

            if (RT_FAILURE(vrc))
                /** @todo r=bird: Delete stateTo when appropriate. */
                throw setError(E_FAIL,
                               tr("Could not copy the state file '%s' to '%s' (%Rrc)"),
                               stateFrom.raw(),
                               stateTo.raw(),
                               vrc);
        }
    }
    catch (HRESULT hrc)
    {
        LogThisFunc(("Caught %Rhrc [%s]\n", hrc, Global::stringifyMachineState(mData->mMachineState) ));
        if (    mSnapshotData.mLastState != mData->mMachineState
             && (   mSnapshotData.mLastState == MachineState_Running
                  ? mData->mMachineState == MachineState_LiveSnapshotting
                  : mData->mMachineState == MachineState_Saving)
           )
            setMachineState(mSnapshotData.mLastState);

        pSnapshot->uninit();
        pSnapshot.setNull();
        mSnapshotData.mLastState = MachineState_Null;
        mSnapshotData.mSnapshot.setNull();

        rc = hrc;

        // @todo r=dj what with the implicit diff that we created above? this is never cleaned up
    }

    if (fTakingSnapshotOnline && SUCCEEDED(rc))
        strStateFilePath.cloneTo(aStateFilePath);
    else
        *aStateFilePath = NULL;

    // @todo r=dj normally we would need to save the settings if fNeedsSaveSettings was set to true,
    // but since we have no error handling that cleans up the diff image that might have gotten created,
    // there's no point in saving the disk registry at this point either... this needs fixing.

    LogFlowThisFunc(("LEAVE - %Rhrc [%s]\n", rc, Global::stringifyMachineState(mData->mMachineState) ));
    return rc;
}

/**
 * Implementation for IInternalMachineControl::endTakingSnapshot().
 *
 * Called by the Console when it's done saving the VM state into the snapshot
 * (if online) and reconfiguring the hard disks. See BeginTakingSnapshot() above.
 *
 * This also gets called if the console part of snapshotting failed after the
 * BeginTakingSnapshot() call, to clean up the server side.
 *
 * @note Locks VirtualBox and this object for writing.
 *
 * @param aSuccess Whether Console was successful with the client-side snapshot things.
 * @return
 */
STDMETHODIMP SessionMachine::EndTakingSnapshot(BOOL aSuccess)
{
    LogFlowThisFunc(("\n"));

    AutoCaller autoCaller(this);
    AssertComRCReturn (autoCaller.rc(), autoCaller.rc());

    AutoWriteLock machineLock(this COMMA_LOCKVAL_SRC_POS);

    AssertReturn(   !aSuccess
                 || (    (    mData->mMachineState == MachineState_Saving
                           || mData->mMachineState == MachineState_LiveSnapshotting)
                      && mSnapshotData.mLastState != MachineState_Null
                      && !mSnapshotData.mSnapshot.isNull()
                    )
                 , E_FAIL);

    /*
     * Restore the state we had when BeginTakingSnapshot() was called,
     * Console::fntTakeSnapshotWorker restores its local copy when we return.
     * If the state was Running, then let Console::fntTakeSnapshotWorker do it
     * all to avoid races.
     */
    if (    mData->mMachineState != mSnapshotData.mLastState
         && mSnapshotData.mLastState != MachineState_Running
       )
        setMachineState(mSnapshotData.mLastState);

    ComObjPtr<Snapshot> pOldFirstSnap = mData->mFirstSnapshot;
    ComObjPtr<Snapshot> pOldCurrentSnap = mData->mCurrentSnapshot;

    bool fOnline = Global::IsOnline(mSnapshotData.mLastState);

    HRESULT rc = S_OK;

    if (aSuccess)
    {
        // new snapshot becomes the current one
        mData->mCurrentSnapshot = mSnapshotData.mSnapshot;

        /* memorize the first snapshot if necessary */
        if (!mData->mFirstSnapshot)
            mData->mFirstSnapshot = mData->mCurrentSnapshot;

        int flSaveSettings = SaveS_Force;       // do not do a deep compare in machine settings,
                                                // snapshots change, so we know we need to save
        if (!fOnline)
            /* the machine was powered off or saved when taking a snapshot, so
             * reset the mCurrentStateModified flag */
            flSaveSettings |= SaveS_ResetCurStateModified;

        rc = saveSettings(NULL, flSaveSettings);
                // no need to change for whether VirtualBox.xml needs saving since
                // we'll save the global settings below anyway
    }

    if (aSuccess && SUCCEEDED(rc))
    {
        /* associate old hard disks with the snapshot and do locking/unlocking*/
        commitMedia(fOnline);

        /* inform callbacks */
        mParent->onSnapshotTaken(mData->mUuid,
                                 mSnapshotData.mSnapshot->getId());
    }
    else
    {
        /* delete all differencing hard disks created (this will also attach
         * their parents back by rolling back mMediaData) */
        rollbackMedia();

        mData->mFirstSnapshot = pOldFirstSnap;      // might have been changed above
        mData->mCurrentSnapshot = pOldCurrentSnap;      // might have been changed above

        /* delete the saved state file (it might have been already created) */
        if (mSnapshotData.mSnapshot->stateFilePath().length())
            RTFileDelete(mSnapshotData.mSnapshot->stateFilePath().c_str());

        mSnapshotData.mSnapshot->uninit();
    }

    /* clear out the snapshot data */
    mSnapshotData.mLastState = MachineState_Null;
    mSnapshotData.mSnapshot.setNull();

    // save VirtualBox.xml (media registry most probably changed with diff image)
    machineLock.release();
    AutoWriteLock vboxLock(mParent COMMA_LOCKVAL_SRC_POS);
    mParent->saveSettings();

    return rc;
}

////////////////////////////////////////////////////////////////////////////////
//
// RestoreSnapshot methods (SessionMachine and related tasks)
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Implementation for IInternalMachineControl::restoreSnapshot().
 *
 * Gets called from Console::RestoreSnapshot(), and that's basically the
 * only thing Console does. Restoring a snapshot happens entirely on the
 * server side since the machine cannot be running.
 *
 * This creates a new thread that does the work and returns a progress
 * object to the client which is then returned to the caller of
 * Console::RestoreSnapshot().
 *
 * Actual work then takes place in RestoreSnapshotTask::handler().
 *
 * @note Locks this + children objects for writing!
 *
 * @param aInitiator in: rhe console on which Console::RestoreSnapshot was called.
 * @param aSnapshot in: the snapshot to restore.
 * @param aMachineState in: client-side machine state.
 * @param aProgress out: progress object to monitor restore thread.
 * @return
 */
STDMETHODIMP SessionMachine::RestoreSnapshot(IConsole *aInitiator,
                                             ISnapshot *aSnapshot,
                                             MachineState_T *aMachineState,
                                             IProgress **aProgress)
{
    LogFlowThisFuncEnter();

    AssertReturn(aInitiator, E_INVALIDARG);
    AssertReturn(aSnapshot && aMachineState && aProgress, E_POINTER);

    AutoCaller autoCaller(this);
    AssertComRCReturn(autoCaller.rc(), autoCaller.rc());

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    // machine must not be running
    ComAssertRet(!Global::IsOnlineOrTransient(mData->mMachineState),
                 E_FAIL);

    ComObjPtr<Snapshot> pSnapshot(static_cast<Snapshot*>(aSnapshot));
    ComObjPtr<SnapshotMachine> pSnapMachine = pSnapshot->getSnapshotMachine();

    // create a progress object. The number of operations is:
    // 1 (preparing) + # of hard disks + 1 (if we need to copy the saved state file) */
    LogFlowThisFunc(("Going thru snapshot machine attachments to determine progress setup\n"));

    ULONG ulOpCount = 1;            // one for preparations
    ULONG ulTotalWeight = 1;        // one for preparations
    for (MediaData::AttachmentList::iterator it = pSnapMachine->mMediaData->mAttachments.begin();
         it != pSnapMachine->mMediaData->mAttachments.end();
         ++it)
    {
        ComObjPtr<MediumAttachment> &pAttach = *it;
        AutoReadLock attachLock(pAttach COMMA_LOCKVAL_SRC_POS);
        if (pAttach->getType() == DeviceType_HardDisk)
        {
            ++ulOpCount;
            ++ulTotalWeight;         // assume one MB weight for each differencing hard disk to manage
            Assert(pAttach->getMedium());
            LogFlowThisFunc(("op %d: considering hard disk attachment %s\n", ulOpCount, pAttach->getMedium()->getName().c_str()));
        }
    }

    ULONG ulStateFileSizeMB = 0;
    if (pSnapshot->stateFilePath().length())
    {
        ++ulOpCount;      // one for the saved state

        uint64_t ullSize;
        int irc = RTFileQuerySize(pSnapshot->stateFilePath().c_str(), &ullSize);
        if (!RT_SUCCESS(irc))
            // if we can't access the file here, then we'll be doomed later also, so fail right away
            setError(E_FAIL, tr("Cannot access state file '%s', runtime error, %Rra"), pSnapshot->stateFilePath().c_str(), irc);
        if (ullSize == 0) // avoid division by zero
            ullSize = _1M;

        ulStateFileSizeMB = (ULONG)(ullSize / _1M);
        LogFlowThisFunc(("op %d: saved state file '%s' has %RI64 bytes (%d MB)\n",
                         ulOpCount, pSnapshot->stateFilePath().raw(), ullSize, ulStateFileSizeMB));

        ulTotalWeight += ulStateFileSizeMB;
    }

    ComObjPtr<Progress> pProgress;
    pProgress.createObject();
    pProgress->init(mParent, aInitiator,
                    BstrFmt(tr("Restoring snapshot '%s'"), pSnapshot->getName().c_str()),
                    FALSE /* aCancelable */,
                    ulOpCount,
                    ulTotalWeight,
                    Bstr(tr("Restoring machine settings")),
                    1);

    /* create and start the task on a separate thread (note that it will not
     * start working until we release alock) */
    RestoreSnapshotTask *task = new RestoreSnapshotTask(this,
                                                        pProgress,
                                                        pSnapshot,
                                                        ulStateFileSizeMB);
    int vrc = RTThreadCreate(NULL,
                             taskHandler,
                             (void*)task,
                             0,
                             RTTHREADTYPE_MAIN_WORKER,
                             0,
                             "RestoreSnap");
    if (RT_FAILURE(vrc))
    {
        delete task;
        ComAssertRCRet(vrc, E_FAIL);
    }

    /* set the proper machine state (note: after creating a Task instance) */
    setMachineState(MachineState_RestoringSnapshot);

    /* return the progress to the caller */
    pProgress.queryInterfaceTo(aProgress);

    /* return the new state to the caller */
    *aMachineState = mData->mMachineState;

    LogFlowThisFuncLeave();

    return S_OK;
}

/**
 * Worker method for the restore snapshot thread created by SessionMachine::RestoreSnapshot().
 * This method gets called indirectly through SessionMachine::taskHandler() which then
 * calls RestoreSnapshotTask::handler().
 *
 * The RestoreSnapshotTask contains the progress object returned to the console by
 * SessionMachine::RestoreSnapshot, through which progress and results are reported.
 *
 * @note Locks mParent + this object for writing.
 *
 * @param aTask Task data.
 */
void SessionMachine::restoreSnapshotHandler(RestoreSnapshotTask &aTask)
{
    LogFlowThisFuncEnter();

    AutoCaller autoCaller(this);

    LogFlowThisFunc(("state=%d\n", autoCaller.state()));
    if (!autoCaller.isOk())
    {
        /* we might have been uninitialized because the session was accidentally
         * closed by the client, so don't assert */
        aTask.pProgress->notifyComplete(E_FAIL,
                                        COM_IIDOF(IMachine),
                                        getComponentName(),
                                        tr("The session has been accidentally closed"));

        LogFlowThisFuncLeave();
        return;
    }

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    /* discard all current changes to mUserData (name, OSType etc.) (note that
     * the machine is powered off, so there is no need to inform the direct
     * session) */
    if (mData->flModifications)
        rollback(false /* aNotify */);

    HRESULT rc = S_OK;

    bool stateRestored = false;
    bool fNeedsSaveSettings = false;

    try
    {
        /* Delete the saved state file if the machine was Saved prior to this
         * operation */
        if (aTask.machineStateBackup == MachineState_Saved)
        {
            Assert(!mSSData->mStateFilePath.isEmpty());
            RTFileDelete(mSSData->mStateFilePath.c_str());
            mSSData->mStateFilePath.setNull();
            aTask.modifyBackedUpState(MachineState_PoweredOff);
            rc = saveStateSettings(SaveSTS_StateFilePath);
            if (FAILED(rc)) throw rc;
        }

        RTTIMESPEC snapshotTimeStamp;
        RTTimeSpecSetMilli(&snapshotTimeStamp, 0);

        {
            AutoReadLock snapshotLock(aTask.pSnapshot COMMA_LOCKVAL_SRC_POS);

            /* remember the timestamp of the snapshot we're restoring from */
            snapshotTimeStamp = aTask.pSnapshot->getTimeStamp();

            ComPtr<SnapshotMachine> pSnapshotMachine(aTask.pSnapshot->getSnapshotMachine());

            /* copy all hardware data from the snapshot */
            copyFrom(pSnapshotMachine);

            LogFlowThisFunc(("Restoring hard disks from the snapshot...\n"));

            // restore the attachments from the snapshot
            setModified(IsModified_Storage);
            mMediaData.backup();
            mMediaData->mAttachments = pSnapshotMachine->mMediaData->mAttachments;

            /* leave the locks before the potentially lengthy operation */
            snapshotLock.release();
            alock.leave();

            rc = createImplicitDiffs(mUserData->mSnapshotFolderFull,
                                     aTask.pProgress,
                                     1,
                                     false /* aOnline */,
                                     &fNeedsSaveSettings);
            if (FAILED(rc)) throw rc;

            alock.enter();
            snapshotLock.acquire();

            /* Note: on success, current (old) hard disks will be
             * deassociated/deleted on #commit() called from #saveSettings() at
             * the end. On failure, newly created implicit diffs will be
             * deleted by #rollback() at the end. */

            /* should not have a saved state file associated at this point */
            Assert(mSSData->mStateFilePath.isEmpty());

            if (!aTask.pSnapshot->stateFilePath().isEmpty())
            {
                Utf8Str snapStateFilePath = aTask.pSnapshot->stateFilePath();

                Utf8Str stateFilePath = Utf8StrFmt("%ls%c{%RTuuid}.sav",
                                                   mUserData->mSnapshotFolderFull.raw(),
                                                   RTPATH_DELIMITER,
                                                   mData->mUuid.raw());

                LogFlowThisFunc(("Copying saved state file from '%s' to '%s'...\n",
                                  snapStateFilePath.raw(), stateFilePath.raw()));

                aTask.pProgress->SetNextOperation(Bstr(tr("Restoring the execution state")),
                                                  aTask.m_ulStateFileSizeMB);        // weight

                /* leave the lock before the potentially lengthy operation */
                snapshotLock.release();
                alock.leave();

                /* copy the state file */
                int vrc = RTFileCopyEx(snapStateFilePath.c_str(),
                                       stateFilePath.c_str(),
                                       0,
                                       progressCallback,
                                       static_cast<IProgress*>(aTask.pProgress));

                alock.enter();
                snapshotLock.acquire();

                if (RT_SUCCESS(vrc))
                    mSSData->mStateFilePath = stateFilePath;
                else
                    throw setError(E_FAIL,
                                   tr("Could not copy the state file '%s' to '%s' (%Rrc)"),
                                   snapStateFilePath.raw(),
                                   stateFilePath.raw(),
                                   vrc);
            }

            LogFlowThisFunc(("Setting new current snapshot {%RTuuid}\n", aTask.pSnapshot->getId().raw()));
            /* make the snapshot we restored from the current snapshot */
            mData->mCurrentSnapshot = aTask.pSnapshot;
        }

        /* grab differencing hard disks from the old attachments that will
         * become unused and need to be auto-deleted */
        std::list< ComObjPtr<MediumAttachment> > llDiffAttachmentsToDelete;

        for (MediaData::AttachmentList::const_iterator it = mMediaData.backedUpData()->mAttachments.begin();
             it != mMediaData.backedUpData()->mAttachments.end();
             ++it)
        {
            ComObjPtr<MediumAttachment> pAttach = *it;
            ComObjPtr<Medium> pMedium = pAttach->getMedium();

            /* while the hard disk is attached, the number of children or the
             * parent cannot change, so no lock */
            if (    !pMedium.isNull()
                 && pAttach->getType() == DeviceType_HardDisk
                 && !pMedium->getParent().isNull()
                 && pMedium->getChildren().size() == 0
               )
            {
                LogFlowThisFunc(("Picked differencing image '%s' for deletion\n", pMedium->getName().raw()));

                llDiffAttachmentsToDelete.push_back(pAttach);
            }
        }

        int saveFlags = 0;

        /* we have already deleted the current state, so set the execution
         * state accordingly no matter of the delete snapshot result */
        if (!mSSData->mStateFilePath.isEmpty())
            setMachineState(MachineState_Saved);
        else
            setMachineState(MachineState_PoweredOff);

        updateMachineStateOnClient();
        stateRestored = true;

        /* assign the timestamp from the snapshot */
        Assert(RTTimeSpecGetMilli (&snapshotTimeStamp) != 0);
        mData->mLastStateChange = snapshotTimeStamp;

        // detach the current-state diffs that we detected above and build a list of
        // image files to delete _after_ saveSettings()

        MediaList llDiffsToDelete;

        for (std::list< ComObjPtr<MediumAttachment> >::iterator it = llDiffAttachmentsToDelete.begin();
             it != llDiffAttachmentsToDelete.end();
             ++it)
        {
            ComObjPtr<MediumAttachment> pAttach = *it;        // guaranteed to have only attachments where medium != NULL
            ComObjPtr<Medium> pMedium = pAttach->getMedium();

            AutoWriteLock mlock(pMedium COMMA_LOCKVAL_SRC_POS);

            LogFlowThisFunc(("Detaching old current state in differencing image '%s'\n", pMedium->getName().raw()));

            // Normally we "detach" the medium by removing the attachment object
            // from the current machine data; saveSettings() below would then
            // compare the current machine data with the one in the backup
            // and actually call Medium::detachFrom(). But that works only half
            // the time in our case so instead we force a detachment here:
            // remove from machine data
            mMediaData->mAttachments.remove(pAttach);
            // remove it from the backup or else saveSettings will try to detach
            // it again and assert
            mMediaData.backedUpData()->mAttachments.remove(pAttach);
            // then clean up backrefs
            pMedium->detachFrom(mData->mUuid);

            llDiffsToDelete.push_back(pMedium);
        }

        // save machine settings, reset the modified flag and commit;
        rc = saveSettings(&fNeedsSaveSettings, SaveS_ResetCurStateModified | saveFlags);
        if (FAILED(rc)) throw rc;

        // let go of the locks while we're deleting image files below
        alock.leave();
        // from here on we cannot roll back on failure any more

        for (MediaList::iterator it = llDiffsToDelete.begin();
             it != llDiffsToDelete.end();
             ++it)
        {
            ComObjPtr<Medium> &pMedium = *it;
            LogFlowThisFunc(("Deleting old current state in differencing image '%s'\n", pMedium->getName().raw()));

            HRESULT rc2 = pMedium->deleteStorage(NULL /* aProgress */,
                                                 true /* aWait */,
                                                 &fNeedsSaveSettings);
            // ignore errors here because we cannot roll back after saveSettings() above
            if (SUCCEEDED(rc2))
                pMedium->uninit();
        }

        if (fNeedsSaveSettings)
        {
            // finally, VirtualBox.xml needs saving too
            AutoWriteLock vboxLock(mParent COMMA_LOCKVAL_SRC_POS);
            mParent->saveSettings();
        }
    }
    catch (HRESULT aRC)
    {
        rc = aRC;
    }

    if (FAILED(rc))
    {
        /* preserve existing error info */
        ErrorInfoKeeper eik;

        /* undo all changes on failure */
        rollback(false /* aNotify */);

        if (!stateRestored)
        {
            /* restore the machine state */
            setMachineState(aTask.machineStateBackup);
            updateMachineStateOnClient();
        }
    }

    /* set the result (this will try to fetch current error info on failure) */
    aTask.pProgress->notifyComplete(rc);

    if (SUCCEEDED(rc))
        mParent->onSnapshotDeleted(mData->mUuid, Guid());

    LogFlowThisFunc(("Done restoring snapshot (rc=%08X)\n", rc));

    LogFlowThisFuncLeave();
}

////////////////////////////////////////////////////////////////////////////////
//
// DeleteSnapshot methods (SessionMachine and related tasks)
//
////////////////////////////////////////////////////////////////////////////////

/**
 * Implementation for IInternalMachineControl::deleteSnapshot().
 *
 * Gets called from Console::DeleteSnapshot(), and that's basically the
 * only thing Console does. Deleting a snapshot happens entirely on the
 * server side since the machine cannot be running.
 *
 * This creates a new thread that does the work and returns a progress
 * object to the client which is then returned to the caller of
 * Console::DeleteSnapshot().
 *
 * Actual work then takes place in DeleteSnapshotTask::handler().
 *
 * @note Locks mParent + this + children objects for writing!
 */
STDMETHODIMP SessionMachine::DeleteSnapshot(IConsole *aInitiator,
                                            IN_BSTR aId,
                                            MachineState_T *aMachineState,
                                            IProgress **aProgress)
{
    LogFlowThisFuncEnter();

    Guid id(aId);
    AssertReturn(aInitiator && !id.isEmpty(), E_INVALIDARG);
    AssertReturn(aMachineState && aProgress, E_POINTER);

    AutoCaller autoCaller(this);
    AssertComRCReturn(autoCaller.rc(), autoCaller.rc());

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    // machine must not be running
    ComAssertRet(!Global::IsOnlineOrTransient(mData->mMachineState), E_FAIL);

    ComObjPtr<Snapshot> pSnapshot;
    HRESULT rc = findSnapshot(id, pSnapshot, true /* aSetError */);
    if (FAILED(rc)) return rc;

    AutoWriteLock snapshotLock(pSnapshot COMMA_LOCKVAL_SRC_POS);

    size_t childrenCount = pSnapshot->getChildrenCount();
    if (childrenCount > 1)
        return setError(VBOX_E_INVALID_OBJECT_STATE,
                        tr("Snapshot '%s' of the machine '%ls' cannot be deleted. because it has %d child snapshots, which is more than the one snapshot allowed for deletion"),
                        pSnapshot->getName().c_str(),
                        mUserData->mName.raw(),
                        childrenCount);

    /* If the snapshot being deleted is the current one, ensure current
     * settings are committed and saved.
     */
    if (pSnapshot == mData->mCurrentSnapshot)
    {
        if (mData->flModifications)
        {
            rc = saveSettings(NULL);
                // no need to change for whether VirtualBox.xml needs saving since
                // we can't have a machine XML rename pending at this point
            if (FAILED(rc)) return rc;
        }
    }

    ComObjPtr<SnapshotMachine> pSnapMachine = pSnapshot->getSnapshotMachine();

    /* create a progress object. The number of operations is:
     *   1 (preparing) + 1 if the snapshot is online + # of normal hard disks
     */
    LogFlowThisFunc(("Going thru snapshot machine attachments to determine progress setup\n"));

    ULONG ulOpCount = 1;            // one for preparations
    ULONG ulTotalWeight = 1;        // one for preparations

    if (pSnapshot->stateFilePath().length())
    {
        ++ulOpCount;
        ++ulTotalWeight;            // assume 1 MB for deleting the state file
    }

    // count normal hard disks and add their sizes to the weight
    for (MediaData::AttachmentList::iterator it = pSnapMachine->mMediaData->mAttachments.begin();
         it != pSnapMachine->mMediaData->mAttachments.end();
         ++it)
    {
        ComObjPtr<MediumAttachment> &pAttach = *it;
        AutoReadLock attachLock(pAttach COMMA_LOCKVAL_SRC_POS);
        if (pAttach->getType() == DeviceType_HardDisk)
        {
            ComObjPtr<Medium> pHD = pAttach->getMedium();
            Assert(pHD);
            AutoReadLock mlock(pHD COMMA_LOCKVAL_SRC_POS);

            MediumType_T type = pHD->getType();
            if (type != MediumType_Writethrough) // writethrough images are unaffected by snapshots, so do nothing for them
            {
                // normal or immutable media need attention
                ++ulOpCount;
                ulTotalWeight += (ULONG)(pHD->getSize() / _1M);
            }
            LogFlowThisFunc(("op %d: considering hard disk attachment %s\n", ulOpCount, pHD->getName().c_str()));
        }
    }

    ComObjPtr<Progress> pProgress;
    pProgress.createObject();
    pProgress->init(mParent, aInitiator,
                    BstrFmt(tr("Deleting snapshot '%s'"), pSnapshot->getName().c_str()),
                    FALSE /* aCancelable */,
                    ulOpCount,
                    ulTotalWeight,
                    Bstr(tr("Setting up")),
                    1);

    /* create and start the task on a separate thread */
    DeleteSnapshotTask *task = new DeleteSnapshotTask(this, pProgress, pSnapshot);
    int vrc = RTThreadCreate(NULL,
                             taskHandler,
                             (void*)task,
                             0,
                             RTTHREADTYPE_MAIN_WORKER,
                             0,
                             "DeleteSnapshot");
    if (RT_FAILURE(vrc))
    {
        delete task;
        return E_FAIL;
    }

    // the task might start running but will block on acquiring the machine's write lock
    // which we acquired above; once this function leaves, the task will be unblocked;
    // set the proper machine state here now (note: after creating a Task instance)
    setMachineState(MachineState_DeletingSnapshot);

    /* return the progress to the caller */
    pProgress.queryInterfaceTo(aProgress);

    /* return the new state to the caller */
    *aMachineState = mData->mMachineState;

    LogFlowThisFuncLeave();

    return S_OK;
}

/**
 * Helper struct for SessionMachine::deleteSnapshotHandler().
 */
struct MediumDeleteRec
{
    MediumDeleteRec()
        : mpMediumLockList(NULL)
    {}

    MediumDeleteRec(const ComObjPtr<Medium> &aHd,
                    const ComObjPtr<Medium> &aSource,
                    const ComObjPtr<Medium> &aTarget,
                    bool fMergeForward,
                    const ComObjPtr<Medium> &aParentForTarget,
                    const MediaList &aChildrenToReparent,
                    MediumLockList *aMediumLockList)
        : mpHD(aHd),
          mpSource(aSource),
          mpTarget(aTarget),
          mfMergeForward(fMergeForward),
          mpParentForTarget(aParentForTarget),
          mChildrenToReparent(aChildrenToReparent),
          mpMediumLockList(aMediumLockList)
    {}

    MediumDeleteRec(const ComObjPtr<Medium> &aHd,
                    const ComObjPtr<Medium> &aSource,
                    const ComObjPtr<Medium> &aTarget,
                    bool fMergeForward,
                    const ComObjPtr<Medium> &aParentForTarget,
                    const MediaList &aChildrenToReparent,
                    MediumLockList *aMediumLockList,
                    const ComObjPtr<MediumAttachment> &aReplaceHda,
                    const Guid &aSnapshotId)
        : mpHD(aHd),
          mpSource(aSource),
          mpTarget(aTarget),
          mfMergeForward(fMergeForward),
          mpParentForTarget(aParentForTarget),
          mChildrenToReparent(aChildrenToReparent),
          mpMediumLockList(aMediumLockList),
          mpReplaceHda(aReplaceHda),
          mSnapshotId(aSnapshotId)
    {}

    ComObjPtr<Medium> mpHD;
    ComObjPtr<Medium> mpSource;
    ComObjPtr<Medium> mpTarget;
    bool mfMergeForward;
    ComObjPtr<Medium> mpParentForTarget;
    MediaList mChildrenToReparent;
    MediumLockList *mpMediumLockList;
    /* these are for the replace hard disk case: */
    ComObjPtr<MediumAttachment> mpReplaceHda;
    Guid mSnapshotId;
};

typedef std::list<MediumDeleteRec> MediumDeleteRecList;

/**
 * Worker method for the delete snapshot thread created by SessionMachine::DeleteSnapshot().
 * This method gets called indirectly through SessionMachine::taskHandler() which then
 * calls DeleteSnapshotTask::handler().
 *
 * The DeleteSnapshotTask contains the progress object returned to the console by
 * SessionMachine::DeleteSnapshot, through which progress and results are reported.
 *
 * SessionMachine::DeleteSnapshot() has set the machne state to MachineState_DeletingSnapshot
 * right after creating this task. Since we block on the machine write lock at the beginning,
 * once that has been acquired, we can assume that the machine state is indeed that.
 *
 * @note Locks the machine + the snapshot + the media tree for writing!
 *
 * @param aTask Task data.
 */
void SessionMachine::deleteSnapshotHandler(DeleteSnapshotTask &aTask)
{
    LogFlowThisFuncEnter();

    AutoCaller autoCaller(this);

    LogFlowThisFunc(("state=%d\n", autoCaller.state()));
    if (!autoCaller.isOk())
    {
        /* we might have been uninitialized because the session was accidentally
         * closed by the client, so don't assert */
        aTask.pProgress->notifyComplete(E_FAIL,
                                        COM_IIDOF(IMachine),
                                        getComponentName(),
                                        tr("The session has been accidentally closed"));
        LogFlowThisFuncLeave();
        return;
    }

    MediumDeleteRecList toDelete;

    HRESULT rc = S_OK;

    bool fMachineSettingsChanged = false;       // Machine
    bool fNeedsSaveSettings = false;            // VirtualBox.xml

    Guid snapshotId;

    try
    {
        /* Locking order:  */
        AutoMultiWriteLock3 multiLock(this->lockHandle(),                   // machine
                                      aTask.pSnapshot->lockHandle(),        // snapshot
                                      &mParent->getMediaTreeLockHandle()    // media tree
                                      COMMA_LOCKVAL_SRC_POS);
            // once we have this lock, we know that SessionMachine::DeleteSnapshot()
            // has exited after setting the machine state to MachineState_DeletingSnapshot

        ComObjPtr<SnapshotMachine> pSnapMachine = aTask.pSnapshot->getSnapshotMachine();
        // no need to lock the snapshot machine since it is const by definiton
        Guid machineId = pSnapMachine->getId();

        // save the snapshot ID (for callbacks)
        snapshotId = aTask.pSnapshot->getId();

        // first pass:
        LogFlowThisFunc(("1: Checking hard disk merge prerequisites...\n"));

        // go thru the attachments of the snapshot machine
        // (the media in here point to the disk states _before_ the snapshot
        // was taken, i.e. the state we're restoring to; for each such
        // medium, we will need to merge it with its one and only child (the
        // diff image holding the changes written after the snapshot was taken)
        for (MediaData::AttachmentList::iterator it = pSnapMachine->mMediaData->mAttachments.begin();
             it != pSnapMachine->mMediaData->mAttachments.end();
             ++it)
        {
            ComObjPtr<MediumAttachment> &pAttach = *it;
            AutoReadLock attachLock(pAttach COMMA_LOCKVAL_SRC_POS);
            if (pAttach->getType() != DeviceType_HardDisk)
                continue;

            ComObjPtr<Medium> pHD = pAttach->getMedium();
            Assert(!pHD.isNull());

            {
                // writethrough images are unaffected by snapshots, skip them
                AutoReadLock medlock(pHD COMMA_LOCKVAL_SRC_POS);
                MediumType_T type = pHD->getType();
                if (type == MediumType_Writethrough)
                    continue;
            }

#ifdef DEBUG
            pHD->dumpBackRefs();
#endif

            // needs to be merged with child or deleted, check prerequisites
            ComObjPtr<Medium> pTarget;
            ComObjPtr<Medium> pSource;
            bool fMergeForward = false;
            ComObjPtr<Medium> pParentForTarget;
            MediaList childrenToReparent;
            MediumLockList *pMediumLockList = NULL;
            rc = prepareDeleteSnapshotMedium(pHD, machineId, snapshotId,
                                             pSource, pTarget, fMergeForward,
                                             pParentForTarget,
                                             childrenToReparent,
                                             pMediumLockList);
            if (FAILED(rc)) throw rc;

            // for simplicity, we merge pHd onto its child (forward merge), not
            // the other way round, because that saves us from updating the
            // attachments for the machine that follows the snapshot (next
            // snapshot or real machine), unless it's a base image:

            if (pMediumLockList != NULL && !fMergeForward)
            {
                // parent is null -> this disk is a base hard disk: we will
                // then do a backward merge, i.e. merge its only child onto the
                // base disk. Here we need then to update the attachment that
                // refers to the child and have it point to the parent instead
                Assert(pHD->getParent().isNull());
                Assert(pHD->getChildren().size() == 1);

                ComObjPtr<Medium> pReplaceHD = pHD->getChildren().front();

                ComAssertThrow(pReplaceHD == pSource, E_FAIL);

                const Guid *pReplaceMachineId = pSource->getFirstMachineBackrefId();
                NOREF(pReplaceMachineId);
                Assert(pReplaceMachineId);
                Assert(*pReplaceMachineId == mData->mUuid);

                Guid snapshotId2;
                const Guid *pSnapshotId = pSource->getFirstMachineBackrefSnapshotId();
                if (pSnapshotId)
                    snapshotId2 = *pSnapshotId;

                HRESULT rc2 = S_OK;

                attachLock.release();

                // First we must detach the child otherwise mergeTo() will
                // will assert because it is going to delete the child.
                // Adjust the backreferences:
                // 1) detach the first child hard disk
                rc2 = pSource->detachFrom(mData->mUuid, snapshotId2);
                AssertComRC(rc2);
                // 2) attach to machine and snapshot
                rc2 = pHD->attachTo(mData->mUuid, snapshotId2);
                AssertComRC(rc2);

                /* replace the hard disk in the attachment object */
                if (snapshotId2.isEmpty())
                {
                    /* in current state */
                    AssertBreak(pAttach = findAttachment(mMediaData->mAttachments, pSource));
                }
                else
                {
                    /* in snapshot */
                    ComObjPtr<Snapshot> snapshot;
                    rc2 = findSnapshot(snapshotId2, snapshot);
                    AssertComRC(rc2);

                    /* don't lock the snapshot; cannot be modified outside */
                    MediaData::AttachmentList &snapAtts = snapshot->getSnapshotMachine()->mMediaData->mAttachments;
                    AssertBreak(pAttach = findAttachment(snapAtts, pSource));
                }

                AutoWriteLock attLock(pAttach COMMA_LOCKVAL_SRC_POS);
                pAttach->updateMedium(pHD, false /* aImplicit */);

                toDelete.push_back(MediumDeleteRec(pHD, pSource, pTarget,
                                                   fMergeForward,
                                                   pParentForTarget,
                                                   childrenToReparent,
                                                   pMediumLockList,
                                                   pAttach, snapshotId2));
            }
            else
                toDelete.push_back(MediumDeleteRec(pHD, pSource, pTarget,
                                                   fMergeForward,
                                                   pParentForTarget,
                                                   childrenToReparent,
                                                   pMediumLockList));
        }

        // we can release the lock now since the machine state is MachineState_DeletingSnapshot
        multiLock.release();

        /** @todo r=klaus the comment below makes only limited sense to me.
         * Deleting the snapshot early has some benefits, but also the drawback
         * that a normal user surely has difficulties to perform the not
         * completed merge operations later. Before IMedium::mergeTo is added
         * to VBoxManage it actually is completely impossible. */

        /* Now we checked that we can successfully merge all normal hard disks
         * (unless a runtime error like end-of-disc happens). Prior to
         * performing the actual merge, we want to delete the snapshot itself
         * and remove it from the XML file to make sure that a possible merge
         * runtime error will not make this snapshot inconsistent because of
         * the partially merged or corrupted hard disks */

        /* second pass: */
        LogFlowThisFunc(("2: Deleting snapshot...\n"));

        {
            // saveAllSnapshots() needs a machine lock, and the snapshots
            // tree is protected by the machine lock as well
            AutoWriteLock machineLock(this COMMA_LOCKVAL_SRC_POS);

            ComObjPtr<Snapshot> parentSnapshot = aTask.pSnapshot->getParent();
            Utf8Str stateFilePath = aTask.pSnapshot->stateFilePath();

            // Note that deleting the snapshot will deassociate it from the
            // hard disks which will allow the merge+delete operation for them
            aTask.pSnapshot->beginSnapshotDelete();
            aTask.pSnapshot->uninit();
                    // this requests the machine lock in turn when deleting all the children
                    // in the snapshot machine

            rc = saveAllSnapshots(*mData->pMachineConfigFile);
            machineLock.release();
            if (FAILED(rc)) throw rc;

            if (!stateFilePath.isEmpty())
            {
                aTask.pProgress->SetNextOperation(Bstr(tr("Deleting the execution state")),
                                                  1);        // weight

                RTFileDelete(stateFilePath.c_str());
            }

            /// @todo NEWMEDIA to provide a good level of fauilt tolerance, we
            /// should restore the shapshot in the snapshot tree if
            /// saveSnapshotSettings fails. Actually, we may call
            /// #saveSnapshotSettings() with a special flag that will tell it to
            /// skip the given snapshot as if it would have been deleted and
            /// only actually delete it if the save operation succeeds.
        }

        /* here we come when we've irreversibly eleted the snapshot which
         * means that the VM settigns (our relevant changes to mData) need to be
         * saved too */
        /// @todo NEWMEDIA maybe save everything in one operation in place of
        ///  saveSnapshotSettings() above
        fMachineSettingsChanged = true;

        /* third pass: */
        LogFlowThisFunc(("3: Performing actual hard disk merging...\n"));

        /// @todo NEWMEDIA turn the following errors into warnings because the
        /// snapshot itself has been already deleted (and interpret these
        /// warnings properly on the GUI side)
        for (MediumDeleteRecList::iterator it = toDelete.begin();
             it != toDelete.end();)
        {
            const ComObjPtr<Medium> &pMedium(it->mpHD);
            ULONG ulWeight;

            {
                AutoReadLock alock(pMedium COMMA_LOCKVAL_SRC_POS);
                ulWeight = (ULONG)(pMedium->getSize() / _1M);
            }

            aTask.pProgress->SetNextOperation(BstrFmt(tr("Merging differencing image '%s'"),
                                              pMedium->getName().raw()),
                                              ulWeight);

            if (it->mpMediumLockList == NULL)
            {
                /* no real merge needed, just updating state and delete
                 * diff files if necessary */
                AutoMultiWriteLock2 mLock(&mParent->getMediaTreeLockHandle(), pMedium->lockHandle() COMMA_LOCKVAL_SRC_POS);

                Assert(   !it->mfMergeForward
                       || pMedium->getChildren().size() == 0);

                /* Delete the differencing hard disk (has no children). Two
                 * exceptions: if it's the last medium in the chain or if it's
                 * a backward merge we don't want to handle due to complextity.
                 * In both cases leave the image in place. If it's the first
                 * exception the user can delete it later if he wants. */
                if (!pMedium->getParent().isNull())
                {
                    Assert(pMedium->getState() == MediumState_Deleting);
                    rc = pMedium->deleteStorage(&aTask.pProgress,
                                                true /* aWait */,
                                                &fNeedsSaveSettings);
                    if (SUCCEEDED(rc))
                        pMedium->uninit();
                }
            }
            else
            {
                /* Normal merge operation, in the direction decided earlier. */
                rc = it->mpSource->mergeTo(it->mpTarget, it->mfMergeForward,
                                           it->mpParentForTarget,
                                           it->mChildrenToReparent,
                                           it->mpMediumLockList,
                                           &aTask.pProgress, true /* aWait */,
                                           &fNeedsSaveSettings);
                if (SUCCEEDED(rc))
                {
                    /* On success delete the no longer needed medium lock
                     * list. This unlocks the media as well. */
                    delete it->mpMediumLockList;
                    it->mpMediumLockList = NULL;
                    it->mpSource->uninit();
                }
            }

            if (FAILED(rc)) throw rc;

            /* prevent from calling cancelDeleteSnapshotMedium() */
            it = toDelete.erase(it);
        }
    }
    catch (HRESULT aRC) { rc = aRC; }

    if (FAILED(rc))
    {
        AutoMultiWriteLock2 multiLock(this->lockHandle(),                   // machine
                                      &mParent->getMediaTreeLockHandle()    // media tree
                                      COMMA_LOCKVAL_SRC_POS);

        // un-prepare the remaining hard disks
        for (MediumDeleteRecList::const_iterator it = toDelete.begin();
             it != toDelete.end();
             ++it)
        {
            cancelDeleteSnapshotMedium(it->mpHD, it->mpSource, it->mpTarget,
                                       it->mChildrenToReparent,
                                       it->mpMediumLockList,
                                       it->mpReplaceHda, it->mSnapshotId);
        }
    }

    // whether we were successful or not, we need to set the machine
    // state and save the machine settings;
    {
        // preserve existing error info so that the result can
        // be properly reported to the progress object below
        ErrorInfoKeeper eik;

        // restore the machine state that was saved when the
        // task was started
        setMachineState(aTask.machineStateBackup);
        updateMachineStateOnClient();

        if (fMachineSettingsChanged || fNeedsSaveSettings)
        {
            if (fMachineSettingsChanged)
            {
                AutoWriteLock machineLock(this COMMA_LOCKVAL_SRC_POS);
                saveSettings(&fNeedsSaveSettings, SaveS_InformCallbacksAnyway);
            }

            if (fNeedsSaveSettings)
            {
                AutoWriteLock vboxLock(mParent COMMA_LOCKVAL_SRC_POS);
                mParent->saveSettings();
            }
        }
    }

    // report the result (this will try to fetch current error info on failure)
    aTask.pProgress->notifyComplete(rc);

    if (SUCCEEDED(rc))
        mParent->onSnapshotDeleted(mData->mUuid, snapshotId);

    LogFlowThisFunc(("Done deleting snapshot (rc=%08X)\n", rc));
    LogFlowThisFuncLeave();
}

/**
 * Checks that this hard disk (part of a snapshot) may be deleted/merged and
 * performs necessary state changes. Must not be called for writethrough disks
 * because there is nothing to delete/merge then.
 *
 * This method is to be called prior to calling #deleteSnapshotMedium().
 * If #deleteSnapshotMedium() is not called or fails, the state modifications
 * performed by this method must be undone by #cancelDeleteSnapshotMedium().
 *
 * @param aHD           Hard disk which is connected to the snapshot.
 * @param aMachineId    UUID of machine this hard disk is attached to.
 * @param aSnapshotId   UUID of snapshot this hard disk is attached to. May
 *                      be a zero UUID if no snapshot is applicable.
 * @param aSource       Source hard disk for merge (out).
 * @param aTarget       Target hard disk for merge (out).
 * @param aMergeForward Merge direction decision (out).
 * @param aParentForTarget New parent if target needs to be reparented (out).
 * @param aChildrenToReparent Children which have to be reparented to the
 *                      target (out).
 * @param aMediumLockList Where to store the created medium lock list (may
 *                      return NULL if no real merge is necessary).
 *
 * @note Caller must hold media tree lock for writing. This locks this object
 *       and every medium object on the merge chain for writing.
 */
HRESULT SessionMachine::prepareDeleteSnapshotMedium(const ComObjPtr<Medium> &aHD,
                                                    const Guid &aMachineId,
                                                    const Guid &aSnapshotId,
                                                    ComObjPtr<Medium> &aSource,
                                                    ComObjPtr<Medium> &aTarget,
                                                    bool &aMergeForward,
                                                    ComObjPtr<Medium> &aParentForTarget,
                                                    MediaList &aChildrenToReparent,
                                                    MediumLockList * &aMediumLockList)
{
    Assert(mParent->getMediaTreeLockHandle().isWriteLockOnCurrentThread());

    AutoWriteLock alock(aHD COMMA_LOCKVAL_SRC_POS);

    // Medium must not be writethrough at this point
    AssertReturn(aHD->getType() != MediumType_Writethrough, E_FAIL);

    aMediumLockList = NULL;

    if (aHD->getChildren().size() == 0)
    {
        /* special treatment of the last hard disk in the chain: */
        if (aHD->getParent().isNull())
        {
            /* lock only, to prevent any usage until the snapshot deletion
             * is completed */
            return aHD->LockWrite(NULL);
        }

        /* the differencing hard disk w/o children will be deleted, protect it
         * from attaching to other VMs (this is why Deleting) */
        return aHD->markForDeletion();
   }

    /* not going multi-merge as it's too expensive */
    if (aHD->getChildren().size() > 1)
        return setError(E_FAIL,
                        tr ("Hard disk '%s' has more than one child hard disk (%d)"),
                        aHD->getLocationFull().raw(),
                        aHD->getChildren().size());

    ComObjPtr<Medium> pChild = aHD->getChildren().front();

    /* we keep this locked, so lock the affected child to make sure the lock
     * order is correct when calling prepareMergeTo() */
    AutoWriteLock childLock(pChild COMMA_LOCKVAL_SRC_POS);

    /* the rest is a normal merge setup */
    if (aHD->getParent().isNull())
    {
        /* base hard disk, backward merge */
        const Guid *pMachineId1 = pChild->getFirstMachineBackrefId();
        const Guid *pMachineId2 = aHD->getFirstMachineBackrefId();
        if (pMachineId1 && pMachineId2 && *pMachineId1 != *pMachineId2)
        {
            /* backward merge is too tricky, we'll just detach on snapshot
             * deletion, so lock only, to prevent any usage */
            return aHD->LockWrite(NULL);
        }

        aSource = pChild;
        aTarget = aHD;
    }
    else
    {
        /* forward merge */
        aSource = aHD;
        aTarget = pChild;
    }

    return aSource->prepareMergeTo(aTarget, &aMachineId, &aSnapshotId,
                                   aMergeForward, aParentForTarget,
                                   aChildrenToReparent, aMediumLockList);
}

/**
 * Cancels the deletion/merging of this hard disk (part of a snapshot). Undoes
 * what #prepareDeleteSnapshotMedium() did. Must be called if
 * #deleteSnapshotMedium() is not called or fails.
 *
 * @param aHD           Hard disk which is connected to the snapshot.
 * @param aSource       Source hard disk for merge.
 * @param aTarget       Source hard disk for merge.
 * @param aChildrenToReparent Children to unlock.
 * @param aMediumLockList Medium locks to cancel.
 * @param aReplaceHda   Hard disk attachment to restore.
 * @param aSnapshotId   Snapshot id to attach the medium to.
 *
 * @note Locks the medium tree and the hard disks in the chain for writing.
 */
void SessionMachine::cancelDeleteSnapshotMedium(const ComObjPtr<Medium> &aHD,
                                                const ComObjPtr<Medium> &aSource,
                                                const ComObjPtr<Medium> &aTarget,
                                                const MediaList &aChildrenToReparent,
                                                MediumLockList *aMediumLockList,
                                                const ComObjPtr<MediumAttachment> &aReplaceHda,
                                                const Guid &aSnapshotId)
{
    if (aMediumLockList == NULL)
    {
        AutoMultiWriteLock2 mLock(&mParent->getMediaTreeLockHandle(), aHD->lockHandle() COMMA_LOCKVAL_SRC_POS);

        Assert(aHD->getChildren().size() == 0);

        if (aHD->getParent().isNull())
        {
            HRESULT rc = aHD->UnlockWrite(NULL);;
            AssertComRC(rc);
        }
        else
        {
            HRESULT rc = aHD->unmarkForDeletion();
            AssertComRC(rc);
        }
    }
    else
        aSource->cancelMergeTo(aChildrenToReparent, aMediumLockList);

    if (!aReplaceHda.isNull())
    {
        HRESULT rc = aTarget->attachTo(mData->mUuid, aSnapshotId);
        AssertComRC(rc);

        rc = aHD->detachFrom(mData->mUuid, aSnapshotId);
        AssertComRC(rc);

        AutoWriteLock attLock(aReplaceHda COMMA_LOCKVAL_SRC_POS);
        aReplaceHda->updateMedium(aTarget, false /* aImplicit */);
    }
}
