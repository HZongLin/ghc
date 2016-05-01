/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1998-2000
 *
 * Support for profiling
 *
 * ---------------------------------------------------------------------------*/

#ifdef PROFILING

#include "PosixSource.h"
#include "Rts.h"

#include "RtsUtils.h"
#include "Profiling.h"
#include "Proftimer.h"
#include "ProfHeap.h"
#include "Arena.h"
#include "RetainerProfile.h"
#include "Printer.h"
#include "Capability.h"

#include <string.h>

#ifdef DEBUG
#include "Trace.h"
#endif

/*
 * Profiling allocation arena.
 */
static Arena *prof_arena;

/*
 * Global variables used to assign unique IDs to cc's, ccs's, and
 * closure_cats
 */

unsigned int CC_ID  = 1;
unsigned int CCS_ID = 1;

/* figures for the profiling report.
 */
static StgWord64 total_alloc;
static W_      total_prof_ticks;

/* Globals for opening the profiling log file(s)
 */
static char *prof_filename; /* prof report file name = <program>.prof */
FILE *prof_file;

static char *hp_filename;       /* heap profile (hp2ps style) log file */
FILE *hp_file;

/* Linked lists to keep track of CCs and CCSs that haven't
 * been declared in the log file yet
 */
CostCentre      *CC_LIST  = NULL;
CostCentreStack *CCS_LIST = NULL;

#ifdef THREADED_RTS
static Mutex ccs_mutex;
#endif

/*
 * Built-in cost centres and cost-centre stacks:
 *
 *    MAIN   is the root of the cost-centre stack tree.  If there are
 *           no {-# SCC #-}s in the program, all costs will be attributed
 *           to MAIN.
 *
 *    SYSTEM is the RTS in general (scheduler, etc.).  All costs for
 *           RTS operations apart from garbage collection are attributed
 *           to SYSTEM.
 *
 *    GC     is the storage manager / garbage collector.
 *
 *    OVERHEAD gets all costs generated by the profiling system
 *           itself.  These are costs that would not be incurred
 *           during non-profiled execution of the program.
 *
 *    DONT_CARE is a placeholder cost-centre we assign to static
 *           constructors.  It should *never* accumulate any costs.
 *
 *    PINNED accumulates memory allocated to pinned objects, which
 *           cannot be profiled separately because we cannot reliably
 *           traverse pinned memory.
 */

CC_DECLARE(CC_MAIN,      "MAIN",        "MAIN",      "<built-in>", CC_NOT_CAF, );
CC_DECLARE(CC_SYSTEM,    "SYSTEM",      "SYSTEM",    "<built-in>", CC_NOT_CAF, );
CC_DECLARE(CC_GC,        "GC",          "GC",        "<built-in>", CC_NOT_CAF, );
CC_DECLARE(CC_OVERHEAD,  "OVERHEAD_of", "PROFILING", "<built-in>", CC_NOT_CAF, );
CC_DECLARE(CC_DONT_CARE, "DONT_CARE",   "MAIN",      "<built-in>", CC_NOT_CAF, );
CC_DECLARE(CC_PINNED,    "PINNED",      "SYSTEM",    "<built-in>", CC_NOT_CAF, );
CC_DECLARE(CC_IDLE,      "IDLE",        "IDLE",      "<built-in>", CC_NOT_CAF, );

CCS_DECLARE(CCS_MAIN,       CC_MAIN,       );
CCS_DECLARE(CCS_SYSTEM,     CC_SYSTEM,     );
CCS_DECLARE(CCS_GC,         CC_GC,         );
CCS_DECLARE(CCS_OVERHEAD,   CC_OVERHEAD,   );
CCS_DECLARE(CCS_DONT_CARE,  CC_DONT_CARE,  );
CCS_DECLARE(CCS_PINNED,     CC_PINNED,     );
CCS_DECLARE(CCS_IDLE,       CC_IDLE,       );

/*
 * Static Functions
 */

static  CostCentreStack * appendCCS       ( CostCentreStack *ccs1,
                                            CostCentreStack *ccs2 );
static  CostCentreStack * actualPush_     ( CostCentreStack *ccs, CostCentre *cc,
                                            CostCentreStack *new_ccs );
static  rtsBool           ignoreCCS       ( CostCentreStack *ccs );
static  void              countTickss     ( CostCentreStack *ccs );
static  void              inheritCosts    ( CostCentreStack *ccs );
static  uint32_t           numDigits       ( StgInt i );
static  void              findCCSMaxLens  ( CostCentreStack *ccs,
                                            uint32_t indent,
                                            uint32_t *max_label_len,
                                            uint32_t *max_module_len,
                                            uint32_t *max_id_len );
static  void              logCCS          ( CostCentreStack *ccs,
                                            uint32_t indent,
                                            uint32_t max_label_len,
                                            uint32_t max_module_len,
                                            uint32_t max_id_len );
static  void              reportCCS       ( CostCentreStack *ccs );
static  CostCentreStack * checkLoop       ( CostCentreStack *ccs,
                                            CostCentre *cc );
static  CostCentreStack * pruneCCSTree    ( CostCentreStack *ccs );
static  CostCentreStack * actualPush      ( CostCentreStack *, CostCentre * );
static  CostCentreStack * isInIndexTable  ( IndexTable *, CostCentre * );
static  IndexTable *      addToIndexTable ( IndexTable *, CostCentreStack *,
                                            CostCentre *, unsigned int );
static  void              ccsSetSelected  ( CostCentreStack *ccs );

static  void              initTimeProfiling    ( void );
static  void              initProfilingLogFile ( void );

/* -----------------------------------------------------------------------------
   Initialise the profiling environment
   -------------------------------------------------------------------------- */

void initProfiling (void)
{
    // initialise our arena
    prof_arena = newArena();

    /* for the benefit of allocate()... */
    {
        uint32_t n;
        for (n=0; n < n_capabilities; n++) {
            capabilities[n]->r.rCCCS = CCS_SYSTEM;
        }
    }

#ifdef THREADED_RTS
    initMutex(&ccs_mutex);
#endif

    /* Set up the log file, and dump the header and cost centre
     * information into it.
     */
    initProfilingLogFile();

    /* Register all the cost centres / stacks in the program
     * CC_MAIN gets link = 0, all others have non-zero link.
     */
    REGISTER_CC(CC_MAIN);
    REGISTER_CC(CC_SYSTEM);
    REGISTER_CC(CC_GC);
    REGISTER_CC(CC_OVERHEAD);
    REGISTER_CC(CC_DONT_CARE);
    REGISTER_CC(CC_PINNED);
    REGISTER_CC(CC_IDLE);

    REGISTER_CCS(CCS_SYSTEM);
    REGISTER_CCS(CCS_GC);
    REGISTER_CCS(CCS_OVERHEAD);
    REGISTER_CCS(CCS_DONT_CARE);
    REGISTER_CCS(CCS_PINNED);
    REGISTER_CCS(CCS_IDLE);
    REGISTER_CCS(CCS_MAIN);

    /* find all the registered cost centre stacks, and make them
     * children of CCS_MAIN.
     */
    ASSERT(CCS_LIST == CCS_MAIN);
    CCS_LIST = CCS_LIST->prevStack;
    CCS_MAIN->prevStack = NULL;
    CCS_MAIN->root = CCS_MAIN;
    ccsSetSelected(CCS_MAIN);

    initProfiling2();

    if (RtsFlags.CcFlags.doCostCentres) {
        initTimeProfiling();
    }

    if (RtsFlags.ProfFlags.doHeapProfile) {
        initHeapProfiling();
    }
}

//
// Should be called after loading any new Haskell code.
//
void initProfiling2 (void)
{
    CostCentreStack *ccs, *next;

    // make CCS_MAIN the parent of all the pre-defined CCSs.
    for (ccs = CCS_LIST; ccs != NULL; ) {
        next = ccs->prevStack;
        ccs->prevStack = NULL;
        actualPush_(CCS_MAIN,ccs->cc,ccs);
        ccs->root = ccs;
        ccs = next;
    }
    CCS_LIST = NULL;
}

void
freeProfiling (void)
{
    arenaFree(prof_arena);
}

CostCentre *mkCostCentre (char *label, char *module, char *srcloc)
{
    CostCentre *cc = stgMallocBytes (sizeof(CostCentre), "mkCostCentre");
    cc->label = label;
    cc->module = module;
    cc->srcloc = srcloc;
    return cc;
}

static void
initProfilingLogFile(void)
{
    char *prog;

    prog = arenaAlloc(prof_arena, strlen(prog_name) + 1);
    strcpy(prog, prog_name);
#ifdef mingw32_HOST_OS
    // on Windows, drop the .exe suffix if there is one
    {
        char *suff;
        suff = strrchr(prog,'.');
        if (suff != NULL && !strcmp(suff,".exe")) {
            *suff = '\0';
        }
    }
#endif

    if (RtsFlags.CcFlags.doCostCentres == 0 && !doingRetainerProfiling())
    {
        /* No need for the <prog>.prof file */
        prof_filename = NULL;
        prof_file = NULL;
    }
    else
    {
        /* Initialise the log file name */
        prof_filename = arenaAlloc(prof_arena, strlen(prog) + 6);
        sprintf(prof_filename, "%s.prof", prog);

        /* open the log file */
        if ((prof_file = fopen(prof_filename, "w")) == NULL) {
            debugBelch("Can't open profiling report file %s\n", prof_filename);
            RtsFlags.CcFlags.doCostCentres = 0;
            // Retainer profiling (`-hr` or `-hr<cc> -h<x>`) writes to
            // both <program>.hp as <program>.prof.
            if (doingRetainerProfiling()) {
                RtsFlags.ProfFlags.doHeapProfile = 0;
            }
        }
    }

    if (RtsFlags.ProfFlags.doHeapProfile) {
        /* Initialise the log file name */
        hp_filename = arenaAlloc(prof_arena, strlen(prog) + 6);
        sprintf(hp_filename, "%s.hp", prog);

        /* open the log file */
        if ((hp_file = fopen(hp_filename, "w")) == NULL) {
            debugBelch("Can't open profiling report file %s\n",
                    hp_filename);
            RtsFlags.ProfFlags.doHeapProfile = 0;
        }
    }
}

void
initTimeProfiling(void)
{
    /* Start ticking */
    startProfTimer();
};

void
endProfiling ( void )
{
    if (RtsFlags.CcFlags.doCostCentres) {
        stopProfTimer();
    }
    if (RtsFlags.ProfFlags.doHeapProfile) {
        endHeapProfiling();
    }
}

/* -----------------------------------------------------------------------------
   Set CCCS when entering a function.

   The algorithm is as follows.

     ccs ++> ccsfn  =  ccs ++ dropCommonPrefix ccs ccsfn

   where

     dropCommonPrefix A B
        -- returns the suffix of B after removing any prefix common
        -- to both A and B.

   e.g.

     <a,b,c> ++> <>      = <a,b,c>
     <a,b,c> ++> <d>     = <a,b,c,d>
     <a,b,c> ++> <a,b>   = <a,b,c>
     <a,b>   ++> <a,b,c> = <a,b,c>
     <a,b,c> ++> <a,b,d> = <a,b,c,d>

   -------------------------------------------------------------------------- */

// implements  c1 ++> c2,  where c1 and c2 are equal depth
//
static CostCentreStack *
enterFunEqualStacks (CostCentreStack *ccs0,
                     CostCentreStack *ccsapp,
                     CostCentreStack *ccsfn)
{
    ASSERT(ccsapp->depth == ccsfn->depth);
    if (ccsapp == ccsfn) return ccs0;
    return pushCostCentre(enterFunEqualStacks(ccs0,
                                              ccsapp->prevStack,
                                              ccsfn->prevStack),
                          ccsfn->cc);
}

// implements  c1 ++> c2,  where c2 is deeper than c1.
// Drop elements of c2 until we have equal stacks, call
// enterFunEqualStacks(), and then push on the elements that we
// dropped in reverse order.
//
static CostCentreStack *
enterFunCurShorter (CostCentreStack *ccsapp, CostCentreStack *ccsfn, StgWord n)
{
    if (n == 0) {
        ASSERT(ccsfn->depth == ccsapp->depth);
        return enterFunEqualStacks(ccsapp,ccsapp,ccsfn);;
    } else {
        ASSERT(ccsfn->depth > ccsapp->depth);
        return pushCostCentre(enterFunCurShorter(ccsapp, ccsfn->prevStack, n-1),
                              ccsfn->cc);
    }
}

void enterFunCCS (StgRegTable *reg, CostCentreStack *ccsfn)
{
    CostCentreStack *ccsapp;

    // common case 1: both stacks are the same
    if (ccsfn == reg->rCCCS) {
        return;
    }

    // common case 2: the function stack is empty, or just CAF
    if (ccsfn->prevStack == CCS_MAIN) {
        return;
    }

    ccsapp = reg->rCCCS;
    reg->rCCCS = CCS_OVERHEAD;

    // common case 3: the stacks are completely different (e.g. one is a
    // descendent of MAIN and the other of a CAF): we append the whole
    // of the function stack to the current CCS.
    if (ccsfn->root != ccsapp->root) {
        reg->rCCCS = appendCCS(ccsapp,ccsfn);
        return;
    }

    // uncommon case 4: ccsapp is deeper than ccsfn
    if (ccsapp->depth > ccsfn->depth) {
        uint32_t i, n;
        CostCentreStack *tmp = ccsapp;
        n = ccsapp->depth - ccsfn->depth;
        for (i = 0; i < n; i++) {
            tmp = tmp->prevStack;
        }
        reg->rCCCS = enterFunEqualStacks(ccsapp,tmp,ccsfn);
        return;
    }

    // uncommon case 5: ccsfn is deeper than CCCS
    if (ccsfn->depth > ccsapp->depth) {
        reg->rCCCS = enterFunCurShorter(ccsapp, ccsfn,
                                        ccsfn->depth - ccsapp->depth);
        return;
    }

    // uncommon case 6: stacks are equal depth, but different
    reg->rCCCS = enterFunEqualStacks(ccsapp,ccsapp,ccsfn);
}

/* -----------------------------------------------------------------------------
   Decide whether closures with this CCS should contribute to the heap
   profile.
   -------------------------------------------------------------------------- */

static void
ccsSetSelected (CostCentreStack *ccs)
{
    if (RtsFlags.ProfFlags.modSelector) {
        if (! strMatchesSelector (ccs->cc->module,
                                  RtsFlags.ProfFlags.modSelector) ) {
            ccs->selected = 0;
            return;
        }
    }
    if (RtsFlags.ProfFlags.ccSelector) {
        if (! strMatchesSelector (ccs->cc->label,
                                  RtsFlags.ProfFlags.ccSelector) ) {
            ccs->selected = 0;
            return;
        }
    }
    if (RtsFlags.ProfFlags.ccsSelector) {
        CostCentreStack *c;
        for (c = ccs; c != NULL; c = c->prevStack)
        {
            if ( strMatchesSelector (c->cc->label,
                                     RtsFlags.ProfFlags.ccsSelector) ) {
                break;
            }
        }
        if (c == NULL) {
            ccs->selected = 0;
            return;
        }
    }

    ccs->selected = 1;
    return;
}

/* -----------------------------------------------------------------------------
   Cost-centre stack manipulation
   -------------------------------------------------------------------------- */

#ifdef DEBUG
CostCentreStack * _pushCostCentre ( CostCentreStack *ccs, CostCentre *cc );
CostCentreStack *
pushCostCentre ( CostCentreStack *ccs, CostCentre *cc )
#define pushCostCentre _pushCostCentre
{
    IF_DEBUG(prof,
             traceBegin("pushing %s on ", cc->label);
             debugCCS(ccs);
             traceEnd(););

    return pushCostCentre(ccs,cc);
}
#endif

/* Append ccs1 to ccs2 (ignoring any CAF cost centre at the root of ccs1 */

#ifdef DEBUG
CostCentreStack *_appendCCS ( CostCentreStack *ccs1, CostCentreStack *ccs2 );
CostCentreStack *
appendCCS ( CostCentreStack *ccs1, CostCentreStack *ccs2 )
#define appendCCS _appendCCS
{
  IF_DEBUG(prof,
          if (ccs1 != ccs2) {
            debugBelch("Appending ");
            debugCCS(ccs1);
            debugBelch(" to ");
            debugCCS(ccs2);
            debugBelch("\n");});
  return appendCCS(ccs1,ccs2);
}
#endif

CostCentreStack *
appendCCS ( CostCentreStack *ccs1, CostCentreStack *ccs2 )
{
    if (ccs1 == ccs2) {
        return ccs1;
    }

    if (ccs2 == CCS_MAIN || ccs2->cc->is_caf == CC_IS_CAF) {
        // stop at a CAF element
        return ccs1;
    }

    return pushCostCentre(appendCCS(ccs1, ccs2->prevStack), ccs2->cc);
}

// Pick one:
// #define RECURSION_DROPS
#define RECURSION_TRUNCATES

CostCentreStack *
pushCostCentre (CostCentreStack *ccs, CostCentre *cc)
{
    CostCentreStack *temp_ccs, *ret;
    IndexTable *ixtable;

    if (ccs == EMPTY_STACK) {
        ACQUIRE_LOCK(&ccs_mutex);
        ret = actualPush(ccs,cc);
    }
    else
    {
        if (ccs->cc == cc) {
            return ccs;
        } else {
            // check if we've already memoized this stack
            ixtable = ccs->indexTable;
            temp_ccs = isInIndexTable(ixtable,cc);

            if (temp_ccs != EMPTY_STACK) {
                return temp_ccs;
            } else {

                // not in the IndexTable, now we take the lock:
                ACQUIRE_LOCK(&ccs_mutex);

                if (ccs->indexTable != ixtable)
                {
                    // someone modified ccs->indexTable while
                    // we did not hold the lock, so we must
                    // check it again:
                    temp_ccs = isInIndexTable(ixtable,cc);
                    if (temp_ccs != EMPTY_STACK)
                    {
                        RELEASE_LOCK(&ccs_mutex);
                        return temp_ccs;
                    }
                }
                temp_ccs = checkLoop(ccs,cc);
                if (temp_ccs != NULL) {
                    // This CC is already in the stack somewhere.
                    // This could be recursion, or just calling
                    // another function with the same CC.
                    // A number of policies are possible at this
                    // point, we implement two here:
                    //   - truncate the stack to the previous instance
                    //     of this CC
                    //   - ignore this push, return the same stack.
                    //
                    CostCentreStack *new_ccs;
#if defined(RECURSION_TRUNCATES)
                    new_ccs = temp_ccs;
#else // defined(RECURSION_DROPS)
                    new_ccs = ccs;
#endif
                    ccs->indexTable = addToIndexTable (ccs->indexTable,
                                                       new_ccs, cc, 1);
                    ret = new_ccs;
                } else {
                    ret = actualPush (ccs,cc);
                }
            }
        }
    }

    RELEASE_LOCK(&ccs_mutex);
    return ret;
}

static CostCentreStack *
checkLoop (CostCentreStack *ccs, CostCentre *cc)
{
    while (ccs != EMPTY_STACK) {
        if (ccs->cc == cc)
            return ccs;
        ccs = ccs->prevStack;
    }
    return NULL;
}

static CostCentreStack *
actualPush (CostCentreStack *ccs, CostCentre *cc)
{
    CostCentreStack *new_ccs;

    // allocate space for a new CostCentreStack
    new_ccs = (CostCentreStack *) arenaAlloc(prof_arena, sizeof(CostCentreStack));

    return actualPush_(ccs, cc, new_ccs);
}

static CostCentreStack *
actualPush_ (CostCentreStack *ccs, CostCentre *cc, CostCentreStack *new_ccs)
{
    /* assign values to each member of the structure */
    new_ccs->ccsID = CCS_ID++;
    new_ccs->cc = cc;
    new_ccs->prevStack = ccs;
    new_ccs->root = ccs->root;
    new_ccs->depth = ccs->depth + 1;

    new_ccs->indexTable = EMPTY_TABLE;

    /* Initialise the various _scc_ counters to zero
     */
    new_ccs->scc_count        = 0;

    /* Initialize all other stats here.  There should be a quick way
     * that's easily used elsewhere too
     */
    new_ccs->time_ticks = 0;
    new_ccs->mem_alloc = 0;
    new_ccs->inherited_ticks = 0;
    new_ccs->inherited_alloc = 0;

    // Set the selected field.
    ccsSetSelected(new_ccs);

    /* update the memoization table for the parent stack */
    ccs->indexTable = addToIndexTable(ccs->indexTable, new_ccs, cc,
                                      0/*not a back edge*/);

    /* return a pointer to the new stack */
    return new_ccs;
}


static CostCentreStack *
isInIndexTable(IndexTable *it, CostCentre *cc)
{
    while (it!=EMPTY_TABLE)
    {
        if (it->cc == cc)
            return it->ccs;
        else
            it = it->next;
    }

    /* otherwise we never found it so return EMPTY_TABLE */
    return EMPTY_TABLE;
}


static IndexTable *
addToIndexTable (IndexTable *it, CostCentreStack *new_ccs,
                 CostCentre *cc, unsigned int back_edge)
{
    IndexTable *new_it;

    new_it = arenaAlloc(prof_arena, sizeof(IndexTable));

    new_it->cc = cc;
    new_it->ccs = new_ccs;
    new_it->next = it;
    new_it->back_edge = back_edge;
    return new_it;
}

/* -----------------------------------------------------------------------------
   Generating a time & allocation profiling report.
   -------------------------------------------------------------------------- */

/* We omit certain system-related CCs and CCSs from the default
 * reports, so as not to cause confusion.
 */
static rtsBool
ignoreCC (CostCentre *cc)
{
    if (RtsFlags.CcFlags.doCostCentres < COST_CENTRES_ALL &&
        (   cc == CC_OVERHEAD
         || cc == CC_DONT_CARE
         || cc == CC_GC
         || cc == CC_SYSTEM
         || cc == CC_IDLE)) {
        return rtsTrue;
    } else {
        return rtsFalse;
    }
}

static rtsBool
ignoreCCS (CostCentreStack *ccs)
{
    if (RtsFlags.CcFlags.doCostCentres < COST_CENTRES_ALL &&
        (   ccs == CCS_OVERHEAD
         || ccs == CCS_DONT_CARE
         || ccs == CCS_GC
         || ccs == CCS_SYSTEM
         || ccs == CCS_IDLE)) {
        return rtsTrue;
    } else {
        return rtsFalse;
    }
}

/* -----------------------------------------------------------------------------
   Generating the aggregated per-cost-centre time/alloc report.
   -------------------------------------------------------------------------- */

static CostCentre *sorted_cc_list;

static void
aggregateCCCosts( CostCentreStack *ccs )
{
    IndexTable *i;

    ccs->cc->mem_alloc += ccs->mem_alloc;
    ccs->cc->time_ticks += ccs->time_ticks;

    for (i = ccs->indexTable; i != 0; i = i->next) {
        if (!i->back_edge) {
            aggregateCCCosts(i->ccs);
        }
    }
}

static void
insertCCInSortedList( CostCentre *new_cc )
{
    CostCentre **prev, *cc;

    prev = &sorted_cc_list;
    for (cc = sorted_cc_list; cc != NULL; cc = cc->link) {
        if (new_cc->time_ticks > cc->time_ticks) {
            new_cc->link = cc;
            *prev = new_cc;
            return;
        } else {
            prev = &(cc->link);
        }
    }
    new_cc->link = NULL;
    *prev = new_cc;
}

static uint32_t
strlen_utf8 (char *s)
{
    uint32_t n = 0;
    unsigned char c;

    for (; *s != '\0'; s++) {
        c = *s;
        if (c < 0x80 || c > 0xBF) n++;
    }
    return n;
}

static void
reportPerCCCosts( void )
{
    CostCentre *cc, *next;
    uint32_t max_label_len, max_module_len;

    aggregateCCCosts(CCS_MAIN);
    sorted_cc_list = NULL;

    max_label_len  = 11; // no shorter than the "COST CENTRE" header
    max_module_len = 6;  // no shorter than the "MODULE" header

    for (cc = CC_LIST; cc != NULL; cc = next) {
        next = cc->link;
        if (cc->time_ticks > total_prof_ticks/100
            || cc->mem_alloc > total_alloc/100
            || RtsFlags.CcFlags.doCostCentres >= COST_CENTRES_ALL) {
            insertCCInSortedList(cc);

            max_label_len = stg_max(strlen_utf8(cc->label), max_label_len);
            max_module_len = stg_max(strlen_utf8(cc->module), max_module_len);
        }
    }

    fprintf(prof_file, "%-*s %-*s", max_label_len, "COST CENTRE", max_module_len, "MODULE");
    fprintf(prof_file, " %6s %6s", "%time", "%alloc");
    if (RtsFlags.CcFlags.doCostCentres >= COST_CENTRES_VERBOSE) {
        fprintf(prof_file, "  %5s %9s", "ticks", "bytes");
    }
    fprintf(prof_file, "\n\n");

    for (cc = sorted_cc_list; cc != NULL; cc = cc->link) {
        if (ignoreCC(cc)) {
            continue;
        }
        fprintf(prof_file, "%s%*s %s%*s",
                cc->label,
                max_label_len - strlen_utf8(cc->label), "",
                cc->module,
                max_module_len - strlen_utf8(cc->module), "");

        fprintf(prof_file, " %6.1f %6.1f",
                total_prof_ticks == 0 ? 0.0 : (cc->time_ticks / (StgFloat) total_prof_ticks * 100),
                total_alloc == 0 ? 0.0 : (cc->mem_alloc / (StgFloat)
                                          total_alloc * 100)
            );

        if (RtsFlags.CcFlags.doCostCentres >= COST_CENTRES_VERBOSE) {
            fprintf(prof_file, "  %5" FMT_Word64 " %9" FMT_Word64,
                    (StgWord64)(cc->time_ticks), cc->mem_alloc*sizeof(W_));
        }
        fprintf(prof_file, "\n");
    }

    fprintf(prof_file,"\n\n");
}

/* -----------------------------------------------------------------------------
   Generate the cost-centre-stack time/alloc report
   -------------------------------------------------------------------------- */

static void
fprintHeader( uint32_t max_label_len, uint32_t max_module_len,
                uint32_t max_id_len )
{
    fprintf(prof_file, "%-*s %-*s %-*s %11s  %12s   %12s\n",
            max_label_len, "",
            max_module_len, "",
            max_id_len, "",
            "", "individual", "inherited");

    fprintf(prof_file, "%-*s %-*s %-*s",
            max_label_len, "COST CENTRE",
            max_module_len, "MODULE",
            max_id_len, "no.");

    fprintf(prof_file, " %11s  %5s %6s   %5s %6s",
            "entries", "%time", "%alloc", "%time", "%alloc");

    if (RtsFlags.CcFlags.doCostCentres >= COST_CENTRES_VERBOSE) {
        fprintf(prof_file, "  %5s %9s", "ticks", "bytes");
    }

    fprintf(prof_file, "\n\n");
}

void
reportCCSProfiling( void )
{
    uint32_t count;
    char temp[128]; /* sigh: magic constant */

    stopProfTimer();

    total_prof_ticks = 0;
    total_alloc = 0;
    countTickss(CCS_MAIN);

    if (RtsFlags.CcFlags.doCostCentres == 0) return;

    fprintf(prof_file, "\t%s Time and Allocation Profiling Report  (%s)\n",
            time_str(), "Final");

    fprintf(prof_file, "\n\t  ");
    fprintf(prof_file, " %s", prog_name);
    fprintf(prof_file, " +RTS");
    for (count = 0; rts_argv[count]; count++)
        fprintf(prof_file, " %s", rts_argv[count]);
    fprintf(prof_file, " -RTS");
    for (count = 1; prog_argv[count]; count++)
        fprintf(prof_file, " %s", prog_argv[count]);
    fprintf(prof_file, "\n\n");

    fprintf(prof_file, "\ttotal time  = %11.2f secs   (%lu ticks @ %d us, %d processor%s)\n",
            ((double) total_prof_ticks *
             (double) RtsFlags.MiscFlags.tickInterval) / (TIME_RESOLUTION * n_capabilities),
            (unsigned long) total_prof_ticks,
            (int) TimeToUS(RtsFlags.MiscFlags.tickInterval),
            n_capabilities, n_capabilities > 1 ? "s" : "");

    fprintf(prof_file, "\ttotal alloc = %11s bytes",
            showStgWord64(total_alloc * sizeof(W_),
                                 temp, rtsTrue/*commas*/));

    fprintf(prof_file, "  (excludes profiling overheads)\n\n");

    reportPerCCCosts();

    inheritCosts(CCS_MAIN);

    reportCCS(pruneCCSTree(CCS_MAIN));
}

static uint32_t
numDigits(StgInt i) {
    uint32_t result;

    result = 1;

    if (i < 0) i = 0;

    while (i > 9) {
        i /= 10;
        result++;
    }

    return result;
}

static void
findCCSMaxLens(CostCentreStack *ccs, uint32_t indent, uint32_t *max_label_len,
        uint32_t *max_module_len, uint32_t *max_id_len) {
    CostCentre *cc;
    IndexTable *i;

    cc = ccs->cc;

    *max_label_len = stg_max(*max_label_len, indent + strlen_utf8(cc->label));
    *max_module_len = stg_max(*max_module_len, strlen_utf8(cc->module));
    *max_id_len = stg_max(*max_id_len, numDigits(ccs->ccsID));

    for (i = ccs->indexTable; i != 0; i = i->next) {
        if (!i->back_edge) {
            findCCSMaxLens(i->ccs, indent+1,
                    max_label_len, max_module_len, max_id_len);
        }
    }
}

static void
logCCS(CostCentreStack *ccs, uint32_t indent,
        uint32_t max_label_len, uint32_t max_module_len, uint32_t max_id_len)
{
    CostCentre *cc;
    IndexTable *i;

    cc = ccs->cc;

    /* Only print cost centres with non 0 data ! */

    if (!ignoreCCS(ccs))
        /* force printing of *all* cost centres if -Pa */
    {

        fprintf(prof_file, "%-*s%s%*s %s%*s",
                indent, "",
                cc->label,
                max_label_len-indent - strlen_utf8(cc->label), "",
                cc->module,
                max_module_len - strlen_utf8(cc->module), "");

        fprintf(prof_file,
                " %*" FMT_Int "%11" FMT_Word64 "  %5.1f  %5.1f   %5.1f  %5.1f",
                max_id_len, ccs->ccsID, ccs->scc_count,
                total_prof_ticks == 0 ? 0.0 : ((double)ccs->time_ticks / (double)total_prof_ticks * 100.0),
                total_alloc == 0 ? 0.0 : ((double)ccs->mem_alloc / (double)total_alloc * 100.0),
                total_prof_ticks == 0 ? 0.0 : ((double)ccs->inherited_ticks / (double)total_prof_ticks * 100.0),
                total_alloc == 0 ? 0.0 : ((double)ccs->inherited_alloc / (double)total_alloc * 100.0)
            );

        if (RtsFlags.CcFlags.doCostCentres >= COST_CENTRES_VERBOSE) {
            fprintf(prof_file, "  %5" FMT_Word64 " %9" FMT_Word64,
                    (StgWord64)(ccs->time_ticks), ccs->mem_alloc*sizeof(W_));
        }
        fprintf(prof_file, "\n");
    }

    for (i = ccs->indexTable; i != 0; i = i->next) {
        if (!i->back_edge) {
            logCCS(i->ccs, indent+1, max_label_len, max_module_len, max_id_len);
        }
    }
}

static void
reportCCS(CostCentreStack *ccs)
{
    uint32_t max_label_len, max_module_len, max_id_len;

    max_label_len = 11; // no shorter than "COST CENTRE" header
    max_module_len = 6; // no shorter than "MODULE" header
    max_id_len = 3; // no shorter than "no." header

    findCCSMaxLens(ccs, 0, &max_label_len, &max_module_len, &max_id_len);

    fprintHeader(max_label_len, max_module_len, max_id_len);
    logCCS(ccs, 0, max_label_len, max_module_len, max_id_len);
}


/* Traverse the cost centre stack tree and accumulate
 * ticks/allocations.
 */
static void
countTickss(CostCentreStack *ccs)
{
    IndexTable *i;

    if (!ignoreCCS(ccs)) {
        total_alloc += ccs->mem_alloc;
        total_prof_ticks += ccs->time_ticks;
    }
    for (i = ccs->indexTable; i != NULL; i = i->next)
        if (!i->back_edge) {
            countTickss(i->ccs);
        }
}

/* Traverse the cost centre stack tree and inherit ticks & allocs.
 */
static void
inheritCosts(CostCentreStack *ccs)
{
    IndexTable *i;

    if (ignoreCCS(ccs)) { return; }

    ccs->inherited_ticks += ccs->time_ticks;
    ccs->inherited_alloc += ccs->mem_alloc;

    for (i = ccs->indexTable; i != NULL; i = i->next)
        if (!i->back_edge) {
            inheritCosts(i->ccs);
            ccs->inherited_ticks += i->ccs->inherited_ticks;
            ccs->inherited_alloc += i->ccs->inherited_alloc;
        }

    return;
}

//
// Prune CCSs with zero entries, zero ticks or zero allocation from
// the tree, unless COST_CENTRES_ALL is on.
//
static CostCentreStack *
pruneCCSTree (CostCentreStack *ccs)
{
    CostCentreStack *ccs1;
    IndexTable *i, **prev;

    prev = &ccs->indexTable;
    for (i = ccs->indexTable; i != 0; i = i->next) {
        if (i->back_edge) { continue; }

        ccs1 = pruneCCSTree(i->ccs);
        if (ccs1 == NULL) {
            *prev = i->next;
        } else {
            prev = &(i->next);
        }
    }

    if ( (RtsFlags.CcFlags.doCostCentres >= COST_CENTRES_ALL
          /* force printing of *all* cost centres if -P -P */ )

         || ( ccs->indexTable != 0 )
         || ( ccs->scc_count || ccs->time_ticks || ccs->mem_alloc )
        ) {
        return ccs;
    } else {
        return NULL;
    }
}

void
fprintCCS( FILE *f, CostCentreStack *ccs )
{
    fprintf(f,"<");
    for (; ccs && ccs != CCS_MAIN; ccs = ccs->prevStack ) {
        fprintf(f,"%s.%s", ccs->cc->module, ccs->cc->label);
        if (ccs->prevStack && ccs->prevStack != CCS_MAIN) {
            fprintf(f,",");
        }
    }
    fprintf(f,">");
}

// Returns: True if the call stack ended with CAF
static rtsBool fprintCallStack (CostCentreStack *ccs)
{
    CostCentreStack *prev;

    fprintf(stderr,"%s.%s", ccs->cc->module, ccs->cc->label);
    prev = ccs->prevStack;
    while (prev && prev != CCS_MAIN) {
        ccs = prev;
        fprintf(stderr, ",\n  called from %s.%s",
                ccs->cc->module, ccs->cc->label);
        prev = ccs->prevStack;
    }
    fprintf(stderr, "\n");

    return (!strncmp(ccs->cc->label, "CAF", 3));
}

/* For calling from .cmm code, where we can't reliably refer to stderr */
void
fprintCCS_stderr (CostCentreStack *ccs, StgClosure *exception, StgTSO *tso)
{
    rtsBool is_caf;
    StgPtr frame;
    StgStack *stack;
    CostCentreStack *prev_ccs;
    uint32_t depth = 0;
    const uint32_t MAX_DEPTH = 10; // don't print gigantic chains of stacks

    {
        char *desc;
        StgInfoTable *info;
        info = get_itbl(UNTAG_CLOSURE(exception));
        switch (info->type) {
        case CONSTR:
        case CONSTR_1_0:
        case CONSTR_0_1:
        case CONSTR_2_0:
        case CONSTR_1_1:
        case CONSTR_0_2:
        case CONSTR_STATIC:
        case CONSTR_NOCAF_STATIC:
            desc = GET_CON_DESC(itbl_to_con_itbl(info));
            break;
       default:
            desc = closure_type_names[info->type];
            break;
        }
        fprintf(stderr, "*** Exception (reporting due to +RTS -xc): (%s), stack trace: \n  ", desc);
    }

    is_caf = fprintCallStack(ccs);

    // traverse the stack down to the enclosing update frame to
    // find out where this CCS was evaluated from...

    stack = tso->stackobj;
    frame = stack->sp;
    prev_ccs = ccs;

    for (; is_caf && depth < MAX_DEPTH; depth++)
    {
        switch (get_itbl((StgClosure*)frame)->type)
        {
        case UPDATE_FRAME:
            ccs = ((StgUpdateFrame*)frame)->header.prof.ccs;
            frame += sizeofW(StgUpdateFrame);
            if (ccs == CCS_MAIN) {
                goto done;
            }
            if (ccs == prev_ccs) {
                // ignore if this is the same as the previous stack,
                // we're probably in library code and haven't
                // accumulated any more interesting stack items
                // since the last update frame.
                break;
            }
            prev_ccs = ccs;
            fprintf(stderr, "  --> evaluated by: ");
            is_caf = fprintCallStack(ccs);
            break;
        case UNDERFLOW_FRAME:
            stack = ((StgUnderflowFrame*)frame)->next_chunk;
            frame = stack->sp;
            break;
        case STOP_FRAME:
            goto done;
        default:
            frame += stack_frame_sizeW((StgClosure*)frame);
            break;
        }
    }
done:
    return;
}

#ifdef DEBUG
void
debugCCS( CostCentreStack *ccs )
{
    debugBelch("<");
    for (; ccs && ccs != CCS_MAIN; ccs = ccs->prevStack ) {
        debugBelch("%s.%s", ccs->cc->module, ccs->cc->label);
        if (ccs->prevStack && ccs->prevStack != CCS_MAIN) {
            debugBelch(",");
        }
    }
    debugBelch(">");
}
#endif /* DEBUG */

#endif /* PROFILING */
