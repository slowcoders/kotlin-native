/*
 * Copyright 2010-2019 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */
#include "Memory.h"
#include "MemoryPrivate.hpp"

// Note that only C++ part of the runtime goes via those functions, Kotlin uses specialized versions.

extern "C" {

const bool IsStrictMemoryModel = false;

OBJ_GETTER(AllocInstance, const TypeInfo* typeInfo) {
  RETURN_RESULT_OF(AllocInstanceRelaxed, typeInfo);
}

OBJ_GETTER(AllocArrayInstance, const TypeInfo* typeInfo, int32_t elements) {
  RETURN_RESULT_OF(AllocArrayInstanceRelaxed, typeInfo, elements);
}

OBJ_GETTER(InitInstance,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*)) {
  RETURN_RESULT_OF(InitInstanceRelaxed, location, typeInfo, ctor);
}

OBJ_GETTER(InitSharedInstance,
    ObjHeader** location, const TypeInfo* typeInfo, void (*ctor)(ObjHeader*)) {
  RETURN_RESULT_OF(InitSharedInstanceRelaxed, location, typeInfo, ctor);
}

void ReleaseRef(const ObjHeader* object) {
  ReleaseRefRelaxed(object);
}

void ZeroStackRef(ObjHeader** location) {
  ZeroStackRefRelaxed(location);
}

void SetStackRef(ObjHeader** location, const ObjHeader* object) {
  SetStackRefRelaxed(location, object);
}

void SetHeapRef(ObjHeader** location, const ObjHeader* object) {
  SetHeapRefRelaxed(location, object);
}

void UpdateStackRef(ObjHeader** location, const ObjHeader* object) {
  UpdateStackRefRelaxed(location, object);
}

void UpdateHeapRef(ObjHeader** location, const ObjHeader* object, const ObjHeader* owner) {
  UpdateHeapRefRelaxed(location, object, owner);
}

void UpdateReturnRef(ObjHeader** returnSlot, const ObjHeader* object) {
  UpdateReturnRefRelaxed(returnSlot, object);
}

void EnterFrame(ObjHeader** start, int parameters, int count) {
  EnterFrameRelaxed(start, parameters, count);
}

void LeaveFrame(ObjHeader** start, int parameters, int count) {
  LeaveFrameRelaxed(start, parameters, count);
}

const ObjHeader* LeaveFrameAndReturnRef(ObjHeader** start, int param_count, ObjHeader** resultSlot, const ObjHeader* returnRef) {
  return LeaveFrameAndReturnRefRelaxed(start, param_count, resultSlot, returnRef);
}

}  // extern "C"
