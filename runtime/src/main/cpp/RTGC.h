#ifndef RTGC_H
#define RTGC_H

#include "KAssert.h"
#include "Common.h"
#include "TypeInfo.h"
#include "Atomic.h"
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

#define RTGC_NO_INLINE // NO_INLINE

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
  GCRefList() { first_ = 0; }
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


template <class T, int ITEM_COUNT, int BUCKET_COUNT> 
struct BucketPool {
  struct _Bucket {
    T* _freeItem;
    _Bucket* _next;
    T* _items;

    void setItems(T* items) {
      this->_items = items;
    }

    void initBucket() {
      if (GET_NEXT_FREE(_items) == NULL) {
        T* item = _items;
        for (int i = ITEM_COUNT; --i > 0; ) {
          SET_NEXT_FREE(item, item+1);
        }
      }
    }

    T* popItem() {
      T* item = _freeItem;
      if (item != NULL) {
        _freeItem = GET_NEXT_FREE(item);
      }
      return item;
    }

    void recycleItem(T* item) {
      SET_NEXT_FREE(item, _freeItem);
      _freeItem = item;
    }
  };

  T* _alocatedItems;
  _Bucket _buckets[BUCKET_COUNT];
  _Bucket* _freeBuckets[BUCKET_COUNT];
  std::atomic<int> _cntFreeBucket;

  BucketPool() {
    _alocatedItems = new T[ITEM_COUNT*BUCKET_COUNT];
    _cntFreeBucket = BUCKET_COUNT;

    for (int i = 0; i < BUCKET_COUNT; i++) {
      _buckets[i].setItems(_alocatedItems + i * ITEM_COUNT);
      _freeBuckets[BUCKET_COUNT-1-i] = _buckets + i;
    }
  }

  static T* GET_NEXT_FREE(T* chain) {
      return *(T**)chain;
  }

  static void SET_NEXT_FREE(T* chain, T* next) {
      (*(T**)chain = next);
  }

  _Bucket* getBucket(T* item) {
    int offset = reinterpret_cast<char*>(item) - reinterpret_cast<char*>(_alocatedItems);
    int idx = offset / (sizeof(T) * ITEM_COUNT);
    return _buckets + idx;
  }

  _Bucket* popBucket() {
    int idx = --_cntFreeBucket;
    RuntimeAssert(idx >= 0, "Inssuficient Buckets");
    _Bucket* bucket = _freeBuckets[idx];
    bucket->initBucket();
    return bucket;
  }

  void recycleBuckets(_Bucket* bucket) {
    bucket->_next = NULL;
    _freeBuckets[_cntFreeBucket++] = bucket;
  }

  struct LocalAllocator {
    _Bucket* _currBucket;
    BucketPool* _buckets;

    void init(BucketPool* buckets) {
      this->_buckets = buckets;
    }

    int getItemIndex(T* item) {
      return item - _buckets->_alocatedItems;
    }

    T* getItem(int idx) {
      return _buckets->_alocatedItems + idx;
    }

    T* allocItem() {
      _Bucket* prev = _currBucket;
      T* item = prev->popItem();
      if (item != NULL) return item;

      _Bucket* bucket;
      for (bucket = prev->_next; bucket != NULL; bucket = bucket->_next) {
        if (bucket->_freeItem != NULL) {
          prev->_next = bucket->_next;
          bucket->_next = _currBucket;
          _currBucket = bucket;
          return bucket->popItem();
        }
      }

      bucket = _buckets->popBucket();
      bucket->_next = _currBucket;
      return bucket->popItem();
    }

    void recycleItem(T* item) {
      _Bucket* bucket = _buckets->getBucket(item);
      bucket->recycleItem(item);
    }

    void destroyAlloctor() {
      for (_Bucket* bucket = _currBucket; bucket != NULL; ) {
        _Bucket* next = bucket->_next;
        _buckets->recycleBuckets(bucket);
        bucket = next;
      }
    }
  };
};


typedef BucketPool<GCRefChain, 8192, 256> RefBucket;
typedef BucketPool<CyclicNode, 1024, 256> CyclicBucket;

struct RTGCMemState {
  RefBucket::LocalAllocator refChainAllocator;
  CyclicBucket::LocalAllocator cyclicNodeAllocator;

  CyclicNode* g_damagedCylicNodes;
  GCRefList g_cyclicTestNodes;
};



using RTGC_FIELD_TRAVERSE_CALLBACK = std::function<void(GCObject*)>;

void RTGC_traverseObjectFields(GCObject* obj, RTGC_FIELD_TRAVERSE_CALLBACK process) RTGC_NO_INLINE;

#endif // RTGC_H
