/*****************************************************************************/
/* DlgNameScanner.cpp                     Copyright (c) Ladislav Zezula 2008 */
/*---------------------------------------------------------------------------*/
/* Scanning names from a Warcraft 3 map                                      */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 10.04.08  1.00  Lad  The first version of DlgNameBreaker.cpp              */
/*****************************************************************************/

#include "MPQEditor.h"
#include "resource.h"

//-----------------------------------------------------------------------------
// Local structures

#define FILE_SIGNATURE_BLP  0x31504C42
#define FILE_SIGNATURE_MP3  0x03334449
#define FILE_SIGNATURE_MDX  0x584C444D
#define FILE_SIGNATURE_SLK  0x503B4449
#define FILE_SIGNATURE_TEXT 0x00545854

#define MAX_SINGLE_NAME 0x1000              // The longest length of a single name
#define NAME_CACHE_SIZE 0x200               // Size of the name cache. Must be a power of two

#define MAX_TRY_COUNT 20

#define STORM_FLAG_EXIT_PROCESS 0x01

typedef struct _STORM_SCAN_DATA
{
    DWORD cbTotalSize;
    DWORD cbDataSize;
    DWORD dwFlags;
    DWORD dwReserved;

} STORM_SCAN_DATA, *PSTORM_SCAN_DATA;

struct TMdxBlockHeader
{
    DWORD dwBlockType;                      // Block type ('VERS', 'SEQS', 'MODL', 'TEXS', 'ATCH', 'PREM', ...
    DWORD dwBlockSize;                      // Block size, in bytes. Does not include the header
    
    // Followed by the block data (variable length)
};

struct TObjDefinition
{
    DWORD dwOrigObjectID;                   // Original object ID
    DWORD dwNewObjectID;                    // New object ID. (if it is on original table, this is 0, since it isn't used)
    DWORD dwModCount;                       // Number of modifications
};

struct TModDefinition
{
    DWORD dwModId;                          // Modification ID
    DWORD dwVarType;                        // 0 = int, 1 = Real, 2 = Unreal, 3 = String
    DWORD dwLevel;                          // level/variation (this integer is only used by some object files depending on the object type, for example the units file doesn�t use this additional integer, but the ability file does, see the table at the bottom to see which object files use this int*) in the ability and upgrade file this is the level of the ability/upgrade, in the doodads file this is the variation, set to 0 if the object doesn't have more than one level/variation
    DWORD dwDataPointer;                    // data pointer (again this int is only used by those object files that also use the level/variation int, see table*) in reality this is only used in the ability file for values that are originally stored in one of the Data columns in AbilityData.slk, this int tells the game to which of those columns the value resolves (0 = A, 1 = B, 2 = C, 3 = D, 4 = F, 5 = G, 6 = H), for example if the change applies to the column DataA3 the level int will be set to 3 and the data pointer to 0

    // Either 32-bit int, float or ASCIIZ string
};

struct TStringInfo
{
    DWORD cbLength;
    const char * szString;
};

struct TFileNameEntry
{
    LIST_ENTRY Entry;                       // Link to other entries
    char szFileName[1];                     // File name (variable length)
};

struct TNameScannerData
{
    LIST_ENTRY NameCache[NAME_CACHE_SIZE];  // Cache of the file names
    PSTORM_SCAN_DATA pScanData;
    TAnchors * pAnchors;
    UINT_PTR TimerId;
    LPTSTR szMpqName;
    LPCTSTR szListFile;
    HANDLE hProcess;                        // Handle to the process
    HANDLE hEvent;                          // Sync event with the game
    HANDLE hMpq;
    HWND hDlgWorker;
    HWND hDlg;
    HWND hListView;
    DWORD dwFoundNames;
    DWORD dwStopCount;                      // Internal counter for whether the worker was stopped
    UINT nScannerMode;
    bool bFreeListFile;                     // If TRUE, then the list file name needs to be freed
    bool bWorkStopped;
    bool bIsClosing;
};

#define GetNameScannerData(hDlg)  ((TNameScannerData *)(LONG_PTR)GetWindowLongPtr(hDlg, DWLP_USER))

//-----------------------------------------------------------------------------
// Local variables

static TListViewColumns Columns[] = 
{
    {IDS_FILE_NAME,   -1},
    {0, 0}
};

static TStringInfo DirectoryNames[] = 
{
    {0x06, "Fonts\\"},
    {0x23, "ReplaceableTextures\\CommandButtons\\"},
    {0x2B, "ReplaceableTextures\\CommandButtonsDisabled\\"},
    {0x2E, "ReplaceableTextures\\CommandButtonsDisabled\\DIS"},
    {0x31, "ReplaceableTextures\\CommandButtonsDisabled\\DISDIS"},
    {0x23, "ReplaceableTextures\\PassiveButtons\\"},
    {0x10, "war3mapImported\\"},
    {0,  NULL}
};

static TStringInfo Extensions[] =
{
    {4, ".blp"},
    {4, ".tga"},
    {4, ".mdx"},
    {4, ".mdl"},
    {4, ".mp3"},
    {4, ".wav"},
    {4, ".ogg"},
    {0, NULL}
};

// Extensions in subdirectories
static TStringInfo ImgExtensions[] =
{
    {4, ".blp"},
    {4, ".mdx"},
    {4, ".tga"},
    {0, NULL}
};

static TStringInfo KnownTextFiles_L1[] =
{
    {8, "Campaign"},
    {6, "Common"},
    {5, "Human"},
    {4, "Item"},
    {7, "Neutral"},
    {8, "NightElf"},
    {3, "Orc"},
    {6, "Undead"},
    {4, "Unit"},
    {7, "Upgrade"},
    {0, NULL},
};

static TStringInfo KnownTextFiles_L2[] =
{
    {0, ""},
    {7, "Ability"},
    {4, "Unit"},
    {7, "Upgrade"},
    {0, NULL},
};

static TStringInfo KnownTextFiles_L3[] =
{
    {4, "Data"},
    {4, "Func"},
    {7, "Strings"},
    {0, NULL},
};

static TStringInfo KnownSlkFiles_L1[] =
{
    {0x7, "Ability"},
    {0x8, "Ambience"},
    {0x4, "Anim"},
    {0x5, "Cliff"},
    {0xC, "Destructable"},
    {0x6, "Dialog"},
    {0x6, "Doodad"},
    {0xB, "Environment"},
    {0x4, "Item"},
    {0x9, "Lightning"},
    {0x4, "Misc"},
    {0xC, "NotUsed_Unit"},
    {0x8, "Portrait"},
    {0x4, "Skin"},
    {0x5, "Spawn"},
    {0x5, "Splat"},
    {0x9, "UberSplat"},
    {0x2, "UI"},
    {0x4, "Unit"},
    {0xA, "UnitCombat"},
    {0x7, "Upgrade"},
    {0x0, NULL},
};

static TStringInfo KnownSlkFiles_L2[] =
{
    {0x9, "Abilities"},
    {0x5, "Anims"},
    {0x7, "Balance"},
    {0x8, "BuffData"},
    {0xC, "BuffMetaData"},
    {0x4, "Data"},
    {0x7, "Lookups"},
    {0x8, "MetaData"},
    {0x1, "s"},
    {0x6, "Sounds"},
    {0x5, "Types"},
    {0x2, "UI"},
    {0x7, "Weapons"},
    {0x0, NULL},
};

static const char * JassFiles[] = 
{
    "common.j",
    "blizzard.j",
    "war3map.j",
    "scripts\\common.j",
    "scripts\\blizzard.j",
    "scripts\\war3map.j",
    NULL
};

// Note: "war3map.w3?" files are covered by extra loop from 'a' to 'z'
static const char * KnownFiles[] = 
{
    LISTFILE_NAME,
    ATTRIBUTES_NAME,
    SIGNATURE_NAME,
    "war3map.wtg",
    "war3map.wct",
    "war3map.wts",
    "war3map.j",
    "war3map.shd",
    "war3map.mmp",
    "war3map.wpm",
    "war3map.doo",
    "war3map.imp",
    "war3mapMap.blp",
    "war3mapMap.b00",
    "war3mapMap.tga",
    "war3mapMisc.txt",
    "war3mapPath.tga",
    "war3mapUnits.doo",
    "war3mapSkin.txt",
    "war3mapExtra.txt",
    "war3mapPreview.tga",
    NULL
};

static const char * ObjectFiles[] = 
{
    "war3map.w3a",      // Abilities
    "war3map.w3b",      // Destructables
    "war3map.w3d",      // Doodads file
    "war3map.w3h",      // Buffs file
    "war3map.w3q",      // Upgrades file
    "war3map.w3t",      // Items file
    "war3map.w3u",      // Units file
    {NULL}
};

// Table of characters that are allowed to be the begin of file name
// Letters, digits, and characters !#%()-.@_
static unsigned char IsValidFileNameCharacter[256] = 
{
//        00    01    02    03    04    05    06    07    08    09    0A    0B    0C    0D    0E    0F
//       ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----
/* 00 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* 10 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* 20 */ 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 
/* 30 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* 40 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 
/* 50 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 
/* 60 */ 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 
/* 70 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* 80 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* 90 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* A0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* B0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* C0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* D0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* E0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* F0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
};

// Table of characters that are printanble (in text files)
static unsigned char IsTextCharacter[256] = 
{
//        00    01    02    03    04    05    06    07    08    09    0A    0B    0C    0D    0E    0F
//       ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----  ----
/* 00 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 
/* 10 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* 20 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 
/* 30 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 
/* 40 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 
/* 50 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 
/* 60 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 
/* 70 */ 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 
/* 80 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* 90 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* A0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* B0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* C0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* D0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* E0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
/* F0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
};

LPCTSTR szScanEventName   = _T("MPQEditor_LiveScan_Event");
LPCTSTR szScanSectionName = _T("MPQEditor_LiveScan_Data");

//-----------------------------------------------------------------------------
// Worker thread support

#ifdef _DEBUG
static void CheckStringLengths(TStringInfo * pStrings)
{
    size_t nLength;

    while(pStrings->szString != NULL)
    {
        // Check the string length
        nLength = strlen(pStrings->szString);
        assert((DWORD)nLength == (DWORD)pStrings->cbLength);
        pStrings++;
    }
}
#endif // _DEBUG

// Check for "C;X3;K"
static bool CheckForX3Tag(LPBYTE pbLineBegin, LPBYTE pbLineEnd)
{
    return ((pbLineEnd - pbLineBegin) > 6 && memcmp(pbLineBegin, "C;X3;K", 6) == 0);
}

static bool IsTextFile(LPBYTE pbFileData, DWORD cbFileData)
{
    DWORD PrintableCount = 0;
    DWORD i;

    // Limit file data size to 0x1000 characters
    cbFileData = min(cbFileData, 0x1000);

    // Few quick checks for known files
    if(cbFileData > 0x10)
    {
        // Any file having zero (0x00) byte in the first 16 bytes is treated as binary
        if(memchr(pbFileData, 0, 0x10) != NULL)
            return false;

        // UTF-8 files from Warcraft III are treated as text files
        if(pbFileData[0] == 0xEF && pbFileData[1] == 0xBB && pbFileData[2] == 0xBF)
            return true;
    }

    // Count printable characters
    for(i = 0; i < cbFileData; i++)
    {
        char * szFileData = (char *)(pbFileData + i);

        // Check for fragments of test files
        if(!_strnicmp(szFileData, "Art=", 4))
            return true;
        if(!_strnicmp(szFileData, "Name=", 5))
            return true;

        // Count printable characters
        PrintableCount += IsTextCharacter[pbFileData[i]];
    }

    // If more than 2/3 letters, we consider it as text file
    return (PrintableCount > (cbFileData * 2 / 3));
}

static DWORD CalcHashValue(const char * szFileName)
{
    char szNormName[MAX_PATH + 1];
    DWORD dwHash = 0;
    size_t i = 0;

    // Convert the entire string to uppercase
    strcpy(szNormName, szFileName);
    _strupr(szNormName);

    // Add the string
	while(szNormName[i] != 0)
    {
		dwHash = (dwHash >> 24) + (dwHash << 5) + dwHash + szNormName[i++];
    }

    // Also add the value
    return (dwHash & (NAME_CACHE_SIZE - 1));
}

static DWORD GetFileType(LPBYTE pbFileData, DWORD cbFileData)
{
    DWORD dwSignature = 0;

    // Check for known signatures of known files
    if(cbFileData > 4)
        dwSignature = *(PDWORD)pbFileData;

    if(dwSignature == FILE_SIGNATURE_BLP ||
       dwSignature == FILE_SIGNATURE_MP3 ||
       dwSignature == FILE_SIGNATURE_MDX)
    {
        return dwSignature;
    }

    // SLK files
    if(!memcmp(pbFileData, "ID;PWXL;N;E", 11))
        return FILE_SIGNATURE_SLK;

    if(IsTextFile(pbFileData, cbFileData))
        return FILE_SIGNATURE_TEXT;

    return 0;
}

// Replaces double backslash with single backslash
static void NormalizeQuotedString(LPBYTE pbQuotedStr, LPBYTE pbQuotedEnd)
{
    LPBYTE pbSource = pbQuotedStr;
    LPBYTE pbTarget = pbQuotedStr;

    // Do the job
    while(pbSource < pbQuotedEnd)
    {
        // Replace double backslash
        if(pbSource[0] == '\\' && pbSource[1] == '\\')
            pbSource++;
        *pbTarget++ = *pbSource++;
    }

    // Terminate the string
    pbTarget[0] = 0;
}

static bool ExtractQuotedString(
    LPBYTE pbLineBegin,
//  LPBYTE pbLineEnd,
    LPSTR * PtrStringBegin,
    LPSTR * PtrStringEnd)
{
    LPSTR szStringBegin;
    LPSTR szStringEnd;

    // Find the begin of the string
    szStringBegin = strchr((char *)pbLineBegin, '\"');
    if(szStringBegin == NULL)
        return false;

    // Find the end of the string
    szStringEnd = strchr(++szStringBegin, '\"');
    
    PtrStringBegin[0] = szStringBegin;
    PtrStringEnd[0] = szStringEnd;
    return (szStringEnd > szStringBegin);
}

static bool ExtractSingleLine(
    LPBYTE pbFileData,
    LPBYTE pbFileEnd,
    LPBYTE * PtrLineBegin,
    LPBYTE * PtrEqualSign,
    LPBYTE * PtrCommaPtr,
    LPBYTE * PtrLineEnd)
{
    LPBYTE pbLineBegin = NULL;
    LPBYTE pbEqualSign = NULL;
    LPBYTE pbCommaPtr = NULL;

    // Skip all sub-space chars to find the begin of the line
    while(pbFileData < pbFileEnd && pbFileData[0] <= 0x20)
        pbFileData++;
    pbLineBegin = pbFileData;

    // Find the end of the line
    while(pbFileData < pbFileEnd && pbFileData[0] != 0x0A && pbFileData[0] != 0x0D)
    {
        if(pbFileData[0] == '=')
            pbEqualSign = pbFileData;
        if(pbFileData[0] == ',')
            pbCommaPtr = pbFileData;
        pbFileData++;
    }

    // Terminate the line and skip it
    if(pbFileData < pbFileEnd && (pbFileData[0] == 0x0A || pbFileData[0] == 0x0D))
        *pbFileData = 0;

    // Give all pointers to the caller
    PtrLineBegin[0] = pbLineBegin;
    PtrEqualSign[0] = pbEqualSign;
    PtrCommaPtr[0] = pbCommaPtr;
    PtrLineEnd[0] = pbFileData;

    // Return true if the line is there
    return (pbLineBegin < pbFileEnd);
}

static void ConstructFullName1(
    char * szBuffer,
    const char * szDirectoryName,
    size_t cbDirectoryName,
    const char * szPlainName,
    size_t cbPlainName,
    const char * szExtension,
    size_t cbExtension)
{
    // Check whether the full name fits in the MAX_PATH-sized buffer
    if((cbDirectoryName + cbPlainName + cbExtension) <= MAX_PATH)
    {
        // Copy the directory name as-is
        if(szDirectoryName != NULL)
        {
            memcpy(szBuffer, szDirectoryName, cbDirectoryName);
            szBuffer += cbDirectoryName;
        }

        // Copy the plain name
        if(szPlainName != NULL)
        {
            memcpy(szBuffer, szPlainName, cbPlainName);
            szBuffer += cbPlainName;
        }

        // Copy extension
        if(szExtension != NULL)
        {
            memcpy(szBuffer, szExtension, cbExtension);
            szBuffer += cbExtension;
        }
    }

    szBuffer[0] = 0;
}

static void ConstructFullName2(
    char * szBuffer,
    const char * szDirNameBegin,
    const char * szDirNameEnd,
    const char * szPlainName,
    const char * szPlainNameEnd)
{
    // Check the length
    if((szDirNameEnd - szDirNameBegin) + 1 + (szPlainNameEnd - szPlainName) <= MAX_PATH)
    {
        // Copy the directory name
        memcpy(szBuffer, szDirNameBegin, (szDirNameEnd - szDirNameBegin));
        szBuffer += (szDirNameEnd - szDirNameBegin);

        // Put the backslash
        *szBuffer++ = '\\';

        // Copy the plain name
        memcpy(szBuffer, szPlainName, (szPlainNameEnd - szPlainName));
        szBuffer += (szPlainNameEnd - szPlainName);
    }

    // Terminate the buffer
    szBuffer[0] = 0;
}

// Changing thread context
static int SetThreadStartAddress(HANDLE hThread, PVOID StartAddress)
{
    WOW64GETTHREADCONTEXT pfnGetThreadContext;
    WOW64GETTHREADCONTEXT pfnSetThreadContext;
    WOW64_CONTEXT Context;   
    HMODULE hKernel32 = GetModuleHandle(_T("Kernel32.dll"));

    // Either use 32-bit context or raw context
    pfnGetThreadContext = (WOW64GETTHREADCONTEXT)GetProcAddress(hKernel32, "Wow64GetThreadContext");
    pfnSetThreadContext = (WOW64GETTHREADCONTEXT)GetProcAddress(hKernel32, "Wow64SetThreadContext");
    if(pfnGetThreadContext == NULL || pfnSetThreadContext == NULL)
    {
        pfnGetThreadContext = (WOW64GETTHREADCONTEXT)GetThreadContext;
        pfnSetThreadContext = (WOW64GETTHREADCONTEXT)SetThreadContext;
    }

    Context.ContextFlags = CONTEXT_FULL;
    if(!pfnGetThreadContext(hThread, &Context))
        return GetLastError();

    Context.Eax = (DWORD)(DWORD_PTR)StartAddress;
    if(!pfnSetThreadContext(hThread, &Context))
        return GetLastError();

    return ERROR_SUCCESS;
}

static bool CheckForWorkerStopped(TNameScannerData * pData, DWORD dwStepCount)
{
    // Once per 100 tries, check for worker stopped
    if(pData->dwStopCount >= dwStepCount)
    {
        pData->bWorkStopped = WorkerWasCancelled(pData->hDlgWorker);
        pData->dwStopCount = 0;
    }

    // Increment counter and return result
    pData->dwStopCount++;
    return pData->bWorkStopped;
}

// Verifies if the file name is a pseudo-name
static bool IsPseudoFileName(const char * szFileName)
{
    DWORD dwFileIndex = 0;

    if(szFileName != NULL)
    {
        // Must be "File########.ext"
        if(!_strnicmp(szFileName, "File", 4))
        {
            // Check 8 digits
            for(int i = 4; i < 4+8; i++)
            {
                if(szFileName[i] < '0' || szFileName[i] > '9')
                    return false;
                dwFileIndex = (dwFileIndex * 10) + (szFileName[i] - '0');
            }

            // An extension must follow
            return (szFileName[12] == '.');
        }
    }

    // Not a pseudo-name
    return false;
}


static bool InsertFileName(TNameScannerData * pData, const char * szFileName)
{
    TFileNameEntry * pNameEntry;
    PLIST_ENTRY pHeadEntry;
    PLIST_ENTRY pListEntry;
    size_t cbToAllocate;
    size_t cbLength;
    DWORD dwHashValue;

    // Calculate the hash value
    dwHashValue = CalcHashValue(szFileName);
    pHeadEntry = &pData->NameCache[dwHashValue];

    if(!_strnicmp(szFileName, "File0000", 8))
        DebugBreak();
    
    // Check if the name is already there
    if(pHeadEntry->Flink && pHeadEntry->Blink)
    {
        for(pListEntry = pHeadEntry->Flink; pListEntry != pHeadEntry; pListEntry = pListEntry->Flink)
        {
            pNameEntry = CONTAINING_RECORD(pListEntry, TFileNameEntry, Entry);
            if(!_stricmp(pNameEntry->szFileName, szFileName))
                return false;
        }
    }

    // Create new file name entry
    cbLength = strlen(szFileName);
    cbToAllocate = sizeof(TFileNameEntry) + cbLength;
    pNameEntry = (TFileNameEntry *)HeapAlloc(g_hHeap, HEAP_ZERO_MEMORY, cbToAllocate);
    if(pNameEntry == NULL)
        return false;

    // Initialize list head, if not initialized yet
    if(pHeadEntry->Flink == NULL || pHeadEntry->Blink == NULL)
        InitializeListHead(pHeadEntry);

    // Insert the entry to the list
    memcpy(pNameEntry->szFileName, szFileName, cbLength);
    InsertTailList(pHeadEntry, &pNameEntry->Entry);
    pData->dwFoundNames++;
    return true;
}

static bool CheckFileName(TNameScannerData * pData, const char * szFileName)
{
    HANDLE hFile = NULL;
    bool bResult = false;

    // Only for valid file names
    if(szFileName != NULL && szFileName[0] != 0 && IsPseudoFileName(szFileName) == false)
    {
        // Try to open the file
        if(SFileOpenFileEx(pData->hMpq, szFileName, 0, &hFile))
        {
            // MPQ protectors add fake files to the MPQ
            if((SFileGetFileSize(hFile, NULL) & 0xF0000000) == 0)
            {
                InsertFileName(pData, szFileName);
                bResult = true;
            }

            // Close the file
            SFileCloseFile(hFile);
        }
    }

    return bResult;
}

static LPBYTE LoadMpqFileToMemory(TNameScannerData * pData, const char * szFileName, PDWORD PtrFileSize)
{
    HANDLE hFile = NULL;
    LPBYTE pbFileData = NULL;
    DWORD dwFileSize = 0;

    // Attempt to open the file
    if(SFileOpenFileEx(pData->hMpq, szFileName, 0, &hFile))
    {
        // Retrieve the file size
        dwFileSize = SFileGetFileSize(hFile, NULL);
        if(dwFileSize != 0 && (dwFileSize & 0xFF000000) == 0)
        {
            // Allocate buffer for the file data
            pbFileData = (LPBYTE)HeapAlloc(g_hHeap, 0, dwFileSize + 1);
            if(pbFileData != NULL)
            {
                DWORD dwBytesRead = 0;

                // Load the entire file to memory
                SFileReadFile(hFile, pbFileData, dwFileSize, &dwBytesRead, NULL);
                pbFileData[dwFileSize] = 0;

                // If failed, free the buffer
                if(dwBytesRead == dwFileSize)
                {
                    if(!IsPseudoFileName(szFileName))
                    {
                        InsertFileName(pData, szFileName);
                    }
                }
                else
                {
                    HeapFree(g_hHeap, 0, pbFileData);
                    pbFileData = NULL;
                    dwFileSize = 0;
                }
            }
        }

        // Close the file
        SFileCloseFile(hFile);
    }

    // Return what we got
    PtrFileSize[0] = dwFileSize;
    return pbFileData;
}

static void CheckMdxVariants(TNameScannerData * pData, const char * szFullName)
{
    const char * szSuffix = "_portrait.mdx";
    char * szExtension;
    char szMdxVariant[MAX_PATH+1];
    size_t nLength;

    // Check the file extension
    szExtension = GetFileExtension(szFullName);
    if(!_stricmp(szExtension, ".mdx"))
    {
        // If this is the "_portrait.mdx" variant, try the one without
        nLength = strlen(szFullName);
        if(nLength > 13 && !_stricmp(szFullName + nLength - 13, szSuffix))
        {
            strcpy(szMdxVariant, szFullName);
            strcpy(szMdxVariant + nLength - 13, ".mdx");
            CheckFileName(pData, szMdxVariant);
        }
        else
        {
            strcpy(szMdxVariant, szFullName);
            strcpy(szMdxVariant + nLength - 4, szSuffix);
            CheckFileName(pData, szMdxVariant);
        }
    }
}

static void CheckNameVariants(TNameScannerData * pData, const char * szFullName)
{
    char * szPlainName = GetPlainName(szFullName);
    size_t cbFullName = 0;
    size_t cbPlainName = 0;
    char szBuffer[MAX_PATH+1];
    int i, j;

    // Check the full file name as-is
    CheckFileName(pData, szFullName);
    cbPlainName = strlen(szPlainName);
    cbFullName = strlen(szFullName);

__RetrySearch:

    // Attempt to search plain name with all known extensions
    if(szPlainName > szFullName)
    {
        for(i = 0; Extensions[i].szString != NULL; i++)
        {
            ConstructFullName1(szBuffer, NULL, 0, szPlainName, cbPlainName, Extensions[i].szString, Extensions[i].cbLength);
            CheckFileName(pData, szBuffer);
        }
    }

    // Attempt to search full name with all known extensions
    if(szFullName != szPlainName)
    {
        for(i = 0; Extensions[i].szString != NULL; i++)
        {
            ConstructFullName1(szBuffer, NULL, 0, szFullName, cbFullName, Extensions[i].szString, Extensions[i].cbLength);
            CheckFileName(pData, szBuffer);
        }
    }

    // Attempt to open the file with any subdirectory, and either ".blp" or ".tga"
    for(i = 0; DirectoryNames[i].szString != NULL; i++)
    {
        // Circulate all extensions
        for(j = 0; ImgExtensions[j].szString != NULL; j++)
        {
            ConstructFullName1(szBuffer, DirectoryNames[i].szString, 
                                         DirectoryNames[i].cbLength,
                                         szPlainName,
                                         cbPlainName,
                                         ImgExtensions[j].szString,
                                         ImgExtensions[j].cbLength);
            CheckFileName(pData, szBuffer);
        }
    }

    // If the name contains at least one extension, cut it and repeat
    if(cbPlainName > 0)
    {
        // If the cbPlainName is already pointing at extension, go back by one character
        if(szPlainName[cbPlainName] == '.')
            cbPlainName--;

        // Find the previous extension
        while(cbPlainName > 0)
        {
            // Go one character back
            if(szPlainName[cbPlainName] == '.')
                goto __RetrySearch;
            cbPlainName--;
            cbFullName--;
        }
    }
}

static void CheckNameVariants(
    TNameScannerData * pData,
    const char * szLineBegin,
    char * szLineEnd)
{
    // Save the character from the end of the name
    char chSaveChar = szLineEnd[0];

    // Terminate the string
    szLineEnd[0] = 0;
    CheckNameVariants(pData, szLineBegin);
    szLineEnd[0] = chSaveChar;
}

static void CheckNameVariants_CommaSeparated(
    TNameScannerData * pData,
    char * szLineBegin)
{
    char * szLineEnd = szLineBegin;

    // Parse the line until we find an end
    while(szLineEnd[0] != 0)
    {
        // Find the end of the token
        while(szLineEnd[0] != ',' && szLineEnd[0] != 0)
            szLineEnd++;

        // Is there a token with nonzero length > 2?
        // (note: all 2-char names were or will be checked separately)
        if((szLineEnd - szLineBegin) > 2)
            CheckNameVariants(pData, szLineBegin, szLineEnd);

        // Skip the comma
        while(szLineEnd[0] == ',')
            szLineEnd++;
        szLineBegin = szLineEnd;
    }
}

static void CheckNameVariantsForLine(
    TNameScannerData * pData,
    char * szFullLine,
    char * szEqualSign,
    char * szCommaPtr)
{
    char * szQuotedStr;
    char * szQuotedEnd;

    // Try the line as-is
    CheckNameVariants(pData, szFullLine);

    // If there is equal sign, we retry search after the equal sign
    if(szEqualSign != NULL)
    {
        // Move the begin of the line past the equal sign
        szFullLine = SkipSpaces(szEqualSign + 1);

        // Check the line as-is
        CheckNameVariants(pData, szFullLine);

        // If there is also comma, we will check all comma-separated parts
        if(szCommaPtr > szFullLine)
            CheckNameVariants_CommaSeparated(pData, szFullLine);
    }

    // Search all quoted strings
    while((szFullLine = strchr(szFullLine, '\"')) != NULL)
    {
        // Get the begin and end of the quoted part
        szQuotedStr = szFullLine + 1;
        szQuotedEnd = strchr(szQuotedStr, '\"');

        // If the quoted part was properly found, verify it
        if(szQuotedEnd == NULL)
            break;

        // Verify the quoted string
        NormalizeQuotedString((LPBYTE)szQuotedStr, (LPBYTE)szQuotedEnd);
        CheckNameVariants(pData, szQuotedStr);

        // Move past the end of the quoted string
        szFullLine = szQuotedEnd + 1;
    }
}

static void Worker_ScanListFile(TNameScannerData * pData, LPCTSTR szListFile)
{
    SFILE_FIND_DATA sf;
    HANDLE hFind;
    DWORD dwFileCount = 0;

    // Initiate listfile search
    hFind = SListFileFindFirstFileT(NULL, szListFile, _T("*"), &sf);
    if(hFind != NULL)
    {
        // As long as the work was not stopped
        while(CheckForWorkerStopped(pData, 500) == false)
        {
            // Check all variants of the name in the MPQ
            CheckNameVariants(pData, sf.cFileName);

            // Find the next file name
            if(!SListFileFindNextFile(hFind, &sf))
                break;
            dwFileCount++;
        }

        // Close the 
        SListFileFindClose(hFind);
    }
}

static void Worker_ScanKnownTextFiles(TNameScannerData * pData)
{
    char szNameBuff[MAX_PATH];
    char * szNamePtr1 = szNameBuff + 6;

    // Prepare the begin of the name buffer
    memcpy(szNameBuff, "Units\\", 6);

    // All combinations for name part level 1
    for(int i1 = 0; KnownTextFiles_L1[i1].szString != NULL; i1++)
    {
        // Get permanent pointer where the level-2 part will be copied
        char * szNamePtr2 = szNamePtr1;

        // Copy the level-1 part of the name
        memcpy(szNamePtr2, KnownTextFiles_L1[i1].szString, KnownTextFiles_L1[i1].cbLength);
        szNamePtr2 += KnownTextFiles_L1[i1].cbLength;

        // All combinations for name part level 2
        for(int i2 = 0; KnownTextFiles_L2[i2].szString != NULL; i2++)
        {
            // Get permanent pointer where the level-3 part will be copied
            char * szNamePtr3 = szNamePtr2;

            // Copy the level-2 part of the name
            memcpy(szNamePtr3, KnownTextFiles_L2[i2].szString, KnownTextFiles_L2[i2].cbLength);
            szNamePtr3 += KnownTextFiles_L2[i2].cbLength;

            // All combinations for name part level 2
            for(int i3 = 0; KnownTextFiles_L3[i3].szString != NULL; i3++)
            {
                char * szNamePtr = szNamePtr3;

                // Copy the level-3 part of the name
                memcpy(szNamePtr, KnownTextFiles_L3[i3].szString, KnownTextFiles_L3[i3].cbLength);
                szNamePtr += KnownTextFiles_L3[i3].cbLength;

                // Append extension
                *szNamePtr++ = '.';
                *szNamePtr++ = 't';
                *szNamePtr++ = 'x';
                *szNamePtr++ = 't';
                *szNamePtr++ = 0;

                // Check than name
                CheckFileName(pData, szNameBuff);
            }
        }
    }
}

static void Worker_ScanKnownSlkFiles(TNameScannerData * pData, const char * szPrefix, size_t cchPrefix)
{
    char szNameBuff[MAX_PATH];
    char * szNamePtr1 = szNameBuff + cchPrefix;

    // Prepare the begin of the name buffer
    assert(cchPrefix < 0x20);
    memcpy(szNameBuff, szPrefix, cchPrefix);

    // All combinations for name part level 1
    for(int i1 = 0; KnownSlkFiles_L1[i1].szString != NULL; i1++)
    {
        // Get permanent pointer where the level-2 part will be copied
        char * szNamePtr2 = szNamePtr1;

        // Copy the level-1 part of the name
        memcpy(szNamePtr2, KnownSlkFiles_L1[i1].szString, KnownSlkFiles_L1[i1].cbLength);
        szNamePtr2 += KnownSlkFiles_L1[i1].cbLength;

        // All combinations for name part level 2
        for(int i2 = 0; KnownSlkFiles_L2[i2].szString != NULL; i2++)
        {
            // Get permanent pointer where the level-3 part will be copied
            char * szNamePtr = szNamePtr2;

            // Copy the level-2 part of the name
            memcpy(szNamePtr, KnownSlkFiles_L2[i2].szString, KnownSlkFiles_L2[i2].cbLength);
            szNamePtr += KnownSlkFiles_L2[i2].cbLength;

            // Append extension
            *szNamePtr++ = '.';
            *szNamePtr++ = 's';
            *szNamePtr++ = 'l';
            *szNamePtr++ = 'k';
            *szNamePtr++ = 0;

            // Check than name
            CheckFileName(pData, szNameBuff);
        }
    }
}

static void Worker_ScanKnownSlkFiles(TNameScannerData * pData)
{
    Worker_ScanKnownSlkFiles(pData, "Doodads\\", 8);
    Worker_ScanKnownSlkFiles(pData, "Splats\\", 7);
    Worker_ScanKnownSlkFiles(pData, "Units\\", 6);
}

static void Worker_ScanTwoCharFileNames(TNameScannerData * pData)
{
    char szPlainName[4];

    // Parse all two character names
    for(USHORT NameCounter = 0x2100; NameCounter < 0x5A5A; NameCounter++)
    {
        BYTE Character1 = (BYTE)(NameCounter >> 0x08);
        BYTE Character2 = (BYTE)(NameCounter & 0x0FF);

        // Check for work stopped
        if(CheckForWorkerStopped(pData, 1000))
            break;

        // Can the first character be a part of file name?
        if(IsValidFileNameCharacter[Character1] && IsValidFileNameCharacter[Character2])
        {
            // Construct the file name
            szPlainName[0] = (char)Character1;
            szPlainName[1] = (char)Character2;
            szPlainName[2] = 0;

            CheckNameVariants(pData, szPlainName);
        }
    }
}

static void Worker_ScanQuotedStrings(TNameScannerData * pData, LPBYTE pbFileData, DWORD cbFileData)
{
    LPBYTE pbFileEnd = pbFileData + cbFileData;
    LPBYTE pbQuotedStr;
    LPBYTE pbQuotedEnd;

    // As long as we don't hit the end of the file
    while(pbFileData < pbFileEnd)
    {
        // Phase 1: Find a quoted string
        while(pbFileData < pbFileEnd && pbFileData[0] != '\"')
            pbFileData++;
        pbQuotedStr = ++pbFileData;

        // Find the end of the quoted string
        while(pbFileData < pbFileEnd && pbFileData[0] != '\"')
            pbFileData++;
        pbQuotedEnd = pbFileData;

        // Skip the quoted string
        if(pbFileData < pbFileEnd)
            pbFileData++;

        // Did we actually find a quoted string?
        if(pbQuotedEnd > pbQuotedStr)
        {
            NormalizeQuotedString(pbQuotedStr, pbQuotedEnd);
            CheckNameVariants(pData, (char *)pbQuotedStr);
        }
    }
}

static LPBYTE Worker_ScanObjectTable(TNameScannerData * pData, LPBYTE pbFileData, LPBYTE pbFileEnd)
{
    TObjDefinition * pObjDef;
    TModDefinition * pModDef;
    DWORD dwObjectCount = 0;

    // Retrieve the number of objects in the table
    if(pbFileData + sizeof(DWORD) < pbFileEnd)
        dwObjectCount = *(PDWORD)pbFileData;
    pbFileData += sizeof(DWORD);

    // Parse all objects
    for(DWORD i = 0; i < dwObjectCount; i++)
    {
        // Retrieve the object definition structure
        if(pbFileData + sizeof(TObjDefinition) > pbFileEnd)
            return pbFileEnd;
        pObjDef = (TObjDefinition *)pbFileData;

        // Parse all object modifications
        pbFileData += sizeof(TObjDefinition);
        for(DWORD j = 0; j < pObjDef->dwModCount; j++)
        {
            if(pbFileData + sizeof(TModDefinition) > pbFileEnd)
                return pbFileEnd;
            pModDef = (TModDefinition *)pbFileData;

            // Skip the modification structure header
            pbFileData += sizeof(TModDefinition);

            // Perform action according the variable type
            switch(pModDef->dwVarType)
            {
                case 0:     // 32-bit int
                    pbFileData += sizeof(DWORD);
                    break;

                case 1:     // 32-bit float 
                case 2:     // Unreal (float, 0 or 1)
                    pbFileData += 4;
                    break;

                case 3:     // ASCIIZ string
                    CheckNameVariants(pData, (char *)pbFileData);
                    pbFileData += strlen((char *)pbFileData) + 1;
                    break;

                default:    // Data corruption?
                    return pbFileEnd;
            }

            // Skip the terminator (DWORD)
            pbFileData += sizeof(DWORD);
        }
    }

    return pbFileData;
}

static void Worker_ScanObjectFile(TNameScannerData * pData, LPBYTE pbFileData, LPBYTE pbFileEnd)
{
    DWORD dwVersion = 0;

    // Check version
    if(pbFileData + sizeof(DWORD) < pbFileEnd)
        dwVersion = *(PDWORD)pbFileData;
    pbFileData += sizeof(DWORD);

    // Only for known version
    if(dwVersion == 1 || dwVersion == 2)
    {
        // Parse the original object table
        if(pbFileData < pbFileEnd)
            pbFileData = Worker_ScanObjectTable(pData, pbFileData, pbFileEnd);

        // Parse the custom object table
        if(pbFileData < pbFileEnd)
            pbFileData = Worker_ScanObjectTable(pData, pbFileData, pbFileEnd);
    }
}

static void Worker_ScanJassFiles(TNameScannerData * pData)
{
    // For all JASS files: Load them to memory and try quoted strings
    for(int i = 0; JassFiles[i] != NULL; i++)
    {
        LPBYTE pbFileData;
        DWORD cbFileData = 0;

        // Try to load the file to memory
        pbFileData = LoadMpqFileToMemory(pData, JassFiles[i], &cbFileData);
        if(pbFileData != NULL)
        {
            // Search the text file for quoted strings
            Worker_ScanQuotedStrings(pData, pbFileData, cbFileData);
            HeapFree(g_hHeap, 0, pbFileData);
        }
    }
}

static void Worker_ScanObjectFiles(TNameScannerData * pData)
{
    // For all object files: check all string values
    for(int i = 0; ObjectFiles[i] != NULL; i++)
    {
        LPBYTE pbFileData;
        DWORD cbFileData = 0;

        // Try to load the file to memory
        pbFileData = LoadMpqFileToMemory(pData, ObjectFiles[i], &cbFileData);
        if(pbFileData != NULL)
        {
            // Search the text file for quoted strings
            Worker_ScanObjectFile(pData, pbFileData, pbFileData + cbFileData);
            HeapFree(g_hHeap, 0, pbFileData);
        }
    }
}

static void Worker_ScanW3IFile(TNameScannerData * pData)
{
    const char * szFileName = "war3map.w3i";
    LPBYTE pbFileData;
    DWORD cbFileData = 0;
    DWORD dwFileFormat = 0;

    pbFileData = LoadMpqFileToMemory(pData, szFileName, &cbFileData);
    if(pbFileData != NULL)
    {
        LPBYTE pbFilePtr = pbFileData;
        LPBYTE pbFileEnd = pbFileData + cbFileData;

        // Get the file format
        if((pbFilePtr + sizeof(DWORD)) <= pbFileEnd)
            dwFileFormat = *(PDWORD)pbFilePtr;
        pbFilePtr += sizeof(DWORD);

        // Only format 25 has the loading screen
        if(dwFileFormat == 18 || dwFileFormat == 25)
        {
            // Skip next two DWORDS
            pbFilePtr += sizeof(DWORD) + sizeof(DWORD);

            // Skip four strings (map name, map author, map description, players recommended)
            for(int i = 0; i < 4 && pbFilePtr < pbFileEnd; i++)
                pbFilePtr += strlen((char *)pbFilePtr) + 1;

            // Skip camera bounds and camera bounds complements
            pbFilePtr += 32 + 16;

            // Skip map width, map height, flags, map main ground type, loading screen number/game data set
            pbFilePtr += sizeof(DWORD) + sizeof(DWORD) + sizeof(DWORD) + sizeof(BYTE) + sizeof(DWORD);

            // Check for the game loading map
            if(dwFileFormat == 25 && pbFilePtr < pbFileEnd)
                CheckFileName(pData, (char *)pbFilePtr);
        }

        // Free the file data
        HeapFree(g_hHeap, 0, pbFileData);
    }
}

static void Worker_ScanTextFile(TNameScannerData * pData, LPBYTE pbFileData, DWORD cbFileData)
{
    LPBYTE pbFileEnd = pbFileData + cbFileData;
    LPBYTE pbLineBegin = NULL;
    LPBYTE pbEqualSign = NULL;
    LPBYTE pbCommaPtr = NULL;
    LPBYTE pbLineEnd = NULL;

    // Work until we find the end of the file
    while(ExtractSingleLine(pbFileData, pbFileEnd, &pbLineBegin, &pbEqualSign, &pbCommaPtr, &pbLineEnd))
    {
        // If the line hash nonzero size, scan it
        if(pbLineEnd > pbLineBegin)
        {
            CheckNameVariantsForLine(pData, (char *)pbLineBegin, (char *)pbEqualSign, (char *)pbCommaPtr);
        }

        // Move to the next line and repeat
        pbFileData = pbLineEnd;
    }
}

// TEXS file is an array, not just a single name. Thanks Actboy168
// http://www.wc3c.net/tools/specs/NubMdxFormat.txt
static void Worker_ScanMdxFile_TEXS(TNameScannerData * pData, TMdxBlockHeader * pBlockHdr, LPBYTE pbFileEnd)
{
    LPBYTE pbBlockBegin = (LPBYTE)(pBlockHdr + 1);
    LPBYTE pbBlockEnd = pbBlockBegin + pBlockHdr->dwBlockSize;

    // Get the end of the block data
    pbBlockEnd = min(pbBlockEnd, pbFileEnd);

    // Parse all textures
    while(pbBlockBegin < pbBlockEnd)
    {
        CheckNameVariants(pData, (char *)(pbBlockBegin + sizeof(DWORD)));
        pbBlockBegin += 0x10C;
    }
}

static void Worker_ScanMdxFile_ATCH(TNameScannerData * pData, TMdxBlockHeader * pBlockHdr, LPBYTE pbFileEnd)
{
    LPBYTE pbBlockEnd = (LPBYTE)(pBlockHdr + 1) + pBlockHdr->dwBlockSize;
    LPBYTE pbBlockPtr = (LPBYTE)(pBlockHdr + 1);
    LPBYTE pbNamePtr;

    // Make sure that the block don't go past the end of the file
    if(pbBlockEnd < pbFileEnd)
    {
        // Grab all sub-blocks
        while(pbBlockPtr < pbBlockEnd)
        {
            DWORD dwSubBlockSize = 0;
            DWORD dwNameOffset = 0;

            // Get the size of sub-block
            if((pbBlockPtr + sizeof(DWORD) + sizeof(DWORD)) <= pbBlockEnd)
            {
                dwSubBlockSize = *(PDWORD)pbBlockPtr;
                dwNameOffset   = *(PDWORD)(pbBlockPtr + 4);
            }

            // Get the offset of the file name
            pbNamePtr = pbBlockPtr + sizeof(DWORD) + dwNameOffset;

            // Check the name
            if(pbNamePtr < pbBlockEnd && pbNamePtr[0] != 0)
                CheckNameVariants(pData, (char *)pbNamePtr);
            pbBlockPtr += dwSubBlockSize;
        }
    }
}

static void Worker_ScanMdxFile(TNameScannerData * pData, LPBYTE pbFileData, DWORD cbFileData)
{
    TMdxBlockHeader * pBlockHdr;
    LPBYTE pbFileEnd = pbFileData + cbFileData;

    // Skip the file signature
    pbFileData += sizeof(DWORD);

    // Parse all blocks
    while(pbFileData < pbFileEnd)
    {
        // Get the block header and pointer to the next block
        pBlockHdr = (TMdxBlockHeader *)pbFileData;
        pbFileData = (LPBYTE)(pBlockHdr + 1) + pBlockHdr->dwBlockSize;

        // If the block is valid, scan its content
        if(pbFileData <= pbFileEnd)
        {
            // Only if the block has nonzero size
            if(pBlockHdr->dwBlockSize != 0)
            {
                // Some blocks contain file names
                switch(pBlockHdr->dwBlockType)
                {
                    case 'LDOM':    // "MODL"
                    case 'SQES':    // "SEQS"
                        CheckNameVariants(pData, (char *)(pBlockHdr + 1));
                        break;

                    case 'SXET':    // "TEXS"
                        Worker_ScanMdxFile_TEXS(pData, pBlockHdr, pbFileEnd);
                        break;

                    case 'HCTA':    // "ATCH"
                        Worker_ScanMdxFile_ATCH(pData, pBlockHdr, pbFileEnd);
                        break;

                    case 'MERP':    // "PREM"
                    case '2ERP':    // "PRE2"
                        CheckNameVariants(pData, (char *)(pBlockHdr + 1) + sizeof(DWORD) + sizeof(DWORD));
                        break;
                }
            }
        }
    }
}

//
// Discover file names from the SLK strings, like this:
// "ReplaceableTextures\Splats\SunRaySplat.blp"
//
// C;X7;K0.5
// C;X6;K100
// C;X4;K"SunRaySplat"                  <=== Plain Name, without extension
// C;X3;K"ReplaceableTextures\Splats"   <=== Directory name
// C;X2;K"SunRay"
// C;X1;K"SRAY"
// C;Y59;K"IPTH"
static void Worker_ScanSlkFile(TNameScannerData * pData, LPBYTE pbFileData, DWORD cbFileData)
{
    LPBYTE pbFileEnd = pbFileData + cbFileData;
    LPBYTE pbLineBegin = NULL;
    LPBYTE pbEqualSign = NULL;
    LPBYTE pbCommaPtr = NULL;
    LPBYTE pbLineEnd = NULL;
    char * szPrevDirectory = NULL;
    char * szPrevDirectEnd = NULL;
    char * szPrevFileName = NULL;
    char * szPrevNameEnd = NULL;
    char * szStringBegin = NULL;
    char * szStringEnd = NULL;
    char szNameBuff[MAX_PATH+1];

    // Work until we find the end of the file
    while(ExtractSingleLine(pbFileData, pbFileEnd, &pbLineBegin, &pbEqualSign, &pbCommaPtr, &pbLineEnd))
    {
        // Attempt to extract quoted string from the line
        if(ExtractQuotedString(pbLineBegin, &szStringBegin, &szStringEnd))
        {
            // Terminate the quoted string
            szStringEnd[0] = 0;

            // Check the variants for the quoted string name as-is
            CheckNameVariantsForLine(pData, szStringBegin, NULL, (char *)pbCommaPtr);

            // If we encountered a directory name, also check with the previous file
            // If we encountered a file name, also check with the previous directory
            if(CheckForX3Tag(pbLineBegin, pbLineEnd))
            {
                // Directory: If we had file name before, check them together
                if(szPrevFileName != NULL)
                {
                    ConstructFullName2(szNameBuff, szStringBegin,
                                                   szStringEnd,
                                                   szPrevFileName,
                                                   szPrevNameEnd);
                    CheckNameVariants(pData, szNameBuff);
                }

                // Remember that we had directory
                szPrevDirectory = szStringBegin;
                szPrevDirectEnd = szStringEnd;
            }
            else
            {
                // File name: If we had file name before, check them together
                if(szPrevDirectory != NULL)
                {
                    ConstructFullName2(szNameBuff, szPrevDirectory,
                                                   szPrevDirectEnd,
                                                   szStringBegin,
                                                   szStringEnd);
                    CheckNameVariants(pData, szNameBuff);
                }

                // Remember that we had file name
                szPrevFileName = szStringBegin;
                szPrevNameEnd = szStringEnd;
            }
        }

        // Move to the next line and repeat
        pbFileData = pbLineEnd;
    }
}

static void Worker_ScanFileData(TNameScannerData * pData)
{
    LPBYTE pbFileData;
    DWORD dwFileCount = 0;
    DWORD dwFileSize = 0;
    DWORD cbLength = 0;
    DWORD dwFileSignature;
    TCHAR szProgressText[MAX_PATH];
    char szFileName[0x40];

    // Retrieve the number of files in the MPQ
    SFileGetFileInfo(pData->hMpq, SFileMpqBlockTableSize, &dwFileCount, sizeof(DWORD), &cbLength);
    SetWorkerProgressRange(pData->hDlgWorker, NULL, dwFileCount);

    // Scan all files that are in the block table
    for(DWORD dwFileIndex = 0; dwFileIndex < dwFileCount; dwFileIndex++)
    {
        // Check for work stopped
        if(CheckForWorkerStopped(pData, 1))
            break;

        // Inform the user about what we are doing
        _stprintf(szProgressText, _T("Scanning File %u ...."), dwFileIndex);
        SetWorkerProgress(pData->hDlgWorker, NULL, dwFileIndex);

        // Attempt to open the file by index
        sprintf(szFileName, "File%08u.xxx", dwFileIndex);
        pbFileData = LoadMpqFileToMemory(pData, szFileName, &dwFileSize);
        if(pbFileData != NULL)
        {
            // Only for files with nonzero size
            if(dwFileSize != 0)
            {
                dwFileSignature = GetFileType(pbFileData, dwFileSize);
                switch(dwFileSignature)
                {
                    case FILE_SIGNATURE_MDX:
                        SetWorkerProgressText(pData->hDlgWorker, szProgressText);
                        Worker_ScanMdxFile(pData, pbFileData, dwFileSize);
                        break;

                    case FILE_SIGNATURE_SLK:
                        SetWorkerProgressText(pData->hDlgWorker, szProgressText);
                        Worker_ScanSlkFile(pData, pbFileData, dwFileSize);
                        break;

                    case FILE_SIGNATURE_TEXT:
                        SetWorkerProgressText(pData->hDlgWorker, szProgressText);
                        Worker_ScanTextFile(pData, pbFileData, dwFileSize);
                        break;
                }
            }

            // Free the buffer
            HeapFree(g_hHeap, 0, pbFileData);
        }
    }
}

static void Worker_ScanKnownFiles(TNameScannerData * pData)
{
    SFILE_FIND_DATA sf;
    HANDLE hFind;
    DWORD dwFileCount = 0;
    char szWar3Map[0x20];

    // Scan names like "war3map.w3x"
    memcpy(szWar3Map, "war3map.w3?", 12);
    for(char ch = 'a'; ch <= 'z'; ch++)
    {
        szWar3Map[10] = ch;
        CheckFileName(pData, szWar3Map);
    }

    // Scan the known files from the fixed list
    for(int i = 0; KnownFiles[i] != NULL; i++)
    {
        CheckFileName(pData, KnownFiles[i]);
    }

    // Initiate archive search
    hFind = SFileFindFirstFileT(pData->hMpq, _T("*"), &sf, NULL);
    if(hFind != NULL)
    {
        // As long as the work was not stopped
        while(CheckForWorkerStopped(pData, 5) == false)
        {
            // Check all variants of the name in the MPQ
            CheckNameVariants(pData, sf.cFileName);

            // For MDX files, there can be the "portrait" variant
            CheckMdxVariants(pData, sf.cFileName);

            // Find the next file name
            if(!SFileFindNextFile(hFind, &sf))
                break;
            dwFileCount++;
        }

        // Close the 
        SFileFindClose(hFind);
    }
}

static LPTSTR Worker_CreateCopyOfMap(TNameScannerData * pData, LPCTSTR szWar3Dir)
{
    LPTSTR szCopyName;
    TCHAR szNewPlainName[0x40];
    DWORD dwTryCount = 1;
    int nError = ERROR_SUCCESS;

    // Create the name of the map copy
    szCopyName = CreateFullPath(szWar3Dir, _T("Maps"), GetPlainName(pData->szMpqName));
    if(szCopyName == NULL)
        nError = ERROR_NOT_ENOUGH_MEMORY;

    // Make sure that the copy name is not equal to the original name
    // (in case the user opened a map directly within Warcraft III\Maps)
    if(nError == ERROR_SUCCESS)
    {
        while(!_tcsicmp(szCopyName, pData->szMpqName) && dwTryCount < MAX_TRY_COUNT)
        {
            // Delete the name of the copy
            delete [] szCopyName;

            // Format new name
            _stprintf(szNewPlainName, _T("Map_LiveScan%02u%s"), dwTryCount, GetFileExtension(pData->szMpqName));
            szCopyName = CreateFullPath(szWar3Dir, _T("Maps"), szNewPlainName);
            if(szCopyName == NULL)
            {
                nError = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }
        }

        // If we exceeded the try count, delete the map and repeat
        if(dwTryCount >= MAX_TRY_COUNT)
            nError = ERROR_FILE_EXISTS;
    }

    // Make sure that the path to maps exists
    if(nError == ERROR_SUCCESS)
    {
        nError = ForcePathExist(szCopyName, FALSE);
    }

    // If all succeeded, make a copy of the map
    if(nError == ERROR_SUCCESS)
    {
        if(!CopyFile(pData->szMpqName, szCopyName, FALSE))
            nError = GetLastError();
    }

    // If something failed, free the map name and return error
    if(nError != ERROR_SUCCESS)
    {
        SetLastError(nError);
        delete [] szCopyName;
        szCopyName = NULL;
    }

    return szCopyName;
}

static int Worker_LaunchTheGame(LPCTSTR szWar3Dir, LPCTSTR szCopyName, PPROCESS_INFORMATION pProcInfo)
{
    STARTUPINFO si = {sizeof(STARTUPINFO)};
    LPTSTR szCommandLine;
    size_t cchLength;
    int nError = ERROR_SUCCESS;

    // Allocate space for the command line
    cchLength = _tcslen(szWar3Dir) + 12 + 10 + _tcslen(szCopyName) + 3;
    szCommandLine = new TCHAR[cchLength];
    if(szCommandLine != NULL)
    {
        // Format the command line
        _stprintf(szCommandLine, _T("\"%s\\war3.exe\" -loadfile \"%s\""), szWar3Dir, szCopyName);
        if(!CreateProcess(NULL, szCommandLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, pProcInfo))
            nError = GetLastError();

        delete [] szCommandLine;
    }
    else
        nError = ERROR_NOT_ENOUGH_MEMORY;

    return nError;
}

static void Worker_ParseScanData(TNameScannerData * pData, PSTORM_SCAN_DATA pScanCopy)
{
    HANDLE hFile;
    char * szFileName = (char *)(pScanCopy + 1);
    char * szFileEnd = szFileName + pScanCopy->cbDataSize;

    while(szFileName < szFileEnd)
    {
        // Verify if the name exists in the MPQ, then add it to the list
        if(SFileOpenFileEx(pData->hMpq, szFileName, 0, &hFile))
        {
            InsertFileName(pData, szFileName);
            SFileCloseFile(hFile);
        }

        // Get the next file name
        while(szFileName < szFileEnd && szFileName[0] != 0)
            szFileName++;
        while(szFileName < szFileEnd && szFileName[0] == 0)
            szFileName++;
    }
}

static int Worker_LiveScanModalLoop(TNameScannerData * pData)
{
    PSTORM_SCAN_DATA pScanData = pData->pScanData;
    PSTORM_SCAN_DATA pScanCopy;
    SIZE_T cbToAllocate;

    // Sanity checks
    assert(pData->pScanData != NULL);
    assert(pData->hProcess != NULL);
    assert(pData->hEvent != NULL);

    // Allocate copy of the scan data
    cbToAllocate = sizeof(STORM_SCAN_DATA) + pScanData->cbTotalSize;
    pScanCopy = (PSTORM_SCAN_DATA)HeapAlloc(g_hHeap, 0, cbToAllocate);
    if(pScanCopy != NULL)
    {
        // Work as long as the game is running
        while(WaitForSingleObject(pData->hProcess, 500) == WAIT_TIMEOUT)
        {
            // If the cancel button was checked, stop it
            if(WorkerWasCancelled(pData->hDlgWorker))
            {
                if(Worker_MessageBoxRc(pData->hDlgWorker, IDS_QUESTION, IDS_WANT_KILL_WARCRAFT3) == IDYES)
                    TerminateProcess(pData->hProcess, 3);
                SetWorkerCancelState(pData->hDlgWorker, false);
            }

            // Lock the scan data
            if(WaitForSingleObject(pData->hEvent, 500) == WAIT_OBJECT_0)
            {
                // Copy the scan data to our structure
                memcpy(pScanCopy, pScanData, cbToAllocate);
                pScanData->cbDataSize = 0;

                // Unlock the scan data
                SetEvent(pData->hEvent);

                // Parse the data copy
                Worker_ParseScanData(pData, pScanCopy);
            }
        }

        // Free the copy of the scan
        HeapFree(g_hHeap, 0, pScanCopy);
    }

    return ERROR_SUCCESS;
}

static int WorkerLiveScan(HWND hDlgWorker, LPVOID pvParam)
{
    PROCESS_INFORMATION pi = {0};
    TNameScannerData * pData = (TNameScannerData *)pvParam;
    PSTORM_SCAN_DATA pScanData = NULL;
    LPVOID pvRemoteBuffer = NULL;
    LPBYTE pbScannerCode = NULL;
    LPTSTR szCopyName = NULL;
    HANDLE hMap = NULL;
    SIZE_T cbWritten = 0;
    TCHAR szWar3Dir[MAX_PATH+1];
    DWORD cbScannerCode = 0;
    int nError = ERROR_SUCCESS;

    // Allow the work to be cancelled
    EnableWorkerCancelButton(hDlgWorker, TRUE);
    pData->hDlgWorker = hDlgWorker;

#ifdef _DEBUG
//  system("pskill.exe war3.exe");
#endif

    // Load the scanner code from resources
    pbScannerCode = LoadResourceData(g_hInst, MAKEINTRESOURCE(IDR_SCANNER_CODE), _T("RT_BINARY"), &cbScannerCode);
    if(pbScannerCode == NULL || cbScannerCode == 0)
        nError = ERROR_CAN_NOT_COMPLETE;

    // Get the installation directory of War3
    if(nError == ERROR_SUCCESS)
    {
        if(!IsWarcraft3Installed(szWar3Dir, MAX_PATH))
            nError = ERROR_CAN_NOT_COMPLETE;
    }

    // Create event that will be used by the hook code
    if(nError == ERROR_SUCCESS)
    {
        pData->hEvent = CreateEvent(NULL, FALSE, TRUE, _T("MPQEditor_LiveScan_Event"));
        if(pData->hEvent == NULL)
            nError = GetLastError();
    }

    // Create file mapping that will be used by the hook code
    if(nError == ERROR_SUCCESS)
    {
        DWORD cbNameBufferSize = (0x10000 - sizeof(STORM_SCAN_DATA));
        DWORD cbToAllocate = sizeof(STORM_SCAN_DATA) + cbNameBufferSize;

        // Prepare objects needed for the remote code
        hMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, cbToAllocate, _T("MPQEditor_LiveScan_Data"));
        if(hMap != NULL)
        {
            pScanData = (PSTORM_SCAN_DATA)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if(pScanData != NULL)
            {
                pScanData->cbTotalSize = cbNameBufferSize;
                pScanData->cbDataSize = 0;
                pData->pScanData = pScanData;
            }
            else
                nError = GetLastError();
        }
        else
            nError = GetLastError();
    }

    // Create copy of the map
    if(nError == ERROR_SUCCESS)
    {
        szCopyName = Worker_CreateCopyOfMap(pData, szWar3Dir);
        if(szCopyName == NULL)
            nError = GetLastError();
    }

    // Execute Warcraft III and force it to load the map
    if(nError == ERROR_SUCCESS)
    {
        SetWorkerProgressText(hDlgWorker, _T("Launching Warcraft III ..."));
        nError = Worker_LaunchTheGame(szWar3Dir, szCopyName, &pi);
    }

    // Allocate remote buffer
    if(nError == ERROR_SUCCESS)
    {
        pvRemoteBuffer = VirtualAllocEx(pi.hProcess, NULL, cbScannerCode, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if(pvRemoteBuffer == NULL)
            nError = GetLastError();
    }

    // Copy the hook code to the buffer
    if(nError == ERROR_SUCCESS)
    {
        WriteProcessMemory(pi.hProcess, pvRemoteBuffer, pbScannerCode, cbScannerCode, &cbWritten);
        if(cbWritten != cbScannerCode)
            nError = ERROR_WRITE_FAULT;
    }

    // Initialize the file mapping and change the thread context
    if(nError == ERROR_SUCCESS)
    {
        nError = SetThreadStartAddress(pi.hThread, pvRemoteBuffer);
        if(nError == ERROR_SUCCESS)
        {
            ResumeThread(pi.hThread);
            pData->hProcess = pi.hProcess;

            SetWorkerProgressText(hDlgWorker, _T("Scanning names ..."));
            Worker_LiveScanModalLoop(pData);
        }
    }

    // Delete the map
    if(szCopyName != NULL)
        DeleteFile(szCopyName);

    // Free the buffers and exit
    if(pi.hThread != NULL)
        CloseHandle(pi.hThread);
    if(pi.hProcess != NULL)
        CloseHandle(pi.hProcess);
    if(szCopyName != NULL)
        delete [] szCopyName;
    if(pScanData != NULL)
        UnmapViewOfFile(pScanData);
    if(hMap != NULL)
        CloseHandle(hMap);
    if(pData->hEvent != NULL)
        CloseHandle(pData->hEvent);

    // Clear the worker dialog handle
    pData->hDlgWorker = NULL;
    return nError;
}

static int WorkerMapScan(HWND hDlgWorker, LPVOID pvParam)
{
    TNameScannerData * pData = (TNameScannerData *)pvParam;

    // Allow the work to be cancelled
    EnableWorkerCancelButton(hDlgWorker, TRUE);
    pData->hDlgWorker = hDlgWorker;

    // Verify the lengths of the strings
#ifdef _DEBUG
    CheckStringLengths(DirectoryNames);
    CheckStringLengths(Extensions);
    CheckStringLengths(KnownTextFiles_L1);
    CheckStringLengths(KnownTextFiles_L2);
    CheckStringLengths(KnownTextFiles_L3);
    CheckStringLengths(KnownSlkFiles_L1);
    CheckStringLengths(KnownSlkFiles_L2);
#endif

    // Search the archive (using the internal listfile)
    if(pData->bWorkStopped == false && pData->szListFile != NULL)
    {
        SetWorkerProgressText(hDlgWorker, _T("Scanning listfile ..."));
        Worker_ScanListFile(pData, pData->szListFile);
    }

    // Search known text files, like "Units\CampaignAbilityFunc.txt"
    if(pData->bWorkStopped == false)
    {
        SetWorkerProgressText(hDlgWorker, _T("Scanning known text files ..."));
        Worker_ScanKnownTextFiles(pData);
    }

    // Search known text files, like "Units\CampaignAbilityFunc.txt"
    if(pData->bWorkStopped == false)
    {
        SetWorkerProgressText(hDlgWorker, _T("Scanning known SLK files ..."));
        Worker_ScanKnownSlkFiles(pData);
    }

    // Search for all names up to 2 characters
    if(pData->bWorkStopped == false)
    {
        SetWorkerProgressText(hDlgWorker, _T("Scanning short file names ..."));
        Worker_ScanTwoCharFileNames(pData);
    }

    // Search all JASS script files
    if(pData->bWorkStopped == false)
    {
        SetWorkerProgressText(hDlgWorker, _T("Scanning JASS scripts ..."));
        Worker_ScanJassFiles(pData);
    }

    // Search all object files
    if(pData->bWorkStopped == false)
    {
        SetWorkerProgressText(hDlgWorker, _T("Scanning object files ..."));
        Worker_ScanObjectFiles(pData);
    }

    // Search the w3i file for loading screen
    if(pData->bWorkStopped == false)
    {
        SetWorkerProgressText(hDlgWorker, _T("Scanning w3i file ..."));
        Worker_ScanW3IFile(pData);
    }

    // Search the strings inside the files
    if(pData->bWorkStopped == false)
    {
        SetWorkerProgressText(hDlgWorker, _T("Scanning files..."));
        Worker_ScanFileData(pData);
    }

    // Search known files and check for their variants
    if(pData->bWorkStopped == false)
    {
        SetWorkerProgressText(hDlgWorker, _T("Scanning known files ..."));
        Worker_ScanKnownFiles(pData);
    }

    // Clear the worker dialog handle
    pData->hDlgWorker = NULL;
    return 0;
}

//-----------------------------------------------------------------------------
// Local functions

static BOOL SaveListFileDialog(HWND hDlg, LPTSTR szListFile)
{
    OPENFILENAME ofn;
    
    InitOpenFileName(&ofn);
    ofn.lpstrFile = szListFile;
    ofn.lpstrTitle = MAKEINTRESOURCE(IDS_SAVE_LISTFILE);
    ofn.lpstrFilter = MAKEINTRESOURCE(IDS_FILTERS_LISTFILES);
    ofn.lpstrDefExt = _T("txt");
    return GetSaveFileNameRc(hDlg, &ofn); 
}

static void FreeFileNameCache(TNameScannerData * pData)
{
    TFileNameEntry * pNameEntry;
    PLIST_ENTRY pHeadEntry;
    PLIST_ENTRY pListEntry;

    // Free all existing entries in the name cache
    for(int i = 0; i < NAME_CACHE_SIZE; i++)
    {
        // Only if the name cache entry is valid
        if(pData->NameCache[i].Flink && pData->NameCache[i].Blink)
        {
            // Delete all name entries
            pHeadEntry = &pData->NameCache[i];
            for(pListEntry = pHeadEntry->Flink; pListEntry != pHeadEntry; )
            {
                // Get the name entry and next entry
                pNameEntry = CONTAINING_RECORD(pListEntry, TFileNameEntry, Entry);
                pListEntry = pListEntry->Flink;

                // Free the name entry
                HeapFree(g_hHeap, 0, pNameEntry);
            }
        }
    }

    // Initialize all name entries to zero
    memset(pData->NameCache, 0, sizeof(pData->NameCache));
    pData->dwFoundNames = 0;
}

//-----------------------------------------------------------------------------
// Message handlers

static int OnInitDialog(HWND hDlg, LPARAM lParam)
{
    TNameScannerData * pData = (TNameScannerData *)lParam;
    TAnchors * pAnchors = new TAnchors;
    HWND hListView = GetDlgItem(hDlg, IDC_FILELIST);

    // Center the dialog to the parent
    SetDialogIcon(hDlg, IDI_MAIN_ICON);
    CENTER_TO_PARENT(hDlg);

    // Initialize the dialog data
    pData->pAnchors = pAnchors;
    pData->hDlg = hDlg;
    pData->hListView = hListView;
    SetWindowLongPtr(hDlg, DWLP_USER, lParam);

    // Initialize anchors
    pAnchors->AddAnchor(hDlg, IDC_FILELIST, akAll);
    pAnchors->AddAnchor(hDlg, IDC_RESULT, akLeft | akRight | akBottom);
    pAnchors->AddAnchor(hDlg, IDC_APPLY_LIST, akRight | akBottom);
    pAnchors->AddAnchor(hDlg, IDC_SAVE_LIST, akRight | akBottom);
    pAnchors->AddAnchor(hDlg, IDC_CLOSE, akRight | akBottom);

    // If we have no MPQ Editor window, hide the APPLY button
    if(GetParent(hDlg) == NULL)
        EnableDlgItems(hDlg, FALSE, IDC_APPLY_LIST, 0);

    // Set the style for list view
    ListView_SetExtendedListViewStyle(hListView, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);
    ListView_CreateColumns(hListView, Columns);
    return TRUE;
}

static BOOL OnSize(HWND hDlg)
{
    TNameScannerData * pData = GetNameScannerData(hDlg);

    // Rearrange the dialog
    if(pData->pAnchors != NULL)
        pData->pAnchors->OnSize();

    // Adjust the width of the last column
    if(pData->hListView != NULL)
        ListView_ResizeColumns(pData->hListView, Columns);
    return TRUE;
}

static BOOL OnGetMinMaxInfo(HWND hDlg, LPARAM lParam)
{
    TNameScannerData * pData = GetNameScannerData(hDlg);

    if(pData != NULL && pData->pAnchors != NULL)
        pData->pAnchors->OnGetMinMaxInfo(lParam);
    return TRUE;
}

static INT_PTR OnStartWork(HWND hDlg)
{
    TNameScannerData * pData = GetNameScannerData(hDlg);
    TFileNameEntry * pNameEntry;
    PLIST_ENTRY pHeadEntry;
    PLIST_ENTRY pListEntry;
    WORKERPROC pfnWorkerProc;
    LVITEMA lvi;
    HWND hWndStatus = GetDlgItem(hDlg, IDC_RESULT);
    int nError;

    // Delete all names from the list view
    ListView_DeleteAllItems(pData->hListView);
    SetWindowTextRc(hWndStatus, IDS_SCANNING);

    // Free all names from the name cache
    FreeFileNameCache(pData);

    // Run the worker dialog
    pfnWorkerProc = (pData->nScannerMode == IDC_LIVE_SCAN) ? WorkerLiveScan : WorkerMapScan;
    nError = (int)WorkerDialog(hDlg, IDS_SCANNING_FILE_NAMES, pfnWorkerProc, pData);

    // If the worker dialog ended successfully, insert the names to the list
    if(nError == ERROR_SUCCESS)
    {
        // Insert all found names to the dialog
        if(pData->dwFoundNames != 0)
        {
            // Disable redrawing
            SendMessage(pData->hListView, WM_SETREDRAW, FALSE, 0);

            // Fill the list view
            for(DWORD i = 0; i < NAME_CACHE_SIZE; i++)
            {
                // Is that hash entry occupied?
                pHeadEntry = &pData->NameCache[i];
                if(pHeadEntry->Flink && pHeadEntry->Blink)
                {
                    for(pListEntry = pHeadEntry->Flink; pListEntry != pHeadEntry; pListEntry = pListEntry->Flink)
                    {
                        pNameEntry = CONTAINING_RECORD(pListEntry, TFileNameEntry, Entry);
                        
                        ZeroMemory(&lvi, sizeof(LVITEMA));
                        lvi.iItem   = 0x7FFFFFFF;
                        lvi.mask    = LVIF_TEXT | LVIF_PARAM;
                        lvi.pszText = pNameEntry->szFileName;
                        lvi.lParam  = (LPARAM)pNameEntry;
                        SendMessage(pData->hListView, LVM_INSERTITEMA, 0, (LPARAM)&lvi);
                    }
                }
            }

            // Enable redrawing
            SendMessage(pData->hListView, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(pData->hListView, NULL, TRUE);
        }
    }
    else
    {
        MessageBoxError(hDlg, IDS_E_NAME_SCAN_FAILED, nError);
    }

    // Show the number of file names found
    SetWindowTextRc(hWndStatus, IDS_FILES_FOUND, pData->dwFoundNames);
    return 0;
}

static void OnSaveList(HWND hDlg)
{
    TNameScannerData * pData = GetNameScannerData(hDlg);
    TFileNameEntry * pNameEntry;
    LVITEM lvi;
    HANDLE hFile;
    DWORD dwBytesToWrite;
    DWORD dwBytesWritten;
    TCHAR szFileName[MAX_PATH+1] = _T("");
    int nItems = ListView_GetItemCount(pData->hListView);
    int nError = ERROR_SUCCESS;

    // Ask the user for a listfile
    if(pData->szListFile != NULL)
        _tcscpy(szFileName, pData->szListFile);
    if(SaveListFileDialog(hDlg, szFileName) != IDOK)
        return;

    // Create the listfile name
    hFile = CreateFile(szFileName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if(hFile == INVALID_HANDLE_VALUE)
    {
        MessageBoxError(hDlg, IDS_E_CREATE_LISTFILE, 0, szFileName);
        return;
    }

    // Prepare the listview item structure
    for(int i = 0; i < nItems; i++)
    {
        // Query the file name from the list view
        lvi.mask     = LVIF_PARAM;
        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.lParam   = 0;
        ListView_GetItem(pData->hListView, &lvi);

        // Save it, if any
        pNameEntry = (TFileNameEntry *)lvi.lParam;
        if(pNameEntry != NULL)
        {
            dwBytesToWrite = (DWORD)strlen(pNameEntry->szFileName);
            WriteFile(hFile, pNameEntry->szFileName, dwBytesToWrite, &dwBytesWritten, NULL);
            if(dwBytesWritten != dwBytesToWrite)
            {
                MessageBoxError(hDlg, IDS_E_DISK_FULL);
                nError = ERROR_DISK_FULL;
                break;
            }

            WriteFile(hFile, "\x0D\x0A", 2, &dwBytesWritten, NULL);
        }
    }

    CloseHandle(hFile);
}

static int OnCommand(HWND hDlg, UINT nNotify, UINT_PTR nIDCtrl)
{
    if(nNotify == BN_CLICKED)
    {
        switch(nIDCtrl)
        {
            case IDC_SAVE_LIST:
                OnSaveList(hDlg);
                return TRUE;

            case IDCANCEL:
            case IDC_APPLY_LIST:
                EndDialog(hDlg, nIDCtrl);
                return TRUE;
        }
    }

    return FALSE;
}

static BOOL OnDestroy(HWND hDlg)
{
    TNameScannerData * pData = GetNameScannerData(hDlg);

    if(pData != NULL)
    {
        // Free the name cache
        FreeFileNameCache(pData);

        // Delete the anchors
        if(pData->pAnchors != NULL)
            delete pData->pAnchors;
        pData->pAnchors = NULL;

        // Free the listfile name, if needed
        if(pData->szListFile && pData->bFreeListFile)
            delete [] pData->szListFile;
        pData->szListFile = NULL;
    }

    return FALSE;
}

static INT_PTR CALLBACK DialogProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch(uMsg)
    {
        case WM_INITDIALOG:
            return OnInitDialog(hDlg, lParam);

        case WM_SHOWWINDOW:
            if(wParam == TRUE)
                PostMessage(hDlg, WM_START_WORK, 0, 0);
            break;

        case WM_START_WORK:
            return OnStartWork(hDlg);

        case WM_SIZE:
            return OnSize(hDlg);

        case WM_GETMINMAXINFO:
            return OnGetMinMaxInfo(hDlg, lParam);

        case WM_COMMAND:
            return OnCommand(hDlg, WMC_NOTIFY(wParam), WMC_CTRLID(wParam));

        case WM_DESTROY:
            return OnDestroy(hDlg);
    }
    return FALSE;
}

static INT_PTR NameScannerDialog(HWND hParent, TNameScannerData * pData, LPCTSTR szListFile, UINT nScannerMode)
{
    DWORD dwMpqFlags = 0;

    // Check whether the MPQ is a warcraft III map
    SFileGetFileInfo(pData->hMpq, SFileMpqFlags, &dwMpqFlags, sizeof(DWORD), NULL);
    if((dwMpqFlags & MPQ_FLAG_WAR3_MAP) == 0)
    {
        if(MessageBoxRc(hParent, IDS_QUESTION, IDS_NOT_WARCRAFT3MAP) != IDYES)
            return IDCANCEL;
    }

    // Supply the listfile name and the scanner mode
    pData->szListFile = szListFile;
    pData->nScannerMode = nScannerMode;
    return DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_NAME_SCANNER), hParent, DialogProc, (LPARAM)pData);
}

INT_PTR NameScannerDialog(HWND hParent, LPCTSTR szMpqName, LPCTSTR szListFile, HANDLE hMpq, UINT nScannerMode)
{
    TNameScannerData * pData;
    INT_PTR nResult = IDCANCEL;
    DWORD cbFileName = 0;

    // Allocate the name scanner data
    pData = (TNameScannerData *)HeapAlloc(g_hHeap, HEAP_ZERO_MEMORY, sizeof(TNameScannerData));
    if(pData != NULL)
    {
        // Case 1: The MPQ was entered by handle
        if(szMpqName == NULL && hMpq != NULL)
        {
            SFileGetFileInfo(hMpq, SFileMpqFileName, NULL, 0, &cbFileName);
            pData->szMpqName = (LPTSTR)HeapAlloc(g_hHeap, 0, cbFileName);
            if(pData->szMpqName != NULL)
            {
                SFileGetFileInfo(hMpq, SFileMpqFileName, pData->szMpqName, cbFileName, &cbFileName);
                pData->hMpq = hMpq;
                nResult = NameScannerDialog(hParent, pData, szListFile, nScannerMode);
                HeapFree(g_hHeap, 0, pData->szMpqName);
            }
        }

        // Case 2: The MPQ was entered by file name
        else if(szMpqName != NULL && hMpq == NULL)
        {
            if(SFileOpenArchive(szMpqName, 0, MPQ_OPEN_READ_ONLY, &pData->hMpq))
            {
                pData->szMpqName = (LPTSTR)szMpqName;
                nResult = NameScannerDialog(hParent, pData, szListFile, nScannerMode);
                SFileCloseArchive(pData->hMpq);
            }
        }
        
        // Invalid parameters
        else
        {
            MessageBoxError(NULL, IDS_E_BAD_NB_CMD_LINE);
        }

        // Free the data
        HeapFree(g_hHeap, 0, pData);
    }

    return nResult;
}
