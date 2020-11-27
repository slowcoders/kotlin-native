/*
 * Copyright 2010-2020 JetBrains s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <stdio.h>

#include <cstddef> // for offsetof

// Allow concurrent global cycle collector.
#define USE_CYCLIC_GC 0
// CycleDetector internally uses static local with runtime initialization,
// which requires atomics. Atomics are not available on WASM.
#if 1 // RTGC
#define USE_CYCLE_DETECTOR 0
#else
#ifdef KONAN_WASM
#define USE_CYCLE_DETECTOR 0
#else
#define USE_CYCLE_DETECTOR 1
#endif
#endif

#include "Alloc.h"
#include "KAssert.h"
#include "Atomic.h"
#if USE_CYCLIC_GC
#include "CyclicCollector.h"
#endif  // USE_CYCLIC_GC
#include "Exceptions.h"
#include "KString.h"
#include "Memory.h"
#include "MemoryPrivate.hpp"
#include "Natives.h"
#include "Porting.h"
#include "Runtime.h"
#include "Utils.h"
#include "WorkerBoundReference.h"

// If garbage collection algorithm for cyclic garbage to be used.
// We are using the Bacon's algorithm for GC, see
// http://researcher.watson.ibm.com/researcher/files/us-bacon/Bacon03Pure.pdf.
#define USE_GC 1
// Define to 1 to print all memory operations.
#define TRACE_MEMORY 0
// Define to 1 to print major GC events.
#define TRACE_GC 0
// Collect memory manager events statistics.
#define COLLECT_STATISTIC 0
// Define to 1 to print detailed time statistics for GC events.
#define PROFILE_GC 0

#if COLLECT_STATISTIC
#include <algorithm>
#endif

#define inline //

namespace {

// Granularity of arena container chunks.
constexpr container_size_t kContainerAlignment = 1024;
// Single object alignment.
constexpr container_size_t kObjectAlignment = 8;

// Required e.g. for object size computations to be correct.
static_assert(sizeof(ContainerHeader) % kObjectAlignment == 0, "sizeof(ContainerHeader) is not aligned");

#if TRACE_MEMORY
#undef TRACE_GC
#define TRACE_GC 1
#define MEMORY_LOG(...) konan::consolePrintf(__VA_ARGS__);
#else
#define MEMORY_LOG(...)
#endif

#if TRACE_GC
#define GC_LOG(...) konan::consolePrintf(__VA_ARGS__);
#else
#define GC_LOG(...)
#endif

#if USE_GC
// Collection threshold default (collect after having so many elements in the
// release candidates set).
constexpr size_t kGcThreshold = 8 * 1024;
// Ergonomic thresholds.
// If GC to computations time ratio is above that value,
// increase GC threshold by 1.5 times.
constexpr double kGcToComputeRatioThreshold = 0.5;
// Never exceed this value when increasing GC threshold.
constexpr size_t kMaxErgonomicThreshold = 32 * 1024;
// Threshold of size for toFree set, triggering actual cycle collector.
constexpr size_t kMaxToFreeSizeThreshold = 8 * 1024;
// Never exceed this value when increasing size for toFree set, triggering actual cycle collector.
constexpr size_t kMaxErgonomicToFreeSizeThreshold = 8 * 1024 * 1024;
// How many elements in finalizer queue allowed before cleaning it up.
constexpr int32_t kFinalizerQueueThreshold = 32;
// If allocated that much memory since last GC - force new GC.
constexpr size_t kMaxGcAllocThreshold = 8 * 1024 * 1024;
// If the ratio of GC collection cycles time to program execution time is greater this value,
// increase GC threshold for cycles collection.
constexpr double kGcCollectCyclesLoadRatio = 0.3;
// Minimum time of cycles collection to change thresholds.
constexpr size_t kGcCollectCyclesMinimumDuration = 200;

#endif  // USE_GC

typedef KStdUnorderedSet<ContainerHeader*> ContainerHeaderSet;
typedef KStdVector<ContainerHeader*> ContainerHeaderList;
typedef KStdDeque<ContainerHeader*> ContainerHeaderDeque;
typedef KStdVector<KRef> KRefList;
typedef KStdVector<KRef*> KRefPtrList;
typedef KStdUnorderedSet<KRef> KRefSet;
typedef KStdUnorderedMap<KRef, KInt> KRefIntMap;
typedef KStdDeque<KRef> KRefDeque;
typedef KStdDeque<KRefList> KRefListDeque;
typedef KStdUnorderedMap<void**, std::pair<KRef*,int>> KThreadLocalStorageMap;

// A little hack that allows to enable -O2 optimizations
// Prevents clang from replacing FrameOverlay struct
// with single pointer.
// Can be removed when FrameOverlay will become more complex.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
FrameOverlay exportFrameOverlay;
#pragma clang diagnostic pop

// Current number of allocated containers.
volatile int allocCount = 0;
volatile int aliveMemoryStatesCount = 0;

#if USE_CYCLIC_GC
KBoolean g_hasCyclicCollector = true;
#endif  // USE_CYCLIC_GC

// TODO: Consider using ObjHolder.
class ScopedRefHolder {
 public:
  ScopedRefHolder() = default;

  explicit ScopedRefHolder(KRef obj);

  ScopedRefHolder(const ScopedRefHolder&) = delete;

  ScopedRefHolder(ScopedRefHolder&& other) noexcept: obj_(other.obj_) {
    other.obj_ = nullptr;
    RTGC_LOG("ScopedRefHolder created %p\n", &other);
  }

  ScopedRefHolder& operator=(const ScopedRefHolder&) = delete;

  ScopedRefHolder& operator=(ScopedRefHolder&& other) noexcept {
    ScopedRefHolder tmp(std::move(other));
    swap(tmp);
    RTGC_LOG("ScopedRefHolder assigned %p\n", &other);
    return *this;
  }

  ~ScopedRefHolder();

  void swap(ScopedRefHolder& other) noexcept {
    std::swap(obj_, other.obj_);
  }

 private:
  KRef obj_ = nullptr;
};

#if USE_CYCLE_DETECTOR

struct CycleDetectorRootset {
  // Orders roots.
  KStdVector<KRef> roots;
  // Pins a state of each root.
  KStdUnorderedMap<KRef, KStdVector<KRef>> rootToFields;
  // Holding roots and their fields to avoid GC-ing them.
  KStdVector<ScopedRefHolder> heldRefs;
};

class CycleDetector {
 public:
  static void insertCandidateIfNeeded(KRef object) {
    if (canBeACandidate(object))
      instance().insertCandidate(object);
  }

  static void removeCandidateIfNeeded(KRef object) {
    if (canBeACandidate(object))
      instance().removeCandidate(object);
  }

  static CycleDetectorRootset collectRootset();

 private:
  CycleDetector() = default;
  ~CycleDetector() = default;
  CycleDetector(const CycleDetector&) = delete;
  CycleDetector(CycleDetector&&) = delete;
  CycleDetector& operator=(const CycleDetector&) = delete;
  CycleDetector& operator=(CycleDetector&&) = delete;

  static CycleDetector& instance() {
    // Only store a pointer to CycleDetector in .bss
    static CycleDetector* result = new CycleDetector();
    return *result;
  }

  static bool canBeACandidate(KRef object) {
    return KonanNeedDebugInfo &&
        Kotlin_memoryLeakCheckerEnabled() &&
        (object->type_info()->flags_ & TF_LEAK_DETECTOR_CANDIDATE) != 0;
  }

  void insertCandidate(KRef candidate) {
    LockGuard<SimpleMutex> guard(lock_);

    auto it = candidateList_.insert(candidateList_.begin(), candidate);
    candidateInList_.emplace(candidate, it);
    RTGC_LOG("CycleDetector insertCandidate %p\n", candidate);

  }

  void removeCandidate(KRef candidate) {
    LockGuard<SimpleMutex> guard(lock_);

    auto it = candidateInList_.find(candidate);
    if (it == candidateInList_.end())
      return;
    candidateList_.erase(it->second);
    candidateInList_.erase(it);
    RTGC_LOG("CycleDetector removeCandidate %p\n", candidate);
  }

  SimpleMutex lock_;
  using CandidateList = KStdList<KRef>;
  CandidateList candidateList_;
  KStdUnorderedMap<KRef, CandidateList::iterator> candidateInList_;
};

#endif  // USE_CYCLE_DETECTOR

// TODO: can we pass this variable as an explicit argument?
THREAD_LOCAL_VARIABLE MemoryState* memoryState = nullptr;
THREAD_LOCAL_VARIABLE FrameOverlay* currentFrame = nullptr;

#if COLLECT_STATISTIC
class MemoryStatistic {
public:
  // UpdateRef per-object type counters.
  uint64_t updateCounters[12][10];
  // Alloc per container type counters.
  uint64_t containerAllocs[2];
  // Free per container type counters.
  uint64_t objectAllocs[6];
  // Histogram of allocation size distribution.
  KStdUnorderedMap<int, int>* allocationHistogram;
  // Number of allocation cache hits.
  int allocCacheHit;
  // Number of allocation cache misses.
  int allocCacheMiss;
  // Number of regular reference increments.
  uint64_t addRefs;
  // Number of atomic reference increments.
  uint64_t atomicAddRefs;
  // Number of regular reference decrements.
  uint64_t releaseRefs;
  // Number of atomic reference decrements.
  uint64_t atomicReleaseRefs;
  // Number of potential cycle candidates.
  uint64_t releaseCyclicRefs;

  // Map of array index to human readable name.
  static constexpr const char* indexToName[] = {
    "local ", "stack ", "perm  ", "frozen", "shared", "null  " };

  void init() {
    memset(containerAllocs, 0, sizeof(containerAllocs));
    memset(objectAllocs, 0, sizeof(objectAllocs));
    memset(updateCounters, 0, sizeof(updateCounters));
    allocationHistogram = konanConstructInstance<KStdUnorderedMap<int, int>>();
    allocCacheHit = 0;
    allocCacheMiss = 0;
  }

  void deinit() {
    konanDestructInstance(allocationHistogram);
    allocationHistogram = nullptr;
  }

  void incAddRef(const ContainerHeader* header, bool atomic, int stack) {
    if (atomic) atomicAddRefs++; else addRefs++;
  }

  void incReleaseRef(const ContainerHeader* header, bool atomic, bool cyclic, int stack) {
   if (atomic) {
      atomicReleaseRefs++;
    } else {
      if (cyclic) releaseCyclicRefs++; else releaseRefs++;
    }
  }

  void incUpdateRef(const ObjHeader* objOld, const ObjHeader* objNew, int stack) {
    updateCounters[toIndex(objOld, stack)][toIndex(objNew, stack)]++;
  }

  void incAlloc(size_t size, const ContainerHeader* header) {
    containerAllocs[0]++;
    ++(*allocationHistogram)[size];
  }

  void incFree(const ContainerHeader* header) {
    containerAllocs[1]++;
  }

  void incAlloc(size_t size, const ObjHeader* header) {
    objectAllocs[toIndex(header, 0)]++;
  }

  static int toIndex(const ObjHeader* obj, int stack) {
    if (reinterpret_cast<uintptr_t>(obj) > 1)
        return toIndex(obj->container(), stack);
    else
        return 4 + stack * 6;
  }

  static int toIndex(const ContainerHeader* header, int stack) {
    if (header == nullptr) return 2 + stack * 6; // permanent.
    switch (header->tag()) {
      case CONTAINER_TAG_LOCAL  : return 0 + stack * 6;
      case CONTAINER_TAG_STACK  : return 1 + stack * 6;
      case CONTAINER_TAG_FROZEN : return 3 + stack * 6;
      case CONTAINER_TAG_SHARED : return 4 + stack * 6;

    }
    RuntimeAssert(false, "unknown container type");
    return -1;
  }

  static double percents(uint64_t value, uint64_t all) {
   return all == 0 ? 0 : ((double)value / (double)all) * 100.0;
  }

  void printStatistic() {
    konan::consolePrintf("\nMemory manager statistic:\n\n");
    konan::consolePrintf("Container alloc: %lld, free: %lld\n",
                           containerAllocs[0], containerAllocs[1]);
    for (int i = 0; i < 6; i++) {
      // Only local, shared and frozen can be allocated.
      if (i == 0 || i == 3 || i == 4)
        konan::consolePrintf("Object %s alloc: %lld\n", indexToName[i], objectAllocs[i]);
    }
    konan::consolePrintf("\n");

    uint64_t allUpdateRefs = 0, heapUpdateRefs = 0, stackUpdateRefs = 0;
    for (int i = 0; i < 12; i++) {
      for (int j = 0; j < 12; j++) {
         allUpdateRefs += updateCounters[i][j];
         if (i < 6 && j < 6)
           heapUpdateRefs += updateCounters[i][j];
         if (i >= 6 && j >= 6)
           stackUpdateRefs += updateCounters[i][j];
      }
    }
    konan::consolePrintf("Total updates: %lld, stack: %lld(%.2lf%%), heap: %lld(%.2lf%%)\n",
        allUpdateRefs,
        stackUpdateRefs, percents(stackUpdateRefs, allUpdateRefs),
        heapUpdateRefs, percents(heapUpdateRefs, allUpdateRefs));
    for (int i = 0; i < 6; i++) {
      for (int j = 0; j < 6; j++) {
        if (updateCounters[i][j] != 0)
            konan::consolePrintf("UpdateHeapRef[%s -> %s]: %lld (%.2lf%% of all, %.2lf%% of heap)\n",
                             indexToName[i], indexToName[j], updateCounters[i][j],
                             percents(updateCounters[i][j], allUpdateRefs),
                             percents(updateCounters[i][j], heapUpdateRefs));
      }
    }
    for (int i = 6; i < 12; i++) {
        for (int j = 6; j < 12; j++) {
            if (updateCounters[i][j] != 0)
                konan::consolePrintf("UpdateStackRef[%s -> %s]: %lld (%.2lf%% of all, %.2lf%% of stack)\n",
                                 indexToName[i - 6], indexToName[j - 6],
                                 updateCounters[i][j],
                                 percents(updateCounters[i][j], allUpdateRefs),
                                 percents(updateCounters[i][j], stackUpdateRefs));
        }
    }
    konan::consolePrintf("\n");

    konan::consolePrintf("Allocation histogram:\n");
    KStdVector<int> keys(allocationHistogram->size());
    int index = 0;
    for (auto& it : *allocationHistogram) {
      keys[index++] = it.first;
    }
    std::sort(keys.begin(), keys.end());
    int perLine = 4;
    int count = 0;
    for (auto it : keys) {
      konan::consolePrintf(
          "%d bytes -> %d times  ", it, (*allocationHistogram)[it]);
      if (++count % perLine == (perLine - 1) || (count == keys.size()))
        konan::consolePrintf("\n");
    }


    uint64_t allAddRefs = addRefs + atomicAddRefs;
    uint64_t allReleases = releaseRefs + atomicReleaseRefs + releaseCyclicRefs;
    konan::consolePrintf("AddRefs:\t%lld/%lld (%.2lf%% of atomic)\n"
                         "Releases:\t%lld/%lld (%.2lf%% of atomic)\n"
                         "ReleaseRefs affecting cycle collector   : %lld (%.2lf%% of cyclic)\n",
                         addRefs, atomicAddRefs, percents(atomicAddRefs, allAddRefs),
                         releaseRefs, atomicReleaseRefs, percents(atomicReleaseRefs, allReleases),
                         releaseCyclicRefs, percents(releaseCyclicRefs, allReleases));
  }
};

constexpr const char* MemoryStatistic::indexToName[];

#endif  // COLLECT_STATISTIC

inline bool isPermanentOrFrozen(ContainerHeader* container) {
    return container == nullptr || container->frozen();
}

inline bool isShareable(ContainerHeader* container) {
    return container == nullptr || container->shared() || container->frozen();
}

void shareAny(ObjHeader* obj);

inline bool tryMakeShareable(ContainerHeader* container) {
    if (container == nullptr || container->shared()) {
      return true;
    }
    if (container->frozen()) {
      shareAny((ObjHeader*)(container+1));
      return true;
    }
    return false;
}

void garbageCollect();


}  // namespace

class ForeignRefManager {
 public:
  static ForeignRefManager* create() {
    ForeignRefManager* result = konanConstructInstance<ForeignRefManager>();
    if (!RTGC) result->addRef();
    return result;
  }

  void addRef() {
    if (!RTGC) atomicAdd(&refCount, 1);
  }

  void releaseRef() {
    if (!RTGC && atomicAdd(&this->refCount, -1) == 0) {
      // So the owning MemoryState has abandoned [this].
      // Leaving the queued work items would result in memory leak.
      // Luckily current thread has exclusive access to [this],
      // so it can process the queue pretending like it takes ownership of all its objects:
      this->processAbandoned();

      konanDestructInstance(this);
    }
  }

  bool tryReleaseRefOwned() {
    if (!RTGC && atomicAdd(&this->refCount, -1) == 0) {
      if (this->releaseList != nullptr) {
        // There are no more holders of [this] to process the enqueued work items in [releaseRef].
        // Revert the reference counter back and notify the caller to process and then retry:
        atomicAdd(&this->refCount, 1);
        return false;
      }

      konanDestructInstance(this);
    }

    return true;
  }

  void enqueueReleaseRef(ObjHeader* obj) {
    ListNode* newListNode = konanConstructInstance<ListNode>();
    newListNode->obj = obj;
    while (true) {
      ListNode* next = this->releaseList;
      newListNode->next = next;
      if (compareAndSet(&this->releaseList, next, newListNode)) break;
    }
  }

  template <typename func>
  void processEnqueuedReleaseRefsWith(func process) {
    if (releaseList == nullptr) return;

    ListNode* toProcess = nullptr;

    while (true) {
      toProcess = releaseList;
      if (compareAndSet<ListNode*>(&this->releaseList, toProcess, nullptr)) break;
    }

    while (toProcess != nullptr) {
      if (RTGC) {
        ReleaseRef(toProcess->obj);
      }
      else {
        process(toProcess->obj);
      }
      ListNode* next = toProcess->next;
      konanDestructInstance(toProcess);
      toProcess = next;
    }
  }

private:
  int refCount;

  struct ListNode {
    ObjHeader* obj;
    ListNode* next;
  };

  ListNode* volatile releaseList;

  void processAbandoned() {
    if (!RTGC && this->releaseList != nullptr) {
      bool hadNoStateInitialized = (memoryState == nullptr);

      if (hadNoStateInitialized) {
        // Disregard request if all runtimes are no longer alive.
        if (atomicGet(&aliveMemoryStatesCount) == 0)
          return;

        memoryState = InitMemory(); // Required by ReleaseRef.
      }

      processEnqueuedReleaseRefsWith([](ObjHeader* obj) {
        ReleaseRef(obj);
      });

      if (hadNoStateInitialized) {
        // Discard the memory state.
        DeinitMemory(memoryState);
      }
    }
  }
};

struct MemoryState : RTGCMemState {
#if TRACE_MEMORY
  // Set of all containers.
  ContainerHeaderSet* containers;
#endif

  KThreadLocalStorageMap* tlsMap;
  KRef* tlsMapLastStart;
  void* tlsMapLastKey;

#if USE_GC
  // Finalizer queue - linked list of containers scheduled for finalization.
  ContainerHeader* finalizerQueue;
  int finalizerQueueSize;
  int finalizerQueueSuspendCount;
  /*
   * Typical scenario for GC is as following:
   * we have 90% of objects with refcount = 0 which will be deleted during
   * the first phase of the algorithm.
   * We could mark them with a bit in order to tell the next two phases to skip them
   * and thus requiring only one list, but the downside is that both of the
   * next phases would iterate over the whole list of objects instead of only 10%.
   */
  ContainerHeaderList* toFree; // List of all cycle candidates.
  ContainerHeaderList* roots; // Real candidates excluding those with refcount = 0.
  // How many GC suspend requests happened.
  int gcSuspendCount;
  // How many candidate elements in toRelease shall trigger collection.
  size_t gcThreshold;
  // How many candidate elements in toFree shall trigger cycle collection.
  uint64_t gcCollectCyclesThreshold;
  // If collection is in progress.
  int gcInProgress;
  // Objects to be released.
  KStdDeque<ContainerHeader*>* toRelease;

  ForeignRefManager* foreignRefManager;

  bool gcErgonomics;
  uint64_t lastGcTimestamp;
  uint64_t lastCyclicGcTimestamp;
  uint32_t gcEpoque;

  uint64_t allocSinceLastGc;
  uint64_t allocSinceLastGcThreshold;
#endif // USE_GC

  // A stack of initializing singletons.
  KStdVector<std::pair<ObjHeader**, ObjHeader*>> initializingSingletons;

#if COLLECT_STATISTIC
  #define CONTAINER_ALLOC_STAT(state, size, container) state->statistic.incAlloc(size, container);
  #define CONTAINER_DESTROY_STAT(state, container) \
    state->statistic.incFree(container);
  #define OBJECT_ALLOC_STAT(state, size, object) \
    state->statistic.incAlloc(size, object); \
    state->statistic.incAddRef(object->container(), 0, 0);
  #define UPDATE_REF_STAT(state, oldRef, newRef, slot, stack) \
    state->statistic.incUpdateRef(oldRef, newRef, stack);
  #define UPDATE_ADDREF_STAT(state, obj, atomic, stack) \
      state->statistic.incAddRef(obj, atomic, stack);
  #define UPDATE_RELEASEREF_STAT(state, obj, atomic, cyclic, stack) \
        state->statistic.incReleaseRef(obj, atomic, cyclic, stack);
  #define INIT_STAT(state) \
    state->statistic.init();
  #define DEINIT_STAT(state) \
    state->statistic.deinit();
  #define PRINT_STAT(state) \
    state->statistic.printStatistic();
  MemoryStatistic statistic;
#else
  #define CONTAINER_ALLOC_STAT(state, size, container)
  #define CONTAINER_DESTROY_STAT(state, container)
  #define OBJECT_ALLOC_STAT(state, size, object)
  #define UPDATE_REF_STAT(state, oldRef, newRef, slot, stack)
  #define UPDATE_ADDREF_STAT(state, obj, atomic, stack)
  #define UPDATE_RELEASEREF_STAT(state, obj, atomic, cyclic, stack)
  #define INIT_STAT(state)
  #define DEINIT_STAT(state)
  #define PRINT_STAT(state)
#endif // COLLECT_STATISTIC
};

namespace {

#if TRACE_MEMORY
#define INIT_TRACE(state) \
  memoryState->containers = konanConstructInstance<ContainerHeaderSet>();
#define DEINIT_TRACE(state) \
   konanDestructInstance(memoryState->containers); \
   memoryState->containers = nullptr;
#else
#define INIT_TRACE(state)
#define DEINIT_TRACE(state)
#endif
#define CONTAINER_ALLOC_TRACE(state, size, container) \
  MEMORY_LOG("Container alloc %d at %p\n", size, container)
#define CONTAINER_DESTROY_TRACE(state, container) \
  MEMORY_LOG("Container destroy %p\n", container)
#define OBJECT_ALLOC_TRACE(state, size, object) \
  MEMORY_LOG("Object alloc %d at %p\n", size, object)
#define UPDATE_REF_TRACE(state, oldRef, newRef, slot, owner) \
  MEMORY_LOG("UpdateRef %p->%p: %p -> %p\n", (void*)owner, slot, oldRef, newRef)

// Events macro definitions.
// Called on worker's memory init.
#define INIT_EVENT(state) \
  INIT_STAT(state) \
  INIT_TRACE(state)
// Called on worker's memory deinit.
#define DEINIT_EVENT(state) \
  DEINIT_STAT(state)
// Called on container allocation.
#define CONTAINER_ALLOC_EVENT(state, size, container) \
  CONTAINER_ALLOC_STAT(state, size, container) \
  CONTAINER_ALLOC_TRACE(state, size, container)
// Called on container destroy (memory is released to allocator).
#define CONTAINER_DESTROY_EVENT(state, container) \
  CONTAINER_DESTROY_STAT(state, container) \
  CONTAINER_DESTROY_TRACE(state, container)
// Object was just allocated.
#define OBJECT_ALLOC_EVENT(state, size, object) \
  OBJECT_ALLOC_STAT(state, size, object) \
  OBJECT_ALLOC_TRACE(state, size, object)
// Object is freed.
#define OBJECT_FREE_EVENT(state, size, object)  \
  OBJECT_FREE_STAT(state, size, object) \
  OBJECT_FREE_TRACE(state, object)
// Reference in memory is being updated.
#define UPDATE_REF_EVENT(state, oldRef, newRef, slot, stack) \
  UPDATE_REF_STAT(state, oldRef, newRef, slot, stack) \
  UPDATE_REF_TRACE(state, oldRef, newRef, slot, stack)
// Infomation shall be printed as worker is exiting.
#define PRINT_EVENT(state) \
  PRINT_STAT(state)

// Forward declarations.
#ifndef RTGC
void freeContainer(ContainerHeader* header, int garbageNodeId=0) NO_INLINE;
#endif
#if USE_GC
void garbageCollect(MemoryState* state, bool force) NO_INLINE;
void cyclicGarbageCollect() NO_INLINE;
void rememberNewContainer(ContainerHeader* container);
#endif  // USE_GC

// Class representing arbitrary placement container.
class Container {
 public:
  ContainerHeader* header() const { return header_; }
 protected:
  // Data where everything is being stored.
  ContainerHeader* header_;

  void SetHeader(ObjHeader* obj, const TypeInfo* type_info) {
    obj->typeInfoOrMeta_ = const_cast<TypeInfo*>(type_info);
    // Take into account typeInfo's immutability for ARC strategy.
    if ((type_info->flags_ & TF_IMMUTABLE) != 0)
      header_->freezeRef();
    if ((type_info->flags_ & (TF_IMMUTABLE | TF_ACYCLIC)) != 0)
      header_->markAcyclic();
  }
};

// Container for a single object.
class ObjectContainer : public Container {
 public:
  // Single instance.
  explicit ObjectContainer(MemoryState* state, const TypeInfo* type_info) {
    Init(state, type_info);
  }

  // Object container shalln't have any dtor, as it's being freed by
  // ::Release().

  ObjHeader* GetPlace() const {
    return reinterpret_cast<ObjHeader*>(header_ + 1);
  }

 private:
  void Init(MemoryState* state, const TypeInfo* type_info);
};


class ArrayContainer : public Container {
 public:
  ArrayContainer(MemoryState* state, const TypeInfo* type_info, uint32_t elements) {
    Init(state, type_info, elements);
  }

  // Array container shalln't have any dtor, as it's being freed by ::Release().

  ArrayHeader* GetPlace() const {
    return reinterpret_cast<ArrayHeader*>(header_ + 1);
  }

 private:
  void Init(MemoryState* state, const TypeInfo* type_info, uint32_t elements);
};

// Class representing arena-style placement container.
// Container is used for reference counting, and it is assumed that objects
// with related placement will share container. Only
// whole container can be freed, individual objects are not taken into account.
class ArenaContainer;

struct ContainerChunk {
  ContainerChunk* next;
  ArenaContainer* arena;
  // Then we have ContainerHeader here.
  ContainerHeader* asHeader() {
    return reinterpret_cast<ContainerHeader*>(this + 1);
  }
};

class ArenaContainer {
 public:
  void Init();
  void Deinit();

  // Place individual object in this container.
  ObjHeader* PlaceObject(const TypeInfo* type_info);

  // Places an array of certain type in this container. Note that array_type_info
  // is type info for an array, not for an individual element. Also note that exactly
  // same operation could be used to place strings.
  ArrayHeader* PlaceArray(const TypeInfo* array_type_info, container_size_t count);

  ObjHeader** getSlot();

 private:
  void* place(container_size_t size);

  bool allocContainer(container_size_t minSize);

  void setHeader(ObjHeader* obj, const TypeInfo* typeInfo) {
    obj->typeInfoOrMeta_ = const_cast<TypeInfo*>(typeInfo);
    obj->setContainer(currentChunk_->asHeader());
    // Here we do not take into account typeInfo's immutability for ARC strategy, as there's no ARC.
  }

  ContainerChunk* currentChunk_;
  uint8_t* current_;
  uint8_t* end_;
  ArrayHeader* slots_;
  uint32_t slotsCount_;
};

constexpr int kFrameOverlaySlots = sizeof(FrameOverlay) / sizeof(ObjHeader**);

inline bool isFreeable(const ContainerHeader* header) {
  return header != nullptr && header->freeable();
}

#if !RTGC
inline bool isArena(const ContainerHeader* header) {
  return header != nullptr && !header->isStack();
}
#endif

inline bool isAggregatingFrozenContainer(const ContainerHeader* header) {
  return header != nullptr && header->frozen() && header->objectCount() > 1;
}

inline bool isMarkedAsRemoved(ContainerHeader* container) {
  return (reinterpret_cast<uintptr_t>(container) & 1) != 0;
}

inline ContainerHeader* markAsRemoved(ContainerHeader* container) {
  return reinterpret_cast<ContainerHeader*>(reinterpret_cast<uintptr_t>(container) | 1);
}

inline ContainerHeader* clearRemoved(ContainerHeader* container) {
  return reinterpret_cast<ContainerHeader*>(
    reinterpret_cast<uintptr_t>(container) & ~static_cast<uintptr_t>(1));
}

inline container_size_t alignUp(container_size_t size, int alignment) {
  return (size + alignment - 1) & ~(alignment - 1);
}

inline ContainerHeader* realShareableContainer(ContainerHeader* container) {
  tryMakeShareable(container);
  RuntimeAssert(container->shared(), "Only makes sense on shareable objects");
  return reinterpret_cast<ObjHeader*>(container + 1)->container();
}

inline uint32_t arrayObjectSize(const TypeInfo* typeInfo, uint32_t count) {
  // Note: array body is aligned, but for size computation it is enough to align the sum.
  static_assert(kObjectAlignment % alignof(KLong) == 0, "");
  static_assert(kObjectAlignment % alignof(KDouble) == 0, "");
  return alignUp(sizeof(ArrayHeader) - typeInfo->instanceSize_ * count, kObjectAlignment);
}

inline uint32_t arrayObjectSize(const ArrayHeader* obj) {
  return arrayObjectSize(obj->type_info(), obj->count_);
}

// TODO: shall we do padding for alignment?
inline container_size_t objectSize(const ObjHeader* obj) {
  const TypeInfo* type_info = obj->type_info();
  container_size_t size = (type_info->instanceSize_ < 0 ?
      // An array.
      arrayObjectSize(obj->array())
      :
      type_info->instanceSize_);
  return alignUp(size, kObjectAlignment);
}

template <typename func>
inline void traverseObjectFields(ObjHeader* obj, func process) {
  const TypeInfo* typeInfo = obj->type_info();
  if (typeInfo != theArrayTypeInfo) {
    for (int index = 0; index < typeInfo->objOffsetsCount_; index++) {
      ObjHeader** location = reinterpret_cast<ObjHeader**>(
          reinterpret_cast<uintptr_t>(obj) + typeInfo->objOffsets_[index]);
      process(location);
    }
  } else {
    ArrayHeader* array = obj->array();
    for (uint32_t index = 0; index < array->count_; index++) {
      process(ArrayAddressOfElementAt(array, index));
    }
  }
}
} // namespce
void RTGC_traverseObjectFields(ContainerHeader* container, RTGC_FIELD_TRAVERSE_CALLBACK process) {
  traverseObjectFields((ObjHeader*)(container + 1), [process](ObjHeader** location) {
    ObjHeader* ref = *location;
    if (ref != nullptr) {
      ContainerHeader* container = ref->container();
      if (container != NULL) {
        process(container);
      }
    }
  });
}
namespace {
template <typename func>
inline void traverseReferredObjects(ObjHeader* obj, func process) {
  const TypeInfo* typeInfo = obj->type_info();
  if (typeInfo != theArrayTypeInfo) {
    const int32_t* offsets = typeInfo->objOffsets_;
    for (int index = typeInfo->objOffsetsCount_; --index >= 0; ) {
      ObjHeader** location = reinterpret_cast<ObjHeader**>(
          reinterpret_cast<uintptr_t>(obj) + *offsets++);
      KRef ref = *location;
      if (ref != nullptr) {
        process(ref);
      }
    }
  } else {
    ArrayHeader* array = obj->array();
    KRef* pRef = ArrayAddressOfElementAt(array, 0);
    for (int32_t index = (int32_t)array->count_; --index >= 0;) {
      KRef ref = *pRef ++;
      if (ref != nullptr) {
        process(ref);
      }
    }
  }
}

//*
template <typename func>
inline void traverseContainerObjectFields(ContainerHeader* container, func process) {
  RuntimeAssert(!isAggregatingFrozenContainer(container), "Must not be called on such containers");
  ObjHeader* obj = reinterpret_cast<ObjHeader*>(container + 1);

  RTGC_LOG("traverseContainerObjectFields %p(%d)\n", container, container->objectCount());
  for (uint32_t i = container->objectCount(); i > 0; i--) {
    traverseObjectFields(obj, process);
    obj = reinterpret_cast<ObjHeader*>(
      reinterpret_cast<uintptr_t>(obj) + objectSize(obj));
  }
}

/*/
template <typename func>
inline void traverseContainerObjects_obsolete(ContainerHeader* container, func process) {
  RuntimeAssert(!isAggregatingFrozenContainer(container), "Must not be called on such containers");
  ObjHeader* obj = reinterpret_cast<ObjHeader*>(container + 1);
  for (uint32_t i = 0; i < container->objectCount(); ++i) {
    process(obj);
    obj = reinterpret_cast<ObjHeader*>(
      reinterpret_cast<uintptr_t>(obj) + objectSize(obj));
  }
}

template <typename func>
inline void traverseContainerObjectFields(ContainerHeader* container, func process) {
  traverseContainerObjects_obsolete(container, [process](ObjHeader* obj) {
    traverseObjectFields(obj, process);
  });
}
//*/

template <typename func>
inline void traverseContainerReferredObjects(ContainerHeader* container, func process) {
  traverseContainerObjectFields(container, [process](ObjHeader** location) {
    ObjHeader* ref = *location;
    if (ref != nullptr) process(ref);
  });
}

inline FrameOverlay* asFrameOverlay(ObjHeader** slot) {
  return reinterpret_cast<FrameOverlay*>(slot);
}

inline bool isRefCounted(KConstRef object) {
  return isFreeable(object->container());
}

#if !RTGC
inline void lock(KInt* spinlock) {
  while (compareAndSwap(spinlock, 0, 1) != 0) {}
}

inline void unlock(KInt* spinlock) {
  RuntimeCheck(compareAndSwap(spinlock, 1, 0) == 1, "Must succeed");
}
#endif

inline bool canFreeze(ContainerHeader* container) {
  if (IsStrictMemoryModel)
    // In strict memory model we ignore permanent, frozen and shared object when recursively freezing.
    return container != nullptr && !container->shared() && !container->frozen();
  else
    // In relaxed memory model we ignore permanent and frozen object when recursively freezing.
    return container != nullptr && !container->frozen_or_freezing();
}

inline bool isFreezableAtomic(ObjHeader* obj) {
  return obj->type_info() == theFreezableAtomicReferenceTypeInfo;
}

inline bool isFreezableAtomic(ContainerHeader* container) {
  RuntimeAssert(!isAggregatingFrozenContainer(container), "Must be single object");
  ObjHeader* obj = reinterpret_cast<ObjHeader*>(container + 1);
  return isFreezableAtomic(obj);
}

ContainerHeader* allocContainer(MemoryState* state, size_t size) {
 ContainerHeader* result = nullptr;
#if USE_GC
  // We recycle elements of finalizer queue for new allocations, to avoid trashing memory manager.
  ContainerHeader* container = state != nullptr ? state->finalizerQueue : nullptr;
  ContainerHeader* previous = nullptr;
  while (container != nullptr) {
    // TODO: shall it be == instead?
    if (container->hasContainerSize() &&
        container->containerSize() >= size && container->containerSize() <= size + 16) {
      MEMORY_LOG("recycle %p for request %d\n", container, size)
      result = container;
      if (previous == nullptr)
        state->finalizerQueue = container->nextLink();
      else
        previous->setNextLink(container->nextLink());
      state->finalizerQueueSize--;
      memset(container, 0, size);
      break;
    }
    previous = container;
    container = container->nextLink();
  }
#endif
  if (result == nullptr) {
#if USE_GC
    if (state != nullptr)
        state->allocSinceLastGc += size;
#endif
    result = konanConstructSizedInstance<ContainerHeader>(alignUp(size, kObjectAlignment));
    atomicAdd(&allocCount, 1);
  }
  if (state != nullptr) {
    CONTAINER_ALLOC_EVENT(state, size, result);
#if TRACE_MEMORY
    state->containers->insert(result);
#endif
  }
  return result;
}

ContainerHeader* allocAggregatingFrozenContainer(KStdVector<ContainerHeader*>& containers) {
  auto componentSize = containers.size();
  auto* superContainer = allocContainer(memoryState, sizeof(ContainerHeader) + sizeof(void*) * componentSize);
  auto* place = reinterpret_cast<ContainerHeader**>(superContainer + 1);
  for (auto* container : containers) {
    *place++ = container;
    // Set link to the new container.
    auto* obj = reinterpret_cast<ObjHeader*>(container + 1);
    obj->setContainer(superContainer);
    MEMORY_LOG("Set fictitious frozen container for %p: %p\n", obj, superContainer);
  }
  superContainer->setObjectCount(componentSize);
  superContainer->freezeRef();
  return superContainer;
}


#if USE_GC
void processFinalizerQueue(MemoryState* state) {
  // TODO: reuse elements of finalizer queue for new allocations.
  RTGC_LOG("Processing FinalizerQ\n");
  while (state->finalizerQueue != nullptr) {
    auto* container = state->finalizerQueue;

    state->finalizerQueue = container->nextLink();
    state->finalizerQueueSize--;
#if TRACE_MEMORY
    state->containers->erase(container);
#endif
    CONTAINER_DESTROY_EVENT(state, container)
    konanFreeMemory(container);
    atomicAdd(&allocCount, -1);
  }
  RuntimeAssert(state->finalizerQueueSize == 0, "Queue must be empty here");
  RTGC_LOG("Processing FinalizerQ done\n");
}


#ifdef RTGC    

bool hasExternalRefs(ContainerHeader* start, ContainerHeaderDeque* visited) {
  RTGC_TRAP("checking foreign refs of %p\n", start);

  ContainerHeaderDeque toVisit;
  bool hasExternalRefs = false;
  start->attachNode();
  start->mark();
  toVisit.push_back(start);

  for (size_t idx = 0; idx < toVisit.size(); idx ++) {
    auto* container = toVisit[idx];

    traverseContainerReferredObjects(container, [&toVisit](ObjHeader* ref) {
      auto* child = ref->container();
      if (tryMakeShareable(child)) return;

      if (child->isAcyclic()) {
        child->incMemberRefCount<false>(true);
      }
      if (!child->marked()) {
        RTGC_TRAP("** push %p (%d) toVisit\n", child, child->isAcyclic());
        child->mark();
        toVisit.push_back(child);
      }
    });
  }

  for (auto* it: toVisit) {
    if (it->isAcyclic()) {
      RTGCRef ref = it->getRTGCRef();
      RuntimeAssert(ref.obj <= ref.root, "RefCount mismatch");
      if (ref.obj != ref.root && it != start) {
        hasExternalRefs |= !tryMakeShareable(it);
        RTGC_TRAP("acyclic mismatch %p(%d) %d %d\n", it, hasExternalRefs, it->getRTGCRef().obj, it->getRTGCRef().root);
      }
      it->clearMemberRefCount();
    }
    else if (!hasExternalRefs) {
      GCRefChain* chain = it->getNode()->externalReferrers.topChain();
      for (; chain != NULL; chain = chain->next()) {
        ContainerHeader* referrer = chain->obj();
        if (!referrer->marked() && !tryMakeShareable(referrer)) {
          RTGC_TRAP("%p has foreign ref=%p\n", it, referrer);
          hasExternalRefs = true;
          break;
        }
      }
    }
  }

  for (auto* it: toVisit) {
    it->unMark();
  }

  return hasExternalRefs;
}
#else
bool hasExternalRefs(ContainerHeader* start, ContainerHeaderSet* visited) {
  ContainerHeaderDeque toVisit;
  toVisit.push_back(start);
  while (!toVisit.empty()) {
    auto* container = toVisit.front();
    toVisit.pop_front();
    visited->insert(container);
    if (container->refCount() > 0) {
      MEMORY_LOG("container %p with rc %d blocks transfer\n", container, container->refCount())
      return true;
    }
    traverseContainerReferredObjects(container, [&toVisit, visited](ObjHeader* ref) {
        auto* child = ref->container();
        if (!isShareable(child) && (visited->count(child) == 0)) {
           toVisit.push_front(child);
        }
     });
  }
  return false;
}
#endif


#endif  // USE_GC

void scheduleDestroyContainer(MemoryState* state, ContainerHeader* container, const char* msg = "free") {
  RTGC_LOG("scheduleDestroyContainer %p isEnqueued=%d %s\n", container, container->isEnquedCyclicTest(), msg);
  if (true) {
    if (RTGC_LATE_DESTROY_CYCLIC_SUSPECT && container->isEnquedCyclicTest()) {
      return;
    }

    bool isShared = false; // container->shared();
    OnewayNode* node = container->getLocalOnewayNode();
    if (isShared) GCNode::rtgcLock(_FreeContainer);
    if (!RTGC_LATE_DESTROY_CYCLIC_SUSPECT) {
      CyclicNode::removeCyclicTest(state, container);
    }
    if (node != NULL) {
      node->dealloc();
    }
    if (isShared) GCNode::rtgcUnlock();
  }
#if USE_GC
  RuntimeAssert(container != nullptr, "Cannot destroy null container");

  container->setNextLink(state->finalizerQueue);
  state->finalizerQueue = container;
  state->finalizerQueueSize++;
  // We cannot clean finalizer queue while in GC.
  if (!state->gcInProgress && state->finalizerQueueSuspendCount == 0 &&
      state->finalizerQueueSize >= kFinalizerQueueThreshold) {
    RTGC_LOG("scheduleDestroyContainer finalize %p\n", container);
    processFinalizerQueue(state);
  }
#else
  konanFreeMemory(container);
  atomicAdd(&allocCount, -1);
  CONTAINER_DESTROY_EVENT(state, container);
#endif
}

void freeAggregatingFrozenContainer(ContainerHeader* container) {
  auto* state = memoryState;
  RuntimeAssert(isAggregatingFrozenContainer(container), "expected fictitious frozen container");
  MEMORY_LOG("%p is fictitious frozen container\n", container);
  RuntimeAssert(!container->buffered(), "frozen objects must not participate in GC")
#if USE_GC
  // Forbid finalizerQueue handling.
  ++state->finalizerQueueSuspendCount;
#endif
  // Special container for frozen objects.
  ContainerHeader** subContainer = reinterpret_cast<ContainerHeader**>(container + 1);
  MEMORY_LOG("Total subcontainers = %d\n", container->objectCount());
  for (uint32_t i = 0; i < container->objectCount(); ++i) {
    RTGC_LOG("Freeing subcontainer %p\n", *subContainer);
    freeContainer(*subContainer++);
  }
#if USE_GC
  --state->finalizerQueueSuspendCount;
#endif
  scheduleDestroyContainer(state, container);
  MEMORY_LOG("Freeing subcontainers done\n");
}

// This is called from 2 places where it's unconditionally called,
// so better be inlined.
ALWAYS_INLINE void runDeallocationHooks(ContainerHeader* container, ForeignRefManager* manager) {
  ObjHeader* obj = reinterpret_cast<ObjHeader*>(container + 1);

  for (uint32_t index = 0; index < container->objectCount(); index++) {
    auto* type_info = obj->type_info();
    if (type_info == theWorkerBoundReferenceTypeInfo) {
      RTGC_LOG("## runDeallocationHooks-DisposeWorkerBoundReference %p\n", obj);
      DisposeWorkerBoundReference(obj);
      RTGC_LOG("## runDeallocationHooks-DisposeWorkerBoundReference done %p\n", obj);
    }
#if USE_CYCLIC_GC
    if ((type_info->flags_ & TF_LEAK_DETECTOR_CANDIDATE) != 0) {
      cyclicRemoveAtomicRoot(obj);
    }
#endif  // USE_CYCLIC_GC
#if USE_CYCLE_DETECTOR
    CycleDetector::removeCandidateIfNeeded(obj);
#endif  // USE_CYCLE_DETECTOR
    if (obj->has_meta_object()) {
      RTGC_LOG("## runDeallocationHooks-destroyMetaObject %p\n", obj);
      ObjHeader::destroyMetaObject(&obj->typeInfoOrMeta_, manager);
      RTGC_LOG("## runDeallocationHooks-destroyMetaObject done %p\n", obj);
    }
    obj = reinterpret_cast<ObjHeader*>(reinterpret_cast<uintptr_t>(obj) + objectSize(obj));
  }
}
} // namespace
int decrementMemberRC_internal(ContainerHeader* deassigned, ContainerHeader* owner);

void freeContainer(ContainerHeader* container, int garbageNodeId) {
  RuntimeAssert(container != nullptr, "this kind of container shalln't be freed");

  if (isAggregatingFrozenContainer(container)) {
    freeAggregatingFrozenContainer(container);
    return;
  }

  const bool RTGC_LATE_DESTORY = false;

  RTGC_LOG("## RTGC free container %p/%d %p freeable=%d\n", container, garbageNodeId, memoryState, container->freeable());
  bool isRoot = memoryState->gcInProgress ++ == 0;
  auto toRelease = memoryState->toRelease;
  runDeallocationHooks(container, nullptr);

  bool doFree = container->freeable();
  if (RTGC && doFree) {

      container->markDestroyed();
      ContainerHeader* owner = container;
      DebugAssert(container->objectCount() == 1);
      bool isOwnerPushed = isRoot;
      while (true) {//garbageNodeId == 0) {
        // traverseContainerObjectFields(owner, [garbageNodeId, toRelease, &isOwnerPushed, owner](ObjHeader** location) {
        //   ObjHeader* old = *location;
        //   if (old == NULL) return;
        traverseReferredObjects((ObjHeader*)(owner+1), [garbageNodeId, toRelease, &isOwnerPushed, owner](ObjHeader* old) {
          ContainerHeader* deassigned = old->container();
          DebugAssert(deassigned == NULL || deassigned->objectCount() == 1);
          RTGC_LOG_V("--- cleaning fields start %p(%p) IN %p(%d)\n", deassigned, old, owner, garbageNodeId);
          if (isFreeable(deassigned)) {
            //*location = NULL;
            if (garbageNodeId != 0) {
              if (deassigned->getNodeId() == garbageNodeId) {
                RTGC_LOG_V("--- cleaning fields in cyclicNode %p (%d)\n", deassigned, garbageNodeId);
                freeContainer(deassigned, garbageNodeId);
              }
              else {
                RTGC_LOG_V("--- cleaning fields Node %p (%d)\n", deassigned, garbageNodeId);
                if (RTGC_LATE_DESTORY) {
                  if (!isOwnerPushed) {
                    isOwnerPushed = true;
                    toRelease->push_back((ContainerHeader*)((char*)owner + 1));
                  }
                  toRelease->push_back(deassigned);
                }
                else {
                  updateHeapRef_internal(NULL, old, (ObjHeader*)(owner + 1));
                }
              }
            } else {
              RTGC_LOG_V("--- cleaning fields any %p (%d)\n", deassigned, garbageNodeId);
              if (RTGC_LATE_DESTORY) {
                if (!isOwnerPushed) {
                  isOwnerPushed = true;
                  toRelease->push_back((ContainerHeader*)((char*)owner + 1));
                }
                toRelease->push_back(deassigned);
              }
              else {
                updateHeapRef_internal(NULL, old, (ObjHeader*)(owner + 1));
              }
            }
          }
          RTGC_LOG_V("--- cleaning fields done %p (%d)\n", old, garbageNodeId);
        });
        if (RTGC_LATE_DESTORY) {
          if (!isRoot) {
            break;
          }
          while (!toRelease->empty()) {
            ContainerHeader* old = toRelease->front();
            toRelease->pop_front();
            if (((int64_t)old & 1) != 0) {
              owner = (ContainerHeader*)((int64_t)old & ~1);
              continue;
            }
            if (old->freeable()) {
              decrementMemberRC_internal(old, owner);
            }
          }
        }        
        break;
      }
  }
  else {
    // Now let's clean all object's fields in this container.
    traverseContainerObjectFields(container, [](ObjHeader** location) {
          RTGC_LOG_V("--- cleaning not freeable %p\n", location);
          ZeroHeapRef(location);
    });
  }

  RTGC_LOG_V("--- free container check free %p\n", container);
  // And release underlying memory.
  memoryState->gcInProgress --;
  if (doFree) {
#if !RTGC
    container->setColorEvenIfGreen(CONTAINER_TAG_GC_BLACK);
#endif
    if (RTGC || !container->buffered()) {
      scheduleDestroyContainer(memoryState, container);
    }
  }

  RTGC_LOG_V("## RTGC free container done %p(%d) gcDepth=(%d)\n", container, garbageNodeId, memoryState->gcInProgress);
}

namespace {
/**
  * Do DFS cycle detection with three colors:
  *  - 'marked' bit as BLACK marker (object and its descendants processed)
  *  - 'seen' bit as GRAY marker (object is being processed)
  *  - not 'marked' and not 'seen' as WHITE marker (object is unprocessed)
  * When we see GREY during DFS, it means we see cycle.
  */
 #ifndef RTGC
void depthFirstTraversal(ContainerHeader* start, bool* hasCycles,
                         KRef* firstBlocker, KStdVector<ContainerHeader*>* order) {
  ContainerHeaderDeque toVisit;
  toVisit.push_back(start);
  start->setSeen();

  while (!toVisit.empty()) {
    auto* container = toVisit.front();
    toVisit.pop_front();
    if (isMarkedAsRemoved(container)) {
      container = clearRemoved(container);
      // Mark BLACK.
      container->resetSeen();
      container->mark();
      order->push_back(container);
      continue;
    }
    toVisit.push_front(markAsRemoved(container));
    traverseContainerReferredObjects(container, [container, hasCycles, firstBlocker, &toVisit](ObjHeader* obj) {
      if (*firstBlocker != nullptr)
        return;
      if (obj->has_meta_object() && ((obj->meta_object()->flags_ & MF_NEVER_FROZEN) != 0)) {
          *firstBlocker = obj;
          return;
      }
      ContainerHeader* objContainer = obj->container();
      if (canFreeze(objContainer)) {
        // Marked GREY, there's cycle.
        if (objContainer->seen()) *hasCycles = true;

        // Go deeper if WHITE.
        if (!objContainer->seen() && !objContainer->marked()) {
          // Mark GRAY.
          objContainer->setSeen();
          // Here we do rather interesting trick: when doing DFS we postpone processing references going from
          // FreezableAtomic, so that in 'order' referred value will be seen as not actually belonging
          // to the same SCC (unless there are other edges not going through FreezableAtomic reaching the same value).
          if (isFreezableAtomic(container)) {
            toVisit.push_back(objContainer);
          } else {
            toVisit.push_front(objContainer);
          }
        }
      }
    });
  }
}
#endif
void traverseStronglyConnectedComponent(ContainerHeader* start,
                                        KStdUnorderedMap<ContainerHeader*,
                                            KStdVector<ContainerHeader*>> const* reversedEdges,
                                        KStdVector<ContainerHeader*>* component) {
  ContainerHeaderDeque toVisit;
  toVisit.push_back(start);
  start->mark();

  while (!toVisit.empty()) {
    auto* container = toVisit.front();
    toVisit.pop_front();
    component->push_back(container);
    auto it = reversedEdges->find(container);
    RuntimeAssert(it != reversedEdges->end(), "unknown node during condensation building");
    for (auto* nextContainer : it->second) {
      if (!nextContainer->marked()) {
          nextContainer->mark();
          toVisit.push_front(nextContainer);
      }
    }
  }
}

#if RTGC

template <bool Atomic>
inline void incrementAcyclicRC(ContainerHeader* container) {
  container->incRefCount<Atomic>();
}

template <bool Atomic>
inline void incrementRC(ContainerHeader* container) {
  if (Atomic) GCNode::rtgcLock(_IncrementRC);
  do {
    RTGCRef ref = container->incRootCount<false>();
    if (ref.root != 1) break;

    CyclicNode* cyclic = CyclicNode::getNode(container);
    if (cyclic != NULL) {
      cyclic->incRootObjectCount<false>();
    }
  } while(false);
  if (Atomic) GCNode::rtgcUnlock();
}

template <bool Atomic, bool UseCycleCollector>
inline int decrementRCtoZero(ContainerHeader* container) {
  if (Atomic) GCNode::rtgcLock(_DecrementRC);
  int freeNode = 0;
  RTGCRef ref = container->decRootCount<false>();
  if (ref.root == 0) {
    CyclicNode* cyclic = CyclicNode::getNode(container);
    if (cyclic != NULL &&
        0 == cyclic->decRootObjectCount<false>() &&  
        cyclic->externalReferrers.isEmpty()) {
      freeNode = ref.node;
    }
    else if (ref.obj == 0) {
      freeNode = -1;
    }
  }
  if (Atomic) GCNode::rtgcUnlock();
  return freeNode;
}

inline void checkGrabage(ContainerHeader* container, int freeNode) {
  if (freeNode != 0) {    
    freeContainer(container, freeNode);
    if (freeNode > 1) {
      CyclicNode::getNode(freeNode)->dealloc();
    }
  }
}

template <bool Atomic, bool UseCycleCollector>
inline void decrementRC(ContainerHeader* container) {
  int freeNode = decrementRCtoZero<Atomic, UseCycleCollector>(container);
  checkGrabage(container, freeNode);
}

template <bool Atomic>
inline int decrementAcyclicRCtoZero(ContainerHeader* container) {
  int64_t rc = container->decRefCount<Atomic>();
  return rc == 0 ? -1 : 0;
}

template <bool Atomic>
inline void decrementAcyclicRC(ContainerHeader* container) {
  if (decrementAcyclicRCtoZero<Atomic>(container) != 0) {
    if (Atomic) GCNode::rtgcLock(_DecrementAcyclicRC);
    checkGrabage(container, -1);
    if (Atomic) GCNode::rtgcUnlock();
  }
}

#if !RTGC
inline void decrementRC(ContainerHeader* container) {
  if (isShareable(container))
    decrementRC<true, false>(container);
  else
    decrementRC<false, false>(container);
}
#endif

template <bool Atomic>
void incrementMemberRC(ContainerHeader* container, ContainerHeader* owner);


template <bool Atomic>
void incrementMemberRC(ContainerHeader* container, ContainerHeader* owner) {
  // @zee rootRef 변경으로 인해 Atomic 처리 필요.
  GCNode* val_node;
  GCNode* owner_node = owner->attachNode();

  MEMORY_LOG("incrementMemberRC %p: rc=%d\n", container, container->refCount() + RTGC_MEMBER_REF_INCREEMENT);

  if (!container->isGCNodeAttached()) {
    val_node = container->attachNode();
    container->incMemberRefCount<false>();
  }
  else {
    val_node = container->getNode();
    container->incMemberRefCount<Atomic>();
    if (val_node == owner_node) {
      return;
    }
    
    if (!container->isEnquedCyclicTest()) {
      bool check_two_way_link = true;
      if (check_two_way_link && owner_node->externalReferrers.find(container)) {
        CyclicNode::createTwoWayLink(owner, container);
        return;
      }
      else if (val_node->externalReferrers.isEmpty() &&
              !owner_node->externalReferrers.isEmpty()) {
        CyclicNode::addCyclicTest(container, true);
      }
    }
  }
  val_node->externalReferrers.push(owner);
}

template <bool Atomic>
void decrementMemberRC(ContainerHeader* container, ContainerHeader* owner) RTGC_NO_INLINE;

template <bool Atomic>
int decrementMemberRCtoZero(ContainerHeader* container, ContainerHeader* owner) {
  GCNode* owner_node = owner->getNode();
  GCNode* val_node = container->getNode();

  container->decMemberRefCount<Atomic>();
  MEMORY_LOG("decrementMemberRC %p: rc=%x\n", container, container->refCount());

  if (val_node != owner_node) {
    val_node = container->getNode();
    owner_node = owner->getNode();
    if (container->isInCyclicNode()) {
      MEMORY_LOG("## RTGC remove referrer of cyclic node %p: %p\n", container, container->getNodeId());
    }
    val_node->externalReferrers.remove(owner);
  }
  else {
    CyclicNode* cyclic = container->getLocalCyclicNode();
    RuntimeAssert(cyclic != NULL, "no cylic node");
    if (container->isGarbage()) {
      cyclic->removeSuspectedGarbage(container);
      return -1;
    }
    else {
      cyclic->markSuspectedGarbage(container);
      return 0;
    }
  }

  if (container->isGarbage()) {
    return -1;
  }
  

  if (container->isInCyclicNode()) {
    CyclicNode* cyclic = (CyclicNode*)val_node;
    if (cyclic->isCyclicGarbage()) {
      return container->getNodeId();
    }
  }

  if (!container->isEnquedCyclicTest() &&
    !val_node->externalReferrers.isEmpty()) {
      CyclicNode::addCyclicTest(container, false);
  }
  return 0;
}

template <bool Atomic>
void decrementMemberRC(ContainerHeader* container, ContainerHeader* owner) {
  int freeNode = decrementMemberRCtoZero<Atomic>(container, owner);
  checkGrabage(container, freeNode);
}

#elif !USE_GC

template <bool Atomic>
inline void incrementRC(ContainerHeader* container) {
  container->incRefCount<Atomic>();
}

template <bool Atomic, bool UseCycleCollector>
inline void decrementRC(ContainerHeader* container) {
  if (container->decRefCount<Atomic>() == 0) {
    freeContainer(container);
  }
}

inline void decrementRC(ContainerHeader* container) {
  if (isShareable(container))
    decrementRC<true, false>(container);
  else
    decrementRC<false, false>(container);
}

template <bool CanCollect>
inline void enqueueDecrementRC(ContainerHeader* container) {
  RuntimeCheck(false, "Not yet implemeneted");
}

#else // USE_GC

template <bool Atomic>
inline void incrementRC(ContainerHeader* container) {
  container->incRefCount<Atomic>();
}

template <bool Atomic, bool UseCycleCollector>
inline void decrementRC(ContainerHeader* container) {
  // TODO: enable me, once account for inner references in frozen objects correctly.
  // RuntimeAssert(container->refCount() > 0, "Must be positive");
  if (container->decRefCount<Atomic>() == 0) {
    freeContainer(container);
  } else if (UseCycleCollector) { // Possible root.
    RuntimeAssert(container->refCount() > 0, "Must be positive");
    RuntimeAssert(!Atomic && !container->shared(), "Cycle collector shalln't be used with shared objects yet");
    RuntimeAssert(container->objectCount() == 1, "cycle collector shall only work with single object containers");
    // We do not use cycle collector for frozen objects, as we already detected
    // possible cycles during freezing.
    // Also do not use cycle collector for provable acyclic objects.
    int color = container->color();
    if (color != CONTAINER_TAG_GC_PURPLE && color != CONTAINER_TAG_GC_GREEN) {
      container->setColorAssertIfGreen(CONTAINER_TAG_GC_PURPLE);
      if (!container->buffered()) {
        auto* state = memoryState;
        container->setBuffered();
        if (state->toFree != nullptr) {
          state->toFree->push_back(container);
          MEMORY_LOG("toFree is now %d\n", state->toFree->size())
          if (state->gcSuspendCount == 0 && state->toRelease->size() >= state->gcThreshold) {
            GC_LOG("Calling GC from DecrementRC: %d\n", state->toRelease->size())
            garbageCollect(state, false);
          }
        }
      }
    }
  }
}

inline void decrementRC(ContainerHeader* container) {
  auto* state = memoryState;
  RuntimeAssert(!IsStrictMemoryModel || state->gcInProgress, "Must only be called during GC");
  // TODO: enable me, once account for inner references in frozen objects correctly.
  // RuntimeAssert(container->refCount() > 0, "Must be positive");
  bool useCycleCollector = container->local();
  if (container->decRefCount() == 0) {
    freeContainer(container);
  } else if (useCycleCollector && state->toFree != nullptr) {
      RuntimeAssert(IsStrictMemoryModel, "No cycle collector in relaxed mode yet");
      RuntimeAssert(container->refCount() > 0, "Must be positive");
      RuntimeAssert(!container->sharead(), "Cycle collector shalln't be used with shared objects yet");
      RuntimeAssert(container->objectCount() == 1, "cycle collector shall only work with single object containers");
      // We do not use cycle collector for frozen objects, as we already detected
      // possible cycles during freezing.
      // Also do not use cycle collector for provable acyclic objects.
      int color = container->color();
      if (color != CONTAINER_TAG_GC_PURPLE && color != CONTAINER_TAG_GC_GREEN) {
        container->setColorAssertIfGreen(CONTAINER_TAG_GC_PURPLE);
        if (!container->buffered()) {
          container->setBuffered();
          state->toFree->push_back(container);
        }
      }
  }
}

#endif

template <bool Atomic>
inline bool tryIncrementRC(ContainerHeader* container) {
#if !RTGC  
  return container->tryIncRefCount<Atomic>();
#else
    bool res = false;
    if (Atomic) GCNode::rtgcLock(_TryIncrementRC);
      // Note: tricky case here is doing this during cycle collection.
      // This can actually happen due to deallocation hooks.
      // Fortunately by this point reference counts have been made precise again.
    if (container->refCount() > 0) {
      CyclicNode* c = container->getLocalCyclicNode();
      if (c == nullptr || !c->isCyclicGarbage()) {
        if (container->isAcyclic()) {
          incrementAcyclicRC<false>(container);
        }
        else { 
          incrementRC<false>(container);
        }
        res = true;
      }
    } 

    if (Atomic) GCNode::rtgcUnlock();
    return res;

#endif
}


#ifdef USE_GC

template <bool CanCollect>
inline void enqueueDecrementRC(ContainerHeader* container) {
#if !RTGC  
  auto* state = memoryState;
  if (CanCollect) {
    if (state->toRelease->size() >= state->gcThreshold && state->gcSuspendCount == 0) {
      GC_LOG("Calling GC from EnqueueDecrementRC: %d\n", state->toRelease->size())
      garbageCollect(state, false);
    }
  }
  state->toRelease->push_back(container);
#endif  
}

inline void initGcThreshold(MemoryState* state, uint32_t gcThreshold) {
  state->gcThreshold = gcThreshold;
  //state->toRelease->reserve(gcThreshold);
}

inline void initGcCollectCyclesThreshold(MemoryState* state, uint64_t gcCollectCyclesThreshold) {
  state->gcCollectCyclesThreshold = gcCollectCyclesThreshold;
  state->toFree->reserve(gcCollectCyclesThreshold);
}

inline void increaseGcThreshold(MemoryState* state) {
  auto newThreshold = state->gcThreshold * 3 / 2 + 1;
  if (newThreshold <= kMaxErgonomicThreshold) {
    initGcThreshold(state, newThreshold);
  }
}

inline void increaseGcCollectCyclesThreshold(MemoryState* state) {
  auto newThreshold = state->gcCollectCyclesThreshold * 2;
  if (newThreshold <= kMaxErgonomicToFreeSizeThreshold) {
    initGcCollectCyclesThreshold(state, newThreshold);
  }
}

#endif // USE_GC

#if TRACE_MEMORY && USE_GC

//const char* colorNames[] = {"BLACK", "GRAY", "WHITE", "PURPLE", "GREEN", "ORANGE", "RED"};

void dumpObject(ObjHeader* ref, int indent) {
  for (int i = 0; i < indent; i++) MEMORY_LOG(" ");
  auto* typeInfo = ref->type_info();
  auto* packageName =
    typeInfo->packageName_ != nullptr ? CreateCStringFromString(typeInfo->packageName_) : nullptr;
  auto* relativeName =
    typeInfo->relativeName_ != nullptr ? CreateCStringFromString(typeInfo->relativeName_) : nullptr;
  MEMORY_LOG("%p %s.%s\n", ref,
    packageName ? packageName : "<unknown>", relativeName ? relativeName : "<unknown>");
  if (packageName) konan::free(packageName);
  if (relativeName) konan::free(relativeName);
}

void dumpContainerContent(ContainerHeader* container) {
  if (container->refCount() < 0) {
    MEMORY_LOG("%p has negative RC %d, likely a memory bug\n", container, container->refCount())
    return;
  }
  if (isAggregatingFrozenContainer(container)) {
    MEMORY_LOG("aggregating container %p with %d objects rc=%d\n",
               //colorNames[container->color()], 
               container, container->objectCount(), container->refCount());
    ContainerHeader** subContainer = reinterpret_cast<ContainerHeader**>(container + 1);
    for (uint32_t i = 0; i < container->objectCount(); ++i) {
      ContainerHeader* sub = *subContainer++;
      MEMORY_LOG("    container %p\n ", sub);
      dumpContainerContent(sub);
    }
  } else {
    MEMORY_LOG("regular %s%scontainer %p with %d objects rc=%d\n",
               //colorNames[container->color()],
               container->frozen() ? "frozen " : "",
               !container->freeable() ? "!freeable " : "",
               container, container->objectCount(),
               container->refCount());
    ObjHeader* obj = reinterpret_cast<ObjHeader*>(container + 1);
    dumpObject(obj, 4);
  }
}

void dumpWorker(const char* prefix, ContainerHeader* header, ContainerHeaderSet* seen) {
  dumpContainerContent(header);
  seen->insert(header);
  if (!isAggregatingFrozenContainer(header)) {
    traverseContainerReferredObjects(header, [prefix, seen](ObjHeader* ref) {
      auto* child = ref->container();
      #if !RTGC
      RuntimeAssert(!isArena(child), "A reference to local object is encountered");
      #endif
      if (child != nullptr && (seen->count(child) == 0)) {
        dumpWorker(prefix, child, seen);
      }
    });
  }
}

void dumpReachable(const char* prefix, const ContainerHeaderSet* roots) {
  ContainerHeaderSet seen;
  for (auto* container : *roots) {
    dumpWorker(prefix, container, &seen);
  }
}

#endif

#if USE_GC

void markRoots(MemoryState*);
void scanRoots(MemoryState*);
void collectRoots(MemoryState*);
void scan(ContainerHeader* container);

template <bool useColor>
void markGray(ContainerHeader* start) {
#if !RTGC

  ContainerHeaderDeque toVisit;
  toVisit.push_front(start);

  while (!toVisit.empty()) {
    auto* container = toVisit.front();
    MEMORY_LOG("MarkGray visit %p [%s]\n", container, colorNames[container->color()]);
    toVisit.pop_front();
    if (useColor) {
      int color = container->color();
      if (color == CONTAINER_TAG_GC_GRAY) continue;
      // If see an acyclic object not being garbage - ignore it. We must properly traverse garbage, although.
      if (color == CONTAINER_TAG_GC_GREEN && container->refCount() != 0) {
        continue;
      }
      // Only garbage green object could be recolored here.
      container->setColorEvenIfGreen(CONTAINER_TAG_GC_GRAY);
    } else {
      if (container->marked()) continue;
      container->mark();
    }

    traverseContainerReferredObjects(container, [&toVisit](ObjHeader* ref) {
      auto* childContainer = ref->container();
      RuntimeAssert(!isArena(childContainer), "A reference to local object is encountered");
      if (!isShareable(childContainer)) {
        childContainer->decRefCount<false>();
        toVisit.push_front(childContainer);
      }
    });
  }
#endif
}

template <bool useColor>
void scanBlack(ContainerHeader* start) {
#if !RTGC

  ContainerHeaderDeque toVisit;
  toVisit.push_front(start);
  while (!toVisit.empty()) {
    auto* container = toVisit.front();
    MEMORY_LOG("ScanBlack visit %p [%s]\n", container, colorNames[container->color()]);
    toVisit.pop_front();
    if (useColor) {
      auto color = container->color();
      if (color == CONTAINER_TAG_GC_GREEN || color == CONTAINER_TAG_GC_BLACK) continue;
      container->setColorAssertIfGreen(CONTAINER_TAG_GC_BLACK);
    } else {
      if (!container->marked()) continue;
      container->unMark();
    }
    traverseContainerReferredObjects(container, [&toVisit](ObjHeader* ref) {
        auto childContainer = ref->container();
        RuntimeAssert(!isArena(childContainer), "A reference to local object is encountered");
        if (!isShareable(childContainer)) {
          childContainer->incRefCount<false>();
          if (useColor) {
            int color = childContainer->color();
            if (color != CONTAINER_TAG_GC_BLACK)
              toVisit.push_front(childContainer);
          } else {
            if (childContainer->marked())
              toVisit.push_front(childContainer);
          }
        }
    });
  }
#endif
}

void collectWhite(MemoryState*, ContainerHeader* container);

void collectCycles(MemoryState* state) {
  markRoots(state);
  scanRoots(state);
  collectRoots(state);
  state->toFree->clear();
  state->roots->clear();
}

void markRoots(MemoryState* state) {
#if !RTGC
  for (auto container : *(state->toFree)) {
    if (isMarkedAsRemoved(container))
      continue;
    // Acyclic containers cannot be in this list.
    RuntimeCheck(container->color() != CONTAINER_TAG_GC_GREEN, "Must not be green");
    auto color = container->color();
    auto rcIsZero = container->refCount() == 0;
    if (color == CONTAINER_TAG_GC_PURPLE && !rcIsZero) {
      markGray<true>(container);
      state->roots->push_back(container);
    } else {
      container->resetBuffered();
      RuntimeAssert(color != CONTAINER_TAG_GC_GREEN, "Must not be green");
      if (color == CONTAINER_TAG_GC_BLACK && rcIsZero) {
        scheduleDestroyContainer(state, container);
      }
    }
  }
#endif
}

void scanRoots(MemoryState* state) {
  for (auto* container : *(state->roots)) {
    scan(container);
  }
}

void collectRoots(MemoryState* state) {
  // Here we might free some objects and call deallocation hooks on them,
  // which in turn might call DecrementRC and trigger new GC - forbid that.
  state->gcSuspendCount++;
  for (auto* container : *(state->roots)) {
    container->resetBuffered();
    collectWhite(state, container);
  }
  state->gcSuspendCount--;
}

void scan(ContainerHeader* start) {
#if !RTGC
  ContainerHeaderDeque toVisit;
  toVisit.push_front(start);

  while (!toVisit.empty()) {
     auto* container = toVisit.front();
     toVisit.pop_front();
     if (container->color() != CONTAINER_TAG_GC_GRAY) continue;
     if (container->refCount() != 0) {
       scanBlack<true>(container);
       continue;
     }
     container->setColorAssertIfGreen(CONTAINER_TAG_GC_WHITE);
     traverseContainerReferredObjects(container, [&toVisit](ObjHeader* ref) {
       auto* childContainer = ref->container();
       RuntimeAssert(!isArena(childContainer), "A reference to local object is encountered");
       if (!isShareable(childContainer)) {
         toVisit.push_front(childContainer);
       }
     });
   }
#endif   
}

void collectWhite(MemoryState* state, ContainerHeader* start) {
#if !RTGC
   ContainerHeaderDeque toVisit;
   toVisit.push_back(start);

   while (!toVisit.empty()) {
     auto* container = toVisit.front();
     toVisit.pop_front();
     if (container->color() != CONTAINER_TAG_GC_WHITE || container->buffered()) continue;
     container->setColorAssertIfGreen(CONTAINER_TAG_GC_BLACK);
     traverseContainerObjectFields(container, [&toVisit](ObjHeader** location) {
        auto* ref = *location;
        if (ref == nullptr) return;
        auto* childContainer = ref->container();
        RuntimeAssert(!isArena(childContainer), "A reference to local object is encountered");
        if (isShareable(childContainer)) {
          ZeroHeapRef(location);
        } else {
          toVisit.push_front(childContainer);
        }
     });
     runDeallocationHooks(container);
     scheduleDestroyContainer(state, container);
  }
#endif
}
#endif

inline bool needAtomicAccess(ContainerHeader* container) {
  return container->shared();
}

inline bool canBeCyclic(ContainerHeader* container) {
#if !RTGC
  if (container->refCount() == 1) return false;
  if (container->color() == CONTAINER_TAG_GC_GREEN) return false;
#endif
  return true;
}

inline void retainRef(const ObjHeader* object) {
  auto* container = object->container();
  if (!isFreeable(container)) {
    return;
  }

  MEMORY_LOG("RetainRef %p: rc=%d\n", container, container->refCount())
  UPDATE_ADDREF_STAT(memoryState, container, needAtomicAccess(container), 0)
  if (container->shared()) {
    if (container->isAcyclic()) {
      incrementAcyclicRC<true>(container);
    }
    else { 
      incrementRC<true>(container);
    }
  }
  else {
    if (container->isAcyclic()) {
      incrementAcyclicRC<false>(container);
    }
    else { 
      incrementRC<false>(container);
    }
  }
}


inline bool tryRetainRef(ContainerHeader* container) {
  if (container->shared()) {
    if (!tryIncrementRC<true>(container)) return false;
  }
  else {
    if (!tryIncrementRC<false>(container)) return false;
  }

  MEMORY_LOG("RetainRef %p: rc=%d\n", container, container->refCount() - 1)
  UPDATE_ADDREF_STAT(memoryState, container, needAtomicAccess(container), 0)
  return true;
}

inline bool tryRetainRef(const ObjHeader* header) {
  auto* container = header->container();
  return (container != nullptr) ? tryRetainRef(container) : true;
}

template <bool Strict>
inline void releaseRef(const ObjHeader* object) {
  auto* container = object->container();
  if (!isFreeable(container)) {
    return;
  }

  MEMORY_LOG("ReleaseRef %p: rc=%d\n", container, container->refCount())
  UPDATE_RELEASEREF_STAT(memoryState, container, needAtomicAccess(container), canBeCyclic(container), 0)
  if (Strict) {
    enqueueDecrementRC</* CanCollect = */ true>(container);
    return;
  }

  if (container->shared()) {
    if (container->isAcyclic()) {
      decrementAcyclicRC<true>(container);
    }
    else { 
      decrementRC<true, false>(container);
    }
  }
  else {
    if (container->isAcyclic()) {
      decrementAcyclicRC<false>(container);
    }
    else { 
      decrementRC<false, false>(container);
    }
  }
}

// We use first slot as place to store frame-local arena container.
// TODO: create ArenaContainer object on the stack, so that we don't
// do two allocations per frame (ArenaContainer + actual container).
inline ArenaContainer* initedArena(ObjHeader** auxSlot) {
  auto frame = asFrameOverlay(auxSlot);
  auto arena = reinterpret_cast<ArenaContainer*>(frame->arena);
  if (!arena) {
    arena = konanConstructInstance<ArenaContainer>();
    MEMORY_LOG("Initializing arena in %p\n", frame)
    arena->Init();
    frame->arena = arena;
  }
  return arena;
}

inline size_t containerSize(const ContainerHeader* container) {
  size_t result = 0;
  const ObjHeader* obj = reinterpret_cast<const ObjHeader*>(container + 1);
  for (uint32_t object = 0; object < container->objectCount(); object++) {
    size_t size = objectSize(obj);
    result += size;
    obj = reinterpret_cast<ObjHeader*>(reinterpret_cast<uintptr_t>(obj) + size);
  }
  return result;
}

#if USE_GC
void incrementStack(MemoryState* state) {
  FrameOverlay* frame = currentFrame;
  while (frame != nullptr) {
    ObjHeader** current = reinterpret_cast<ObjHeader**>(frame + 1) + frame->parameters;
    ObjHeader** end = current + frame->count - kFrameOverlaySlots - frame->parameters;
    while (current < end) {
      ObjHeader* obj = *current++;
      if (obj != nullptr) {
        auto* container = obj->container();
        if (container == nullptr) continue;
        if (container->shared()) {
          incrementRC<true>(container);
        } else {
          incrementRC<false>(container);
        }
      }
    }
    frame = frame->previous;
  }
}

void processDecrements(MemoryState* state) {
#if !RTGC
  RuntimeAssert(IsStrictMemoryModel, "Only works in strict model now");
  auto* toRelease = state->toRelease;
  state->gcSuspendCount++;
  while (toRelease->size() > 0) {
     auto* container = toRelease->back();
     toRelease->pop_back();
     if (isMarkedAsRemoved(container))
       continue;
     if (container->shared())
       container = realShareableContainer(container);
     decrementRC(container);
  }

  state->foreignRefManager->processEnqueuedReleaseRefsWith([](ObjHeader* obj) {
    ContainerHeader* container = obj->container();
    if (container != nullptr) decrementRC(container);
  });
  state->gcSuspendCount--;
#endif
}

void decrementStack(MemoryState* state) {
  RuntimeAssert(IsStrictMemoryModel, "Only works in strict model now");
  state->gcSuspendCount++;
  FrameOverlay* frame = currentFrame;
  while (frame != nullptr) {
    ObjHeader** current = reinterpret_cast<ObjHeader**>(frame + 1) + frame->parameters;
    ObjHeader** end = current + frame->count - kFrameOverlaySlots - frame->parameters;
    while (current < end) {
      ObjHeader* obj = *current++;
      if (obj != nullptr) {
        MEMORY_LOG("decrement stack %p\n", obj)
        auto* container = obj->container();
        if (container != nullptr)
          enqueueDecrementRC</* CanCollect = */ false>(container);
      }
    }
    frame = frame->previous;
  }
  state->gcSuspendCount--;
}

void garbageCollect(MemoryState* state, bool force) {
  RuntimeAssert(!state->gcInProgress, "Recursive GC is disallowed");

#if TRACE_GC
  uint64_t allocSinceLastGc = state->allocSinceLastGc;
#endif  // TRACE_GC
  state->allocSinceLastGc = 0;

  if (RTGC || !IsStrictMemoryModel) {
    RTGC_LOG("garbageCollect %p::%p\n", state, memoryState);
    state->foreignRefManager->processEnqueuedReleaseRefsWith([](ObjHeader* obj) {
        ReleaseRef(obj);
      });
    CyclicNode::garbageCollectCycles(nullptr);
    // GCNode::dumpGCLog();    
    // In relaxed model we just process finalizer queue and be done with it.
    processFinalizerQueue(state);
    return;
  }

  GC_LOG(">>> %s GC: threshold = %d toFree %d toRelease %d alloc = %lld\n", \
     force ? "forced" : "regular", state->gcThreshold, state->toFree->size(),
     state->toRelease->size(), allocSinceLastGc)

  auto gcStartTime = konan::getTimeMicros();

  state->gcInProgress = true;
  state->gcEpoque++;

  incrementStack(state);
#if USE_CYCLIC_GC
  // Block if the concurrent cycle collector is running.
  // We must do that to ensure collector sees state where actual RC properly upper estimated.
  if (g_hasCyclicCollector)
    cyclicLocalGC();
#endif  // USE_CYCLIC_GC
#if PROFILE_GC
  auto processDecrementsStartTime = konan::getTimeMicros();
#endif
  processDecrements(state);
#if PROFILE_GC
  auto processDecrementsDuration = konan::getTimeMicros() - processDecrementsStartTime;
  GC_LOG("||| GC: processDecrementsDuration = %lld\n", processDecrementsDuration);
  auto decrementStackStartTime = konan::getTimeMicros();
#endif
  size_t beforeDecrements = state->toRelease->size();
  decrementStack(state);
  size_t afterDecrements = state->toRelease->size();
#if PROFILE_GC
  auto decrementStackDuration = konan::getTimeMicros() - decrementStackStartTime;
  GC_LOG("||| GC: decrementStackDuration = %lld\n", decrementStackDuration);
#endif
  RuntimeAssert(afterDecrements >= beforeDecrements, "toRelease size must not have decreased");
  size_t stackReferences = afterDecrements - beforeDecrements;
  if (state->gcErgonomics && stackReferences * 5 > state->gcThreshold) {
    increaseGcThreshold(state);
    GC_LOG("||| GC: too many stack references, increased threshold to %d\n", state->gcThreshold);
  }

  GC_LOG("||| GC: toFree %d toRelease %d\n", state->toFree->size(), state->toRelease->size())
#if PROFILE_GC
  auto processFinalizerQueueStartTime = konan::getTimeMicros();
#endif
  processFinalizerQueue(state);
#if PROFILE_GC
  auto processFinalizerQueueDuration = konan::getTimeMicros() - processFinalizerQueueStartTime;
  GC_LOG("||| GC: processFinalizerQueueDuration %lld\n", processFinalizerQueueDuration);
#endif

  if (force || state->toFree->size() > state->gcCollectCyclesThreshold) {
    auto cyclicGcStartTime = konan::getTimeMicros();
    while (state->toFree->size() > 0) {
      collectCycles(state);
      #if PROFILE_GC
        processFinalizerQueueStartTime = konan::getTimeMicros();
      #endif
      processFinalizerQueue(state);
      #if PROFILE_GC
        processFinalizerQueueDuration += konan::getTimeMicros() - processFinalizerQueueStartTime;
        GC_LOG("||| GC: processFinalizerQueueDuration = %lld\n", processFinalizerQueueDuration);
      #endif
    }
    auto cyclicGcEndTime = konan::getTimeMicros();
    #if PROFILE_GC
      GC_LOG("||| GC: collectCyclesDuration = %lld\n", cyclicGcEndTime - cyclicGcStartTime);
    #endif
    auto cyclicGcDuration = cyclicGcEndTime - cyclicGcStartTime;
    if (!force && state->gcErgonomics && cyclicGcDuration > kGcCollectCyclesMinimumDuration &&
        double(cyclicGcDuration) / (cyclicGcStartTime - state->lastCyclicGcTimestamp + 1) > kGcCollectCyclesLoadRatio) {
      increaseGcCollectCyclesThreshold(state);
      GC_LOG("Adjusting GC collecting cycles threshold to %lld\n", state->gcCollectCyclesThreshold);
    }
    state->lastCyclicGcTimestamp = cyclicGcEndTime;
  }

  state->gcInProgress = false;
  auto gcEndTime = konan::getTimeMicros();

  if (state->gcErgonomics) {
    auto gcToComputeRatio = double(gcEndTime - gcStartTime) / (gcStartTime - state->lastGcTimestamp + 1);
    if (!force && gcToComputeRatio > kGcToComputeRatioThreshold) {
      increaseGcThreshold(state);
      GC_LOG("Adjusting GC threshold to %d\n", state->gcThreshold);
    }
  }
  GC_LOG("GC: gcToComputeRatio=%f duration=%lld sinceLast=%lld\n", double(gcEndTime - gcStartTime) / (gcStartTime - state->lastGcTimestamp + 1), (gcEndTime - gcStartTime), gcStartTime - state->lastGcTimestamp);
  state->lastGcTimestamp = gcEndTime;

#if TRACE_MEMORY
  for (auto* obj: *state->toRelease) {
    MEMORY_LOG("toRelease %p\n", obj)
  }
#endif

  GC_LOG("<<< GC: toFree %d toRelease %d\n", state->toFree->size(), state->toRelease->size())
}

void rememberNewContainer(ContainerHeader* container) {
  if (container == nullptr) return;
  // Instances can be allocated before actual runtime init - be prepared for that.
  if (memoryState != nullptr) {
    incrementRC</* Atomic = */ true>(container);
    // We cannot collect until reference will be stored into the stack slot.
    enqueueDecrementRC</* CanCollect = */ true>(container);
  }
}

void garbageCollect() {
  garbageCollect(memoryState, true);
}

#endif  // USE_GC

ForeignRefManager* initLocalForeignRef(ObjHeader* object) {
  if (!IsStrictMemoryModel && !RTGC) return nullptr;

  return memoryState->foreignRefManager;
}

ForeignRefManager* initForeignRef(ObjHeader* object) {
  retainRef(object);
  tryMakeShareable(object->container());
  RTGC_LOG("initForeignRef %p\n", object);

  if (!IsStrictMemoryModel && !RTGC) return nullptr;

  // Note: it is possible to return nullptr for shared object as an optimization,
  // but this will force the implementation to release objects on uninitialized threads
  // which is generally a memory leak. See [deinitForeignRef].
  auto* manager = memoryState->foreignRefManager;
  manager->addRef();
  return manager;
}

bool isForeignRefAccessible(ObjHeader* object, ForeignRefManager* manager) {
  if (!IsStrictMemoryModel && !RTGC) return true;

  bool canAccess = manager == memoryState->foreignRefManager;
    // Note: it is important that this code neither crashes nor returns false-negative result
    // (although may produce false-positive one) if [manager] is a dangling pointer.
    // See BackRefFromAssociatedObject::releaseRef for more details.
  if (!canAccess) {
  // Note: getting container and checking it with 'isShareable()' is supposed to be correct even for unowned object.
  // @zee can not use isShareable at external thread.
    canAccess = object->container() == NULL || object->container()->shared();
  }

  RTGC_LOG("isForeignRefAccessible %p canAccess=%d\n", object, canAccess)
  return canAccess;
}

void deinitForeignRef(ObjHeader* object, ForeignRefManager* manager) {
  RTGC_LOG("deinitForeignRef %p(mem=%p)canAccess=%d\n", object, memoryState, (memoryState != nullptr && isForeignRefAccessible(object, manager)));

  if (RTGC || IsStrictMemoryModel) {
    if (memoryState != nullptr && (RTGC || isForeignRefAccessible(object, manager))) {
      if (RTGC) {
        //bool isLocalThread = manager == memoryState->foreignRefManager;
        //if (!isLocalThread) GCNode::rtgcLock(_IncrementRC);
        releaseRef<false>(object);
        //if (!isLocalThread) GCNode::rtgcUnlock();
      }
      else {
        releaseRef<true>(object);
      }
    } else {
      if (RTGC && object->container()->refCount() == 1) {
        // early destory WorkerBound/Weak References. cf) testObjCExport
        runDeallocationHooks(object->container(), manager);
      }
      // Prefer this for (memoryState == nullptr) since otherwise the object may leak:
      // an uninitialized thread did not run any Kotlin code;
      // it may be an externally-managed thread which is not supposed to run Kotlin code
      // and not going to exit soon.
      manager->enqueueReleaseRef(object);
    }

    manager->releaseRef();
  } else {
    releaseRef<false>(object);
    RuntimeAssert(manager == nullptr, "must be null");
  }
}

MemoryState* initMemory() {
  RuntimeAssert(offsetof(ArrayHeader, typeInfoOrMeta_)
                ==
                offsetof(ObjHeader,   typeInfoOrMeta_),
                "Layout mismatch");
  RuntimeAssert(offsetof(TypeInfo, typeInfo_)
                ==
                offsetof(MetaObjHeader, typeInfo_),
                "Layout mismatch");
  RuntimeAssert(sizeof(FrameOverlay) % sizeof(ObjHeader**) == 0, "Frame overlay should contain only pointers")
  RuntimeAssert(memoryState == nullptr, "memory state must be clear");
  memoryState = konanConstructInstance<MemoryState>();
  INIT_EVENT(memoryState)
#if USE_GC
  memoryState->toFree = konanConstructInstance<ContainerHeaderList>();
  memoryState->roots = konanConstructInstance<ContainerHeaderList>();
  memoryState->gcInProgress = false;
  memoryState->gcSuspendCount = 0;
  memoryState->toRelease = konanConstructInstance<KStdDeque<ContainerHeader*>>();
  initGcThreshold(memoryState, kGcThreshold);
  initGcCollectCyclesThreshold(memoryState, kMaxToFreeSizeThreshold);
  memoryState->allocSinceLastGcThreshold = kMaxGcAllocThreshold;
  memoryState->gcErgonomics = true;
#endif
  memoryState->tlsMap = konanConstructInstance<KThreadLocalStorageMap>();
  memoryState->foreignRefManager = ForeignRefManager::create();
  bool firstMemoryState = atomicAdd(&aliveMemoryStatesCount, 1) == 1;
  if (firstMemoryState) {
#if USE_CYCLIC_GC
    cyclicInit();
#endif  // USE_CYCLIC_GC
  }
  GCNode::initMemory(memoryState);
  return memoryState;
}

void deinitMemory(MemoryState* memoryState) {
  RTGC_LOG("deinitMemory %p {{\n", memoryState);
  static int pendingDeinit = 0;
  atomicAdd(&pendingDeinit, 1);
#if USE_GC
  bool lastMemoryState = atomicAdd(&aliveMemoryStatesCount, -1) == 0;
  bool checkLeaks __attribute__((unused))= Kotlin_memoryLeakCheckerEnabled() && lastMemoryState;
  if (RTGC || lastMemoryState) {
    garbageCollect(memoryState, true);
    memoryState->refChainAllocator.destroyAlloctor();
    memoryState->cyclicNodeAllocator.destroyAlloctor();
    RTGC_LOG("deinitMemory RTGC allocators are destroyed")
#if USE_CYCLIC_GC
   // If there are other pending deinits (rare situation) - just skip the leak checker.
   // This may happen when there're several threads with Kotlin runtimes created
   // by foreign code, and that code stops those threads simultaneously.
   if (atomicGet(&pendingDeinit) > 1) {
     checkLeaks = false;
   }
   cyclicDeinit(g_hasCyclicCollector);
#endif  // USE_CYCLIC_GC
  }
  // Actual GC only implemented in strict memory model at the moment.
  if (!RTGC) {
    do {
      GC_LOG("Calling garbageCollect from DeinitMemory()\n")
      garbageCollect(memoryState, true);
    } while (memoryState->toRelease->size() > 0 || !memoryState->foreignRefManager->tryReleaseRefOwned());
  }
  RTGC_LOG("deinitMemory 2")

  RuntimeAssert(memoryState->toFree->size() == 0, "Some memory have not been released after GC");
  RuntimeAssert(memoryState->toRelease->size() == 0, "Some memory have not been released after GC");
  konanDestructInstance(memoryState->toFree);
  konanDestructInstance(memoryState->roots);
  konanDestructInstance(memoryState->toRelease);
  RuntimeAssert(memoryState->tlsMap->size() == 0, "Must be already cleared");
  konanDestructInstance(memoryState->tlsMap);
  RuntimeAssert(memoryState->finalizerQueue == nullptr, "Finalizer queue must be empty");
  RuntimeAssert(memoryState->finalizerQueueSize == 0, "Finalizer queue must be empty");
#endif // USE_GC

  atomicAdd(&pendingDeinit, -1);

#if TRACE_MEMORY
  if (IsStrictMemoryModel && lastMemoryState && allocCount > 0) {
    MEMORY_LOG("*** Memory leaks, leaked %d containers ***\n", allocCount);
    dumpReachable("", memoryState->containers);
  }
#else
#if USE_GC
  if ((IsStrictMemoryModel || RTGC) && allocCount > 0 && checkLeaks) {
    konan::consoleErrorf(
        "Memory leaks detected, %d objects leaked!\n"
        "Use `Platform.isMemoryLeakCheckerActive = false` to avoid this check.\n", allocCount);
    konan::consoleFlush();
    konan::abort();
  }
#endif  // USE_GC
#endif  // TRACE_MEMORY


  PRINT_EVENT(memoryState)
  DEINIT_EVENT(memoryState)

  konanFreeMemory(memoryState);
  RTGC_LOG("}} deinitMemory %p done.\n", memoryState);
  ::memoryState = nullptr;
}

MemoryState* suspendMemory() {
    auto result = ::memoryState;
    ::memoryState = nullptr;
    return result;
}

void resumeMemory(MemoryState* state) {
    ::memoryState = state;
}

void makeShareable(ContainerHeader* container) {
  if (!container->frozen())
    container->makeShared();
}

template<bool Strict>
void setStackRef(ObjHeader** location, const ObjHeader* object) {
  MEMORY_LOG("SetStackRef *%p: %p\n", location, object)
  UPDATE_REF_EVENT(memoryState, nullptr, object, location, 1);
  if (!Strict && object != nullptr)
    retainRef(object);
  *const_cast<const ObjHeader**>(location) = object;
}

template<bool Strict>
void setHeapRef(ObjHeader** location, const ObjHeader* object) {
  MEMORY_LOG("SetHeapRef *%p: %p\n", location, object)
  UPDATE_REF_EVENT(memoryState, nullptr, object, location, 0);
  if (object != nullptr)
    retainRef(const_cast<ObjHeader*>(object));
  *const_cast<const ObjHeader**>(location) = object;
}

void zeroHeapRef(ObjHeader** location) {
  MEMORY_LOG("ZeroHeapRef %p\n", location)
  auto* value = *location;
  if (reinterpret_cast<uintptr_t>(value) > 1) {
    UPDATE_REF_EVENT(memoryState, value, nullptr, location, 0);
    *location = nullptr;
    ReleaseRef(value);
  }
}

template<bool Strict>
void zeroStackRef(ObjHeader** location) {
  // currently not used
  MEMORY_LOG("ZeroStackRef %p\n", location)
  if (Strict) {
    *location = nullptr;
  } else {
    auto* old = *location;
    *location = nullptr;
    if (old != nullptr) releaseRef<Strict>(old);
  }
}

#if RTGC
} // namespace


int decrementMemberRC_internal(ContainerHeader* deassigned, ContainerHeader* owner) {
  int freeNode;
  if (deassigned->shared()) {
      if (deassigned->isAcyclic()) {
        freeNode = decrementAcyclicRCtoZero</* Atomic = */ true>(deassigned);
      }
      else {
        GCNode::rtgcLock(_DeassignRef);
        freeNode = decrementMemberRCtoZero</* Atomic = */ true>(deassigned, owner);
        GCNode::rtgcUnlock();
      }
  }
  else {
      if (deassigned->isAcyclic()) {
        freeNode = decrementAcyclicRCtoZero</* Atomic = */ false>(deassigned);
      }
      else {
        freeNode = decrementMemberRCtoZero</* Atomic = */ false>(deassigned, owner);
      }
  }
  return freeNode;
}

void updateHeapRef_internal(const ObjHeader* object, const ObjHeader* old, const ObjHeader* owner) {

  if (object != nullptr && object != owner) {
    ContainerHeader* container = object->container();

    if (isFreeable(container)) {
      if (container->shared()) {
          if (container->isAcyclic()) {
            owner->container()->attachNode();
            container->attachNode();
            incrementAcyclicRC</* Atomic = */ true>(container);
          }
          else { 
            GCNode::rtgcLock(_AssignRef);
            incrementMemberRC</* Atomic = */ true>(container, owner->container());
            GCNode::rtgcUnlock();
          }
      }
      else {
          if (container->isAcyclic()) {
            owner->container()->attachNode();
            container->attachNode();
            incrementAcyclicRC</* Atomic = */ false>(container);
          }
          else { 
            incrementMemberRC</* Atomic = */ false>(container, owner->container());
          }
      }
    }
  }

  if (reinterpret_cast<uintptr_t>(old) > 1 && old != owner) {
    ContainerHeader* container = old->container();
    if (isFreeable(container)) {
        int freeNode = decrementMemberRC_internal(container, owner->container());
        checkGrabage(container, freeNode);
    }
    //releaseRef<Strict>(old);
  }

}
namespace {

template <bool Strict>
void updateHeapRef(ObjHeader** location, const ObjHeader* object, const ObjHeader* owner) {
  UPDATE_REF_EVENT(memoryState, *location, object, location, owner);

  if (owner->local()) {
    // konan::consolePrintf("updateHeapRef on stackLocal Owner");
    UpdateStackRef(location, object);
    return;
  }
  bool isShared = owner->container()->shared();
  if (isShared) {
    if (object != NULL) shareAny((ObjHeader*)object);
    GCNode::rtgcLock(_UpdateHeapRef);
  }
  ObjHeader* old = *location;
  if (old != object) {
    *location = (ObjHeader*)object;
    updateHeapRef_internal(object, old, owner);
  }
  if (isShared) GCNode::rtgcUnlock();
}


#else 
template <bool Strict>
void updateHeapRef(ObjHeader** location, const ObjHeader* object, const ObjHeader* owner) {
  UPDATE_REF_EVENT(memoryState, *location, object, location, 0);
  ObjHeader* old = *location;
  if (old != object) {
    if (object != nullptr) {
      retainRef(object);
    }
    *const_cast<const ObjHeader**>(location) = object;
    if (reinterpret_cast<uintptr_t>(old) > 1) {
      releaseRef<Strict>(old);
    }
  }
}
#endif

template <bool Strict>
void updateStackRef(ObjHeader** location, const ObjHeader* object) {
  UPDATE_REF_EVENT(memoryState, *location, object, location, 1)
  RuntimeAssert(object != reinterpret_cast<ObjHeader*>(1), "Markers disallowed here");
  if (Strict) {
    *const_cast<const ObjHeader**>(location) = object;
  } else {
     ObjHeader* old = *location;
     if (old != object) {
        if (object != nullptr) {
          retainRef(object);
        }
        *const_cast<const ObjHeader**>(location) = object;
        if (old != nullptr) {
           releaseRef<false>(old);
        }
     }
  }
}

template <bool Strict>
void updateReturnRef(ObjHeader** returnSlot, const ObjHeader* value) {
  updateStackRef<Strict>(returnSlot, value);
}

void updateHeapRefIfNull(ObjHeader** location, const ObjHeader* object) {
  if (object != nullptr) {
#if KONAN_NO_THREADS
    ObjHeader* old = *location;
    if (old == nullptr) {
      retainRef(const_cast<ObjHeader*>(object));
      *const_cast<const ObjHeader**>(location) = object;
    }
#else
    retainRef(const_cast<ObjHeader*>(object));
    auto old = __sync_val_compare_and_swap(location, nullptr, const_cast<ObjHeader*>(object));
    if (old != nullptr) {
      // Failed to store, was not null.
     ReleaseRef(const_cast<ObjHeader*>(object));
    }
#endif
    UPDATE_REF_EVENT(memoryState, old, object, location, 0);
  }
}

inline void checkIfGcNeeded(MemoryState* state) {
#if USE_GC  
  if (state != nullptr && state->allocSinceLastGc > state->allocSinceLastGcThreshold) {
    // To avoid GC trashing check that at least 10ms passed since last GC.
    if (konan::getTimeMicros() - state->lastGcTimestamp > 10 * 1000) {
      GC_LOG("Calling GC from checkIfGcNeeded: %d\n", state->toRelease->size())
      garbageCollect(state, false);
    }
  }
#endif  
}

inline void checkIfForceCyclicGcNeeded(MemoryState* state) {
  if (state != nullptr && state->toFree != nullptr && state->toFree->size() > kMaxToFreeSizeThreshold) {
    // To avoid GC trashing check that at least 10ms passed since last GC.
    if (konan::getTimeMicros() - state->lastGcTimestamp > 10 * 1000) {
      GC_LOG("Calling GC from checkIfForceCyclicGcNeeded: %d\n", state->toFree->size())
      garbageCollect(state, true);
    }
  }
}

template <bool Strict>
OBJ_GETTER(allocInstance, const TypeInfo* type_info) {
  RuntimeAssert(type_info->instanceSize_ >= 0, "must be an object");
  //free(type_);
  auto* state = memoryState;
#if USE_GC
  checkIfGcNeeded(state);
#endif  // USE_GC
  auto container = ObjectContainer(state, type_info);
  ObjHeader* obj = container.GetPlace();
#if USE_GC
  if (Strict) {
    rememberNewContainer(container.header());
  } else if (!RTGC) {
    makeShareable(container.header());
  }
#endif  // USE_GC
#if USE_CYCLE_DETECTOR
  CycleDetector::insertCandidateIfNeeded(obj);
#endif  // USE_CYCLE_DETECTOR
#if USE_CYCLIC_GC
  if ((obj->type_info()->flags_ & TF_LEAK_DETECTOR_CANDIDATE) != 0) {
    // Note: this should be performed after [rememberNewContainer] (above).
    // Otherwise cyclic collector can observe this atomic root with RC = 0,
    // thus consider it garbage and then zero it after initialization.
    cyclicAddAtomicRoot(obj);
  }
#endif  // USE_CYCLIC_GC
  RETURN_OBJ(obj);
}

template <bool Strict>
OBJ_GETTER(allocArrayInstance, const TypeInfo* type_info, int32_t elements) {
  RuntimeAssert(type_info->instanceSize_ < 0, "must be an array");
  if (elements < 0) ThrowIllegalArgumentException();
  auto* state = memoryState;
#if USE_GC
  checkIfGcNeeded(state);
#endif  // USE_GC
  auto container = ArrayContainer(state, type_info, elements);
#if USE_GC
  if (Strict) {
    rememberNewContainer(container.header());
  } else if (!RTGC) {
    makeShareable(container.header());
  }
  if (type_info == theStringTypeInfo) {
    rtgc_trap(container.header());
  }

#endif  // USE_GC
  RETURN_OBJ(container.GetPlace()->obj());
}

template <bool Strict>
OBJ_GETTER(initInstance,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*)) {
  ObjHeader* value = *location;
  if (value != nullptr) {
    // OK'ish, inited by someone else.
    RETURN_OBJ(value);
  }
  ObjHeader* object = allocInstance<Strict>(typeInfo, OBJ_RESULT);
  updateStackRef<Strict>(location, object);  
#if KONAN_NO_EXCEPTIONS
  ctor(object);
  return object;
#else
  try {
    ctor(object);
    return object;
  } catch (...) {
    UpdateReturnRef(OBJ_RESULT, nullptr);
    ZeroStackRef(location);
    throw;
  }
#endif
}

void runFreezeHooksRecursive(ObjHeader* root, KStdVector<KRef>* newlyFrozen);

void sharePermanentSubgraph(ObjHeader* obj);

template <bool Strict>
OBJ_GETTER(initSharedInstance,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*)) {
  RTGC_LOG("initSharedInstance %p %p", location, typeInfo);
#if KONAN_NO_THREADS
  ObjHeader* value = *location;
  if (value != nullptr) {
    // OK'ish, inited by someone else.
    RETURN_OBJ(value);
  }
  ObjHeader* object = AllocInstance(typeInfo, OBJ_RESULT);
  UpdateStackRef(location, object);
  #if KONAN_NO_EXCEPTIONS
    ctor(object);
    FreezeSubgraph(object);
    return object;
  #else
    try {
      ctor(object);
      if (Strict)
        FreezeSubgraph(object);
      return object;
    } catch (...) {
      UpdateReturnRef(OBJ_RESULT, nullptr);
      ZeroStackRef(location);
      throw;
    }
  #endif  // KONAN_NO_EXCEPTIONS
#else  // KONAN_NO_THREADS
  // Search from the top of the stack.
  for (auto it = memoryState->initializingSingletons.rbegin(); it != memoryState->initializingSingletons.rend(); ++it) {
    if (it->first == location) {
      RETURN_OBJ(it->second);
    }
  }

  ObjHeader* initializing = reinterpret_cast<ObjHeader*>(1);

  // Spin lock.
  ObjHeader* value = nullptr;
  while ((value = __sync_val_compare_and_swap(location, nullptr, initializing)) == initializing);
  if (value != nullptr) {
    // OK'ish, inited by someone else.
    RETURN_OBJ(value);
  }
  ObjHeader* object = AllocInstance(typeInfo, OBJ_RESULT);
  memoryState->initializingSingletons.push_back(std::make_pair(location, object));
  #if KONAN_NO_EXCEPTIONS
    ctor(object);
    if (Strict)
      FreezeSubgraph(object);
    #ifdef RTGC
      setStackRef<Strict>(location, object);
    #else    
      UpdateStackRef(location, object);
    #endif  
    synchronize();
    memoryState->initializingSingletons.pop_back();
    return object;
  #else  // KONAN_NO_EXCEPTIONS
    try {
      if (true || RTGC_STATISTCS) {
        // RTGC_dumpTypeInfo("initShared", typeInfo, object->container());
      }
      ctor(object);
      if (Strict) { // ZZZZZ??? } || RTGC) {
        FreezeSubgraph(object);
      }
      else if (RTGC) {
        if (!IS_SHARED_PERMANENT_NEVER_FREEABLE) {
          garbageCollect();
        }
        if (!isPermanentOrFrozen(object->container())) {
          KStdVector<KRef> newlyFrozen;
          runFreezeHooksRecursive(object, &newlyFrozen);
        }
        sharePermanentSubgraph(object);
      }
      #ifdef RTGC
        retainRef(object);
        *location = object;
      #else    
        UpdateStackRef(location, object);
      #endif
      synchronize();
      memoryState->initializingSingletons.pop_back();
      return object;
    } catch (...) {
      UpdateReturnRef(OBJ_RESULT, nullptr);
      ZeroStackRef(location);
      memoryState->initializingSingletons.pop_back();
      synchronize();
      throw;
    }
  #endif  // KONAN_NO_EXCEPTIONS
#endif  // KONAN_NO_THREADS
}

/**
 * We keep thread affinity and reference value based cookie in the atomic references, so that
 * repeating read operation of the same value do not lead to the repeating rememberNewContainer() operation.
 * We must invalidate cookie after the local GC, as otherwise fact that container of the `value` is retained
 * may change, if the last reference to the value read is lost during GC and we re-read same value from
 * the same atomic reference. Thus we also include GC epoque into the cookie.
 */
inline int32_t computeCookie() {
  auto* state = memoryState;
  auto epoque = state->gcEpoque;
  return (static_cast<int32_t>(reinterpret_cast<intptr_t>(state))) ^ static_cast<int32_t>(epoque);
}

OBJ_GETTER(swapHeapRefLocked,
    ObjHeader** location, ObjHeader* expectedValue, ObjHeader* newValue, int32_t* spinlock, ObjHeader* owner, int32_t* cookie) {
  MEMORY_LOG("swapHeapRefLocked: %p, v=%p, o=%p isLocal=%d\n", location, newValue, owner, owner != nullptr && !owner->container()->shared());
  bool isLocal = owner != nullptr && !owner->container()->shared();
  if (isLocal) {
      ObjHeader* old = *location;
      UpdateReturnRef(OBJ_RESULT, old);
      if (old == expectedValue) {
          UpdateHeapRef(location, newValue, owner);
      }
      return old;
  }
    GCNode::rtgcLock(_SetHeapRefLocked);
//lock(spinlock);
  ObjHeader* oldValue = *location;
  bool shallRemember = false;
  if (IsStrictMemoryModel) {
    auto realCookie = computeCookie();
    shallRemember = *cookie != realCookie;
    if (shallRemember) *cookie = realCookie;
  }

  if (oldValue == expectedValue) {
#if USE_CYCLIC_GC
    if (g_hasCyclicCollector)
      cyclicMutateAtomicRoot(newValue);
#endif  // USE_CYCLIC_GC
    if (owner == NULL) {
      SetHeapRef(location, newValue);
      // @zee oldValue will-keep sameRefCount;
    }
    else {
      // @zee protect deleting oldValue;
      UpdateReturnRef(OBJ_RESULT, oldValue);
      UpdateHeapRef(location, newValue, owner);
    }
  }
  else {
    UpdateReturnRef(OBJ_RESULT, oldValue);
  }

  if (IsStrictMemoryModel && shallRemember && oldValue != nullptr && oldValue != expectedValue) {
    // Only remember container if it is not known to this thread (i.e. != expectedValue).
    rememberNewContainer(oldValue->container());
  }
    GCNode::rtgcUnlock();

  //unlock(spinlock);

  return oldValue;
}

void setHeapRefLocked(ObjHeader** location, ObjHeader* newValue, int32_t* spinlock, ObjHeader* owner, int32_t* cookie) {
  MEMORY_LOG("setHeapRefLocked: %p, v=%p, o=%p\n", location, newValue, owner);
  bool isLocal = owner != nullptr && !owner->container()->shared();
  if (isLocal) {
      UpdateHeapRef(location, newValue, owner);
      return;
  }
  GCNode::rtgcLock(_SetHeapRefLocked);
#if USE_CYCLIC_GC
  if (g_hasCyclicCollector)
    cyclicMutateAtomicRoot(newValue);
#endif  // USE_CYCLIC_GC
  // We do not use UpdateRef() here to avoid having ReleaseRef() on old value under the lock.
  if (owner == NULL) {
    ObjHeader* oldValue = *location;
    SetHeapRef(location, newValue);
    if (oldValue != nullptr)
      ReleaseRef(oldValue);
  }
  else {
    UpdateHeapRef(location, newValue, owner);
  }
  *cookie = computeCookie();
  GCNode::rtgcUnlock();
  //unlock(spinlock);
}

OBJ_GETTER(readHeapRefLocked, ObjHeader** location, int32_t* spinlock, ObjHeader* owner, int32_t* cookie) {
  MEMORY_LOG("ReadHeapRefLocked: %p\n", location)
  bool isLocal = owner != nullptr && !owner->container()->shared();
  if (isLocal) {
      ObjHeader* value = *location;
      UpdateReturnRef(OBJ_RESULT, value);
      return value;
  }
  GCNode::rtgcLock(_SetHeapRefLocked);
//lock(spinlock);
  ObjHeader* value = *location;
  auto realCookie = computeCookie();
  bool shallRemember = *cookie != realCookie;
  if (shallRemember) *cookie = realCookie;
  UpdateReturnRef(OBJ_RESULT, value);
#if USE_GC
  if (IsStrictMemoryModel && shallRemember && value != nullptr) {
    auto* container = value->container();
    rememberNewContainer(container);
  }
#endif  // USE_GC
  GCNode::rtgcUnlock();
  //unlock(spinlock);
  return value;
}

OBJ_GETTER(readHeapRefNoLock, ObjHeader* object, KInt index) {
  MEMORY_LOG("ReadHeapRefNoLock: %p index %d\n", object, index)
  ObjHeader** location = reinterpret_cast<ObjHeader**>(
    reinterpret_cast<uintptr_t>(object) + object->type_info()->objOffsets_[index]);
  ObjHeader* value = *location;
#if USE_GC
  if (IsStrictMemoryModel && (value != nullptr)) {
    // Maybe not so good to do that under lock.
    rememberNewContainer(value->container());
  }
#endif  // USE_GC
  RETURN_OBJ(value);
}

template <bool Strict>
void enterFrame(ObjHeader** start, int parameters, int count) {
  MEMORY_LOG("EnterFrame %p: %d parameters %d locals\n", start, parameters, count)
  FrameOverlay* frame = reinterpret_cast<FrameOverlay*>(start);
  if (Strict) {
    frame->previous = currentFrame;
    currentFrame = frame;
    // TODO: maybe compress in single value somehow.
    frame->parameters = parameters;
    frame->count = count;
  }
}

// for RTGC only
template <bool Strict>
const ObjHeader* leaveFrameAndReturnRef(ObjHeader** start, int param_count, ObjHeader** resultSlot, const ObjHeader* returnRef) {
  int parameters = param_count >> 16;
  int count = (int16_t)param_count;
  MEMORY_LOG("leaveFrameAndReturnRef %p: %d parameters %d locals. returns %p(%p:%p) \n", start, parameters, count, returnRef, resultSlot, *resultSlot);
  const ObjHeader* res = *resultSlot;
  if (res != returnRef) {
    *resultSlot = (ObjHeader*)returnRef;
    if (res != NULL) {
      releaseRef<Strict>(res);
    }
    res = returnRef;
  }
  else {
    returnRef = NULL; 
  }

  FrameOverlay* frame = reinterpret_cast<FrameOverlay*>(start);
  if (Strict) {
    currentFrame = frame->previous;
  } else {
    ObjHeader** current = start + parameters + kFrameOverlaySlots;
    count -= parameters;
    while (count-- > kFrameOverlaySlots) {
      ObjHeader* object = *current;
      if (object != nullptr) {
        if (object == returnRef) {
          returnRef = NULL;
        }
        else {
          zeroStackRef<Strict>(current);
        }
      }
      current++;
    }
    if (returnRef != NULL) {
      retainRef(returnRef);
    }
  }
  return res;
}

template <bool Strict>
void leaveFrame(ObjHeader** start, int parameters, int count) {
  MEMORY_LOG("LeaveFrame %p: %d parameters %d locals\n", start, parameters, count)
  FrameOverlay* frame = reinterpret_cast<FrameOverlay*>(start);
  if (Strict) {
    currentFrame = frame->previous;
  } else {
    ObjHeader** current = start + parameters + kFrameOverlaySlots;
    count -= parameters;
    while (count-- > kFrameOverlaySlots) {
      ObjHeader* object = *current;
      if (object != nullptr) {
        zeroStackRef<Strict>(current);
      }
      current++;
    }
  }
}

#if USE_GC
void suspendGC() {
  GC_LOG("suspendGC\n")
  memoryState->gcSuspendCount++;
}

void resumeGC() {
  GC_LOG("resumeGC\n")
  MemoryState* state = memoryState;
  if (state->gcSuspendCount > 0) {
    state->gcSuspendCount--;
    if (state->toRelease != nullptr &&
        state->toRelease->size() >= state->gcThreshold &&
        state->gcSuspendCount == 0) {

      garbageCollect(state, false);
    }
  }
}

void stopGC() {
  GC_LOG("stopGC\n")
  if (memoryState->toRelease != nullptr) {
    memoryState->gcSuspendCount = 0;
    garbageCollect(memoryState, true);
    konanDestructInstance(memoryState->toRelease);
    konanDestructInstance(memoryState->toFree);
    konanDestructInstance(memoryState->roots);
    memoryState->toRelease = nullptr;
    memoryState->toFree = nullptr;
    memoryState->roots = nullptr;
  }
}

void startGC() {
  GC_LOG("startGC\n")
  if (memoryState->toFree == nullptr) {
    memoryState->toFree = konanConstructInstance<ContainerHeaderList>();
    memoryState->toRelease = konanConstructInstance<KStdDeque<ContainerHeader*>>();
    memoryState->roots = konanConstructInstance<ContainerHeaderList>();
    memoryState->gcSuspendCount = 0;
  }
}

void setGCThreshold(KInt value) {
  GC_LOG("setGCThreshold %d\n", value)
  if (value <= 0) {
    ThrowIllegalArgumentException();
  }
  initGcThreshold(memoryState, value);
}

KInt getGCThreshold() {
  GC_LOG("getGCThreshold\n")
  return memoryState->gcThreshold;
}

void setGCCollectCyclesThreshold(KLong value) {
  GC_LOG("setGCCollectCyclesThreshold %d\n", value)
  if (value <= 0) {
    ThrowIllegalArgumentException();
  }
  initGcCollectCyclesThreshold(memoryState, value);
}

KInt getGCCollectCyclesThreshold() {
  GC_LOG("getGCCollectCyclesThreshold\n")
  return memoryState->gcCollectCyclesThreshold;
}

void setGCThresholdAllocations(KLong value) {
  GC_LOG("setGCThresholdAllocations %lld\n", value)
  if (value <= 0) {
    ThrowIllegalArgumentException();
  }

  memoryState->allocSinceLastGcThreshold = value;
}

KLong getGCThresholdAllocations() {
  GC_LOG("getGCThresholdAllocation\n")
  return memoryState->allocSinceLastGcThreshold;
}

void setTuneGCThreshold(KBoolean value) {
  GC_LOG("setTuneGCThreshold %d\n", value)
  memoryState->gcErgonomics = value;
}

KBoolean getTuneGCThreshold() {
  GC_LOG("getTuneGCThreshold %d\n")
  return memoryState->gcErgonomics;
}
#endif

KNativePtr createStablePointer(KRef any) {
  if (any == nullptr) return nullptr;
  MEMORY_LOG("CreateStablePointer for %p rc=%d\n", any, any->container() ? any->container()->refCount() : 0)
  retainRef(any);
  return reinterpret_cast<KNativePtr>(any);
}

void disposeStablePointer(KNativePtr pointer) {
  if (pointer == nullptr) return;
  KRef any = reinterpret_cast<KRef>(pointer);
  MEMORY_LOG("disposeStablePointer for %p rc=%d\n", any, any->container() ? any->container()->refCount() : 0)
  ReleaseRef(any);
}

OBJ_GETTER(derefStablePointer, KNativePtr pointer) {
  KRef ref = reinterpret_cast<KRef>(pointer);
  if (pointer != nullptr) {
    MEMORY_LOG("disposeStablePointer for %p rc=%d\n", ref, ref->container() ? ref->container()->refCount() : 0)
  }
  AdoptReferenceFromSharedVariable(ref);
  RETURN_OBJ(ref);
}

OBJ_GETTER(adoptStablePointer, KNativePtr pointer) {
  synchronize();
  KRef ref = reinterpret_cast<KRef>(pointer);
  if (pointer != nullptr) {
    MEMORY_LOG("adopting stable pointer %p, rc=%d\n", \
       ref, (ref && ref->container()) ? ref->container()->refCount() : -1)
  }
  UpdateReturnRef(OBJ_RESULT, ref);
  DisposeStablePointer(pointer);
  return ref;
}

bool clearSubgraphReferences(ObjHeader* root, bool checked) {
  MEMORY_LOG("ClearSubgraphReferences %p\n", root)
#if USE_GC
  if (root == nullptr) return true;
  auto state = memoryState;
  auto* container = root->container();

  if (tryMakeShareable(container)) {
    // We assume, that frozen/shareable objects can be safely passed and not present
    // in the GC candidate list.
    // TODO: assert for that?
    return true;
  }

#ifdef RTGC
  garbageCollect();
  ContainerHeaderDeque visited;
#else
  ContainerHeaderSet visited;  
#endif
  if (!checked) {
    hasExternalRefs(container, &visited);
  } else {
    // Now decrement RC of elements in toRelease set for reachibility analysis.
    for (auto it = state->toRelease->begin(); it != state->toRelease->end(); ++it) {
      auto released = *it;
      RuntimeAssert(!RTGC, "no kotlin gc");
      if (!isMarkedAsRemoved(released) && !released->shared()) {
        released->decRefCount<false>();
      }
    }
#ifdef RTGC
    auto bad = hasExternalRefs(container, &visited);
#else
    container->decRefCount<false>();
    markGray<false>(container);
    auto bad = hasExternalRefs(container, &visited);
    scanBlack<false>(container);
    // Restore original RC.
    container->incRefCount<false>();
    for (auto it = state->toRelease->begin(); it != state->toRelease->end(); ++it) {
       auto released = *it;
       if (!isMarkedAsRemoved(released) && released->local()) {
         released->incRefCount<false>();
       }
    }
#endif    
    if (bad) {
      return false;
    }
  }

  // Remove all no longer owned containers from GC structures.
  // TODO: not very efficient traversal.
  for (auto it = state->toFree->begin(); it != state->toFree->end(); ++it) {
    #ifdef RTGC
      RuntimeAssert(!RTGC, "no kotlin gc");
    #else  
    auto container = *it;
    if (visited.count(container) != 0) {
      MEMORY_LOG("removing %p from the toFree list\n", container)
      container->resetBuffered();
      container->setColorAssertIfGreen(CONTAINER_TAG_GC_BLACK);
      *it = markAsRemoved(container);
    }
    #endif
  }
  for (auto it = state->toRelease->begin(); it != state->toRelease->end(); ++it) {
    #ifdef RTGC
      RuntimeAssert(!RTGC, "no kotlin gc");
    #else
    auto container = *it;
    if (!isMarkedAsRemoved(container) && visited.count(container) != 0) {
      MEMORY_LOG("removing %p from the toRelease list\n", container)
      container->decRefCount<false>();
      *it = markAsRemoved(container);
    }
    #endif
  }

#if TRACE_MEMORY
  // Forget transferred containers.
  for (auto* it: visited) {
    state->containers->erase(it);
  }
#endif

#endif  // USE_GC
  return true;
}

void freezeAcyclic(ContainerHeader* rootContainer, ContainerHeaderSet* newlyFrozen) {
  KStdDeque<ContainerHeader*> queue;
  queue.push_back(rootContainer);
  while (!queue.empty()) {
    ContainerHeader* current = queue.front();
    queue.pop_front();
    current->unMark();
    current->resetBuffered();
#if !RTGC
    current->setColorUnlessGreen(CONTAINER_TAG_GC_BLACK);
    // Note, that once object is frozen, it could be concurrently accessed, so
    // color and similar attributes shall not be used.
    if (!current->frozen())
      newlyFrozen->insert(current);
    MEMORY_LOG("freezing acyclic %p\n", current)
#endif
    current->freezeRef();
    current->makeShared();
    traverseContainerReferredObjects(current, [&queue](ObjHeader* obj) {
        ContainerHeader* objContainer = obj->container();
        if (canFreeze(objContainer)) {
          if (objContainer->marked())
            queue.push_back(objContainer);
        }
    });
  }
}

void freezeCyclic(ObjHeader* root,
                  const KStdVector<ContainerHeader*>& order,
                  ContainerHeaderSet* newlyFrozen) {
  KStdUnorderedMap<ContainerHeader*, KStdVector<ContainerHeader*>> reversedEdges;
  KStdDeque<ObjHeader*> queue;
  queue.push_back(root);
  while (!queue.empty()) {
    ObjHeader* current = queue.front();
    queue.pop_front();
    ContainerHeader* currentContainer = current->container();
    currentContainer->unMark();
    reversedEdges.emplace(currentContainer, KStdVector<ContainerHeader*>(0));
    traverseContainerReferredObjects(currentContainer, [current, currentContainer, &queue, &reversedEdges](ObjHeader* obj) {
          ContainerHeader* objContainer = obj->container();
          if (canFreeze(objContainer)) {
            if (objContainer->marked())
              queue.push_back(obj);
            // We ignore references from FreezableAtomicsReference during condensation, to avoid KT-33824.
            if (!isFreezableAtomic(current))
              reversedEdges.emplace(objContainer, KStdVector<ContainerHeader*>(0)).
                first->second.push_back(currentContainer);
          }
      });
   }

   KStdVector<KStdVector<ContainerHeader*>> components;
   MEMORY_LOG("Condensation:\n");
   // Enumerate in the topological order.
   for (auto it = order.rbegin(); it != order.rend(); ++it) {
     auto* container = *it;
     if (container->marked()) continue;
     KStdVector<ContainerHeader*> component;
     traverseStronglyConnectedComponent(container, &reversedEdges, &component);
     MEMORY_LOG("SCC:\n");
  #if TRACE_MEMORY
     for (auto c: component)
       konan::consolePrintf("    %p\n", c);
  #endif
     components.push_back(std::move(component));
   }

  // Enumerate strongly connected components in reversed topological order.
  for (auto it = components.rbegin(); it != components.rend(); ++it) {
    auto& component = *it;
    int internalRefsCount = 0;
    int totalCount = 0;
    for (auto* container : component) {
      RuntimeAssert(!isAggregatingFrozenContainer(container), "Must not be called on such containers");
      totalCount += container->refCount();
      if (isFreezableAtomic(container)) {
        RuntimeAssert(component.size() == 1, "Must be trivial condensation");
        continue;
      }
      traverseContainerReferredObjects(container, [&internalRefsCount](ObjHeader* obj) {
          auto* container = obj->container();
          if (canFreeze(container))
            ++internalRefsCount;
        });
    }

    // Freeze component.
    for (auto* container : component) {
      container->resetBuffered();
#if !RTGC
      container->setColorUnlessGreen(CONTAINER_TAG_GC_BLACK);
#endif
      if (!container->frozen())
        newlyFrozen->insert(container);
      // Note, that once object is frozen, it could be concurrently accessed, so
      // color and similar attributes shall not be used.
      MEMORY_LOG("freezing Cyclic %p\n", container)
      container->freezeRef();
      // We set refcount of original container to zero, so that it is seen as such after removal
      // meta-object, where aggregating container is stored.
      container->setRefCount(0);
    }

    // Create fictitious container for the whole component.
    auto superContainer = component.size() == 1 ? component[0] : allocAggregatingFrozenContainer(component);
    // Don't count internal references.
    MEMORY_LOG("Setting aggregating %p rc to %d (total %d inner %d)\n", \
       superContainer, totalCount - internalRefsCount, totalCount, internalRefsCount)
    superContainer->setRefCount(totalCount - internalRefsCount);
    newlyFrozen->insert(superContainer);
  }
}

// These hooks are only allowed to modify `obj` subgraph.
void runFreezeHooks(ObjHeader* obj) {
  if (obj->type_info() == theWorkerBoundReferenceTypeInfo) {
    RTGC_LOG("runFreezeHooks: %p\n", obj)
    WorkerBoundReferenceFreezeHook(obj);
  }
}

#ifdef RTGC
void runFreezeHooksRecursive(ObjHeader* root, KStdVector<KRef>* toVisit) {
#else
void runFreezeHooksRecursive(ObjHeader* root) {
  KStdVector<KRef> seen;
#endif
  RTGC_LOG("runFreezeHooksRecursive %p {{\n", root);
  toVisit->push_back(root);
  root->container()->markFreezing();
  for (size_t idx = 0; idx < toVisit->size(); idx ++) {
    KRef obj = (*toVisit)[idx];
    if (RTGC && obj->has_meta_object() && (obj->meta_object()->flags_ & MF_NEVER_FROZEN) != 0) {
      for (auto* o : *toVisit) {
        o->container()->clearFreezing();
      }
      MEMORY_LOG("See freeze blocker for %p: %p\n", root, obj)
      ThrowFreezingException(root, obj);
    }

    if (false && ENABLE_RTGC_LOG) {
      RTGC_dumpRefInfo(obj->container());
    }
    runFreezeHooks(obj);

    traverseReferredObjects(obj, [toVisit](ObjHeader* field) {
      /**
       * runFreezeHooks() 수행 도중 side-effect 방지를 위해 mark() 대신에 seen set 를 이용(?!?)
       */
      if (canFreeze(field->container())) {
        field->container()->markFreezing();
        toVisit->push_back(field);
      }
    });
  }
  RTGC_LOG("}} runFreezeHooks done %p\n", root);
}

/**
 * Theory of operations.
 *
 * Kotlin/Native supports object graph freezing, allowing to make certain subgraph immutable and thus
 * suitable for safe sharing amongst multiple concurrent executors. This operation recursively operates
 * on all objects reachable from the given object, and marks them as frozen. In frozen state object's
 * fields cannot be modified, and so, lifetime of frozen objects correlates. Practically, it means
 * that lifetimes of all strongly connected components are fully controlled by incoming reference
 * counters, and so if we place all members of strongly connected component to the single container
 * it could be correctly released by just atomic decrement on reference counter, without additional
 * cycle collector run.
 * So during subgraph freezing operation, we perform the following steps:
 *   - run Kosoraju-Sharir algorithm to find strongly connected components
 *   - put all objects in each strongly connected component into an artificial container
 *     (we assume that they all were in single element containers initially), single-object
 *     components remain in the same container
 *   - artificial container sums up outer reference counters of all its objects (i.e.
 *     incoming references from the same strongly connected component are not counted)
 *   - mark all object's headers as frozen
 *
 *  Further reference counting on frozen objects is performed with atomic operations, and so frozen
 * references could be passed across multiple threads.
 */
void freezeSubgraph(ObjHeader* root) {
  if (root == nullptr) return;
  // First check that passed object graph has no cycles.
  // If there are cycles - run graph condensation on cyclic graphs using Kosoraju-Sharir.
  MEMORY_LOG("freeze requested %p\n", root);

  ContainerHeader* rootContainer = root->container();
  if (isPermanentOrFrozen(rootContainer)) {
    shareAny(root);
    return;
  }

  // Note: Actual freezing can fail, but these hooks won't be undone, and moreover
  // these hooks will run again on a repeated freezing attempt.

#if RTGC
  bool gc_only_freezing = true;
  if (!gc_only_freezing) garbageCollect();
  KStdVector<KRef> newlyFrozen;
  runFreezeHooksRecursive(root, &newlyFrozen);
  if (gc_only_freezing) CyclicNode::garbageCollectCycles(&newlyFrozen);

  for (auto* e: newlyFrozen) {
    ContainerHeader* container = e->container();
    DebugAssert(container->isFreezing());
    container->clearFreezing();
    container->freezeRef();
    container->makeShared();
  }
#else
  runFreezeHooksRecursive(root);

  #if USE_GC && !RTGC
    auto state = memoryState;
    // Free cyclic garbage to decrease number of analyzed objects.
    checkIfForceCyclicGcNeeded(state);
  #endif

  // Do DFS cycle detection.
  bool hasCycles = false;
  KRef firstBlocker = root->has_meta_object() && ((root->meta_object()->flags_ & MF_NEVER_FROZEN) != 0) ?
    root : nullptr;
  KStdVector<ContainerHeader*> order;
  depthFirstTraversal(rootContainer, &hasCycles, &firstBlocker, &order);
  MEMORY_LOG("Freeze subgraph of %p\n", root)
  if (firstBlocker != nullptr) {
    MEMORY_LOG("See freeze blocker for %p: %p\n", root, firstBlocker)
    ThrowFreezingException(root, firstBlocker);
  }
  ContainerHeaderSet newlyFrozen;
  // Now unmark all marked objects, and freeze them, if no cycles detected.
  if (hasCycles) {
    freezeCyclic(root, order, &newlyFrozen);
  } else  {
    freezeAcyclic(rootContainer, &newlyFrozen);
  }
  MEMORY_LOG("Graph of %p is %s with %d elements\n", root, hasCycles ? "cyclic" : "acyclic", newlyFrozen.size())

#if USE_GC
  // Now remove frozen objects from the toFree list.
  // TODO: optimize it by keeping ignored (i.e. freshly frozen) objects in the set,
  // and use it when analyzing toFree during collection.
  for (auto& container : *(state->toFree)) {
    if (!isMarkedAsRemoved(container) && container->frozen()) {
      RuntimeAssert(newlyFrozen.count(container) != 0, "Must be newly frozen");
      container = markAsRemoved(container);
    }
  }
#endif
#endif  
}

void ensureNeverFrozen(ObjHeader* object) {
   auto* container = object->container();
   if (container == nullptr || container->frozen())
      ThrowFreezingException(object, object);
   // TODO: note, that this API could not not be called on frozen objects, so no need to care much about concurrency,
   // although there's subtle race with case, where other thread freezes the same object after check.
   object->meta_object()->flags_ |= MF_NEVER_FROZEN;
}

void shareAny(ObjHeader* obj) {
  auto* container = obj->container();
  if (container == NULL || container->shared()) return;
  //RuntimeCheck(container->objectCount() == 1, "Must be a single object container");
  container->makeShared();
  traverseReferredObjects(obj, [](KRef field) {
    shareAny(field);
  });
}

void sharePermanentSubgraph(ObjHeader* obj) {
  auto* container = obj->container();
  if (!isFreeable(container)) return;
  //RuntimeCheck(container->objectCount() == 1, "Must be a single object container");
  container->makeSharedPermanent();
  atomicAdd(&allocCount, -1);

  traverseReferredObjects(obj, [](KRef field) {
    sharePermanentSubgraph(field);
  });
}

ScopedRefHolder::ScopedRefHolder(KRef obj): obj_(obj) {
  if (obj_) {
    retainRef(obj_);
  }
}

ScopedRefHolder::~ScopedRefHolder() {
  if (obj_) {
    ReleaseRef(obj_);
  }
}

#if USE_CYCLE_DETECTOR

// static
CycleDetectorRootset CycleDetector::collectRootset() {
  auto& detector = instance();
  CycleDetectorRootset rootset;
  LockGuard<SimpleMutex> guard(detector.lock_);
  for (auto* candidate: detector.candidateList_) {
    // Only frozen candidates are to be analyzed.
    if (!isPermanentOrFrozen(candidate))
      continue;
    rootset.roots.push_back(candidate);
    rootset.heldRefs.emplace_back(candidate);
    traverseReferredObjects(candidate, [&rootset, candidate](KRef field) {
      rootset.rootToFields[candidate].push_back(field);
      // TODO: There's currently a race here:
      // some other thread might null this field and destroy it in GC before
      // we put it in ScopedRefHolder.
      rootset.heldRefs.emplace_back(field);
    });
  }
  return rootset;
}

KStdVector<KRef> findCycleWithDFS(KRef root, const CycleDetectorRootset& rootset) {
  auto traverseFields = [&rootset](KRef obj, auto process) {
    auto it = rootset.rootToFields.find(obj);
    // If obj is in the rootset, use it's pinned state.
    if (it != rootset.rootToFields.end()) {
      const auto& fields = it->second;
      for (KRef field: fields) {
        if (field != nullptr) {
          process(field);
        }
      }
      return;
    }

    traverseReferredObjects(obj, process);
  };

  KStdVector<KStdVector<KRef>> toVisit;
  auto appendFieldsToVisit = [&toVisit, &traverseFields](KRef obj, const KStdVector<KRef>& currentPath) {
    traverseFields(obj, [&toVisit, &currentPath](KRef field) {
      auto path = currentPath;
      path.push_back(field);
      toVisit.emplace_back(std::move(path));
    });
  };

  appendFieldsToVisit(root, KRefList(1, root));

  KStdUnorderedSet<KRef> seen;
  seen.insert(root);
  while (!toVisit.empty()) {
    KStdVector<KRef> currentPath = std::move(toVisit.back());
    toVisit.pop_back();
    KRef node = currentPath[currentPath.size() - 1];

    if (node == root) {
      // Found a cycle.
      return currentPath;
    }

    // Already traversed this node.
    if (seen.count(node) != 0)
      continue;
    seen.insert(node);

    appendFieldsToVisit(node, currentPath);
  }

  return {};
}

template <typename C>
OBJ_GETTER(createAndFillArray, const C& container) {
  auto* result = AllocArrayInstance(theArrayTypeInfo, container.size(), OBJ_RESULT)->array();
  KRef* place = ArrayAddressOfElementAt(result, 0);
  RTGC_LOG("createAndFillArray: %p\n", result);

  for (KRef it: container) {
    UpdateHeapRef(place++, it, result->obj());
  }
  RETURN_OBJ(result->obj());
}

OBJ_GETTER0(detectCyclicReferences) {
  auto rootset = CycleDetector::collectRootset();

  KStdVector<KRef> cyclic;
  RTGC_LOG("detectCyclicReferences\n");

  for (KRef root: rootset.roots) {
    if (!findCycleWithDFS(root, rootset).empty()) {
      cyclic.push_back(root);
    }
  }

  RETURN_RESULT_OF(createAndFillArray, cyclic);
}

OBJ_GETTER(findCycle, KRef root) {
  auto rootset = CycleDetector::collectRootset();
  RTGC_LOG("findCycle: %p\n", root);
  auto cycle = findCycleWithDFS(root, rootset);
  if (cycle.empty()) {
    RETURN_OBJ(nullptr);
  }
  RETURN_RESULT_OF(createAndFillArray, cycle);
}

#endif  // USE_CYCLE_DETECTOR

}  // namespace

MetaObjHeader* ObjHeader::createMetaObject(TypeInfo** location) {
  TypeInfo* typeInfo = *location;
  RuntimeCheck(!hasPointerBits(typeInfo, OBJECT_TAG_MASK), "Object must not be tagged");
  RTGC_LOG("ObjHeader::createMetaObject: %p\n", typeInfo);

#if !KONAN_NO_THREADS
  if (typeInfo->typeInfo_ != typeInfo) {
    // Someone installed a new meta-object since the check.
    return reinterpret_cast<MetaObjHeader*>(typeInfo);
  }
#endif

  MetaObjHeader* meta = konanConstructInstance<MetaObjHeader>();
  meta->typeInfo_ = typeInfo;
#if KONAN_NO_THREADS
  *location = reinterpret_cast<TypeInfo*>(meta);
#else
  TypeInfo* old = __sync_val_compare_and_swap(location, typeInfo, reinterpret_cast<TypeInfo*>(meta));
  if (old != typeInfo) {
    // Someone installed a new meta-object since the check.
    konanFreeMemory(meta);
    meta = reinterpret_cast<MetaObjHeader*>(old);
  }
#endif
  return meta;
}

void ObjHeader::destroyMetaObject(TypeInfo** location, ForeignRefManager* manager) {
  MetaObjHeader* meta = clearPointerBits(*(reinterpret_cast<MetaObjHeader**>(location)), OBJECT_TAG_MASK);
  *const_cast<const TypeInfo**>(location) = meta->typeInfo_;
  if (meta->WeakReference.counter_ != nullptr) {
    WeakReferenceCounterClear(meta->WeakReference.counter_);
    if (manager == nullptr) {
      ZeroHeapRef(&meta->WeakReference.counter_);
    }
    else {
      deinitForeignRef(meta->WeakReference.counter_, manager);
    }
  }

#ifdef KONAN_OBJC_INTEROP
  Kotlin_ObjCExport_releaseAssociatedObject(meta->associatedObject_);
#endif

  konanFreeMemory(meta);
}

void ObjectContainer::Init(MemoryState* state, const TypeInfo* typeInfo) {
  RuntimeAssert(typeInfo->instanceSize_ >= 0, "Must be an object");
  uint32_t allocSize = sizeof(ContainerHeader) + typeInfo->instanceSize_;
  header_ = allocContainer(state, allocSize);
  RuntimeCheck(header_ != nullptr, "Cannot alloc memory");
  // One object in this container, no need to set.
  header_->setContainerSize(allocSize);
  RuntimeAssert(header_->objectCount() == 1, "Must work properly");
  // header->refCount_ is zero initialized by allocContainer().
  SetHeader(GetPlace(), typeInfo);
  OBJECT_ALLOC_EVENT(memoryState, typeInfo->instanceSize_, GetPlace())
  RTGC_LOG("allocate %s %p\n", CreateCStringFromString(typeInfo->relativeName_), header_);
}

void ArrayContainer::Init(MemoryState* state, const TypeInfo* typeInfo, uint32_t elements) {
  RuntimeAssert(typeInfo->instanceSize_ < 0, "Must be an array");
  uint32_t allocSize =
      sizeof(ContainerHeader) + arrayObjectSize(typeInfo, elements);
  header_ = allocContainer(state, allocSize);
  RuntimeCheck(header_ != nullptr, "Cannot alloc memory");
  // One object in this container, no need to set.
  header_->setContainerSize(allocSize);
  RuntimeAssert(header_->objectCount() == 1, "Must work properly");
  // header->refCount_ is zero initialized by allocContainer().
  GetPlace()->count_ = elements;
  SetHeader(GetPlace()->obj(), typeInfo);
  OBJECT_ALLOC_EVENT(memoryState, arrayObjectSize(typeInfo, elements), GetPlace()->obj())
  MEMORY_LOG("array allocated %s\n", CreateCStringFromString(typeInfo->relativeName_));
}

// TODO: store arena containers in some reuseable data structure, similar to
// finalizer queue.
void ArenaContainer::Init() {
  allocContainer(1024);
}

void ArenaContainer::Deinit() {
  MEMORY_LOG("Arena::Deinit start: %p\n", this)
  auto chunk = currentChunk_;
  while (chunk != nullptr) {
    // freeContainer() doesn't release memory when CONTAINER_TAG_STACK is set.
    MEMORY_LOG("Arena::Deinit free chunk %p\n", chunk)
    freeContainer(chunk->asHeader());
    chunk = chunk->next;
  }
  chunk = currentChunk_;
  while (chunk != nullptr) {
    auto toRemove = chunk;
    chunk = chunk->next;
    konanFreeMemory(toRemove);
  }
}

bool ArenaContainer::allocContainer(container_size_t minSize) {
  auto size = minSize + sizeof(ContainerHeader) + sizeof(ContainerChunk);
  size = alignUp(size, kContainerAlignment);
  // TODO: keep simple cache of container chunks.
  ContainerChunk* result = konanConstructSizedInstance<ContainerChunk>(size);
  RuntimeCheck(result != nullptr, "Cannot alloc memory");
  if (result == nullptr) return false;
  result->next = currentChunk_;
  result->arena = this;
  result->asHeader()->setRefCountAndFlags(1, CONTAINER_TAG_STACK_OR_PERMANANT);
  currentChunk_ = result;
  current_ = reinterpret_cast<uint8_t*>(result->asHeader() + 1);
  end_ = reinterpret_cast<uint8_t*>(result) + size;
  return true;
}

void* ArenaContainer::place(container_size_t size) {
  size = alignUp(size, kObjectAlignment);
  // Fast path.
  if (current_ + size < end_) {
    void* result = current_;
    current_ += size;
    return result;
  }
  if (!allocContainer(size)) {
    return nullptr;
  }
  void* result = current_;
  current_ += size;
  RuntimeAssert(current_ <= end_, "Must not overflow");
  return result;
}

#define ARENA_SLOTS_CHUNK_SIZE 16

ObjHeader** ArenaContainer::getSlot() {
  if (slots_ == nullptr || slotsCount_ >= ARENA_SLOTS_CHUNK_SIZE) {
    slots_ = PlaceArray(theArrayTypeInfo, ARENA_SLOTS_CHUNK_SIZE);
    slotsCount_ = 0;
  }
  return ArrayAddressOfElementAt(slots_, slotsCount_++);
}

ObjHeader* ArenaContainer::PlaceObject(const TypeInfo* type_info) {
  RuntimeAssert(type_info->instanceSize_ >= 0, "must be an object");
  uint32_t size = type_info->instanceSize_;
  ObjHeader* result = reinterpret_cast<ObjHeader*>(place(size));
  if (!result) {
    return nullptr;
  }
  OBJECT_ALLOC_EVENT(memoryState, type_info->instanceSize_, result)
  MEMORY_LOG("Arena allocate %s\n", CreateCStringFromString(type_info->relativeName_));
  currentChunk_->asHeader()->incObjectCount();
  setHeader(result, type_info);
  return result;
}

ArrayHeader* ArenaContainer::PlaceArray(const TypeInfo* type_info, uint32_t count) {
  RuntimeAssert(type_info->instanceSize_ < 0, "must be an array");
  container_size_t size = arrayObjectSize(type_info, count);
  ArrayHeader* result = reinterpret_cast<ArrayHeader*>(place(size));
  if (!result) {
    return nullptr;
  }
  OBJECT_ALLOC_EVENT(memoryState, arrayObjectSize(type_info, count), result->obj())
  MEMORY_LOG("Arena Array allocate %s\n", CreateCStringFromString(type_info->relativeName_));
  currentChunk_->asHeader()->incObjectCount();
  setHeader(result->obj(), type_info);
  result->count_ = count;
  return result;
}

void ScheduleDestroyContainer(MemoryState* state, ContainerHeader* container, const char* msg) {
  scheduleDestroyContainer(state, container, msg);
}

// API of the memory manager.
extern "C" {

// Private memory interface.
bool TryRetainRef(const ObjHeader* object) {
  return tryRetainRef(object);
}

void ReleaseRefStrict(const ObjHeader* object) {
  releaseRef<true>(const_cast<ObjHeader*>(object));
}
void ReleaseRefRelaxed(const ObjHeader* object) {
  releaseRef<false>(const_cast<ObjHeader*>(object));
}

ForeignRefContext InitLocalForeignRef(ObjHeader* object) {
  return initLocalForeignRef(object);
}

ForeignRefContext InitForeignRef(ObjHeader* object) {
  return initForeignRef(object);
}

void DeinitForeignRef(ObjHeader* object, ForeignRefContext context) {
  deinitForeignRef(object, context);
}

bool IsForeignRefAccessible(ObjHeader* object, ForeignRefContext context) {
  return isForeignRefAccessible(object, context);
}

void AdoptReferenceFromSharedVariable(ObjHeader* object) {
#if USE_GC
  if (IsStrictMemoryModel && object != nullptr && isShareable(object->container()))
    rememberNewContainer(object->container());
#endif  // USE_GC
}

// Public memory interface.
MemoryState* InitMemory() {
  return initMemory();
}

void DeinitMemory(MemoryState* memoryState) {
  deinitMemory(memoryState);
}

MemoryState* SuspendMemory() {
  return suspendMemory();
}

void ResumeMemory(MemoryState* state) {
  resumeMemory(state);
}

OBJ_GETTER(AllocInstanceStrict, const TypeInfo* type_info) {
  RETURN_RESULT_OF(allocInstance<true>, type_info);
}
OBJ_GETTER(AllocInstanceRelaxed, const TypeInfo* type_info) {
  RETURN_RESULT_OF(allocInstance<false>, type_info);
}

OBJ_GETTER(AllocArrayInstanceStrict, const TypeInfo* typeInfo, int32_t elements) {
  RETURN_RESULT_OF(allocArrayInstance<true>, typeInfo, elements);
}
OBJ_GETTER(AllocArrayInstanceRelaxed, const TypeInfo* typeInfo, int32_t elements) {
  RETURN_RESULT_OF(allocArrayInstance<false>, typeInfo, elements);
}

OBJ_GETTER(InitInstanceStrict,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*)) {
  RETURN_RESULT_OF(initInstance<true>, location, typeInfo, ctor);
}
OBJ_GETTER(InitInstanceRelaxed,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*)) {
  RETURN_RESULT_OF(initInstance<false>, location, typeInfo, ctor);
}

OBJ_GETTER(InitSharedInstanceStrict,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*)) {
  RETURN_RESULT_OF(initSharedInstance<true>, location, typeInfo, ctor);
}
OBJ_GETTER(InitSharedInstanceRelaxed,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*)) {
  RETURN_RESULT_OF(initSharedInstance<false>, location, typeInfo, ctor);
}

void SetStackRefStrict(ObjHeader** location, const ObjHeader* object) {
  setStackRef<true>(location, object);
}
void SetStackRefRelaxed(ObjHeader** location, const ObjHeader* object) {
  setStackRef<false>(location, object);
}

void SetHeapRefStrict(ObjHeader** location, const ObjHeader* object) {
  setHeapRef<true>(location, object);
}
void SetHeapRefRelaxed(ObjHeader** location, const ObjHeader* object) {
  setHeapRef<false>(location, object);
}

void ZeroHeapRef(ObjHeader** location) {
  zeroHeapRef(location);
}

void ZeroStackRefStrict(ObjHeader** location) {
  zeroStackRef<true>(location);
}
void ZeroStackRefRelaxed(ObjHeader** location) {
  zeroStackRef<false>(location);
}

void UpdateStackRefStrict(ObjHeader** location, const ObjHeader* object) {
  updateStackRef<true>(location, object);
}
void UpdateStackRefRelaxed(ObjHeader** location, const ObjHeader* object) {
  updateStackRef<false>(location, object);
}

void UpdateHeapRefStrict(ObjHeader** location, const ObjHeader* object, const ObjHeader* owner) {
  updateHeapRef<true>(location, object, owner);
}
void UpdateHeapRefRelaxed(ObjHeader** location, const ObjHeader* object, const ObjHeader* owner) {
  updateHeapRef<false>(location, object, owner);
}

void UpdateReturnRefStrict(ObjHeader** returnSlot, const ObjHeader* value) {
  updateReturnRef<true>(returnSlot, value);
}
void UpdateReturnRefRelaxed(ObjHeader** returnSlot, const ObjHeader* value) {
  updateReturnRef<false>(returnSlot, value);
}

void ZeroStackLocalArrayRefs(ArrayHeader* array) {
  RTGC_LOG("ZeroStackLocalArrayRefs: %p\n", array);

  for (uint32_t index = 0; index < array->count_; ++index) {
    ObjHeader** location = ArrayAddressOfElementAt(array, index);
    ZeroStackRef(location);
  }
}

void UpdateHeapRefIfNull(ObjHeader** location, const ObjHeader* object) {
  updateHeapRefIfNull(location, object);
}

OBJ_GETTER(SwapHeapRefLocked,
    ObjHeader** location, ObjHeader* expectedValue, ObjHeader* newValue, int32_t* spinlock, ObjHeader* owner, int32_t* cookie) {
  RETURN_RESULT_OF(swapHeapRefLocked, location, expectedValue, newValue, spinlock, owner, cookie);
}

void SetHeapRefLocked(ObjHeader** location, ObjHeader* newValue, int32_t* spinlock, ObjHeader* owner, int32_t* cookie) {
  setHeapRefLocked(location, newValue, spinlock, owner, cookie);
}

OBJ_GETTER(ReadHeapRefLocked, ObjHeader** location, int32_t* spinlock, ObjHeader* owner, int32_t* cookie) {
  RETURN_RESULT_OF(readHeapRefLocked, location, spinlock, owner, cookie);
}

OBJ_GETTER(ReadHeapRefNoLock, ObjHeader* object, KInt index) {
  RETURN_RESULT_OF(readHeapRefNoLock, object, index);
}

void EnterFrameStrict(ObjHeader** start, int parameters, int count) {
  enterFrame<true>(start, parameters, count);
}
void EnterFrameRelaxed(ObjHeader** start, int parameters, int count) {
  enterFrame<false>(start, parameters, count);
}

void LeaveFrameStrict(ObjHeader** start, int parameters, int count) {
  leaveFrame<true>(start, parameters, count);
}
void LeaveFrameRelaxed(ObjHeader** start, int parameters, int count) {
  leaveFrame<false>(start, parameters, count);
}

const ObjHeader* LeaveFrameAndReturnRefStrict(ObjHeader** start, int param_count, ObjHeader** resultSlot, const ObjHeader* returnRef) {
  return leaveFrameAndReturnRef<true>(start, param_count, resultSlot, returnRef);
}
const ObjHeader* LeaveFrameAndReturnRefRelaxed(ObjHeader** start, int param_count, ObjHeader** resultSlot, const ObjHeader* returnRef) {
  return leaveFrameAndReturnRef<false>(start, param_count, resultSlot, returnRef);
}


void Kotlin_native_internal_GC_collect(KRef) {
#if USE_GC
  garbageCollect();
#endif
}

void Kotlin_native_internal_GC_collectCyclic(KRef) {
#if USE_CYCLIC_GC
  if (g_hasCyclicCollector)
    cyclicScheduleGarbageCollect();
#else
  ThrowIllegalArgumentException();
#endif
}

void Kotlin_native_internal_GC_suspend(KRef) {
#if USE_GC
  suspendGC();
#endif
}

void Kotlin_native_internal_GC_resume(KRef) {
#if USE_GC
  resumeGC();
#endif
}

void Kotlin_native_internal_GC_stop(KRef) {
#if USE_GC
  stopGC();
#endif
}

void Kotlin_native_internal_GC_start(KRef) {
#if USE_GC
  startGC();
#endif
}

void Kotlin_native_internal_GC_setThreshold(KRef, KInt value) {
#if USE_GC
  setGCThreshold(value);
#endif
}

KInt Kotlin_native_internal_GC_getThreshold(KRef) {
#if USE_GC
  return getGCThreshold();
#else
  return -1;
#endif
}

void Kotlin_native_internal_GC_setCollectCyclesThreshold(KRef, KLong value) {
#if USE_GC
  setGCCollectCyclesThreshold(value);
#endif
}

KLong Kotlin_native_internal_GC_getCollectCyclesThreshold(KRef) {
#if USE_GC
  return getGCCollectCyclesThreshold();
#else
  return -1;
#endif
}

void Kotlin_native_internal_GC_setThresholdAllocations(KRef, KLong value) {
#if USE_GC
  setGCThresholdAllocations(value);
#endif
}

KLong Kotlin_native_internal_GC_getThresholdAllocations(KRef) {
#if USE_GC
  return getGCThresholdAllocations();
#else
  return -1;
#endif
}

void Kotlin_native_internal_GC_setTuneThreshold(KRef, KInt value) {
#if USE_GC
  setTuneGCThreshold(value);
#endif
}

KBoolean Kotlin_native_internal_GC_getTuneThreshold(KRef) {
#if USE_GC
  return getTuneGCThreshold();
#else
  return false;
#endif
}

OBJ_GETTER(Kotlin_native_internal_GC_detectCycles, KRef) {
  RTGC_LOG("Kotlin_native_internal_GC_detectCycles");

#if USE_CYCLE_DETECTOR
  if (!KonanNeedDebugInfo || !Kotlin_memoryLeakCheckerEnabled()) RETURN_OBJ(nullptr);
  RETURN_RESULT_OF0(detectCyclicReferences);
#else
  RETURN_OBJ(nullptr);
#endif
}

OBJ_GETTER(Kotlin_native_internal_GC_findCycle, KRef, KRef root) {
  RTGC_LOG("Kotlin_native_internal_GC_findCycle");

#if USE_CYCLE_DETECTOR
  RETURN_RESULT_OF(findCycle, root);
#else
  RETURN_OBJ(nullptr);
#endif
}

KNativePtr CreateStablePointer(KRef any) {
  return createStablePointer(any);
}

void DisposeStablePointer(KNativePtr pointer) {
  disposeStablePointer(pointer);
}

OBJ_GETTER(DerefStablePointer, KNativePtr pointer) {
  RETURN_RESULT_OF(derefStablePointer, pointer);
}

OBJ_GETTER(AdoptStablePointer, KNativePtr pointer) {
  RETURN_RESULT_OF(adoptStablePointer, pointer);
}

bool ClearSubgraphReferences(ObjHeader* root, bool checked) {
  return clearSubgraphReferences(root, checked);
}

void FreezeSubgraph(ObjHeader* root) {
  freezeSubgraph(root);
}

// This function is called from field mutators to check if object's header is frozen.
// If object is frozen or permanent, an exception is thrown.
void MutationCheck(ObjHeader* obj) {
  if (obj->local()) return;
  auto* container = obj->container();
  if (container == nullptr || container->frozen())
    ThrowInvalidMutabilityException(obj);
}

void CheckLifetimesConstraint(ObjHeader* obj, ObjHeader* pointee) {
  if (!obj->local() && pointee != nullptr && pointee->local()) {
    konan::consolePrintf("Attempt to store a stack object %p into a heap object %p\n", pointee, obj);
    konan::consolePrintf("This is a compiler bug, please report it to https://kotl.in/issue\n");
    konan::abort();
  }
}

void EnsureNeverFrozen(ObjHeader* object) {
  ensureNeverFrozen(object);
}

void Kotlin_Any_share(ObjHeader* obj) {
  shareAny(obj);
}

void AddTLSRecord(MemoryState* memory, void** key, int size) {
  auto* tlsMap = memory->tlsMap;
  auto it = tlsMap->find(key);
  if (it != tlsMap->end()) {
    RuntimeAssert(it->second.second == size, "Size must be consistent");
    return;
  }
  KRef* start = reinterpret_cast<KRef*>(konanAllocMemory(size * sizeof(KRef)));
  tlsMap->emplace(key, std::make_pair(start, size));
}

void ClearTLSRecord(MemoryState* memory, void** key) {
  auto* tlsMap = memory->tlsMap;
  auto it = tlsMap->find(key);
  if (it != tlsMap->end()) {
    KRef* start = it->second.first;
    int count = it->second.second;
    for (int i = 0; i < count; i++) {
      UpdateStackRef(start + i, nullptr);
    }
    konanFreeMemory(start);
    tlsMap->erase(it);
  }
}

KRef* LookupTLS(void** key, int index) {
  auto* state = memoryState;
  auto* tlsMap = state->tlsMap;
  // In many cases there is only one module, so this one element cache.
  if (state->tlsMapLastKey == key) {
    return state->tlsMapLastStart + index;
  }
  auto it = tlsMap->find(key);
  RuntimeAssert(it != tlsMap->end(), "Must be there");
  RuntimeAssert(index < it->second.second, "Out of bound in TLS access");
  KRef* start = it->second.first;
  state->tlsMapLastKey = key;
  state->tlsMapLastStart = start;
  return start + index;
}


void GC_RegisterWorker(void* worker) {
#if USE_CYCLIC_GC
  cyclicAddWorker(worker);
#endif  // USE_CYCLIC_GC
}

void GC_UnregisterWorker(void* worker) {
#if USE_CYCLIC_GC
  cyclicRemoveWorker(worker, g_hasCyclicCollector);
#endif  // USE_CYCLIC_GC
}

void GC_CollectorCallback(void* worker) {
#if USE_CYCLIC_GC
  if (g_hasCyclicCollector)
    cyclicCollectorCallback(worker);
#endif   // USE_CYCLIC_GC
}

KBoolean Kotlin_native_internal_GC_getCyclicCollector(KRef gc) {
#if USE_CYCLIC_GC
  return g_hasCyclicCollector;
#else
  return false;
#endif  // USE_CYCLIC_GC
}

void Kotlin_native_internal_GC_setCyclicCollector(KRef gc, KBoolean value) {
#if USE_CYCLIC_GC
  g_hasCyclicCollector = value;
#else
  if (value)
    ThrowIllegalArgumentException();
#endif  // USE_CYCLIC_GC
}

} // extern "C"
