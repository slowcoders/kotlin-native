#ifndef RTGC_H
#define RTGC_H

#include "KAssert.h"
#include "Common.h"
#include "TypeInfo.h"
#include "Atomic.h"

#define RTGC 1

typedef struct ContainerHeader GCObject;

#define RTGC_ROOT_REF_BITS         12  // 4K
#define RTGC_MEMBER_REF_BITS       28  // 256M
#define RTGC_NODE_SLOT_BITS        (64 - RTGC_ROOT_REF_BITS - RTGC_ROOT_REF_BITS - 1)

#define RTGC_ROOT_REF_INCREEMENT   1
#define RTGC_MEMBER_REF_INCREEMENT (1 << RTGC_ROOT_REF_BITS)

#define RTGC_REF_COUNT_MASK        (((uint64_t)-1) >> RTGC_NODE_SLOT_BITS)

struct RTGCRef {
  uint64_t root: RTGC_ROOT_REF_BITS;
  uint64_t obj:  RTGC_MEMBER_REF_BITS;  
  uint64_t cylic: 1;
  uint64_t node: RTGC_NODE_SLOT_BITS;
};

enum GCFlags {
  needGarbageTest = 1,
};

enum TraceState {
  NOT_TRCAED,
  IN_TRACING,
  TRACE_FINISHED,
  OUT_OF_SCOPE
};

struct GCRefChain;

struct GCRefList {
private:  
  GCRefChain* first_;
public:
  GCRefChain* first() { return first_; }
  void add(GCObject* obj);
  void remove(GCObject* obj);
  bool isEmpty() { return first_ == nullptr; }
};

struct CyclicNode;
struct OnewayNode;

struct GCNode {
  GCRefList externalReferrers;
protected:  
  TraceState state;
  bool needCyclicTest;

  static OnewayNode* g_onewayNodes;
  static CyclicNode* g_cyclicNodes;
  static int64_t g_memberUpdateLock;

public:
  static void initMemory();

  static GCNode* getNode(RTGCRef ref);
  static CyclicNode* getCyclicNode(RTGCRef ref);

  static void* lock(GCObject* owner, GCObject* assigned, GCObject* erased) {
    while (!__sync_bool_compare_and_swap(&g_memberUpdateLock, 0, 1)) {}
    return 0;
  }
  static void unlock(void* lock) {
    g_memberUpdateLock = 0;
  }

};

struct OnewayNode : GCNode {
  static int create();
};

struct CyclicNode : GCNode {
private:  
  int32_t rootObjectCount;
  CyclicNode* nextDamaged;
  GCRefList garbageTestList;
public:

  bool isDamaged() {
    return nextDamaged != 0;
  }

  void markDamaged();

  void markDamaged(GCObject* suspectedGarbage) {
      garbageTestList.add(suspectedGarbage);
      markDamaged();
  }

  void incRootObjectCount() {
    this->rootObjectCount ++;
  }

  void decRootObjectCount() {
    this->rootObjectCount --;
  }

  static void addCyclicTest(GCNode* node);
};

inline CyclicNode* GCNode::getCyclicNode(RTGCRef ref) {
  return ref.cylic ? g_cyclicNodes + ref.node : nullptr;
}

inline GCNode* GCNode::getNode(RTGCRef ref) {
  return ref.cylic ? (GCNode*)(g_cyclicNodes + ref.node) : (GCNode*)(g_onewayNodes + ref.node);
}

#endif // RTGC_H
