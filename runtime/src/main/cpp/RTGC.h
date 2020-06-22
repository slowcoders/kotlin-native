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
static const int CYCLIC_NODE_ID_START = (1 << RTGC_NODE_SLOT_BITS) * 3 / 4;


struct RTGCRef {
  uint64_t root: RTGC_ROOT_REF_BITS;
  uint64_t obj:  RTGC_MEMBER_REF_BITS;  
  uint64_t node: RTGC_NODE_SLOT_BITS;
};

enum GCFlags {
  needGarbageTest = 1,
};

enum TraceState {
  NOT_TRACED,
  IN_TRACING,
  TRACE_FINISHED,
  OUT_OF_SCOPE
};

struct GCRefChain;

struct GCRefList {
private:  
  GCRefChain* first_;
public:
  GCRefList() { first_ = NULL; }
  GCRefChain* first() { return first_; }
  GCRefChain* find(GCObject* obj);
  GCRefChain* find(int node_id);
  void add(GCObject* obj);
  void remove(GCObject* obj);
  void moveTo(GCObject* retiree, GCRefList* receiver);
  bool tryRemove(GCObject* obj);
  bool isEmpty() { return first_ == nullptr; }
  void setFirst(GCRefChain* last);
  void clear() { setFirst(NULL); }
};

struct CyclicNode;
struct OnewayNode;

struct GCNode {
  friend CyclicNode;  
  GCRefList externalReferrers;
protected:  
  TraceState traceState;
  bool needCyclicTest;

  static OnewayNode* g_onewayNodes;
  static CyclicNode* g_cyclicNodes;
  static int64_t g_memberUpdateLock;

  static GCNode* getNode(int node_id);
  void clearSuspectedCyclic() { needCyclicTest = false; } 
  void markSuspectedCyclic() { needCyclicTest = true; } 

public:

  bool isSuspectedCyclic() {
    return needCyclicTest;
  }

  static void initMemory();

  static GCNode* getNode(RTGCRef ref) { return getNode((int)ref.node); }
  static CyclicNode* getCyclicNode(RTGCRef ref);

  static void* lock(GCObject* owner, GCObject* assigned, GCObject* erased) {
    while (!__sync_bool_compare_and_swap(&g_memberUpdateLock, 0, 1)) {}
    return 0;
  }
  static void unlock(void* lock) {
    g_memberUpdateLock = 0;
  }

  static bool isLocked(void* lock) {
    return g_memberUpdateLock != 0;
  }

  static void dealloc(int nodeId, bool isLocked);

};

struct OnewayNode : GCNode {
  static int create();

  int getId() {
    return (this - g_onewayNodes);
  }


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

  bool isDamaged() {
    return nextDamaged != 0;
  }

  bool isGarbage() {
    return rootObjectCount == 0 && externalReferrers.first() == 0;
  }  

  void markDamaged();

  void markSuspectedGarbage(GCObject* suspectedGarbage) {
      garbageTestList.add(suspectedGarbage);
      markDamaged();
  }

  void incRootObjectCount() {
    this->rootObjectCount ++;
  }

  int decRootObjectCount() {
    return --this->rootObjectCount;
  }

  void freeNode(GCObject* last);

  static CyclicNode* create();
  static void addCyclicTest(GCObject* node);
  static void detectCycles();
};

using RTGC_FIELD_TRAVERSE_CALLBACK = std::function<void(GCObject*)>;

void RTGC_traverseObjectFields(GCObject* obj, RTGC_FIELD_TRAVERSE_CALLBACK process);


inline CyclicNode* GCNode::getCyclicNode(RTGCRef ref) {
  int n_id = (int)ref.node;
  return n_id >= CYCLIC_NODE_ID_START ? (CyclicNode*)getNode(n_id) : nullptr;
}

inline GCNode* GCNode::getNode(int n_id) {
  if (n_id >= CYCLIC_NODE_ID_START) {
    return g_cyclicNodes + (n_id - CYCLIC_NODE_ID_START);
  }
  else {
    return g_onewayNodes + n_id;
  }
}


#endif // RTGC_H
