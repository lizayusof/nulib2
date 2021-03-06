/*
 * NuLib2
 * Copyright (C) 2000-2007 by Andy McFadden, All Rights Reserved.
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the BSD License, see the file COPYING.
 *
 * Common archive-related utility functions.
 */
#include "NuLib2.h"


/*
 * ===========================================================================
 *      Output pathnames
 * ===========================================================================
 */

/*
 * General-purpose output pathname filter, invoked by nufxlib via a
 * callback.  Normalizes the pathname to match the current OS requirements.
 *
 * If we're extracting to stdout, this fills in the "newFp" field instead.
 *
 * The buffer we return to the archive library will be overwritten the
 * next time this function gets called.  This is expected.
 */
static NuResult OutputPathnameFilter(NuArchive* pArchive, void* vproposal)
{
    NuPathnameProposal* pathProposal = vproposal;
    NulibState* pState;
    const UNICHAR* newPathnameUNI;
    NuRecordIdx renameFromIdx;
    UNICHAR* renameToStrUNI;
    UNICHAR* resultBufUNI;

    Assert(pArchive != NULL);
    (void) NuGetExtraData(pArchive, (void**) &pState);
    Assert(pState != NULL);

    /* handle extract-to-pipe */
    if (NState_GetCommand(pState) == kCommandExtractToPipe) {
        pathProposal->newDataSink = NState_GetPipeSink(pState);
        NState_SetSuppressOutput(pState, true);
        return kNuOK;
    }

    /*
     * If they're trying to rename this file, do so now.  We don't run
     * the output through the normalizer; if the user typed it, assume
     * that it's okay (and let the OS tell them if it isn't).
     */
    renameToStrUNI = NState_GetRenameToStr(pState);
    if (renameToStrUNI != NULL) {
        renameFromIdx = NState_GetRenameFromIdx(pState);

        if (renameFromIdx == pathProposal->pRecord->recordIdx) {
            /* right source file, proceed with the rename */
            NState_SetTempPathnameLen(pState, strlen(renameToStrUNI) +1);
            resultBufUNI = NState_GetTempPathnameBuf(pState);
            Assert(resultBufUNI != NULL);
            strcpy(resultBufUNI, renameToStrUNI);

            pathProposal->newPathnameUNI = resultBufUNI;
        }

        /* free up renameToStrUNI */
        NState_SetRenameToStr(pState, NULL);

        goto bail;
    }

    /*
     * Convert the pathname into something suitable for the current OS.
     */
    newPathnameUNI = NormalizePath(pState, pathProposal);
    if (newPathnameUNI == NULL) {
        ReportError(kNuErrNone, "unable to convert pathname");
        return kNuAbort;
    }

    pathProposal->newPathnameUNI = newPathnameUNI;

bail:
    return kNuOK;
}


/*
 * ===========================================================================
 *      User input
 * ===========================================================================
 */

/*
 * Get a single character from the input.
 *
 * For portability, I'm just getting a line of input and keeping the
 * first character.  A fancier version would play with line disciplines
 * so you wouldn't have to hit "return".
 */
static char GetReplyChar(char defaultReply)
{
    char tmpBuf[32];

    if (fgets(tmpBuf, sizeof(tmpBuf), stdin) == NULL)
        return defaultReply;
    if (tmpBuf[0] == '\n' || tmpBuf[0] == '\r')
        return defaultReply;

    return tmpBuf[0];
}


#define kMaxInputLen    128

/*
 * Get a string back from the user.
 *
 * String returned should be freed by the caller.
 */
static char* GetReplyString(const char* prompt)
{
    char buf[kMaxInputLen];
    char* result;
    int len;

    printf("%s", prompt);
    fflush(stdout);
    result = fgets(buf, sizeof(buf), stdin);
    if (result == NULL || feof(stdin) || ferror(stdin) || buf[0] == '\0' ||
        buf[0] == '\n')
    {
        return NULL;
    }

    /* nuke the terminating '\n', which is lots of fun in filenames */
    len = strlen(buf);
    if (buf[len-1] == '\n')
        buf[len-1] = '\0';

    return strdup(buf);
}


/*
 * Get a one-line comment from the user, of at most "maxLen" bytes.
 *
 * If the user enters a blank line, return "NULL".
 *
 * A pointer to a newly-allocated buffer is returned.
 */
char* GetSimpleComment(NulibState* pState, const char* pathname, int maxLen)
{
    char* buf = NULL;
    char* result;
    int len;

    buf = Malloc(maxLen);
    if (buf == NULL)
        return NULL;

    printf("Enter one-line comment for '%s'\n: ", pathname);
    fflush(stdout);

    result = fgets(buf, maxLen, stdin);
    if (result == NULL || feof(stdin) || ferror(stdin) || buf[0] == '\0' ||
        buf[0] == '\n')
    {
        Free(buf);
        return NULL;
    }

    /* nuke the terminating '\n', which we don't need */
    len = strlen(buf);
    if (buf[len-1] == '\n')
        buf[len-1] = '\0';

    return buf;
}


/*
 * ===========================================================================
 *      Callbacks (progress updates, error handling, record selection)
 * ===========================================================================
 */

#define kMaxDisplayLen  60

/*
 * Returns "true" if the filespec in "spec" matches what's in "pRecord".
 *
 * (Someday "spec" might be a regexp.)
 */
static Boolean SpecMatchesRecord(NulibState* pState, const char* spec,
    const NuRecord* pRecord)
{
#ifdef NU_CASE_SENSITIVE
    if (NState_GetModRecurse(pState))
        return (strncmp(spec, pRecord->filename, strlen(spec)) == 0);
    else
        return (strcmp(spec, pRecord->filename) == 0);
#else
    if (NState_GetModRecurse(pState))
        return (strncasecmp(spec, pRecord->filenameMOR, strlen(spec)) == 0);
    else
        return (strcasecmp(spec, pRecord->filenameMOR) == 0);
#endif
}

/*
 * Determine whether the current record we're examining is described by
 * the file specification given on the command line.
 *
 * If no filespec was provided, then all records are "specified".
 *
 * We pass the entire NuRecord in because we may want to allow
 * extraction by criteria other than name, e.g. all text files or all
 * files archived before a certain date.
 */
Boolean IsSpecified(NulibState* pState, const NuRecord* pRecord)
{
    char* const* pSpec;
    int i;

    if (!NState_GetFilespecCount(pState))
        return true;

    pSpec = NState_GetFilespecPointer(pState);
    for (i = NState_GetFilespecCount(pState); i > 0; i--, pSpec++) {
        if (SpecMatchesRecord(pState, *pSpec, pRecord))
            return true;
    }

    return false;
}


/*
 * General-purpose selection filter, invoked as a callback.  Compares the
 * selection proposal with the filenames in "filespec".
 */
NuResult SelectionFilter(NuArchive* pArchive, void* vproposal)
{
    const NuSelectionProposal* selProposal = vproposal;
    NulibState* pState;

    Assert(pArchive != NULL);
    (void) NuGetExtraData(pArchive, (void**) &pState);
    Assert(pState != NULL);

    if (IsSpecified(pState, selProposal->pRecord)) {
        NState_IncMatchCount(pState);

        /* we don't get progress notifications for delete, so do it here */
        if (NState_GetCommand(pState) == kCommandDelete) {
            printf("Deleting %s\n", selProposal->pRecord->filenameMOR);
        }

        return kNuOK;
    } else {
        return kNuSkip;
    }
}


/*
 * Print a three-digit progress percentage; range is 0% to 100%.
 */
void PrintPercentage(uint32_t total, uint32_t progress)
{
    uint32_t perc;

    if (!total) {
        /*printf("   %%");*/
        printf("    ");
        return;
    }

    if (total < 21474836) {
        perc = (progress * 100 + 50) / total;
        if (perc > 100)
            perc = 100;
    } else {
        perc = progress / (total / 100);
        if (perc > 100)
            perc = 100;
    }

    printf("%3d%%", perc);
}

/*
 * Show our progress, unless we're expanding to a pipe.  Invoked as a
 * callback by nufxlib.
 */
NuResult ProgressUpdater(NuArchive* pArchive, void* vProgress)
{
    const NuProgressData* pProgress = vProgress;
    NulibState* pState;
    const char* percStr;
    const char* actionStr;
    char nameBuf[kMaxDisplayLen+1];
    Boolean showName, eolConv;

    Assert(pArchive != NULL);
    (void) NuGetExtraData(pArchive, (void**) &pState);
    Assert(pState != NULL);

    if (NState_GetSuppressOutput(pState))
        return kNuOK;

    percStr = NULL;
    showName = false;
    eolConv = false;

    switch (pProgress->operation) {
    case kNuOpAdd:
        switch (pProgress->state) {
        case kNuProgressPreparing:
        case kNuProgressOpening:
            actionStr = "adding     ";
            showName = true;
            break;
        case kNuProgressAnalyzing:
            actionStr = "analyzing  ";
            break;
        case kNuProgressCompressing:
            actionStr = "compressing";
            break;
        case kNuProgressStoring:
            actionStr = "storing    ";
            break;
        default:
            actionStr = "??????     ";
            break;
        }
        break;
    case kNuOpExtract:
        switch (pProgress->state) {
        case kNuProgressPreparing:
        case kNuProgressOpening:
            actionStr = "extracting";
            showName = true;
            break;
        case kNuProgressExpanding:
            actionStr = "expanding ";
            break;
        case kNuProgressCopying:
            actionStr = "extracting";
            break;
        default:
            actionStr = "??????    ";
            break;
        }
        if (pProgress->expand.convertEOL == kNuConvertOn)
            eolConv = true;
        break;
    case kNuOpTest:
        switch (pProgress->state) {
        case kNuProgressPreparing:
        case kNuProgressOpening:
            showName = true;
            /* no break */
        case kNuProgressExpanding:
        case kNuProgressCopying:
            actionStr = "verifying";
            break;
        default:
            actionStr = "??????   ";
            break;
        }
        break;
    default:
        Assert(0);
        actionStr = "????";
    }

    switch (pProgress->state) {
    case kNuProgressDone:
        actionStr = NULL;
        percStr = "DONE\n";
        break;
    case kNuProgressSkipped:
        actionStr = NULL;
        percStr = "SKIP\n";
        break;
    case kNuProgressAborted:    /* not currently possible in NuLib2 */
        actionStr = NULL;
        percStr = "CNCL\n";
        break;
    case kNuProgressFailed:
        actionStr = NULL;
        percStr = "FAIL\n";
        break;
    default:
        break;
    }

    if (showName) {
        /*
         * Use "pathname" (whole thing) rather than "filename" (file part).
         * Could also use "origPathname", but I like to show what they're
         * getting instead of what they're giving.
         */
        int len = strlen(pProgress->pathnameUNI);
        if (len < sizeof(nameBuf)) {
            strcpy(nameBuf, pProgress->pathnameUNI);
        } else {
            nameBuf[0] = nameBuf[1] = '.';
            strncpy(nameBuf+2,
                pProgress->pathnameUNI + len - (sizeof(nameBuf)-3),
                sizeof(nameBuf)-3);
            nameBuf[sizeof(nameBuf)-1] = '\0';
        }
    }

    if (actionStr == NULL && percStr != NULL) {
        printf("\r%s", percStr);
    } else if (actionStr != NULL && percStr == NULL) {
        if (percStr == NULL) {
            putc('\r', stdout);
            PrintPercentage(pProgress->uncompressedLength,
                pProgress->uncompressedProgress);
            if (showName)
                printf(" %s%c %s", actionStr, eolConv ? '+' : ' ', nameBuf);
            else
                printf(" %s%c", actionStr, eolConv ? '+' : ' ');
        }
    } else {
        Assert(0);
        printf("????\n");
    }

    fflush(stdout);
    /*printf(" %ld \n", pProgress->uncompressedProgress);*/
    /*usleep(250000);*/

    return kNuOK;
}


/*
 * Decide whether or not to replace an existing file (during extract)
 * or record (during add).
 */
static NuResult HandleReplaceExisting(NulibState* pState, NuArchive* pArchive,
    const NuErrorStatus* pErrorStatus)
{
    NuResult result = kNuOK;
    char* renameName;
    char reply;

    Assert(pState != NULL);
    Assert(pErrorStatus != NULL);
    Assert(pErrorStatus->pathnameUNI != NULL);

    Assert(pErrorStatus->canOverwrite);
    Assert(pErrorStatus->canSkip);
    Assert(pErrorStatus->canAbort);

    if (NState_GetInputUnavailable(pState)) {
        putc('\n', stdout);
        ReportError(pErrorStatus->err, "Giving up");
        result = kNuAbort;
        goto bail;
    }

    while (1) {
        printf("\n  Replace %s?  [y]es, [n]o, [A]ll, [N]one",
            pErrorStatus->pathnameUNI);
        if (pErrorStatus->canRename)    /* renaming record adds not allowed */
            printf(", [r]ename: ");
        else
            printf(": ");
        fflush(stdout);

        reply = GetReplyChar('n');

        switch (reply) {
        case 'y':
            result = kNuOverwrite;
            goto bail;
        case 'n':
            result = kNuSkip;
            goto bail;
        case 'A':
            (void) NuSetValue(pArchive, kNuValueHandleExisting,
                    kNuAlwaysOverwrite);
            result = kNuOverwrite;
            goto bail;
        case 'N':
            (void) NuSetValue(pArchive, kNuValueHandleExisting,
                    kNuNeverOverwrite);
            result = kNuSkip;
            goto bail;
        case 'r':
            if (!pErrorStatus->canRename) {
                printf("Response not acceptable\n");
                break;  /* continue in "while" loop */
            }
            renameName = GetReplyString("New name: ");
            if (renameName == NULL)
                break;      /* continue in "while" loop */
            if (pErrorStatus->pRecord == NULL) {
                ReportError(kNuErrNone, "Unexpected NULL record");
                break;      /* continue in "while" loop */
            }
            NState_SetRenameFromIdx(pState,
                pErrorStatus->pRecord->recordIdx);
            NState_SetRenameToStr(pState, renameName);
            result = kNuRename;
            goto bail;
        case 'q':       /* stealth option to quit */
        case 'Q':
            result = kNuAbort;
            goto bail;
        default:
            printf("Response not understood -- please use y/n/A/N/r\n");
            break;      /* continue in "while" loop */
        }
    }

bail:
    return result;
}

/*
 * Found a bad CRC... should we press onward?
 *
 * Note pErrorStatus->pathname may be NULL if the error was found in the
 * master header or in the record header.
 */
static NuResult HandleBadCRC(NulibState* pState, NuArchive* pArchive,
    const NuErrorStatus* pErrorStatus)
{
    NuResult result = kNuOK;
    char reply;

    Assert(pState != NULL);
    Assert(pErrorStatus != NULL);

    if (NState_GetInputUnavailable(pState)) {
        putc('\n', stderr);
        ReportError(pErrorStatus->err, "Giving up");
        result = kNuAbort;
        goto bail;
    }

    while (1) {
        if (pErrorStatus->pathnameUNI != NULL)
            fprintf(stderr, "\n  Found a bad CRC in %s\n",
                pErrorStatus->pathnameUNI);
        else
            fprintf(stderr, "\n  Found a bad CRC in the archive\n");

        fprintf(stderr,
            "  Archive may be damaged, continue anyway?  [y]es, [n]o: ");
        fflush(stderr);

        reply = GetReplyChar('n');

        switch (reply) {
        case 'y':
            result = kNuIgnore;
            goto bail;
        case 'n':
        case 'N':
            result = kNuAbort;
            goto bail;
        default:
            fprintf(stderr, "Response not understood -- please use y/n\n");
            break;      /* continue in "while" loop */
        }
    }

bail:
    return result;
}

/*
 * Tried to add a nonexistent file; continue?
 *
 * To make this happen, you need to:
 *  - nulib2 aer lots-of-files another-file
 *  - remove another-file while nulib2 is still processing lots-of-files
 *
 * The trick is to delete a file explicitly mentioned on the command line
 * after NuFlush starts to do its thing.  Otherwise, we're just using
 * the system equivalent of readdir to scan a directory, so deleting a
 * file just means it won't get added.
 */
static NuResult HandleAddNotFound(NulibState* pState, NuArchive* pArchive,
    const NuErrorStatus* pErrorStatus)
{
    NuResult result = kNuOK;
    char reply;

    Assert(pState != NULL);
    Assert(pErrorStatus != NULL);
    Assert(pErrorStatus->pathnameUNI != NULL);

    if (NState_GetInputUnavailable(pState)) {
        putc('\n', stdout);
        ReportError(pErrorStatus->err, "Giving up");
        result = kNuAbort;
        goto bail;
    }

    while (1) {
        fprintf(stderr, "\n  Couldn't find %s, continue?  [y]es, [n]o: ",
            pErrorStatus->pathnameUNI);
        fflush(stderr);

        reply = GetReplyChar('n');

        switch (reply) {
        case 'y':
            result = kNuSkip;
            goto bail;
        case 'n':
        case 'N':
            result = kNuAbort;
            goto bail;
        default:
            fprintf(stderr, "Response not understood -- please use y/n\n");
            break;      /* continue in "while" loop */
        }
    }

bail:
    return result;
}

/*
 * Something failed, and the user may want to choose how to handle it.
 * Invoked as a callback.
 */
NuResult ErrorHandler(NuArchive* pArchive, void* vErrorStatus)
{
    const NuErrorStatus* pErrorStatus = vErrorStatus;
    NulibState* pState;
    NuResult result;

    Assert(pArchive != NULL);
    (void) NuGetExtraData(pArchive, (void**) &pState);
    Assert(pState != NULL);

    /* default action is to abort the current operation */
    result = kNuAbort;

    /*
     * When extracting, the error handler callback gets invoked for several
     * different problems because we might want to rename the file.  Also,
     * because extractions are done with "bulk" calls, returning an
     * individual error message would be meaningless.
     *
     * When adding files, the NuAddFile and NuAddRecord calls can return
     * immediate, specific results for a single add.  The only reasons for
     * calling here are to decide if an existing record should be replaced
     * or not (without even an option to rename), or to decide what to do
     * when the NuFlush call runs into a problem while adding a file.
     */
    if (pErrorStatus->operation == kNuOpExtract) {
        if (pErrorStatus->err == kNuErrFileExists) {
            result = HandleReplaceExisting(pState, pArchive, pErrorStatus);
        } else if (pErrorStatus->err == kNuErrNotNewer) {
            /* if we were expecting this, it's okay */
            if (NState_GetModFreshen(pState) || NState_GetModUpdate(pState)) {
                printf("\rSKIP\n");
                result = kNuSkip;
            } else {
                DBUG(("WEIRD one\n"));
            }
        } else if (pErrorStatus->err == kNuErrDuplicateNotFound) {
            /* if we were expecting this, it's okay */
            if (NState_GetModFreshen(pState)) {
                printf("\rSKIP\n");
                result = kNuSkip;
            } else {
                DBUG(("WEIRD two\n"));
            }
        }
    } else if (pErrorStatus->operation == kNuOpAdd) {
        if (pErrorStatus->err == kNuErrRecordExists) {
            /* if they want to update or freshen, don't hassle them */
            if (NState_GetModFreshen(pState) || NState_GetModUpdate(pState))
                result = kNuOverwrite;
            else
                result = HandleReplaceExisting(pState, pArchive, pErrorStatus);
        } else if (pErrorStatus->err == kNuErrFileNotFound) {
            /* file was specified with NuAdd but removed during NuFlush */
            result = HandleAddNotFound(pState, pArchive, pErrorStatus);
        }
    } else if (pErrorStatus->operation == kNuOpTest) {
        if (pErrorStatus->err == kNuErrBadMHCRC ||
            pErrorStatus->err == kNuErrBadRHCRC ||
            pErrorStatus->err == kNuErrBadThreadCRC ||
            pErrorStatus->err == kNuErrBadDataCRC)
        {
            result = HandleBadCRC(pState, pArchive, pErrorStatus);
        }
    }

    return result;
}


#if 0
/*
 * Display an error message.
 *
 * (This was just a test to see if it worked... NufxLib's default behavior
 * is fine for NuLib2.)
 */
NuResult ErrorMessageHandler(NuArchive* pArchive, void* vErrorMessage)
{
    const NuErrorMessage* pErrorMessage = (const NuErrorMessage*) vErrorMessage;

    fprintf(stderr, "%s%d %3d %s:%d %s %s\n",
        pArchive == NULL ? "(GLOBAL)" : "",
        pErrorMessage->isDebug, pErrorMessage->err, pErrorMessage->file,
        pErrorMessage->line, pErrorMessage->function, pErrorMessage->message);
    return kNuOK;
}
#endif


/*
 * ===========================================================================
 *      Open an archive
 * ===========================================================================
 */

/* an archive name of "-" indicates we want to use stdin */
static const char* kStdinArchive = "-";


/*
 * Determine whether the access bits on the record make it a read-only
 * file or not.
 *
 * Uses a simplified view of the access flags.
 */
Boolean IsRecordReadOnly(const NuRecord* pRecord)
{
    if (pRecord->recAccess == 0x21L || pRecord->recAccess == 0x01L)
        return true;
    else
        return false;
}


/*
 * Returns "true" if "archiveName" is the name we use to represent stdin.
 */
Boolean IsFilenameStdin(const char* archiveName)
{
    Assert(archiveName != NULL);
    return (strcmp(archiveName, kStdinArchive) == 0);
}


#define BailError(err) { if (err != kNuErrNone) goto bail; }

/*
 * Open the archive in read-only mode.  We use "file mode" for a file, or
 * "streaming mode" for stdin.
 */
NuError OpenArchiveReadOnly(NulibState* pState)
{
    NuError err;
    NuArchive* pArchive = NULL;

    Assert(pState != NULL);

    if (IsFilenameStdin(NState_GetArchiveFilename(pState))) {
        err = NuStreamOpenRO(stdin, &pArchive);
        if (err != kNuErrNone) {
            ReportError(err, "unable to open stdin archive");
            if (err == kNuErrIsBinary2)
                err = kNuErrNotNuFX;    /* we can't seek back, so forget BNY */
            goto bail;
        }
        /*
         * Since the archive is on stdin, we can't ask the user questions.
         * On a UNIX system we could open /dev/tty, but that's not portable,
         * and I don't think archives on stdin are going to be popular
         * enough to make this worth doing.
         */
        NState_SetInputUnavailable(pState, true);
    } else {
        err = NuOpenRO(NState_GetArchiveFilename(pState), &pArchive);
        if (err != kNuErrNone) {
            if (err != kNuErrIsBinary2) {
                ReportError(err, "unable to open '%s'",
                    NState_GetArchiveFilename(pState));
            }
            goto bail;
        }
    }

    /* introduce them */
    NState_SetNuArchive(pState, pArchive);
    err = NuSetExtraData(pArchive, pState);

    NuSetSelectionFilter(pArchive, SelectionFilter);
    NuSetOutputPathnameFilter(pArchive, OutputPathnameFilter);
    NuSetProgressUpdater(pArchive, ProgressUpdater);
    NuSetErrorHandler(pArchive, ErrorHandler);
    /*NuSetErrorMessageHandler(pArchive, ErrorMessageHandler);*/

    /* set the EOL conversion */
    if (NState_GetModConvertAll(pState))
        err = NuSetValue(pArchive, kNuValueConvertExtractedEOL, kNuConvertOn);
    else if (NState_GetModConvertText(pState))
        err = NuSetValue(pArchive, kNuValueConvertExtractedEOL, kNuConvertAuto);
    else
        err = NuSetValue(pArchive, kNuValueConvertExtractedEOL, kNuConvertOff);
    BailError(err);

    /* if we're converting EOL, we probably ought to do this too */
    err = NuSetValue(pArchive, kNuValueStripHighASCII, true);
    BailError(err);

    /* handle "-s" flag */
    if (NState_GetModOverwriteExisting(pState)) {
        err = NuSetValue(pArchive, kNuValueHandleExisting, kNuAlwaysOverwrite);
        BailError(err);
    }

    /* handle "-f" and "-u" flags (this overrides "-s" during extraction) */
    if (NState_GetModFreshen(pState) || NState_GetModUpdate(pState)) {
        err = NuSetValue(pArchive, kNuValueOnlyUpdateOlder, true);
        BailError(err);
    }
    if (NState_GetModFreshen(pState)) {
        err = NuSetValue(pArchive, kNuValueHandleExisting, kNuMustOverwrite);
        BailError(err);
    }

    DBUG(("--- enabling ShrinkIt compatibility mode\n"));
    err = NuSetValue(pArchive, kNuValueMimicSHK, true);
    BailError(err);

    /* handy for some malformed archives */
    err = NuSetValue(pArchive, kNuValueHandleBadMac, true);
    BailError(err);

/*
    DBUG(("--- enabling 'mask dataless' mode\n"));
    err = NuSetValue(pArchive, kNuValueMaskDataless, true);
    BailError(err);
*/

    if (strcmp(SYSTEM_DEFAULT_EOL, "\r") == 0)
        err = NuSetValue(pArchive, kNuValueEOL, kNuEOLCR);
    else if (strcmp(SYSTEM_DEFAULT_EOL, "\n") == 0)
        err = NuSetValue(pArchive, kNuValueEOL, kNuEOLLF);
    else if (strcmp(SYSTEM_DEFAULT_EOL, "\r\n") == 0)
        err = NuSetValue(pArchive, kNuValueEOL, kNuEOLCRLF);
    else {
        Assert(0);
        err = kNuErrInternal;
        ReportError(err, "Unknown SYSTEM_DEFAULT_EOL '%s'", SYSTEM_DEFAULT_EOL);
        goto bail;
    }
    BailError(err);

bail:
    if (err != kNuErrNone && pArchive != NULL) {
        /* clean up */
        (void) NuClose(pArchive);
        NState_SetNuArchive(pState, NULL);
    }
    return err;
}


/*
 * Open the archive in read-write mode, for purposes of adding, deleting,
 * or updating files.  We don't plan on extracting anything with this.
 *
 * "Streaming mode" isn't allowed.
 */
NuError OpenArchiveReadWrite(NulibState* pState)
{
    NuError err = kNuErrNone;
    NuArchive* pArchive = NULL;
    char* tempName = NULL;

    Assert(pState != NULL);
    Assert(IsFilenameStdin(NState_GetArchiveFilename(pState)) == false);

    tempName = MakeTempArchiveName(pState);
    if (tempName == NULL)
        goto bail;
    DBUG(("TEMP NAME = '%s'\n", tempName));

    err = NuOpenRW(NState_GetArchiveFilename(pState), tempName,
            kNuOpenCreat, &pArchive);
    if (err != kNuErrNone) {
        ReportError(err, "unable to open '%s'",
            NState_GetArchiveFilename(pState));
        goto bail;
    }

    /* introduce them */
    NState_SetNuArchive(pState, pArchive);
    err = NuSetExtraData(pArchive, pState);
    BailError(err);

    NuSetSelectionFilter(pArchive, SelectionFilter);
    NuSetProgressUpdater(pArchive, ProgressUpdater);
    NuSetErrorHandler(pArchive, ErrorHandler);
    /*NuSetErrorMessageHandler(pArchive, ErrorMessageHandler);*/

    /* handle "-0" flag */
    if (NState_GetModNoCompression(pState)) {
        err = NuSetValue(pArchive, kNuValueDataCompression, kNuCompressNone);
        BailError(err);
    }
    /* handle "-z" flag */
    if (NState_GetModCompressDeflate(pState)) {
        err = NuSetValue(pArchive, kNuValueDataCompression, kNuCompressDeflate);
        BailError(err);
    }
    /* handle "-zz" flag */
    if (NState_GetModCompressBzip2(pState)) {
        err = NuSetValue(pArchive, kNuValueDataCompression, kNuCompressBzip2);
        BailError(err);
    }

    /* handle "-f" and "-u" flags */
    /* (BUG: if "-f" is set, creating a new archive is impossible) */
    if (NState_GetModFreshen(pState) || NState_GetModUpdate(pState)) {
        err = NuSetValue(pArchive, kNuValueOnlyUpdateOlder, true);
        BailError(err);
    }
    if (NState_GetModFreshen(pState)) {
        err = NuSetValue(pArchive, kNuValueHandleExisting, kNuMustOverwrite);
        BailError(err);
    }

    DBUG(("--- enabling ShrinkIt compatibility mode\n"));
    err = NuSetValue(pArchive, kNuValueMimicSHK, true);
    BailError(err);

    /* this probably isn't needed here, but set it anyway */
    if (strcmp(SYSTEM_DEFAULT_EOL, "\r") == 0)
        err = NuSetValue(pArchive, kNuValueEOL, kNuEOLCR);
    else if (strcmp(SYSTEM_DEFAULT_EOL, "\n") == 0)
        err = NuSetValue(pArchive, kNuValueEOL, kNuEOLLF);
    else if (strcmp(SYSTEM_DEFAULT_EOL, "\r\n") == 0)
        err = NuSetValue(pArchive, kNuValueEOL, kNuEOLCRLF);
    else {
        Assert(0);
        err = kNuErrInternal;
        ReportError(err, "Unknown SYSTEM_DEFAULT_EOL '%s'", SYSTEM_DEFAULT_EOL);
        goto bail;
    }
    BailError(err);

    /*(void) NuSetValue(pArchive, kNuValueAllowDuplicates, true);*/

bail:
    Free(tempName);
    if (err != kNuErrNone && pArchive != NULL) {
        /* clean up */
        NuAbort(pArchive);
        (void) NuClose(pArchive);
        NState_SetNuArchive(pState, NULL);
    }
    return err;
}

