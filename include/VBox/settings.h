/** @file
 * Settings file data structures.
 *
 * These structures are created by the settings file loader and filled with values
 * copied from the raw XML data. This was all new with VirtualBox 3.1 and allows us
 * to finally make the XML reader version-independent and read VirtualBox XML files
 * from earlier and even newer (future) versions without requiring complicated,
 * tedious and error-prone XSLT conversions.
 *
 * It is this file that defines all structures that map VirtualBox global and
 * machine settings to XML files. These structures are used by the rest of Main,
 * even though this header file does not require anything else in Main.
 *
 * Note: Headers in Main code have been tweaked to only declare the structures
 * defined here so that this header need only be included from code files that
 * actually use these structures.
 */

/*
 * Copyright (C) 2007-2010 Sun Microsystems, Inc.
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
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

#ifndef ___VBox_settings_h
#define ___VBox_settings_h

#include <iprt/time.h>

#include "VBox/com/VirtualBox.h"

#include <VBox/com/Guid.h>
#include <VBox/com/string.h>

#include <list>
#include <map>

namespace xml
{
    class ElementNode;
}

namespace settings
{

class ConfigFileError;

////////////////////////////////////////////////////////////////////////////////
//
// Helper classes
//
////////////////////////////////////////////////////////////////////////////////

// ExtraDataItem (used by both VirtualBox.xml and machines XML)
typedef std::map<com::Utf8Str, com::Utf8Str> ExtraDataItemsMap;
struct USBDeviceFilter;
typedef std::list<USBDeviceFilter> USBDeviceFiltersList;

/**
 * Common base class for both MainConfigFile and MachineConfigFile
 * which contains some common logic for both.
 */
class ConfigFileBase
{
public:
    bool fileExists();

    void copyBaseFrom(const ConfigFileBase &b);

protected:
    ConfigFileBase(const com::Utf8Str *pstrFilename);
    ~ConfigFileBase();

    void parseUUID(com::Guid &guid,
                   const com::Utf8Str &strUUID) const;
    void parseTimestamp(RTTIMESPEC &timestamp,
                        const com::Utf8Str &str) const;

    com::Utf8Str makeString(const RTTIMESPEC &tm);
    com::Utf8Str makeString(const com::Guid &guid);

    void readExtraData(const xml::ElementNode &elmExtraData,
                       ExtraDataItemsMap &map);
    void readUSBDeviceFilters(const xml::ElementNode &elmDeviceFilters,
                              USBDeviceFiltersList &ll);

    void createStubDocument();

    void writeExtraData(xml::ElementNode &elmParent, const ExtraDataItemsMap &me);
    void writeUSBDeviceFilters(xml::ElementNode &elmParent,
                               const USBDeviceFiltersList &ll,
                               bool fHostMode);

    void clearDocument();

    struct Data;
    Data *m;

    friend class ConfigFileError;
};

////////////////////////////////////////////////////////////////////////////////
//
// Structures shared between Machine XML and VirtualBox.xml
//
////////////////////////////////////////////////////////////////////////////////

/**
 * USB device filter definition. This struct is used both in MainConfigFile
 * (for global USB filters) and MachineConfigFile (for machine filters).
 *
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct USBDeviceFilter
{
    USBDeviceFilter()
        : fActive(false),
          action(USBDeviceFilterAction_Null),
          ulMaskedInterfaces(0)
    {}

    bool operator==(const USBDeviceFilter&u) const;

    com::Utf8Str            strName;
    bool                    fActive;
    com::Utf8Str            strVendorId,
                            strProductId,
                            strRevision,
                            strManufacturer,
                            strProduct,
                            strSerialNumber,
                            strPort;
    USBDeviceFilterAction_T action;                 // only used with host USB filters
    com::Utf8Str            strRemote;              // irrelevant for host USB objects
    uint32_t                ulMaskedInterfaces;     // irrelevant for host USB objects
};

////////////////////////////////////////////////////////////////////////////////
//
// VirtualBox.xml structures
//
////////////////////////////////////////////////////////////////////////////////

struct Host
{
    USBDeviceFiltersList    llUSBDeviceFilters;
};

struct SystemProperties
{
    SystemProperties()
        : ulLogHistoryCount(3)
    {}

    com::Utf8Str            strDefaultMachineFolder;
    com::Utf8Str            strDefaultHardDiskFolder;
    com::Utf8Str            strDefaultHardDiskFormat;
    com::Utf8Str            strRemoteDisplayAuthLibrary;
    com::Utf8Str            strWebServiceAuthLibrary;
    uint32_t                ulLogHistoryCount;
};

typedef std::map<com::Utf8Str, com::Utf8Str> PropertiesMap;

struct Medium;
typedef std::list<Medium> MediaList;

struct Medium
{
    com::Guid       uuid;
    com::Utf8Str    strLocation;
    com::Utf8Str    strDescription;

    // the following are for hard disks only:
    com::Utf8Str    strFormat;
    bool            fAutoReset;         // optional, only for diffs, default is false
    PropertiesMap   properties;
    MediumType_T    hdType;

    MediaList       llChildren;         // only used with hard disks
};

struct MachineRegistryEntry
{
    com::Guid       uuid;
    com::Utf8Str    strSettingsFile;
};
typedef std::list<MachineRegistryEntry> MachinesRegistry;

struct DHCPServer
{
    com::Utf8Str    strNetworkName,
                    strIPAddress,
                    strIPNetworkMask,
                    strIPLower,
                    strIPUpper;
    bool            fEnabled;
};
typedef std::list<DHCPServer> DHCPServersList;

class MainConfigFile : public ConfigFileBase
{
public:
    MainConfigFile(const com::Utf8Str *pstrFilename);

    typedef enum {Error, HardDisk, DVDImage, FloppyImage} MediaType;
    void readMedium(MediaType t, const xml::ElementNode &elmMedium, MediaList &llMedia);
    void readMediaRegistry(const xml::ElementNode &elmMediaRegistry);
    void readMachineRegistry(const xml::ElementNode &elmMachineRegistry);
    void readDHCPServers(const xml::ElementNode &elmDHCPServers);

    void writeHardDisk(xml::ElementNode &elmMedium,
                       const Medium &m,
                       uint32_t level);
    void write(const com::Utf8Str strFilename);

    Host                    host;
    SystemProperties        systemProperties;
    MediaList               llHardDisks,
                            llDvdImages,
                            llFloppyImages;
    MachinesRegistry        llMachines;
    DHCPServersList         llDhcpServers;
    ExtraDataItemsMap       mapExtraDataItems;
};

////////////////////////////////////////////////////////////////////////////////
//
// Machine XML structures
//
////////////////////////////////////////////////////////////////////////////////

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct VRDPSettings
{
    VRDPSettings()
        : fEnabled(true),
          authType(VRDPAuthType_Null),
          ulAuthTimeout(5000),
          fAllowMultiConnection(false),
          fReuseSingleConnection(false)
    {}

    bool operator==(const VRDPSettings& v) const;

    bool            fEnabled;
    com::Utf8Str    strPort;
    com::Utf8Str    strNetAddress;
    VRDPAuthType_T  authType;
    uint32_t        ulAuthTimeout;
    bool            fAllowMultiConnection,
                    fReuseSingleConnection;
};

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct BIOSSettings
{
    BIOSSettings()
        : fACPIEnabled(true),
          fIOAPICEnabled(false),
          fLogoFadeIn(true),
          fLogoFadeOut(true),
          ulLogoDisplayTime(0),
          biosBootMenuMode(BIOSBootMenuMode_MessageAndMenu),
          fPXEDebugEnabled(false),
          llTimeOffset(0)
    {}

    bool operator==(const BIOSSettings &d) const;

    bool            fACPIEnabled,
                    fIOAPICEnabled,
                    fLogoFadeIn,
                    fLogoFadeOut;
    uint32_t        ulLogoDisplayTime;
    com::Utf8Str    strLogoImagePath;
    BIOSBootMenuMode_T  biosBootMenuMode;
    bool            fPXEDebugEnabled;
    int64_t         llTimeOffset;
};

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct USBController
{
    USBController()
        : fEnabled(false),
          fEnabledEHCI(false)
    {}

    bool operator==(const USBController &u) const;

    bool                    fEnabled;
    bool                    fEnabledEHCI;
    USBDeviceFiltersList    llDeviceFilters;
};

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct NetworkAdapter
{
    NetworkAdapter()
        : ulSlot(0),
          type(NetworkAdapterType_Am79C970A),
          fEnabled(false),
          fCableConnected(false),
          ulLineSpeed(0),
          fTraceEnabled(false),
          mode(NetworkAttachmentType_Null)
    {}

    bool operator==(const NetworkAdapter &n) const;

    uint32_t                ulSlot;

    NetworkAdapterType_T    type;
    bool                    fEnabled;
    com::Utf8Str            strMACAddress;
    bool                    fCableConnected;
    uint32_t                ulLineSpeed;
    bool                    fTraceEnabled;
    com::Utf8Str            strTraceFile;

    NetworkAttachmentType_T mode;
    com::Utf8Str            strName;            // with NAT: nat network or empty;
                                                // with bridged: host interface or empty;
                                                // otherwise: network name (required)
};
typedef std::list<NetworkAdapter> NetworkAdaptersList;

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct SerialPort
{
    SerialPort()
        : ulSlot(0),
          fEnabled(false),
          ulIOBase(4),
          ulIRQ(0x3f8),
          portMode(PortMode_Disconnected),
          fServer(false)
    {}

    bool operator==(const SerialPort &n) const;

    uint32_t        ulSlot;

    bool            fEnabled;
    uint32_t        ulIOBase;
    uint32_t        ulIRQ;
    PortMode_T      portMode;
    com::Utf8Str    strPath;
    bool            fServer;
};
typedef std::list<SerialPort> SerialPortsList;

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct ParallelPort
{
    ParallelPort()
        : ulSlot(0),
          fEnabled(false),
          ulIOBase(0x378),
          ulIRQ(4)
    {}

    bool operator==(const ParallelPort &d) const;

    uint32_t        ulSlot;

    bool            fEnabled;
    uint32_t        ulIOBase;
    uint32_t        ulIRQ;
    com::Utf8Str    strPath;
};
typedef std::list<ParallelPort> ParallelPortsList;

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct AudioAdapter
{
    AudioAdapter()
        : fEnabled(true),
          controllerType(AudioControllerType_AC97),
          driverType(AudioDriverType_Null)
    {}

    bool operator==(const AudioAdapter &a) const
    {
        return     (this == &a)
                || (    (fEnabled        == a.fEnabled)
                     && (controllerType  == a.controllerType)
                     && (driverType      == a.driverType)
                   );
    }

    bool                    fEnabled;
    AudioControllerType_T   controllerType;
    AudioDriverType_T       driverType;
};

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct SharedFolder
{
    SharedFolder()
        : fWritable(false)
    {}

    bool operator==(const SharedFolder &a) const;

    com::Utf8Str    strName,
                    strHostPath;
    bool            fWritable;
};
typedef std::list<SharedFolder> SharedFoldersList;

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct GuestProperty
{
    GuestProperty()
        : timestamp(0)
    {};

    bool operator==(const GuestProperty &g) const;

    com::Utf8Str    strName,
                    strValue;
    uint64_t        timestamp;
    com::Utf8Str    strFlags;
};
typedef std::list<GuestProperty> GuestPropertiesList;

typedef std::map<uint32_t, DeviceType_T> BootOrderMap;

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct CpuIdLeaf
{
    CpuIdLeaf()
        : ulId(UINT32_MAX),
          ulEax(0),
          ulEbx(0),
          ulEcx(0),
          ulEdx(0)
    {}

    bool operator==(const CpuIdLeaf &c) const
    {
        return (    (this == &c)
                 || (    (ulId      == c.ulId)
                      && (ulEax     == c.ulEax)
                      && (ulEbx     == c.ulEbx)
                      && (ulEcx     == c.ulEcx)
                      && (ulEdx     == c.ulEdx)
                    )
               );
    }

    uint32_t                ulId;
    uint32_t                ulEax;
    uint32_t                ulEbx;
    uint32_t                ulEcx;
    uint32_t                ulEdx;
};
typedef std::list<CpuIdLeaf> CpuIdLeafsList;

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct Cpu
{
    Cpu()
        : ulId(UINT32_MAX)
    {}

    bool operator==(const Cpu &c) const
    {
        return (ulId == c.ulId);
    }

    uint32_t                ulId;
};
typedef std::list<Cpu> CpuList;

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct IoSettings
{
    IoSettings();

    bool operator==(const IoSettings &i) const
    {
        return (   (ioMgrType        == i.ioMgrType)
                && (ioBackendType    == i.ioBackendType)
                && (fIoCacheEnabled  == i.fIoCacheEnabled)
                && (ulIoCacheSize    == i.ulIoCacheSize)
                && (ulIoBandwidthMax == i.ulIoBandwidthMax));
    }

    IoMgrType_T     ioMgrType;
    IoBackendType_T ioBackendType;
    bool            fIoCacheEnabled;
    uint32_t        ulIoCacheSize;
    uint32_t        ulIoBandwidthMax;
};

/**
 * Representation of Machine hardware; this is used in the MachineConfigFile.hardwareMachine
 * field.
 *
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct Hardware
{
    Hardware();

    bool operator==(const Hardware&) const;

    com::Utf8Str        strVersion;             // hardware version, optional
    com::Guid           uuid;                   // hardware uuid, optional (null).

    bool                fHardwareVirt,
                        fHardwareVirtExclusive,
                        fNestedPaging,
                        fLargePages,
                        fVPID,
                        fSyntheticCpu,
                        fPAE;
    uint32_t            cCPUs;
    bool                fCpuHotPlug;            // requires settings version 1.10 (VirtualBox 3.2)
    CpuList             llCpus;                 // requires settings version 1.10 (VirtualBox 3.2)
    bool                fHpetEnabled;           // requires settings version 1.10 (VirtualBox 3.2)

    CpuIdLeafsList      llCpuIdLeafs;

    uint32_t            ulMemorySizeMB;

    BootOrderMap        mapBootOrder;           // item 0 has highest priority

    uint32_t            ulVRAMSizeMB;
    uint32_t            cMonitors;
    bool                fAccelerate3D,
                        fAccelerate2DVideo;     // requires settings version 1.8 (VirtualBox 3.1)
    FirmwareType_T      firmwareType;           // requires settings version 1.9 (VirtualBox 3.1)

    PointingHidType_T   pointingHidType;        // requires settings version 1.10 (VirtualBox 3.2)
    KeyboardHidType_T   keyboardHidType;        // requires settings version 1.10 (VirtualBox 3.2)

    VRDPSettings        vrdpSettings;

    BIOSSettings        biosSettings;
    USBController       usbController;
    NetworkAdaptersList llNetworkAdapters;
    SerialPortsList     llSerialPorts;
    ParallelPortsList   llParallelPorts;
    AudioAdapter        audioAdapter;

    // technically these two have no business in the hardware section, but for some
    // clever reason <Hardware> is where they are in the XML....
    SharedFoldersList   llSharedFolders;
    ClipboardMode_T     clipboardMode;

    uint32_t            ulMemoryBalloonSize;
    uint32_t            ulStatisticsUpdateInterval;

    GuestPropertiesList llGuestProperties;
    com::Utf8Str        strNotificationPatterns;

    IoSettings          ioSettings;             // requires settings version 1.10 (VirtualBox 3.2)
};

/**
 * A device attached to a storage controller. This can either be a
 * hard disk or a DVD drive or a floppy drive and also specifies
 * which medium is "in" the drive; as a result, this is a combination
 * of the Main IMedium and IMediumAttachment interfaces.
 *
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct AttachedDevice
{
    AttachedDevice()
        : deviceType(DeviceType_Null),
          fPassThrough(false),
          lPort(0),
          lDevice(0)
    {}

    bool operator==(const AttachedDevice &a) const;

    DeviceType_T        deviceType;         // only HardDisk, DVD or Floppy are allowed

    // DVDs can be in pass-through mode:
    bool                fPassThrough;

    int32_t             lPort;
    int32_t             lDevice;

    // if an image file is attached to the device (ISO, RAW, or hard disk image such as VDI),
    // this is its UUID; it depends on deviceType which media registry this then needs to
    // be looked up in. If no image file (only permitted for DVDs and floppies), then the UUID is NULL
    com::Guid           uuid;

    // for DVDs and floppies, the attachment can also be a host device:
    com::Utf8Str        strHostDriveSrc;        // if != NULL, value of <HostDrive>/@src
};
typedef std::list<AttachedDevice> AttachedDevicesList;

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct StorageController
{
    StorageController()
        : storageBus(StorageBus_IDE),
          controllerType(StorageControllerType_PIIX3),
          ulPortCount(2),
          ulInstance(0),
          lIDE0MasterEmulationPort(0),
          lIDE0SlaveEmulationPort(0),
          lIDE1MasterEmulationPort(0),
          lIDE1SlaveEmulationPort(0)
    {}

    bool operator==(const StorageController &s) const;

    com::Utf8Str            strName;
    StorageBus_T            storageBus;             // _SATA, _SCSI, _IDE
    StorageControllerType_T controllerType;
    uint32_t                ulPortCount;
    uint32_t                ulInstance;

    // only for when controllerType == StorageControllerType_IntelAhci:
    int32_t                 lIDE0MasterEmulationPort,
                            lIDE0SlaveEmulationPort,
                            lIDE1MasterEmulationPort,
                            lIDE1SlaveEmulationPort;

    AttachedDevicesList     llAttachedDevices;
};
typedef std::list<StorageController> StorageControllersList;

/**
 * We wrap the storage controllers list into an extra struct so we can
 * use an undefined struct without needing std::list<> in all the headers.
 *
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct Storage
{
    bool operator==(const Storage &s) const;

    StorageControllersList  llStorageControllers;
};

struct Snapshot;
typedef std::list<Snapshot> SnapshotsList;

/**
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by MachineConfigFile::operator==(), or otherwise
 * your settings might never get saved.
 */
struct Snapshot
{
    bool operator==(const Snapshot &s) const;

    com::Guid       uuid;
    com::Utf8Str    strName,
                    strDescription;             // optional
    RTTIMESPEC      timestamp;

    com::Utf8Str    strStateFile;               // for online snapshots only

    Hardware        hardware;
    Storage         storage;

    SnapshotsList   llChildSnapshots;
};

/**
 * MachineConfigFile represents an XML machine configuration. All the machine settings
 * that go out to the XML (or are read from it) are in here.
 *
 * NOTE: If you add any fields in here, you must update a) the constructor and b)
 * the operator== which is used by Machine::saveSettings(), or otherwise your settings
 * might never get saved.
 */
class MachineConfigFile : public ConfigFileBase
{
public:
    MachineConfigFile(const com::Utf8Str *pstrFilename);

    bool operator==(const MachineConfigFile &m) const;

    void readNetworkAdapters(const xml::ElementNode &elmHardware, NetworkAdaptersList &ll);
    void readCpuIdTree(const xml::ElementNode &elmCpuid, CpuIdLeafsList &ll);
    void readCpuTree(const xml::ElementNode &elmCpu, CpuList &ll);
    void readSerialPorts(const xml::ElementNode &elmUART, SerialPortsList &ll);
    void readParallelPorts(const xml::ElementNode &elmLPT, ParallelPortsList &ll);
    void readGuestProperties(const xml::ElementNode &elmGuestProperties, Hardware &hw);
    void readStorageControllerAttributes(const xml::ElementNode &elmStorageController, StorageController &sctl);
    void readHardware(const xml::ElementNode &elmHardware, Hardware &hw, Storage &strg);
    void readHardDiskAttachments_pre1_7(const xml::ElementNode &elmHardDiskAttachments, Storage &strg);
    void readStorageControllers(const xml::ElementNode &elmStorageControllers, Storage &strg);
    void readDVDAndFloppies_pre1_9(const xml::ElementNode &elmHardware, Storage &strg);
    void readSnapshot(const xml::ElementNode &elmSnapshot, Snapshot &snap);
    void convertOldOSType_pre1_5(com::Utf8Str &str);
    void readMachine(const xml::ElementNode &elmMachine);

    void writeHardware(xml::ElementNode &elmParent, const Hardware &hw, const Storage &strg);
    void writeStorageControllers(xml::ElementNode &elmParent, const Storage &st);
    void writeSnapshot(xml::ElementNode &elmParent, const Snapshot &snap);
    void bumpSettingsVersionIfNeeded();
    void write(const com::Utf8Str &strFilename);

    com::Guid               uuid;
    com::Utf8Str            strName;
    bool                    fNameSync;
    com::Utf8Str            strDescription;
    com::Utf8Str            strOsType;
    com::Utf8Str            strStateFile;
    com::Guid               uuidCurrentSnapshot;
    com::Utf8Str            strSnapshotFolder;
    bool                    fTeleporterEnabled;
    uint32_t                uTeleporterPort;
    com::Utf8Str            strTeleporterAddress;
    com::Utf8Str            strTeleporterPassword;
    bool                    fRTCUseUTC;

    bool                    fCurrentStateModified;      // optional, default is true
    RTTIMESPEC              timeLastStateChange;        // optional, defaults to now
    bool                    fAborted;                   // optional, default is false

    Hardware                hardwareMachine;
    Storage                 storageMachine;

    ExtraDataItemsMap       mapExtraDataItems;

    SnapshotsList           llFirstSnapshot;            // first snapshot or empty list if there's none
};

} // namespace settings


#endif /* ___VBox_settings_h */
