/*
    Title:  savestate.cpp - Save and Load state

    Copyright (c) 2007, 2015, 2017-19, 2021 David C.J. Matthews

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License version 2.1 as published by the Free Software Foundation.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/


#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_WIN32)
#include "winconfig.h"
#else
#error "No configuration file"
#endif

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h> // For MAX_PATH
#endif

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h> // For MAX_PATH
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_TIME_H
#include <time.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x)   assert(x)
#else
#define ASSERT(x)
#endif

#if (defined(_WIN32))
#include <tchar.h>
#define ERRORNUMBER _doserrno
#define NOMEMORY ERROR_NOT_ENOUGH_MEMORY
#else
typedef char TCHAR;
#define _T(x) x
#define _tfopen fopen
#define _tcscpy strcpy
#define _tcsdup strdup
#define _tcslen strlen
#define _fputtc fputc
#define _fputts fputs
#ifndef lstrcmpi
#define lstrcmpi strcasecmp
#endif
#define ERRORNUMBER errno
#define NOMEMORY ENOMEM
#endif


#include "globals.h"
#include "savestate.h"
#include "processes.h"
#include "run_time.h"
#include "polystring.h"
#include "scanaddrs.h"
#include "arb.h"
#include "memmgr.h"
#include "mpoly.h" // For exportTimeStamp
#include "exporter.h" // For CopyScan
#include "machine_dep.h"
#include "osmem.h"
#include "gc.h" // For FullGC.
#include "timing.h"
#include "rtsentry.h"
#include "check_objects.h"
#include "rtsentry.h"

#include "../polyexports.h" // For InitHeaderFromExport
#include "version.h" // For InitHeaderFromExport

#ifdef _MSC_VER
// Don't tell me about ISO C++ changes.
#pragma warning(disable:4996)
#endif

extern "C" {
    POLYEXTERNALSYMBOL POLYUNSIGNED PolySaveState(POLYUNSIGNED threadId, POLYUNSIGNED fileName, POLYUNSIGNED depth);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyLoadState(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyShowHierarchy(POLYUNSIGNED threadId);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyRenameParent(POLYUNSIGNED threadId, POLYUNSIGNED childName, POLYUNSIGNED parentName);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyShowParent(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyStoreModule(POLYUNSIGNED threadId, POLYUNSIGNED name, POLYUNSIGNED contents);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyLoadModule(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyLoadHierarchy(POLYUNSIGNED threadId, POLYUNSIGNED arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyGetModuleDirectory(POLYUNSIGNED threadId);
}

// Helper class to close files on exit.
class AutoClose {
public:
    AutoClose(FILE *f = 0): m_file(f) {}
    ~AutoClose() { if (m_file) ::fclose(m_file); }

    operator FILE*() { return m_file; }
    FILE* operator = (FILE* p)  { return (m_file = p); }

private:
    FILE *m_file;
};

// This is probably generally useful so may be moved into
// a general header file.
template<typename BASE> class AutoFree
{
public:
    AutoFree(BASE p = 0): m_value(p) {}
    ~AutoFree() { free(m_value); }

    // Automatic conversions to the base type.
    operator BASE() { return m_value; }
    BASE operator = (BASE p)  { return (m_value = p); }

private:
    BASE m_value;
};

#ifdef HAVE__FTELLI64
// fseek and ftell are only 32-bits in Windows.
#define off_t   __int64
#define fseek _fseeki64
#define ftell _ftelli64
#endif

/*
 *  Structure definitions for the saved state files.
 */

#define SAVEDSTATESIGNATURE "POLYSAVE"
#define SAVEDSTATEVERSION   2

// File header for a saved state file.  This appears as the first entry
// in the file.
typedef struct _savedStateHeader
{
    // These entries are primarily to check that we have a valid
    // saved state file before we try to interpret anything else.
    char        headerSignature[8];     // Should contain SAVEDSTATESIGNATURE
    unsigned    headerVersion;          // Should contain SAVEDSTATEVERSION
    unsigned    headerLength;           // Number of bytes in the header
    unsigned    segmentDescrLength;     // Number of bytes in a descriptor

    // These entries contain the real data.
    off_t       segmentDescr;           // Position of segment descriptor table
    unsigned    segmentDescrCount;      // Number of segment descriptors in the table
    off_t       stringTable;            // Pointer to the string table (zero if none)
    size_t      stringTableSize;        // Size of string table
    unsigned    parentNameEntry;        // Position of parent name in string table (0 if top)
    time_t      timeStamp;            // The time stamp for this file.
    time_t      parentTimeStamp;      // The time stamp for the parent.
    void       *originalBaseAddr;        // Original base address (32-in-64 only)
} SavedStateHeader;

// Entry for segment table.  This describes the segments on the disc that
// need to be loaded into memory.
typedef struct _savedStateSegmentDescr
{
    off_t       segmentData;            // Position of the segment data
    size_t      segmentSize;            // Size of the segment data
    off_t       relocations;            // Position of the relocation table
    unsigned    relocationCount;        // Number of entries in relocation table
    unsigned    relocationSize;         // Size of a relocation entry
    unsigned    segmentFlags;           // Segment flags (see SSF_ values)
    unsigned    segmentIndex;           // The index of this segment or the segment it overwrites
    void        *originalAddress;       // The base address when the segment was written.
} SavedStateSegmentDescr;

#define SSF_WRITABLE    1               // The segment contains mutable data
#define SSF_OVERWRITE   2               // The segment overwrites the data (mutable) in a parent.
#define SSF_NOOVERWRITE 4               // The segment must not be further overwritten
#define SSF_BYTES       8               // The segment contains only byte data
#define SSF_CODE        16              // The segment contains only code

typedef struct _relocationEntry
{
    // Each entry indicates a location that has to be set to an address.
    // The location to be set is determined by adding "relocAddress" to the base address of
    // this segment (the one to which these relocations apply) and the value to store
    // by adding "targetAddress" to the base address of the segment indicated by "targetSegment".
    POLYUNSIGNED    relocAddress;       // The (byte) offset in this segment that we will set
    POLYUNSIGNED    targetAddress;      // The value to add to the base of the destination segment
    unsigned        targetSegment;      // The base segment.  0 is IO segment. 
    ScanRelocationKind relKind;         // The kind of relocation (processor dependent).
} RelocationEntry;

#define SAVE(x) taskData->saveVec.push(x)

/*
 *  Hierarchy table: contains information about last loaded or saved state.
 */

// Pointer to list of files loaded in last load.
// There's no need for a lock since the update is only made when all
// the ML threads have stopped.
class HierarchyTable
{
public:
    HierarchyTable(const TCHAR *file, time_t time):
      fileName(_tcsdup(file)), timeStamp(time) { }
    AutoFree<TCHAR*> fileName;
    time_t          timeStamp;
};

HierarchyTable **hierarchyTable;

static unsigned hierarchyDepth;

static bool AddHierarchyEntry(const TCHAR *fileName, time_t timeStamp)
{
    // Add an entry to the hierarchy table for this file.
    HierarchyTable *newEntry = new HierarchyTable(fileName, timeStamp);
    if (newEntry == 0) return false;
    HierarchyTable **newTable =
        (HierarchyTable **)realloc(hierarchyTable, sizeof(HierarchyTable *)*(hierarchyDepth+1));
    if (newTable == 0) return false;
    hierarchyTable = newTable;
    hierarchyTable[hierarchyDepth++] = newEntry;
    return true;
}

// Test whether we're overwriting a parent of ourself.
#if (defined(_WIN32) || defined(__CYGWIN__))
static bool sameFile(const TCHAR *x, const TCHAR *y)
{
    HANDLE hXFile = INVALID_HANDLE_VALUE, hYFile = INVALID_HANDLE_VALUE;
    bool result = false;

    hXFile = CreateFile(x, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hXFile == INVALID_HANDLE_VALUE) goto closeAndExit;
    hYFile = CreateFile(y, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hYFile == INVALID_HANDLE_VALUE) goto closeAndExit;
    BY_HANDLE_FILE_INFORMATION fileInfoX, fileInfoY;
    if (! GetFileInformationByHandle(hXFile, &fileInfoX)) goto closeAndExit;
    if (! GetFileInformationByHandle(hYFile, &fileInfoY)) goto closeAndExit;

    result = fileInfoX.dwVolumeSerialNumber == fileInfoY.dwVolumeSerialNumber &&
        fileInfoX.nFileIndexLow == fileInfoY.nFileIndexLow &&
        fileInfoX.nFileIndexHigh == fileInfoY.nFileIndexHigh;

closeAndExit:
    if (hXFile != INVALID_HANDLE_VALUE) CloseHandle(hXFile);
    if (hYFile != INVALID_HANDLE_VALUE) CloseHandle(hYFile);
    return result;
}
#else
static bool sameFile(const char *x, const char *y)
{
    struct stat xStat, yStat;
    // If either file does not exist that's fine.
    if (stat(x, &xStat) != 0 || stat(y, &yStat) != 0)
        return false;
    return (xStat.st_dev == yStat.st_dev && xStat.st_ino == yStat.st_ino);
}
#endif

/*
 *  Saving state.
 */

// This class is used to create the relocations.  It uses Exporter
// for this but this may perhaps be too heavyweight.
class SaveStateExport: public Exporter, public ScanAddress
{
public:
    SaveStateExport(unsigned int h=0): Exporter(h), relocationCount(0) {}
public:
    virtual void exportStore(void) {} // Not used.

private:
    // ScanAddress overrides
    virtual void ScanConstant(PolyObject *base, byte *addrOfConst, ScanRelocationKind code, intptr_t displacement);
    // At the moment we should only get calls to ScanConstant.
    virtual PolyObject *ScanObjectAddress(PolyObject *base) { return base; }

protected:
    void setRelocationAddress(void *p, POLYUNSIGNED *reloc);
    PolyWord createRelocation(PolyWord p, void *relocAddr);
    unsigned relocationCount;

    friend class SaveRequest;
};


// Generate the address relative to the start of the segment.
void SaveStateExport::setRelocationAddress(void *p, POLYUNSIGNED *reloc)
{
    unsigned area = findArea(p);
    POLYUNSIGNED offset = (POLYUNSIGNED)((char*)p - (char*)memTable[area].mtOriginalAddr);
    *reloc = offset;
}


// Create a relocation entry for an address at a given location.
PolyWord SaveStateExport::createRelocation(PolyWord p, void *relocAddr)
{
    RelocationEntry reloc;
    // Set the offset within the section we're scanning.
    setRelocationAddress(relocAddr, &reloc.relocAddress);
    void *addr = p.AsAddress();
    unsigned addrArea = findArea(addr);
    reloc.targetAddress = (POLYUNSIGNED)((char*)addr - (char*)memTable[addrArea].mtOriginalAddr);
    reloc.targetSegment = (unsigned)memTable[addrArea].mtIndex;
    reloc.relKind = PROCESS_RELOC_DIRECT;
    fwrite(&reloc, sizeof(reloc), 1, exportFile);
    relocationCount++;
    return p; // Don't change the contents
}


/* This is called for each constant within the code. 
   Print a relocation entry for the word and return a value that means
   that the offset is saved in original word. */
void SaveStateExport::ScanConstant(PolyObject *base, byte *addr, ScanRelocationKind code, intptr_t displacement)
{
    PolyObject *p = GetConstantValue(addr, code, displacement);

    if (p == 0)
        return;

    void *a = p;
    unsigned aArea = findArea(a);

    // We don't need a relocation if this is relative to the current segment
    // since the relative address will already be right.
    if (code == PROCESS_RELOC_I386RELATIVE && aArea == findArea(addr))
        return;

    // Set the value at the address to the offset relative to the symbol.
    RelocationEntry reloc;
    setRelocationAddress(addr, &reloc.relocAddress);
    reloc.targetAddress = (POLYUNSIGNED)((char*)a - (char*)memTable[aArea].mtOriginalAddr);
    reloc.targetSegment = (unsigned)memTable[aArea].mtIndex;
    reloc.relKind = code;
    fwrite(&reloc, sizeof(reloc), 1, exportFile);
    relocationCount++;
}

// Request to the main thread to save data.
class SaveRequest: public MainThreadRequest
{
public:
    SaveRequest(const TCHAR *name, unsigned h): MainThreadRequest(MTP_SAVESTATE),
        fileName(name), newHierarchy(h),
        errorMessage(0), errCode(0) {}

    virtual void Perform();
    const TCHAR *fileName;
    unsigned newHierarchy;
    const char *errorMessage;
    int errCode;
};

// This class is used to update references to objects that have moved.  If
// we have copied an object into the area to be exported we may still have references
// to it from the stack or from RTS data structures.  We have to ensure that these
// are updated.
// This is very similar to ProcessFixupAddress in sharedata.cpp
class SaveFixupAddress: public ScanAddress
{
protected:
    virtual POLYUNSIGNED ScanAddressAt(PolyWord *pt);
    virtual POLYUNSIGNED ScanCodeAddressAt(PolyObject **pt) { *pt = ScanObjectAddress(*pt); return 0; }
    virtual PolyObject *ScanObjectAddress(PolyObject *base);

public:
    void ScanCodeSpace(CodeSpace *space);
};


POLYUNSIGNED SaveFixupAddress::ScanAddressAt(PolyWord *pt)
{
    PolyWord val = *pt;
    if (val.IsDataPtr() && val != PolyWord::FromUnsigned(0))
        *pt = ScanObjectAddress(val.AsObjPtr());
    return 0;
}

// Returns the new address if the argument is the address of an object that
// has moved, otherwise returns the original.
PolyObject *SaveFixupAddress::ScanObjectAddress(PolyObject *obj)
{
    if (obj->ContainsForwardingPtr()) // tombstone is a pointer to a moved object
    {
#ifdef POLYML32IN64
        MemSpace *space = gMem.SpaceForAddress((PolyWord*)obj - 1);
        PolyObject *newp;
        if (space->isCode)
            newp = (PolyObject*)(globalCodeBase + ((obj->LengthWord() & ~_OBJ_TOMBSTONE_BIT) << 1));
        else newp = obj->GetForwardingPtr();
#else
        PolyObject *newp = obj->GetForwardingPtr();
#endif
        ASSERT (newp->ContainsNormalLengthWord());
        return newp;
    }
    
    ASSERT (obj->ContainsNormalLengthWord()); // object is not moved
    return obj;
}

// Fix up addresses in the code area.  Unlike ScanAddressesInRegion this updates
// cells that have been moved.  We need to do that because we may still have
// return addresses into those cells and we don't move return addresses.  We
// do want the code to see updated constant addresses.
void SaveFixupAddress::ScanCodeSpace(CodeSpace *space)
{
    for (PolyWord *pt = space->bottom; pt < space->top; )
    {
        pt++;
        PolyObject *obj = (PolyObject*)pt;
#ifdef POLYML32IN64
        PolyObject *dest = obj;
        while (dest->ContainsForwardingPtr())
        {
            MemSpace *space = gMem.SpaceForObjectAddress(dest);
            if (space->isCode)
                dest = (PolyObject*)(globalCodeBase + ((dest->LengthWord() & ~_OBJ_TOMBSTONE_BIT) << 1));
            else dest = dest->GetForwardingPtr();
        }
#else
        PolyObject *dest = obj->FollowForwardingChain();
#endif
        POLYUNSIGNED length = dest->Length();
        if (length != 0)
            ScanAddressesInObject(obj, dest->LengthWord());
        pt += length;
    }
}

// Called by the root thread to actually save the state and write the file.
void SaveRequest::Perform()
{
    if (debugOptions & DEBUG_SAVING)
        Log("SAVE: Beginning saving state.\n");
    // Check that we aren't overwriting our own parent.
    for (unsigned q = 0; q < newHierarchy-1; q++) {
        if (sameFile(hierarchyTable[q]->fileName, fileName))
        {
            errorMessage = "File being saved is used as a parent of this file";
            errCode = 0;
            if (debugOptions & DEBUG_SAVING)
                Log("SAVE: File being saved is used as a parent of this file.\n");
            return;
        }
    }

    SaveStateExport exports;
    // Open the file.  This could quite reasonably fail if the path is wrong.
    exports.exportFile = _tfopen(fileName, _T("wb"));
    if (exports.exportFile == NULL)
    {
        errorMessage = "Cannot open save file";
        errCode = ERRORNUMBER;
        if (debugOptions & DEBUG_SAVING)
            Log("SAVE: Cannot open save file.\n");
        return;
    }

    // Scan over the permanent mutable area copying all reachable data that is
    // not in a lower hierarchy into new permanent segments.
    CopyScan copyScan(newHierarchy);
    copyScan.initialise(false);
    bool success = true;
    try {
        for (std::vector<PermanentMemSpace*>::iterator i = gMem.pSpaces.begin(); i < gMem.pSpaces.end(); i++)
        {
            PermanentMemSpace *space = *i;
            if (space->isMutable && !space->noOverwrite && !space->byteOnly)
            {
                if (debugOptions & DEBUG_SAVING)
                    Log("SAVE: Scanning permanent mutable area %p allocated at %p size %lu\n",
                        space, space->bottom, space->spaceSize());
                copyScan.ScanAddressesInRegion(space->bottom, space->top);
            }
        }
    }
    catch (MemoryException &)
    {
        success = false;
        if (debugOptions & DEBUG_SAVING)
            Log("SAVE: Scan of permanent mutable area raised memory exception.\n");
    }

    // Copy the areas into the export object.  Make sufficient space for
    // the largest possible number of entries.
    exports.memTable = new memoryTableEntry[gMem.eSpaces.size()+gMem.pSpaces.size()+1];
    unsigned memTableCount = 0;

    // Permanent spaces at higher level.  These have to have entries although
    // only the mutable entries will be written.
    for (std::vector<PermanentMemSpace*>::iterator i = gMem.pSpaces.begin(); i < gMem.pSpaces.end(); i++)
    {
        PermanentMemSpace *space = *i;
        if (space->hierarchy < newHierarchy)
        {
            memoryTableEntry *entry = &exports.memTable[memTableCount++];
            entry->mtOriginalAddr = entry->mtCurrentAddr = space->bottom;
            entry->mtLength = (space->topPointer-space->bottom)*sizeof(PolyWord);
            entry->mtIndex = space->index;
            entry->mtFlags = 0;
            if (space->isMutable)
            {
                entry->mtFlags |= MTF_WRITEABLE;
                if (space->noOverwrite) entry->mtFlags |= MTF_NO_OVERWRITE;
                if (space->byteOnly) entry->mtFlags |= MTF_BYTES;
            }
            if (space->isCode)
                entry->mtFlags |= MTF_EXECUTABLE;
        }
    }
    unsigned permanentEntries = memTableCount; // Remember where new entries start.

    // Newly created spaces.
    for (std::vector<PermanentMemSpace *>::iterator i = gMem.eSpaces.begin(); i < gMem.eSpaces.end(); i++)
    {
        memoryTableEntry *entry = &exports.memTable[memTableCount++];
        PermanentMemSpace *space = *i;
        entry->mtOriginalAddr = entry->mtCurrentAddr = space->bottom;
        entry->mtLength = (space->topPointer-space->bottom)*sizeof(PolyWord);
        entry->mtIndex = space->index;
        entry->mtFlags = 0;
        if (space->isMutable)
        {
            entry->mtFlags |= MTF_WRITEABLE;
            if (space->noOverwrite) entry->mtFlags |= MTF_NO_OVERWRITE;
            if (space->byteOnly) entry->mtFlags |= MTF_BYTES;
        }
        if (space->isCode)
            entry->mtFlags |= MTF_EXECUTABLE;
    }

    exports.memTableEntries = memTableCount;

    if (debugOptions & DEBUG_SAVING)
        Log("SAVE: Updating references to moved objects.\n");

    // Update references to moved objects.
    SaveFixupAddress fixup;
    for (std::vector<LocalMemSpace*>::iterator i = gMem.lSpaces.begin(); i < gMem.lSpaces.end(); i++)
    {
        LocalMemSpace *space = *i;
        fixup.ScanAddressesInRegion(space->bottom, space->lowerAllocPtr);
        fixup.ScanAddressesInRegion(space->upperAllocPtr, space->top);
    }
    for (std::vector<CodeSpace *>::iterator i = gMem.cSpaces.begin(); i < gMem.cSpaces.end(); i++)
        fixup.ScanCodeSpace(*i);

    GCModules(&fixup);

    // Restore the length words in the code areas.
    // Although we've updated any pointers to the start of the code we could have return addresses
    // pointing to the original code.  GCModules updates the stack but doesn't update return addresses.
    for (std::vector<CodeSpace *>::iterator i = gMem.cSpaces.begin(); i < gMem.cSpaces.end(); i++)
    {
        CodeSpace *space = *i;
        for (PolyWord *pt = space->bottom; pt < space->top; )
        {
            pt++;
            PolyObject *obj = (PolyObject*)pt;
            if (obj->ContainsForwardingPtr())
            {
#ifdef POLYML32IN64
                PolyObject *forwardedTo = obj;
                while (forwardedTo->ContainsForwardingPtr())
                    forwardedTo = (PolyObject*)(globalCodeBase + ((forwardedTo->LengthWord() & ~_OBJ_TOMBSTONE_BIT) << 1));
#else
                PolyObject *forwardedTo = obj->FollowForwardingChain();
#endif
                POLYUNSIGNED lengthWord = forwardedTo->LengthWord();
                space->writeAble(obj)->SetLengthWord(lengthWord);
            }
            pt += obj->Length();
        }
    }

    // Update the global memory space table.  Old segments at the same level
    // or lower are removed.  The new segments become permanent.
    // Try to promote the spaces even if we've had a failure because export
    // spaces are deleted in ~CopyScan and we may have already copied
    // some objects there.
    if (debugOptions & DEBUG_SAVING)
        Log("SAVE: Promoting export spaces to permanent spaces.\n");
    if (! gMem.PromoteExportSpaces(newHierarchy) || ! success)
    {
        errorMessage = "Out of Memory";
        errCode = NOMEMORY;
        if (debugOptions & DEBUG_SAVING)
            Log("SAVE: Unable to promote export spaces.\n");
        return;
    }
    // Remove any deeper entries from the hierarchy table.
    while (hierarchyDepth > newHierarchy-1)
    {
        hierarchyDepth--;
        delete(hierarchyTable[hierarchyDepth]);
        hierarchyTable[hierarchyDepth] = 0;
    }

    if (debugOptions & DEBUG_SAVING)
        Log("SAVE: Writing out data.\n");

    // Write out the file header.
    SavedStateHeader saveHeader;
    memset(&saveHeader, 0, sizeof(saveHeader));
    saveHeader.headerLength = sizeof(saveHeader);
    memcpy(saveHeader.headerSignature,
        SAVEDSTATESIGNATURE, sizeof(saveHeader.headerSignature));
    saveHeader.headerVersion = SAVEDSTATEVERSION;
    saveHeader.segmentDescrLength = sizeof(SavedStateSegmentDescr);
    if (newHierarchy == 1)
        saveHeader.parentTimeStamp = exportTimeStamp;
    else
    {
        saveHeader.parentTimeStamp = hierarchyTable[newHierarchy-2]->timeStamp;
        saveHeader.parentNameEntry = sizeof(TCHAR); // Always the first entry.
    }
    saveHeader.timeStamp = getBuildTime();
    saveHeader.segmentDescrCount = exports.memTableEntries; // One segment for each space.
#ifdef POLYML32IN64
    saveHeader.originalBaseAddr = globalHeapBase;
#endif
    // Write out the header.
    fwrite(&saveHeader, sizeof(saveHeader), 1, exports.exportFile);

    // We need a segment header for each permanent area whether it is
    // actually in this file or not.
    SavedStateSegmentDescr *descrs = new SavedStateSegmentDescr [exports.memTableEntries];

    for (unsigned j = 0; j < exports.memTableEntries; j++)
    {
        memoryTableEntry *entry = &exports.memTable[j];
        memset(&descrs[j], 0, sizeof(SavedStateSegmentDescr));
        descrs[j].relocationSize = sizeof(RelocationEntry);
        descrs[j].segmentIndex = (unsigned)entry->mtIndex;
        descrs[j].segmentSize = entry->mtLength; // Set this even if we don't write it.
        descrs[j].originalAddress = entry->mtOriginalAddr;
        if (entry->mtFlags & MTF_WRITEABLE)
        {
            descrs[j].segmentFlags |= SSF_WRITABLE;
            if (entry->mtFlags & MTF_NO_OVERWRITE)
                descrs[j].segmentFlags |= SSF_NOOVERWRITE;
            if (j < permanentEntries && (entry->mtFlags & MTF_NO_OVERWRITE) == 0)
                descrs[j].segmentFlags |= SSF_OVERWRITE;
            if (entry->mtFlags & MTF_BYTES)
                descrs[j].segmentFlags |= SSF_BYTES;
        }
        if (entry->mtFlags & MTF_EXECUTABLE)
            descrs[j].segmentFlags |= SSF_CODE;
    }
    // Write out temporarily. Will be overwritten at the end.
    saveHeader.segmentDescr = ftell(exports.exportFile);
    fwrite(descrs, sizeof(SavedStateSegmentDescr), exports.memTableEntries, exports.exportFile);

    // Write out the relocations and the data.
    for (unsigned k = 1 /* Not IO area */; k < exports.memTableEntries; k++)
    {
        memoryTableEntry *entry = &exports.memTable[k];
        // Write out the contents if this is new or if it is a normal, overwritable
        // mutable area.
        if (k >= permanentEntries ||
            (entry->mtFlags & (MTF_WRITEABLE|MTF_NO_OVERWRITE)) == MTF_WRITEABLE)
        {
            descrs[k].relocations = ftell(exports.exportFile);
            // Have to write this out.
            exports.relocationCount = 0;
            // Create the relocation table.
            char *start = (char*)entry->mtOriginalAddr;
            char *end = start + entry->mtLength;
            for (PolyWord *p = (PolyWord*)start; p < (PolyWord*)end; )
            {
                p++;
                PolyObject *obj = (PolyObject*)p;
                POLYUNSIGNED length = obj->Length();
                // Most relocations can be computed when the saved state is
                // loaded so we only write out the difficult ones: those that
                // occur within compiled code.
                //  exports.relocateObject(obj);
                if (length != 0 && obj->IsCodeObject())
                    machineDependent->ScanConstantsWithinCode(obj, &exports);
                p += length;
            }
            descrs[k].relocationCount = exports.relocationCount;
            // Write out the data.
            descrs[k].segmentData = ftell(exports.exportFile);
            fwrite(entry->mtOriginalAddr, entry->mtLength, 1, exports.exportFile);
       }
    }

    // If this is a child we need to write a string table containing the parent name.
    if (newHierarchy > 1)
    {
        saveHeader.stringTable = ftell(exports.exportFile);
        _fputtc(0, exports.exportFile); // First byte of string table is zero
        _fputts(hierarchyTable[newHierarchy-2]->fileName, exports.exportFile);
        _fputtc(0, exports.exportFile); // A terminating null.
        saveHeader.stringTableSize = (_tcslen(hierarchyTable[newHierarchy-2]->fileName) + 2)*sizeof(TCHAR);
    }

    // Rewrite the header and the segment tables now they're complete.
    fseek(exports.exportFile, 0, SEEK_SET);
    fwrite(&saveHeader, sizeof(saveHeader), 1, exports.exportFile);
    fwrite(descrs, sizeof(SavedStateSegmentDescr), exports.memTableEntries, exports.exportFile);

    if (debugOptions & DEBUG_SAVING)
        Log("SAVE: Writing complete.\n");

    // Add an entry to the hierarchy table for this file.
    (void)AddHierarchyEntry(fileName, saveHeader.timeStamp);

    delete[](descrs);

    CheckMemory();
}

// Write a saved state file.
POLYUNSIGNED PolySaveState(POLYUNSIGNED threadId, POLYUNSIGNED fileName, POLYUNSIGNED depth)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();

    try {
        TempString fileNameBuff(Poly_string_to_T_alloc(PolyWord::FromUnsigned(fileName)));
        // The value of depth is zero for top-level save so we need to add one for hierarchy.
        unsigned newHierarchy = get_C_unsigned(taskData, PolyWord::FromUnsigned(depth)) + 1;

        if (newHierarchy > hierarchyDepth + 1)
            raise_fail(taskData, "Depth must be no more than the current hierarchy plus one");

        // Request a full GC first.  The main reason is to avoid running out of memory as a
        // result of repeated saves.  Old export spaces are turned into local spaces and
        // the GC will delete them if they are completely empty
        FullGC(taskData);

        SaveRequest request(fileNameBuff, newHierarchy);
        processes->MakeRootRequest(taskData, &request);
        if (request.errorMessage)
            raise_syscall(taskData, request.errorMessage, request.errCode);
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

/*
 *  Loading saved state files.
 */

class StateLoader: public MainThreadRequest
{
public:
    StateLoader(bool isH, Handle files): MainThreadRequest(MTP_LOADSTATE),
        isHierarchy(isH), fileNameList(files), errorResult(0), errNumber(0) { }

    virtual void Perform(void);
    bool LoadFile(bool isInitial, time_t requiredStamp, PolyWord tail);
    bool isHierarchy;
    Handle fileNameList;
    const char *errorResult;
    // The fileName here is the last file loaded.  As well as using it
    // to load the name can also be printed out at the end to identify the
    // particular file in the hierarchy that failed.
    AutoFree<TCHAR*> fileName;
    int errNumber;
};

// Called by the main thread once all the ML threads have stopped.
void StateLoader::Perform(void)
{
    // Copy the first file name into the buffer.
    if (isHierarchy)
    {
        if (ML_Cons_Cell::IsNull(fileNameList->Word()))
        {
            errorResult = "Hierarchy list is empty";
            return;
        }
        ML_Cons_Cell *p = DEREFLISTHANDLE(fileNameList);
        fileName = Poly_string_to_T_alloc(p->h);
        if (fileName == NULL)
        {
            errorResult = "Insufficient memory";
            errNumber = NOMEMORY;
            return;
        }
        (void)LoadFile(true, 0, p->t);
    }
    else
    {
        fileName = Poly_string_to_T_alloc(fileNameList->Word());
        if (fileName == NULL)
        {
            errorResult = "Insufficient memory";
            errNumber = NOMEMORY;
            return;
        }
        (void)LoadFile(true, 0, TAGGED(0));
    }
}

class ClearVolatile: public ScanAddress
{
public:
    ClearVolatile() {}
    virtual PolyObject *ScanObjectAddress(PolyObject *base) { return base; }
    virtual void ScanAddressesInObject(PolyObject *base, POLYUNSIGNED lengthWord);
};

// Set the values of external references and clear the values of other weak byte refs.
void ClearVolatile::ScanAddressesInObject(PolyObject *base, POLYUNSIGNED lengthWord)
{
    if (OBJ_IS_MUTABLE_OBJECT(lengthWord) && OBJ_IS_NO_OVERWRITE(lengthWord))
    {
        if (OBJ_IS_BYTE_OBJECT(lengthWord))
        {
            if (OBJ_IS_WEAKREF_OBJECT(lengthWord))
            {
                POLYUNSIGNED len = OBJ_OBJECT_LENGTH(lengthWord);
                if (len >= sizeof(uintptr_t) / sizeof(PolyWord))
                    *((uintptr_t*)base) = 0;
                setEntryPoint(base);
            }
        }
        else
        {
            // Clear volatile refs
            POLYUNSIGNED len = OBJ_OBJECT_LENGTH(lengthWord);
            for (POLYUNSIGNED i = 0; i < len; i++)
                base->Set(i, TAGGED(0));
        }
    }
}

// This is copied from the B-tree in MemMgr.  It probably should be
// merged but will do for the moment.  It's intended to reduce the
// cost of finding the segment for relocation.

class SpaceBTree
{
public:
    SpaceBTree(bool is, unsigned i = 0) : isLeaf(is), index(i) { }
    virtual ~SpaceBTree() {}

    bool isLeaf;
    unsigned index; // The index if this is a leaf
};

// A non-leaf node in the B-tree
class SpaceBTreeTree : public SpaceBTree
{
public:
    SpaceBTreeTree();
    virtual ~SpaceBTreeTree();

    SpaceBTree *tree[256];
};

SpaceBTreeTree::SpaceBTreeTree() : SpaceBTree(false)
{
    for (unsigned i = 0; i < 256; i++)
        tree[i] = 0;
}

SpaceBTreeTree::~SpaceBTreeTree()
{
    for (unsigned i = 0; i < 256; i++)
        delete(tree[i]);
}


// This class is used to relocate addresses in areas that have been loaded.
class LoadRelocate: public ScanAddress
{
public:
    LoadRelocate(bool pcc = false): processCodeConstants(pcc), originalBaseAddr(0), descrs(0),
        targetAddresses(0), nDescrs(0), spaceTree(0) {}
    ~LoadRelocate();

    void RelocateObject(PolyObject *p);
    virtual PolyObject *ScanObjectAddress(PolyObject *base) { ASSERT(0); return base; } // Not used
    virtual void ScanConstant(PolyObject *base, byte *addressOfConstant, ScanRelocationKind code, intptr_t displacement);
    void RelocateAddressAt(PolyWord *pt);
    PolyObject *RelocateAddress(PolyObject *obj);
    void AddTreeRange(SpaceBTree **t, unsigned index, uintptr_t startS, uintptr_t endS);

    bool processCodeConstants;
    PolyWord *originalBaseAddr;
    SavedStateSegmentDescr *descrs;
    PolyWord **targetAddresses;
    unsigned nDescrs;
    SpaceBTree *spaceTree;
    intptr_t relativeOffset;
};

LoadRelocate::~LoadRelocate()
{
    if (descrs) delete[](descrs);
    if (targetAddresses) delete[](targetAddresses);
    delete(spaceTree);
}

// Add an entry to the space B-tree.
void LoadRelocate::AddTreeRange(SpaceBTree **tt, unsigned index, uintptr_t startS, uintptr_t endS)
{
    if (*tt == 0)
        *tt = new SpaceBTreeTree;
    ASSERT(!(*tt)->isLeaf);
    SpaceBTreeTree *t = (SpaceBTreeTree*)*tt;

    const unsigned shift = (sizeof(void*) - 1) * 8; // Takes the high-order byte
    uintptr_t r = startS >> shift;
    ASSERT(r < 256);
    const uintptr_t s = endS == 0 ? 256 : endS >> shift;
    ASSERT(s >= r && s <= 256);

    if (r == s) // Wholly within this entry
        AddTreeRange(&(t->tree[r]), index, startS << 8, endS << 8);
    else
    {
        // Deal with any remainder at the start.
        if ((r << shift) != startS)
        {
            AddTreeRange(&(t->tree[r]), index, startS << 8, 0 /*End of range*/);
            r++;
        }
        // Whole entries.
        while (r < s)
        {
            ASSERT(t->tree[r] == 0);
            t->tree[r] = new SpaceBTree(true, index);
            r++;
        }
        // Remainder at the end.
        if ((s << shift) != endS)
            AddTreeRange(&(t->tree[r]), index, 0, endS << 8);
    }
}


// Update the addresses in a group of words.
void LoadRelocate::RelocateAddressAt(PolyWord *pt)
{
    PolyWord val = *pt;
    if (! val.IsTagged())
        *gMem.SpaceForAddress(pt)->writeAble(pt) = RelocateAddress(val.AsObjPtr(originalBaseAddr));
}

PolyObject *LoadRelocate::RelocateAddress(PolyObject *obj)
{
    // Which segment is this address in?
    // N.B. As with SpaceForAddress we need to subtract 1 to point to the length word.
    uintptr_t t = (uintptr_t)((PolyWord*)obj - 1);
    SpaceBTree *tr = spaceTree;

    // Each level of the tree is either a leaf or a vector of trees.
    unsigned j = sizeof(void *) * 8;
    for (;;)
    {
        if (tr == 0) break;
        if (tr->isLeaf) {
            // It's in this segment: relocate it to the current position.
            unsigned i = tr->index;
            SavedStateSegmentDescr *descr = &descrs[i];
            PolyWord *newAddress = targetAddresses[descr->segmentIndex];
            ASSERT((char*)obj > descr->originalAddress &&
                (char*)obj <= (char*)descr->originalAddress + descr->segmentSize);
            ASSERT(newAddress != 0);
            byte *setAddress = (byte*)newAddress + ((char*)obj - (char*)descr->originalAddress);
            return (PolyObject*)setAddress;
        }
        j -= 8;
        tr = ((SpaceBTreeTree*)tr)->tree[(t >> j) & 0xff];
    }

    // This should never happen.
    ASSERT(0);
    return 0;
}

// This is based on Exporter::relocateObject but does the reverse.
// It attempts to adjust all the addresses in the object when it has
// been read in.
void LoadRelocate::RelocateObject(PolyObject *p)
{
    if (p->IsByteObject())
    {
    }
    else if (p->IsCodeObject())
    {
        POLYUNSIGNED constCount;
        PolyWord *cp;
        ASSERT(! p->IsMutable() );
        machineDependent->GetConstSegmentForCode(p, cp, constCount);
        /* Now the constant area. */
        for (POLYUNSIGNED i = 0; i < constCount; i++) RelocateAddressAt(&(cp[i]));
        // Saved states and modules have relocation entries for constants in the code.
        // We can't use them when loading object files in 32-in-64 so have to process the
        // constants here.
        if (processCodeConstants)
            machineDependent->ScanConstantsWithinCode(p, this);
        // On 32-in-64 ARM we may have to update the global heap base in an FFI callback.
        machineDependent->UpdateGlobalHeapReference(p);
    }
    else if (p->IsClosureObject())
    {
        // The first word is the address of the code.
        POLYUNSIGNED length = p->Length();
        *(PolyObject**)p = RelocateAddress(*(PolyObject**)p);
        for (POLYUNSIGNED i = sizeof(PolyObject*)/sizeof(PolyWord); i < length; i++)
            RelocateAddressAt(p->Offset(i));
    }
    else /* Ordinary objects, essentially tuples. */
    {
        POLYUNSIGNED length = p->Length();
        for (POLYUNSIGNED i = 0; i < length; i++) RelocateAddressAt(p->Offset(i));
    }
}

// Update addresses as constants within the code.
void LoadRelocate::ScanConstant(PolyObject *base, byte *addressOfConstant, ScanRelocationKind code, intptr_t displacement)
{
    PolyObject *p = GetConstantValue(addressOfConstant, code, displacement);

    if (p != 0)
    {
        // Relative addresses are computed by adding the CURRENT address.
        // We have to convert them into addresses in original space before we
        // can relocate them.
        if (code == PROCESS_RELOC_I386RELATIVE)
            p = (PolyObject*)((PolyWord*)p + relativeOffset);
        PolyObject *newValue = RelocateAddress(p);
        SetConstantValue(addressOfConstant, newValue, code);
    }
}

// Work around bug in Mac OS when reading into MAP_JIT memory.
static size_t readData(void *ptr, size_t size, FILE *stream)
{
#ifndef MACOSX
    return fread(ptr, size, 1, stream);
#else
    char buff[1024];
    for (size_t s = 0; s < size; )
    {
        size_t unit = sizeof(buff);
        if (size - s < unit) unit = size-s;
        if (fread(buff, unit, 1, stream) != 1)
            return 0;
        memcpy((char*)ptr+s, buff, unit);
        s += unit;
    }
    return 1; // Succeeded
#endif
}

// Load a saved state file.  Calls itself to handle parent files.
bool StateLoader::LoadFile(bool isInitial, time_t requiredStamp, PolyWord tail)
{
    LoadRelocate relocate;
    AutoFree<TCHAR*> thisFile(_tcsdup(fileName));

    AutoClose loadFile(_tfopen(fileName, _T("rb")));
    if ((FILE*)loadFile == NULL)
    {
        errorResult = "Cannot open load file";
        errNumber = ERRORNUMBER;
        return false;
    }

    SavedStateHeader header;
    // Read the header and check the signature.
    if (fread(&header, sizeof(SavedStateHeader), 1, loadFile) != 1)
    {
        errorResult = "Unable to load header";
        return false;
    }
    if (strncmp(header.headerSignature, SAVEDSTATESIGNATURE, sizeof(header.headerSignature)) != 0)
    {
        errorResult = "File is not a saved state";
        return false;
    }
    if (header.headerVersion != SAVEDSTATEVERSION ||
        header.headerLength != sizeof(SavedStateHeader) ||
        header.segmentDescrLength != sizeof(SavedStateSegmentDescr))
    {
        errorResult = "Unsupported version of saved state file";
        return false;
    }

    // Check that we have the required stamp before loading any children.
    // If a parent has been overwritten we could get a loop.
    if (! isInitial && header.timeStamp != requiredStamp)
    {
        // Time-stamps don't match.
        errorResult = "The parent for this saved state does not match or has been changed";
        return false;
    }

    // Have verified that this is a reasonable saved state file.  If it isn't a
    // top-level file we have to load the parents first.
    if (header.parentNameEntry != 0)
    {
        if (isHierarchy)
        {
            // Take the file name from the list
            if (ML_Cons_Cell::IsNull(tail))
            {
                errorResult = "Missing parent name in argument list";
                return false;
            }
            ML_Cons_Cell *p = (ML_Cons_Cell *)tail.AsObjPtr();
            fileName = Poly_string_to_T_alloc(p->h);
            if (fileName == NULL)
            {
                errorResult = "Insufficient memory";
                errNumber = NOMEMORY;
                return false;
            }
            if (! LoadFile(false, header.parentTimeStamp, p->t))
                return false;
        }
        else
        {
            size_t toRead = header.stringTableSize-header.parentNameEntry;
            size_t elems = ((toRead + sizeof(TCHAR) - 1) / sizeof(TCHAR));
            // Always allow space for null terminator
            size_t roundedBytes = (elems + 1) * sizeof(TCHAR);
            TCHAR *newFileName = (TCHAR *)realloc(fileName, roundedBytes);
            if (newFileName == NULL)
            {
                errorResult = "Insufficient memory";
                errNumber = NOMEMORY;
                return false;
            }
            fileName = newFileName;

            if (header.parentNameEntry >= header.stringTableSize /* Bad entry */ ||
                fseek(loadFile, header.stringTable + header.parentNameEntry, SEEK_SET) != 0 ||
                fread(fileName, 1, toRead, loadFile) != toRead)
            {
                errorResult = "Unable to read parent file name";
                return false;
            }
            fileName[elems] = 0; // Should already be null-terminated, but just in case.

            if (! LoadFile(false, header.parentTimeStamp, TAGGED(0)))
                return false;
        }

        ASSERT(hierarchyDepth > 0 && hierarchyTable[hierarchyDepth-1] != 0);
    }
    else // Top-level file
    {
        if (isHierarchy && ! ML_Cons_Cell::IsNull(tail))
        {
            // There should be no further file names if this is really the top.
            errorResult = "Too many file names in the list";
            return false;
        }
        if (header.parentTimeStamp != exportTimeStamp)
        {
            // Time-stamp does not match executable.
            errorResult = 
                    "Saved state was exported from a different executable or the executable has changed";
            return false;
        }

        // Any existing spaces at this level or greater must be turned
        // into local spaces.  We may have references from the stack to objects that
        // have previously been imported but otherwise these spaces are no longer
        // needed.
        gMem.DemoteImportSpaces();
        // Clean out the hierarchy table.
        for (unsigned h = 0; h < hierarchyDepth; h++)
        {
            delete(hierarchyTable[h]);
            hierarchyTable[h] = 0;
        }
        hierarchyDepth = 0;
    }

    // Now have a valid, matching saved state.
    // Load the segment descriptors.
    relocate.nDescrs = header.segmentDescrCount;
    relocate.descrs = new SavedStateSegmentDescr[relocate.nDescrs];
    relocate.originalBaseAddr = (PolyWord*)header.originalBaseAddr;

    if (fseek(loadFile, header.segmentDescr, SEEK_SET) != 0 ||
        fread(relocate.descrs, sizeof(SavedStateSegmentDescr), relocate.nDescrs, loadFile) != relocate.nDescrs)
    {
        errorResult = "Unable to read segment descriptors";
        return false;
    }
    {
        unsigned maxIndex = 0;
        for (unsigned i = 0; i < relocate.nDescrs; i++)
        {
            if (relocate.descrs[i].segmentIndex > maxIndex)
                maxIndex = relocate.descrs[i].segmentIndex;
            relocate.AddTreeRange(&relocate.spaceTree, i, (uintptr_t)relocate.descrs[i].originalAddress,
                (uintptr_t)((char*)relocate.descrs[i].originalAddress + relocate.descrs[i].segmentSize-1));
        }
        relocate.targetAddresses = new PolyWord*[maxIndex+1];
        for (unsigned i = 0; i <= maxIndex; i++) relocate.targetAddresses[i] = 0;
    }

    // Read in and create the new segments first.  If we have problems,
    // in particular if we have run out of memory, then it's easier to recover.  
    for (unsigned i = 0; i < relocate.nDescrs; i++)
    {
        SavedStateSegmentDescr *descr = &relocate.descrs[i];
        MemSpace *space = gMem.SpaceForIndex(descr->segmentIndex);
        if (space != NULL) relocate.targetAddresses[descr->segmentIndex] = space->bottom;

        if (descr->segmentData == 0)
        { // No data - just an entry in the index.
            if (space == NULL/* ||
                descr->segmentSize != (size_t)((char*)space->top - (char*)space->bottom)*/)
            {
                errorResult = "Mismatch for existing memory space";
                return false;
            }
        }
        else if ((descr->segmentFlags & SSF_OVERWRITE) == 0)
        { // New segment.
            if (space != NULL)
            {
                errorResult = "Segment already exists";
                return false;
            }
            // Allocate memory for the new segment.
            unsigned mFlags =
                (descr->segmentFlags & SSF_WRITABLE ? MTF_WRITEABLE : 0) |
                (descr->segmentFlags & SSF_NOOVERWRITE ? MTF_NO_OVERWRITE : 0) |
                (descr->segmentFlags & SSF_BYTES ? MTF_BYTES : 0) |
                (descr->segmentFlags & SSF_CODE ? MTF_EXECUTABLE : 0);
            PermanentMemSpace *newSpace =
                gMem.AllocateNewPermanentSpace(descr->segmentSize, mFlags, descr->segmentIndex, hierarchyDepth + 1);
            if (newSpace == 0)
            {
                errorResult = "Unable to allocate memory";
                return false;
            }

            PolyWord *mem  = newSpace->bottom;
            PolyWord* writeAble = newSpace->writeAble(mem);
            if (fseek(loadFile, descr->segmentData, SEEK_SET) != 0)
            {
                errorResult = "Unable to seek segment";
                return false;
            }
            if (readData(writeAble, descr->segmentSize, loadFile) != 1)
            {
                errorResult = "Unable to read segment";
                return false;
            }
            // Fill unused space to the top of the area.
            gMem.FillUnusedSpace(writeAble +descr->segmentSize/sizeof(PolyWord),
                newSpace->spaceSize() - descr->segmentSize/sizeof(PolyWord));
            // Leave it writable until we've done the relocations.

            relocate.targetAddresses[descr->segmentIndex] = mem;
            if (newSpace->noOverwrite)
            {
                ClearVolatile cwbr;
                cwbr.ScanAddressesInRegion(newSpace->bottom, newSpace->topPointer);
            }
        }
    }

    // Now read in the mutable overwrites and relocate.

    for (unsigned j = 0; j < relocate.nDescrs; j++)
    {
        SavedStateSegmentDescr *descr = &relocate.descrs[j];
        MemSpace *space = gMem.SpaceForIndex(descr->segmentIndex);
        ASSERT(space != NULL); // We should have created it.
        if (descr->segmentFlags & SSF_OVERWRITE)
        {
            if (fseek(loadFile, descr->segmentData, SEEK_SET) != 0 ||
                fread(space->bottom, descr->segmentSize, 1, loadFile) != 1)
            {
                errorResult = "Unable to read segment";
                return false;
            }
        }

        // Relocation.
        if (descr->segmentData != 0)
        {
            // Adjust the addresses in the loaded segment.
            for (PolyWord *p = space->bottom; p < space->top; )
            {
                p++;
                PolyObject *obj = (PolyObject*)p;
                POLYUNSIGNED length = obj->Length();
                relocate.RelocateObject(obj);
                p += length;
            }
        }

        // Process explicit relocations.
        // If we get errors just skip the error and continue rather than leave
        // everything in an unstable state.
        if (descr->relocations)
        {
            if (fseek(loadFile, descr->relocations, SEEK_SET) != 0)
            {
                errorResult = "Unable to read relocation segment";
                return false;
            }
            for (unsigned k = 0; k < descr->relocationCount; k++)
            {
                RelocationEntry reloc;
                if (fread(&reloc, sizeof(reloc), 1, loadFile) != 1)
                {
                    errorResult = "Unable to read relocation segment";
                    return false;
                }
                MemSpace *toSpace = gMem.SpaceForIndex(reloc.targetSegment);
                if (toSpace == NULL)
                {
                    errorResult = "Unknown space reference in relocation";
                    continue;
                }
                byte *setAddress = (byte*)space->bottom + reloc.relocAddress;
                byte *targetAddress = (byte*)toSpace->bottom + reloc.targetAddress;
                if (setAddress >= (byte*)space->top || targetAddress >= (byte*)toSpace->top)
                {
                    errorResult = "Bad relocation";
                    continue;
                }
                ScanAddress::SetConstantValue(setAddress, (PolyObject*)(targetAddress), reloc.relKind);
            }
        }
    }

    // Set the final permissions.
    for (unsigned j = 0; j < relocate.nDescrs; j++)
    {
        SavedStateSegmentDescr *descr = &relocate.descrs[j];
        if (descr->segmentData != 0)
        {
            PermanentMemSpace* space = gMem.SpaceForIndex(descr->segmentIndex);
            gMem.CompletePermanentSpaceAllocation(space);
        }
    }

    // Add an entry to the hierarchy table for this file.
    if (! AddHierarchyEntry(thisFile, header.timeStamp))
        return false;

    return true; // Succeeded
}

static void LoadState(TaskData *taskData, bool isHierarchy, Handle hFileList)
// Load a saved state or a hierarchy.  
// hFileList is a list if this is a hierarchy and a single name if it is not.
{
    StateLoader loader(isHierarchy, hFileList);
    // Request the main thread to do the load.  This may set the error string if it failed.
    processes->MakeRootRequest(taskData, &loader);

    if (loader.errorResult != 0)
    {
        if (loader.errNumber == 0)
            raise_fail(taskData, loader.errorResult);
        else
        {
            AutoFree<char*> buff((char *)malloc(strlen(loader.errorResult) + 2 + _tcslen(loader.fileName) * sizeof(TCHAR) + 1));
#if (defined(_WIN32) && defined(UNICODE))
            sprintf(buff, "%s: %S", loader.errorResult, (TCHAR *)loader.fileName);
#else
            sprintf(buff, "%s: %s", loader.errorResult, (TCHAR *)loader.fileName);
#endif
            raise_syscall(taskData, buff, loader.errNumber);
        }
    }
}

// Load a saved state file and any ancestors.
POLYUNSIGNED PolyLoadState(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);

    try {
        LoadState(taskData, false, pushedArg);
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

// Load hierarchy.  This provides a complete list of children and parents.
POLYUNSIGNED PolyLoadHierarchy(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);

    try {
         LoadState(taskData, true, pushedArg);
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

/*
 *  Additional functions to provide information or change saved-state files.
 */

// These functions do not affect the global state so can be executed by
// the ML threads directly.

static Handle ShowHierarchy(TaskData *taskData)
// Return the list of files in the hierarchy.
{
    Handle saved = taskData->saveVec.mark();
    Handle list  = SAVE(ListNull);

    // Process this in reverse order.
    for (unsigned i = hierarchyDepth; i > 0; i--)
    {
        Handle value = SAVE(C_string_to_Poly(taskData, hierarchyTable[i-1]->fileName));
        Handle next  = alloc_and_save(taskData, sizeof(ML_Cons_Cell)/sizeof(PolyWord));
        DEREFLISTHANDLE(next)->h = value->Word();
        DEREFLISTHANDLE(next)->t = list->Word();
        taskData->saveVec.reset(saved);
        list = SAVE(next->Word());
    }
    return list;
}

// Show the hierarchy.
POLYUNSIGNED PolyShowHierarchy(POLYUNSIGNED threadId)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle result = 0;

    try {
        result = ShowHierarchy(taskData);
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

static void RenameParent(TaskData *taskData, PolyWord childName, PolyWord parentName)
// Change the name of the immediate parent stored in a child
{
    // The name of the file to modify.
    AutoFree<TCHAR*> fileNameBuff(Poly_string_to_T_alloc(childName));
    if (fileNameBuff == NULL)
        raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    // The new parent name to insert.
    AutoFree<TCHAR*> parentNameBuff(Poly_string_to_T_alloc(parentName));
    if (parentNameBuff == NULL)
        raise_syscall(taskData, "Insufficient memory", NOMEMORY);

    AutoClose loadFile(_tfopen(fileNameBuff, _T("r+b"))); // Open for reading and writing
    if ((FILE*)loadFile == NULL)
    {
        AutoFree<char*> buff((char *)malloc(23 + _tcslen(fileNameBuff) * sizeof(TCHAR) + 1));
#if (defined(_WIN32) && defined(UNICODE))
        sprintf(buff, "Cannot open load file: %S", (TCHAR *)fileNameBuff);
#else
        sprintf(buff, "Cannot open load file: %s", (TCHAR *)fileNameBuff);
#endif
        raise_syscall(taskData, buff, ERRORNUMBER);
    }

    SavedStateHeader header;
    // Read the header and check the signature.
    if (fread(&header, sizeof(SavedStateHeader), 1, loadFile) != 1)
        raise_fail(taskData, "Unable to load header");

    if (strncmp(header.headerSignature, SAVEDSTATESIGNATURE, sizeof(header.headerSignature)) != 0)
        raise_fail(taskData, "File is not a saved state");

    if (header.headerVersion != SAVEDSTATEVERSION ||
        header.headerLength != sizeof(SavedStateHeader) ||
        header.segmentDescrLength != sizeof(SavedStateSegmentDescr))
    {
        raise_fail(taskData, "Unsupported version of saved state file");
    }

    // Does this actually have a parent?
    if (header.parentNameEntry == 0)
        raise_fail(taskData, "File does not have a parent");

    // At the moment the only entry in the string table is the parent
    // name so we can simply write a new one on the end of the file.
    // This makes the file grow slightly each time but it shouldn't be
    // significant.
    fseek(loadFile, 0, SEEK_END);
    header.stringTable = ftell(loadFile); // Remember where this is
    _fputtc(0, loadFile); // First byte of string table is zero
    _fputts(parentNameBuff, loadFile);
    _fputtc(0, loadFile); // A terminating null.
    header.stringTableSize = (_tcslen(parentNameBuff) + 2)*sizeof(TCHAR);

    // Now rewind and write the header with the revised string table.
    fseek(loadFile, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, loadFile);
}

POLYUNSIGNED PolyRenameParent(POLYUNSIGNED threadId, POLYUNSIGNED childName, POLYUNSIGNED parentName)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();

    try {
        RenameParent(taskData, PolyWord::FromUnsigned(childName), PolyWord::FromUnsigned(parentName));
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

static Handle ShowParent(TaskData *taskData, Handle hFileName)
// Return the name of the immediate parent stored in a child
{
    AutoFree<TCHAR*> fileNameBuff(Poly_string_to_T_alloc(hFileName->Word()));
    if (fileNameBuff == NULL)
        raise_syscall(taskData, "Insufficient memory", NOMEMORY);

    AutoClose loadFile(_tfopen(fileNameBuff, _T("rb")));
    if ((FILE*)loadFile == NULL)
    {
        AutoFree<char*> buff((char *)malloc(23 + _tcslen(fileNameBuff) * sizeof(TCHAR) + 1));
        if (buff == NULL)
            raise_syscall(taskData, "Insufficient memory", NOMEMORY);
#if (defined(_WIN32) && defined(UNICODE))
        sprintf(buff, "Cannot open load file: %S", (TCHAR *)fileNameBuff);
#else
        sprintf(buff, "Cannot open load file: %s", (TCHAR *)fileNameBuff);
#endif
        raise_syscall(taskData, buff, ERRORNUMBER);
    }

    SavedStateHeader header;
    // Read the header and check the signature.
    if (fread(&header, sizeof(SavedStateHeader), 1, loadFile) != 1)
        raise_fail(taskData, "Unable to load header");

    if (strncmp(header.headerSignature, SAVEDSTATESIGNATURE, sizeof(header.headerSignature)) != 0)
        raise_fail(taskData, "File is not a saved state");

    if (header.headerVersion != SAVEDSTATEVERSION ||
        header.headerLength != sizeof(SavedStateHeader) ||
        header.segmentDescrLength != sizeof(SavedStateSegmentDescr))
    {
        raise_fail(taskData, "Unsupported version of saved state file");
    }

    // Does this have a parent?
    if (header.parentNameEntry != 0)
    {
        size_t toRead = header.stringTableSize-header.parentNameEntry;
        size_t elems = ((toRead + sizeof(TCHAR) - 1) / sizeof(TCHAR));
        // Always allow space for null terminator
        size_t roundedBytes = (elems + 1) * sizeof(TCHAR);
        AutoFree<TCHAR*> parentFileName((TCHAR *)malloc(roundedBytes));
        if (parentFileName == NULL)
            raise_syscall(taskData, "Insufficient memory", NOMEMORY);

        if (header.parentNameEntry >= header.stringTableSize /* Bad entry */ ||
            fseek(loadFile, header.stringTable + header.parentNameEntry, SEEK_SET) != 0 ||
            fread(parentFileName, 1, toRead, loadFile) != toRead)
        {
            raise_fail(taskData, "Unable to read parent file name");
        }
        parentFileName[elems] = 0; // Should already be null-terminated, but just in case.
        // Convert the name into a Poly string and then build a "Some" value.
        // It's possible, although silly, to have the empty string as a parent name.
        Handle resVal = SAVE(C_string_to_Poly(taskData, parentFileName));
        Handle result = alloc_and_save(taskData, 1);
        DEREFHANDLE(result)->Set(0, resVal->Word());
        return result;
    }
    else return SAVE(NONE_VALUE);
}

// Return the name of the immediate parent stored in a child
POLYUNSIGNED PolyShowParent(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);
    Handle result = 0;

    try {
        result = ShowParent(taskData, pushedArg);
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

// Module system
#define MODULESIGNATURE "POLYMODU"
#define MODULEVERSION   2

typedef struct _moduleHeader
{
    // These entries are primarily to check that we have a valid
    // saved state file before we try to interpret anything else.
    char        headerSignature[8];     // Should contain MODULESIGNATURE
    unsigned    headerVersion;          // Should contain MODULEVERSION
    unsigned    headerLength;           // Number of bytes in the header
    unsigned    segmentDescrLength;     // Number of bytes in a descriptor

    // These entries contain the real data.
    off_t       segmentDescr;           // Position of segment descriptor table
    unsigned    segmentDescrCount;      // Number of segment descriptors in the table
    time_t      timeStamp;              // The time stamp for this file.
    time_t      executableTimeStamp;    // The time stamp for the parent executable.
    // Root
    uintptr_t   rootSegment;
    POLYUNSIGNED     rootOffset;
} ModuleHeader;

// Store a module
class ModuleStorer: public MainThreadRequest
{
public:
    ModuleStorer(const TCHAR *file, Handle r):
        MainThreadRequest(MTP_STOREMODULE), fileName(file), root(r), errorMessage(0), errCode(0) {}

    virtual void Perform();

    const TCHAR *fileName;
    Handle root;
    const char *errorMessage;
    int errCode;
};

class ModuleExport: public SaveStateExport
{
public:
    ModuleExport(): SaveStateExport(1/* Everything EXCEPT the executable. */) {}
    virtual void exportStore(void); // Write the data out.
};

void ModuleStorer::Perform()
{
    ModuleExport exporter;
#if (defined(_WIN32) && defined(UNICODE))
    exporter.exportFile = _wfopen(fileName, L"wb");
#else
    exporter.exportFile = fopen(fileName, "wb");
#endif
    if (exporter.exportFile == NULL)
    {
        errorMessage = "Cannot open export file";
        errCode = ERRORNUMBER;
        return;
    }
    // RunExport copies everything reachable from the root, except data from
    // the executable because we've set the hierarchy to 1, using CopyScan.
    // It builds the tables in the export data structure then calls exportStore
    // to actually write the data.
    if (! root->Word().IsDataPtr())
    {
        // If we have a completely empty module the list may be null.
        // This needs to be dealt with at a higher level.
        errorMessage = "Module root is not an address";
        return;
    }
    exporter.RunExport(root->WordP());
    errorMessage = exporter.errorMessage; // This will be null unless there's been an error.
}

void ModuleExport::exportStore(void)
{
    // What we need to do here is implement the export in a similar way to e.g. PECOFFExport::exportStore
    // This is copied from SaveRequest::Perform and should be common code.
    ModuleHeader modHeader;
    memset(&modHeader, 0, sizeof(modHeader));
    modHeader.headerLength = sizeof(modHeader);
    memcpy(modHeader.headerSignature,
        MODULESIGNATURE, sizeof(modHeader.headerSignature));
    modHeader.headerVersion = MODULEVERSION;
    modHeader.segmentDescrLength = sizeof(SavedStateSegmentDescr);
    modHeader.executableTimeStamp = exportTimeStamp;
    {
        unsigned rootArea = findArea(this->rootFunction);
        struct _memTableEntry *mt = &memTable[rootArea];
        modHeader.rootSegment = mt->mtIndex;
        modHeader.rootOffset = (POLYUNSIGNED)((char*)this->rootFunction - (char*)mt->mtOriginalAddr);
    }
    modHeader.timeStamp = getBuildTime();
    modHeader.segmentDescrCount = this->memTableEntries; // One segment for each space.
    // Write out the header.
    fwrite(&modHeader, sizeof(modHeader), 1, this->exportFile);

    SavedStateSegmentDescr *descrs = new SavedStateSegmentDescr [this->memTableEntries];
    // We need an entry in the descriptor tables for each segment in the executable because
    // we may have relocations that refer to addresses in it.
    for (unsigned j = 0; j < this->memTableEntries; j++)
    {
        SavedStateSegmentDescr *thisDescr = &descrs[j];
        memoryTableEntry *entry = &this->memTable[j];
        memset(thisDescr, 0, sizeof(SavedStateSegmentDescr));
        thisDescr->relocationSize = sizeof(RelocationEntry);
        thisDescr->segmentIndex = (unsigned)entry->mtIndex;
        thisDescr->segmentSize = entry->mtLength; // Set this even if we don't write it.
        thisDescr->originalAddress = entry->mtOriginalAddr;
        if (entry->mtFlags & MTF_WRITEABLE)
        {
            thisDescr->segmentFlags |= SSF_WRITABLE;
            if (entry->mtFlags & MTF_NO_OVERWRITE)
                thisDescr->segmentFlags |= SSF_NOOVERWRITE;
            if ((entry->mtFlags & MTF_NO_OVERWRITE) == 0)
                thisDescr->segmentFlags |= SSF_OVERWRITE;
            if (entry->mtFlags & MTF_BYTES)
                thisDescr->segmentFlags |= SSF_BYTES;
        }
        if (entry->mtFlags & MTF_EXECUTABLE)
             thisDescr->segmentFlags |= SSF_CODE;
    }
    // Write out temporarily. Will be overwritten at the end.
    modHeader.segmentDescr = ftell(this->exportFile);
    fwrite(descrs, sizeof(SavedStateSegmentDescr), this->memTableEntries, this->exportFile);

    // Write out the relocations and the data.
    for (unsigned k = 0; k < this->memTableEntries; k++)
    {
        SavedStateSegmentDescr *thisDescr = &descrs[k];
        memoryTableEntry *entry = &this->memTable[k];
        if (k >= newAreas) // Not permanent areas
        {
            thisDescr->relocations = ftell(this->exportFile);
            // Have to write this out.
            this->relocationCount = 0;
            // Create the relocation table.
            char *start = (char*)entry->mtOriginalAddr;
            char *end = start + entry->mtLength;
            for (PolyWord *p = (PolyWord*)start; p < (PolyWord*)end; )
            {
                p++;
                PolyObject *obj = (PolyObject*)p;
                POLYUNSIGNED length = obj->Length();
                // For saved states we don't include explicit relocations except
                // in code but it's easier if we do for modules.
                if (length != 0 && obj->IsCodeObject())
                    machineDependent->ScanConstantsWithinCode(obj, this);
                relocateObject(obj);
                p += length;
            }
            thisDescr->relocationCount = this->relocationCount;
            // Write out the data.
            thisDescr->segmentData = ftell(exportFile);
            fwrite(entry->mtOriginalAddr, entry->mtLength, 1, exportFile);
        }
    }

    // Rewrite the header and the segment tables now they're complete.
    fseek(exportFile, 0, SEEK_SET);
    fwrite(&modHeader, sizeof(modHeader), 1, exportFile);
    fwrite(descrs, sizeof(SavedStateSegmentDescr), this->memTableEntries, exportFile);
    delete[](descrs);

    fclose(exportFile); exportFile = NULL;
}

// Store a module
POLYUNSIGNED PolyStoreModule(POLYUNSIGNED threadId, POLYUNSIGNED name, POLYUNSIGNED contents)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedContents = taskData->saveVec.push(contents);

    try {
        TempString fileName(PolyWord::FromUnsigned(name));
        ModuleStorer storer(fileName, pushedContents);
        processes->MakeRootRequest(taskData, &storer);
        if (storer.errorMessage)
            raise_syscall(taskData, storer.errorMessage, storer.errCode);
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned();
}

// Load a module.
class ModuleLoader: public MainThreadRequest
{
public:
    ModuleLoader(TaskData *taskData, const TCHAR *file):
        MainThreadRequest(MTP_LOADMODULE), callerTaskData(taskData), fileName(file),
            errorResult(NULL), errNumber(0), rootHandle(0) {}

    virtual void Perform();

    TaskData *callerTaskData;
    const TCHAR *fileName;
    const char *errorResult;
    int errNumber;
    Handle rootHandle;
};

void ModuleLoader::Perform()
{
    AutoClose loadFile(_tfopen(fileName, _T("rb")));
    if ((FILE*)loadFile == NULL)
    {
        errorResult = "Cannot open load file";
        errNumber = ERRORNUMBER;
        return;
    }

    ModuleHeader header;
    // Read the header and check the signature.
    if (fread(&header, sizeof(ModuleHeader), 1, loadFile) != 1)
    {
        errorResult = "Unable to load header";
        return;
    }
    if (strncmp(header.headerSignature, MODULESIGNATURE, sizeof(header.headerSignature)) != 0)
    {
        errorResult = "File is not a Poly/ML module";
        return;
    }
    if (header.headerVersion != MODULEVERSION ||
        header.headerLength != sizeof(ModuleHeader) ||
        header.segmentDescrLength != sizeof(SavedStateSegmentDescr))
    {
        errorResult = "Unsupported version of module file";
        return;
    }
    if (header.executableTimeStamp != exportTimeStamp)
    {
        // Time-stamp does not match executable.
        errorResult = 
                "Module was exported from a different executable or the executable has changed";
        return;
    }
    LoadRelocate relocate;
    relocate.nDescrs = header.segmentDescrCount;
    relocate.descrs = new SavedStateSegmentDescr[relocate.nDescrs];

    if (fseek(loadFile, header.segmentDescr, SEEK_SET) != 0 ||
        fread(relocate.descrs, sizeof(SavedStateSegmentDescr), relocate.nDescrs, loadFile) != relocate.nDescrs)
    {
        errorResult = "Unable to read segment descriptors";
        return;
    }
    {
        unsigned maxIndex = 0;
        for (unsigned i = 0; i < relocate.nDescrs; i++)
            if (relocate.descrs[i].segmentIndex > maxIndex)
                maxIndex = relocate.descrs[i].segmentIndex;
        relocate.targetAddresses = new PolyWord*[maxIndex+1];
        for (unsigned i = 0; i <= maxIndex; i++) relocate.targetAddresses[i] = 0;
    }

    // Read in and create the new segments first.  If we have problems,
    // in particular if we have run out of memory, then it's easier to recover.  
    for (unsigned i = 0; i < relocate.nDescrs; i++)
    {
        SavedStateSegmentDescr *descr = &relocate.descrs[i];
        MemSpace *space = gMem.SpaceForIndex(descr->segmentIndex);

        if (descr->segmentData == 0)
        { // No data - just an entry in the index.
            if (space == NULL/* ||
                descr->segmentSize != (size_t)((char*)space->top - (char*)space->bottom)*/)
            {
                errorResult = "Mismatch for existing memory space";
                return;
            }
            else relocate.targetAddresses[descr->segmentIndex] = space->bottom;
        }
        else
        { // New segment.
            if (space != NULL)
            {
                errorResult = "Segment already exists";
                return;
            }
            // Allocate memory for the new segment.
            size_t actualSize = descr->segmentSize;
            MemSpace *space;
            if (descr->segmentFlags & SSF_CODE)
            {
                CodeSpace *cSpace = gMem.NewCodeSpace(actualSize);
                if (cSpace == 0)
                {
                    errorResult = "Unable to allocate memory";
                    return;
                }
                space = cSpace;
                cSpace->firstFree = (PolyWord*)((byte*)space->bottom + descr->segmentSize);
                if (cSpace->firstFree != cSpace->top)
                    gMem.FillUnusedSpace(cSpace->firstFree, cSpace->top - cSpace->firstFree);
            }
            else
            {
                LocalMemSpace *lSpace = gMem.NewLocalSpace(actualSize, descr->segmentFlags & SSF_WRITABLE);
                if (lSpace == 0)
                {
                    errorResult = "Unable to allocate memory";
                    return;
                }
                space = lSpace;
                lSpace->lowerAllocPtr = (PolyWord*)((byte*)lSpace->bottom + descr->segmentSize);
            }
            if (fseek(loadFile, descr->segmentData, SEEK_SET) != 0)
            {
                errorResult = "Unable to seek to segment";
                return;
            }
            if (readData(space->bottom, descr->segmentSize, loadFile) != 1)
            {
                errorResult = "Unable to read segment";
                return;
            }
            relocate.targetAddresses[descr->segmentIndex] = space->bottom;
            if (space->isMutable && (descr->segmentFlags & SSF_BYTES) != 0)
            {
                ClearVolatile cwbr;
                cwbr.ScanAddressesInRegion(space->bottom, (PolyWord*)((byte*)space->bottom + descr->segmentSize));
            }
        }
    }
    // Now deal with relocation.
    for (unsigned j = 0; j < relocate.nDescrs; j++)
    {
        SavedStateSegmentDescr *descr = &relocate.descrs[j];
        PolyWord *baseAddr = relocate.targetAddresses[descr->segmentIndex];
        ASSERT(baseAddr != NULL); // We should have created it.
        // Process explicit relocations.
        // If we get errors just skip the error and continue rather than leave
        // everything in an unstable state.
        if (descr->relocations)
        {
            if (fseek(loadFile, descr->relocations, SEEK_SET) != 0)
                errorResult = "Unable to read relocation segment";
            for (unsigned k = 0; k < descr->relocationCount; k++)
            {
                RelocationEntry reloc;
                if (fread(&reloc, sizeof(reloc), 1, loadFile) != 1)
                    errorResult = "Unable to read relocation segment";
                byte *setAddress = (byte*)baseAddr + reloc.relocAddress;
                byte *targetAddress = (byte*)relocate.targetAddresses[reloc.targetSegment] + reloc.targetAddress;
                ScanAddress::SetConstantValue(setAddress, (PolyObject*)(targetAddress), reloc.relKind);
            }
        }
    }

    // Get the root address.  Push this to the caller's save vec.  If we put the
    // newly created areas into local memory we could get a GC as soon as we
    // complete this root request.
    {
        PolyWord *baseAddr = relocate.targetAddresses[header.rootSegment];
        rootHandle = callerTaskData->saveVec.push((PolyObject*)((byte*)baseAddr + header.rootOffset));
    }
}

static Handle LoadModule(TaskData *taskData, Handle args)
{
    TempString fileName(args->Word());
    ModuleLoader loader(taskData, fileName);
    processes->MakeRootRequest(taskData, &loader);

    if (loader.errorResult != 0)
    {
        if (loader.errNumber == 0)
            raise_fail(taskData, loader.errorResult);
        else
        {
            AutoFree<char*> buff((char *)malloc(strlen(loader.errorResult) + 2 + _tcslen(loader.fileName) * sizeof(TCHAR) + 1));
#if (defined(_WIN32) && defined(UNICODE))
            sprintf(buff, "%s: %S", loader.errorResult, loader.fileName);
#else
            sprintf(buff, "%s: %s", loader.errorResult, loader.fileName);
#endif
            raise_syscall(taskData, buff, loader.errNumber);
        }
    }

    return loader.rootHandle;
}

// Load a module
POLYUNSIGNED PolyLoadModule(POLYUNSIGNED threadId, POLYUNSIGNED arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);
    Handle result = 0;

    try {
        result = LoadModule(taskData, pushedArg);
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}


PolyObject *InitHeaderFromExport(struct _exportDescription *exports)
{
    // Check the structure sizes stored in the export structure match the versions
    // used in this library.
    if (exports->structLength != sizeof(exportDescription) ||
        exports->memTableSize != sizeof(memoryTableEntry) ||
        exports->rtsVersion < FIRST_supported_version ||
        exports->rtsVersion > LAST_supported_version)
    {
#if (FIRST_supported_version == LAST_supported_version)
        Exit("The exported object file has version %0.2f but this library supports %0.2f",
            ((float)exports->rtsVersion) / 100.0,
            ((float)FIRST_supported_version) / 100.0);
#else
        Exit("The exported object file has version %0.2f but this library supports %0.2f-%0.2f",
            ((float)exports->rtsVersion) / 100.0,
            ((float)FIRST_supported_version) / 100.0,
            ((float)LAST_supported_version) / 100.0);
#endif
    }
    // We could also check the RTS version and the architecture.
    exportTimeStamp = exports->timeStamp; // Needed for load and save.

    memoryTableEntry *memTable = exports->memTable;
#ifdef POLYML32IN64
    // We need to copy this into the heap before beginning execution.
    // This is very like loading a saved state and the code should probably
    // be merged.
    LoadRelocate relocate(true);
    relocate.nDescrs = exports->memTableEntries;
    relocate.descrs = new SavedStateSegmentDescr[relocate.nDescrs];
    relocate.targetAddresses = new PolyWord*[exports->memTableEntries];
    relocate.originalBaseAddr = (PolyWord*)exports->originalBaseAddr;

    PolyObject *root = 0;

    for (unsigned i = 0; i < exports->memTableEntries; i++)
    {
        relocate.descrs[i].segmentIndex = memTable[i].mtIndex;
        relocate.descrs[i].originalAddress = memTable[i].mtOriginalAddr;
        relocate.descrs[i].segmentSize = memTable[i].mtLength;
        PermanentMemSpace *newSpace =
            gMem.AllocateNewPermanentSpace(memTable[i].mtLength, (unsigned)memTable[i].mtFlags, (unsigned)memTable[i].mtIndex);
        if (newSpace == 0)
            Exit("Unable to initialise a permanent memory space");

        PolyWord *mem = newSpace->bottom;
        memcpy(newSpace->writeAble(mem), memTable[i].mtCurrentAddr, memTable[i].mtLength);
        PolyWord* unused = mem + memTable[i].mtLength / sizeof(PolyWord);
        gMem.FillUnusedSpace(newSpace->writeAble(unused),
            newSpace->spaceSize() - memTable[i].mtLength / sizeof(PolyWord));

        if (newSpace == 0)
            Exit("Unable to initialise a permanent memory space");

        relocate.targetAddresses[i] = mem;
        relocate.AddTreeRange(&relocate.spaceTree, i, (uintptr_t)relocate.descrs[i].originalAddress,
            (uintptr_t)((char*)relocate.descrs[i].originalAddress + relocate.descrs[i].segmentSize - 1));

        // Relocate the root function.
        if (exports->rootFunction >= memTable[i].mtCurrentAddr && exports->rootFunction < (char*)memTable[i].mtCurrentAddr + memTable[i].mtLength)
        {
            root = (PolyObject*)((char*)mem + ((char*)exports->rootFunction - (char*)memTable[i].mtCurrentAddr));
        }
    }

    // Now relocate the addresses
    for (unsigned j = 0; j < exports->memTableEntries; j++)
    {
        SavedStateSegmentDescr *descr = &relocate.descrs[j];
        MemSpace *space = gMem.SpaceForIndex(descr->segmentIndex);
        // Any relative addresses have to be corrected by adding this.
        relocate.relativeOffset = (PolyWord*)descr->originalAddress - space->bottom;
        for (PolyWord *p = space->bottom; p < space->top; )
        {
#ifdef POLYML32IN64
            if ((((uintptr_t)p) & 4) == 0)
            {
                // Skip any padding.  The length word should be on an odd-word boundary.
                p++;
                continue;
            }
#endif
            p++;
            PolyObject *obj = (PolyObject*)p;
            POLYUNSIGNED length = obj->Length();
            relocate.RelocateObject(obj);
            p += length;
        }
    }

    // Set the final permissions.
    for (unsigned j = 0; j < exports->memTableEntries; j++)
    {
        PermanentMemSpace *space = gMem.SpaceForIndex(memTable[j].mtIndex);
        gMem.CompletePermanentSpaceAllocation(space);
    }

    return root;

#else
    for (unsigned i = 0; i < exports->memTableEntries; i++)
    {
        // Construct a new space for each of the entries.
        if (gMem.NewPermanentSpace(
            (PolyWord*)memTable[i].mtCurrentAddr,
            memTable[i].mtLength / sizeof(PolyWord), (unsigned)memTable[i].mtFlags,
            (unsigned)memTable[i].mtIndex) == 0)
            Exit("Unable to initialise a permanent memory space");
    }
    return (PolyObject *)exports->rootFunction;
#endif
}

// Return the system directory for modules.  This is configured differently
// in Unix and in Windows.
POLYUNSIGNED PolyGetModuleDirectory(POLYUNSIGNED threadId)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle result = 0;

    try {
#if (defined(MODULEDIR))
        result = SAVE(C_string_to_Poly(taskData, MODULEDIR));
#elif (defined(_WIN32))
        {
            // This registry key is configured when Poly/ML is installed using the installer.
            // It gives the path to the Poly/ML installation directory.  We return the
            // Modules subdirectory.
            HKEY hk;
            if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                _T("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\PolyML.exe"), 0,
                KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS)
            {
                DWORD valSize;
                if (RegQueryValueEx(hk, _T("Path"), 0, NULL, NULL, &valSize) == ERROR_SUCCESS)
                {
#define MODULEDIR _T("Modules")
                    TempString buff((TCHAR*)malloc(valSize + (_tcslen(MODULEDIR) + 1) * sizeof(TCHAR)));
                    DWORD dwType;
                    if (RegQueryValueEx(hk, _T("Path"), 0, &dwType, (LPBYTE)(LPTSTR)buff, &valSize) == ERROR_SUCCESS)
                    {
                        // The registry entry should end with a backslash.
                        _tcscat(buff, MODULEDIR);
                        result = SAVE(C_string_to_Poly(taskData, buff));
                    }
                }
                RegCloseKey(hk);
            }
            result = SAVE(C_string_to_Poly(taskData, ""));
        }
#else
        result = SAVE(C_string_to_Poly(taskData, ""));
#endif
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

struct _entrypts savestateEPT[] =
{
    { "PolySaveState",                  (polyRTSFunction)&PolySaveState },
    { "PolyLoadState",                  (polyRTSFunction)&PolyLoadState },
    { "PolyShowHierarchy",              (polyRTSFunction)&PolyShowHierarchy },
    { "PolyRenameParent",               (polyRTSFunction)&PolyRenameParent },
    { "PolyShowParent",                 (polyRTSFunction)&PolyShowParent },
    { "PolyStoreModule",                (polyRTSFunction)&PolyStoreModule },
    { "PolyLoadModule",                 (polyRTSFunction)&PolyLoadModule },
    { "PolyLoadHierarchy",              (polyRTSFunction)&PolyLoadHierarchy },
    { "PolyGetModuleDirectory",         (polyRTSFunction)&PolyGetModuleDirectory },

    { NULL, NULL } // End of list.
};

