/** @file
 * Virtual Disk Container API - Media type definitions shared with PDM.
 */

/*
 * Copyright (C) 2017 Oracle Corporation
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

#ifndef ___VBox_vdmedia_h
#define ___VBox_vdmedia_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <iprt/types.h>

/** @name VD container type.
 * @{
 */
typedef enum VDTYPE
{
    /** Invalid. */
    VDTYPE_INVALID = 0,
    /** HardDisk */
    VDTYPE_HDD,
    /** Any kind of optical disc (CD/DVD etc.). */
    VDTYPE_OPTICAL_DISC,
    /** Floppy. */
    VDTYPE_FLOPPY
} VDTYPE;
/** @}*/

/** @name VD medium type.
 * @{
 */
typedef enum VDMEDIUMTYPE
{
    /** Invalid. */
    VDMEDIUMTYPE_INVALID = 0,
    /** HardDisk (spinning platter or SSD). */
    VDMEDIUMTYPE_HDD,
    /** CD-ROM */
    VDMEDIUMTYPE_CDROM,
    /** DVD-ROM */
    VDMEDIUMTYPE_DVDROM,
    /** BluRay. */
    VDMEDIUMTYPE_BD,
    /** 360KB 5 1/4" floppy. */
    VDMEDIUMTYPE_FLOPPY_360,
    /** 720KB 3 1/2" floppy. */
    VDMEDIUMTYPE_FLOPPY_720,
    /** 1.2MB 5 1/4" floppy. */
    VDMEDIUMTYPE_FLOPPY_1_20,
    /** 1.44MB 3 1/2" floppy. */
    VDMEDIUMTYPE_FLOPPY_1_44,
    /** 2.88MB 3 1/2" floppy. */
    VDMEDIUMTYPE_FLOPPY_2_88,
    /** Fake disk that can take up to 15.6 MB images.
     * C=255, H=2, S=63.  */
    VDMEDIUMTYPE_FLOPPY_FAKE_15_6,
    /** Fake disk that can take up to 63.5 MB images.
     * C=255, H=2, S=255.  */
    VDMEDIUMTYPE_FLOPPY_FAKE_63_5
} VDMEDIUMTYPE;
/** @} */

/** Check if the given medium type is a floppy. */
#define VDMEDIUMTYPE_IS_FLOPPY(a_enmType) ( (a_enmType) >= VDMEDIUMTYPE_FLOPPY_360 && (a_enmType) <= VDMEDIUMTYPE_FLOPPY_2_88 )

/**
 * Disk geometry.
 */
typedef struct VDGEOMETRY
{
    /** Number of cylinders. */
    uint32_t    cCylinders;
    /** Number of heads. */
    uint32_t    cHeads;
    /** Number of sectors. */
    uint32_t    cSectors;
} VDGEOMETRY;

/** Pointer to disk geometry. */
typedef VDGEOMETRY *PVDGEOMETRY;
/** Pointer to constant disk geometry. */
typedef const VDGEOMETRY *PCVDGEOMETRY;

/**
 * Disk region data form known to us from various standards.
 */
typedef enum VDREGIONDATAFORM
{
    /** Invalid data form. */
    VDREGIONDATAFORM_INVALID = 0,
    /** Raw data, no standardized format. */
    VDREGIONDATAFORM_RAW,
    /** CD-DA (audio CD), 2352 bytes of data. */
    VDREGIONDATAFORM_CDDA,
    /** CDDA data is pause. */
    VDREGIONDATAFORM_CDDA_PAUSE,
    /** Mode 1 with 2048 bytes sector size. */
    VDREGIONDATAFORM_MODE1_2048,
    /** Mode 1 with 2352 bytes sector size. */
    VDREGIONDATAFORM_MODE1_2352,
    /** Mode 1 with 0 bytes sector size (generated by the drive). */
    VDREGIONDATAFORM_MODE1_0,
    /** XA Mode with 2336 bytes sector size. */
    VDREGIONDATAFORM_XA_2336,
    /** XA Mode with 2352 bytes sector size. */
    VDREGIONDATAFORM_XA_2352,
    /** XA Mode with 0 bytes sector size (generated by the drive). */
    VDREGIONDATAFORM_XA_0,
    /** Mode 2 with 2336 bytes sector size. */
    VDREGIONDATAFORM_MODE2_2336,
    /** Mode 2 with 2352 bytes sector size. */
    VDREGIONDATAFORM_MODE2_2352,
    /** Mode 2 with 0 bytes sector size (generated by the drive). */
    VDREGIONDATAFORM_MODE2_0
} VDREGIONDATAFORM;
/** Pointer to a region data form. */
typedef VDREGIONDATAFORM *PVDREGIONDATAFORM;
/** Pointer to a const region data form. */
typedef const VDREGIONDATAFORM PCVDREGIONDATAFORM;

/**
 * Disk region metadata forms known to us.
 */
typedef enum VDREGIONMETADATAFORM
{
    /** Invalid metadata form. */
    VDREGIONMETADATAFORM_INVALID = 0,
    /** No metadata assined to the region. */
    VDREGIONMETADATAFORM_NONE,
    /** Raw metadata, no standardized format. */
    VDREGIONMETADATAFORM_RAW
} VDREGIONMETADATAFORM;
/** Pointer to a region metadata form. */
typedef VDREGIONMETADATAFORM *PVDREGIONMETADATAFORM;
/** Pointer to a const region metadata form. */
typedef const VDREGIONMETADATAFORM PCVDREGIONMETADATAFORM;

/**
 * Disk region descriptor.
 */
typedef struct VDREGIONDESC
{
    /** Start of the region in bytes or LBA number (depending on the flag in the
     * list header). */
    uint64_t                 offRegion;
    /** Overall size of the region in bytes or number of blocks (depending on the
     * flag in the list header). */
    uint64_t                 cRegionBlocksOrBytes;
    /** Size of one block in bytes, containing user and metadata. */
    uint64_t                 cbBlock;
    /** User data form of the block. */
    VDREGIONDATAFORM         enmDataForm;
    /** Metadata form of the block. */
    VDREGIONMETADATAFORM     enmMetadataForm;
    /** Size of the data block in bytes. */
    uint64_t                 cbData;
    /** Size of the metadata in a block in bytes. */
    uint64_t                 cbMetadata;
} VDREGIONDESC;
/** Pointer to a region descriptor. */
typedef VDREGIONDESC *PVDREGIONDESC;
/** Pointer to a constant region descriptor. */
typedef const VDREGIONDESC *PCVDREGIONDESC;

/**
 * Disk region list.
 */
typedef struct VDREGIONLIST
{
    /** Flags valid for the region list. */
    uint32_t                 fFlags;
    /** Number of regions in the descriptor array. */
    uint32_t                 cRegions;
    /** Region descriptors - variable in size. */
    VDREGIONDESC             aRegions[RT_FLEXIBLE_ARRAY_NESTED];
} VDREGIONLIST;
/** Pointer to a region list. */
typedef VDREGIONLIST *PVDREGIONLIST;
/** Pointer to a constant region list. */
typedef const VDREGIONLIST *PCVDREGIONLIST;
/** Pointer to a region list pointer. */
typedef PVDREGIONLIST *PPVDREGIONLIST;

/** @name Valid region list flags.
 * @{
 */
/** When set the region start offset and size are given in numbers of blocks
 * instead of byte offsets and sizes. */
#define VD_REGION_LIST_F_LOC_SIZE_BLOCKS     RT_BIT_32(0)
/** Mask of all valid flags. */
#define VD_REGION_LIST_F_VALID               (VD_REGION_LIST_F_LOC_SIZE_BLOCKS)
/** @} */

#endif /* !___VBox_vdmedia_h */

