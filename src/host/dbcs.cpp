/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "dbcs.h"

#include "misc.h"
#include "Ucs2CharRow.hpp"
#include "../types/inc/convert.hpp"

#include "..\interactivity\inc\ServiceLocator.hpp"

#pragma hdrstop

// Routine Description:
// - This routine setup of line character code.
// Arguments:
// - pScreenInfo - Pointer to screen information structure.
// Return Value:
// - <none>
void SetLineChar(_In_ SCREEN_INFORMATION * const pScreenInfo)
{
    pScreenInfo->LineChar[UPPER_LEFT_CORNER] = 0x250c;
    pScreenInfo->LineChar[UPPER_RIGHT_CORNER] = 0x2510;
    pScreenInfo->LineChar[HORIZONTAL_LINE] = 0x2500;
    pScreenInfo->LineChar[VERTICAL_LINE] = 0x2502;
    pScreenInfo->LineChar[BOTTOM_LEFT_CORNER] = 0x2514;
    pScreenInfo->LineChar[BOTTOM_RIGHT_CORNER] = 0x2518;
}

// Routine Description:
// - This routine check bisected on Ascii string end.
// Arguments:
// - pchBuf - Pointer to Ascii string buffer.
// - cbBuf - Number of Ascii string.
// Return Value:
// - TRUE - Bisected character.
// - FALSE - Correctly.
bool CheckBisectStringA(_In_reads_bytes_(cbBuf) PCHAR pchBuf, _In_ DWORD cbBuf, _In_ const CPINFO * const pCPInfo)
{
    while (cbBuf)
    {
        if (IsDBCSLeadByteConsole(*pchBuf, pCPInfo))
        {
            if (cbBuf <= 1)
            {
                return true;
            }
            else
            {
                pchBuf += 2;
                cbBuf -= 2;
            }
        }
        else
        {
            pchBuf++;
            cbBuf--;
        }
    }

    return false;
}

// Routine Description:
// - updates dbcs attributes of output buffer cells at the beginning and end of where a string will be
// written. This is to clean up leading trailing pairs when one of the cells in the pair is about to be overwritten.
// Arguments:
// - stringLen - the length of the string to write
// - coordTarget - the location the string will be written to
// - pScreenInfo - the screen buffer to update
// Return Value:
// - <none>
void CleanupDbcsEdgesForWrite(_In_ const size_t stringLen,
                              _In_ const COORD coordTarget,
                              _Inout_ SCREEN_INFORMATION* const pScreenInfo)
{
    PTEXT_BUFFER_INFO const pTextInfo = pScreenInfo->TextInfo;
    const COORD coordScreenBufferSize = pScreenInfo->GetScreenBufferSize();
    const SHORT rowIndex = (pTextInfo->GetFirstRowIndex() + coordTarget.Y) % coordScreenBufferSize.Y;

    try
    {
        ROW& row = pTextInfo->GetRowAtIndex(rowIndex);

        ICharRow& iCharRow = row.GetCharRow();
        // we only support ucs2 encoded char rows
        FAIL_FAST_IF_MSG(iCharRow.GetSupportedEncoding() != ICharRow::SupportedEncoding::Ucs2,
                        "only support UCS2 char rows currently");

        Ucs2CharRow& charRow = static_cast<Ucs2CharRow&>(iCharRow);
        // Check start position of strings
        if (charRow.GetAttribute(coordTarget.X).IsTrailing())
        {
            if (coordTarget.X == 0)
            {
                pTextInfo->GetPrevRow(row).ClearColumn(coordScreenBufferSize.X - 1);
            }
            else
            {
                row.ClearColumn(coordTarget.X - 1);
            }
        }

        // Check end position of strings
        if (coordTarget.X + static_cast<short>(stringLen) < coordScreenBufferSize.X)
        {
            size_t column = coordTarget.X + stringLen;
            if (charRow.GetAttribute(column).IsTrailing())
            {
                row.ClearColumn(column);
            }
        }
        else if (coordTarget.Y + 1 < coordScreenBufferSize.Y)
        {
            ROW& rowNext = pTextInfo->GetNextRow(row);
            ICharRow& iCharRowNext = rowNext.GetCharRow();
            // we only support ucs2 encoded char rows
            FAIL_FAST_IF_MSG(iCharRow.GetSupportedEncoding() != ICharRow::SupportedEncoding::Ucs2,
                            "only support UCS2 char rows currently");

            Ucs2CharRow& charRowNext = static_cast<Ucs2CharRow&>(iCharRowNext);
            if (charRowNext.GetAttribute(0).IsTrailing())
            {
                rowNext.ClearColumn(0);
            }
        }
    }
    CATCH_LOG_RETURN();
}

// Routine Description:
// - Determine if the given Unicode char is fullwidth or not. If we fail the 
//      simple lookup, we'll fall back to asking the current renderer whether or
//      not the character is full width.
// Return:
// - FALSE : half width. Uses 1 column per one character
// - TRUE  : full width. Uses 2 columns per one character
BOOL IsCharFullWidth(_In_ WCHAR wch)
{
    bool isFullWidth = false;
    HRESULT hr = IsCharFullWidth(wch, &isFullWidth);
    if (hr == S_OK)
    {
        return isFullWidth;
    }
    else if (hr == S_FALSE)
    {
        if (ServiceLocator::LocateGlobals().pRender != nullptr)
        {
            return ServiceLocator::LocateGlobals().pRender->IsCharFullWidthByFont(wch);
        }
    }
    ASSERT(FALSE);
    return FALSE;
}

// Routine Description:
// - This routine removes the double copies of characters used when storing DBCS/Double-wide characters in the text buffer.
// Arguments:
// - pciDst - Pointer to destination.
// - pciSrc - Pointer to source.
// - cch - Number of characters in buffer.
// Return Value:
// - The length of the destination buffer.
DWORD RemoveDbcsMarkCell(_Out_writes_(cch) PCHAR_INFO pciDst, _In_reads_(cch) const CHAR_INFO * pciSrc, _In_ DWORD cch)
{
    // Walk through the source CHAR_INFO and copy each to the destination.
    // EXCEPT for trailing bytes (this will de-duplicate the leading/trailing byte double copies of the CHAR_INFOs as stored in the buffer).

    // Set up indices used for arrays.
    DWORD iDst = 0;

    // Walk through every CHAR_INFO
    for (DWORD iSrc = 0; iSrc < cch; iSrc++)
    {
        // If it's not a trailing byte, copy it straight over, stripping out the Leading/Trailing flags from the attributes field.
        if (!IsFlagSet(pciSrc[iSrc].Attributes, COMMON_LVB_TRAILING_BYTE))
        {
            pciDst[iDst] = pciSrc[iSrc];
            pciDst[iDst].Attributes &= ~COMMON_LVB_SBCSDBCS;
            iDst++;
        }

        // If it was a trailing byte, we'll just walk past it and keep going.
    }

    // Zero out the remaining part of the destination buffer that we didn't use.
    DWORD const cchDstToClear = cch - iDst;

    if (cchDstToClear > 0)
    {
        CHAR_INFO* const pciDstClearStart = pciDst + iDst;
        ZeroMemory(pciDstClearStart, cchDstToClear * sizeof(CHAR_INFO));
    }

    // Add the additional length we just modified.
    iDst += cchDstToClear;

    // now that we're done, we should have copied, left alone, or cleared the entire length.
    ASSERT(iDst == cch);

    return iDst;
}

// Routine Description:
// - Checks if a char is a lead byte for a given code page.
// Arguments:
// - ch - the char to check.
// - pCPInfo - the code page to check the char in.
// Return Value:
// true if ch is a lead byte, false otherwise.
bool IsDBCSLeadByteConsole(_In_ const CHAR ch, _In_ const CPINFO * const pCPInfo)
{
    ASSERT(pCPInfo != nullptr);
    // NOTE: This must be unsigned for the comparison. If we compare signed, this will never hit
    // because lead bytes are ironically enough always above 0x80 (signed char negative range).
    unsigned char const uchComparison = (unsigned char)ch;

    int i = 0;
    // this is ok because the the array is guaranteed to have 2
    // null bytes at the end.
    while (pCPInfo->LeadByte[i])
    {
        if (pCPInfo->LeadByte[i] <= uchComparison && uchComparison <= pCPInfo->LeadByte[i + 1])
        {
            return true;
        }
        i += 2;
    }
    return false;
}

BYTE CodePageToCharSet(_In_ UINT const uiCodePage)
{
    CHARSETINFO csi;

    if (!ServiceLocator::LocateInputServices()->TranslateCharsetInfo((DWORD *) IntToPtr(uiCodePage), &csi, TCI_SRCCODEPAGE))
    {
        csi.ciCharset = OEM_CHARSET;
    }

    return (BYTE) csi.ciCharset;
}

BOOL IsAvailableEastAsianCodePage(_In_ UINT const uiCodePage)
{
    BYTE const CharSet = CodePageToCharSet(uiCodePage);

    switch (CharSet)
    {
    case SHIFTJIS_CHARSET:
    case HANGEUL_CHARSET:
    case CHINESEBIG5_CHARSET:
    case GB2312_CHARSET:
        return true;
    default:
        return false;
    }
}

_Ret_range_(0, cbAnsi)
ULONG TranslateUnicodeToOem(_In_reads_(cchUnicode) PCWCHAR pwchUnicode,
                            _In_ const ULONG cchUnicode,
                            _Out_writes_bytes_(cbAnsi) PCHAR pchAnsi,
                            _In_ const ULONG cbAnsi,
                            _Out_ std::unique_ptr<IInputEvent>& partialEvent)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    PWCHAR const TmpUni = new WCHAR[cchUnicode];
    if (TmpUni == nullptr)
    {
        return 0;
    }

    memcpy(TmpUni, pwchUnicode, cchUnicode* sizeof(WCHAR));

    BYTE AsciiDbcs[2];
    AsciiDbcs[1] = 0;

    ULONG i, j;
    for (i = 0, j = 0; i < cchUnicode && j < cbAnsi; i++, j++)
    {
        if (IsCharFullWidth(TmpUni[i]))
        {
            ULONG const NumBytes = sizeof(AsciiDbcs);
            ConvertToOem(gci.CP, &TmpUni[i], 1, (LPSTR) & AsciiDbcs[0], NumBytes);
            if (IsDBCSLeadByteConsole(AsciiDbcs[0], &gci.CPInfo))
            {
                if (j < cbAnsi - 1)
                {   // -1 is safe DBCS in buffer
                    pchAnsi[j] = AsciiDbcs[0];
                    j++;
                    pchAnsi[j] = AsciiDbcs[1];
                    AsciiDbcs[1] = 0;
                }
                else
                {
                    pchAnsi[j] = AsciiDbcs[0];
                    break;
                }
            }
            else
            {
                pchAnsi[j] = AsciiDbcs[0];
                AsciiDbcs[1] = 0;
            }
        }
        else
        {
            ConvertToOem(gci.CP, &TmpUni[i], 1, &pchAnsi[j], 1);
        }
    }

    if (AsciiDbcs[1])
    {
        try
        {
            std::unique_ptr<KeyEvent> keyEvent = std::make_unique<KeyEvent>();
            if (keyEvent.get())
            {
                keyEvent->SetCharData(AsciiDbcs[1]);
                partialEvent.reset(static_cast<IInputEvent* const>(keyEvent.release()));
            }
        }
        catch (...)
        {
            LOG_HR(wil::ResultFromCaughtException());
        }
    }

    delete[] TmpUni;
    return j;
}
