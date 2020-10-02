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

int RTGCGlobal::cntRefChain = 0;
int RTGCGlobal::cntCyclicNodes = 0;

CyclicNode* RTGCGlobal::g_freeCyclicNode;


GCRefChain* GCRefList::g_refChains;
GCRefChain* RTGCGlobal::g_freeRefChain;

CyclicNode* GCNode::g_cyclicNodes = NULL;
int RTGCGlobal::g_cntLocalCyclicTest = 0;
int RTGCGlobal::g_cntMemberCyclicTest = 0;

static pthread_t g_lockThread = NULL;
static int g_cntLock = 0;
//std::recursive_mutex g_mutex;


void GCNode::rtgcLock() {
    pthread_t curr_thread = pthread_self();
    if (curr_thread != g_lockThread) {
        while (!__sync_bool_compare_and_swap(&g_lockThread, NULL, curr_thread)) {}
    }
    g_cntLock ++;
}

void GCNode::rtgcUnlock() {
    RuntimeAssert(pthread_self() == g_lockThread, "unlock in wrong thread");
    if (--g_cntLock == 0) {
        g_lockThread = NULL;
    }
}

bool GCNode::isLocked() {
    pthread_t curr_thread = pthread_self();
    return curr_thread == g_lockThread;
}

static int dump_recycle_log = 0;
static const int LOCKFREE_REF_CHAIN_POOL = true;
static GCRefChain* popFreeChain() {
    assert(LOCKFREE_REF_CHAIN_POOL || GCNode::isLocked());
    GCRefChain* freeChain = RTGCGlobal::g_freeRefChain;
    if (freeChain == NULL) {
        RTGC_LOG("Insufficient RefChains!");
        if (ENABLE_RTGC_LOG) {
            GCNode::dumpGCLog();
            dump_recycle_log ++;
        }
        CyclicNode::detectCycles();
        freeChain = RTGCGlobal::g_freeRefChain;
        if (ENABLE_RTGC_LOG) {
            dump_recycle_log --;
            GCNode::dumpGCLog();
        }
        if (freeChain == NULL) {
            RTGC_LOG("Out of RefChains!");
            ThrowOutOfMemoryError();
        }
    }
    if (LOCKFREE_REF_CHAIN_POOL) {
        while (true) {
            GCRefChain* next = (GCRefChain*)GET_NEXT_FREE(freeChain);
            if (__sync_bool_compare_and_swap(&RTGCGlobal::g_freeRefChain, freeChain, next)) break;
            freeChain = RTGCGlobal::g_freeRefChain;
        }
    }
    else {
        RTGCGlobal::g_freeRefChain = (GCRefChain*)GET_NEXT_FREE(freeChain);
    }
    RTGCGlobal::cntRefChain ++;
    int node_id = freeChain - GCRefList::g_refChains;
    if (true || node_id % 1000 == 0) {
       // printf("RTGC Ref chain: %d, %p\n", node_id, g_freeRefChain);
    }
    return freeChain;
}

static void recycleChain(GCRefChain* expired, const char* type) {
    if (LOCKFREE_REF_CHAIN_POOL) {
        while (true) {
            GCRefChain* freeChain = RTGCGlobal::g_freeRefChain;
            SET_NEXT_FREE(expired, freeChain);
            if (__sync_bool_compare_and_swap(&RTGCGlobal::g_freeRefChain, freeChain, expired)) break;
        }
    }
    else {
        SET_NEXT_FREE(expired, RTGCGlobal::g_freeRefChain);
        RTGCGlobal::g_freeRefChain = expired;
    }
    RTGCGlobal::cntRefChain --;
    if (ENABLE_RTGC_LOG && dump_recycle_log > 0) {//} || node_id % 1000 == 0) {
        RTGC_LOG("RTGC Recycle chain: %d\n", RTGCGlobal::cntRefChain);
    }
}

static int getRefChainIndex(GCRefChain* chain) {
    return chain == NULL ? 0 : chain - GCRefList::g_refChains;
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
    if (prev->obj_ == item) {
        first_ = getRefChainIndex(prev->next_);
        recycleChain(prev, "first");
        return;
    }

    GCRefChain* chain = prev->next_;
    while (chain->obj_ != item) {
        prev = chain;
        chain = chain->next_;
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

void GCNode::initMemory() {
    RTGCGlobal::init();
}

void RTGCGlobal::validateMemPool() {
    // CyclicNode* node = g_freeCyclicNode;
    // for (; node != NULL; node ++) {
    //     if (GET_NEXT_FREE(node) == NULL) break;
    //     assert(GET_NEXT_FREE(node) == node +1);
    // }
    // assert(node == g_cyclicNodes + CNT_CYCLIC_NODE - 1);
}

void RTGCGlobal::init() {
    if (g_cyclicNodes != NULL) return;
    // printf("GCNode initialized");

    g_freeCyclicNode = new CyclicNode[CNT_CYCLIC_NODE];
    g_cyclicNodes = g_freeCyclicNode;
    g_freeRefChain = new GCRefChain[CNT_REF_CHAIN];

    GCRefList::g_refChains = g_freeRefChain - 1;
    RTGC_LOG("g_freeRefChain = %p\n", GCRefList::g_refChains + 1);

    int i = CNT_CYCLIC_NODE-1;
    for (CyclicNode* node = g_freeCyclicNode; --i >= 0;) {
        node = (CyclicNode*)SET_NEXT_FREE(node, node + 1);
    }

    i = CNT_REF_CHAIN-1;
    for (GCRefChain* node = g_freeRefChain; --i >= 0;) {
        node = (GCRefChain*)SET_NEXT_FREE(node, node + 1);
    }

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