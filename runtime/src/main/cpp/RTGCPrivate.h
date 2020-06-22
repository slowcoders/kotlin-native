#ifndef RTGC_PRIVATE_H
#define RTGC_PRIVATE_H

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

struct RTGCGlobal : GCNode {
  static int cntRefChain;
  static int cntOneWayNodes;
  static int cntCyclicNodes;

  static OnewayNode* g_freeOnewayNode;
  static CyclicNode* g_freeCyclicNode;

  
  static GCRefChain* g_refChains;
  static GCRefChain* g_freeRefChain;

  static void init();

  static bool isInCyclicNode(GCObject* obj) {
    return obj->getNodeId() >= CYCLIC_NODE_ID_START;
  }
};

inline void* GET_NEXT_FREE(void* chain) {
    return *(void**)chain;
}

inline void* SET_NEXT_FREE(void* chain, void* next) {
    return (*(void**)chain = next);
}


#endif // RTGC_PRIVATE_H
