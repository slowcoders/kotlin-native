#include <string.h>
#include <stdio.h>

#include <cstddef> // for offsetof

#include "Alloc.h"
#include "KAssert.h"
#include "Atomic.h"
#include "Exceptions.h"
#include "KString.h"
#include "Memory.h"
#include "MemoryPrivate.hpp"
#include "Natives.h"
#include "Porting.h"
#include "Runtime.h"
#include "RTGCPrivate.h"
#include "KDebug.h"
#include "assert.h"
#include <pthread.h>

THREAD_LOCAL_VARIABLE RTGCMemState* memoryState;

int RTGCGlobal::cntRefChain = 0;
int RTGCGlobal::cntCyclicNodes = 0;

CyclicNode lastDummy;


// CyclicNode* RTGCGlobal::g_freeCyclicNode;
// GCRefChain* GCRefList::g_refChains;
// GCRefChain* RTGCGlobal::g_freeRefChain;
// CyclicNode* GCNode::g_cyclicNodes = NULL;

int RTGCGlobal::g_cntLocalCyclicTest = 0;
int RTGCGlobal::g_cntMemberCyclicTest = 0;

static pthread_t g_lockThread = NULL;
static int g_cntLock = 0;
THREAD_LOCAL_VARIABLE int32_t isHeapLocked = 0;
static const bool RECURSIVE_LOCK = false;
static const bool SKIP_REMOVE_ERROR = true;

void GCNode::rtgcLock() {
    if (RECURSIVE_LOCK) {
        pthread_t curr_thread = pthread_self();
        if (curr_thread != g_lockThread) {
            while (!__sync_bool_compare_and_swap(&g_lockThread, NULL, curr_thread)) {}
        }
    }
    g_cntLock ++;
}

void GCNode::rtgcUnlock() {
    if (RECURSIVE_LOCK) {
        RuntimeAssert(pthread_self() == g_lockThread, "unlock in wrong thread");
    }
    if (--g_cntLock == 0) {
        if (RECURSIVE_LOCK) {
            g_lockThread = NULL;
        }
    }
}

bool GCNode::isLocked() {
    if (RECURSIVE_LOCK) {
        pthread_t curr_thread = pthread_self();
        return curr_thread == g_lockThread;
    }
    else {
        return g_cntLock > 0;
    }
}

static int dump_recycle_log = 0;//ENABLE_RTGC_LOG;
static GCRefChain* popFreeChain() {
    assert(GCNode::isLocked());
    GCRefChain* freeChain = memoryState->g_freeRefChain;
    if (freeChain == NULL) {
        RTGC_LOG("Insufficient RefChains!");
        if (ENABLE_RTGC_LOG) {
            GCNode::dumpGCLog();
            dump_recycle_log ++;
        }
        CyclicNode::detectCycles();
        freeChain = memoryState->g_freeRefChain;
        if (ENABLE_RTGC_LOG) {
            dump_recycle_log --;
            GCNode::dumpGCLog();
        }
        if (freeChain == NULL) {
            RTGC_LOG("Out of RefChains!");
            ThrowOutOfMemoryError();
        }
    }
    memoryState->g_freeRefChain = (GCRefChain*)GET_NEXT_FREE(freeChain);
    RTGCGlobal::cntRefChain ++;
    int node_id = freeChain - memoryState->g_refChains;
    if (true || node_id % 1000 == 0) {
       // printf("RTGC Ref chain: %d, %p\n", node_id, g_freeRefChain);
    }
    return freeChain;
}

static void recycleChain(GCRefChain* expired, const char* type) {
    SET_NEXT_FREE(expired, memoryState->g_freeRefChain);
    RTGCGlobal::cntRefChain --;
    if (ENABLE_RTGC_LOG && dump_recycle_log > 0) {//} || node_id % 1000 == 0) {
        RTGC_LOG("RTGC Recycle chain: %d\n", RTGCGlobal::cntRefChain);
    }
    memoryState->g_freeRefChain = expired;
}

static int getRefChainIndex(GCRefChain* chain) {
    return chain == NULL ? 0 : chain - memoryState->g_refChains;
}

GCRefChain* GCRefList::topChain() { 
    return first_ == 0 ? NULL : memoryState->g_refChains + first_; 
}

void GCRefList::push(GCObject* item) {
    GCRefChain* chain = popFreeChain();
    chain->obj_ = item;
    chain->next_ = this->topChain();
    first_ = getRefChainIndex(chain);
}

void GCRefList::remove(GCObject* item) {
    RuntimeAssert(this->first_ != 0, "RefList is empty");
    GCRefChain* prev = topChain();
    if (SKIP_REMOVE_ERROR && prev == NULL) {
        RTGC_LOG("can't remove item 0 %p", item);
        return;
    }
    if (prev->obj_ == item) {
        first_ = getRefChainIndex(prev->next_);
        recycleChain(prev, "first");
        return;
    }

    GCRefChain* chain = prev->next_;
    if (SKIP_REMOVE_ERROR && chain == NULL) {
        RTGC_LOG("can't remove item 1 %p", item);
        return;
    }
    while (chain->obj_ != item) {
        prev = chain;
        chain = chain->next_;
        if (SKIP_REMOVE_ERROR && chain == NULL) {
            RTGC_LOG("can't remove item 2 %p", item);
            return;
        }
    }
    prev->next_ = chain->next_;
    recycleChain(chain, "next");
}

void GCRefList::moveTo(GCObject* item, GCRefList* receiver) {
    RuntimeAssert(this->first_ != 0, "RefList is empty");
    GCRefChain* prev = topChain();
    if (prev->obj_ == item) {
        first_ = getRefChainIndex(prev->next_);
        // move to receiver;
        prev->next_ = receiver->topChain();
        receiver->first_ = getRefChainIndex(prev);
        return;
    }

    GCRefChain* chain = prev->next_;
    while (chain->obj_ != item) {
        prev = chain;
        chain = chain->next_;
    }
    prev->next_ = chain->next_;
    // move to receiver;
    chain->next_ = receiver->topChain();
    receiver->first_ = getRefChainIndex(chain);
}

GCObject* GCRefList::pop() { 
    GCRefChain* chain = topChain(); 
    if (chain == NULL) { 
        return NULL;
    }
    first_ = getRefChainIndex(chain->next_); 
    GCObject* obj = chain->obj();
    recycleChain(chain, "pop");
    return obj;
}

bool GCRefList::tryRemove(GCObject* item) {
    GCRefChain* prev = NULL;
    for (GCRefChain* chain = topChain(); chain != NULL; chain = chain->next()) {
        if (chain->obj_ == item) {
            if (prev == NULL) {
                first_ = getRefChainIndex(chain->next_);
            }
            else {
                prev->next_ = chain->next_;
            }
            recycleChain(chain, "first");
            return true;
        }
        prev = chain;
    }
    return false;
}

GCRefChain* GCRefList::find(GCObject* item) {
    for (GCRefChain* chain = topChain(); chain != NULL; chain = chain->next_) {
        if (chain->obj_ == item) {
            return chain;
        }
    }
    return NULL;
}

GCRefChain* GCRefList::find(int node_id) {    
    for (GCRefChain* chain = topChain(); chain != NULL; chain = chain->next_) {
        if (chain->obj_->getNodeId() == node_id) {
            return chain;
        }
    }
    return NULL;
}

void GCRefList::setFirst(GCRefChain* newFirst) {
    if (ENABLE_RTGC_LOG && dump_recycle_log > 0) {//} || node_id % 1000 == 0) {
         RTGC_LOG("RTGC setFirst %p, top %p\n", newFirst, topChain());
    }
    for (GCRefChain* chain = topChain(); chain != newFirst; ) {
        GCRefChain* next = chain->next_;
        recycleChain(chain, "setLast");
        chain = next;
    }
    this->first_ = getRefChainIndex(newFirst);
}

void OnewayNode::dealloc() {
    RuntimeAssert(isLocked(), "GCNode is not locked")
    if (ENABLE_RTGC_LOG && dump_recycle_log > 0) {//} || node_id % 1000 == 0) {
         RTGC_LOG("OnewayNode::dealloc, top %p\n", externalReferrers.topChain());
    }
    externalReferrers.clear();
}


void GCNode::dumpGCLog() {
    printf("** cntRefChain %d\n", RTGCGlobal::cntRefChain);
    printf("** cntCyclicNodes %d\n", RTGCGlobal::cntCyclicNodes);
    printf("** cntLocalCyclicTest %d\n", RTGCGlobal::g_cntLocalCyclicTest);
    printf("** cntMemberCyclicTest %d\n", RTGCGlobal::g_cntMemberCyclicTest);
}

extern "C" {

void Kotlin_native_internal_GC_rtgcLog(KRef __unused) {
    GCNode::dumpGCLog();
}

KInt Kotlin_native_internal_GC_refCount(KRef __unused, KRef obj) {
    if (obj == NULL) return -1;
    return obj->container()->refCount();
}
};


int CyclicNode::getId() { 
    return (this - memoryState->g_cyclicNodes) + CYCLIC_NODE_ID_START; 
}

CyclicNode* CyclicNode::getNode(int nodeId) {
    if (nodeId < CYCLIC_NODE_ID_START) {
        return NULL;
    }
    return memoryState->g_cyclicNodes + nodeId - CYCLIC_NODE_ID_START;
}

void GCNode::initMemory(RTGCMemState* memState) {
    RTGCGlobal::init(memState);
    memoryState = memState;
}

void RTGCGlobal::validateMemPool() {
    // CyclicNode* node = g_freeCyclicNode;
    // for (; node != NULL; node ++) {
    //     if (GET_NEXT_FREE(node) == NULL) break;
    //     assert(GET_NEXT_FREE(node) == node +1);
    // }
    // assert(node == g_cyclicNodes + CNT_CYCLIC_NODE - 1);
}

void RTGCGlobal::init(RTGCMemState* memoryState) {
    if (memoryState->g_cyclicNodes != NULL) return;
    // printf("GCNode initialized");

    memoryState->g_freeCyclicNode = new CyclicNode[CNT_CYCLIC_NODE];
    memoryState->g_cyclicNodes = memoryState->g_freeCyclicNode;
    memoryState->g_freeRefChain = new GCRefChain[CNT_REF_CHAIN];
    memoryState->g_damagedCylicNodes = &lastDummy;


    memoryState->g_refChains = memoryState->g_freeRefChain - 1;
    RTGC_LOG("g_freeRefChain = %p\n", memoryState->g_refChains + 1);

    int i = CNT_CYCLIC_NODE-1;
    for (CyclicNode* node = memoryState->g_freeCyclicNode; --i >= 0;) {
        node = (CyclicNode*)SET_NEXT_FREE(node, node + 1);
    }

    i = CNT_REF_CHAIN-1;
    for (GCRefChain* node = memoryState->g_freeRefChain; --i >= 0;) {
        node = (GCRefChain*)SET_NEXT_FREE(node, node + 1);
    }

    ::memoryState = memoryState;
    validateMemPool();
}

bool enable_rtgc_trap = ENABLE_RTGC_LOG;
bool rtgc_trap() {
    return enable_rtgc_trap;
}

void RTGC_Error(GCObject* obj) {
    if (obj != NULL) {
        RTGC_dumpRefInfo(obj);
    }
    ThrowOutOfMemoryError();
}

void RTGC_dumpRefInfo(GCObject* obj) {
    static const char* UNKNOWN = "???";
    const TypeInfo* typeInfo = ((ObjHeader*)(obj+1))->type_info();
    const char* classname = (typeInfo != NULL && typeInfo->relativeName_ != NULL)
        ? CreateCStringFromString(typeInfo->relativeName_) : UNKNOWN;
    printf("%s %p:%d rc=%p, tag=%d flags=%x\n", 
        classname, obj, obj->getNodeId(), 
        (void*)obj->refCount(), (obj->tag() >> 7), obj->getFlags());
    if (classname != UNKNOWN) konan::free((void*)classname);
    rtgc_trap();
}