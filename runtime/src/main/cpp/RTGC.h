#ifndef RTGC_H
#define RTGC_H

#include "KAssert.h"
#include "Common.h"
#include "TypeInfo.h"
#include "Atomic.h"
#include "Porting.h"
#include <functional>
#include <utility>
#include <atomic>
#include <vector>

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

#define ENABLE_RTGC_LOG             0
bool rtgc_trap(void* pObj) NO_INLINE;

#if ENABLE_RTGC_LOG
#define RTGC_LOG(...) konan::consolePrintf(__VA_ARGS__);
#define RTGC_TRAP(...) if (rtgc_trap(NULL)) konan::consolePrintf(__VA_ARGS__);
#else
#define RTGC_LOG(...)
#define RTGC_TRAP(...)
#endif
static const bool RTGC_STATISTCS = true;


void RTGC_dumpRefInfo(GCObject*) NO_INLINE;
void RTGC_dumpTypeInfo(const char* msg, const TypeInfo* typeInfo, GCObject* obj);
void RTGC_Error(GCObject* obj) NO_INLINE;

#define RTGC_NO_INLINE  NO_INLINE

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

  // static GCRefChain* g_refChains;
  GCRefList() { first_ = 0; flags_ = 0; }
  GCRefChain* topChain();// { return first_ == 0 ? NULL : g_refChains + first_; }
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

enum LockType {
  _FreeContainer,
  _IncrementRC,
  _IncrementAcyclicRC,
  _DecrementRC,    
  _DecrementAcyclicRC,    
  _AssignRef,
  _DeassignRef,
  _UpdateHeapRef,
  _PopBucket,
  _RecycleBucket,
  _DetectCylcles
};



struct GCNode {
  friend CyclicNode;  
  GCRefList externalReferrers;
protected:  

  //static CyclicNode* g_cyclicNodes;

  void clearSuspectedCyclic() { externalReferrers.flags_ &= ~NEED_CYCLIC_TEST; } 
  void markSuspectedCyclic() { externalReferrers.flags_ |= NEED_CYCLIC_TEST; } 

public:

  bool isSuspectedCyclic() {
    return (externalReferrers.flags_ & NEED_CYCLIC_TEST) != 0;
  }

  static void initMemory(struct RTGCMemState* state);


  static void rtgcLock(LockType type) RTGC_NO_INLINE;

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

  void addCyclicObject(GCObject* obj)  RTGC_NO_INLINE;
  void mergeCyclicNode(GCObject* obj, int expiredNodeId)  RTGC_NO_INLINE;
  static void detectCyclicNodes(GCObject* tracingObj, GCRefList* traceList, GCRefList* finishedList)  RTGC_NO_INLINE;
public:

  int getId(); // { return (this - g_cyclicNodes) + CYCLIC_NODE_ID_START; }

  static CyclicNode* getNode(int nodeId);
  // {
  //   if (nodeId < CYCLIC_NODE_ID_START) {
  //     return NULL;
  //   }
  //   return g_cyclicNodes + nodeId - CYCLIC_NODE_ID_START;
  // }

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

  template <bool Atomic>
  void incRootObjectCount() {
    int32_t value __attribute__((unused))= Atomic ?
       __sync_add_and_fetch(&rootObjectCount, 1)
        : rootObjectCount += 1;
  }

  template <bool Atomic>
  int32_t decRootObjectCount() {
    int32_t value __attribute__((unused))= Atomic ?
       __sync_sub_and_fetch(&rootObjectCount, 1)
        : rootObjectCount -= 1;
    return value;
  }

  void dealloc();


  static CyclicNode* create()  RTGC_NO_INLINE;
  static void addCyclicTest(GCObject* node, bool isLocalTest)  RTGC_NO_INLINE;
  static void removeCyclicTest(GCObject* node)  RTGC_NO_INLINE;
  static void detectCycles()  RTGC_NO_INLINE;
};

#define DEBUG_BUCKET 0

#if DEBUG_BUCKET
#define BUCKET_LOG(...) konan::consolePrintf(__VA_ARGS__);
#else
#define BUCKET_LOG(...)
#endif


template <class T, int ITEM_COUNT, int BUCKET_COUNT> 
struct SharedBucket {

  T* _alocatedItems;
  T* g_freeItemQ;
  #if DEBUG_BUCKET    
  int cntFreeItem;
  #endif

  SharedBucket() {
    g_freeItemQ = _alocatedItems = new T[ITEM_COUNT*BUCKET_COUNT];
    T* item = _alocatedItems;
    for (int i = ITEM_COUNT*BUCKET_COUNT-1; --i > 0; item++) {
      SET_NEXT_FREE(item, item+1);
    }
    #if DEBUG_BUCKET
    this->cntFreeItem = ITEM_COUNT*BUCKET_COUNT;
    #endif
  }

  static T* GET_NEXT_FREE(T* chain) {
      return *(T**)chain;
  }

  static void SET_NEXT_FREE(T* chain, T* next) {
      (*(T**)chain = next);
  }

  T* popBucket(int id) RTGC_NO_INLINE {
    GCNode::rtgcLock(_PopBucket);
    T* bucket = g_freeItemQ;
    T* last = bucket;
    for (int i = ITEM_COUNT; --i > 0; ) {
      last = GET_NEXT_FREE(last);
    }
    g_freeItemQ = GET_NEXT_FREE(last);
    SET_NEXT_FREE(last, NULL);
    #if DEBUG_BUCKET
    this->cntFreeItem-= ITEM_COUNT;
    BUCKET_LOG("popBucket[:%d] => %d\n", id, this->cntFreeItem)
    #endif
    GCNode::rtgcUnlock();
    return bucket;
  }

  void recycleBucket(T* first, int id) {
    if (first == NULL) return;
    GCNode::rtgcLock(_RecycleBucket);
    T* last = NULL;
    #if DEBUG_BUCKET
    int cntRecycle = 0;
    #endif
    for (T* item = first; item != NULL; item = GET_NEXT_FREE(item)) {
      last = item;
      #if DEBUG_BUCKET
      cntRecycle ++;
      #endif
    }

    #if DEBUG_BUCKET
    this->cntFreeItem += cntRecycle;
    BUCKET_LOG("recycleBucket[:%d] + %d => %d\n", id, cntRecycle, this->cntFreeItem)
    #endif
    SET_NEXT_FREE(last, g_freeItemQ);
    g_freeItemQ = first;
    GCNode::rtgcUnlock();
  }

  struct LocalAllocator {
    SharedBucket* _buckets;
    T* _currBucket;
    int _id;

    void init(SharedBucket* buckets, int id) {
      this->_buckets = buckets;
      this->_id = id;
      _currBucket = buckets->popBucket(_id);
      BUCKET_LOG("init Allocator %d %p\n", this->_id, this);
    }

    int getItemIndex(T* item) {
      return item - _buckets->_alocatedItems;
    }

    T* getItem(int idx) {
      return _buckets->_alocatedItems + idx;
    }

    T* allocItem() RTGC_NO_INLINE {
      T* item = _currBucket;
      if (item == NULL) {
        item = _buckets->popBucket(_id);
      }
      _currBucket = GET_NEXT_FREE(item);
      return item;
    }

    void recycleItem(T* item) {
      SET_NEXT_FREE(item, _currBucket);
      _currBucket = item;
    }

    void destroyAlloctor() {
      BUCKET_LOG("destroyAlloctor[:%d] %p\n", _id, this);
      _buckets->recycleBucket(_currBucket, _id); 
    }
  };
};


typedef SharedBucket<GCRefChain, 8192, 256> RefBucket;
typedef SharedBucket<CyclicNode, 8192, 256> CyclicBucket;

struct RTGCMemState {
  RefBucket::LocalAllocator refChainAllocator;
  CyclicBucket::LocalAllocator cyclicNodeAllocator;

  CyclicNode* g_damagedCylicNodes;
  GCRefList g_cyclicTestNodes;
};



using RTGC_FIELD_TRAVERSE_CALLBACK = std::function<void(GCObject*)>;

void RTGC_traverseObjectFields(GCObject* obj, RTGC_FIELD_TRAVERSE_CALLBACK process) RTGC_NO_INLINE;

#endif // RTGC_H
