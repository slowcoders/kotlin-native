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
    RTGC_traverseObjectFields(obj, [this, expiredNodeId](GCObject* referent) {
        if (referent->getNodeId() == expiredNodeId) {
            this->mergeCyclicNode(referent, expiredNodeId);
        }
    });
}

void CyclicNode::addCyclicObject(
    GCObject* rookie /* 추가 객체*/ 
) {
    GCNode* oldNode = GCNode::getNode(rookie->getRTGCRef());
    if (oldNode == this) return;
    
    /* 객체의 소속 참조 노드 변경 */
    int this_id = this->getId();
    rookie->setNodeId(this_id);
    
    if (RTGCGlobal::isInCyclicNode(rookie)) {
        CyclicNode* oldCyclicNode = (CyclicNode*)oldNode;
        // 순환 참조 노드의 기반 객체수(431) 변경.
        this->rootObjectCount += oldCyclicNode->rootObjectCount;
        /* 추가 객체 및 해당 객체와 동일한 순환 참조 노드에 속한 객체들의 소속 참조구간을 
         * 새로운 새로운 순환 참조 노드로 변경한다. */
        mergeCyclicNode(rookie, oldCyclicNode->getId());
    }
    else {
        /* 순환 참조 노드의 기반 객체수(431) 변경. */
        if (rookie->getRootRefCount() > 0) {
            this->rootObjectCount ++;
        }
    }

    /* 순환 참조 노드의 외부 참조자 목록(411)을 변경한다. */
    for(GCRefChain* chain = oldNode->externalReferrers.first(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        if (referrer->getNodeId() != this_id) {
            this->externalReferrers.add(rookie);
        }
    }
    this->externalReferrers.tryRemove(rookie);

}


void CyclicNode::detectCyclicNodes(GCObject* tracingObj, GCRefList* tracingList, GCRefList* finishedList) {
    GCNode* current_node = GCNode::getNode(tracingObj->getRTGCRef());   
    int current_node_id =  tracingObj->getNodeId();
    current_node->traceState = IN_TRACING;
    current_node->clearSuspectedCyclic();

    tracingList->add(tracingObj);
    printf("## RTGC tracingList add: %p=%p\n", tracingList->first(), tracingObj);
    
    for (GCRefChain* chain = current_node->externalReferrers.first(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        GCNode* referrer_node = GCNode::getNode(referrer->getRTGCRef());
        switch (referrer_node->traceState) {
        case NOT_TRACED: // 추적되지 않은 참조 노드
        {
            /* 외부참조자에 대한 역방향 추적을 재귀적으로 수행한다. */
            detectCyclicNodes(referrer, tracingList, finishedList);
            // current_node 재설정 후 검사.
            if (tracingObj->getNodeId() != referrer->getNodeId()) {
                // tracingObj와 referent 를 경유하는 순환 경로가 발견되지 않은 경우
                referrer->getNode()->traceState = TRACE_FINISHED;
                printf("## RTGC tracingList remove: %p, %p\n", referrer, tracingObj);
                //tracingList->remove(referrer);
                tracingList->moveTo(referrer, finishedList);
            }
            break;
        }
        case IN_TRACING: // 순환 경로 발견
        {
            /* 추적 객체와 referent 객체가 속한 참조 노드를 경유하는 순환 경로가 발견되었다.
             * 추적경로에 포함된 참조 노드들을 하나의 순환 참조 노드로 병합한다. */
            int ref_node_id = referrer->getNodeId();
            if (ref_node_id == current_node_id) {
                break;
            }
            CyclicNode* cyclicNode = CyclicNode::create();
            GCRefChain* chain = tracingList->first();
            int cnt = 1;
            while (chain->obj()->getNodeId() != ref_node_id) {
                printf("## RTGC Cyclic add: %p:%d, %p\n", chain, cnt, chain->obj());
                cnt ++;
                cyclicNode->addCyclicObject(chain->obj());
                chain = chain->next();
            }
            printf("## RTGC Cyclic add: %p;%d, %p\n", chain, cnt, referrer);
            cyclicNode->addCyclicObject(referrer);
            cyclicNode->traceState = IN_TRACING;
            tracingList->setFirst(chain);
            current_node_id = cyclicNode->getId();
            printf("## RTGC Cyclic found %d\n", cnt);
            break;
        }    
        case TRACE_FINISHED: // 이미 추적된 참조 노드
            break;
        case OUT_OF_SCOPE: 
            break;
        }
    }
    if (current_node_id != tracingObj->getNodeId()) {
        //GCNode::dealloc(current_node_id, true);
    }
}


void CyclicNode::detectCycles() {
    lock(0, 0, 0);
    GCRefList tracingList;
    GCRefList finishedList;
    printf("## RTGC detect cycles : %p\n", tracingList.first());
    for (GCRefChain* chain = g_cyclicTestNodes.first(); chain != NULL; chain = chain->next()) {
        GCObject* obj = chain->obj();
        if (!obj->getNode()->isSuspectedCyclic()) {
            continue;
        }
        detectCyclicNodes(obj, &tracingList, &finishedList);
        tracingList.clear();
        for (GCRefChain* chain = finishedList.first(); chain != NULL; chain = chain->next()) {
            chain->obj()->getNode()->traceState = NOT_TRACED;
        }
        obj->getNode()->traceState = NOT_TRACED;
        finishedList.clear();

        if (RTGCGlobal::isInCyclicNode(obj)) {
            CyclicNode* cyclic = GCNode::getCyclicNode(obj->getRTGCRef());
            if (cyclic->isGarbage()) {
                printf("## RTGC Garbage Cycle detected");
                ::freeContainer(obj, cyclic->getId());
                //cyclic->freeNode(obj);
            }
            else {
                printf("## RTGC Cycle detected");
            }
        }
    }
    unlock(0);
}

// static void freeObjectsInNode(GCObject* obj, int nodeId) {
//     obj->setNodeId(0);
//     RTGC_traverseObjectFields(obj, [obj, nodeId](GCObject* referent) {
//         if (referent->getNodeId() == nodeId) {
//             freeObjectsInNode(referent, nodeId);
//         }
//         else if (referent->getNodeId() != 0) {
//             updateHeapRef_internal(NULL, (ObjHeader*)(referent + 1), (ObjHeader*)(obj + 1));
//         }
//     });
//     freeContainer(obj);
// }

void CyclicNode::freeNode(GCObject* last) {
    //freeObjectsInNode(last, this->getId());
}
