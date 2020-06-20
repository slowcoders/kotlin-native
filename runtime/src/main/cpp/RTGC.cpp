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
OnewayNode* GCNode::g_onewayNodes;
CyclicNode* GCNode::g_cyclicNodes;

static OnewayNode* g_freeOnewayNode;
static CyclicNode* g_freeCyclicNode;

static CyclicNode* g_damagedCylicNodes = NULL;
static GCNode* g_cyclicTestNodes = NULL;
int64_t GCNode::g_memberUpdateLock = 0;

struct GCRefChain {
    friend GCRefList;
private:
  GCObject* obj_;
  GCRefChain* next_;
public:
  GCObject* obj() { return obj_; }
  GCRefChain* next() { return next_; }
};

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
    return freeChain;
}

void recycleChain(GCRefChain* expired) {
    SET_NEXT_FREE(expired, g_freeRefChain);
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
        return;
    }

    GCRefChain* chain = prev->next_;
    while (chain->obj_ != item) {
        prev = chain;
        chain = chain->next_;
    }
    prev->next_ = chain->next_;
    recycleChain(chain);
}

void CyclicNode::markDamaged() {

}

int OnewayNode::create() {
    assert(g_memberUpdateLock != 0);
    OnewayNode* node = g_freeOnewayNode;
    int node_id = node - g_onewayNodes;
        //if (__sync_bool_compare_and_swap(pRef, ref, new_ref)) {
    g_freeOnewayNode = (OnewayNode*)GET_NEXT_FREE(node);
    SET_NEXT_FREE(node, NULL);
    return node_id;
}

void CyclicNode::addCyclicTest(GCNode* node) {
    
}

void GCNode::initMemory() {
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

