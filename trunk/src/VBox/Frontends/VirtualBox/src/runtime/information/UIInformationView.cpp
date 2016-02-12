/* $Id$ */
/** @file
 * VBox Qt GUI - UIInformationView class implementation.
 */

/*
 * Copyright (C) 2016 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifdef VBOX_WITH_PRECOMPILED_HEADERS
# include <precomp.h>
#else  /* !VBOX_WITH_PRECOMPILED_HEADERS */

# include <QClipboard>
# include <QDebug>

/* GUI includes: */
# include "UIInformationView.h"
# include "UIInformationItem.h"

#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */

UIInformationView::UIInformationView(QWidget *pParent)
    : QListView(pParent)
{
}

void UIInformationView::updateData(const QModelIndex & topLeft, const QModelIndex & bottomRight)
{
    update(topLeft);
}

