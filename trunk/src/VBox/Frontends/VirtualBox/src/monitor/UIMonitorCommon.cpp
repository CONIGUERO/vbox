/* $Id$ */
/** @file
 * VBox Qt GUI - UIMonitorCommon class implementation.
 */

/*
 * Copyright (C) 2016-2020 Oracle Corporation
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
#include <QPainter>
#include <QXmlStreamReader>


/* GUI includes: */
#include "UICommon.h"
#include "UIMonitorCommon.h"

/* COM includes: */
#include "CMachineDebugger.h"
#include "CPerformanceCollector.h"

/* static */
void UIMonitorCommon::getNetworkLoad(CMachineDebugger &debugger, quint64 &uOutNetworkReceived, quint64 &uOutNetworkTransmitted)
{
    uOutNetworkReceived = 0;
    uOutNetworkTransmitted = 0;
    QVector<UIDebuggerMetricData> xmlData = getAndParseStatsFromDebugger(debugger, "/Public/NetAdapter/*/Bytes*");
    foreach (const UIDebuggerMetricData &data, xmlData)
    {
        if (data.m_strName.endsWith("BytesReceived"))
            uOutNetworkReceived += data.m_counter;
        else if (data.m_strName.endsWith("BytesTransmitted"))
            uOutNetworkTransmitted += data.m_counter;
        else
            AssertMsgFailed(("name=%s\n", data.m_strName.toLocal8Bit().data()));
    }
}

/* static */
void UIMonitorCommon::getDiskLoad(CMachineDebugger &debugger, quint64 &uOutDiskWritten, quint64 &uOutDiskRead)
{
    uOutDiskWritten = 0;
    uOutDiskRead = 0;
    QVector<UIDebuggerMetricData> xmlData = getAndParseStatsFromDebugger(debugger, "/Public/Storage/*/Port*/Bytes*");
    foreach (const UIDebuggerMetricData &data, xmlData)
    {
        if (data.m_strName.endsWith("BytesWritten"))
            uOutDiskWritten += data.m_counter;
        else if (data.m_strName.endsWith("BytesRead"))
            uOutDiskRead += data.m_counter;
        else
            AssertMsgFailed(("name=%s\n", data.m_strName.toLocal8Bit().data()));
    }
}

/* static */
void UIMonitorCommon::getVMMExitCount(CMachineDebugger &debugger, quint64 &uOutVMMExitCount)
{
    uOutVMMExitCount = 0;
    QVector<UIDebuggerMetricData> xmlData = getAndParseStatsFromDebugger(debugger, "/PROF/CPU*/EM/RecordedExits");
    foreach (const UIDebuggerMetricData &data, xmlData)
    {
        if (data.m_strName.endsWith("RecordedExits"))
            uOutVMMExitCount += data.m_counter;
        else
            AssertMsgFailed(("name=%s\n", data.m_strName.toLocal8Bit().data()));
    }
}


/* static */
QVector<UIDebuggerMetricData> UIMonitorCommon::getAndParseStatsFromDebugger(CMachineDebugger &debugger, const QString &strQuery)
{
    QVector<UIDebuggerMetricData> xmlData;
    if (strQuery.isEmpty())
        return xmlData;
    QString strStats = debugger.GetStats(strQuery, false);
    QXmlStreamReader xmlReader;
    xmlReader.addData(strStats);
    if (xmlReader.readNextStartElement())
    {
        while (xmlReader.readNextStartElement())
        {
            if (xmlReader.name() == "Counter")
            {
                QXmlStreamAttributes attributes = xmlReader.attributes();
                quint64 iCounter = attributes.value("c").toULongLong();
                xmlData.push_back(UIDebuggerMetricData(attributes.value("name"), iCounter));
            }
            else if (xmlReader.name() == "U64")
            {
                QXmlStreamAttributes attributes = xmlReader.attributes();
                quint64 iCounter = attributes.value("val").toULongLong();
                xmlData.push_back(UIDebuggerMetricData(attributes.value("name"), iCounter));
            }
            xmlReader.skipCurrentElement();
        }
    }
    return xmlData;
}

/* static */
void UIMonitorCommon::getRAMLoad(CPerformanceCollector &comPerformanceCollector, QVector<QString> &nameList,
                                 QVector<CUnknown>& objectList, quint64 &iOutTotalRAM, quint64 &iOutFreeRAM)
{
    iOutTotalRAM = 0;
    iOutFreeRAM = 0;
    QVector<QString>  aReturnNames;
    QVector<CUnknown>  aReturnObjects;
    QVector<QString>  aReturnUnits;
    QVector<ULONG>  aReturnScales;
    QVector<ULONG>  aReturnSequenceNumbers;
    QVector<ULONG>  aReturnDataIndices;
    QVector<ULONG>  aReturnDataLengths;
    /* Make a query to CPerformanceCollector to fetch some metrics (e.g RAM usage): */
    QVector<LONG> returnData = comPerformanceCollector.QueryMetricsData(nameList,
                                                                        objectList,
                                                                        aReturnNames,
                                                                        aReturnObjects,
                                                                        aReturnUnits,
                                                                        aReturnScales,
                                                                        aReturnSequenceNumbers,
                                                                        aReturnDataIndices,
                                                                        aReturnDataLengths);
    /* Parse the result we get from CPerformanceCollector to get respective values: */
    for (int i = 0; i < aReturnNames.size(); ++i)
    {
        if (aReturnDataLengths[i] == 0)
            continue;
        /* Read the last of the return data disregarding the rest since we are caching the data in GUI side: */
        float fData = returnData[aReturnDataIndices[i] + aReturnDataLengths[i] - 1] / (float)aReturnScales[i];
        if (aReturnNames[i].contains("RAM", Qt::CaseInsensitive) && !aReturnNames[i].contains(":"))
        {
            if (aReturnNames[i].contains("Total", Qt::CaseInsensitive))
                iOutTotalRAM = (quint64)fData;
            if (aReturnNames[i].contains("Free", Qt::CaseInsensitive))
                iOutFreeRAM = (quint64)fData;
        }
    }
}

/* static */
QPainterPath UIMonitorCommon::doughnutSlice(const QRectF &outerRectangle, const QRectF &innerRectangle, float fStartAngle, float fSweepAngle)
{
    QPainterPath subPath1;
    subPath1.moveTo(outerRectangle.center());
    subPath1.arcTo(outerRectangle, fStartAngle,
                   -1.f * fSweepAngle);
    subPath1.closeSubpath();

    QPainterPath subPath2;
    subPath2.moveTo(innerRectangle.center());
    subPath2.arcTo(innerRectangle, fStartAngle,
                   -1.f * fSweepAngle);
    subPath2.closeSubpath();

    return subPath1.subtracted(subPath2);
}

/* static */
QPainterPath UIMonitorCommon::wholeArc(const QRectF &rectangle)
{
    QPainterPath arc;
    arc.addEllipse(rectangle);
    return arc;
}

/* static */
void UIMonitorCommon::drawCombinedDoughnutChart(quint64 data1, const QColor &data1Color,
                               quint64 data2, const QColor &data2Color,
                               QPainter &painter, quint64  iMaximum,
                               const QRectF &chartRect, const QRectF &innerRect, int iOverlayAlpha)
{
    /* Draw two arcs. one for the inner the other for the outer circle: */
    painter.setPen(QPen(QColor(100, 100, 100, iOverlayAlpha), 1));
    painter.drawArc(chartRect, 0, 3600 * 16);
    painter.drawArc(innerRect, 0, 3600 * 16);

    /* Draw a translucent white background: */
    QPainterPath background = wholeArc(chartRect).subtracted(wholeArc(innerRect));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, iOverlayAlpha));
    painter.drawPath(background);

    /* Draw a doughnut slice for the first data series: */
    float fAngle = 360.f * data1 / (float)iMaximum;
    painter.setBrush(data1Color);
    painter.drawPath(doughnutSlice(chartRect, innerRect, 90, fAngle));

    float fAngle2 = 360.f * data2 / (float)iMaximum;
    painter.setBrush(data2Color);
    painter.drawPath(doughnutSlice(chartRect, innerRect, 90 - fAngle, fAngle2));
}
