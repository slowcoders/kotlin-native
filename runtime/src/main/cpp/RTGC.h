#ifndef RTGC_H
#define RTGC_H

#include "KAssert.h"
#include "Common.h"
#include "TypeInfo.h"
#include "Atomic.h"

#define RTGC 1

typedef struct ContainerHeader GCObject;

enum GCFlags {
  needGarbageTest = 1,
};

enum TraceState {
  NOT_TRCAED,
  IN_TRACING,
  TRACE_FINISHED,
  OUT_OF_SCOPE
};

struct GCRefChain {
private:
  GCObject* obj_;
  GCRefChain* next_;
public:
  GCObject* obj() { return obj_; }
  GCRefChain* next() { return next_; }
};

struct GCRefList {
private:  
  GCRefChain* first_;
public:
  GCRefChain* first() { return first_; }
  void add(GCObject* obj);
  void remove(GCObject* obj);
};

struct GCNode {
  GCRefList externalReferrers;
  TraceState state;
  bool needCyclicTest;
};

struct CyclicNode : GCNode {
private:  
  int32_t rootObjectCount;
  CyclicNode* nextDamaged;
public:
  GCRefList garbageTestList;

  bool isDamaged() {
    return nextDamaged != 0;
  }

  void markDamaged();
};


#endif // RTGC_H
