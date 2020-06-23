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

CyclicNode lastDummy;

CyclicNode* CyclicNode::g_damagedCylicNodes = &lastDummy;
GCRefList CyclicNode::g_cyclicTestNodes;

static const int ENABLE_RTGC_LOG = 1;
#define RTGC_LOG if (ENABLE_RTGC_LOG) printf

void CyclicNode::markDamaged() {
    assert(isLocked(0));

    if (!this->isDamaged()) {
        this->nextDamaged = g_damagedCylicNodes;
        g_damagedCylicNodes = this;
    }
}

void CyclicNode::addCyclicTest(GCObject* obj) {
    assert(isLocked(0));
    obj->getNode()->markSuspectedCyclic();
    g_cyclicTestNodes.add(obj);
}

void CyclicNode::mergeCyclicNode(GCObject* obj, int expiredNodeId) {
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
    rookie->setNodeId(this_id);
    assert(rookie->isInCyclicNode() && rookie->getNode() == this);
    
    if (rookie->isInCyclicNode()) {
        CyclicNode* oldCyclicNode = (CyclicNode*)oldNode;
        printf("## RTGC Merge Cycle %d %d\n", this->getRootObjectCount(), oldCyclicNode->getRootObjectCount());
        this->rootObjectCount += oldCyclicNode->rootObjectCount;
        mergeCyclicNode(rookie, oldCyclicNode->getId());
    }
    else {
        if (rookie->getRootRefCount() > 0) {
            this->rootObjectCount ++;
        }
    }

    for(GCRefChain* chain = oldNode->externalReferrers.first(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        if (referrer->getNode() != this) {
            this->externalReferrers.add(referrer);
        }
    }

    this->externalReferrers.tryRemove(rookie);

}


void CyclicNode::detectCyclicNodes(GCObject* tracingObj, GCRefList* tracingList, GCRefList* finishedList) {
    GCNode* current_node = tracingObj->getNode();   
    int last_node_id =  tracingObj->getNodeId();
    current_node->setTraceState(IN_TRACING);
    current_node->clearSuspectedCyclic();

    tracingList->add(tracingObj);
    RTGC_LOG("## RTGC tracingList add: %p=%p\n", tracingList->first(), tracingObj);
    
    for (GCRefChain* chain = current_node->externalReferrers.first(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        GCNode* referrer_node = referrer->getNode();
        switch (referrer_node->getTraceState()) {
        case NOT_TRACED: // 추적되지 않은 참조 노드
        {
            detectCyclicNodes(referrer, tracingList, finishedList);
            // current_node 재설정 후 검사.
            if (tracingObj->getNodeId() != referrer->getNodeId()) {
                // tracingObj와 referent 를 경유하는 순환 경로가 발견되지 않은 경우
                referrer->getNode()->setTraceState(TRACE_FINISHED);
                RTGC_LOG("## RTGC tracingList remove: %p, %p\n", referrer, tracingObj);
                tracingList->moveTo(referrer, finishedList);
            }
            break;
        }
        case IN_TRACING: // 순환 경로 발견
        {
            GCNode* ref_node = referrer->getNode();
            if (ref_node == tracingObj->getNode()) {
                break;
            }
            CyclicNode* cyclicNode = CyclicNode::create();
            GCRefChain* chain = tracingList->first();
            int cnt = 1;
            while (chain->obj()->getNode() != ref_node) {
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
            break;
        case OUT_OF_SCOPE: 
            break;
        }
    }
    if (last_node_id != tracingObj->getNodeId()) {
        GCNode::dealloc(last_node_id, true);
    }
}


void CyclicNode::detectCycles() {
    rtgcLock(0, 0, 0);
    GCRefList tracingList;
    GCRefList finishedList;
    for (GCRefChain* chain = g_cyclicTestNodes.first(); chain != NULL; chain = chain->next()) {
        GCObject* obj = chain->obj();
        if (!obj->getNode()->isSuspectedCyclic()) {
            continue;
        }
        detectCyclicNodes(obj, &tracingList, &finishedList);
        tracingList.clear();
        for (GCRefChain* chain = finishedList.first(); chain != NULL; chain = chain->next()) {
            chain->obj()->getNode()->setTraceState(NOT_TRACED);
        }
        obj->getNode()->setTraceState(NOT_TRACED);
        finishedList.clear();

        CyclicNode* cyclic = obj->getLocalCyclicNode();
        if (cyclic != NULL) {
            printf("## RTGC Cycle detected %d %d\n", cyclic->getRootObjectCount(), cyclic->externalReferrers.first() == 0 ? 0 : 1);
            if (cyclic->isGarbage()) {
                printf("## RTGC Garbage Cycle detected\n");
                ::freeContainer(obj, cyclic->getId());
                //cyclic->freeNode(obj);
            }
            else {
            }
        }
    }
    rtgcUnlock(0);
}

