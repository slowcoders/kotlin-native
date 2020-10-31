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
static const bool DELAY_NODE_DESTROY = true;

class CyclicNodeDetector {
    GCRefList tracingList;
    GCRefList finishedList;
    KStdDeque<char*> destroyedNodes;
    // GCRefList finishedList_;
public:
    CyclicNodeDetector() {}

    GCNode* markInTracing(GCObject* tracingObj);

    void addCyclicObject(CyclicNode* targetNode, GCObject* obj)  RTGC_NO_INLINE;

    void checkCyclic(GCObject* root);

    void checkCyclic(GCRefList* g_cyclicTestNodes);

    void detectCyclicNodes(GCObject* tracingObj);
    void detectCyclicNodes2();

    void traceCyclicNodes(GCObject* tracingObj);

    void destroyNode(GCNode* node);
};


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
    //assert(isLocked());

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

void CyclicNode::removeCyclicTest(RTGCMemState* rtgcMem, GCObject* obj) {
    //RuntimeAssert(isLocked(), "GCNode is not locked")
    if (!obj->isNeedCyclicTest()) return;
    obj->clearNeedCyclicTest();
    if (RTGC_STATISTCS) RTGCGlobal::g_cntRemoveCyclicTest ++;

    RTGC_LOG("## RTGC Remove Cyclic Test %p:%d\n", obj, obj->getNodeId());
    /* TODO ZZZZ replace tryRemove => remove */
    rtgcMem->g_cyclicTestNodes.tryRemove(obj, true);
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

void CyclicNodeDetector::addCyclicObject(
    CyclicNode* targetNode, GCObject* rookie /* 추가 객체*/
) {
    GCNode* oldNode = rookie->getNode();
    if (oldNode == targetNode) return;
    
    /* 객체의 소속 참조 노드 변경 */
    int this_id = targetNode->getId();
    bool rookieInCyclic = rookie->isInCyclicNode();
    rookie->setNodeId(this_id);
    assert(rookie->isInCyclicNode() && rookie->getNode() == targetNode);
    
    if (rookieInCyclic) {
        CyclicNode* oldCyclicNode = (CyclicNode*)oldNode;
        RTGC_LOG("## RTGC Merge Cycle %d %d\n", targetNode->getRootObjectCount(), oldCyclicNode->getRootObjectCount());
        targetNode->rootObjectCount += oldCyclicNode->rootObjectCount;
        targetNode->mergeCyclicNode(rookie, oldCyclicNode->getId());
    }
    else {
        if (rookie->getRootRefCount() > 0) {
            targetNode->rootObjectCount ++;
        }
    }


    RTGC_LOG("## RTGC merge external referrers: %p\n", oldNode->externalReferrers.topChain());
    for(GCRefChain* chain = oldNode->externalReferrers.topChain(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        if (referrer->getNode() != targetNode) {
            RTGC_LOG("## RTGC add referrer of cyclic: %p=%p\n", chain, referrer);
            targetNode->externalReferrers.push(referrer);
        }
    }

    // RTGC_LOG("## RTGC merge external referrers done: %p\n", oldNode->externalReferrers.topChain());
    targetNode->externalReferrers.tryRemove(rookie, false);
    if (DELAY_NODE_DESTROY) {
        destroyedNodes.push_front((char*)oldNode + (rookieInCyclic ? 1 : 0));
    }

    RTGC_LOG("## RTGC add cyclic obj done: %p\n", this_id);
}


GCNode* CyclicNodeDetector::markInTracing(GCObject* tracingObj) {
    GCNode* current_node = tracingObj->getNode();
    current_node->setTraceState(IN_TRACING);
    CyclicNode::removeCyclicTest(rtgcMem, tracingObj);
    current_node->clearSuspectedCyclic();

    tracingList.push(tracingObj);
    return current_node;
}


void CyclicNodeDetector::traceCyclicNodes(GCObject* tracingObj) {
    /** TODO
     * KStdVect<GCRefChain*> tracingList;
     */
    GCNode* current_node = tracingObj->getNode();
    CyclicNode::removeCyclicTest(rtgcMem, tracingObj);
    current_node->clearSuspectedCyclic();
    current_node->setTraceState(IN_TRACING);

    // int last_node_id =  tracingObj->getNodeId();

    RTGC_LOG("## RTGC tracing start %p\n", tracingObj);
    
    for (GCRefChain* chain = current_node->externalReferrers.topChain(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        RTGC_LOG("##    Tracing into %p %d memberRef=%d isCyclic=%d\n", referrer, referrer->getNodeId(), referrer->getMemberRefCount(), referrer->isInCyclicNode());
        GCNode* referrer_node = referrer->getNode();

        if (ENABLE_RTGC_LOG) RTGCGlobal::validateMemPool();
        switch (referrer_node->getTraceState()) {
            case NOT_TRACED: // 추적되지 않은 참조 노드
                RTGC_LOG("##     Push to trace %p\n", referrer);
                current_node->setTraceState(TRACE_REQUESTED);
                tracingList.push(referrer);
                break;
            case TRACE_REQUESTED:
                RTGC_LOG("##     Recursive trace %p\n", referrer);
                /**
                 * 이미 TracingList 에 추가된 객체다.
                 * 해당 객체를 통하여 순환참조가 발생할 수 있다. 어쩔 수 없이 recursive 검사.
                 */
                traceCyclicNodes(referrer);
                break;
            case IN_TRACING: // 순환 경로 발견
            {
                RTGC_LOG("##     Cyclic Found %p-%d\n", referrer, referrer->getNodeId());
                if (referrer_node == tracingObj->getNode()) {
                    RTGC_LOG("##     Cyclic inside: %p:%p, %p:%p\n", tracingObj, referrer->getNode(), referrer, referrer->getNode());
                    break;
                }
                CyclicNode* cyclicNode = referrer->isInCyclicNode() ? (CyclicNode*)referrer_node : CyclicNode::create();
                int cnt = 1;
                GCRefChain* prev = NULL;
                GCRefChain* tracingChain = tracingList.topChain();
                while (tracingChain->obj()->getNode() != referrer_node) {
                    GCObject* rookie = tracingChain->obj();
                    GCRefChain* next = tracingChain->next();
                    if (rookie->getNode()->getTraceState() == IN_TRACING) {
                        RTGC_LOG("##     Cyclic add: %d:%d, %p\n", cyclicNode->getId(), cnt++, rookie);
                        addCyclicObject(cyclicNode, rookie);
                        if (prev == NULL) {
                            tracingList.setFirst(next);
                        }
                        else {
                            prev->next_ = next;
                        }
                    }
                    else {
                        prev = tracingChain;
                    }
                    tracingChain = next;
                }
                RTGC_LOG("##     Cyclic add last: %d:%d, %p\n", cyclicNode->getId(), cnt, referrer);
                addCyclicObject(cyclicNode, referrer);
                cyclicNode->setTraceState(IN_TRACING);
                cnt = 1;
                RTGC_LOG("## rootObjCount of cyclic node: %d -> %d\n", cyclicNode->getId(), cyclicNode->getRootObjectCount());
                for (GCRefChain* c = cyclicNode->externalReferrers.topChain(); c != NULL; c = c->next()) {
                    RTGC_LOG("## External Referrer of cyclic node: %d, %d:%p\n", cyclicNode->getId(), ++cnt, c->obj());
                }
                break;
            }    
            case TRACE_FINISHED: // 이미 추적된 참조 노드
                RTGC_LOG("##     already Traced %p\n", referrer);
            default: 
                // tracingList.trySetFirst(tracingChain);
                break;
        }
    }
    RTGC_LOG("## RTGC tracing 2 %p\n", tracingObj);
}

void CyclicNodeDetector::detectCyclicNodes2() {

    while (!tracingList.isEmpty()) {
        GCObject* tracingObj = tracingList.topChain()->obj();
        GCNode* current_node = tracingObj->getNode();
        if (current_node->getTraceState() != TRACE_REQUESTED) {
            current_node->setTraceState(TRACE_FINISHED);
            tracingList.pop();
            finishedList.push(tracingObj);
            continue;
        }
        traceCyclicNodes(tracingObj);

    }
}


void CyclicNode::detectCycles() {
    CyclicNodeDetector detector;
    RTGCMemState* memState = rtgcMem;
    if (memState == NULL) {
        RTGC_LOG("## memState == NULL!");
        return;
    }
    detector.checkCyclic(&memState->g_cyclicTestNodes);

    //GCNode::dumpGCLog();
    RTGC_LOG("RefChain--: %d\n", RTGCGlobal::g_cntAddRefChain);
}


void CyclicNodeDetector::checkCyclic(GCRefList* cyclicTestNodes) {
    GCNode::rtgcLock(_DetectCylcles);
    for (GCObject* root = cyclicTestNodes->pop(); root != NULL; root = cyclicTestNodes->pop()) {
        this->checkCyclic(root);
    }
    RTGC_LOG("## RTGC 2\n");
    for (GCObject* obj_; (obj_ = finishedList.pop()) != NULL;) {
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

    GCNode::rtgcUnlock();

    if (DELAY_NODE_DESTROY) {
        while (!destroyedNodes.empty()) {
            char* node = destroyedNodes.front();
            destroyedNodes.pop_front();
            if (((int64_t)node & 1) == 0) {
                ((OnewayNode*)node)->dealloc();
            }
            else {
                node -= 1;
                ((CyclicNode*)node)->dealloc();
            }
        }
    }
}

void CyclicNodeDetector::checkCyclic(GCObject* root) {
    if (root->isAcyclic()) {
        RTGC_LOG("## RTGC skip acyclic: %p\n", root);
        root->clearNeedCyclicTest();
        return;
    }
    RTGC_LOG("## RTGC c root: %p, next: %p\n", root, rtgcMem->g_cyclicTestNodes.topChain() == NULL ? NULL : rtgcMem->g_cyclicTestNodes.topChain()->obj());
    assert(root->getNodeId() != 0);
    if (!root->isNeedCyclicTest()) {
        RTGC_LOG("## RTGC skip root: %p\n", root);
        return;
    }

    if (RTGC_STATISTCS) RTGCGlobal::g_cntRemoveCyclicTest ++;
    if (!root->getNode()->isSuspectedCyclic()) {
        RTGC_LOG("## RTGC skip node root: %p\n", root);
        root->clearNeedCyclicTest();
        return;
    }

    if (false) {
        root->getNode()->setTraceState(TRACE_REQUESTED);
        tracingList.push(root);
        detectCyclicNodes2();        
    }
    else {
        detectCyclicNodes(root);
        assert(tracingList.topChain()->obj() == root);
        assert(tracingList.topChain()->next() == NULL);
        tracingList.clear();
    }

    CyclicNode* cyclic = root->getLocalCyclicNode();
    if ((cyclic != NULL) && cyclic->isGarbage()) {
        RTGC_LOG("## RTGC Garbage Cycle detected in tracing %d/%d :%p\n", root->getNodeId(), cyclic->getId(), root);
        ::freeContainer(root, cyclic->getId());
        cyclic->dealloc();
    }
}

void CyclicNodeDetector::detectCyclicNodes(GCObject* tracingObj) {

    GCNode* current_node = markInTracing(tracingObj);
    int last_node_id =  tracingObj->getNodeId();
    const bool OPT_TRACING = false;


    RTGC_LOG("## RTGC tracingList add: %p=%p\n", tracingList.topChain(), tracingObj);
    
    for (GCRefChain* chain = current_node->externalReferrers.topChain(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        RTGC_LOG("## Tracing Obj %p %d memberRef=%d isCyclic=%d\n", referrer, referrer->getNodeId(), referrer->getMemberRefCount(), referrer->isInCyclicNode());
        GCNode* referrer_node = referrer->getNode();
        while (OPT_TRACING && referrer->getMemberRefCount() == 1 && referrer_node->getTraceState() == NOT_TRACED && !referrer->isInCyclicNode()) {
            /**
             * current_node 만이 유일한 referrer 인 경우, referrer_node 의 상태는 반드시 NOT_TRACED 이어야 한다.
             * current_node 만이 순환참조의 진입점이 될 수 있다. (외줄 순환인 경우,referrer 와 tracingObj 가 동일해질 수 있다.)
             * skip 된 referrer 의 TraceState 는 Reset하지 않다도 된다. 
             * (순환참조 구성 시에는 Node 가 바뀌고, 아닌 경우 이미 Reset(NOT_TRACED) 상태이기 때문이다.
             */
            //RuntimeAssert(referrer_node->getTraceState() == NOT_TRACED, "Something wrong in detectCyclicNodes");
            markInTracing(referrer);
            referrer = referrer_node->externalReferrers.topChain()->obj();
            referrer_node = referrer->getNode();
        }

        if (ENABLE_RTGC_LOG) RTGCGlobal::validateMemPool();
        switch (referrer_node->getTraceState()) {
            case NOT_TRACED: // 추적되지 않은 참조 노드
            {
                RTGC_LOG("## RTGC Cyclic NOT TRACED");
                detectCyclicNodes(referrer);
                // 노드 변경 상황 반영.
                if (!OPT_TRACING) {
                    referrer_node = referrer->getNode();
                    if (tracingObj->getNode() != referrer_node) {
                        // tracingObj와 referent 를 경유하는 순환 경로가 발견되지 않은 경우
                        RTGC_LOG("## RTGC tracingList remove: %p, %p\n", referrer, tracingObj);
                        tracingList.moveTo(referrer, &finishedList);
                        referrer_node->setTraceState(TRACE_FINISHED);
                    }
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
                int cnt = 1;
                GCRefChain* tracingChain = tracingList.topChain();
                while (tracingChain->obj()->getNode() != referrer_node) {
                    GCObject* rookie = tracingChain->obj();
                    RTGC_LOG("## RTGC Cyclic add: %d:%d, %p\n", cyclicNode->getId(), cnt++, rookie);
                    tracingChain = tracingChain->next();
                    addCyclicObject(cyclicNode, rookie);
                }
                RTGC_LOG("## RTGC Cyclic add last: %d:%d, %p\n", cyclicNode->getId(), cnt, referrer);
                addCyclicObject(cyclicNode, referrer);
                cyclicNode->setTraceState(IN_TRACING);
                tracingList.setFirst(tracingChain);
                cnt = 1;
                RTGC_LOG("## rootObjCount of cyclic node: %d -> %d\n", cyclicNode->getId(), cyclicNode->getRootObjectCount());
                for (GCRefChain* c = cyclicNode->externalReferrers.topChain(); c != NULL; c = c->next()) {
                    RTGC_LOG("## External Referrer of cyclic node: %d, %d:%p\n", cyclicNode->getId(), ++cnt, c->obj());
                }
                break;
            }    
            case TRACE_FINISHED: // 이미 추적된 참조 노드
            default: 
                // tracingList.trySetFirst(tracingChain);
                break;
        }
        GCNode* topNode = tracingObj->getNode();
        while (OPT_TRACING) {
            GCObject* obj = tracingList.topChain()->obj();
            GCNode* node = obj->getNode();
            if (node == topNode) break;

            tracingList.pop();
            finishedList.push(obj);
            node->setTraceState(TRACE_FINISHED);
        }
    }

    if (!DELAY_NODE_DESTROY && last_node_id != tracingObj->getNodeId()) {
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