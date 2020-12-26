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
const bool NO_RECURSIVE_TRACING = true;

class CyclicNodeDetector {
    GCRefList tracingList;
    GCRefList finishedList;
    KStdDeque<char*> destroyedNodes;
    KStdDeque<GCRefChain*> traceStack;
    // GCRefList finishedList_;
public:
    bool checkFreezingOnly;
    CyclicNodeDetector() {}

    GCNode* markInTracing(GCObject* tracingObj) RTGC_NO_INLINE;

    void buildCyclicNode(GCObject* obj)  RTGC_NO_INLINE;
    void addCyclicObject(CyclicNode* targetNode, GCObject* obj)  RTGC_NO_INLINE;

    void traceCyclic(GCObject* root) RTGC_NO_INLINE;

    void checkCyclic(KStdVector<KRef>* freezing) RTGC_NO_INLINE;

    void detectCyclicNodes(GCObject* tracingObj) RTGC_NO_INLINE;

    void cleanUp() RTGC_NO_INLINE;
};


CyclicNode* CyclicNode::create() {
    CyclicNode* node = rtgcMem->cyclicNodeAllocator.allocItem();
    memset(node, 0, sizeof(CyclicNode));
    if (RTGC_STATISTCS) RTGCGlobal::g_cntAddCyclicNode ++;
    RTGC_LOG("## RTGC cyclic node created %p:%d\n", node, node->getId());
    return node;
}

void CyclicNode::dealloc() {
    RTGC_LOG("## RTGC cyclic node dealloc %p:%d\n", this, this->getId());
    //RuntimeAssert(isLocked(), "GCNode is not locked")
    externalReferrers.clear();
    if (RTGC_DEBUG) {
      memset(this, -1, sizeof(CyclicNode));
    }
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
    RTGC_LOG("addCyclicTest %p %p\n", obj, rtgcMem);
    if (obj->enqueueCyclicTest()) {
        rtgcMem->g_cyclicTestNodes.push_back(obj);
    }
    else {
        DebugRefAssert(obj, obj->frozen());
        //konan::consolePrintf("Double enque cyclic test!!!");
    }
}

void CyclicNode::removeCyclicTest(RTGCMemState* rtgcMem, GCObject* obj) {
    DebugAssert(!RTGC_LATE_DESTROY_CYCLIC_SUSPECT);
    if (!obj->isEnquedCyclicTest()) return;

    obj->dequeueCyclicTest();
    RTGC_LOG("## RTGC Remove Cyclic Test %p:%d\n", obj, obj->getNodeId());
    KStdDeque<GCObject*>& q = rtgcMem->g_cyclicTestNodes;
    /* TODO ZZZZ replace tryRemove => remove */
    for (int i = (int)q.size(); --i >= 0; ) {
        if (q[i] == obj) {
            q[i] = q.back();
            q.resize(q.size() - 1);
            break;
        }
    }
}


void CyclicNode::mergeCyclicNode(GCObject* obj, int expiredNodeId) {
    int this_id = this->getId();
    obj->setNodeId(this_id);

    std::deque<ReferentIterator> traceStack;
    traceStack.push_back((ObjHeader*)(obj+1));
    while (!traceStack.empty()) {
        ReferentIterator* it = &traceStack.back();
        ObjHeader* obj = it->next();
        if (obj == nullptr) {
            traceStack.pop_back();
        }
        else if (obj->container()->getNodeId() == expiredNodeId) {
            obj->container()->setNodeId(this_id);
            traceStack.push_back(obj);
        }

    }
    // DebugAssert(obj->getNodeId() == expiredNodeId);
    // RTGC_LOG("      RTGC Merge Cycle %p/%d -> %d\n", obj, expiredNodeId, this->getId());
    // obj->setNodeId(this->getId());
    // assert(obj->isInCyclicNode() && obj->getNode() == this);
    // RTGC_traverseObjectFields(obj, [this, expiredNodeId](GCObject* referent) {
    //     if (referent->getNodeId() == expiredNodeId) {
    //         this->mergeCyclicNode(referent, expiredNodeId);
    //     }
    // });
}

void CyclicNodeDetector::addCyclicObject(
    CyclicNode* targetNode, GCObject* rookie /* 추가 객체*/
) {
    char* destroyed = targetNode->addCyclicObject(rookie);
    if (DELAY_NODE_DESTROY) {
        if (destroyed != nullptr) {
            RTGC_LOG("    push destroyed %p node=%p\n", rookie, destroyed);
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
    
    if (rookieInCyclic) {
        CyclicNode* oldCyclicNode = (CyclicNode*)oldNode;
        //if (RTGC_STATISTCS) konan::consolePrintf("merge cyclic\n");
        this->rootObjectCount += oldCyclicNode->rootObjectCount;
        this->mergeCyclicNode(rookie, oldCyclicNode->getId());
    }
    else {
        rookie->setNodeId(this_id);
        if (rookie->getRootRefCount() > 0) {
            this->rootObjectCount ++;
        }
    }


    for(GCRefChain* chain = oldNode->externalReferrers.topChain(); chain != NULL; chain = chain->next()) {
        GCObject* referrer = chain->obj();
        if (!referrer->isAcyclic() && referrer->getNodeId() != this_id) {
            RTGC_LOG_V("      RTGC add referrer of cyclic: %p -> %d\n", referrer, this_id);
            this->externalReferrers.push(referrer);
        }
    }

    // RTGC_LOG("## RTGC merge external referrers done: %p\n", oldNode->externalReferrers.topChain());
    this->markDirtyReferrerList();

    this->cntCyclicRefs ++;
    RTGC_LOG_V("    RTGC add cyclic obj(%d) done: %p\n", cntCyclicRefs, rookie);
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
    if (ENABLE_RTGC_LOG) {
        RTGC_dumpRefInfo(root, "M1");
        RTGC_dumpRefInfo(rookie, "M2");
    }
    CyclicNode* cyclicNode = root->getLocalCyclicNode();
    CyclicNode* cyclicNode2 = rookie->getLocalCyclicNode();
    if (cyclicNode == nullptr) {
        cyclicNode = cyclicNode2 != nullptr ? cyclicNode2 : CyclicNode::create();
    }
    else if (cyclicNode2 != NULL && cyclicNode->cntCyclicRefs < cyclicNode2->cntCyclicRefs) {
        cyclicNode = cyclicNode2;
    }
    root->markAcyclic();
    rookie->markAcyclic();
    cyclicNode->addCyclicObject(root);
    cyclicNode->addCyclicObject(rookie);
    root->clearAcyclic_unsafe();
    rookie->clearAcyclic_unsafe();
    cyclicNode->clearDirtyReferrers();
    if (!root->isEnquedCyclicTest()) {
        addCyclicTest(rookie, true);
    }

    // CyclicNodeDetector detector;
    // detector.checkFreezingOnly = false;
    // detector.detectCyclicNodes(rookie);
    // detector.cleanUp();
    return cyclicNode;
}


void CyclicNodeDetector::buildCyclicNode(GCObject* referrer) {
    GCNode* referrer_node = referrer->getNode();
    int cnt __attribute__((unused)) = 1;
    
    if (NO_RECURSIVE_TRACING) {
        CyclicNode* cyclicNode = nullptr;

        int start = (int)traceStack.size();
        for (;;) {
            GCObject* obj = traceStack[--start]->obj();
            CyclicNode* cyclic = obj->getLocalCyclicNode();
            DebugAssert(!obj->isAcyclic());
            obj->markAcyclic();
            if (cyclic != nullptr) {
                if (cyclicNode == NULL || cyclicNode->cntCyclicRefs < cyclic->cntCyclicRefs) {
                    cyclicNode = cyclic;
                }
                if (cyclic == referrer_node) {
                    break;
                }
            }
            else if (obj == referrer) {
                break;
            }
        }
        if (cyclicNode == nullptr) {
            cyclicNode = CyclicNode::create();
        }

        for (int idx = (int)traceStack.size(); --idx >= start; ) {
            GCRefChain* pChain = traceStack[idx];
            GCObject* rookie = pChain->obj();
            RTGC_LOG("  Cyclic add %p/%d %d)\n", rookie, cyclicNode->getId(), cnt++);
            addCyclicObject(cyclicNode, rookie);
            rookie->clearAcyclic_unsafe();
        }
        DebugRefAssert(referrer, referrer->getNodeId() == cyclicNode->getId());
        //RTGC_LOG("  Cyclic add last %p/%d:%d\n", referrer, cyclicNode->getId(), cnt);
        //addCyclicObject(cyclicNode, referrer);
        cyclicNode->setTraceState(IN_TRACING);

    }
    else {
        CyclicNode* cyclicNode = referrer->getLocalCyclicNode();
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

        GCRefChain* pChain = tracingList.topChain();
        while (pChain->obj()->getNode() != referrer_node) {
            GCObject* rookie = pChain->obj();
            RTGC_LOG("    RTGC Cyclic add: %d:%d, %p\n", cyclicNode->getId(), cnt++, rookie);
            pChain = pChain->next();
            addCyclicObject(cyclicNode, rookie);
        }
        RTGC_LOG("    RTGC Cyclic add last: %d:%d, %p\n", cyclicNode->getId(), cnt, referrer);
        addCyclicObject(cyclicNode, referrer);
        cyclicNode->setTraceState(IN_TRACING);
        tracingList.setFirst(pChain);
    }

    if (ENABLE_RTGC_LOG_VERBOSE) {
        cnt = 1;
        RTGC_LOG("  rootObjCount of cyclic node: %d cntRoot %d\n", referrer->getNodeId(), 
            ((CyclicNode*)referrer->getNode())->getRootObjectCount());
        RTGC_dumpReferrers(referrer);
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
    RTGC_LOG("## RTGC 0\n");
    GCNode::rtgcLock(_DetectCylcles);
    if (freezing == nullptr) {
        this->checkFreezingOnly = false;
        KStdDeque<GCObject*>* cyclicTestNodes = &rtgcMem->g_cyclicTestNodes;

        while (!cyclicTestNodes->empty()) {
            GCObject* root = cyclicTestNodes->back();
            cyclicTestNodes->pop_back(); 
            RTGC_LOG_V("detect cyclic %p destroyed=%d\n", root, root->isDestroyed());
            DebugAssert(isValidObjectContainer(root));
            DebugRefAssert(root, root->isEnquedCyclicTest() || (!root->freeable() && root->shared()));
            //DebugAssert(cyclicTestNodes->find(root) == nullptr);
            root->dequeueCyclicTest();
            if (RTGC_LATE_DESTROY_CYCLIC_SUSPECT && root->isDestroyed()) {
                ScheduleDestroyContainer((MemoryState*)rtgcMem, root, "in RTGC");
            }
            else {
                DebugRefAssert(root, !root->isDestroyed());
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
            if (root->isEnquedCyclicTest()) {
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

    cleanUp();
    GCNode::rtgcUnlock();
    RTGC_LOG("## RTGC CYCLE DETECTER END %p\n", rtgcMem);

}

void CyclicNodeDetector::cleanUp() {
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
            if (cyclic->isCyclicGarbage()) {
                //if (RTGC_STATISTCS) konan::consolePrintf("## RTGC Garbage Cycle detected in tracing obj:%p node:%p/%d \n", obj_, cyclic, cyclic->getId());
                ::freeContainer(obj_, cyclic->getId());
                cyclic->dealloc();
            }
        }
    }
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

    if (NO_RECURSIVE_TRACING) {
        //node->setTraceState(TRACE_REQUESTED);
        //tracingList.push(root);
        detectCyclicNodes(root);        
        DebugRefAssert(root, root->getNode()->getTraceState() == TRACE_FINISHED);
    }
    else {
        detectCyclicNodes(root);
        DebugAssert(tracingList.topChain()->obj() == root);
        DebugAssert(tracingList.topChain()->next() == NULL);
        tracingList.clear();
        node = root->getNode();
        DebugAssert(node->getTraceState() != TRACE_FINISHED);
        node->setTraceState(TRACE_FINISHED);
        // node maybe changed.
        finishedList.push(root);
    }

}

void CyclicNodeDetector::detectCyclicNodes(GCObject* tracingObj) {

    RTGC_LOG("detectCyclicNodes start: %p(mem:%p)\n", tracingObj, rtgcMem);

    if (NO_RECURSIVE_TRACING) {
        GCRefChain root;
        root.obj_ = tracingObj;
        root.next_ = NULL;
        
        GCRefChain* chain = &root;
        GCRefChain* tmpChain;

        while (true) {
            GCObject* referrer = chain->obj();
            GCNode* referrer_node = referrer->getNode();

            DebugRefAssert(referrer, !referrer->isAcyclic());
            
            switch (referrer_node->getTraceState()) {
                case NOT_TRACED: // 추적되지 않은 참조 노드 
                    if (checkFreezingOnly && !referrer->isFreezing()) break;
                    
                    tmpChain = referrer_node->externalReferrers.topChain();
                    if (tmpChain == NULL) {
                        finishedList.push(referrer);
                        referrer_node->setTraceState(TRACE_FINISHED);
                        break;
                    }

                    RTGC_LOG("RTGC traceStack add: %p(mem:%p)\n", referrer, rtgcMem);
                    referrer_node->setTraceState(IN_TRACING);
                    traceStack.push_back(chain);
                    chain = tmpChain;
                    continue;

                case IN_TRACING: // 순환 경로 발견
                    RTGC_LOG("RTGC Cyclic Found %p:%d\n", referrer, referrer->getNodeId());
                    buildCyclicNode(referrer);
                    referrer_node = referrer->getNode();
                    break;

                case TRACE_FINISHED: // 이미 추적된 참조 노드
                default: 
                    RTGC_LOG("RTGC TRACE_FINISHED Found %p/%d\n", referrer, referrer->getNodeId());
                    break;
            }

            chain = chain->next();
            while (chain == nullptr) {
                if (traceStack.empty()) {
                    return;
                }
                chain = traceStack.back();
                referrer = chain->obj();
                referrer_node = referrer->getNode();
                DebugRefAssert(referrer, referrer_node->getTraceState() != TRACE_FINISHED);
                traceStack.pop_back();                
                if (traceStack.empty() || referrer_node != traceStack.back()->obj()->getNode()) {
                    // tracingObj와 referent 를 경유하는 순환 경로가 발견되지 않은 경우
                    RTGC_LOG("## RTGC traceStack remove: %p/%d\n", referrer, referrer->getNodeId());
                    finishedList.push(referrer);
                    referrer_node->setTraceState(TRACE_FINISHED);
                }
                chain = chain->next();
            }
        }
    }
    else {
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
                    referrer_node = referrer->getNode();
                    if (tracingObj->getNode() != referrer_node) {
                        // tracingObj와 referent 를 경유하는 순환 경로가 발견되지 않은 경우
                        RTGC_LOG("## RTGC tracingList remove: %p, %p\n", referrer, tracingObj);
                        tracingList.moveTo(referrer, &finishedList);
                        referrer_node->setTraceState(TRACE_FINISHED);
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
}