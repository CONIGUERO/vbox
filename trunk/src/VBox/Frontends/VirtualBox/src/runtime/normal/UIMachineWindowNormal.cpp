/* $Id$ */
/** @file
 * VBox Qt GUI - UIMachineWindowNormal class implementation.
 */

/*
 * Copyright (C) 2010-2022 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QMenuBar>
#include <QTimerEvent>
#include <QContextMenuEvent>
#include <QResizeEvent>
#include <QScrollBar>
#ifdef VBOX_WS_X11
# include <QTimer>
#endif

/* GUI includes: */
#include "UICommon.h"
#include "UIDesktopWidgetWatchdog.h"
#include "UIMachineWindowNormal.h"
#include "UIActionPoolRuntime.h"
#include "UIExtraDataManager.h"
#include "UIIndicatorsPool.h"
#include "UIKeyboardHandler.h"
#include "UIMouseHandler.h"
#include "UIMachineLogic.h"
#include "UIMachineView.h"
#include "UINotificationCenter.h"
#include "UIIconPool.h"
#include "UISession.h"
#include "QIStatusBar.h"
#include "QIStatusBarIndicator.h"
#ifndef VBOX_WS_MAC
# include "UIMenuBar.h"
#else  /* VBOX_WS_MAC */
# include "VBoxUtils.h"
# include "UIImageTools.h"
# include "UICocoaApplication.h"
#endif /* VBOX_WS_MAC */

/* COM includes: */
#include "CConsole.h"
#include "CMediumAttachment.h"
#include "CUSBController.h"
#include "CUSBDeviceFilters.h"


UIMachineWindowNormal::UIMachineWindowNormal(UIMachineLogic *pMachineLogic, ulong uScreenId)
    : UIMachineWindow(pMachineLogic, uScreenId)
    , m_pIndicatorsPool(0)
    , m_iGeometrySaveTimerId(-1)
{
}

void UIMachineWindowNormal::sltMachineStateChanged()
{
    /* Call to base-class: */
    UIMachineWindow::sltMachineStateChanged();

    /* Update indicator-pool and virtualization stuff: */
    updateAppearanceOf(UIVisualElement_IndicatorPoolStuff | UIVisualElement_Recording | UIVisualElement_FeaturesStuff);
}

void UIMachineWindowNormal::sltMediumChange(const CMediumAttachment &attachment)
{
    /* Update corresponding medium stuff: */
    KDeviceType type = attachment.GetType();
    if (type == KDeviceType_HardDisk)
        updateAppearanceOf(UIVisualElement_HDStuff);
    if (type == KDeviceType_DVD)
        updateAppearanceOf(UIVisualElement_CDStuff);
    if (type == KDeviceType_Floppy)
        updateAppearanceOf(UIVisualElement_FDStuff);
}

void UIMachineWindowNormal::sltUSBControllerChange()
{
    /* Update USB stuff: */
    updateAppearanceOf(UIVisualElement_USBStuff);
}

void UIMachineWindowNormal::sltUSBDeviceStateChange()
{
    /* Update USB stuff: */
    updateAppearanceOf(UIVisualElement_USBStuff);
}

void UIMachineWindowNormal::sltAudioAdapterChange()
{
    /* Update audio stuff: */
    updateAppearanceOf(UIVisualElement_AudioStuff);
}

void UIMachineWindowNormal::sltNetworkAdapterChange()
{
    /* Update network stuff: */
    updateAppearanceOf(UIVisualElement_NetworkStuff);
}

void UIMachineWindowNormal::sltSharedFolderChange()
{
    /* Update shared-folders stuff: */
    updateAppearanceOf(UIVisualElement_SharedFolderStuff);
}

void UIMachineWindowNormal::sltRecordingChange()
{
    /* Update video-capture stuff: */
    updateAppearanceOf(UIVisualElement_Recording);
}

void UIMachineWindowNormal::sltCPUExecutionCapChange()
{
    /* Update virtualization stuff: */
    updateAppearanceOf(UIVisualElement_FeaturesStuff);
}

void UIMachineWindowNormal::sltHandleSessionInitialized()
{
    /* Update virtualization stuff: */
    updateAppearanceOf(  UIVisualElement_FeaturesStuff
                       | UIVisualElement_HDStuff
                       | UIVisualElement_CDStuff
                       | UIVisualElement_FDStuff);
}

#ifndef RT_OS_DARWIN
void UIMachineWindowNormal::sltHandleMenuBarConfigurationChange(const QUuid &uMachineID)
{
    /* Skip unrelated machine IDs: */
    if (uiCommon().managedVMUuid() != uMachineID)
        return;

    /* Check whether menu-bar is enabled: */
    const bool fEnabled = gEDataManager->menuBarEnabled(uiCommon().managedVMUuid());
    /* Update settings action 'enable' state: */
    QAction *pActionMenuBarSettings = actionPool()->action(UIActionIndexRT_M_View_M_MenuBar_S_Settings);
    pActionMenuBarSettings->setEnabled(fEnabled);
    /* Update switch action 'checked' state: */
    QAction *pActionMenuBarSwitch = actionPool()->action(UIActionIndexRT_M_View_M_MenuBar_T_Visibility);
    pActionMenuBarSwitch->blockSignals(true);
    pActionMenuBarSwitch->setChecked(fEnabled);
    pActionMenuBarSwitch->blockSignals(false);

    /* Update menu-bar visibility: */
    menuBar()->setVisible(pActionMenuBarSwitch->isChecked());
    /* Update menu-bar: */
    updateMenu();

    /* Normalize geometry without moving: */
    normalizeGeometry(false /* adjust position */, shouldResizeToGuestDisplay());
}

void UIMachineWindowNormal::sltHandleMenuBarContextMenuRequest(const QPoint &position)
{
    /* Raise action's context-menu: */
    if (gEDataManager->menuBarContextMenuEnabled(uiCommon().managedVMUuid()))
        actionPool()->action(UIActionIndexRT_M_View_M_MenuBar)->menu()->exec(menuBar()->mapToGlobal(position));
}
#endif /* !RT_OS_DARWIN */

void UIMachineWindowNormal::sltHandleStatusBarConfigurationChange(const QUuid &uMachineID)
{
    /* Skip unrelated machine IDs: */
    if (uiCommon().managedVMUuid() != uMachineID)
        return;

    /* Check whether status-bar is enabled: */
    const bool fEnabled = gEDataManager->statusBarEnabled(uiCommon().managedVMUuid());
    /* Update settings action 'enable' state: */
    QAction *pActionStatusBarSettings = actionPool()->action(UIActionIndexRT_M_View_M_StatusBar_S_Settings);
    pActionStatusBarSettings->setEnabled(fEnabled);
    /* Update switch action 'checked' state: */
    QAction *pActionStatusBarSwitch = actionPool()->action(UIActionIndexRT_M_View_M_StatusBar_T_Visibility);
    pActionStatusBarSwitch->blockSignals(true);
    pActionStatusBarSwitch->setChecked(fEnabled);
    pActionStatusBarSwitch->blockSignals(false);

    /* Update status-bar visibility: */
    statusBar()->setVisible(pActionStatusBarSwitch->isChecked());
    /* Update status-bar indicators-pool: */
    if (m_pIndicatorsPool)
        m_pIndicatorsPool->setAutoUpdateIndicatorStates(statusBar()->isVisible() && uisession()->isRunning());

    /* Normalize geometry without moving: */
    normalizeGeometry(false /* adjust position */, shouldResizeToGuestDisplay());
}

void UIMachineWindowNormal::sltHandleStatusBarContextMenuRequest(const QPoint &position)
{
    /* Raise action's context-menu: */
    if (gEDataManager->statusBarContextMenuEnabled(uiCommon().managedVMUuid()))
        actionPool()->action(UIActionIndexRT_M_View_M_StatusBar)->menu()->exec(statusBar()->mapToGlobal(position));
}

void UIMachineWindowNormal::sltHandleIndicatorContextMenuRequest(IndicatorType enmIndicatorType, const QPoint &indicatorPosition)
{
    /* Sanity check, this slot should be called if m_pIndicatorsPool present anyway: */
    AssertPtrReturnVoid(m_pIndicatorsPool);
    /* Determine action depending on indicator-type: */
    UIAction *pAction = 0;
    switch (enmIndicatorType)
    {
        case IndicatorType_HardDisks:     pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_HardDrives);     break;
        case IndicatorType_OpticalDisks:  pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_OpticalDevices); break;
        case IndicatorType_FloppyDisks:   pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_FloppyDevices);  break;
        case IndicatorType_Audio:         pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_Audio);          break;
        case IndicatorType_Network:       pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_Network);        break;
        case IndicatorType_USB:           pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_USBDevices);     break;
        case IndicatorType_SharedFolders: pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_SharedFolders);  break;
        case IndicatorType_Display:       pAction = actionPool()->action(UIActionIndexRT_M_ViewPopup);                break;
        case IndicatorType_Recording:     pAction = actionPool()->action(UIActionIndexRT_M_View_M_Recording);         break;
        case IndicatorType_Mouse:         pAction = actionPool()->action(UIActionIndexRT_M_Input_M_Mouse);            break;
        case IndicatorType_Keyboard:      pAction = actionPool()->action(UIActionIndexRT_M_Input_M_Keyboard);         break;
        default: break;
    }
    /* Raise action's context-menu: */
    if (pAction && pAction->isEnabled())
        pAction->menu()->exec(m_pIndicatorsPool->mapIndicatorPositionToGlobal(enmIndicatorType, indicatorPosition));
}

#ifdef VBOX_WS_MAC
void UIMachineWindowNormal::sltActionHovered(UIAction *pAction)
{
    /* Show the action message for a ten seconds: */
    statusBar()->showMessage(pAction->statusTip(), 10000);
}
#endif /* VBOX_WS_MAC */

void UIMachineWindowNormal::prepareSessionConnections()
{
    /* Call to base-class: */
    UIMachineWindow::prepareSessionConnections();

    /* We should watch for console events: */
    connect(machineLogic()->uisession(), &UISession::sigMediumChange,
        this, &UIMachineWindowNormal::sltMediumChange);
    connect(machineLogic()->uisession(), &UISession::sigUSBControllerChange,
            this, &UIMachineWindowNormal::sltUSBControllerChange);
    connect(machineLogic()->uisession(), &UISession::sigUSBDeviceStateChange,
            this, &UIMachineWindowNormal::sltUSBDeviceStateChange);
    connect(machineLogic()->uisession(), &UISession::sigAudioAdapterChange,
            this, &UIMachineWindowNormal::sltAudioAdapterChange);
    connect(machineLogic()->uisession(), &UISession::sigNetworkAdapterChange,
            this, &UIMachineWindowNormal::sltNetworkAdapterChange);
    connect(machineLogic()->uisession(), &UISession::sigSharedFolderChange,
            this, &UIMachineWindowNormal::sltSharedFolderChange);
    connect(machineLogic()->uisession(), &UISession::sigRecordingChange,
            this, &UIMachineWindowNormal::sltRecordingChange);
    connect(machineLogic()->uisession(), &UISession::sigCPUExecutionCapChange,
            this, &UIMachineWindowNormal::sltCPUExecutionCapChange);
    connect(machineLogic()->uisession(), &UISession::sigInitialized,
            this, &UIMachineWindowNormal::sltHandleSessionInitialized);
}

#ifndef VBOX_WS_MAC
void UIMachineWindowNormal::prepareMenu()
{
    /* Create menu-bar: */
    setMenuBar(new UIMenuBar);
    AssertPtrReturnVoid(menuBar());
    {
        /* Configure menu-bar: */
        menuBar()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(menuBar(), &UIMenuBar::customContextMenuRequested,
                this, &UIMachineWindowNormal::sltHandleMenuBarContextMenuRequest);
        connect(gEDataManager, &UIExtraDataManager::sigMenuBarConfigurationChange,
                this, &UIMachineWindowNormal::sltHandleMenuBarConfigurationChange);
        /* Update menu-bar: */
        updateMenu();
    }
}
#endif /* !VBOX_WS_MAC */

void UIMachineWindowNormal::prepareStatusBar()
{
    /* Call to base-class: */
    UIMachineWindow::prepareStatusBar();

    /* Create status-bar: */
    setStatusBar(new QIStatusBar);
    AssertPtrReturnVoid(statusBar());
    {
        /* Configure status-bar: */
        statusBar()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(statusBar(), &QIStatusBar::customContextMenuRequested,
                this, &UIMachineWindowNormal::sltHandleStatusBarContextMenuRequest);
        /* Create indicator-pool: */
        m_pIndicatorsPool = new UIIndicatorsPool(machineLogic()->uisession());
        AssertPtrReturnVoid(m_pIndicatorsPool);
        {
            /* Configure indicator-pool: */
            connect(m_pIndicatorsPool, &UIIndicatorsPool::sigContextMenuRequest,
                    this, &UIMachineWindowNormal::sltHandleIndicatorContextMenuRequest);
            /* Add indicator-pool into status-bar: */
            statusBar()->addPermanentWidget(m_pIndicatorsPool, 0);
        }
        /* Post-configure status-bar: */
        connect(gEDataManager, &UIExtraDataManager::sigStatusBarConfigurationChange,
                this, &UIMachineWindowNormal::sltHandleStatusBarConfigurationChange);
#ifdef VBOX_WS_MAC
        /* Make sure the status-bar is aware of action hovering: */
        connect(actionPool(), &UIActionPool::sigActionHovered,
                this, &UIMachineWindowNormal::sltActionHovered);
#endif /* VBOX_WS_MAC */
    }

#ifdef VBOX_WS_MAC
    /* For the status-bar on Cocoa: */
    setUnifiedTitleAndToolBarOnMac(true);
#endif /* VBOX_WS_MAC */
}

void UIMachineWindowNormal::prepareNotificationCenter()
{
    if (gpNotificationCenter && (m_uScreenId == 0))
        gpNotificationCenter->setParent(centralWidget());
}

void UIMachineWindowNormal::prepareVisualState()
{
    /* Call to base-class: */
    UIMachineWindow::prepareVisualState();

#ifdef VBOX_GUI_WITH_CUSTOMIZATIONS1
    /* Customer request: The background has to go black: */
    QPalette palette(centralWidget()->palette());
    palette.setColor(centralWidget()->backgroundRole(), Qt::black);
    centralWidget()->setPalette(palette);
    centralWidget()->setAutoFillBackground(true);
    setAutoFillBackground(true);
#endif /* VBOX_GUI_WITH_CUSTOMIZATIONS1 */

#ifdef VBOX_WS_MAC
    /* Beta label? */
    if (uiCommon().isBeta())
    {
        QPixmap betaLabel = ::betaLabel(QSize(74, darwinWindowTitleHeight(this) - 1));
        ::darwinLabelWindow(this, &betaLabel);
    }

    /* For 'Yosemite' and above: */
    if (uiCommon().osRelease() >= MacOSXRelease_Yosemite)
    {
        /* Enable fullscreen support for every screen which requires it: */
        if (darwinScreensHaveSeparateSpaces() || m_uScreenId == 0)
            darwinEnableFullscreenSupport(this);
        /* Register 'Zoom' button to use our full-screen: */
        UICocoaApplication::instance()->registerCallbackForStandardWindowButton(this, StandardWindowButtonType_Zoom,
                                                                                UIMachineWindow::handleStandardWindowButtonCallback);
    }
#endif /* VBOX_WS_MAC */
}

void UIMachineWindowNormal::loadSettings()
{
    /* Call to base-class: */
    UIMachineWindow::loadSettings();

    /* Load GUI customizations: */
    {
#ifndef VBOX_WS_MAC
        /* Update menu-bar visibility: */
        menuBar()->setVisible(actionPool()->action(UIActionIndexRT_M_View_M_MenuBar_T_Visibility)->isChecked());
#endif /* !VBOX_WS_MAC */
        /* Update status-bar visibility: */
        statusBar()->setVisible(actionPool()->action(UIActionIndexRT_M_View_M_StatusBar_T_Visibility)->isChecked());
        if (m_pIndicatorsPool)
            m_pIndicatorsPool->setAutoUpdateIndicatorStates(statusBar()->isVisible() && uisession()->isRunning());
    }

#ifndef VBOX_GUI_WITH_CUSTOMIZATIONS1
    /* Load window geometry: */
    {
        /* Load extra-data: */
        QRect geo = gEDataManager->machineWindowGeometry(machineLogic()->visualStateType(),
                                                         m_uScreenId, uiCommon().managedVMUuid());

        /* If we do have proper geometry: */
        if (!geo.isNull())
        {
            /* Restore window geometry: */
            m_geometry = geo;
            UIDesktopWidgetWatchdog::setTopLevelGeometry(this, m_geometry);

            /* If previous machine-state was NOT SAVED => normalize window to the optimal-size: */
            if (machine().GetState() != KMachineState_Saved && machine().GetState() != KMachineState_AbortedSaved)
                normalizeGeometry(false /* adjust position */, shouldResizeToGuestDisplay());

            /* Maximize window (if necessary): */
            if (gEDataManager->machineWindowShouldBeMaximized(machineLogic()->visualStateType(),
                                                              m_uScreenId, uiCommon().managedVMUuid()))
                setWindowState(windowState() | Qt::WindowMaximized);
        }
        /* If we do NOT have proper geometry: */
        else
        {
            /* Normalize window to the optimal size: */
            normalizeGeometry(true /* adjust position */, shouldResizeToGuestDisplay());

            /* Move it to the screen-center: */
            m_geometry = geometry();
            m_geometry.moveCenter(gpDesktop->availableGeometry(this).center());
            UIDesktopWidgetWatchdog::setTopLevelGeometry(this, m_geometry);
        }

        /* Normalize to the optimal size: */
#ifdef VBOX_WS_X11
        QTimer::singleShot(0, this, SLOT(sltNormalizeGeometry()));
#else /* !VBOX_WS_X11 */
        normalizeGeometry(true /* adjust position */, shouldResizeToGuestDisplay());
#endif /* !VBOX_WS_X11 */
    }
#endif /* VBOX_GUI_WITH_CUSTOMIZATIONS1 */
}

void UIMachineWindowNormal::cleanupVisualState()
{
#ifdef VBOX_WS_MAC
    /* Unregister 'Zoom' button from using our full-screen since Yosemite: */
    if (uiCommon().osRelease() >= MacOSXRelease_Yosemite)
        UICocoaApplication::instance()->unregisterCallbackForStandardWindowButton(this, StandardWindowButtonType_Zoom);
#endif /* VBOX_WS_MAC */
}

void UIMachineWindowNormal::cleanupNotificationCenter()
{
    if (gpNotificationCenter && (gpNotificationCenter->parent() == centralWidget()))
        gpNotificationCenter->setParent(0);
}

void UIMachineWindowNormal::cleanupStatusBar()
{
    delete m_pIndicatorsPool;
    m_pIndicatorsPool = 0;
}

void UIMachineWindowNormal::cleanupSessionConnections()
{
    /* We should stop watching for console events: */
    disconnect(machineLogic()->uisession(), &UISession::sigMediumChange,
               this, &UIMachineWindowNormal::sltMediumChange);
    disconnect(machineLogic()->uisession(), &UISession::sigUSBControllerChange,
               this, &UIMachineWindowNormal::sltUSBControllerChange);
    disconnect(machineLogic()->uisession(), &UISession::sigUSBDeviceStateChange,
               this, &UIMachineWindowNormal::sltUSBDeviceStateChange);
    disconnect(machineLogic()->uisession(), &UISession::sigNetworkAdapterChange,
               this, &UIMachineWindowNormal::sltNetworkAdapterChange);
    disconnect(machineLogic()->uisession(), &UISession::sigAudioAdapterChange,
               this, &UIMachineWindowNormal::sltAudioAdapterChange);
    disconnect(machineLogic()->uisession(), &UISession::sigSharedFolderChange,
               this, &UIMachineWindowNormal::sltSharedFolderChange);
    disconnect(machineLogic()->uisession(), &UISession::sigRecordingChange,
               this, &UIMachineWindowNormal::sltRecordingChange);
    disconnect(machineLogic()->uisession(), &UISession::sigCPUExecutionCapChange,
               this, &UIMachineWindowNormal::sltCPUExecutionCapChange);

    /* Call to base-class: */
    UIMachineWindow::cleanupSessionConnections();
}

bool UIMachineWindowNormal::event(QEvent *pEvent)
{
    switch (pEvent->type())
    {
        case QEvent::Resize:
        {
#ifdef VBOX_WS_X11
            /* Prevent handling if fake screen detected: */
            if (gpDesktop->isFakeScreenDetected())
                break;
#endif /* VBOX_WS_X11 */

            QResizeEvent *pResizeEvent = static_cast<QResizeEvent*>(pEvent);
            if (!isMaximizedChecked())
            {
                m_geometry.setSize(pResizeEvent->size());
#ifdef VBOX_WITH_DEBUGGER_GUI
                /* Update debugger window position: */
                updateDbgWindows();
#endif /* VBOX_WITH_DEBUGGER_GUI */
            }

            /* Restart geometry save timer: */
            if (m_iGeometrySaveTimerId != -1)
                killTimer(m_iGeometrySaveTimerId);
            m_iGeometrySaveTimerId = startTimer(300);

            /* Let listeners know about geometry changes: */
            emit sigGeometryChange(geometry());
            break;
        }
        case QEvent::Move:
        {
#ifdef VBOX_WS_X11
            /* Prevent handling if fake screen detected: */
            if (gpDesktop->isFakeScreenDetected())
                break;
#endif /* VBOX_WS_X11 */

            if (!isMaximizedChecked())
            {
                m_geometry.moveTo(geometry().x(), geometry().y());
#ifdef VBOX_WITH_DEBUGGER_GUI
                /* Update debugger window position: */
                updateDbgWindows();
#endif /* VBOX_WITH_DEBUGGER_GUI */
            }

            /* Restart geometry save timer: */
            if (m_iGeometrySaveTimerId != -1)
                killTimer(m_iGeometrySaveTimerId);
            m_iGeometrySaveTimerId = startTimer(300);

            /* Let listeners know about geometry changes: */
            emit sigGeometryChange(geometry());
            break;
        }
        case QEvent::WindowActivate:
        {
            /* Let listeners know about geometry changes: */
            emit sigGeometryChange(geometry());
            break;
        }
        /* Handle timer event started above: */
        case QEvent::Timer:
        {
            QTimerEvent *pTimerEvent = static_cast<QTimerEvent*>(pEvent);
            if (pTimerEvent->timerId() == m_iGeometrySaveTimerId)
            {
                killTimer(m_iGeometrySaveTimerId);
                m_iGeometrySaveTimerId = -1;

                /* HACK ALERT! Just ignore this if it arrives to late to be handled.  I typically get
                   these when the COM shutdown on windows flushes pending queue events.  The result
                   is typically a bunch of assertions, but sometimes a NULL pointer dereference for
                   variety.  Going forward here will probably re-instantiate some global objects
                   which were already cleaned up, so generally a bad idea.

                   A sample assertion stack:
                    # Child-SP          RetAddr           Call Site
                   00 00000052`300fe370 00007fff`36ac2cc9 UICommon!CVirtualBox::GetExtraDataKeys+0x80 [E:\vbox\svn\trunk\out\win.amd64\debug\obj\UICommon\include\COMWrappers.cpp @ 3851]
                   01 00000052`300fe430 00007fff`36ac2bf8 UICommon!UIExtraDataManager::prepareGlobalExtraDataMap+0xb9 [E:\vbox\svn\trunk\src\VBox\Frontends\VirtualBox\src\extradata\UIExtraDataManager.cpp @ 4845]
                   02 00000052`300fe590 00007fff`36ab1896 UICommon!UIExtraDataManager::prepare+0x28 [E:\vbox\svn\trunk\src\VBox\Frontends\VirtualBox\src\extradata\UIExtraDataManager.cpp @ 4833]
                   03 00000052`300fe5c0 00007ff7`69db2897 UICommon!UIExtraDataManager::instance+0x66 [E:\vbox\svn\trunk\src\VBox\Frontends\VirtualBox\src\extradata\UIExtraDataManager.cpp @ 2011]
                   04 00000052`300fe610 00007fff`35274990 VirtualBoxVM!UIMachineWindowNormal::event+0x4b7 [E:\vbox\svn\trunk\src\VBox\Frontends\VirtualBox\src\runtime\normal\UIMachineWindowNormal.cpp @ 546]
                   05 00000052`300fe6e0 00007fff`35273a13 Qt5WidgetsVBox!QApplicationPrivate::notify_helper+0x110
                   06 00000052`300fe710 00007fff`3cc3240a Qt5WidgetsVBox!QApplication::notify+0x18b3
                   07 00000052`300fec50 00007fff`3cc7cd09 Qt5CoreVBox!QCoreApplication::notifyInternal2+0xba
                   08 00000052`300fecc0 00007fff`3cc7bf7a Qt5CoreVBox!QEventDispatcherWin32Private::sendTimerEvent+0xf9
                   09 00000052`300fed10 00007fff`7631e7e8 Qt5CoreVBox!QEventDispatcherWin32::processEvents+0xc4a
                   0a 00000052`300fee30 00007fff`7631e229 USER32!UserCallWinProcCheckWow+0x2f8
                   0b 00000052`300fefc0 00007fff`370c2075 USER32!DispatchMessageWorker+0x249
                   0c 00000052`300ff040 00007fff`370c20e5 UICommon!com::NativeEventQueue::dispatchMessageOnWindows+0x145 [E:\vbox\svn\trunk\src\VBox\Main\glue\NativeEventQueue.cpp @ 416]
                   0d 00000052`300ff090 00007fff`370c1b19 UICommon!com::processPendingEvents+0x55 [E:\vbox\svn\trunk\src\VBox\Main\glue\NativeEventQueue.cpp @ 435]
                   0e 00000052`300ff130 00007fff`370c1ebd UICommon!com::NativeEventQueue::processEventQueue+0x149 [E:\vbox\svn\trunk\src\VBox\Main\glue\NativeEventQueue.cpp @ 562]
                   0f 00000052`300ff1d0 00007fff`370bfa9a UICommon!com::NativeEventQueue::uninit+0x2d [E:\vbox\svn\trunk\src\VBox\Main\glue\NativeEventQueue.cpp @ 260]
                   10 00000052`300ff210 00007fff`36b098e4 UICommon!com::Shutdown+0x5a [E:\vbox\svn\trunk\src\VBox\Main\glue\initterm.cpp @ 746]
                   11 00000052`300ff250 00007fff`36b88c43 UICommon!COMBase::CleanupCOM+0x74 [E:\vbox\svn\trunk\src\VBox\Frontends\VirtualBox\src\globals\COMDefs.cpp @ 168]
                   12 00000052`300ff2c0 00007fff`36a700c8 UICommon!UICommon::cleanup+0x313 [E:\vbox\svn\trunk\src\VBox\Frontends\VirtualBox\src\globals\UICommon.cpp @ 849]
                   13 00000052`300ff340 00007fff`36a82ab1 UICommon!UICommon::sltCleanup+0x28 [E:\vbox\svn\trunk\src\VBox\Frontends\VirtualBox\src\globals\UICommon.h @ 580]
                   14 00000052`300ff370 00007fff`36a81a9c UICommon!QtPrivate::FunctorCall<QtPrivate::IndexesList<>,QtPrivate::List<>,void,void (__cdecl UIMessageCenter::*)(void)>::call+0x31 [E:\vbox\svn\trunk\tools\win.amd64\qt\v5.15.2-r349\include\QtCore\qobjectdefs_impl.h @ 152]
                   15 00000052`300ff3b0 00007fff`36a82e45 UICommon!QtPrivate::FunctionPointer<void (__cdecl UIMessageCenter::*)(void)>::call<QtPrivate::List<>,void>+0x3c [E:\vbox\svn\trunk\tools\win.amd64\qt\v5.15.2-r349\include\QtCore\qobjectdefs_impl.h @ 186]
                   16 00000052`300ff3e0 00007fff`3cc51689 UICommon!QtPrivate::QSlotObject<void (__cdecl UIMessageCenter::*)(void),QtPrivate::List<>,void>::impl+0x95 [E:\vbox\svn\trunk\tools\win.amd64\qt\v5.15.2-r349\include\QtCore\qobjectdefs_impl.h @ 419]
                   17 00000052`300ff430 00007fff`3cc31465 Qt5CoreVBox!QObject::qt_static_metacall+0x1409
                   18 00000052`300ff580 00007fff`3cc313ef Qt5CoreVBox!QCoreApplicationPrivate::execCleanup+0x55
                   19 00000052`300ff5c0 00007ff7`69ce3b7a Qt5CoreVBox!QCoreApplication::exec+0x16f
                   1a 00000052`300ff620 00007ff7`69ce4174 VirtualBoxVM!TrustedMain+0x47a [E:\vbox\svn\trunk\src\VBox\Frontends\VirtualBox\src\main.cpp @ 570]
                   1b 00000052`300ff8b0 00007ff7`69e08af8 VirtualBoxVM!main+0x4a4 [E:\vbox\svn\trunk\src\VBox\Frontends\VirtualBox\src\main.cpp @ 739]
                   1c (Inline Function) --------`-------- VirtualBoxVM!invoke_main+0x22 [d:\A01\_work\6\s\src\vctools\crt\vcstartup\src\startup\exe_common.inl @ 78]
                   1d 00000052`300ffa00 00007fff`75107034 VirtualBoxVM!__scrt_common_main_seh+0x10c [d:\A01\_work\6\s\src\vctools\crt\vcstartup\src\startup\exe_common.inl @ 288]
                   1e 00000052`300ffa40 00007fff`76ae2651 KERNEL32!BaseThreadInitThunk+0x14
                   1f 00000052`300ffa70 00000000`00000000 ntdll!RtlUserThreadStart+0x21 */
                if (!UICommon::instance()->isCleaningUp())
                {
                    LogRel2(("GUI: UIMachineWindowNormal: Saving geometry as: Origin=%dx%d, Size=%dx%d\n",
                             m_geometry.x(), m_geometry.y(), m_geometry.width(), m_geometry.height()));
                    gEDataManager->setMachineWindowGeometry(machineLogic()->visualStateType(),
                                                            m_uScreenId, m_geometry,
                                                            isMaximizedChecked(), uiCommon().managedVMUuid());
                }
                else
                    LogRel2(("GUI: UIMachineWindowNormal: Ignoring geometry save timer arriving during cleanup\n"));
            }
            break;
        }
        default:
            break;
    }
    return UIMachineWindow::event(pEvent);
}

void UIMachineWindowNormal::showInNecessaryMode()
{
    /* Make sure this window should be shown at all: */
    if (!uisession()->isScreenVisible(m_uScreenId))
        return hide();

    /* Make sure this window is not minimized: */
    if (isMinimized())
        return;

    /* Show in normal mode: */
    show();

    /* Normalize machine-window geometry: */
    normalizeGeometry(true /* adjust position */, shouldResizeToGuestDisplay());

    /* Make sure machine-view have focus: */
    m_pMachineView->setFocus();
}

void UIMachineWindowNormal::restoreCachedGeometry()
{
    /* Restore the geometry cached by the window: */
    resize(m_geometry.size());
    move(m_geometry.topLeft());

    /* Adjust machine-view accordingly: */
    adjustMachineViewSize();
}

void UIMachineWindowNormal::normalizeGeometry(bool fAdjustPosition, bool fResizeToGuestDisplay)
{
#ifndef VBOX_GUI_WITH_CUSTOMIZATIONS1
    /* Skip if maximized: */
    if (isMaximized())
        return;

    /* Calculate client window offsets: */
    QRect frGeo = frameGeometry();
    const QRect geo = geometry();
    const int dl = geo.left() - frGeo.left();
    const int dt = geo.top() - frGeo.top();
    const int dr = frGeo.right() - geo.right();
    const int db = frGeo.bottom() - geo.bottom();

    if (fResizeToGuestDisplay)
    {
        /* Get the best size w/o scroll-bars: */
        QSize sh = sizeHint();

        /* If guest-screen auto-resize is not enabled
         * or the guest-additions doesn't support graphics
         * we should take scroll-bars size-hints into account: */
        if (!machineView()->isGuestAutoresizeEnabled() || !uisession()->isGuestSupportsGraphics())
        {
            if (machineView()->verticalScrollBar()->isVisible())
                sh -= QSize(machineView()->verticalScrollBar()->sizeHint().width(), 0);
            if (machineView()->horizontalScrollBar()->isVisible())
                sh -= QSize(0, machineView()->horizontalScrollBar()->sizeHint().height());
        }

        /* Resize the frame to fit the contents: */
        sh -= size();
        frGeo.setRight(frGeo.right() + sh.width());
        frGeo.setBottom(frGeo.bottom() + sh.height());
    }

    /* Adjust position if necessary: */
    if (fAdjustPosition)
        frGeo = UIDesktopWidgetWatchdog::normalizeGeometry(frGeo, gpDesktop->overallAvailableRegion());

    /* Finally, set the frame geometry: */
    UIDesktopWidgetWatchdog::setTopLevelGeometry(this, frGeo.left() + dl, frGeo.top() + dt,
                                    frGeo.width() - dl - dr, frGeo.height() - dt - db);
#else /* VBOX_GUI_WITH_CUSTOMIZATIONS1 */
    /* Customer request: There should no be
     * machine-window resize/move on machine-view resize: */
    Q_UNUSED(fAdjustPosition);
    Q_UNUSED(fResizeToGuestDisplay);
#endif /* VBOX_GUI_WITH_CUSTOMIZATIONS1 */
}

void UIMachineWindowNormal::updateAppearanceOf(int iElement)
{
    /* Call to base-class: */
    UIMachineWindow::updateAppearanceOf(iElement);

    /* Set status-bar indicator-pool auto update timer: */
    if (   m_pIndicatorsPool
        && iElement & UIVisualElement_IndicatorPoolStuff)
        m_pIndicatorsPool->setAutoUpdateIndicatorStates(statusBar()->isVisible() && uisession()->isRunning());
    /* Update status-bar indicator-pool appearance only when status-bar is visible: */
    if (   m_pIndicatorsPool
        && statusBar()->isVisible())
    {
        /* If VM is running: */
        if (uisession()->isRunning())
        {
            if (iElement & UIVisualElement_HDStuff)
                m_pIndicatorsPool->updateAppearance(IndicatorType_HardDisks);
            if (iElement & UIVisualElement_CDStuff)
                m_pIndicatorsPool->updateAppearance(IndicatorType_OpticalDisks);
            if (iElement & UIVisualElement_FDStuff)
                m_pIndicatorsPool->updateAppearance(IndicatorType_FloppyDisks);
            if (iElement & UIVisualElement_AudioStuff)
                m_pIndicatorsPool->updateAppearance(IndicatorType_Audio);
            if (iElement & UIVisualElement_NetworkStuff)
                m_pIndicatorsPool->updateAppearance(IndicatorType_Network);
            if (iElement & UIVisualElement_USBStuff)
                m_pIndicatorsPool->updateAppearance(IndicatorType_USB);
            if (iElement & UIVisualElement_SharedFolderStuff)
                m_pIndicatorsPool->updateAppearance(IndicatorType_SharedFolders);
            if (iElement & UIVisualElement_Display)
                m_pIndicatorsPool->updateAppearance(IndicatorType_Display);
            if (iElement & UIVisualElement_FeaturesStuff)
                m_pIndicatorsPool->updateAppearance(IndicatorType_Features);
        }
        /* If VM is running or paused: */
        if (uisession()->isRunning() || uisession()->isPaused())
        {
            if (iElement & UIVisualElement_Recording)
                m_pIndicatorsPool->updateAppearance(IndicatorType_Recording);
        }
    }
}

#ifndef VBOX_WS_MAC
void UIMachineWindowNormal::updateMenu()
{
    /* Rebuild menu-bar: */
    menuBar()->clear();
    foreach (QMenu *pMenu, actionPool()->menus())
        menuBar()->addMenu(pMenu);
}
#endif /* !VBOX_WS_MAC */

bool UIMachineWindowNormal::isMaximizedChecked()
{
#ifdef VBOX_WS_MAC
    /* On the Mac the WindowStateChange signal doesn't seems to be delivered
     * when the user get out of the maximized state. So check this ourself. */
    return ::darwinIsWindowMaximized(this);
#else /* VBOX_WS_MAC */
    return isMaximized();
#endif /* !VBOX_WS_MAC */
}
