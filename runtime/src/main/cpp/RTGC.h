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

#define RTGC_REF_COUNT_MASK        ((1L << RTGC_REF_COUNT_BITS) -1)
static const int CYCLIC_NODE_ID_START = 2;

static const int ENABLE_RTGC_LOG = 0;
#define RTGC_LOG if (ENABLE_RTGC_LOG) printf

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
  void push(GCObject* obj);
  void remove(GCObject* obj);
  void moveTo(GCObject* retiree, GCRefList* receiver);
  bool tryRemove(GCObject* obj);
  bool isEmpty() { return first_ == 0; }
  void setFirst(GCRefChain* last);
  void clear() { setFirst(NULL); }
};

struct CyclicNode;
struct OnewayNode;

struct GCNode {
  friend CyclicNode;  
  GCRefList externalReferrers;
protected:  

  static CyclicNode* g_cyclicNodes;
  static int64_t g_memberUpdateLock;

  void clearSuspectedCyclic() { externalReferrers.flags_ &= ~NEED_CYCLIC_TEST; } 
  void markSuspectedCyclic() { externalReferrers.flags_ |= NEED_CYCLIC_TEST; } 

public:

  bool isSuspectedCyclic() {
    return (externalReferrers.flags_ & NEED_CYCLIC_TEST) != 0;
  }

  static void initMemory();


  static void* rtgcLock(GCObject* owner, GCObject* assigned, GCObject* erased) {
    while (!__sync_bool_compare_and_swap(&g_memberUpdateLock, 0, 1)) {}
    return 0;
  }
  static void rtgcUnlock(void* lock) {
    g_memberUpdateLock = 0;
  }

  static bool isLocked(void* lock) {
    return g_memberUpdateLock != 0;
  }

  int getTraceState() {
    return externalReferrers.flags_ & RTGC_TRACE_STATE_MASK;
  }

  void setTraceState(int state) {
    assert((state & ~RTGC_TRACE_STATE_MASK) == 0);
    externalReferrers.flags_ = (externalReferrers.flags_ & ~RTGC_TRACE_STATE_MASK) | state;
  }

};

struct OnewayNode : GCNode {
  void dealloc(bool isLocked);
};

struct CyclicNode : GCNode {
private:  
  int32_t rootObjectCount;
  CyclicNode* nextDamaged;
  GCRefList garbageTestList;

  static CyclicNode* g_damagedCylicNodes;
  static GCRefList g_cyclicTestNodes;

  void addCyclicObject(GCObject* obj);
  void mergeCyclicNode(GCObject* obj, int expiredNodeId);
  static void detectCyclicNodes(GCObject* tracingObj, GCRefList* traceList, GCRefList* finishedList);
public:

  int getId() {
    return (this - g_cyclicNodes) + CYCLIC_NODE_ID_START;
  }

  static CyclicNode* getNode(int nodeId) {
    nodeId -= CYCLIC_NODE_ID_START;
    if (nodeId < 0) return NULL;
    return g_cyclicNodes + nodeId;
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

  void dealloc(bool isLocked);


  static CyclicNode* create();
  static void addCyclicTest(GCObject* node);
  static void removeCyclicTest(GCObject* node, bool isLocked);
  static void detectCycles();
};

using RTGC_FIELD_TRAVERSE_CALLBACK = std::function<void(GCObject*)>;

void RTGC_traverseObjectFields(GCObject* obj, RTGC_FIELD_TRAVERSE_CALLBACK process);




#endif // RTGC_H
