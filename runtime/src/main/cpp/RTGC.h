#ifndef RTGC_H
#define RTGC_H

#include "KAssert.h"
#include "Common.h"
#include "TypeInfo.h"
#include "Atomic.h"
#include <functional>
#include <utility>

#define RTGC 1

typedef struct ContainerHeader GCObject;

#define RTGC_ROOT_REF_BITS         12  // 4K
#define RTGC_MEMBER_REF_BITS       28  // 256M
#define RTGC_REF_COUNT_BITS        (RTGC_ROOT_REF_BITS + RTGC_MEMBER_REF_BITS)
#define RTGC_NODE_SLOT_BITS        (64 - RTGC_REF_COUNT_BITS)

#define RTGC_ROOT_REF_INCREEMENT   1
#define RTGC_MEMBER_REF_INCREEMENT (1 << RTGC_ROOT_REF_BITS)

#define RTGC_REF_COUNT_MASK        ((uint64_t)((1LL << RTGC_REF_COUNT_BITS) -1))
static const int CYCLIC_NODE_ID_START = 2;

#define ENABLE_RTGC_LOG 0
bool rtgc_trap() NO_INLINE;

#if ENABLE_RTGC_LOG
#define RTGC_LOG(...) konan::consolePrintf(__VA_ARGS__);
#define RTGC_TRAP(...) if (rtgc_trap()) konan::consolePrintf(__VA_ARGS__);
#else
#define RTGC_LOG(...)
#define RTGC_TRAP(...)
#endif


void RTGC_dumpRefInfo(GCObject*) NO_INLINE;
void RTGC_Error(GCObject* obj) NO_INLINE;

#define RTGC_NO_INLINE NO_INLINE

struct RTGCRef {
  uint64_t root: RTGC_ROOT_REF_BITS;
  uint64_t obj:  RTGC_MEMBER_REF_BITS;  
  uint64_t node: RTGC_NODE_SLOT_BITS;
};

enum GCFlags {
  needGarbageTest = 1,
};

static const int NOT_TRACED = 0;
static const int IN_TRACING = 1;
static const int TRACE_FINISHED = 2;
static const int OUT_OF_SCOPE = 3;

static const int RTGC_TRACE_STATE_MASK = NOT_TRACED | IN_TRACING | TRACE_FINISHED | OUT_OF_SCOPE;

struct GCRefChain {
  friend struct GCRefList;
private:
  GCObject* obj_;
  GCRefChain* next_;
public:
  GCObject* obj() { return obj_; }
  GCRefChain* next() { return next_; }
};

struct GCRefList {
private:  
  int32_t first_;
public:
  uint32_t flags_;

  static GCRefChain* g_refChains;
  GCRefList() { first_ = 0; }
  GCRefChain* topChain() { return first_ == 0 ? NULL : g_refChains + first_; }
  GCRefChain* find(GCObject* obj);
  GCRefChain* find(int node_id);
  GCObject* pop();
  void push(GCObject* obj)  RTGC_NO_INLINE;
  void remove(GCObject* obj)  RTGC_NO_INLINE;
  void moveTo(GCObject* retiree, GCRefList* receiver)  RTGC_NO_INLINE;
  bool tryRemove(GCObject* obj)  RTGC_NO_INLINE;
  bool isEmpty() { return first_ == 0; }
  void setFirst(GCRefChain* last)  RTGC_NO_INLINE;
  void clear() { setFirst(NULL); }
};

struct CyclicNode;
struct OnewayNode;

struct GCNode {
  friend CyclicNode;  
  GCRefList externalReferrers;
protected:  

  static CyclicNode* g_cyclicNodes;

  void clearSuspectedCyclic() { externalReferrers.flags_ &= ~NEED_CYCLIC_TEST; } 
  void markSuspectedCyclic() { externalReferrers.flags_ |= NEED_CYCLIC_TEST; } 

public:

  bool isSuspectedCyclic() {
    return (externalReferrers.flags_ & NEED_CYCLIC_TEST) != 0;
  }

  static void initMemory();


  static void rtgcLock() RTGC_NO_INLINE;

  static void rtgcUnlock() RTGC_NO_INLINE;

  static bool isLocked() RTGC_NO_INLINE;

  int getTraceState() {
    return externalReferrers.flags_ & RTGC_TRACE_STATE_MASK;
  }

  void setTraceState(int state) {
    RuntimeAssert((state & ~RTGC_TRACE_STATE_MASK) == 0, "invalid state");
    externalReferrers.flags_ = (externalReferrers.flags_ & ~RTGC_TRACE_STATE_MASK) | state;
  }

  static void dumpGCLog();
};

struct OnewayNode : GCNode {
  void dealloc();
};

struct CyclicNode : GCNode {
private:  
  int32_t rootObjectCount;
  CyclicNode* nextDamaged;
  GCRefList garbageTestList;

  static CyclicNode* g_damagedCylicNodes;
  static GCRefList g_cyclicTestNodes;

  void addCyclicObject(GCObject* obj)  RTGC_NO_INLINE;
  void mergeCyclicNode(GCObject* obj, int expiredNodeId)  RTGC_NO_INLINE;
  static void detectCyclicNodes(GCObject* tracingObj, GCRefList* traceList, GCRefList* finishedList)  RTGC_NO_INLINE;
public:

  int getId() {
    return (this - g_cyclicNodes) + CYCLIC_NODE_ID_START;
  }

  static CyclicNode* getNode(int nodeId) {
    if (nodeId < CYCLIC_NODE_ID_START) {
      return NULL;
    }
    return g_cyclicNodes + nodeId - CYCLIC_NODE_ID_START;
  }

  bool isDamaged() {
    return nextDamaged != 0;
  }

  bool isGarbage() {
    return rootObjectCount == 0 && externalReferrers.topChain() == 0;
  }  

  void markDamaged();

  void markSuspectedGarbage(GCObject* suspectedGarbage) {
      garbageTestList.push(suspectedGarbage);
      markDamaged();
  }

  void removeSuspectedGarbage(GCObject* obj) {
      garbageTestList.remove(obj);
  }

  int getRootObjectCount() {
    return rootObjectCount; 
  }

  void incRootObjectCount() {
    this->rootObjectCount ++;
  }

  int decRootObjectCount() {
    return --this->rootObjectCount;
  }

  void dealloc();


  static CyclicNode* create()  RTGC_NO_INLINE;
  static void addCyclicTest(GCObject* node, bool isLocalTest)  RTGC_NO_INLINE;
  static void removeCyclicTest(GCObject* node)  RTGC_NO_INLINE;
  static void detectCycles()  RTGC_NO_INLINE;
};

using RTGC_FIELD_TRAVERSE_CALLBACK = std::function<void(GCObject*)>;

void RTGC_traverseObjectFields(GCObject* obj, RTGC_FIELD_TRAVERSE_CALLBACK process) RTGC_NO_INLINE;

#endif // RTGC_H
