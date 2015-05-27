/*****************************************************************************/
/* MPQNameScanner.cpp                     Copyright (c) Ladislav Zezula 2015 */
/*---------------------------------------------------------------------------*/
/* Description :                                                             */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 08.11.07  1.00  Lad  The first version of MPQNameScanner.cpp              */
/*****************************************************************************/

#include "MPQEditor.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")

//-----------------------------------------------------------------------------
// Global variables

HINSTANCE g_hInst;
HANDLE g_hHeap;

//-----------------------------------------------------------------------------
// Dummy implementations to keep linker happy

bool IsWarcraft3Installed(LPTSTR szInstallPath, DWORD cchMaxChars)
{
    UNREFERENCED_PARAMETER(szInstallPath);
    UNREFERENCED_PARAMETER(cchMaxChars);
    return false;
}

LPBYTE LoadResourceData(HMODULE hMod, LPCTSTR lpName, LPCTSTR lpType, PDWORD PtrDataSize)
{
    UNREFERENCED_PARAMETER(hMod);
    UNREFERENCED_PARAMETER(lpName);
    UNREFERENCED_PARAMETER(lpType);
    UNREFERENCED_PARAMETER(PtrDataSize);
    return NULL;
}

//-----------------------------------------------------------------------------
// WinMain

int WINAPI _tWinMain(HINSTANCE hInst, HINSTANCE, LPTSTR, int)
{
    LPCTSTR szMpqName = (__argc >= 2) ? __targv[1] : NULL;
    LPCTSTR szLstName = (__argc >= 3) ? __targv[2] : NULL;

    // Initialize global variables
    g_hInst = hInst;
    g_hHeap = GetProcessHeap();
    InitCommonControls();

    // Check for command line parameters
    if(szMpqName == NULL)
    {
        MessageBox(NULL, _T("No Warcraft III Map name entered.\nSyntax: MPQNameScanner W3xMapName [ListFile]"),
                         _T("Error"),
                         MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Run the scanner dialog
    NameScannerDialog(NULL, szMpqName, szLstName, NULL, IDC_MAP_SCAN);
}
