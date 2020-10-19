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


extern THREAD_LOCAL_VARIABLE RTGCMemState* rtgcMem;


CyclicNode* CyclicNode::create() {
    assert(isLocked());
    CyclicNode* node = rtgcMem->cyclicNodeAllocator.allocItem();
    memset(node, 0, sizeof(CyclicNode));
    if (RTGC_STATISTCS) RTGCGlobal::g_cntAddCyclicNode ++;
    return node;
}

void CyclicNode::dealloc() {
    RTGC_LOG("## RTGC deallic node:%d\n", this->getId());
    //RuntimeAssert(isLocked(), "GCNode is not locked")
    externalReferrers.clear();
    rtgcMem->cyclicNodeAllocator.recycleItem(this);
    if (RTGC_STATISTCS) RTGCGlobal::g_cntRemoveCyclicNode ++;
}

void CyclicNode::markDamaged() {
    assert(isLocked());

    if (!this->isDamaged()) {
        this->nextDamaged = rtgcMem->g_damagedCylicNodes;
        rtgcMem->g_damagedCylicNodes = this;
    }
}

void CyclicNode::addCyclicTest(GCObject* obj, bool isLocalTest) {
    //assert(isLocked());
    RTGC_LOG("addCyclicTest %p\n", obj);
    obj->markNeedCyclicTest();
    obj->getNode()->markSuspectedCyclic();
    if (RTGC_STATISTCS) RTGCGlobal::g_cntAddCyclicTest ++;

    rtgcMem->g_cyclicTestNodes.push(obj);
}

void CyclicNode::removeCyclicTest(GCObject* obj) {
    //RuntimeAssert(isLocked(), "GCNode is not locked")
    if (!obj->isNeedCyclicTest()) return;
    obj->clearNeedCyclicTest();
    if (RTGC_STATISTCS) RTGCGlobal::g_cntRemoveCyclicTest ++;

    RTGC_LOG("## RTGC Remove Cyclic Test %p:%d\n", obj, obj->getNodeId());
    /* TODO ZZZZ replace tryRemove => remove */
    rtgcMem->g_cyclicTestNodes.tryRemove(obj);
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
            RTGC_LOG("## RTGC add referrer of cyclic: %p=%p\n", chain, referrer);
            this->externalReferrers.push(referrer);
        }
    }

    this->externalReferrers.tryRemove(rookie);
    if (rookieInCyclic) {
        CyclicNode* oldCyclicNode = (CyclicNode*)oldNode;
        oldCyclicNode->dealloc();
    }
}


void CyclicNode::detectCyclicNodes(GCObject* tracingObj, GCRefList* tracingList, GCRefList* finishedList) {
    GCNode* current_node = tracingObj->getNode();   
    int last_node_id =  tracingObj->getNodeId();
    current_node->setTraceState(IN_TRACING);
    removeCyclicTest(tracingObj);
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
                RTGC_LOG("## RTGC tracingList remove: %p, %p\n", referrer, tracingObj);
                tracingList->moveTo(referrer, finishedList);

                referrer_node->setTraceState(TRACE_FINISHED);
            }
            break;
        }
        case IN_TRACING: // 순환 경로 발견
        {
            RTGC_LOG("## RTGC Cyclic Found %p-%d\n", referrer, referrer->getNodeId());
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
            cnt = 1;
            RTGC_LOG("## rootObjCount of cyclic node: %d -> %d\n", cyclicNode->getId(), cyclicNode->getRootObjectCount());
            for (GCRefChain* c = cyclicNode->externalReferrers.topChain(); c != NULL; c = c->next()) {
                RTGC_LOG("## External Referrer of cyclic node: %d, %d:%p\n", cyclicNode->getId(), ++cnt, c->obj());
            }
            break;
        }    
        case TRACE_FINISHED: // 이미 추적된 참조 노드
            break;
        default: 
            break;
        }
    }
    if (last_node_id != tracingObj->getNodeId()) {
        if (last_node_id < CYCLIC_NODE_ID_START) {
            RTGC_LOG("## ___ deallic OnewayNode %p\n", current_node->externalReferrers.topChain());
            ((OnewayNode*)current_node)->dealloc();
        }
        else {
            RTGC_LOG("## RTGC deallic CyclicNode %d\n", ((CyclicNode*)current_node)->getId());
            ((CyclicNode*)current_node)->dealloc();
        }
    }
}


void CyclicNode::detectCycles() {
    GCRefList tracingList;
    GCRefList finishedList;
    RTGCMemState* memState = rtgcMem;
    if (memState == NULL) {
        RTGC_LOG("## memState == NULL!");
        return;
    }
    rtgcLock(_DetectCylcles);
    for (GCObject* root = memState->g_cyclicTestNodes.pop(); root != NULL; root = memState->g_cyclicTestNodes.pop()) {
        if (root->isAcyclic()) {
            RTGC_LOG("## RTGC skip acyclic: %p\n", root);
            root->clearNeedCyclicTest();
            continue;
        }
        RTGC_LOG("## RTGC c root: %p, next: %p\n", root, memState->g_cyclicTestNodes.topChain() == NULL ? NULL : memState->g_cyclicTestNodes.topChain()->obj());
        int last_node_id = root->getNodeId();
        GCNode* root_node = root->getNode();
        assert(root->getNodeId() != 0);
        if (!root->isNeedCyclicTest()) {
            RTGC_LOG("## RTGC skip root: %p\n", root);
            continue;
        }

        if (RTGC_STATISTCS) RTGCGlobal::g_cntRemoveCyclicTest ++;
        if (!root->getNode()->isSuspectedCyclic()) {
            RTGC_LOG("## RTGC skip node root: %p\n", root);
            root->clearNeedCyclicTest();
            continue;
        }

        root->clearNeedCyclicTest();
        detectCyclicNodes(root, &tracingList, &finishedList);
        if (last_node_id != root->getNodeId()) {
            if (last_node_id < CYCLIC_NODE_ID_START) {
                RTGC_LOG("## ___ dealloc OnewayNode %p\n", root_node->externalReferrers.topChain());
                ((OnewayNode*)root_node)->dealloc();
            }
            else {
                RTGC_LOG("## RTGC dealloc CyclicNode %d\n", ((CyclicNode*)root_node)->getId());
                ((CyclicNode*)root_node)->dealloc();
            }
        }

        RTGC_LOG("## RTGC 1-2\n");

        assert(tracingList.topChain()->obj() == root);
        assert(tracingList.topChain()->next() == NULL);
        tracingList.clear();

        RTGC_LOG("## RTGC 2\n");
        for (GCObject* obj_ = root; obj_ != NULL; obj_ = finishedList.pop()) {
            CyclicNode* cyclic = obj_->getLocalCyclicNode();
            if ((cyclic != NULL) && cyclic->isGarbage()) {
                RTGC_LOG("## RTGC Garbage Cycle detected in tracing %d/%d :%p\n", obj_->getNodeId(), cyclic->getId(), obj_);
                ::freeContainer(obj_, cyclic->getId());
                cyclic->dealloc();
            }
            else {
                obj_->getNode()->setTraceState(NOT_TRACED);
                //RTGC_LOG("## RTGC Cycle detected:%d rrc:%d %d\n", cyclic->getId(), cyclic->getRootObjectCount(), cyclic->externalReferrers.topChain() == 0 ? 0 : 1);
            }
        }

        // CyclicNode* cyclic = root->getLocalCyclicNode();
        // if (cyclic != NULL) {
        //     if (cyclic->isGarbage()) {
        //         RTGC_LOG("## RTGC Garbage Cycle detected %d rrc:%d\n", cyclic->getId(), cyclic->getRootObjectCount());
        //         ::freeContainer(root, cyclic->getId());
        //         cyclic->dealloc();
        //     }
        //     else {
        //         RTGC_LOG("## RTGC Cycle detected:%d rrc:%d %d\n", cyclic->getId(), cyclic->getRootObjectCount(), cyclic->externalReferrers.topChain() == 0 ? 0 : 1);
        //     }
        // }
    }
    rtgcUnlock();

    //GCNode::dumpGCLog();
    RTGC_LOG("RefChain--: %d\n", RTGCGlobal::g_cntAddRefChain);
}

