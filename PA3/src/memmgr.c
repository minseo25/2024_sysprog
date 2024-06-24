//--------------------------------------------------------------------------------------------------
// System Programming                       Memory Lab                                   Spring 2024
//
/// @file
/// @brief dynamic memory manager
/// @author Minseo Kim
/// @studid 2020-17429
//--------------------------------------------------------------------------------------------------


// Dynamic memory manager
// ======================
// This module implements a custom dynamic memory manager.
//
// Heap organization:
// ------------------
// The data segment for the heap is provided by the dataseg module. A 'word' in the heap is
// eight bytes.
//
// Implicit free list:
// -------------------
// - minimal block size: 32 bytes (header + footer + 2 data words)
// - h,f: header/footer of free block
// - H,F: header/footer of allocated block
//
// - state after initialization
//
//         initial sentinel half-block                  end sentinel half-block
//                   |                                             |
//   ds_heap_start   |   heap_start                         heap_end       ds_heap_brk
//               |   |   |                                         |       |
//               v   v   v                                         v       v
//               +---+---+-----------------------------------------+---+---+
//               |???| F | h :                                 : f | H |???|
//               +---+---+-----------------------------------------+---+---+
//                       ^                                         ^
//                       |                                         |
//               32-byte aligned                           32-byte aligned
//
// - allocation policy: best fit
// - block splitting: always at 32-byte boundaries
// - immediate coalescing upon free
//
// Explicit free list:
// -------------------
// - minimal block size: 32 bytes (header + footer + next + prev)
// - h,f: header/footer of free block
// - n,p: next/previous pointer
// - H,F: header/footer of allocated block
//
// - state after initialization
//
//         initial sentinel half-block                  end sentinel half-block
//                   |                                             |
//   ds_heap_start   |   heap_start                         heap_end       ds_heap_brk
//               |   |   |                                         |       |
//               v   v   v                                         v       v
//               +---+---+-----------------------------------------+---+---+
//               |???| F | h : n : p :                         : f | H |???|
//               +---+---+-----------------------------------------+---+---+
//                       ^                                         ^
//                       |                                         |
//               32-byte aligned                           32-byte aligned
//
// - allocation policy: best fit
// - block splitting: always at 32-byte boundaries
// - immediate coalescing upon free
//

#define _GNU_SOURCE

#include <assert.h>
#include <error.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dataseg.h"
#include "memmgr.h"


/// @name global variables
/// @{
static void *ds_heap_start = NULL;                     ///< physical start of data segment
static void *ds_heap_brk   = NULL;                     ///< physical end of data segment
static void *heap_start    = NULL;                     ///< logical start of heap
static void *heap_end      = NULL;                     ///< logical end of heap
static int  PAGESIZE       = 0;                        ///< memory system page size
static void *(*get_free_block)(size_t) = NULL;         ///< get free block for selected allocation policy
static size_t CHUNKSIZE    = 1<<16;                    ///< minimal data segment allocation unit
static size_t SHRINKTHLD   = 1<<14;                    ///< threshold to shrink heap
static int  mm_initialized = 0;                        ///< initialized flag (yes: 1, otherwise 0)
static int  mm_loglevel    = 0;                        ///< log level (0: off; 1: info; 2: verbose)

// Freelist
static FreelistPolicy freelist_policy  = 0;            ///< free list management policy

// Free chunk for explicit free list
struct FreeChunk {
    size_t size;                                   ///< size of free chunk (header)
    void* next;                                    ///< pointer to next free chunk
    void* prev;                                    ///< pointer to previous free chunk
    void* dummy;                                   ///< dummy pointer to align to 32 bytes
};
static struct FreeChunk first;                     ///< first free chunk
static struct FreeChunk last;                      ///< last free chunk

//
// TODO: add more global variables as needed
//
/// @}

/// @name Macro definitions
/// @{
#define MAX(a, b)          ((a) > (b) ? (a) : (b))     ///< MAX function

#define TYPE               unsigned long               ///< word type of heap
#define TYPE_SIZE          sizeof(TYPE)                ///< size of word type

#define ALLOC              1                           ///< block allocated flag
#define FREE               0                           ///< block free flag
#define STATUS_MASK        ((TYPE)(0x7))               ///< mask to retrieve flags from header/footer
#define SIZE_MASK          (~STATUS_MASK)              ///< mask to retrieve size from header/footer

#define BS                 32                          ///< minimal block size. Must be a power of 2
#define BS_MASK            (~(BS-1))                   ///< alignment mask

#define WORD(p)            ((TYPE)(p))                 ///< convert pointer to TYPE
#define PTR(w)             ((void*)(w))                ///< convert TYPE to void*

#define PREV_PTR(p)        ((char *)(p)-TYPE_SIZE)             ///< get pointer to word preceeding p
#define NEXT_PTR(p)        ((char *)(p)+TYPE_SIZE)             ///< get pointer to word succeeding p
#define NEXT_NEXT_PTR(p)   ((char *)(p)+2*TYPE_SIZE)           ///< get pointer to double word succeeding p
#define HDR2FTR(p)         ((char *)(p)+GET_SIZE(p)-TYPE_SIZE) ///< get footer for given header
#define FTR2HDR(p)         ((char *)(p)-GET_SIZE(p)+TYPE_SIZE) ///< get header for given footer

#define PACK(size,status)  ((size) | (status))         ///< pack size & status into boundary tag
#define SIZE(v)            (v & SIZE_MASK)             ///< extract size from boundary tag
#define STATUS(v)          (v & STATUS_MASK)           ///< extract status from boundary tag

#define PUT(p, v)          (*(TYPE*)(p) = (TYPE)(v))   ///< write word v to *p
#define GET(p)             (*(TYPE*)(p))               ///< read word at *p
#define GET_SIZE(p)        (SIZE(GET(p)))              ///< extract size from header/footer
#define GET_STATUS(p)      (STATUS(GET(p)))            ///< extract status from header/footer

#define NEXT_BLKP(p)       ((char *)(p)+GET_SIZE(p))              ///< get pointer to next block
#define PREV_BLKP(p)       ((char *)(p)-GET_SIZE(PREV_PTR(p)))    ///< get pointer to previous block
#define NEXT_LIST_GET(p)   (*(void **)(NEXT_PTR(p)))               ///< get pointer to next free block
#define PREV_LIST_GET(p)   (*(void **)(NEXT_NEXT_PTR(p)))     ///< get pointer to previous free block

//
// TODO: add more macros as needed
//
/// @}


/// @name Logging facilities
/// @{

/// @brief print a log message if level <= mm_loglevel. The variadic argument is a printf format
///        string followed by its parametrs
#ifdef DEBUG
  #define LOG(level, ...) mm_log(level, __VA_ARGS__)

/// @brief print a log message. Do not call directly; use LOG() instead
/// @param level log level of message.
/// @param ... variadic parameters for vprintf function (format string with optional parameters)
static void mm_log(int level, ...)
{
  if (level > mm_loglevel) return;

  va_list va;
  va_start(va, level);
  const char *fmt = va_arg(va, const char*);

  if (fmt != NULL) vfprintf(stdout, fmt, va);

  va_end(va);

  fprintf(stdout, "\n");
}

#else
  #define LOG(level, ...)
#endif

/// @}


/// @name Program termination facilities
/// @{

/// @brief print error message and terminate process. The variadic argument is a printf format
///        string followed by its parameters
#define PANIC(...) mm_panic(__func__, __VA_ARGS__)

/// @brief print error message and terminate process. Do not call directly, Use PANIC() instead.
/// @param func function name
/// @param ... variadic parameters for vprintf function (format string with optional parameters)
static void mm_panic(const char *func, ...)
{
  va_list va;
  va_start(va, func);
  const char *fmt = va_arg(va, const char*);

  fprintf(stderr, "PANIC in %s%s", func, fmt ? ": " : ".");
  if (fmt != NULL) vfprintf(stderr, fmt, va);

  va_end(va);

  fprintf(stderr, "\n");

  exit(EXIT_FAILURE);
}
/// @}


static void* bf_get_free_block_implicit(size_t size);
static void* bf_get_free_block_explicit(size_t size);
static void* coalesce(void* bp, int shrink);
static void place(void* bp, size_t asize);
static void *extend_heap(size_t words);
static void add_free_block(void* bp);
static void remove_free_block(void* bp);

void mm_init(FreelistPolicy fp)
{
  LOG(1, "mm_init()");

  // set free list policy
  freelist_policy = fp;
  switch (freelist_policy) {
    case fp_Implicit:
      get_free_block = bf_get_free_block_implicit;
      break;
      
    case fp_Explicit:
      get_free_block = bf_get_free_block_explicit;
      break;
    
    default:
      PANIC("Non supported freelist policy.");
      break;
  }

  // retrieve heap status and perform a few initial sanity checks
  ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);
  PAGESIZE = ds_getpagesize();

  LOG(2, "  ds_heap_start:          %p\n"
         "  ds_heap_brk:            %p\n"
         "  PAGESIZE:               %d\n",
         ds_heap_start, ds_heap_brk, PAGESIZE);

  if (ds_heap_start == NULL) PANIC("Data segment not initialized.");
  if (ds_heap_start != ds_heap_brk) PANIC("Heap not clean.");
  if (PAGESIZE == 0) PANIC("Reported pagesize == 0.");

  // initialize heap with CHUNK SIZE, update ds_heap_brk
  if(ds_sbrk(CHUNKSIZE) == (void*)-1) PANIC("ds_sbrk() failed in mm_init()");
  ds_heap_brk = ds_sbrk(0);

  // initialize heap_start, heap_end. 32 bytes aligned
  // int 
  heap_start = (void *)((TYPE)((char *)ds_heap_start + TYPE_SIZE + BS - 1) & BS_MASK); // initial sentinel half-block considered
  heap_end = (void *)((TYPE)((char *)ds_heap_brk - TYPE_SIZE) & BS_MASK); // end sentinel half-block considered

  // initialize sentinels, free chunk
  PUT(PREV_PTR(heap_start), PACK(0, 1));
  PUT(heap_end, PACK(0, 1));
  size_t size = (char *)heap_end - (char *)heap_start;
  PUT(heap_start, PACK(size,0));
  PUT(PREV_PTR(heap_end), PACK(size,0));

  // initialize free list
  if(freelist_policy == fp_Explicit) {
    first.next = heap_start;
    first.prev = NULL;
    last.next = NULL;
    last.prev = heap_start;
    PUT(NEXT_PTR(heap_start), &last);
    PUT(NEXT_NEXT_PTR(heap_start), &first);
  }

  // heap is now initialized
  mm_initialized = 1;
}

/// @brief find and return a free block of at least @a size bytes (best fit)
/// @param size size of block (including header & footer tags), in bytes
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
static void* bf_get_free_block_implicit(size_t size)
{
  LOG(1, "bf_get_free_block_implicit(0x%lx (%lu))", size, size);
  assert(mm_initialized);

  char* block = heap_start;
  char* best_fit_block = NULL;
  size_t best_fit_size = -1;

  while(1) {
    size_t b_size = GET_SIZE(block);
    int b_alloc = GET_STATUS(block);

    // met the end sentinel half-block
    if(!b_size) break;

    if(b_alloc == 0) { // free block found
      if(b_size == size) { // if it perfectly fits, return right away
        return block;
      } else if(b_size > size && b_size < best_fit_size) { // if it fits better, update best_fit_block
        best_fit_block = block;
        best_fit_size = b_size;
      }
    }
    // move to next block
    block = NEXT_BLKP(block);
  }

  return best_fit_block;
}


/// @brief find and return a free block of at least @a size bytes (best fit)
/// @param size size of block (including header & footer tags), in bytes
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
static void* bf_get_free_block_explicit(size_t size)
{
  LOG(1, "bf_get_free_block_explicit(0x%lx (%lu))", size, size);
  assert(mm_initialized);
  
  char* block = NEXT_LIST_GET(&first);
  char* best_fit_block = NULL;
  size_t best_fit_size = -1;

  while(block != NULL){
    size_t b_size = GET_SIZE(block);
    int b_alloc = GET_STATUS(block);

    if(b_alloc == 0) { // free block found
      if(b_size == size) { // if it perfectly fits, return right away
        return block;
      } else if(b_size > size && b_size < best_fit_size) {  // if it fits better, update best_fit_block
        best_fit_block = block;
        best_fit_size = b_size;
      }
    }
    block = NEXT_LIST_GET(block);
  }

  return best_fit_block;
}

// free blocks merge 시 free list 내 위치 반영해야 하는지?
static void *coalesce(void *bp, int shrink) {
  LOG(1, "coalesce(0x%p)", bp);
  assert(mm_initialized);

  if(GET_STATUS(bp) == 1) {
    printf("Allocated block passed to coalesce()");
    return NULL;
  }
  
  // PREV_BLKP(bp)의 경우 SIZE=0인 initial sentinel half-block에서 에러 발생, PREV_PTR(bp)로 체크
  int prev_alloc = GET_STATUS(PREV_PTR(bp));
  void *prev_bp = PREV_BLKP(bp);
  int next_alloc = GET_STATUS(NEXT_BLKP(bp));
  void *next_bp = NEXT_BLKP(bp);

  // final block size, final block pointer
  size_t size = GET_SIZE(bp);
  char *result = bp;

  if(prev_alloc && !next_alloc) { // case 2 : prev allocated, next free
    // remove next block from free list
    if(freelist_policy == fp_Explicit) remove_free_block(next_bp);

    size += GET_SIZE(next_bp);
    PUT(bp, PACK(size, 0));
    PUT(HDR2FTR(bp), PACK(size, 0));
    // PUT(next_bp, 0); PUT(PREV_PTR(next_bp), 0);
  } else if(!prev_alloc && next_alloc) { // case 3 : prev free, next allocated
    // remove prev_block from free list
    if(freelist_policy == fp_Explicit) remove_free_block(prev_bp);

    size += GET_SIZE(prev_bp);
    PUT(prev_bp, PACK(size, 0));
    PUT(HDR2FTR(prev_bp), PACK(size, 0));
    // PUT(bp, 0); PUT(PREV_PTR(bp), 0);
    result = prev_bp;
  } else if(!prev_alloc && !next_alloc) { // case 4 : prev free, next free
    // remove prev_block, next_block from free list
    if(freelist_policy == fp_Explicit) {
      remove_free_block(prev_bp);
      remove_free_block(next_bp);
    }

    size += (GET_SIZE(prev_bp) + GET_SIZE(next_bp));
    PUT(prev_bp, PACK(size, 0));
    PUT(HDR2FTR(prev_bp), PACK(size, 0));
    // PUT(next_bp, 0); PUT(PREV_PTR(next_bp), 0); PUT(bp, 0); PUT(PREV_PTR(bp), 0);
    result = prev_bp;
  }
  // case 1 : prev allocated, next allocated => do nothing

  // if coalesce() is called from mm_free() && coalesced block is at the end of heap && size >= BS + SHRINKTHLD
  if(shrink && NEXT_BLKP(result) == heap_end && size >= BS + SHRINKTHLD) {
    // shrink heap
    if(ds_sbrk(-1 * (int)SHRINKTHLD) != (void*)-1) {
      ds_heap_brk = ds_sbrk(0);
      heap_end = (void *)((TYPE)((char *)ds_heap_brk - TYPE_SIZE) & BS_MASK);

      size = (char *)heap_end - result;
      PUT(heap_end, PACK(0, 1));
      PUT(result, PACK(size, 0));
      PUT(PREV_PTR(heap_end), PACK(size, 0));
    }
  }

  // add coalesced block to free list
  if(freelist_policy == fp_Explicit) add_free_block(result);
  
  return result;
}

// extend heap by given size (in bytes)
static void *extend_heap(size_t size)
{
  LOG(1, "extend_heap(%lu words)", words);
  assert(mm_initialized);

  char *bp = heap_end;

  // increase heap size
  if((long)ds_sbrk(size) == -1) return NULL;
  ds_heap_brk = ds_sbrk(0);

  // update heap end. get free block size. 32 bytes aligned
  heap_end = (void *)((TYPE)((char *)ds_heap_brk - TYPE_SIZE) & BS_MASK);
  size = (char *)heap_end - bp;
  
  PUT(bp, PACK(size, 0));
  PUT(PREV_PTR(heap_end), PACK(size, 0));
  PUT(NEXT_BLKP(bp), PACK(0, 1));

  // coalesce if the previous block was free, heap does not shrink
  return coalesce(bp, 0);
}

// when given a single free chunk, place the requested block (split if necessary)
static void place(void *bp, size_t req_size) {
  LOG(1, "place(0x%p, 0x%lx (%lu))", bp, req_size, req_size);
  assert(mm_initialized);

  size_t split_size = GET_SIZE(bp) - req_size;

  // remove from free list
  if(freelist_policy == fp_Explicit) remove_free_block(bp);
  // set header and footer
  PUT(bp, PACK(req_size, 1));
  PUT(HDR2FTR(bp), PACK(req_size, 1));

  // split if necessary
  if(split_size > 0) {
    void *split_bp = NEXT_BLKP(bp);

    // set header and footer
    PUT(split_bp, PACK(split_size, 0));
    PUT(HDR2FTR(split_bp), PACK(split_size, 0));
    // add to the beginning of the free list
    if(freelist_policy == fp_Explicit) add_free_block(split_bp);
  }
}

static void add_free_block(void *bp) {
  void *top = NEXT_LIST_GET(&first);

  // bp->next = top, bp->prev = &first
  PUT(NEXT_PTR(bp), top);
  PUT(NEXT_NEXT_PTR(bp), &first);
  // top->prev = split_bp, first->next = split_bp
  PUT(NEXT_NEXT_PTR(top), bp);
  PUT(NEXT_PTR(&first), bp);
}

static void remove_free_block(void *bp) {
  void *next = NEXT_LIST_GET(bp);
  void *prev = PREV_LIST_GET(bp);

  // bp->prev->next = bp->next, bp->next->prev = bp->prev
  PUT(NEXT_PTR(prev), next);
  PUT(NEXT_NEXT_PTR(next), prev);

  // remove prev and next ptrs
  PUT(NEXT_PTR(bp), 0);
  PUT(NEXT_NEXT_PTR(bp), 0);
}


void* mm_malloc(size_t size)
{
  LOG(1, "mm_malloc(0x%lx (%lu))", size, size);
  assert(mm_initialized);

  // ignore spurious requests
  if(size == 0) return NULL;

  // need space for header&footer. 32 bytes aligned
  size_t req_size = (size + TYPE_SIZE*2 + BS - 1) / BS * BS;

  char* bp = get_free_block(req_size);
  if(bp == NULL) { // failed to get free block, need to extend heap
    size_t extend_size = MAX(req_size, CHUNKSIZE);
    if((bp = extend_heap(extend_size)) == NULL) {
      return NULL;
    }
  }
  
  place(bp, req_size);
  // return payload pointer
  return NEXT_PTR(bp);
}


void* mm_calloc(size_t nmemb, size_t size)
{
  LOG(1, "mm_calloc(0x%lx, 0x%lx (%lu))", nmemb, size, size);
  assert(mm_initialized);

  //
  // calloc is simply malloc() followed by memset()
  //
  void *payload = mm_malloc(nmemb * size);

  if (payload != NULL) memset(payload, 0, nmemb * size);

  return payload;
}


void* mm_realloc(void *ptr, size_t size)
{
  LOG(1, "mm_realloc(%p, 0x%lx (%lu))", ptr, size, size);
  assert(mm_initialized);

  if(ptr == NULL) return mm_malloc(size);
  if(size == 0) { mm_free(ptr); return NULL; }
  // payload pointer -> block pointer
  ptr = PREV_PTR(ptr);
  if(GET_STATUS(ptr) == 0) {
    printf("realloc() of free block");
    return NULL;
  }

  size_t old_size = GET_SIZE(ptr);
  size_t new_size = (size + TYPE_SIZE*2 + BS - 1) / BS * BS;

  if(old_size == new_size) return NEXT_PTR(ptr);

  if(old_size > new_size) { // if the block is large enough, split it
    PUT(ptr, PACK(new_size, 1));
    PUT(HDR2FTR(ptr), PACK(new_size, 1));

    size_t split_size = old_size - new_size;
    void *split_ptr = NEXT_BLKP(ptr);
    PUT(split_ptr, PACK(split_size, 0));
    PUT(HDR2FTR(split_ptr), PACK(split_size, 0));

    if(freelist_policy == fp_Explicit) add_free_block(split_ptr);
    return NEXT_PTR(ptr);
  }

  void* next_ptr = NEXT_BLKP(ptr);
  size_t next_size = GET_SIZE(next_ptr);
  // if there exists successor free block and the sum of the two blocks is large enough
  if(GET_STATUS(next_ptr)==0 && old_size+next_size >= new_size) {
    // remove the next block from the free list and merge the two blocks
    if(freelist_policy == fp_Explicit) remove_free_block(next_ptr);
    PUT(ptr, PACK(new_size, 1));
    PUT(HDR2FTR(ptr), PACK(new_size, 1));

    // possibly split the remainder and add to the free list
    size_t split_size = old_size + next_size - new_size;
    void *split_ptr = NEXT_BLKP(ptr);
    if(split_size > 0) {
      PUT(split_ptr, PACK(split_size, 0));
      PUT(HDR2FTR(split_ptr), PACK(split_size, 0));

      if(freelist_policy == fp_Explicit) add_free_block(split_ptr);
    }
    return NEXT_PTR(ptr);
  }

  void *new_ptr = mm_malloc(size);
  if(new_ptr) {
    memcpy(new_ptr, NEXT_PTR(ptr), old_size - TYPE_SIZE*2);
    mm_free(NEXT_PTR(ptr));
  }
  return new_ptr;
}


void mm_free(void *ptr)
{
  LOG(1, "mm_free(%p)", ptr);
  assert(mm_initialized);

  if(ptr == NULL) return;
  ptr = PREV_PTR(ptr);
  if(GET_STATUS(ptr) == 0) {
    printf("double free error!");
    return;
  }
  
  // update header and footer's status bits
  size_t size = GET_SIZE(ptr);
  PUT(ptr, PACK(size, 0));
  PUT(HDR2FTR(ptr), PACK(size, 0));

  // coalesce the block, shrinks heap if needed
  coalesce(ptr, 1);
}


void mm_setloglevel(int level)
{
  mm_loglevel = level;
}


void mm_check(void)
{
  assert(mm_initialized);

  void *p;

  char *fpstr;
  if (freelist_policy == fp_Implicit) fpstr = "Implicit";
  else if (freelist_policy == fp_Explicit) fpstr = "Explicit";
  else fpstr = "invalid";

  printf("----------------------------------------- mm_check ----------------------------------------------\n");
  printf("  ds_heap_start:          %p\n", ds_heap_start);
  printf("  ds_heap_brk:            %p\n", ds_heap_brk);
  printf("  heap_start:             %p\n", heap_start);
  printf("  heap_end:               %p\n", heap_end);
  printf("  free list policy:       %s\n", fpstr);

  printf("\n");
  p = PREV_PTR(heap_start);
  printf("  initial sentinel:       %p: size: %6lx (%7ld), status: %s\n",
         p, GET_SIZE(p), GET_SIZE(p), GET_STATUS(p) == ALLOC ? "allocated" : "free");
  p = heap_end;
  printf("  end sentinel:           %p: size: %6lx (%7ld), status: %s\n",
         p, GET_SIZE(p), GET_SIZE(p), GET_STATUS(p) == ALLOC ? "allocated" : "free");
  printf("\n");

  if(freelist_policy == fp_Implicit){
    printf("    %-14s  %8s  %10s  %10s  %8s  %s\n", "address", "offset", "size (hex)", "size (dec)", "payload", "status");
  }
  else if(freelist_policy == fp_Explicit){
    printf("    %-14s  %8s  %10s  %10s  %8s  %-14s  %-14s  %s\n", "address", "offset", "size (hex)", "size (dec)", "payload", "next", "prev", "status");
  }

  long errors = 0;
  p = heap_start;
  while (p < heap_end) {
    char *ofs_str, *size_str;

    TYPE hdr = GET(p);
    TYPE size = SIZE(hdr);
    TYPE status = STATUS(hdr);

    void *next = NEXT_LIST_GET(p);
    void *prev = PREV_LIST_GET(p);

    if (asprintf(&ofs_str, "0x%lx", p-heap_start) < 0) ofs_str = NULL;
    if (asprintf(&size_str, "0x%lx", size) < 0) size_str = NULL;

    if(freelist_policy == fp_Implicit){
      printf("    %p  %8s  %10s  %10ld  %8ld  %s\n",
                p, ofs_str, size_str, size, size-2*TYPE_SIZE, status == ALLOC ? "allocated" : "free");
    }
    else if(freelist_policy == fp_Explicit){
      printf("    %p  %8s  %10s  %10ld  %8ld  %-14p  %-14p  %s\n",
                p, ofs_str, size_str, size, size-2*TYPE_SIZE,
                status == ALLOC ? NULL : next, status == ALLOC ? NULL : prev,
                status == ALLOC ? "allocated" : "free");
    }
    
    free(ofs_str);
    free(size_str);

    void *fp = p + size - TYPE_SIZE;
    TYPE ftr = GET(fp);
    TYPE fsize = SIZE(ftr);
    TYPE fstatus = STATUS(ftr);

    if ((size != fsize) || (status != fstatus)) {
      errors++;
      printf("    --> ERROR: footer at %p with different properties: size: %lx, status: %lx\n", 
             fp, fsize, fstatus);
      mm_panic("mm_check");
    }

    p = p + size;
    if (size == 0) {
      printf("    WARNING: size 0 detected, aborting traversal.\n");
      break;
    }
  }

  printf("\n");
  if ((p == heap_end) && (errors == 0)) printf("  Block structure coherent.\n");
  printf("-------------------------------------------------------------------------------------------------\n");
}


