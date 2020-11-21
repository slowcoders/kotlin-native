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

typedef KStdVector<ContainerHeader*> ContainerHeaderList;

extern THREAD_LOCAL_VARIABLE RTGCMemState* rtgcMem;
static const bool DELAY_NODE_DESTROY = true;
const bool OPT_TRACING = false;

class CyclicNodeDetector {
    GCRefList tracingList;
    GCRefList finishedList;
    KStdDeque<char*> destroyedNodes;
    bool checkFreezingOnly;
    KStdDeque<GCRefChain*> tracingQ;
    // GCRefList finishedList_;
public:
    CyclicNodeDetector() {}

    GCNode* markInTracing(GCObject* tracingObj);

    void buildCyclicNode(GCObject* obj)  RTGC_NO_INLINE;
    void buildCyclicNode2(GCObject* obj)  RTGC_NO_INLINE;
    void addCyclicObject(CyclicNode* targetNode, GCObject* obj)  RTGC_NO_INLINE;

    void traceCyclic(GCObject* root);

    void checkCyclic(KStdVector<KRef>* freezing);

    void detectCyclicNodes(GCObject* tracingObj);
    void detectCyclicNodes2(GCObject* tracingObj);

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
    DebugRefAssert(obj, obj->getNode()->mayCreateCyclicReference());
    RTGC_LOG("addCyclicTest %p %p\n", obj, rtgcMem);
    if (false && (char*)obj < (char*)0x10000a914bd0) {
        RTGC_dumpRefInfo(obj);
    }
    if (obj->enqueueCyclicTest()) {
        rtgcMem->g_cyclicTestNodes.push_back(obj);
    }
    else {
        DebugRefAssert(obj, obj->frozen());
        //konan::consolePrintf("Double enque cyclic test!!!");
    }
    obj->getNode()->markSuspectedCyclic();
}

void CyclicNode::removeCyclicTest(RTGCMemState* rtgcMem, GCObject* obj) {
    DebugAssert(!RTGC_LATE_DESTROY_CYCLIC_SUSPECT);
#if !RTGC_LATE_DESTROY_CYCLIC_SUSPECT
    if (!obj->isEnquedCyclicTest()) return;

    obj->dequeueCyclicTest();
    RTGC_LOG("## RTGC Remove Cyclic Test %p:%d\n", obj, obj->getNodeId());
    /* TODO ZZZZ replace tryRemove => remove */
    rtgcMem->g_cyclicTestNodes.tryRemove(obj, true);
#endif
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
    // this->externalReferrers.tryRemove(rookie, false);
    this->markDirtyReferrerList();

    RTGC_LOG_V("## RTGC add cyclic obj done: %p\n", rookie);
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
    RTGC_LOG("twoWay detected: %p/%d, %p/%d\n", root, root->getNodeId(), rookie, rookie->getNodeId());
    CyclicNode* cyclicNode = root->getLocalCyclicNode();
    if (cyclicNode == nullptr) {
        cyclicNode = rookie->getLocalCyclicNode();
    }
    if (cyclicNode == nullptr) {
        cyclicNode = CyclicNode::create();
    }
    if (root->getNode()->isSuspectedCyclic() || rookie->getNode()->isSuspectedCyclic()) {
        cyclicNode->markSuspectedCyclic();
    }
    cyclicNode->addCyclicObject(root);
    cyclicNode->addCyclicObject(rookie);
    cyclicNode->clearDirtyReferrers();
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

    if (ENABLE_RTGC_LOG_VERBOSE) {
        cnt = 1;
        RTGC_LOG("## rootObjCount of cyclic node: %d -> %d\n", cyclicNode->getId(), cyclicNode->getRootObjectCount());
        for (GCRefChain* c = cyclicNode->externalReferrers.topChain(); c != NULL; c = c->next()) {
            RTGC_LOG("## External Referrer of cyclic node: %d, %d:%p\n", cyclicNode->getId(), ++cnt, c->obj());
        }
    }
}


void CyclicNodeDetector::buildCyclicNode2(GCObject* referrer) {
    CyclicNode* cyclicNode = referrer->getLocalCyclicNode();
    GCNode* referrer_node = referrer->getNode();
    if (cyclicNode == nullptr) {
        cyclicNode = CyclicNode::create();
    }

    int cnt __attribute__((unused)) = 1;
    size_t idx = tracingQ.size() - 1;
    DebugAssert(tracingQ[idx]->obj() == referrer);
    for (;;) {
        DebugAssert(idx > 0);
        GCRefChain* pChain = tracingQ[--idx];
        DebugAssert(pChain != nullptr);
        GCObject* rookie = pChain->obj();
        DebugAssert(rookie != nullptr);
        if (rookie->getNode() == referrer_node) {
            break;
        }
        RTGC_LOG("## RTGC Cyclic add: %d:%d, %p\n", cyclicNode->getId(), cnt++, rookie);
        addCyclicObject(cyclicNode, rookie);
    }
    RTGC_LOG("## RTGC Cyclic add last: %d:%d, %p\n", cyclicNode->getId(), cnt, referrer);
    addCyclicObject(cyclicNode, referrer);
    cyclicNode->setTraceState(IN_TRACING);

    if (ENABLE_RTGC_LOG_VERBOSE) {
        cnt = 1;
        RTGC_LOG("## rootObjCount of cyclic node: %d -> %d\n", cyclicNode->getId(), cyclicNode->getRootObjectCount());
        for (GCRefChain* c = cyclicNode->externalReferrers.topChain(); c != NULL; c = c->next()) {
            RTGC_LOG("## External Referrer of cyclic node: %d, %d:%p\n", cyclicNode->getId(), ++cnt, c->obj());
        }
    }
}

void CyclicNodeDetector::detectCyclicNodes2(GCObject* traceRoot) {

    GCRefChain root;
    root.obj_ = traceRoot;
    root.next_ = NULL;
    tracingQ.push_back(&root);
    
    while (true) {
        GCRefChain* chain = tracingQ.back();
        GCObject* referrer = chain->obj();
        
        DebugRefAssert(referrer, !referrer->isAcyclic());
        GCNode* referrer_node = referrer->getNode();
        
        GCRefChain* start_chain;
        switch (referrer_node->getTraceState()) {
            case NOT_TRACED: // 추적되지 않은 참조 노드 
                if (checkFreezingOnly && !referrer->isFreezing()) break;

                start_chain = referrer_node->externalReferrers.topChain();
                if (start_chain == NULL) break;

                RTGC_LOG("## RTGC tracingQ add: %p(mem:%p)\n", referrer, rtgcMem);
                referrer_node->setTraceState(IN_TRACING);
                tracingQ.push_back(start_chain);
                continue;

            case IN_TRACING: // 순환 경로 발견
                RTGC_LOG("## RTGC Cyclic Found %p:%d\n", referrer, referrer->getNodeId());
                buildCyclicNode2(referrer);
                referrer_node = referrer->getNode();
                break;

            case TRACE_FINISHED: // 이미 추적된 참조 노드
            default: 
                RTGC_LOG("## RTGC TRACE_FINISHED Found %p/%d\n", referrer, referrer->getNodeId());
                break;
        }

        GCRefChain* nextChain = chain->next();
        while (true) {
            DebugAssert(tracingQ[tracingQ.size()-1]->obj() == referrer);
            size_t traceDepth = tracingQ.size() - 1;
            GCObject* parent = traceDepth == 0 ? nullptr : tracingQ[traceDepth-1]->obj();
            if (parent == nullptr || referrer_node != parent->getNode()) {
                // tracingObj와 referent 를 경유하는 순환 경로가 발견되지 않은 경우
                RTGC_LOG("## RTGC tracingQ remove: %p\n", referrer);
                finishedList.push(referrer);
                referrer_node->setTraceState(TRACE_FINISHED);
                if (parent == nullptr) {
                    DebugAssert(nextChain == NULL);
                    tracingQ.pop_back();
                    DebugAssert(tracingQ.empty());
                    RTGC_LOG("## RTGC trace finished: %p\n", root);
                    return;
                }
            }

            if (nextChain != NULL) break;
        
            tracingQ.pop_back();
            referrer = parent;
            referrer_node = referrer->getNode();
            nextChain = tracingQ.back()->next();
        }
        tracingQ.back() = nextChain;
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

void ScheduleDestroyContainer(MemoryState* state, ContainerHeader* container, const char* msg="");

void CyclicNodeDetector::checkCyclic(KStdVector<KRef>* freezing) {
    GCNode::rtgcLock(_DetectCylcles);
    if (freezing == nullptr) {
        this->checkFreezingOnly = false;
        KStdDeque<GCObject*>* cyclicTestNodes = &rtgcMem->g_cyclicTestNodes;

        while (!cyclicTestNodes->empty()) {
            GCObject* root = cyclicTestNodes->back();
            cyclicTestNodes->pop_back(); 
            DebugAssert(isValidObjectContainer(root));
            DebugRefAssert(root, root->isEnquedCyclicTest() || (!root->freeable() && root->shared()));
            //DebugAssert(cyclicTestNodes->find(root) == nullptr);
            if (RTGC_LATE_DESTROY_CYCLIC_SUSPECT && root->isDestroyed()) {
                root->dequeueCyclicTest();
                ScheduleDestroyContainer((MemoryState*)rtgcMem, root, "in RTGC");
            }
            else if (root->dequeueCyclicTest()) {
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
            if (root->getNodeId() != 0 && root->getNode()->isSuspectedCyclic()) {
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
        RTGC_LOG_V("## RTGC Reset TraceState obj:%p node:%p/%d\n", obj_, obj_->getNode(), obj_->getNodeId());
        obj_->getNode()->setTraceState(NOT_TRACED);
    }

    for (GCObject* obj_; (obj_ = finishedList.pop()) != NULL;) {
        CyclicNode* cyclic = obj_->getLocalCyclicNode();
        if (cyclic != NULL) {
            cyclic->clearDirtyReferrers();
            if (cyclic->isGarbage()) {
                //if (RTGC_STATISTCS) konan::consolePrintf("## RTGC Garbage Cycle detected in tracing obj:%p node:%p/%d \n", obj_, cyclic, cyclic->getId());
                ::freeContainer(obj_, cyclic->getId());
                cyclic->dealloc();
            }
        }
    }
    GCNode::rtgcUnlock();
    RTGC_LOG("## RTGC CYCLE DETECTER END %p\n", rtgcMem);

}

void CyclicNodeDetector::traceCyclic(GCObject* root) {
    if (root->isAcyclic() || root->frozen()) {
        if (ENABLE_RTGC_LOG_VERBOSE) {
            RTGC_dumpRefInfo(root, "not cyclicable");
        }
        // isAcyclic() = true 인 경우:
        //   InitSharedInstance() 실행 후, cyclicTestNode 에 포함된 객체가
        //   Acyclic으로 변경되었거나, acyclic freezing 객체이다.
        DebugRefAssert(root, root->frozen() || !root->freeable() || root->isFreezing());
        return;
    }

    RTGC_LOG("## RTGC c root: %p freezingOnly=%d\n", root, this->checkFreezingOnly);//, rtgcMem->g_cyclicTestNodes.topChain() == NULL ? NULL : rtgcMem->g_cyclicTestNodes.topChain()->obj());
    GCNode* node = root->getNode();
    if (node->getTraceState() != NOT_TRACED) {
        DebugRefAssert(root, node->getTraceState() == TRACE_FINISHED);
        return;
    }

    if (true) {
        //node->setTraceState(TRACE_REQUESTED);
        //tracingList.push(root);
        detectCyclicNodes2(root);        
    }
    else {
        detectCyclicNodes(root);
        assert(tracingList.topChain()->obj() == root);
        assert(tracingList.topChain()->next() == NULL);
        tracingList.clear();
        node = root->getNode();
        RuntimeAssert(node->getTraceState() != TRACE_FINISHED, "Root trace finished");
        node->setTraceState(TRACE_FINISHED);
    }

    // node maybe changed.
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

        RTGC_LOG("## Tracing Obj %p/%d memberRefCnt=%d\n", referrer, referrer->getNodeId(), referrer->getMemberRefCount());
        GCNode* referrer_node = referrer->getNode();

        if (ENABLE_RTGC_LOG) RTGCGlobal::validateMemPool();

        switch (referrer_node->getTraceState()) {
            case NOT_TRACED: // 추적되지 않은 참조 노드
            {
                RTGC_LOG_V("## RTGC Cyclic NOT TRACED");
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