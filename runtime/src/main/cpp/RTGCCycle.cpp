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
const bool OPT_TRACING = false;

class CyclicNodeDetector {
    GCRefList tracingList;
    GCRefList finishedList;
    KStdDeque<char*> destroyedNodes;
    bool checkFreezingOnly;
    // GCRefList finishedList_;
public:
    CyclicNodeDetector() {}

    GCNode* markInTracing(GCObject* tracingObj);

    void buildCyclicNode(GCObject* obj)  RTGC_NO_INLINE;
    void addCyclicObject(CyclicNode* targetNode, GCObject* obj)  RTGC_NO_INLINE;

    void traceCyclic(GCObject* root);

    void checkCyclic(KStdVector<KRef>* freezing);

    void detectCyclicNodes(GCObject* tracingObj);
    void detectCyclicNodes2();

    void traceCyclicNodes(GCObject* tracingObj);

    void destroyNode(GCNode* node);
};


CyclicNode* CyclicNode::create() {
    CyclicNode* node = rtgcMem->cyclicNodeAllocator.allocItem();
    memset(node, 0, sizeof(CyclicNode));
    if (RTGC_STATISTCS) RTGCGlobal::g_cntAddCyclicNode ++;
    return node;
}

void CyclicNode::dealloc() {
    RTGC_LOG("## RTGC dealloc node %p:%d\n", this, this->getId());
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
    DebugRefAssert(obj, !obj->isAcyclic());
    DebugRefAssert(obj, !obj->isNeedCyclicTest());
    RTGC_LOG("addCyclicTest %p\n", obj);
    obj->markNeedCyclicTest();

    rtgcMem->g_cyclicTestNodes.push(obj);
}

void CyclicNode::removeCyclicTest(RTGCMemState* rtgcMem, GCObject* obj) {
    if (!obj->isEnquedCyclicTest()) return;

    obj->clearNeedCyclicTest();
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
    char* destroyed = targetNode->addCyclicObject(rookie);
    if (DELAY_NODE_DESTROY) {
        if (destroyed != nullptr) {
            destroyedNodes.push_back(destroyed);
        }
    }
}

char* CyclicNode::addCyclicObject(
    GCObject* rookie /* 추가 객체*/
) {
    GCNode* oldNode = rookie->getNode();
    if (oldNode == this) return 0;
    
    /* 객체의 소속 참조 노드 변경 */
    int this_id = this->getId();
    bool rookieInCyclic = rookie->isInCyclicNode();
    rookie->setNodeId(this_id);
    assert(rookie->isInCyclicNode() && rookie->getNode() == this);
    
    if (rookieInCyclic) {
        CyclicNode* oldCyclicNode = (CyclicNode*)oldNode;
        //if (RTGC_STATISTCS) konan::consolePrintf("merge cyclic\n");
        RTGC_LOG("## RTGC Merge Cycle %d %d\n", this->getRootObjectCount(), oldCyclicNode->getRootObjectCount());
        this->rootObjectCount += oldCyclicNode->rootObjectCount;
        this->mergeCyclicNode(rookie, oldCyclicNode->getId());
    }
    else {
        if (rookie->getRootRefCount() > 0) {
            this->rootObjectCount ++;
        }
    }


    //RTGC_LOG("## RTGC merge external referrers: %p\n", oldNode->externalReferrers.topChain());
    for(GCRefChain* chain = oldNode->externalReferrers.topChain(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        if (referrer->getNode() != this) {
            RTGC_LOG("## RTGC add referrer of cyclic: %p -> %d\n", referrer, this_id);
            this->externalReferrers.push(referrer);
        }
    }

    // RTGC_LOG("## RTGC merge external referrers done: %p\n", oldNode->externalReferrers.topChain());
    this->externalReferrers.tryRemove(rookie, false);

    RTGC_LOG("## RTGC add cyclic obj done: %p\n", this_id);
    return (char*)oldNode + (rookieInCyclic ? 1 : 0);
}


GCNode* CyclicNodeDetector::markInTracing(GCObject* tracingObj) {
    DebugRefAssert(tracingObj, !tracingObj->isAcyclic());
    GCNode* current_node = tracingObj->getNode();
    current_node->setTraceState(IN_TRACING);
    tracingList.push(tracingObj);
    return current_node;
}

CyclicNode* CyclicNode::createTwoWayLink(GCObject* root, GCObject* rookie) {
    CyclicNode* cyclicNode = root->getLocalCyclicNode();
    if (cyclicNode == nullptr) {
        cyclicNode = rookie->getLocalCyclicNode();
    }
    if (cyclicNode == nullptr) {
        cyclicNode = CyclicNode::create();
    }
    if (root->isNeedCyclicTest() || rookie->isNeedCyclicTest()) {
        cyclicNode->markSuspectedCyclic();
    }
    cyclicNode->addCyclicObject(root);
    cyclicNode->addCyclicObject(rookie);
    return cyclicNode;
}


void CyclicNodeDetector::buildCyclicNode(GCObject* referrer) {
    CyclicNode* cyclicNode = referrer->getLocalCyclicNode();
    GCNode* referrer_node = referrer->getNode();
    if (cyclicNode == nullptr) {
        if (true) {
            cyclicNode = CyclicNode::create();
        }
        else {
            int cnt = 0;
            for (GCRefChain* pChain = tracingList.topChain(); ; pChain = pChain->next()) {
                cyclicNode = pChain->obj()->getLocalCyclicNode();
                cnt ++;
                if (cyclicNode != nullptr) {
                    break;
                }
                if (pChain->obj()->getNode() == referrer_node) {
                    cyclicNode = CyclicNode::create();
                    break;
                }
            }
        }
    }

    int cnt __attribute__((unused)) = 1;
    if (!OPT_TRACING) {
        GCRefChain* pChain = tracingList.topChain();
        while (pChain->obj()->getNode() != referrer_node) {
            GCObject* rookie = pChain->obj();
            RTGC_LOG("## RTGC Cyclic add: %d:%d, %p\n", cyclicNode->getId(), cnt++, rookie);
            pChain = pChain->next();
            addCyclicObject(cyclicNode, rookie);
        }
        RTGC_LOG("## RTGC Cyclic add last: %d:%d, %p\n", cyclicNode->getId(), cnt, referrer);
        addCyclicObject(cyclicNode, referrer);
        cyclicNode->setTraceState(IN_TRACING);
        tracingList.setFirst(pChain);
    }
    else {
        GCRefChain* prev = NULL;
        for (GCRefChain* pChain = tracingList.topChain(); pChain->obj()->getNode() != referrer_node; ) {
            GCObject* rookie = pChain->obj();
            GCRefChain* next = pChain->next();
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
                prev = pChain;
            }
            pChain = next;
        }
        RTGC_LOG("##     Cyclic add last: %d:%d, %p\n", cyclicNode->getId(), cnt, referrer);
        addCyclicObject(cyclicNode, referrer);
        cyclicNode->setTraceState(IN_TRACING);
    }

    if (ENABLE_RTGC_LOG) {
        cnt = 1;
        RTGC_LOG("## rootObjCount of cyclic node: %d -> %d\n", cyclicNode->getId(), cyclicNode->getRootObjectCount());
        for (GCRefChain* c = cyclicNode->externalReferrers.topChain(); c != NULL; c = c->next()) {
            RTGC_LOG("## External Referrer of cyclic node: %d, %d:%p\n", cyclicNode->getId(), ++cnt, c->obj());
        }
    }
}

void CyclicNodeDetector::traceCyclicNodes(GCObject* tracingObj) {
    /** TODO
     * KStdVect<GCRefChain*> tracingList;
     */
    GCNode* current_node = tracingObj->getNode();
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
                RTGC_LOG("##     Cyclic Found %p-%d\n", referrer, referrer->getNodeId());
                if (referrer_node == tracingObj->getNode()) {
                    RTGC_LOG("##     Cyclic inside: %p:%p, %p:%p\n", tracingObj, referrer->getNode(), referrer, referrer->getNode());
                    break;
                }
                buildCyclicNode(referrer);
                break;
            case TRACE_FINISHED: // 이미 추적된 참조 노드
                RTGC_LOG("##     already Traced %p\n", referrer);
            default: 
                // tracingList.trySetFirst(pChain);
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


void CyclicNode::garbageCollectCycles(void* freezing) {
    CyclicNodeDetector detector;
    RTGCMemState* memState = rtgcMem;
    if (memState == NULL) {
        RTGC_LOG("## memState == NULL!");
        return;
    }
    detector.checkCyclic((KStdVector<KRef>*)freezing);
}

void ScheduleDestroyContainer(MemoryState* state, ContainerHeader* container);

void CyclicNodeDetector::checkCyclic(KStdVector<KRef>* freezing) {
    GCNode::rtgcLock(_DetectCylcles);
    if (freezing == nullptr) {
        this->checkFreezingOnly = false;
        GCRefList* cyclicTestNodes = &rtgcMem->g_cyclicTestNodes;

        for (GCObject* root = cyclicTestNodes->pop(); root != NULL; root = cyclicTestNodes->pop()) {
            DebugAssert(isValidObjectContainer(root));
            if (RTGC_LATE_DESTROY_CYCLIC_SUSPECT && root->isDestroyed()) {
                root->clearNeedCyclicTest();
                ScheduleDestroyContainer((MemoryState*)rtgcMem, root);
            }
            else if (root->clearNeedCyclicTest()) {
                this->traceCyclic(root);
            }
        }
    }
    else {
        this->checkFreezingOnly = true;
        for (auto* obj: *freezing) {
            if (RTGC_STATISTCS) RTGCGlobal::g_cntFreezed ++;
            ContainerHeader* root = obj->container();
            DebugAssert(isValidObjectContainer(root));
            if (root->clearNeedCyclicTest()) {
                this->traceCyclic(root);
            }
        }
    }
    RTGC_LOG("## RTGC 2\n");
    while (DELAY_NODE_DESTROY && !destroyedNodes.empty()) {
        /**
         * 주의) Cyclic Garbage 처리 전에, destroyedNode 를 처리해야 한다.
         * OnewayNode 는 실제로 GCObject의 일부이므로,
         * Cyclic Garbage 삭제시 OnewayNode 가 함께 삭제되는 문제가 발생한다.
         */
        char* node = destroyedNodes.back();
        destroyedNodes.pop_back();
        if (((int64_t)node & 1) == 0) {
            ((OnewayNode*)node)->dealloc();
        }
        else {
            node -= 1;
            ((CyclicNode*)node)->dealloc();
        }
    }

    for (GCRefChain* chain = finishedList.topChain(); chain != NULL; chain = chain->next()) {
        /**
         * garbage cyclic node 를 삭제하는 동안 해당 노드에 연결된 다른 순환 참조 노드가 garbage 상태로 변경되어 삭제될 수 있다.
         * 이에 먼저 state 를 변경하고, garbage cyclic node 삭제작업을 실시한다.
         */
        GCObject* obj_ = chain->obj();
        RTGC_LOG("## RTGC Reset TraceState obj:%p node:%p/%d\n", obj_, obj_->getNode(), obj_->getNodeId());
        obj_->getNode()->setTraceState(NOT_TRACED);
    }

    for (GCObject* obj_; (obj_ = finishedList.pop()) != NULL;) {
        CyclicNode* cyclic = obj_->getLocalCyclicNode();
        if ((cyclic != NULL) && cyclic->isGarbage()) {
            //if (RTGC_STATISTCS) konan::consolePrintf("## RTGC Garbage Cycle detected in tracing obj:%p node:%p/%d \n", obj_, cyclic, cyclic->getId());
            ::freeContainer(obj_, cyclic->getId());
            cyclic->dealloc();
        }
    }
    GCNode::rtgcUnlock();

}

void CyclicNodeDetector::traceCyclic(GCObject* root) {
    if (root->isAcyclic() || root->frozen()) {
        // isAcyclic() = true 인 경우:
        //   InitSharedInstance() 실행 후, cyclicTestNode 에 포함된 객체가
        //   Acyclic으로 변경되었거나, acyclic freezing 객체이다.
        DebugRefAssert(root, root->frozen() || !root->freeable() || root->isFreezing());
        return;
    }

    RTGC_LOG("## RTGC c root: %p, next: %p\n", root, rtgcMem->g_cyclicTestNodes.topChain() == NULL ? NULL : rtgcMem->g_cyclicTestNodes.topChain()->obj());
    GCNode* node = root->getNode();
    if (node->getTraceState() != NOT_TRACED) {
        DebugRefAssert(root, node->getTraceState() == TRACE_FINISHED);
        return;
    }

    if (false) {
        node->setTraceState(TRACE_REQUESTED);
        tracingList.push(root);
        detectCyclicNodes2();        
    }
    else {
        detectCyclicNodes(root);
        assert(tracingList.topChain()->obj() == root);
        assert(tracingList.topChain()->next() == NULL);
        tracingList.clear();
    }

    // node maybe changed.
    node = root->getNode();
    RuntimeAssert(node->getTraceState() != TRACE_FINISHED, "Root trace finished");
    node->setTraceState(TRACE_FINISHED);
    finishedList.push(root);
}

void CyclicNodeDetector::detectCyclicNodes(GCObject* tracingObj) {

    GCNode* current_node = markInTracing(tracingObj);
    int last_node_id =  tracingObj->getNodeId();


    RTGC_LOG("## RTGC tracingList add: %p(mem:%p)\n", tracingObj, rtgcMem);
    
    for (GCRefChain* chain = current_node->externalReferrers.topChain(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        if (checkFreezingOnly && !referrer->isFreezing()) {
            continue;
        }

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
                RTGC_LOG("## RTGC Cyclic Found %p:%d %p\n", referrer, referrer->getNodeId(), tracingObj->getNode());
                if (referrer_node == tracingObj->getNode()) {
                    RTGC_LOG("## RTGC Cyclic inside: %p:%p, %p:%p\n", tracingObj, referrer->getNode(), referrer, referrer->getNode());
                    break;
                }
                buildCyclicNode(referrer);

                break;
            }    
            case TRACE_FINISHED: // 이미 추적된 참조 노드
            default: 
                RTGC_LOG("## RTGC TRACE_FINISHED Found %p-%d\n", referrer, referrer->getNodeId());
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