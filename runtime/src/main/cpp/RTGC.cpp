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

THREAD_LOCAL_VARIABLE RTGCMemState* rtgcMem;

int RTGCGlobal::g_cntAddRefChain = 0;
int RTGCGlobal::g_cntRemoveRefChain = 0;
int RTGCGlobal::g_cntAddCyclicNode = 0;
int RTGCGlobal::g_cntRemoveCyclicNode = 0;
void* RTGC_debugInstance = NULL;

CyclicNode lastDummy;

RefBucket g_refBucket;
CyclicBucket g_cyclicBucket;
// CyclicNode* RTGCGlobal::g_freeCyclicNode;
// GCRefChain* GCRefList::g_refChains;
// GCRefChain* RTGCGlobal::g_freeRefChain;
// CyclicNode* GCNode::g_cyclicNodes = NULL;

int RTGCGlobal::g_cntAddCyclicTest = 0;
int RTGCGlobal::g_cntRemoveCyclicTest = 0;

static pthread_t g_lockThread = NULL;
static int g_cntLock = 0;
THREAD_LOCAL_VARIABLE int32_t isHeapLocked = 0;
static const bool RECURSIVE_LOCK = true;
static const bool SKIP_REMOVE_ERROR = true;
int g_memDebug = false;
int g_cntRTGCLocks[16];

void GCNode::rtgcLock(LockType type) {
    if (RECURSIVE_LOCK) {
        pthread_t curr_thread = pthread_self();
        if (curr_thread != g_lockThread) {
            while (!__sync_bool_compare_and_swap(&g_lockThread, NULL, curr_thread)) {}
        }
    }
    g_cntLock ++;
    if (RTGC_STATISTCS) {
        g_cntRTGCLocks[type] ++;
    }
    if (DEBUG_RTGC_BUCKET && (g_memDebug || g_lockThread != pthread_self())) {
        BUCKET_LOG("g_lockThread =%p(%p) ++%d\n", g_lockThread, pthread_self(), g_cntLock)
    }
}

void GCNode::rtgcUnlock() {
    if (RECURSIVE_LOCK) {
        //RuntimeAssert(pthread_self() == g_lockThread, "unlock in wrong thread");
    }
    if (DEBUG_RTGC_BUCKET && (g_memDebug || g_lockThread != pthread_self())) {
        BUCKET_LOG("g_lockThread =%p(%p) %d--\n", g_lockThread, pthread_self(), g_cntLock)
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
    if (RTGC_STATISTCS) RTGCGlobal::g_cntAddRefChain ++;
    return rtgcMem->refChainAllocator.allocItem();
}

static void recycleChain(GCRefChain* expired, const char* type) {
    if (RTGC_STATISTCS) RTGCGlobal::g_cntRemoveRefChain ++;
    rtgcMem->refChainAllocator.recycleItem(expired);
}

static int getRefChainIndex(GCRefChain* chain) {
    return chain == NULL ? 0 : rtgcMem->refChainAllocator.getItemIndex(chain);
}

GCRefChain* GCRefList::topChain() { 
    return first_ == 0 ? NULL : rtgcMem->refChainAllocator.getItem(first_); 
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

void GCRefList::tryRemove(GCObject* item, bool isUnique) {
    GCRefChain* prev = NULL;
    GCRefChain* next;
    for (GCRefChain* chain = topChain(); chain != NULL; chain = next) {
        next = chain->next();
        if (chain->obj_ != item) {
            prev = chain;
            continue;
        }

        if (prev == NULL) {
            first_ = getRefChainIndex(chain->next_);
        }
        else {
            prev->next_ = chain->next_;
        }
        recycleChain(chain, "first");
        if (isUnique) {
            break;
        }
    }
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
    // RuntimeAssert(isLocked(), "GCNode is not locked")
    if (ENABLE_RTGC_LOG && dump_recycle_log > 0) {//} || node_id % 1000 == 0) {
         RTGC_LOG("OnewayNode::dealloc, top %p\n", externalReferrers.topChain());
    }
    externalReferrers.clear();
}

void GCNode::dumpGCLog() {
    if (!RTGC_STATISTCS) return;
    printf("** RTGCLock FreeContainer %d\n", g_cntRTGCLocks[_FreeContainer]);
    printf("** RTGCLock IncrementRC %d\n", g_cntRTGCLocks[_IncrementRC]);
    printf("** RTGCLock IncrementAcyclicRC %d\n", g_cntRTGCLocks[_IncrementAcyclicRC]);
    printf("** RTGCLock DecrementRC %d\n", g_cntRTGCLocks[_DecrementRC]);
    printf("** RTGCLock DecrementAcyclicRC %d\n", g_cntRTGCLocks[_DecrementAcyclicRC]);
    printf("** RTGCLock AssignRef %d\n", g_cntRTGCLocks[_AssignRef]);
    printf("** RTGCLock DeassignRef %d\n", g_cntRTGCLocks[_DeassignRef]);
    printf("** RTGCLock UpdateHeapRef %d\n", g_cntRTGCLocks[_UpdateHeapRef]);
    printf("** RTGCLock PopBucket %d\n", g_cntRTGCLocks[_PopBucket]);
    printf("** RTGCLock RecycleBucket %d\n", g_cntRTGCLocks[_RecycleBucket]);

    printf("** cntRefChain %d = %d - %d\n", RTGCGlobal::g_cntAddRefChain - RTGCGlobal::g_cntRemoveRefChain,
        RTGCGlobal::g_cntAddRefChain, RTGCGlobal::g_cntRemoveRefChain);
    printf("** cntCyclicNode %d = %d - %d\n", RTGCGlobal::g_cntAddCyclicNode - RTGCGlobal::g_cntRemoveCyclicNode,
        RTGCGlobal::g_cntAddCyclicNode, RTGCGlobal::g_cntRemoveCyclicNode);
    printf("** cntCyclicTest %d = %d - %d\n", RTGCGlobal::g_cntAddCyclicTest - RTGCGlobal::g_cntRemoveCyclicTest,
        RTGCGlobal::g_cntAddCyclicTest, RTGCGlobal::g_cntRemoveCyclicTest);

    RTGCGlobal::g_cntAddRefChain = RTGCGlobal::g_cntRemoveRefChain = 0;
    RTGCGlobal::g_cntAddCyclicNode = RTGCGlobal::g_cntRemoveCyclicNode = 0;
    RTGCGlobal::g_cntAddCyclicTest = RTGCGlobal::g_cntRemoveCyclicTest = 0;
    
    g_cntRTGCLocks[_FreeContainer] = 0;
    g_cntRTGCLocks[_IncrementRC] = 0;
    g_cntRTGCLocks[_DecrementRC] = 0;
    g_cntRTGCLocks[_AssignRef] = 0;
    g_cntRTGCLocks[_DeassignRef] = 0;
    g_cntRTGCLocks[_UpdateHeapRef] = 0;
    g_cntRTGCLocks[_PopBucket] = 0;
    g_cntRTGCLocks[_RecycleBucket] = 0;
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
    return rtgcMem->cyclicNodeAllocator.getItemIndex(this) + CYCLIC_NODE_ID_START; 
}

CyclicNode* CyclicNode::getNode(int nodeId) {
    if (nodeId < CYCLIC_NODE_ID_START) {
        return NULL;
    }
    return rtgcMem->cyclicNodeAllocator.getItem(nodeId - CYCLIC_NODE_ID_START);
}

static int cntMemory = 0; 

void GCNode::initMemory(RTGCMemState* memState) {
    cntMemory ++;
    g_memDebug = cntMemory > 1;
    RTGC_LOG("initMemory: %d", cntMemory);
    memState->refChainAllocator.init(&g_refBucket, cntMemory);
    memState->cyclicNodeAllocator.init(&g_cyclicBucket, cntMemory + 1000);
    rtgcMem = memState;
    if (cntMemory < 0) {
        // To make sure RTGC_dumpRefInfo is included in executable binary;
        RTGC_dumpRefInfo(NULL);
        RTGC_dumpRefInfo0(NULL);
    }
}

void RTGCGlobal::validateMemPool() {
    // CyclicNode* node = g_freeCyclicNode;
    // for (; node != NULL; node ++) {
    //     if (GET_NEXT_FREE(node) == NULL) break;
    //     assert(GET_NEXT_FREE(node) == node +1);
    // }
    // assert(node == g_cyclicNodes + CNT_CYCLIC_NODE - 1);
}


bool enable_rtgc_trap = ENABLE_RTGC_LOG;
bool rtgc_trap(void* pObj) {
    return enable_rtgc_trap;
}

void RTGC_Error(GCObject* obj) {
    if (obj != NULL) {
        RTGC_dumpRefInfo(obj);
    }
    ThrowOutOfMemoryError();
}

void RTGC_dumpRefInfo0(GCObject* obj) {
    RTGC_dumpTypeInfo("*", NULL, obj);
}

void RTGC_dumpRefInfo(GCObject* obj) {
    const TypeInfo* typeInfo = ((ObjHeader*)(obj+1))->type_info();
    RTGC_dumpTypeInfo("*", typeInfo, obj);
}


void RTGC_dumpTypeInfo(const char* msg, const TypeInfo* typeInfo, GCObject* obj) {
    static const char* UNKNOWN = "???";
    const char* classname = (typeInfo != NULL && typeInfo->relativeName_ != NULL)
        ? CreateCStringFromString(typeInfo->relativeName_) : UNKNOWN;
    if (obj == NULL) {
        konan::consolePrintf("%s %s %p \n", msg, classname, obj);
    }
    else {
        konan::consolePrintf("%s %s %p:%d rc=%p, flags=%x\n", 
            msg, classname, obj, obj->getNodeId(), 
            (void*)obj->refCount(), obj->getFlags());
    }
    if (classname != UNKNOWN) konan::free((void*)classname);
    rtgc_trap(NULL);
}