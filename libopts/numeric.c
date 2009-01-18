
/*
 *  $Id: numeric.c,v 4.19 2008/11/02 18:51:26 bkorb Exp $
 *  Time-stamp:      "2008-11-01 14:28:56 bkorb"
 *
 *  This file is part of AutoOpts, a companion to AutoGen.
 *  AutoOpts is free software.
 *  AutoOpts is copyright (c) 1992-2008 by Bruce Korb - all rights reserved
 *  AutoOpts is copyright (c) 1992-2008 by Bruce Korb - all rights reserved
 *
 *  AutoOpts is available under any one of two licenses.  The license
 *  in use must be one of these two and the choice is under the control
 *  of the user of the license.
 *
 *   The GNU Lesser General Public License, version 3 or later
 *      See the files "COPYING.lgplv3" and "COPYING.gplv3"
 *
 *   The Modified Berkeley Software Distribution License
 *      See the file "COPYING.mbsd"
 *
 *  These files have the following md5sums:
 *
 *  239588c55c22c60ffe159946a760a33e pkg/libopts/COPYING.gplv3
 *  fa82ca978890795162346e661b47161a pkg/libopts/COPYING.lgplv3
 *  66a5cedaf62c4b2637025f049f9b826f pkg/libopts/COPYING.mbsd
 */

/*=export_func  optionShowRange
 * private:
 *
 * what:  
 * arg:   + tOptions* + pOpts     + program options descriptor  +
 * arg:   + tOptDesc* + pOptDesc  + the descriptor for this arg +
 * arg:   + void *    + rng_table + the value range tables      +
 * arg:   + int       + rng_count + the number of entries       +
 *
 * doc:
 *   Show information about a numeric option with range constraints.
=*/
void
optionShowRange(tOptions* pOpts, tOptDesc* pOD, void * rng_table, int rng_ct)
{
    const struct {long const rmin, rmax;} * rng = rng_table;
    char const * pz_indent =
        (pOpts != OPTPROC_EMIT_USAGE) ? "\t" : "\t\t\t\t  ";

    if ((pOpts == OPTPROC_EMIT_USAGE) || (pOpts > OPTPROC_EMIT_LIMIT)) {
        char const * lie_in_range = zRangeLie;

        if (pOpts > OPTPROC_EMIT_LIMIT) {
            fprintf(option_usage_fp, zRangeErr,
                    pOpts->pzProgName, pOD->pz_Name, pOD->optArg.argString);
            fprintf(option_usage_fp, "The %s option:\n", pOD->pz_Name);
            lie_in_range = zRangeBadLie;
            pz_indent = "";
        }

        if (pOD->fOptState & OPTST_SCALED_NUM)
            fprintf(option_usage_fp, zRangeScaled, pz_indent);

        if (rng_ct > 1)
            fprintf(option_usage_fp, lie_in_range, pz_indent);
        else {
            fprintf(option_usage_fp, zRangeOnly, pz_indent);
        }

        for (;;) {
            if (rng->rmax == LONG_MIN)
                fprintf(option_usage_fp, zRangeExact, pz_indent, rng->rmin);
            else if (rng->rmin == LONG_MIN)
                fprintf(option_usage_fp, zRangeUpto, pz_indent, rng->rmax);
            else if (rng->rmax == LONG_MAX)
                fprintf(option_usage_fp, zRangeAbove, pz_indent, rng->rmin);
            else
                fprintf(option_usage_fp, zRange, pz_indent, rng->rmin,
                        rng->rmax);

            if  (--rng_ct <= 0) {
                fputc('\n', option_usage_fp);
                break;
            }
            fputs(zRangeOr, option_usage_fp);
            rng++;
        }

        if (pOpts > OPTPROC_EMIT_LIMIT)
            pOpts->pUsageProc(pOpts, EXIT_FAILURE);
    }
}


/*=export_func  optionNumericVal
 * private:
 *
 * what:  process an option with a numeric value.
 * arg:   + tOptions* + pOpts    + program options descriptor +
 * arg:   + tOptDesc* + pOptDesc + the descriptor for this arg +
 *
 * doc:
 *  Decipher a numeric value.
=*/
void
optionNumericVal(tOptions* pOpts, tOptDesc* pOD )
{
    char* pz;
    long  val;

    if ((pOD->fOptState & OPTST_RESET) != 0)
        return;

    /*
     *  Numeric options may have a range associated with it.
     *  If it does, the usage procedure requests that it be
     *  emitted by passing a NULL pOD pointer.
     */
    if ((pOD == NULL) || (pOD->optArg.argString == NULL))
        return;

    errno = 0;
    val = strtol(pOD->optArg.argString, &pz, 0);
    if ((pz == pOD->optArg.argString) || (errno != 0))
        goto bad_number;

    if ((pOD->fOptState & OPTST_SCALED_NUM) != 0)
        switch (*(pz++)) {
        case '\0': pz--; break;
        case 't':  val *= 1000;
        case 'g':  val *= 1000;
        case 'm':  val *= 1000;
        case 'k':  val *= 1000; break;

        case 'T':  val *= 1024;
        case 'G':  val *= 1024;
        case 'M':  val *= 1024;
        case 'K':  val *= 1024; break;

        default:   goto bad_number;
        }

    if (*pz != NUL)
        goto bad_number;

    if (pOD->fOptState & OPTST_ALLOC_ARG) {
        AGFREE(pOD->optArg.argString);
        pOD->fOptState &= ~OPTST_ALLOC_ARG;
    }

    pOD->optArg.argInt = val;
    return;

bad_number:
    fprintf( stderr, zNotNumber, pOpts->pzProgName, pOD->optArg.argString );
    if ((pOpts->fOptSet & OPTPROC_ERRSTOP) != 0)
        (*(pOpts->pUsageProc))(pOpts, EXIT_FAILURE);

    pOD->optArg.argInt = ~0;
}

/*
 * Local Variables:
 * mode: C
 * c-file-style: "stroustrup"
 * indent-tabs-mode: nil
 * End:
 * end of autoopts/numeric.c */