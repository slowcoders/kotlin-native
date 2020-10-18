#ifndef RTGC_PRIVATE_H
#define RTGC_PRIVATE_H

const static int CNT_CYCLIC_NODE = 1000*1000;
const static int CNT_REF_CHAIN = 1000*1000;


struct RTGCGlobal : GCNode {
  static int g_cntAddRefChain;
  static int g_cntRemoveRefChain;
  static int g_cntAddCyclicNode;
  static int g_cntRemoveCyclicNode;
  static int g_cntAddCyclicTest;
  static int g_cntRemoveCyclicTest;

  static void validateMemPool();

  static void init(RTGCMemState* state);
};



#endif // RTGC_PRIVATE_H
