#include "hclib.h"
#ifdef __cplusplus
#include "hclib_cpp.h"
#include "hclib_system.h"
// #include "hclib_openshmem.h"
#endif
/*
 *         ---- The Unbalanced Tree Search (UTS) Benchmark ----
 *  
 *  Copyright (c) 2010 See AUTHORS file for copyright holders
 *
 *  This file is part of the unbalanced tree search benchmark.  This
 *  project is licensed under the MIT Open Source license.  See the LICENSE
 *  file for copyright and licensing information.
 *
 *  UTS is a collaborative project between researchers at the University of
 *  Maryland, the University of North Carolina at Chapel Hill, and the Ohio
 *  State University.  See AUTHORS file for more information.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "uts.h"

/***********************************************************
 *                                                         *
 *  Compiler Type (these flags are set by at compile time) *
 *     (default) ANSI C compiler - sequential execution    *
 *     (_OPENMP) OpenMP enabled C compiler                 *
 *     (__UPC__) UPC compiler                              *
 *     (_SHMEM)  Cray Shmem                                *
 *     (__PTHREADS__) Pthreads multithreaded execution     *
 *                                                         *
 ***********************************************************/
#define GET_NUM_THREADS  hclib_num_workers()

/***********************************************************
 *  Parallel execution parameters                          *
 ***********************************************************/

int chunkSize = 20;       // number of nodes to move to/from shared area
int cbint     = 1;        // Cancellable barrier polling interval
int pollint   = 1;        // BUPC Polling interval

size_t n_nodes = 0;
size_t n_leaves = 0;

#ifdef THREAD_METADATA
typedef struct _thread_metadata {
    size_t ntasks;
} thread_metadata;
thread_metadata t_metadata[MAX_THREADS];
#endif

#ifdef __BERKELEY_UPC__
/* BUPC nonblocking I/O Handles */
bupc_handle_t cb_handle       = BUPC_COMPLETE_HANDLE;
#endif


/***********************************************************
 * Tree statistics (if selected via UTS_STAT)              *
 *   compute overall size and imbalance metrics            *
 *   and histogram size and imbalance per level            *
 ***********************************************************/
#ifdef UTS_STAT

/* Check that we are not being asked to compile parallel with stats.
 * Parallel stats collection is presently not supported.  */
#if PARALLEL
#error "ERROR: Parallel stats collection is not supported!"
#endif

#define MAXHISTSIZE      2000  // max tree depth in histogram
int    stats     = 1;
int    unbType   = 1;
int    maxHeight = 0;         // maximum depth of tree
double maxImb    = 0;         // maximum imbalance
double minImb    = 1;
double treeImb   =-1;         // Overall imbalance, undefined

int    hist[MAXHISTSIZE+1][2];      // average # nodes per level
double unbhist[MAXHISTSIZE+1][3];   // average imbalance per level

int    *rootSize;             // size of the root's children 
double *rootUnb;              // imbalance of root's children

/* Tseng statistics */
int    totalNodes = 0;
double imb_max    = 0;         // % of work in largest child (ranges from 100/n to 100%)
double imb_avg    = 0;
double imb_devmaxavg     = 0;  // ( % of work in largest child ) - ( avg work )
double imb_normdevmaxavg = 0;  // ( % of work in largest child ) - ( avg work ) / ( 100% - avg work )
#else
int stats   = 0;
int unbType = -1;
#endif


/***********************************************************
 *  Execution Tracing                                      *
 ***********************************************************/

#define SS_WORK    0
#define SS_SEARCH  1
#define SS_IDLE    2
#define SS_OVH     3
#define SS_CBOVH   4
#define SS_NSTATES 5

/* session record for session visualization */
struct sessionRecord_t {
  double startTime, endTime;
};
typedef struct sessionRecord_t SessionRecord;

/* steal record for steal visualization */
struct stealRecord_t {
  long int nodeCount;           /* count nodes generated during the session */
  int victimThread;             /* thread from which we stole the work  */
};
typedef struct stealRecord_t StealRecord;

/* Store debugging and trace data */
struct metaData_t {
  SessionRecord sessionRecords[SS_NSTATES][20000];   /* session time records */
  StealRecord stealRecords[20000]; /* steal records */
};
typedef struct metaData_t MetaData;

/* holds text string for debugging info */
char debug_str[1000];


/***********************************************************
 * StealStack types                                        *
 ***********************************************************/

/***********************************************************
 *  Global shared state                                    *
 ***********************************************************/

// termination detection 

/***********************************************************
 *  UTS Implementation Hooks                               *
 ***********************************************************/

// Return a string describing this implementation
char * impl_getName() {
    return "HCLIB";
}


// construct string with all parameter settings 
int impl_paramsToStr(char *strBuf, int ind) {
  ind += sprintf(strBuf+ind, "Execution strategy:  ");
      
  return ind;
}


int impl_parseParam(char *param, char *value) {
  int err = 0;  // Return 0 on a match, nonzero on an error

  switch (param[1]) {
#ifdef UTS_STAT
    case 'u':
      unbType = atoi(value); 
      if (unbType > 2) {
        err = 1;
        break;
      }
      if (unbType < 0)
        stats = 0;
      else
        stats = 1;
      break;
#endif
    default:
      err = 1;
      break;
  }

  return err;
}

void impl_helpMessage() {
}

void impl_abort(int err) {
#if defined(__UPC__)
  upc_global_exit(err);
#elif defined(_OPENMP)
  exit(err);
#elif defined(_SHMEM)
  exit(err);
#else
  exit(err);
#endif
}


/***********************************************************
 *                                                         *
 *  FUNCTIONS                                              *
 *                                                         *
 ***********************************************************/

/* 
 * StealStack
 *    Stack of nodes with sharing at the bottom of the stack
 *    and exclusive access at the top for the "owning" thread 
 *    which has affinity to the stack's address space.
 *
 *    * All operations on the shared portion of the stack
 *      must be guarded using the stack-specific lock.
 *    * Elements move between the shared and exclusive
 *      portion of the stack solely under control of the 
 *      owning thread. (ss_release and ss_acquire)
 *    * workAvail is the count of elements in the shared
 *      portion of the stack.  It may be read without 
 *      acquiring the stack lock, but of course its value
 *      may not be acurate.  Idle threads read workAvail in
 *      this speculative fashion to minimize overhead to 
 *      working threads.
 *    * Elements can be stolen from the bottom of the shared
 *      portion by non-owning threads.  The values are 
 *      reserved under lock by the stealing thread, and then 
 *      copied without use of the lock (currently space for
 *      reserved values is never reclaimed).
 *
 */

/* fatal error */
void ss_error(char *str) {
  printf("*** %s\n", str);
  exit(4);
}

#ifdef UTS_STAT
/*
 * Statistics, 
 * : number of nodes per level
 * : imbalanceness of nodes per level
 *
 */
void initHist()
{
  int i;
  for (i=0; i<MAXHISTSIZE; i++){
    hist[i][0]=0;
    hist[i][1]=0;
    unbhist[i][1]=1;
    unbhist[i][2]=0;
  }
}

void updateHist(Node* c, double unb)
{
  if (c->height<MAXHISTSIZE){
    hist[c->height][1]++;
    hist[c->height][0]+=c->numChildren;

    unbhist[c->height][0]+=unb;
    if (unbhist[c->height][1]>unb)
      unbhist[c->height][1]=unb;
    if (unbhist[c->height][2]<unb)
      unbhist[c->height][2]=unb;
		
  }
  else {
    hist[MAXHISTSIZE][1]++;
    hist[MAXHISTSIZE][0]+=c->numChildren;
  }
}

void showHist(FILE *fp)
{
  int i;	
  fprintf(fp, "depth\tavgNumChildren\t\tnumChildren\t imb\t maxImb\t minImb\t\n");
  for (i=0; i<MAXHISTSIZE; i++){
    if ((hist[i][0]!=0)&&(hist[i][1]!=0))
      fprintf(fp, "%d\t%f\t%d\t %lf\t%lf\t%lf\n", i, (double)hist[i][0]/hist[i][1], 
              hist[i][0], unbhist[i][0]/hist[i][1], unbhist[i][1], unbhist[i][2]);	
  }
}

double getImb(Node *c)
{
  int i=0;
  double avg=.0, tmp=.0;
  double unb=0.0;
  
  avg=(double)c->sizeChildren/c->numChildren;

  for (i=0; i<c->numChildren; i++){		
    if ((type==BIN)&&(c->pp==NULL))
      {
        if (unbType<2)
          tmp=min((double)rootSize[i]/avg, avg/(double)rootSize[i]);
        else 
          tmp=max((double)rootSize[i]/avg, avg/(double)rootSize[i]);
        
        if (unbType>0)
          unb+=tmp*rootUnb[i];
        else 
          unb+=tmp*rootUnb[i]*rootSize[i];
      }	
    else{
      if (unbType<2)
        tmp=min((double)c->size[i]/avg, avg/(double)c->size[i]);
      else 
        tmp=max((double)c->size[i]/avg, avg/(double)c->size[i]);
      
      if (unbType>0)
        unb+=tmp*c->unb[i];
      else 
        unb+=tmp*c->unb[i]*c->size[i];
    }
  }
	
  if (unbType>0){
    if (c->numChildren>0) 
      unb=unb/c->numChildren;
    else unb=1.0;
  }
  else {
    if (c->sizeChildren>1) 
      unb=unb/c->sizeChildren;
    else unb=1.0;
  }
  if ((debug & 1) && unb>1) printf("unb>1%lf\t%d\n", unb, c->numChildren);
	
  return unb;
}

void getImb_Tseng(Node *c)
{
  double t_max, t_avg, t_devmaxavg, t_normdevmaxavg;

  if (c->numChildren==0)
    {
      t_avg =0;
      t_max =0;
    }
  else 
    {
      t_max = (double)c->maxSizeChildren/(c->sizeChildren-1);
      t_avg = (double)1/c->numChildren;
    }

  t_devmaxavg = t_max-t_avg;
	
  if (debug & 1)
    printf("max\t%lf, %lf, %d, %d, %d\n", t_max, t_avg, 
           c->maxSizeChildren, c->sizeChildren, c->numChildren);
	
  if (1-t_avg==0)
    t_normdevmaxavg = 1;
  else
    t_normdevmaxavg = (t_max-t_avg)/(1-t_avg);

  imb_max += t_max;
  imb_avg += t_avg;
  imb_devmaxavg += t_devmaxavg;
  imb_normdevmaxavg +=t_normdevmaxavg;
}

void updateParStat(Node *c)
{
  double unb;

  totalNodes++;
  if (maxHeight<c->height) 
    maxHeight=c->height;
	
  unb=getImb(c);
  maxImb=max(unb, maxImb);
  minImb=min(unb, minImb);
  updateHist(c, unb);
  
  getImb_Tseng(c);
	
  if (c->pp!=NULL){
    if ((c->type==BIN)&&(c->pp->pp==NULL)){
      rootSize[c->pp->ind]=c->sizeChildren;
      rootUnb[c->pp->ind]=unb;
    }
    else{
      c->pp->size[c->pp->ind]=c->sizeChildren;
      c->pp->unb[c->pp->ind]=unb;
    }
    /* update statistics per node*/
    c->pp->ind++;
    c->pp->sizeChildren+=c->sizeChildren;
    if (c->pp->maxSizeChildren<c->sizeChildren)
      c->pp->maxSizeChildren=c->sizeChildren;		
  }
  else 
    treeImb = unb;
}
#endif

/*
 *	Tree Implementation      
 *
 */
void initNode(Node * child)
{
  child->type = -1;
  child->height = -1;
  child->numChildren = -1;    // not yet determined

#ifdef UTS_STAT
  if (stats){	
    int i;
    child->ind = 0;
    child->sizeChildren = 1;
    child->maxSizeChildren = 0;
    child->pp = NULL;
    for (i = 0; i < MAXNUMCHILDREN; i++){
      child->size[i] = 0;
      child->unb[i]  = 0.0;
    }
  }
#endif
}


void initRootNode(Node * root, int type)
{
  uts_initRoot(root, type);

  #ifdef TRACE
    stealStack[0]->md->stealRecords[0].victimThread = 0;  // first session is own "parent session"
  #endif

#ifdef UTS_STAT
  if (stats){
    int i;
    root->ind = 0;
    root->sizeChildren = 1;
    root->maxSizeChildren = 1;
    root->pp = NULL;
    
    if (type != BIN){
      for (i=0; i<MAXNUMCHILDREN; i++){
        root->size[i] = 0;
        root->unb[i]  =.0; 
      }
    }
    else {
      int rbf = (int) ceil(b_0);
      rootSize = malloc(rbf*sizeof(int));
      rootUnb = malloc(rbf*sizeof(double));
      for (i = 0; i < rbf; i++) {
        rootSize[i] = 0;
        rootUnb[i] = 0.0;
      }
    }
  }
#endif
}


/* 
 * Generate all children of the parent
 *
 * details depend on tree type, node type and shape function
 *
 */
typedef struct _pragma521_omp_task {
    Node parent;
    // int (*i_ptr);
    // int (*j_ptr);
    // unsigned char (*(*parent_state_ptr));
    // unsigned char (*(*child_state_ptr));
    // int (*parentHeight_ptr);
    // int (*numChildren_ptr);
    // int (*childType_ptr);
    // Node (*(*child_ptr));
 } pragma521_omp_task;

static void pragma521_omp_task_hclib_async(void *____arg);
void genChildren(Node * parent, Node * child) {
    int parentHeight = parent->height;
    int numChildren, childType;

#ifdef THREAD_METADATA
    t_metadata[omp_get_thread_num()].ntasks += 1;
#endif

    __sync_fetch_and_add(&(n_nodes), 1); ;

    numChildren = uts_numChildren(parent);
    childType   = uts_childType(parent);

    // fprintf(stderr, "numChildren = %d\n", numChildren);

    // record number of children in parent
    parent->numChildren = numChildren;

    // construct children and push onto stack
    if (numChildren > 0) {
        int i, j;
        child->type = childType;
        child->height = parentHeight + 1;

#ifdef UTS_STAT
        if (stats) {
            child->pp = parent;  // pointer to parent
        }
#endif

        unsigned char * parent_state = parent->state.state;
        unsigned char * child_state = child->state.state;

        for (i = 0; i < numChildren; i++) {
            for (j = 0; j < computeGranularity; j++) {
                // TBD:  add parent height to spawn
                // computeGranularity controls number of rng_spawn calls per node
                rng_spawn(parent_state, child_state, i);
            }

            Node parent = *child;

            { 
                pragma521_omp_task *new_ctx = (pragma521_omp_task *)malloc(sizeof(pragma521_omp_task));
                new_ctx->parent = parent;
                // new_ctx->i_ptr = &(i);
                // new_ctx->j_ptr = &(j);
                // new_ctx->parent_state_ptr = &(parent_state);
                // new_ctx->child_state_ptr = &(child_state);
                // new_ctx->parentHeight_ptr = &(parentHeight);
                // new_ctx->numChildren_ptr = &(numChildren);
                // new_ctx->childType_ptr = &(childType);
                // new_ctx->child_ptr = &(child);
                if (!(parent.height < 9)) {
                    pragma521_omp_task_hclib_async(new_ctx);
                } else {
                    // const size_t backlog = hclib_current_worker_backlog();
                    hclib_async(pragma521_omp_task_hclib_async, new_ctx, NO_FUTURE, ANY_PLACE);
                }
            } 
        }
    } else {
        __sync_fetch_and_add(&(n_leaves), 1); ;
    }
}

static inline void pragma521_omp_task_hclib_async(void *____arg) {
    pragma521_omp_task *ctx = (pragma521_omp_task *)____arg;
    Node parent; parent = ctx->parent;
    // hclib_start_finish();
    {
        Node child;
        initNode(&child);

        if (parent.numChildren < 0) {
            genChildren(&parent, &child);
        }
    } ;     ; // hclib_end_finish();

    free(____arg);
}




    
/*
 *  Parallel tree traversal
 *
 */

// cancellable barrier

// initialize lock:  single thread under omp, all threads under upc

//  delay this thread until all threads arrive at barrier
//     or until barrier is cancelled

// causes one or more threads waiting at barrier, if any,
//  to be released
#ifdef TRACE
// print session records for each thread (used when trace is enabled)
void printSessionRecords()
{
  int i, j, k;
  double offset;

  for (i = 0; i < GET_NUM_THREADS; i++) {
    offset = startTime[i] - startTime[0];

    for (j = 0; j < SS_NSTATES; j++)
       for (k = 0; k < stealStack[i]->entries[j]; k++) {
          printf ("%d %d %f %f", i, j,
            stealStack[i]->md->sessionRecords[j][k].startTime - offset,
            stealStack[i]->md->sessionRecords[j][k].endTime - offset);
          if (j == SS_WORK)
            printf (" %d %ld",
              stealStack[i]->md->stealRecords[k].victimThread,
              stealStack[i]->md->stealRecords[k].nodeCount);
            printf ("\n");
     }
  }
}
#endif

// display search statistics
void showStats(double elapsedSecs) {
  int i;
  int tnodes = 0, tleaves = 0, trel = 0, tacq = 0, tsteal = 0, tfail= 0;
  int mdepth = 0, mheight = 0;
  double twork = 0.0, tsearch = 0.0, tidle = 0.0, tovh = 0.0, tcbovh = 0.0;

//   // combine measurements from all threads
//   for (i = 0; i < GET_NUM_THREADS; i++) {
//     tnodes  += stealStack[i]->nNodes;
//     tleaves += stealStack[i]->nLeaves;
//     trel    += stealStack[i]->nRelease;
//     tacq    += stealStack[i]->nAcquire;
//     tsteal  += stealStack[i]->nSteal;
//     tfail   += stealStack[i]->nFail;
//     twork   += stealStack[i]->time[SS_WORK];
//     tsearch += stealStack[i]->time[SS_SEARCH];
//     tidle   += stealStack[i]->time[SS_IDLE];
//     tovh    += stealStack[i]->time[SS_OVH];
//     tcbovh  += stealStack[i]->time[SS_CBOVH];
//     mdepth   = max(mdepth, stealStack[i]->maxStackDepth);
//     mheight  = max(mheight, stealStack[i]->maxTreeDepth);
//   }
//   if (trel != tacq + tsteal) {
//     printf("*** error! total released != total acquired + total stolen\n");
//   }
//     
  uts_showStats(GET_NUM_THREADS, chunkSize, elapsedSecs, n_nodes, n_leaves, mheight);
// 
//   if (verbose > 1) {
//     if (doSteal) {
//       printf("Total chunks released = %d, of which %d reacquired and %d stolen\n",
//           trel, tacq, tsteal);
//       printf("Failed steal operations = %d, ", tfail);
//     }
// 
//     printf("Max stealStack size = %d\n", mdepth);
//     printf("Avg time per thread: Work = %.6f, Search = %.6f, Idle = %.6f\n", (twork / GET_NUM_THREADS),
//         (tsearch / GET_NUM_THREADS), (tidle / GET_NUM_THREADS));
//     printf("                     Overhead = %6f, CB_Overhead = %6f\n\n", (tovh / GET_NUM_THREADS),
//         (tcbovh/GET_NUM_THREADS));
//   }
// 
//   // per thread execution info
//   if (verbose > 2) {
//     for (i = 0; i < GET_NUM_THREADS; i++) {
//       printf("** Thread %d\n", i);
//       printf("  # nodes explored    = %d\n", stealStack[i]->nNodes);
//       printf("  # chunks released   = %d\n", stealStack[i]->nRelease);
//       printf("  # chunks reacquired = %d\n", stealStack[i]->nAcquire);
//       printf("  # chunks stolen     = %d\n", stealStack[i]->nSteal);
//       printf("  # failed steals     = %d\n", stealStack[i]->nFail);
//       printf("  maximum stack depth = %d\n", stealStack[i]->maxStackDepth);
//       printf("  work time           = %.6f secs (%d sessions)\n",
//              stealStack[i]->time[SS_WORK], stealStack[i]->entries[SS_WORK]);
//       printf("  overhead time       = %.6f secs (%d sessions)\n",
//              stealStack[i]->time[SS_OVH], stealStack[i]->entries[SS_OVH]);
//       printf("  search time         = %.6f secs (%d sessions)\n",
//              stealStack[i]->time[SS_SEARCH], stealStack[i]->entries[SS_SEARCH]);
//       printf("  idle time           = %.6f secs (%d sessions)\n",
//              stealStack[i]->time[SS_IDLE], stealStack[i]->entries[SS_IDLE]);
//       printf("  wakeups             = %d, false wakeups = %d (%.2f%%)",
//              stealStack[i]->wakeups, stealStack[i]->falseWakeups,
//              (stealStack[i]->wakeups == 0) ? 0.00 : ((((double)stealStack[i]->falseWakeups)/stealStack[i]->wakeups)*100.0));
//       printf("\n");
//     }
//   }
// 
//   #ifdef TRACE
//     printSessionRecords();
//   #endif
// 
//   // tree statistics output to stat.txt, if requested
// #ifdef UTS_STAT
//   if (stats) {
//     FILE *fp;
//     char * tmpstr;
//     char strBuf[5000];
//     int  ind = 0;
//     
//     fp = fopen("stat.txt", "a+w");
//     fprintf(fp, "\n------------------------------------------------------------------------------------------------------\n");
//     ind = uts_paramsToStr(strBuf, ind);
//     ind = impl_paramsToStr(strBuf, ind);
//     //showParametersStr(strBuf);
//     fprintf(fp, "%s\n", strBuf);
//     
//     fprintf(fp, "\nTotal nodes = %d\n", totalNodes); 
//     fprintf(fp, "Max depth   = %d\n\n", maxHeight); 
//     fprintf(fp, "Tseng ImbMeasure(overall)\n max:\t\t%lf \n avg:\t\t%lf \n devMaxAvg:\t %lf\n normDevMaxAvg: %lf\t\t\n\n", 
//             imb_max/totalNodes, imb_avg/totalNodes, imb_devmaxavg/totalNodes, 
//             imb_normdevmaxavg/totalNodes);
//     
//     switch (unbType){
//     case 0: tmpstr = "(min imb weighted by size)"; break;
//     case 1: tmpstr = "(min imb not weighted by size)"; break;
//     case 2: tmpstr = "(max imb not weighted by size)"; break;
//     default: tmpstr = "(?unknown measure)"; break;
//     }
//     fprintf(fp, "ImbMeasure:\t%s\n Overall:\t %lf\n Max:\t\t%lf\n Min:\t\t%lf\n\n", 
//             tmpstr, treeImb, minImb, maxImb);
//     showHist(fp);
//     fprintf(fp, "\n------------------------------------------------------------------------------------------------------\n\n\n");
//     fclose(fp);
//   }
// #endif
}


/*  Main() function for: Sequential, OpenMP, UPC, and Shmem
 *
 *  Notes on execution model:
 *     - under openMP, global vars are all shared
 *     - under UPC, global vars are private unless explicitly shared
 *     - UPC is SPMD starting with main, OpenMP goes SPMD after
 *       parsing parameters
 */
typedef struct _main_entrypoint_ctx {
    Node root;
    int argc;
    char (*(*argv));
 } main_entrypoint_ctx;


static void main_entrypoint(void *____arg) {
    main_entrypoint_ctx *ctx = (main_entrypoint_ctx *)____arg;
    Node root; root = ctx->root;
    int argc; argc = ctx->argc;
    char (*(*argv)); argv = ctx->argv;
{
  /* cancellable barrier initialization (single threaded under OMP) */

  double t1, t2, et;

  /* show parameter settings */
  uts_printParams();

  initRootNode(&root, type);
  /* time parallel search */
  t1 = uts_wctime();

/********** SPMD Parallel Region **********/
hclib_start_finish(); {
          Node child;
          initNode(&child);
          genChildren(&root, &child);
      } ; hclib_end_finish(); 

  t2 = uts_wctime();
  et = t2 - t1;
  showStats(et);
  } ;     free(____arg);
}

int main(int argc, char *argv[]) {
  Node root;

#ifdef THREAD_METADATA
  memset(t_metadata, 0x00, MAX_THREADS * sizeof(thread_metadata));
#endif

  /* determine benchmark parameters (all PEs) */
  uts_parseParams(argc, argv);

#ifdef UTS_STAT
  if (stats)
    initHist();
#endif  

main_entrypoint_ctx *new_ctx = (main_entrypoint_ctx *)malloc(sizeof(main_entrypoint_ctx));
new_ctx->root = root;
new_ctx->argc = argc;
new_ctx->argv = argv;
hclib_launch(main_entrypoint, new_ctx);

/********** End Parallel Region **********/

  return 0;
} 
