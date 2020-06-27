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
#include "assert.h"

CyclicNode lastDummy;

CyclicNode* CyclicNode::g_damagedCylicNodes = &lastDummy;
GCRefList CyclicNode::g_cyclicTestNodes;


CyclicNode* CyclicNode::create() {
    assert(isLocked(0));
    CyclicNode* node = RTGCGlobal::g_freeCyclicNode;
        //if (__sync_bool_compare_and_swap(pRef, ref, new_ref)) {
    RTGCGlobal::g_freeCyclicNode = (CyclicNode*)GET_NEXT_FREE(node);
    assert(RTGCGlobal::g_freeCyclicNode != node);
    assert(RTGCGlobal::g_freeCyclicNode >= g_cyclicNodes && RTGCGlobal::g_freeCyclicNode < g_cyclicNodes + CNT_CYCLIC_NODE);
    memset(node, 0, sizeof(CyclicNode));
    RTGCGlobal::cntCyclicNodes ++;
    return node;
}

void OnewayNode::dealloc(bool isLocked) {
    if (!isLocked) GCNode::rtgcLock(0, 0, 0);
    externalReferrers.clear();
    if (!isLocked) GCNode::rtgcUnlock(0);
}

void CyclicNode::dealloc(bool isLocked) {
    RTGC_LOG("## RTGC deallic node:%d\n", this->getId());
    if (!isLocked) GCNode::rtgcLock(0, 0, 0);
    externalReferrers.clear();
    SET_NEXT_FREE(this, RTGCGlobal::g_freeCyclicNode);
    RTGCGlobal::g_freeCyclicNode = (CyclicNode*)this;
    RTGCGlobal::cntCyclicNodes --;
    if (!isLocked) GCNode::rtgcUnlock(0);
}

void CyclicNode::markDamaged() {
    assert(isLocked(0));

    if (!this->isDamaged()) {
        this->nextDamaged = g_damagedCylicNodes;
        g_damagedCylicNodes = this;
    }
}

void CyclicNode::addCyclicTest(GCObject* obj) {
    assert(isLocked(0));
    obj->markNeedCyclicTest();
    obj->getNode()->markSuspectedCyclic();
    g_cyclicTestNodes.push(obj);
}

void CyclicNode::removeCyclicTest(GCObject* obj, bool isLocked) {
    if (!obj->isNeedCyclicTest()) return;
    obj->clearNeedCyclicTest();

    RTGC_LOG("## RTGC Remove Cyclic Test %p:%d\n", obj, obj->getNodeId());
    if (!isLocked) rtgcLock(0, 0, 0);
    g_cyclicTestNodes.remove(obj);
    if (!isLocked) rtgcUnlock(0);
}

void CyclicNode::mergeCyclicNode(GCObject* obj, int expiredNodeId) {
        RTGC_LOG("## RTGC Replace Cycle %p:%d -> %d\n", obj, expiredNodeId, obj->getNodeId());
    obj->setNodeId(this->getId());
    assert(obj->isInCyclicNode() && obj->getNode() == this);
    RTGC_traverseObjectFields(obj, [this, expiredNodeId](GCObject* referent) {
        if (referent->getNodeId() == expiredNodeId) {
            this->mergeCyclicNode(referent, expiredNodeId);
        }
    });
}

void CyclicNode::addCyclicObject(
    GCObject* rookie /* 추가 객체*/ 
) {
    GCNode* oldNode = rookie->getNode();
    if (oldNode == this) return;
    
    /* 객체의 소속 참조 노드 변경 */
    int this_id = this->getId();
    bool rookieInCyclic = rookie->isInCyclicNode();
    rookie->setNodeId(this_id);
    assert(rookie->isInCyclicNode() && rookie->getNode() == this);
    
    if (rookieInCyclic) {
        CyclicNode* oldCyclicNode = (CyclicNode*)oldNode;
        RTGC_LOG("## RTGC Merge Cycle %d %d\n", this->getRootObjectCount(), oldCyclicNode->getRootObjectCount());
        this->rootObjectCount += oldCyclicNode->rootObjectCount;
        mergeCyclicNode(rookie, oldCyclicNode->getId());
    }
    else {
        if (rookie->getRootRefCount() > 0) {
            this->rootObjectCount ++;
        }
    }

    for(GCRefChain* chain = oldNode->externalReferrers.topChain(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        if (referrer->getNode() != this) {
            RTGC_LOG("## RTGC add referrer: %p=%p\n", chain, referrer);
            this->externalReferrers.push(referrer);
        }
    }

    this->externalReferrers.tryRemove(rookie);

}


void CyclicNode::detectCyclicNodes(GCObject* tracingObj, GCRefList* tracingList, GCRefList* finishedList) {
    GCNode* current_node = tracingObj->getNode();   
    int last_node_id =  tracingObj->getNodeId();
    current_node->setTraceState(IN_TRACING);
    removeCyclicTest(tracingObj, true);
    current_node->clearSuspectedCyclic();

    tracingList->push(tracingObj);
    RTGC_LOG("## RTGC tracingList add: %p=%p\n", tracingList->topChain(), tracingObj);
    
    for (GCRefChain* chain = current_node->externalReferrers.topChain(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        GCNode* referrer_node = referrer->getNode();
        if (ENABLE_RTGC_LOG) RTGCGlobal::validateMemPool();
        switch (referrer_node->getTraceState()) {
        case NOT_TRACED: // 추적되지 않은 참조 노드
        {
            RTGC_LOG("## RTGC Cyclic NOT TRACED");
            detectCyclicNodes(referrer, tracingList, finishedList);
            // 노드 변경 상황 반영.
            referrer_node = referrer->getNode();
            if (tracingObj->getNode() != referrer_node) {
                // tracingObj와 referent 를 경유하는 순환 경로가 발견되지 않은 경우
                referrer_node->setTraceState(TRACE_FINISHED);
                RTGC_LOG("## RTGC tracingList remove: %p, %p\n", referrer, tracingObj);
                tracingList->moveTo(referrer, finishedList);
            }
            break;
        }
        case IN_TRACING: // 순환 경로 발견
        {
            RTGC_LOG("## RTGC Cyclic Found %p-%d", referrer, referrer->getNodeId());
            if (referrer_node == tracingObj->getNode()) {
                RTGC_LOG("## RTGC Cyclic inside: %p:%p, %p:%p\n", tracingObj, referrer->getNode(), referrer, referrer->getNode());
                break;
            }
            CyclicNode* cyclicNode = referrer->isInCyclicNode() ? (CyclicNode*)referrer_node : CyclicNode::create();
            GCRefChain* chain = tracingList->topChain();
            int cnt = 1;
            while (chain->obj()->getNode() != referrer_node) {
                RTGC_LOG("## RTGC Cyclic add: %d:%d, %p\n", cyclicNode->getId(), cnt, chain->obj());
                cnt ++;
                cyclicNode->addCyclicObject(chain->obj());
                chain = chain->next();
            }
            RTGC_LOG("## RTGC Cyclic add last: %d:%d, %p\n", cyclicNode->getId(), cnt, referrer);
            cyclicNode->addCyclicObject(referrer);
            cyclicNode->setTraceState(IN_TRACING);
            tracingList->setFirst(chain);
            break;
        }    
        case TRACE_FINISHED: // 이미 추적된 참조 노드
            RTGC_LOG("## RTGC Finished");
            break;
        default: 
            RTGC_LOG("## RTGC OOS");
            break;
        }
    }
    if (last_node_id != tracingObj->getNodeId()) {
        if (last_node_id < CYCLIC_NODE_ID_START) {
            ((OnewayNode*)current_node)->dealloc(true);
        }
        else {
            ((CyclicNode*)current_node)->dealloc(true);
        }
    }
}


void CyclicNode::detectCycles() {
    rtgcLock(0, 0, 0);
    GCRefList tracingList;
    GCRefList finishedList;
    for (GCObject* root; (root = g_cyclicTestNodes.pop()) != NULL;) {
        RTGC_LOG("## RTGC c root: %p/n", root);
        assert(root->getNodeId() != 0);
        if (!root->isNeedCyclicTest()) {

            continue;
        }
        if (!root->getNode()->isSuspectedCyclic()) {
            root->clearNeedCyclicTest();
            continue;
        }
        root->clearNeedCyclicTest();
        detectCyclicNodes(root, &tracingList, &finishedList);
        assert(tracingList.topChain()->obj() == root);
        assert(tracingList.topChain()->next() == NULL);
        tracingList.clear();
        for (GCRefChain* chain = finishedList.topChain(); chain != NULL; chain = chain->next()) {
            chain->obj()->getNode()->setTraceState(NOT_TRACED);
        }
            RTGC_LOG("## RTGC 2");
        root->getNode()->setTraceState(NOT_TRACED);
        finishedList.clear();

        CyclicNode* cyclic = root->getLocalCyclicNode();
        if (cyclic != NULL) {
            if (cyclic->isGarbage()) {
                RTGC_LOG("## RTGC Garbage Cycle detected %d rrc:%d\n", cyclic->getId(), cyclic->getRootObjectCount());
                ::freeContainer(root, cyclic->getId());
                cyclic->dealloc(root);
            }
            else {
                RTGC_LOG("## RTGC Cycle detected:%d rrc:%d %d\n", cyclic->getId(), cyclic->getRootObjectCount(), cyclic->externalReferrers.topChain() == 0 ? 0 : 1);
            }
        }
    }
    rtgcUnlock(0);

    RTGC_LOG("RefChain--: %d\n", RTGCGlobal::cntRefChain);
}

