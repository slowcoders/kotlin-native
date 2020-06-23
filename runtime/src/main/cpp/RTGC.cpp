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

int RTGCGlobal::cntRefChain = 0;
int RTGCGlobal::cntCyclicNodes = 0;

CyclicNode* RTGCGlobal::g_freeCyclicNode;


GCRefChain* GCRefList::g_refChains;
GCRefChain* RTGCGlobal::g_freeRefChain;

CyclicNode* GCNode::g_cyclicNodes = NULL;
int64_t GCNode::g_memberUpdateLock = 0;


GCRefChain* popFreeChain() {
    GCRefChain* freeChain = RTGCGlobal::g_freeRefChain;
    assert(GCNode::isLocked(0));
    RTGCGlobal::g_freeRefChain = (GCRefChain*)GET_NEXT_FREE(freeChain);
    RTGCGlobal::cntRefChain ++;
    int node_id = freeChain - GCRefList::g_refChains;
    if (true || node_id % 1000 == 0) {
       // printf("RTGC Ref chain: %d, %p\n", node_id, g_freeRefChain);
    }
    return freeChain;
}

void recycleChain(GCRefChain* expired, const char* type) {
    SET_NEXT_FREE(expired, RTGCGlobal::g_freeRefChain);
    RTGCGlobal::cntRefChain --;
    int node_id = expired - GCRefList::g_refChains;
    if (true || node_id % 1000 == 0) {
       // printf("RTGC Recycle chain: %s %d, %p\n", type, node_id, expired);
    }
    RTGCGlobal::g_freeRefChain = expired;
}

void GCRefList::add(GCObject* item) {
    GCRefChain* chain = popFreeChain();
    chain->obj_ = item;
    chain->next_ = this->first();
    first_ = chain - g_refChains;
}


void GCRefList::remove(GCObject* item) {
    GCRefChain* prev = first();
    if (prev->obj_ == item) {
        first_ = prev->next_ - g_refChains;
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
    GCRefChain* prev = first();
    if (prev->obj_ == item) {
        first_ = prev->next_ - g_refChains;
        // move to receiver;
        prev->next_ = receiver->first();
        receiver->first_ = prev - g_refChains;
        return;
    }

    GCRefChain* chain = prev->next_;
    while (chain->obj_ != item) {
        prev = chain;
        chain = chain->next_;
    }
    prev->next_ = chain->next_;
    // move to receiver;
    chain->next_ = receiver->first();
    receiver->first_ = chain - g_refChains;
}


bool GCRefList::tryRemove(GCObject* item) {
    GCRefChain* prev = NULL;
    for (GCRefChain* chain = first(); chain != NULL; chain = chain->next()) {
        if (chain->obj_ == item) {
            if (prev == NULL) {
                first_ = chain->next_ - g_refChains;;
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
    for (GCRefChain* chain = first(); chain != NULL; chain = chain->next_) {
        if (chain->obj_ == item) {
            return chain;
        }
    }
    return NULL;
}

GCRefChain* GCRefList::find(int node_id) {    
    for (GCRefChain* chain = first(); chain != NULL; chain = chain->next_) {
        if (chain->obj_->getNodeId() == node_id) {
            return chain;
        }
    }
    return NULL;
}

void GCRefList::setFirst(GCRefChain* newFirst) {
    for (GCRefChain* chain = first(); chain != newFirst; ) {
        GCRefChain* next = chain->next_;
        recycleChain(chain, "setLast");
        chain = next;
    }
    this->first_ = newFirst - g_refChains;
}


CyclicNode* CyclicNode::create() {
    assert(isLocked(0));
    CyclicNode* node = RTGCGlobal::g_freeCyclicNode;
        //if (__sync_bool_compare_and_swap(pRef, ref, new_ref)) {
    RTGCGlobal::g_freeCyclicNode = (CyclicNode*)GET_NEXT_FREE(node);
    memset(node, 0, sizeof(CyclicNode));
    RTGCGlobal::cntCyclicNodes ++;
    return node;
}

void GCNode::dealloc(int nodeId, bool isLocked) {
    CyclicNode* node = CyclicNode::getNode(nodeId);
    if (node == NULL) return;

    if (!isLocked) GCNode::rtgcLock(0, 0, 0);
    node->externalReferrers.clear();
    SET_NEXT_FREE(node, RTGCGlobal::g_freeCyclicNode);
    RTGCGlobal::g_freeCyclicNode = (CyclicNode*)node;
    RTGCGlobal::cntCyclicNodes --;
    if (!isLocked) GCNode::rtgcUnlock(0);
}

extern "C" {

void Kotlin_native_internal_GC_rtgcLog(KRef __unused) {
    printf("cntRefChain %d\n", RTGCGlobal::cntRefChain);
    printf("cntCyclicNodes %d\n", RTGCGlobal::cntCyclicNodes);
}

KInt Kotlin_native_internal_GC_refCount(KRef __unused, KRef obj) {
    if (obj == NULL) return -1;
    return obj->container()->refCount();
}
};

void GCNode::initMemory() {
    RTGCGlobal::init();
}

void RTGCGlobal::init() {
    if (g_cyclicNodes != NULL) return;
    // printf("GCNode initialized");

    g_freeCyclicNode = new CyclicNode[CNT_CYCLIC_NODE];
    g_cyclicNodes = g_freeCyclicNode - 1;
    GCRefList::g_refChains = g_freeRefChain = new GCRefChain[CNT_REF_CHAIN];

    int i = CNT_CYCLIC_NODE-1;
    for (CyclicNode* node = g_freeCyclicNode; --i >= 0;) {
        node = (CyclicNode*)SET_NEXT_FREE(node, node + 1);
    }

    i = CNT_REF_CHAIN-1;
    for (GCRefChain* node = g_freeRefChain; --i >= 0;) {
        node = (GCRefChain*)SET_NEXT_FREE(node, node + 1);
    }
}

