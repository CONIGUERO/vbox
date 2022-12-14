# $Id$
## @file
# Include file for generating the workspace projects, called by the gen-XXX-workspace.sh generators.
#

#
# Copyright (C) 2022 Oracle Corporation
#
# This file is part of VirtualBox Open Source Edition (OSE), as
# available from http://www.virtualbox.org. This file is free software;
# you can redistribute it and/or modify it under the terms of the GNU
# General Public License (GPL) as published by the Free Software
# Foundation, in version 2 as it comes in the "COPYING" file of the
# VirtualBox OSE distribution. VirtualBox OSE is distributed in the
# hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
#

#
# Generate the projects and workspace.
#
# Note! The configs aren't optimal yet, lots of adjustment wrt headers left to be done.
#

# src/VBox/Runtime
my_generate_project "IPRT"          "src/VBox/Runtime"                      --begin-incs "include" "src/VBox/Runtime/include"               --end-includes "include/iprt" "src/VBox/Runtime"

# src/VBox/VMM
my_generate_project "VMM"           "src/VBox/VMM"                          --begin-incs "include" "src/VBox/VMM"                           --end-includes "src/VBox/VMM" \
    "include/VBox/vmm/cfgm.h" \
    "include/VBox/vmm/cpum.*" \
    "include/VBox/vmm/dbgf.h" \
    "include/VBox/vmm/em.h" \
    "include/VBox/vmm/gim.h" \
    "include/VBox/vmm/apic.h" \
    "include/VBox/vmm/gmm.*" \
    "include/VBox/vmm/gvm.*" \
    "include/VBox/vmm/hm*.*" \
    "include/VBox/vmm/iom.h" \
    "include/VBox/vmm/mm.h" \
    "include/VBox/vmm/patm.*" \
    "include/VBox/vmm/pdm*.h" \
    "include/VBox/vmm/pgm.*" \
    "include/VBox/vmm/selm.*" \
    "include/VBox/vmm/ssm.h" \
    "include/VBox/vmm/stam.*" \
    "include/VBox/vmm/tm.h" \
    "include/VBox/vmm/trpm.*" \
    "include/VBox/vmm/vm.*" \
    "include/VBox/vmm/vmm.*"

# src/VBox/Additions
my_generate_project "Add-darwin"    "src/VBox/Additions/darwin"             --begin-incs "include" "src/VBox/Additions/darwin"              --end-includes "src/VBox/Additions/darwin"
my_generate_project "Add-freebsd"   "src/VBox/Additions/freebsd"            --begin-incs "include" "src/VBox/Additions/freebsd"             --end-includes "src/VBox/Additions/freebsd"
my_generate_project "Add-haiku"     "src/VBox/Additions/haiku"              --begin-incs "include" "src/VBox/Additions/haiku"               --end-includes "src/VBox/Additions/haiku"
my_generate_project "Add-linux"     "src/VBox/Additions/linux"              --begin-incs "include" "src/VBox/Additions/linux"               --end-includes "src/VBox/Additions/linux"
my_generate_project "Add-os2"       "src/VBox/Additions/os2"                --begin-incs "include" "src/VBox/Additions/os2"                 --end-includes "src/VBox/Additions/os2"
my_generate_project "Add-solaris"   "src/VBox/Additions/solaris"            --begin-incs "include" "src/VBox/Additions/solaris"             --end-includes "src/VBox/Additions/solaris"
my_generate_project "Add-win"       "src/VBox/Additions/WINNT"              --begin-incs "include" "src/VBox/Additions/WINNT"               --end-includes "src/VBox/Additions/WINNT"
if test -z "$MY_OPT_MINIMAL"; then
    my_generate_project "Add-x11"   "src/VBox/Additions/x11"                --begin-incs "include" "src/VBox/Additions/x11"                 --end-includes "src/VBox/Additions/x11"
fi
my_generate_project "Add-Control"   "src/VBox/Additions/common/VBoxControl"   --begin-incs "include" "src/VBox/Additions/common/VBoxControl"  --end-includes "src/VBox/Additions/common/VBoxControl"
my_generate_project "Add-GuestDrv"  "src/VBox/Additions/common/VBoxGuest"     --begin-incs "include" "src/VBox/Additions/common/VBoxGuest"    --end-includes "src/VBox/Additions/common/VBoxGuest"      "include/VBox/VBoxGuest*.*"
my_generate_project "Add-Lib"       "src/VBox/Additions/common/VBoxGuest/lib" --begin-incs "include" "src/VBox/Additions/common/VBoxGuest/lib" --end-includes "src/VBox/Additions/common/VBoxGuest/lib" "include/VBox/VBoxGuest/lib/*.*"
my_generate_project "Add-Service"   "src/VBox/Additions/common/VBoxService"   --begin-incs "include" "src/VBox/Additions/common/VBoxService"  --end-includes "src/VBox/Additions/common/VBoxService"
my_generate_project "Add-VBoxVideo" "src/VBox/Additions/common/VBoxVideo"     --begin-incs "include" "src/VBox/Additions/common/VBoxVideo"    --end-includes "src/VBox/Additions/common/VBoxVideo"
if test -z "$MY_OPT_MINIMAL"; then
    my_generate_project "Add-pam"       "src/VBox/Additions/common/pam"         --begin-incs "include" "src/VBox/Additions/common/pam"          --end-includes "src/VBox/Additions/common/pam"
    my_generate_project "Add-cmn-test"  "src/VBox/Additions/common/testcase"    --begin-incs "include" "src/VBox/Additions/common/testcase"     --end-includes "src/VBox/Additions/common/testcase"
    my_generate_project "Add-CredProv"  "src/VBox/Additions/WINNT/VBoxCredProv" --begin-incs "include" "src/VBox/Additions/WINNT/VBoxCredProv"  --end-includes "src/VBox/Additions/WINNT/VBoxCredProv"
    my_generate_project "Add-GINA"      "src/VBox/Additions/WINNT/VBoxGINA"     --begin-incs "include" "src/VBox/Additions/WINNT/VBoxGINA"      --end-includes "src/VBox/Additions/WINNT/VBoxGINA"
fi

# src/VBox/Debugger
my_generate_project "Debugger"      "src/VBox/Debugger"                     --begin-incs "include" "src/VBox/Debugger"                      --end-includes "src/VBox/Debugger" "include/VBox/dbggui.h" "include/VBox/dbg.h"

# src/VBox/Devices
my_generate_project "Devices"       "src/VBox/Devices"                      --begin-incs "include" "src/VBox/Devices"                       --end-includes "src/VBox/Devices" "include/VBox/pci.h" "include/VBox/pdm*.h"
## @todo split this up.

# src/VBox/Disassembler
my_generate_project "DIS"           "src/VBox/Disassembler"                 --begin-incs "include" "src/VBox/Disassembler"                  --end-includes "src/VBox/Disassembler" "include/VBox/dis*.h"

# src/VBox/Frontends
if test -z "$MY_OPT_MINIMAL"; then
    my_generate_project "FE-VBoxAutostart"   "src/VBox/Frontends/VBoxAutostart"   --begin-incs "include" "src/VBox/Frontends/VBoxAutostart"       --end-includes "src/VBox/Frontends/VBoxAutostart"
    my_generate_project "FE-VBoxBugReport"   "src/VBox/Frontends/VBoxBugReport"   --begin-incs "include" "src/VBox/Frontends/VBoxBugReport"       --end-includes "src/VBox/Frontends/VBoxBugReport"
    my_generate_project "FE-VBoxBalloonCtrl" "src/VBox/Frontends/VBoxBalloonCtrl" --begin-incs "include" "src/VBox/Frontends/VBoxBalloonCtrl"     --end-includes "src/VBox/Frontends/VBoxBalloonCtrl"
fi
my_generate_project "FE-VBoxManage"      "src/VBox/Frontends/VBoxManage"      --begin-incs "include" "src/VBox/Frontends/VBoxManage"          --end-includes "src/VBox/Frontends/VBoxManage"
my_generate_project "FE-VBoxHeadless"    "src/VBox/Frontends/VBoxHeadless"    --begin-incs "include" "src/VBox/Frontends/VBoxHeadless"        --end-includes "src/VBox/Frontends/VBoxHeadless"
my_generate_project "FE-VBoxSDL"         "src/VBox/Frontends/VBoxSDL"         --begin-incs "include" "src/VBox/Frontends/VBoxSDL"             --end-includes "src/VBox/Frontends/VBoxSDL"
my_generate_project "FE-VBoxShell"       "src/VBox/Frontends/VBoxShell"       --begin-incs "include" "src/VBox/Frontends/VBoxShell"           --end-includes "src/VBox/Frontends/VBoxShell"
# noise - my_generate_project "FE-VBoxBFE"      "src/VBox/Frontends/VBoxBFE"      --begin-incs "include" "src/VBox/Frontends/VBoxBFE"            --end-includes "src/VBox/Frontends/VBoxBFE"
FE_VBOX_WRAPPERS=""
for d in ${MY_OUT_DIRS};
do
    if test -d "${MY_ROOT_DIR}/${d}/obj/VirtualBox/include"; then
        FE_VBOX_WRAPPERS="${d}/obj/VirtualBox/include"
        break
    fi
done
if test -n "${FE_VBOX_WRAPPERS}"; then
    my_generate_project "FE-VirtualBox" "src/VBox/Frontends/VirtualBox"     --begin-incs "include" "${FE_VBOX_WRAPPERS}"                    --end-includes "src/VBox/Frontends/VirtualBox" "${FE_VBOX_WRAPPERS}/COMWrappers.cpp" "${FE_VBOX_WRAPPERS}/COMWrappers.h"
else
    my_generate_project "FE-VirtualBox" "src/VBox/Frontends/VirtualBox"     --begin-incs "include"                                          --end-includes "src/VBox/Frontends/VirtualBox"
fi

# src/VBox/GuestHost
my_generate_project "HGSMI-GH"      "src/VBox/GuestHost/HGSMI"              --begin-incs "include"                                          --end-includes "src/VBox/GuestHost/HGSMI"
if test -z "$MY_OPT_MINIMAL"; then
    my_generate_project "DnD-GH"    "src/VBox/GuestHost/DragAndDrop"        --begin-incs "include"                                          --end-includes "src/VBox/GuestHost/DragAndDrop"
fi
my_generate_project "ShClip-GH"     "src/VBox/GuestHost/SharedClipboard"    --begin-incs "include"                                          --end-includes "src/VBox/GuestHost/SharedClipboard"

# src/VBox/HostDrivers
my_generate_project "SUP"           "src/VBox/HostDrivers/Support"          --begin-incs "include" "src/VBox/HostDrivers/Support"           --end-includes "src/VBox/HostDrivers/Support" "include/VBox/sup.h" "include/VBox/sup.mac"
my_generate_project "VBoxNetAdp"    "src/VBox/HostDrivers/VBoxNetAdp"       --begin-incs "include" "src/VBox/HostDrivers/VBoxNetAdp"        --end-includes "src/VBox/HostDrivers/VBoxNetAdp" "include/VBox/intnet.h"
my_generate_project "VBoxNetFlt"    "src/VBox/HostDrivers/VBoxNetFlt"       --begin-incs "include" "src/VBox/HostDrivers/VBoxNetFlt"        --end-includes "src/VBox/HostDrivers/VBoxNetFlt" "include/VBox/intnet.h"
my_generate_project "VBoxUSB"       "src/VBox/HostDrivers/VBoxUSB"          --begin-incs "include" "src/VBox/HostDrivers/VBoxUSB"           --end-includes "src/VBox/HostDrivers/VBoxUSB" "include/VBox/usblib*.h" "include/VBox/usbfilter.h"
my_generate_project "AdpCtl"        "src/VBox/HostDrivers/adpctl"           --begin-incs "include"                                          --end-includes "src/VBox/HostDrivers/adpctl"

# src/VBox/HostServices
my_generate_project "GuestCntl"     "src/VBox/HostServices/GuestControl"    --begin-incs "include" "src/VBox/HostServices/GuestControl"     --end-includes "src/VBox/HostServices/GuestControl"
my_generate_project "DragAndDrop"   "src/VBox/HostServices/DragAndDrop"     --begin-incs "include" "src/VBox/HostServices/DragAndDrop"      --end-includes "src/VBox/HostServices/DragAndDrop"
my_generate_project "GuestProps"    "src/VBox/HostServices/GuestProperties" --begin-incs "include" "src/VBox/HostServices/GuestProperties"  --end-includes "src/VBox/HostServices/GuestProperties"
my_generate_project "ShClip-HS"     "src/VBox/HostServices/SharedClipboard" --begin-incs "include" "src/VBox/HostServices/SharedClipboard"  --end-includes "src/VBox/HostServices/SharedClipboard"
my_generate_project "SharedFolders" "src/VBox/HostServices/SharedFolders"   --begin-incs "include" "src/VBox/HostServices/SharedFolders"    --end-includes "src/VBox/HostServices/SharedFolders" "include/VBox/shflsvc.h"

# src/VBox/ImageMounter
my_generate_project "ImageMounter"  "src/VBox/ImageMounter"                 --begin-incs "include" "src/VBox/ImageMounter"                  --end-includes "src/VBox/ImageMounter"

# src/VBox/Installer
my_generate_project "Installers"    "src/VBox/Installer"                    --begin-incs "include"                                          --end-includes "src/VBox/Installer"

# src/VBox/Main
my_generate_project "Main"          "src/VBox/Main"                         --begin-incs "include" "src/VBox/Main/include"                  --end-includes "src/VBox/Main" "include/VBox/com" "include/VBox/settings.h"
## @todo seperate webservices and Main. pick the right headers. added generated headers.

# src/VBox/Network
my_generate_project "Net-DHCP"      "src/VBox/NetworkServices/Dhcpd"        --begin-incs "include" "src/VBox/NetworkServices/NetLib"        --end-includes "src/VBox/NetworkServices/Dhcpd"
my_generate_project "Net-NAT"       "src/VBox/NetworkServices/NAT"          --begin-incs "include" "src/VBox/NetworkServices/NAT"           --end-includes "src/VBox/NetworkServices/NAT" "src/VBox/Devices/Network/slirp"
my_generate_project "Net-NetLib"    "src/VBox/NetworkServices/NetLib"       --begin-incs "include" "src/VBox/NetworkServices/NetLib"        --end-includes "src/VBox/NetworkServices/NetLib"

# src/VBox/RDP
my_generate_project "RDP-Client"    "src/VBox/RDP/client-1.8.4"             --begin-incs "include" "src/VBox/RDP/client-1.8.4"              --end-includes "src/VBox/RDP/client-1.8.4"
my_generate_project "RDP-Server"    "src/VBox/RDP/server"                   --begin-incs "include" "src/VBox/RDP/server"                    --end-includes "src/VBox/RDP/server"
my_generate_project "RDP-WebClient" "src/VBox/RDP/webclient"                --begin-incs "include" "src/VBox/RDP/webclient"                 --end-includes "src/VBox/RDP/webclient"
my_generate_project "RDP-Misc"      "src/VBox/RDP"                          --begin-incs "include"                                          --end-includes "src/VBox/RDP/auth" "src/VBox/RDP/tscpasswd" "src/VBox/RDP/x11server"

# src/VBox/Storage
my_generate_project "Storage"       "src/VBox/Storage"                      --begin-incs "include" "src/VBox/Storage"                       --end-includes "src/VBox/Storage"

# src/VBox/ValidationKit
my_generate_project "ValidationKit" "src/VBox/ValidationKit"                --begin-incs "include"                                          --end-includes "src/VBox/ValidationKit"

# src/VBox/ExtPacks
my_generate_project "ExtPacks"      "src/VBox/ExtPacks"                     --begin-incs "include"                                          --end-includes "src/VBox/ExtPacks"

# src/bldprogs
my_generate_project "bldprogs"      "src/bldprogs"                          --begin-incs "include"                                          --end-includes "src/bldprogs"

# A few things from src/lib
lib=$(my_get_newest_ver src/libs/zlib)
my_generate_project "zlib"          "${lib}"                                --begin-incs "include"                                          --end-includes "${lib}/*.c" "${lib}/*.h"
lib=$(my_get_newest_ver src/libs/liblzf)
my_generate_project "liblzf"        "${lib}"                                --begin-incs "include"                                          --end-includes "${lib}"
lib=$(my_get_newest_ver src/libs/libpng)
my_generate_project "libpng"        "${lib}"                                --begin-incs "include"                                          --end-includes "${lib}/*.c" "${lib}/*.h"
lib=$(my_get_newest_ver src/libs/openssl)
my_generate_project "openssl"       "${lib}"                                --begin-incs "include" "${lib}/crypto"                          --end-includes "${lib}"
lib=$(my_get_newest_ver src/libs/curl)
my_generate_project "curl"          "${lib}"                                --begin-incs "include" "${lib}/include"                         --end-includes "${lib}"
lib=$(my_get_newest_ver src/libs/softfloat)
my_generate_project "softfloat"     "${lib}"                                --begin-incs "include" "${lib}/source/include"                  --end-includes "${lib}"

# webtools
my_generate_project "webtools"      "webtools"                              --begin-incs "include" "webtools/tinderbox/server/Tinderbox3"   --end-includes "webtools"

# include/VBox
my_generate_project "VBoxHeaders"   "include"                               --begin-incs "include"                                          --end-includes "include/VBox"

# Misc
my_generate_project "misc"          "."                                     --begin-incs "include"                                          --end-includes \
    "configure" \
    "configure.vbs" \
    "Config.kmk" \
    "Makefile.kmk" \
    "src/Makefile.kmk" \
    "src/VBox/Makefile.kmk" \
    "tools/env.sh" \
    "tools/env.cmd" \
    "tools/envSub.vbs" \
    "tools/envSub.cmd" \
    "tools/win/vbscript"


# out/x.y/z/bin/sdk/bindings/xpcom
XPCOM_INCS="src/libs/xpcom18a4"
for d in \
    "out/${KBUILD_TARGET}.${KBUILD_TARGET_ARCH}/${KBUILD_TYPE}/dist/sdk/bindings/xpcom" \
    "out/${KBUILD_TARGET}.${KBUILD_TARGET_ARCH}/${KBUILD_TYPE}/bin/sdk/bindings/xpcom" \
    "out/linux.amd64/debug/bin/sdk/bindings/xpcom" \
    "out/linux.x86/debug/bin/sdk/bindings/xpcom" \
    "out/darwin.amd64/debug/dist/sdk/bindings/xpcom" \
    "out/darwin.x86/debug/bin/dist/bindings/xpcom" \
    "out/haiku.amd64/debug/bin/sdk/bindings/xpcom" \
    "out/haiku.x86/debug/bin/sdk/bindings/xpcom" \
    "out/solaris.amd64/debug/bin/sdk/bindings/xpcom" \
    "out/solaris.x86/debug/bin/sdk/bindings/xpcom";
do
    if test -d "${MY_ROOT_DIR}/${d}"; then
        my_generate_project "SDK-xpcom" "${d}"  --begin-incs "include" "${d}/include" --end-includes "${d}"
        XPCOM_INCS="${d}/include"
        break
    fi
done

# lib/xpcom
my_generate_project "xpcom"         "src/libs/xpcom18a4"                    --begin-incs "include" "${XPCOM_INCS}"                          --end-includes "src/libs/xpcom18a4"
