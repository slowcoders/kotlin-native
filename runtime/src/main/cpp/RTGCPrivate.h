#ifndef RTGC_PRIVATE_H
#define RTGC_PRIVATE_H

const static int CNT_CYCLIC_NODE = 1000*1000;
const static int CNT_REF_CHAIN = 1000*1000;


struct RTGCGlobal : GCNode {
  static int cntRefChain;
  static int cntCyclicNodes;
  static int g_cntLocalCyclicTest;
  static int g_cntMemberCyclicTest;

  // static CyclicNode* g_freeCyclicNode;
  // static GCRefChain* g_freeRefChain;

  static void validateMemPool();

  static void init(RTGCMemState* state);
};

inline void* GET_NEXT_FREE(void* chain) {
    return *(void**)chain;
}

inline void* SET_NEXT_FREE(void* chain, void* next) {
    return (*(void**)chain = next);
}


#endif // RTGC_PRIVATE_H
