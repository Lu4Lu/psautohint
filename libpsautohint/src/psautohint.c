/*
 * Copyright 2014 Adobe Systems Incorporated (http://www.adobe.com/).
 * All Rights Reserved.
 *
 * This software is licensed as OpenSource, under the Apache License, Version
 * 2.0.
 * This license is available at: http://opensource.org/licenses/Apache-2.0.
 */

#include <setjmp.h>

#include "ac.h"
#include "fontinfo.h"
#include "psautohint.h"
#include "version.h"

ACBuffer* gBezOutput = NULL;

static jmp_buf aclibmark; /* to handle exit() calls in the library version*/

static ACBuffer*
NewBuffer(size_t size)
{
    ACBuffer* buffer;

    if (size == 0)
        return NULL;

    buffer = (ACBuffer*)AllocateMem(1, sizeof(ACBuffer), "out buffer");
    if (!buffer)
        return NULL;

    buffer->data = AllocateMem(size, 1, "out buffer data");
    if (!buffer->data) {
        UnallocateMem(buffer);
        return NULL;
    }

    buffer->data[0] = '\0';
    buffer->capacity = size;
    buffer->length = 0;

    return buffer;
}

static void
FreeBuffer(ACBuffer* buffer)
{
    if (!buffer)
        return;

    UnallocateMem(buffer->data);
    UnallocateMem(buffer);
}

ACLIB_API void
AC_SetMemManager(void* ctxptr, AC_MEMMANAGEFUNCPTR func)
{
    setAC_memoryManager(ctxptr, func);
}

ACLIB_API void
AC_SetReportCB(AC_REPORTFUNCPTR reportCB)
{
    gLibReportCB = reportCB;
}

ACLIB_API void
AC_SetReportStemsCB(AC_REPORTSTEMPTR hstemCB, AC_REPORTSTEMPTR vstemCB,
                    unsigned int allStems)
{
    gAllStems = allStems;
    gAddHStemCB = hstemCB;
    gAddVStemCB = vstemCB;
    gDoStems = true;

    gAddGlyphExtremesCB = NULL;
    gAddStemExtremesCB = NULL;
    gDoAligns = false;
}

ACLIB_API void
AC_SetReportZonesCB(AC_REPORTZONEPTR glyphCB, AC_REPORTZONEPTR stemCB)
{
    gAddGlyphExtremesCB = glyphCB;
    gAddStemExtremesCB = stemCB;
    gDoAligns = true;

    gAddHStemCB = NULL;
    gAddVStemCB = NULL;
    gDoStems = false;
}

ACLIB_API void
AC_SetReportRetryCB(AC_RETRYPTR retryCB)
{
    gReportRetryCB = retryCB;
}

/*
 * This is our error handler, it gets called by LogMsg() whenever the log level
 * is LOGERROR (see logging.c for the exact condition). The call to longjmp()
 * will transfer the control to the point where setjmp() is called below. So
 * effectively whenever LogMsg() is called for an error the execution of the
 * calling function will end and we will return back to AutoHintString().
 */
static int
error_handler(int16_t code)
{
    if (code == FATALERROR || code == NONFATALERROR)
        longjmp(aclibmark, -1);
    else
        longjmp(aclibmark, 1);

    return 0; /* we don't actually ever get here */
}

ACLIB_API int
AutoHintString(const char* srcbezdata, const char* fontinfodata,
               char** dstbezdata, size_t* length, int allowEdit,
               int allowHintSub, int roundCoords)
{
    int value, result;
    ACFontInfo* fontinfo = NULL;

    if (!srcbezdata)
        return AC_InvalidParameterError;

    fontinfo = ParseFontInfo(fontinfodata);
    if (!fontinfo)
        return AC_MemoryError;

    set_errorproc(error_handler);
    value = setjmp(aclibmark);

    /* We will return here whenever an error occurs during the execution of
     * AutoHint(), or after it finishes execution. See the error_handler
     * comments above and below. */

    if (value == -1) {
        /* a fatal error occurred somewhere. */
        FreeFontInfo(fontinfo);
        return AC_FatalError;
    } else if (value == 1) {
        /* AutoHint was called successfully */
        FreeFontInfo(fontinfo);

        if (gBezOutput->length >= *length)
            *dstbezdata = ReallocateMem(*dstbezdata, gBezOutput->length + 1,
                                        "Output buffer");

        *length = gBezOutput->length + 1;
        strncpy(*dstbezdata, gBezOutput->data, *length);

        FreeBuffer(gBezOutput);

        return AC_Success;
    }

    gBezOutput = NewBuffer(*length);
    if (!gBezOutput) {
        FreeFontInfo(fontinfo);
        return AC_MemoryError;
    }

    result = AutoHint(fontinfo,     /* font info */
                      srcbezdata,   /* input glyph */
                      allowHintSub, /* extrahint */
                      allowEdit,    /* changeGlyphs */
                      roundCoords);
    /* result == true is good */

    /* The following call to error_handler() always returns control to just
     * after the setjmp() function call above, but with value set to 1 if
     * success, or -1 if not */
    error_handler((result == true) ? OK : NONFATALERROR);

    /* Shouldn't get here */
    return AC_UnknownError;
}

ACLIB_API int
AutoHintStringMM(const char** srcbezdata, const char* fontinfodata,
                 int nmasters, const char** masters, char** dstbezdata,
                 size_t* lengths)
{
    /* Only the master with index 'hintsMasterIx' needs to be hinted; this is
     * why only the fontinfo data for that master is needed. This function
     * expects that the master with index 'hintsMasterIx' has already been
     * hinted with AutoHint().
     *
     * The hints for the others masters are derived a very simple process. When
     * the first master was hinted, the logic recorded the path element index
     * for the path element which sets the inner or outer edge of each hint,
     * and whether it is the endpoint or startpoint which sets the edge. For
     * each other master, the logic gets the path element for the current
     * master using the same path element index as in the first master, and
     * uses the end or start point of that path element to set the edge in the
     * current master.
     *
     * Some code notes: The original hinting pass in AutoHintString() on the
     * first master records the path indicies for each path element that sets a
     * hint edge, and whether it is a start or end point; this stored in the
     * hintElt structures in the first master. There is a hintElt for the main
     * (default) hints at the start of the charstring, and there is hintElt
     * structure attached to each path element which triggers a new hint set.
     * The function charpath.c::ReadandAssignHints(), called from
     * charpath.c::MergeGlyphPaths(), then copies all the hintElts to the
     * current master main or path elements. (This actually happens in
     * charpath.c::InsertHint().) */
    int value, result;
    ACFontInfo* fontinfo = NULL;

    if (!srcbezdata)
        return AC_InvalidParameterError;

    fontinfo = ParseFontInfo(fontinfodata);
    if (!fontinfo)
        return AC_MemoryError;

    set_errorproc(error_handler);
    value = setjmp(aclibmark);

    /* We will return here whenever an error occurs during the execution of
     * AutoHint(), or after it finishes execution. See the error_handler
     * comments above and below. */

    if (value == -1) {
        /* a fatal error occurred somewhere. */
        FreeFontInfo(fontinfo);
        return AC_FatalError;
    } else if (value == 1) {
        /* AutoHint was called successfully */
        FreeFontInfo(fontinfo);
        return AC_Success;
    }

    /* result == true is good */
    result = MergeGlyphPaths(fontinfo, srcbezdata, nmasters, masters,
                             dstbezdata, lengths);

    /* The following call to error_handler() always returns control to just
     * after the setjmp() function call above, but with value set to 1 if
     * success, or -1 if not */
    error_handler((result == true) ? OK : NONFATALERROR);

    /* Shouldn't get here */
    return AC_UnknownError;
}

ACLIB_API void
AC_initCallGlobals(void)
{
    gLibReportCB = NULL;
    gAddGlyphExtremesCB = NULL;
    gAddStemExtremesCB = NULL;
    gDoAligns = false;
    gAddHStemCB = NULL;
    gAddVStemCB = NULL;
    gDoStems = false;
}

ACLIB_API const char*
AC_getVersion(void)
{
    return PSAUTOHINT_VERSION;
}
