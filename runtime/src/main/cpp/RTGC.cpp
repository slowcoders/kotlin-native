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

const static int CNT_ONEWAY_NODE = 1000*1000;
const static int CNT_CYCLIC_NODE = 1000*1000;
const static int CNT_REF_CHAIN = 1000*1000;

struct GCRefChain {
    friend GCRefList;
private:
  GCObject* obj_;
  GCRefChain* next_;
public:
  GCObject* obj() { return obj_; }
  GCRefChain* next() { return next_; }
};


OnewayNode* GCNode::g_onewayNodes = NULL;
CyclicNode* GCNode::g_cyclicNodes;

static OnewayNode* g_freeOnewayNode;
static CyclicNode* g_freeCyclicNode;

static CyclicNode* g_damagedCylicNodes = NULL;
static GCNode* g_cyclicTestNodes = NULL;
int64_t GCNode::g_memberUpdateLock = 0;

static GCRefChain* g_refChains;
static GCRefChain* g_freeRefChain;

bool isGlobalLocked() {
    return true;
}

inline void* GET_NEXT_FREE(void* chain) {
    return *(void**)chain;
}

inline void* SET_NEXT_FREE(void* chain, void* next) {
    return (*(void**)chain = next);
}

GCRefChain* popFreeChain() {
    GCRefChain* freeChain = g_freeRefChain;
    assert(isGlobalLocked());
    g_freeRefChain = (GCRefChain*)GET_NEXT_FREE(freeChain);
    int node_id = freeChain - g_refChains;
    if (true || node_id % 1000 == 0) {
       // printf("RTGC Ref chain: %d, %p\n", node_id, g_freeRefChain);
    }
    return freeChain;
}

void recycleChain(GCRefChain* expired, const char* type) {
    SET_NEXT_FREE(expired, g_freeRefChain);
    int node_id = expired - g_refChains;
    if (true || node_id % 1000 == 0) {
       // printf("RTGC Recycle chain: %s %d, %p\n", type, node_id, expired);
    }
    g_freeRefChain = expired;
}

void GCRefList::add(GCObject* item) {
    GCRefChain* chain = popFreeChain();
    chain->obj_ = item;
    chain->next_ = first_;
    first_ = chain;
}

void GCRefList::remove(GCObject* item) {
    GCRefChain* prev = first_;
    if (prev->obj_ == item) {
        first_ = prev->next_;
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

void CyclicNode::markDamaged() {

}

int OnewayNode::create() {
    assert(g_memberUpdateLock != 0);
    OnewayNode* node = g_freeOnewayNode;
        //if (__sync_bool_compare_and_swap(pRef, ref, new_ref)) {
    g_freeOnewayNode = (OnewayNode*)GET_NEXT_FREE(node);
    SET_NEXT_FREE(node, NULL);
    int node_id = node - g_onewayNodes;
    return node_id;
}

void CyclicNode::addCyclicTest(GCNode* node) {
    
}

void GCNode::freeNode(GCObject* last) {

}

void GCNode::onDeallocObject(GCObject* obj, bool isLocked) {
    RTGCRef ref = obj->getRTGCRef();
    if (ref.node == 0) return;
    if (ref.node < CYCLIC_NODE_ID_START) {
        OnewayNode* node = (OnewayNode*)getNode(ref.node);
        if (!isLocked) GCNode::lock(0, 0, 0);
        for (GCRefChain* chain = node->externalReferrers.first(); chain != NULL; chain = chain->next()) {
            recycleChain(chain, "dealloc");
        }
        SET_NEXT_FREE(node, g_freeOnewayNode);
        g_freeOnewayNode = node;
        if (!isLocked) GCNode::unlock(0);
    }
}

void GCNode::initMemory() {
    if (g_onewayNodes != NULL) return;
    // printf("GCNode initialized");

    g_freeRefChain = new GCRefChain[CNT_REF_CHAIN];
    g_freeOnewayNode = new OnewayNode[CNT_ONEWAY_NODE];
    g_freeCyclicNode = new CyclicNode[CNT_CYCLIC_NODE];
    g_onewayNodes = g_freeOnewayNode - 1;
    g_cyclicNodes = g_freeCyclicNode - 1;
    g_refChains = g_freeRefChain;

    int i = CNT_ONEWAY_NODE-1;
    for (OnewayNode* node = g_freeOnewayNode; --i >= 0;) {
        node = (OnewayNode*)SET_NEXT_FREE(node, node + 1);
    }
    
    i = CNT_CYCLIC_NODE-1;
    for (CyclicNode* node = g_freeCyclicNode; --i >= 0;) {
        node = (CyclicNode*)SET_NEXT_FREE(node, node + 1);
    }

    i = CNT_REF_CHAIN-1;
    for (GCRefChain* node = g_freeRefChain; --i >= 0;) {
        node = (GCRefChain*)SET_NEXT_FREE(node, node + 1);
    }
}

