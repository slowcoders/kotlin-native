/*
 * Copyright 2010-2018 JetBrains s.r.o.
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

#ifndef RUNTIME_MEMORY_H
#define RUNTIME_MEMORY_H
#include <stdio.h>

#include "KAssert.h"
#include "Common.h"
#include "TypeInfo.h"
#include "Atomic.h"


typedef enum {
  CONTAINER_TAG_GC_MARKED   = 0x01, // Unused, Reserved for TRACE_STATE
  CONTAINER_TAG_GC_BUFFERED = 0x02, // Unused, Reserved for TRACE_STATE

  // Container is frozen, could only refer to other frozen objects.
  CONTAINER_TAG_FROZEN = 0x04,

  // Atomic container, reference counter is atomically updated.
  CONTAINER_TAG_SHARED = 0x08,

  // 
  CONTAINER_TAG_ACYCLIC = 0x10,

  // no need to free, children cleanup still shall be there.
  CONTAINER_TAG_NOT_FREEABLE = 0x20,

  NEED_CYCLIC_TEST = 0x40,

  CONTAINER_TAG_STACK_OR_PERMANANT = CONTAINER_TAG_NOT_FREEABLE,//1 << 7, // Unused, Reserved
  CONTAINER_TAG_FREEZING    = 0x80,
  
  CONTAINER_TAG_GC_SEEN     = 0x100,
  // If indeed has more that one object.
  CONTAINER_TAG_GC_HAS_OBJECT_COUNT = 0x200,


  // Shift to get actual object count, if has it.
  CONTAINER_TAG_GC_SHIFT     = 10,
  CONTAINER_TAG_GC_MASK      = (1 << CONTAINER_TAG_GC_SHIFT) - 1,
  CONTAINER_TAG_GC_INCREMENT = 1 << CONTAINER_TAG_GC_SHIFT,
  // Color mask of a container.
  //CONTAINER_TAG_COLOR_SHIFT   = 3,
  //CONTAINER_TAG_GC_COLOR_MASK = (1 << CONTAINER_TAG_COLOR_SHIFT) - 1,
  // Colors.

  /*
  // In use or free.
  CONTAINER_TAG_GC_BLACK  = 0,
  // Possible member of garbage cycle.
  CONTAINER_TAG_GC_GRAY   = 1,
  // Member of garbage cycle.
  CONTAINER_TAG_GC_WHITE  = 2,
  // Possible root of cycle.
  CONTAINER_TAG_GC_PURPLE = 3,
  // Acyclic.
  CONTAINER_TAG_GC_GREEN  = 4,
  // Orange and red are currently unused.
  // Candidate cycle awaiting epoch.
  CONTAINER_TAG_GC_ORANGE = 5,
  // Candidate cycle awaiting sigma computation.
  CONTAINER_TAG_GC_RED    = 6,
  */
  // Individual state bits used during GC and freezing.
  
} ContainerTag;

typedef enum {
  // Must match to permTag() in Kotlin.
  OBJECT_TAG_PERMANENT_CONTAINER = 1 << 0,
  OBJECT_TAG_NONTRIVIAL_CONTAINER = 1 << 1,
  // Keep in sync with immTypeInfoMask in Kotlin.
  OBJECT_TAG_MASK = (1 << 2) - 1
} ObjectTag;

typedef uint32_t container_size_t;
#include "RTGC.h"

const bool IS_SHARED_PERMANENT_NEVER_FREEABLE = true;

// Header of all container objects. Contains reference counter.
struct ContainerHeader {
private:  
  // Reference counter of container. Uses CONTAINER_TAG_SHIFT, lower bits of counter
  // for container type (for polymorphism in ::Release()).
  union {
    RTGCRef rtgc;
    uint64_t count;
  } ref_;

  GCRefList rtNode;
  // // Number of objects in the container.
  // uint32_t objectCount_;
  // uint32_t rtNode.flags_;   
public:  


  // inline bool local() const {
  //     return (rtNode.flags_ & CONTAINER_TAG_MASK) == CONTAINER_TAG_LOCAL;
  // }

  inline bool frozen() const {
    return (rtNode.flags_ & CONTAINER_TAG_FROZEN) != 0;
  }

  inline bool frozen_or_freezing() const {
    return (rtNode.flags_ & (CONTAINER_TAG_FROZEN | CONTAINER_TAG_FREEZING)) != 0;
  }

  inline void markAcyclic() {
    rtNode.flags_ |= CONTAINER_TAG_ACYCLIC;
  }

  inline bool isAcyclic() const {
    return (rtNode.flags_ & CONTAINER_TAG_ACYCLIC) != 0;
  }

  inline bool freeable() const {
    return (rtNode.flags_ & (CONTAINER_TAG_STACK_OR_PERMANANT)) == 0;
  }

  inline void setRefCountAndFlags(uint32_t refCount, uint16_t flags) {
    ref_.count = refCount; rtNode.flags_ = flags;
  }

  inline void freezeRef() {
    if (!frozen()) {
      RuntimeAssert(!isNeedCyclicTest(), "garbageCollect() must be executed before freezerRef()");
      clearFreezing();
      rtNode.flags_ |= CONTAINER_TAG_FROZEN;
      if (!isAcyclic() && !isInCyclicNode()) {
        markAcyclic();
        if (ref_.rtgc.obj != 0) {
          getNode()->externalReferrers.clear();
          int64_t cntMember = ref_.rtgc.obj;
          //ref_.rtgc.node = 0;
          ref_.rtgc.obj = 0;
          ref_.count += cntMember;
        }
      }
    }
  }

  inline void makeShared() {
    rtNode.flags_ |= CONTAINER_TAG_SHARED;
  }

  inline void markFreezing() {
    rtNode.flags_ |= CONTAINER_TAG_FREEZING;
  }

  inline void clearFreezing() {
    rtNode.flags_ &= ~CONTAINER_TAG_FREEZING;
  }

  inline void makeSharedPermanent() {
    int flags = CONTAINER_TAG_FROZEN | CONTAINER_TAG_SHARED;
    if (IS_SHARED_PERMANENT_NEVER_FREEABLE) {
      if (isNeedCyclicTest()) {
        clearNeedCyclicTest();
      }
      flags |= CONTAINER_TAG_STACK_OR_PERMANANT | CONTAINER_TAG_ACYCLIC;
    }
    else {
      RuntimeAssert(!isNeedCyclicTest(), "garbageCollect() must be executed before makeSharedPermanent()");
      if (!isInCyclicNode()) {
        flags |= CONTAINER_TAG_ACYCLIC;
      }
    }
    rtNode.flags_ |= flags;
  }

  inline bool shared() const {
    return (rtNode.flags_ & CONTAINER_TAG_SHARED) != 0;
  }

  // inline bool stack() const {
  //   return (rtNode.flags_ & CONTAINER_TAG_MASK) == CONTAINER_TAG_STACK;
  // }

  inline int64_t refCount() const {
    return ref_.count & RTGC_REF_COUNT_MASK;
  }

  inline void setRefCount(int64_t refCount) {
    ref_.count = (ref_.count & ~RTGC_REF_COUNT_MASK) + refCount;
  }

  template <bool Atomic>
  inline void incRefCount() {
#ifdef KONAN_NO_THREADS
    ref_.count += RTGC_ROOT_REF_INCREEMENT;
#else
    int64_t value __attribute__((unused));
    if (Atomic)
       value =__sync_add_and_fetch(&ref_.count, RTGC_ROOT_REF_INCREEMENT);
     else
       value = (ref_.count += RTGC_ROOT_REF_INCREEMENT);
#endif
#if KONAN_ENABLE_ASSERT
  RuntimeAssert(isAcyclic(), "!Acyclic objct");
    RuntimeAssert(value != 0, "refCount overflow");
#endif    
  }

  template <bool Atomic>
  inline bool tryIncRefCount() {
    if (Atomic) {
      while (true) {
        uint64_t currentRefCount_ = ref_.count;
        if (refCount() > 0) {
          if (compareAndSet(&ref_.count, currentRefCount_, currentRefCount_ + RTGC_ROOT_REF_INCREEMENT)) {
            return true;
          }
        } else {
          return false;
        }
      }
    } else {
      // Note: tricky case here is doing this during cycle collection.
      // This can actually happen due to deallocation hooks.
      // Fortunately by this point reference counts have been made precise again.
      if (refCount() > 0) {
        incRefCount</* Atomic = */ false>();
        return true;
      } else {
        return false;
      }
    }
  }

  GCNode* getNode() {
#if KONAN_ENABLE_ASSERT
    if (ref_.rtgc.node == 0) {
      RTGC_dumpRefInfo0(this);
    }
    RuntimeAssert(ref_.rtgc.node != 0, "node not initialized");
#endif    
    if (this->isInCyclicNode()) {
      return CyclicNode::getNode(ref_.rtgc.node);
    }
    else {
      return (OnewayNode*)&rtNode;
    }
  }

  int32_t getFlags() {
    return this->rtNode.flags_;
  }

  OnewayNode* getLocalOnewayNode() {
    return isInCyclicNode() ? NULL : (OnewayNode*)&rtNode;
  }

  CyclicNode* getLocalCyclicNode() {
    return CyclicNode::getNode(getNodeId());
  }

  bool isGCNodeAttached() {
    return ref_.rtgc.node != 0;
  }
  
  RTGCRef getRTGCRef() {
    return ref_.rtgc;
  }

  GCNode* attachNode() {
    if (!isGCNodeAttached()) {
      ref_.rtgc.node = 1;
    }
    return getNode();
  }

  void markNeedCyclicTest() {
    if (RTGC_STATISTCS && !isNeedCyclicTest()) RTGCGlobal::g_cntAddCyclicTest ++;
    this->rtNode.flags_ |= NEED_CYCLIC_TEST;
  }

  bool isNeedCyclicTest() {
    return (this->rtNode.flags_ & NEED_CYCLIC_TEST) != 0;
  }

  void clearNeedCyclicTest() {
    if (RTGC_STATISTCS && isNeedCyclicTest()) RTGCGlobal::g_cntRemoveCyclicTest ++;
    this->rtNode.flags_ &= ~NEED_CYCLIC_TEST;
  }

  void markDestroyed() {
    this->rtNode.flags_ |= CONTAINER_TAG_NOT_FREEABLE;
  }

  bool isGarbage() {
    return refCount() == 0;
  }

  bool isInCyclicNode() {
    return getNodeId() >= CYCLIC_NODE_ID_START;
  }

  int getNodeId() {
    return (int)ref_.rtgc.node;
  }

  void setNodeId(int node_id) {
    ref_.rtgc.node = node_id;
  }

  int getRootRefCount() {
    return ref_.rtgc.root;
  }

  template <bool Atomic>
  inline void incMemberRefCount(bool ignoreAcyclic = false) {
#ifdef KONAN_NO_THREADS
    ref_.count += RTGC_MEMBER_REF_INCREEMENT;
#else
    int64_t value __attribute__((unused))= Atomic ?
        __sync_add_and_fetch(&ref_.count, RTGC_MEMBER_REF_INCREEMENT) : ref_.count += RTGC_MEMBER_REF_INCREEMENT;
#endif
#if KONAN_ENABLE_ASSERT
    RuntimeAssert(ignoreAcyclic || !isAcyclic(), "Acyclic objct does not have member refCount");
    RuntimeAssert(((RTGCRef*)&value)->obj != 0, "member ref overflow");
#endif    
  }

  inline int getMemberRefCount() {
      return ref_.rtgc.obj;
  }


  inline void clearMemberRefCount() {
      ref_.rtgc.obj = 0;
  }

  template <bool Atomic>
  inline void decMemberRefCount() {
#if KONAN_ENABLE_ASSERT
    RuntimeAssert(!isAcyclic(), "Acyclic objct does not have member refCount");
    RuntimeAssert(ref_.rtgc.obj != 0, "member ref underflow");
#endif    
#ifdef KONAN_NO_THREADS
    int64_t value __attribute__((unused))= ref_.count -= RTGC_MEMBER_REF_INCREEMENT;
#else
    int64_t value __attribute__((unused))= Atomic ?
       __sync_sub_and_fetch(&ref_.count, RTGC_MEMBER_REF_INCREEMENT) : ref_.count -= RTGC_MEMBER_REF_INCREEMENT;
#endif
  }

  template <bool Atomic>
  inline RTGCRef incRootCount() {
#ifdef KONAN_NO_THREADS
    int64_t value __attribute__((unused))= ref_.count += RTGC_ROOT_REF_INCREEMENT;
#else
    int64_t value __attribute__((unused))= Atomic ?
       __sync_add_and_fetch(&ref_.count, RTGC_ROOT_REF_INCREEMENT) : ref_.count += RTGC_ROOT_REF_INCREEMENT;
#endif
#if KONAN_ENABLE_ASSERT
    RuntimeAssert(!isAcyclic(), "Acyclic objct does not have root refCount");
    RuntimeAssert(((RTGCRef*)&value)->root != 0, "root ref overflow");
#endif    
    return *(RTGCRef*)&value;
  }

  inline ObjHeader* asObjHeader() {
    return (ObjHeader*)(this + 1);
  }

  template <bool Atomic>
  inline RTGCRef decRootCount() {
#if KONAN_ENABLE_ASSERT
  RuntimeAssert(!isAcyclic(), "Acyclic objct does not have root refCount");
  RuntimeAssert(ref_.rtgc.root != 0, "root ref underflow");
#endif
#ifdef KONAN_NO_THREADS
    int64_t value __attribute__((unused))= ref_.count -= RTGC_ROOT_REF_INCREEMENT;
#else
    int64_t value __attribute__((unused))= Atomic ?
       __sync_sub_and_fetch(&ref_.count, RTGC_ROOT_REF_INCREEMENT) : ref_.count -= RTGC_ROOT_REF_INCREEMENT;
#endif
    return *(RTGCRef*)&value;
  }

  template <bool Atomic>
  inline int decRefCount() {
#if KONAN_ENABLE_ASSERT
  RuntimeAssert(isAcyclic(), "!Acyclic objct");
    RuntimeAssert(ref_.count != 0, "refCount underflow");
#endif    
#ifdef KONAN_NO_THREADS
    int value __attribute__((unused))= ref_.count -= RTGC_ROOT_REF_INCREEMENT;
#else
    int value __attribute__((unused))= Atomic ?
       __sync_sub_and_fetch(&ref_.count, RTGC_ROOT_REF_INCREEMENT) : ref_.count -= RTGC_ROOT_REF_INCREEMENT;
#endif
    return refCount();//value >> CONTAINER_TAG_SHIFT;
  }

  inline int decRefCount() {
  #if KONAN_ENABLE_ASSERT
    RuntimeAssert(isAcyclic(), "!Acyclic objct");
    RuntimeAssert(ref_.count != 0, "refCount underflow");
#endif    
#ifdef KONAN_NO_THREADS
      int value __attribute__((unused))= ref_.count -= RTGC_ROOT_REF_INCREEMENT;
  #else
      int value __attribute__((unused))= shared() ?
         __sync_sub_and_fetch(&ref_.count, RTGC_ROOT_REF_INCREEMENT) : ref_.count -= RTGC_ROOT_REF_INCREEMENT;
  #endif
      return refCount();//value >> CONTAINER_TAG_SHIFT;
  }


  inline unsigned objectCount() const {
    return (rtNode.flags_ & CONTAINER_TAG_GC_HAS_OBJECT_COUNT) != 0 ?
        (rtNode.flags_ >> CONTAINER_TAG_GC_SHIFT) : 1;
  }

  inline void incObjectCount() {
    RuntimeAssert((rtNode.flags_ & CONTAINER_TAG_GC_HAS_OBJECT_COUNT) != 0, "Must have object count");
    rtNode.flags_ += CONTAINER_TAG_GC_INCREMENT;
  }

  inline void setObjectCount(int count) {
    if (count == 1) {
      rtNode.flags_ &= ~CONTAINER_TAG_GC_HAS_OBJECT_COUNT;
    } else {
      rtNode.flags_ = (count << CONTAINER_TAG_GC_SHIFT) | CONTAINER_TAG_GC_HAS_OBJECT_COUNT;
    }
  }

  inline unsigned containerSize() const {
    RuntimeAssert((rtNode.flags_ & CONTAINER_TAG_GC_HAS_OBJECT_COUNT) == 0, "Must be single-object");
    return (rtNode.flags_ >> CONTAINER_TAG_GC_SHIFT);
  }

  inline void setContainerSize(unsigned size) {
    RuntimeAssert((rtNode.flags_ & CONTAINER_TAG_GC_HAS_OBJECT_COUNT) == 0, "Must not have object count");
    rtNode.flags_ = (rtNode.flags_ & CONTAINER_TAG_GC_MASK) | (size << CONTAINER_TAG_GC_SHIFT);
  }

  inline bool hasContainerSize() {
    return (rtNode.flags_ & CONTAINER_TAG_GC_HAS_OBJECT_COUNT) == 0;
  }

  // inline unsigned color() const {
  //   return rtNode.flags_ & CONTAINER_TAG_GC_COLOR_MASK;
  // }

  inline void setColorAssertIfGreen(unsigned color) {
    #if !RTGC
    RuntimeAssert(this->color() != CONTAINER_TAG_GC_GREEN, "Must not be green");
    setColorEvenIfGreen(color);
    #endif
  }

  inline void setColorEvenIfGreen(unsigned color) {
    // TODO: do we need atomic color update?
    #if !RTGC
    rtNode.flags_ = (rtNode.flags_ & ~CONTAINER_TAG_GC_COLOR_MASK) | color;
    #endif
  }

  inline void setColorUnlessGreen(unsigned color) {
    // TODO: do we need atomic color update?
    #if !RTGC
    unsigned objectCount = rtNode.flags_;//objectCount_;
    if ((objectCount & CONTAINER_TAG_GC_COLOR_MASK) != CONTAINER_TAG_GC_GREEN)
        rtNode.flags_ /*objectCount_*/ = (objectCount & ~CONTAINER_TAG_GC_COLOR_MASK) | color;
    #endif
  }

  inline bool buffered() const {
    return (rtNode.flags_ & CONTAINER_TAG_GC_BUFFERED) != 0;
  }

  inline void setBuffered() {
    rtNode.flags_ |= CONTAINER_TAG_GC_BUFFERED;
  }

  inline void resetBuffered() {
    rtNode.flags_ &= ~CONTAINER_TAG_GC_BUFFERED;
  }

  inline bool marked() const {
    return (rtNode.flags_ & CONTAINER_TAG_GC_MARKED) != 0;
  }

  inline void mark() {
    rtNode.flags_ |= CONTAINER_TAG_GC_MARKED;
  }

  inline void unMark() {
    rtNode.flags_ &= ~CONTAINER_TAG_GC_MARKED;
  }

  inline bool seen() const {
    return (rtNode.flags_ & CONTAINER_TAG_GC_SEEN) != 0;
  }

  inline void setSeen() {
    rtNode.flags_ |= CONTAINER_TAG_GC_SEEN;
  }

  inline void resetSeen() {
    rtNode.flags_ &= ~CONTAINER_TAG_GC_SEEN;
  }

  // Following operations only work on freed container which is in finalization queue.
  // We cannot use 'this' here, as it conflicts with aliasing analysis in clang.
  inline void setNextLink(ContainerHeader* next) {
    *reinterpret_cast<ContainerHeader**>(this + 1) = next;
  }

  inline ContainerHeader* nextLink() {
    return *reinterpret_cast<ContainerHeader**>(this + 1);
  }
};

struct ArrayHeader;
struct MetaObjHeader;

template <typename T>
ALWAYS_INLINE T* setPointerBits(T* ptr, unsigned bits) {
  return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(ptr) | bits);
}

template <typename T>
ALWAYS_INLINE T* clearPointerBits(T* ptr, unsigned bits) {
  return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(ptr) & ~static_cast<uintptr_t>(bits));
}

template <typename T>
ALWAYS_INLINE unsigned getPointerBits(T* ptr, unsigned bits) {
  return reinterpret_cast<uintptr_t>(ptr) & static_cast<uintptr_t>(bits);
}

template <typename T>
ALWAYS_INLINE bool hasPointerBits(T* ptr, unsigned bits) {
  return getPointerBits(ptr, bits) != 0;
}

// Header for the meta-object.
struct MetaObjHeader {
  // Pointer to the type info. Must be first, to match ArrayHeader and ObjHeader layout.
  const TypeInfo* typeInfo_;
  // Container pointer.
  ContainerHeader* container_;

#ifdef KONAN_OBJC_INTEROP
  void* associatedObject_;
#endif

  // Flags for the object state.
  int32_t flags_;

  struct {
    // Strong reference to the counter object.
    ObjHeader* counter_;
  } WeakReference;
};

// Header of every object.
struct ObjHeader {
  TypeInfo* typeInfoOrMeta_;

  const TypeInfo* type_info() const {
    return clearPointerBits(typeInfoOrMeta_, OBJECT_TAG_MASK)->typeInfo_;
  }

  bool has_meta_object() const {
    auto* typeInfoOrMeta = clearPointerBits(typeInfoOrMeta_, OBJECT_TAG_MASK);
    return (typeInfoOrMeta != typeInfoOrMeta->typeInfo_);
  }

  MetaObjHeader* meta_object() {
     return has_meta_object() ?
        reinterpret_cast<MetaObjHeader*>(clearPointerBits(typeInfoOrMeta_, OBJECT_TAG_MASK)) :
        createMetaObject(&typeInfoOrMeta_);
  }

  void setContainer(ContainerHeader* container) {
    meta_object()->container_ = container;
    // if (container->isStack()) {
    //   typeInfoOrMeta_ = setPointerBits(typeInfoOrMeta_, OBJECT_TAG_PERMANENT_CONTAINER);
    // }
    // else {
      typeInfoOrMeta_ = setPointerBits(typeInfoOrMeta_, OBJECT_TAG_NONTRIVIAL_CONTAINER);
    // }
  }

  ContainerHeader* container() const {
    unsigned bits = getPointerBits(typeInfoOrMeta_, OBJECT_TAG_MASK);
    if ((bits & (OBJECT_TAG_PERMANENT_CONTAINER | OBJECT_TAG_NONTRIVIAL_CONTAINER)) == 0)
      return reinterpret_cast<ContainerHeader*>(const_cast<ObjHeader*>(this)) - 1;
    if ((bits & OBJECT_TAG_PERMANENT_CONTAINER) != 0)
      return nullptr;
    return (reinterpret_cast<MetaObjHeader*>(clearPointerBits(typeInfoOrMeta_, OBJECT_TAG_MASK)))->container_;
  }

  inline bool local() const {
    unsigned bits = getPointerBits(typeInfoOrMeta_, OBJECT_TAG_MASK);
    return (bits & (OBJECT_TAG_PERMANENT_CONTAINER | OBJECT_TAG_NONTRIVIAL_CONTAINER)) ==
        (OBJECT_TAG_PERMANENT_CONTAINER | OBJECT_TAG_NONTRIVIAL_CONTAINER);
  }

  // Unsafe cast to ArrayHeader. Use carefully!
  ArrayHeader* array() { return reinterpret_cast<ArrayHeader*>(this); }
  const ArrayHeader* array() const { return reinterpret_cast<const ArrayHeader*>(this); }

  inline bool permanent() const {
    return hasPointerBits(typeInfoOrMeta_, OBJECT_TAG_PERMANENT_CONTAINER);
  }

  static MetaObjHeader* createMetaObject(TypeInfo** location);
  static void destroyMetaObject(TypeInfo** location);
};

// Header of value type array objects. Keep layout in sync with that of object header.
struct ArrayHeader {
  TypeInfo* typeInfoOrMeta_;

  const TypeInfo* type_info() const {
    return clearPointerBits(typeInfoOrMeta_, OBJECT_TAG_MASK)->typeInfo_;
  }

  ObjHeader* obj() { return reinterpret_cast<ObjHeader*>(this); }
  const ObjHeader* obj() const { return reinterpret_cast<const ObjHeader*>(this); }

  // Elements count. Element size is stored in instanceSize_ field of TypeInfo, negated.
  uint32_t count_;
};

inline bool isPermanentOrFrozen(ObjHeader* obj) {
    auto* container = obj->container();
    return container == nullptr || container->frozen();
}

#ifdef __cplusplus
extern "C" {
#endif

#define OBJ_RESULT __result__
#define OBJ_GETTER0(name) ObjHeader* name(ObjHeader** OBJ_RESULT)
#define OBJ_GETTER(name, ...) ObjHeader* name(__VA_ARGS__, ObjHeader** OBJ_RESULT)
#define MODEL_VARIANTS(returnType, name, ...)            \
   returnType name(__VA_ARGS__) RUNTIME_NOTHROW RTGC_NO_INLINE;         \
   returnType name##Strict(__VA_ARGS__) RUNTIME_NOTHROW; \
   returnType name##Relaxed(__VA_ARGS__) RUNTIME_NOTHROW;
#define RETURN_OBJ(value) { ObjHeader* __obj = value; \
    UpdateReturnRef(OBJ_RESULT, __obj);               \
    return __obj; }
#define RETURN_RESULT_OF0(name) {       \
    ObjHeader* __obj = name(OBJ_RESULT);  \
    return __obj;                         \
  }
#define RETURN_RESULT_OF(name, ...) {                   \
    ObjHeader* __result = name(__VA_ARGS__, OBJ_RESULT);  \
    return __result;                                      \
  }

struct MemoryState;

MemoryState* InitMemory();
void DeinitMemory(MemoryState*);

MemoryState* SuspendMemory();
void ResumeMemory(MemoryState* state);

//
// Object allocation.
//
// Allocation can happen in either GLOBAL, FRAME or ARENA scope. Depending on that,
// Alloc* or ArenaAlloc* is called. Regular alloc means allocation happens in the heap,
// and each object gets its individual container. Otherwise, allocator uses aux slot in
// an implementation-defined manner, current behavior is to keep arena pointer there.
// Arena containers are not reference counted, and is explicitly freed when leaving
// its owner frame.
// Escape analysis algorithm is the provider of information for decision on exact aux slot
// selection, and comes from upper bound esteemation of object lifetime.
//
OBJ_GETTER(AllocInstanceStrict, const TypeInfo* type_info) RUNTIME_NOTHROW;
OBJ_GETTER(AllocInstanceRelaxed, const TypeInfo* type_info) RUNTIME_NOTHROW;
OBJ_GETTER(AllocInstance, const TypeInfo* type_info) RUNTIME_NOTHROW;

OBJ_GETTER(AllocArrayInstanceStrict, const TypeInfo* type_info, int32_t elements);
OBJ_GETTER(AllocArrayInstanceRelaxed, const TypeInfo* type_info, int32_t elements);
OBJ_GETTER(AllocArrayInstance, const TypeInfo* type_info, int32_t elements);

OBJ_GETTER(InitInstanceStrict,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*));
OBJ_GETTER(InitInstanceRelaxed,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*));
OBJ_GETTER(InitInstance,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*));

OBJ_GETTER(InitSharedInstanceStrict,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*));
OBJ_GETTER(InitSharedInstanceRelaxed,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*));
OBJ_GETTER(InitSharedInstance,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*));

// Weak reference operations.
// Atomically clears counter object reference.
void WeakReferenceCounterClear(ObjHeader* counter);

//
// Object reference management.
//
// Reference management scheme we use assumes significant degree of flexibility, so that
// one could implement either pure reference counting scheme, or tracing collector without
// much ado.
// Most important primitive is Update*Ref() API, which modifies location to use new
// object reference. In pure reference counted scheme it will check old value,
// decrement reference, increment counter on the new value, and store it into the field.
// In tracing collector-like scheme, only field updates counts, and all other operations are
// essentially no-ops.
//
// On codegeneration phase we adopt following approaches:
//  - every stack frame has several slots, holding object references (allRefs)
//  - those are known by compiler (and shall be grouped together)
//  - it keeps all locally allocated objects in such slot
//  - all local variables keeping an object also allocate a slot
//  - most manipulations on objects happens in SSA variables and do no affect slots
//  - exception handlers knowns slot locations for every function, and can update references
//    in intermediate frames when throwing
//

// Controls the current memory model, is compile-time constant.
extern const bool IsStrictMemoryModel;

// Sets stack location.
MODEL_VARIANTS(void, SetStackRef, ObjHeader** location, const ObjHeader* object);
// Sets heap location.
MODEL_VARIANTS(void, SetHeapRef, ObjHeader** location, const ObjHeader* object);
// Zeroes heap location.
void ZeroHeapRef(ObjHeader** location) RUNTIME_NOTHROW;
// Zeroes an array.
void ZeroStackLocalArrayRefs(ArrayHeader* array) RUNTIME_NOTHROW;
// Zeroes stack location.
MODEL_VARIANTS(void, ZeroStackRef, ObjHeader** location);
// Updates stack location.
MODEL_VARIANTS(void, UpdateStackRef, ObjHeader** location, const ObjHeader* object);
// Updates heap/static data location.
MODEL_VARIANTS(void, UpdateHeapRef, ObjHeader** location, const ObjHeader* object, const ObjHeader* owner);
// Updates location if it is null, atomically.
MODEL_VARIANTS(void, UpdateHeapRefIfNull, ObjHeader** location, const ObjHeader* object);
// Updates reference in return slot.
MODEL_VARIANTS(void, UpdateReturnRef, ObjHeader** returnSlot, const ObjHeader* object);
// Compares and swaps reference with taken lock.
OBJ_GETTER(SwapHeapRefLocked,
    ObjHeader** location, ObjHeader* expectedValue, ObjHeader* newValue, int32_t* spinlock,
    ObjHeader* owner, int32_t* cookie) RUNTIME_NOTHROW;
// Sets reference with taken lock.
void SetHeapRefLocked(ObjHeader** location, ObjHeader* newValue, int32_t* spinlock,
    ObjHeader* owner, int32_t* cookie) RUNTIME_NOTHROW;
// Reads reference with taken lock.
OBJ_GETTER(ReadHeapRefLocked, ObjHeader** location, int32_t* spinlock, int32_t* cookie) RUNTIME_NOTHROW;
// Called on frame enter, if it has object slots.
MODEL_VARIANTS(void, EnterFrame, ObjHeader** start, int parameters, int count);
// Called on frame leave, if it has object slots.
MODEL_VARIANTS(void, LeaveFrame, ObjHeader** start, int parameters, int count);

MODEL_VARIANTS(const ObjHeader*, LeaveFrameAndReturnRef, ObjHeader** start, int param_count, ObjHeader** resultSlot, const ObjHeader* returnRef);

// Clears object subgraph references from memory subsystem, and optionally
// checks if subgraph referenced by given root is disjoint from the rest of
// object graph, i.e. no external references exists.
bool ClearSubgraphReferences(ObjHeader* root, bool checked) RUNTIME_NOTHROW;
// Creates stable pointer out of the object.
void* CreateStablePointer(ObjHeader* obj) RUNTIME_NOTHROW;
// Disposes stable pointer to the object.
void DisposeStablePointer(void* pointer) RUNTIME_NOTHROW;
// Translate stable pointer to object reference.
OBJ_GETTER(DerefStablePointer, void*) RUNTIME_NOTHROW;
// Move stable pointer ownership.
OBJ_GETTER(AdoptStablePointer, void*) RUNTIME_NOTHROW;
// Check mutability state.
void MutationCheck(ObjHeader* obj);
void CheckLifetimesConstraint(ObjHeader* obj, ObjHeader* pointee) RUNTIME_NOTHROW;
// Freeze object subgraph.
void FreezeSubgraph(ObjHeader* obj);
// Ensure this object shall block freezing.
void EnsureNeverFrozen(ObjHeader* obj);
// Add TLS object storage, called by the generated code.
void AddTLSRecord(MemoryState* memory, void** key, int size) RUNTIME_NOTHROW;
// Clear TLS object storage, called by the generated code.
void ClearTLSRecord(MemoryState* memory, void** key) RUNTIME_NOTHROW;
// Lookup element in TLS object storage.
ObjHeader** LookupTLS(void** key, int index) RUNTIME_NOTHROW;

// APIs for the async GC.
void GC_RegisterWorker(void* worker) RUNTIME_NOTHROW;
void GC_UnregisterWorker(void* worker) RUNTIME_NOTHROW;
void GC_CollectorCallback(void* worker) RUNTIME_NOTHROW;

#ifdef __cplusplus
}
#endif


struct FrameOverlay {
  void* arena;
  FrameOverlay* previous;
  // As they go in pair, sizeof(FrameOverlay) % sizeof(void*) == 0 is always held.
  int32_t parameters;
  int32_t count;
};

// Class holding reference to an object, holding object during C++ scope.
class ObjHolder {
 public:
   ObjHolder() : obj_(nullptr) {
     EnterFrame(frame(), 0, sizeof(*this)/sizeof(void*));
   }

   explicit ObjHolder(const ObjHeader* obj) {
     EnterFrame(frame(), 0, sizeof(*this)/sizeof(void*));
     ::SetStackRef(slot(), obj);
   }

   ~ObjHolder() {
     LeaveFrame(frame(), 0, sizeof(*this)/sizeof(void*));
   }

   ObjHeader* obj() { return obj_; }

   const ObjHeader* obj() const { return obj_; }

   ObjHeader** slot() {
     return &obj_;
   }

   void clear() { ::ZeroStackRef(&obj_); }

 private:
   ObjHeader** frame() { return reinterpret_cast<ObjHeader**>(&frame_); }

   FrameOverlay frame_;
   ObjHeader* obj_;
};

//! TODO Follow the Rule of Zero to prevent dangling on unintented copy ctor
class ExceptionObjHolder {
 public:
   explicit ExceptionObjHolder(const ObjHeader* obj) {
     ::SetStackRef(&obj_, obj);
   }

   ~ExceptionObjHolder() {
     ZeroStackRef(&obj_);
   }

   ObjHeader* obj() { return obj_; }

   const ObjHeader* obj() const { return obj_; }

 private:
   ObjHeader* obj_;
};

class ForeignRefManager;
typedef ForeignRefManager* ForeignRefContext;


#ifdef RTGC

void updateHeapRef_internal(const ObjHeader* object, const ObjHeader* old, const ObjHeader* owner) RTGC_NO_INLINE;
void freeContainer(ContainerHeader* header, int garbageNodeId=-1) RTGC_NO_INLINE;
#endif

#endif // RUNTIME_MEMORY_H
