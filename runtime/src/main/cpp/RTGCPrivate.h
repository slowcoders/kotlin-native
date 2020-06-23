#ifndef RTGC_PRIVATE_H
#define RTGC_PRIVATE_H

const static int CNT_CYCLIC_NODE = 1000*1000;
const static int CNT_REF_CHAIN = 1000*1000;


struct RTGCGlobal : GCNode {
  static int cntRefChain;
  static int cntCyclicNodes;

  static CyclicNode* g_freeCyclicNode;

  static GCRefChain* g_freeRefChain;

  static void init();

};

inline void* GET_NEXT_FREE(void* chain) {
    return *(void**)chain;
}

inline void* SET_NEXT_FREE(void* chain, void* next) {
    return (*(void**)chain = next);
}


#endif // RTGC_PRIVATE_H
