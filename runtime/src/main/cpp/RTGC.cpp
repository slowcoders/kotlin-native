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

static CyclicNode* gDamagedCylicNodes = NULL;
static GCNode* gCyclicTestNodes = NULL;

void GCRefList::add(GCObject* item) {

}

void GCRefList::remove(GCObject* item) {

}

void CyclicNode::markDamaged() {

}