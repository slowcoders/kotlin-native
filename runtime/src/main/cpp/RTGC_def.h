#ifndef RTGC_DEF_H
#define RTGC_DEF_H

#include "Common.h"

#define RTGC                              1
#define RTGC_DEBUG                        1
#define RTGC_STATISTCS                    1
#define RTGC_NO_INLINE                    // NO_INLINE
#define ENABLE_RTGC_LOG                   0
#define ENABLE_RTGC_LOG_VERBOSE           (1 & ENABLE_RTGC_LOG)
#define DEBUG_RTGC_BUCKET                 0
#define DETECT_TWOWAY_LINK_EARLY          0

#define RTGC_LATE_DESTROY_CYCLIC_SUSPECT  false

#define RTGC_ROOT_REF_BITS         12  // 4K
#define RTGC_MEMBER_REF_BITS       28  // 256M
#define RTGC_REF_COUNT_BITS        (RTGC_ROOT_REF_BITS + RTGC_MEMBER_REF_BITS)
#define RTGC_NODE_SLOT_BITS        (64 - RTGC_REF_COUNT_BITS)

#define RTGC_ROOT_REF_INCREEMENT   1
#define RTGC_MEMBER_REF_INCREEMENT (1 << RTGC_ROOT_REF_BITS)

#define RTGC_REF_COUNT_MASK        ((uint64_t)((1LL << RTGC_REF_COUNT_BITS) -1))
static const int CYCLIC_NODE_ID_START = 2;

bool rtgc_trap(void* pObj) NO_INLINE;

#if ENABLE_RTGC_LOG
#define RTGC_LOG(...) konan::consolePrintf(__VA_ARGS__);
#define RTGC_TRAP(...) if (rtgc_trap(NULL)) konan::consolePrintf(__VA_ARGS__);
#else
#define RTGC_LOG(...)
#define RTGC_TRAP(...)
#endif

#if ENABLE_RTGC_LOG_VERBOSE
#define RTGC_LOG_V(...) konan::consolePrintf(__VA_ARGS__);
#else
#define RTGC_LOG_V(...)
#endif

#ifdef RTGC_DEBUG
#  define DebugAssert(condition) assert(condition)
#  define DebugRefAssert(ref, condition) assert(RTGC_Check(ref, condition))
#else
#  define DebugAssert(condition) assert(condition)
#  define DebugRefAssert(ref, condition) // ignore
#endif

typedef struct ContainerHeader GCObject;

void RTGC_dumpRefInfo(GCObject* container, const char* msg = "*") NO_INLINE;
void RTGC_dumpRefInfo0(GCObject* container) NO_INLINE;
void RTGC_dumpReferrers(GCObject* container) NO_INLINE;
void RTGC_dumpTypeInfo(const char* msg, const TypeInfo* typeInfo, GCObject* obj);
bool RTGC_Check(GCObject* obj, bool isValid) NO_INLINE;
extern void* RTGC_debugInstance;

struct RTGCGlobal {
  static int g_cntAddRefChain;
  static int g_cntRemoveRefChain;
  static int g_cntAddCyclicNode;
  static int g_cntRemoveCyclicNode;
  static int g_cntAddCyclicTest;
  static int g_cntRemoveCyclicTest;
  static int g_cntFreezed;
  static int g_cntAddSuspectedGarbageInClyce;
  static int g_cntRemoveSuspectedGarbageInClyce;

  static void validateMemPool();

  static void init(struct RTGCMemState* state);
};

#endif // RTGC_DEF_H
