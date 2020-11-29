#ifndef RTGC_PRIVATE_H
#define RTGC_PRIVATE_H

#ifdef RTGC

inline bool isValidObjectContainer(ContainerHeader* container) {
  ObjHeader* obj = (ObjHeader*)(container + 1);
  return obj->container() == container;
}


void updateHeapRef_internal(const ObjHeader* object, const ObjHeader* old, const ObjHeader* owner) RTGC_NO_INLINE;
void freeContainer(ContainerHeader* header, int garbageNodeId=-1) RTGC_NO_INLINE;

struct ReferentIterator {
    ObjHeader* ptr;
    union {
        const int32_t* offsets;
        KRef* pItem;
    };
    int idxField;
    bool isArray;

    ReferentIterator(ObjHeader* obj) {
        this->ptr = obj;
        const TypeInfo* typeInfo = obj->type_info();
        this->isArray = typeInfo == theArrayTypeInfo;
        if (isArray) {
            ArrayHeader* array = obj->array();
            idxField = array->count_;
            pItem = ArrayAddressOfElementAt(array, 0);
        }
        else {
            idxField = typeInfo->objOffsetsCount_;
            offsets = typeInfo->objOffsets_;
        }
    }

    KRef next() {
        if (isArray) {
            while (--idxField >= 0) {
                KRef ref = *pItem++;
                if (ref != nullptr) {
                    return ref;
                }
            }
        }
        else {
            while (--idxField >= 0) {
                ObjHeader** location = reinterpret_cast<ObjHeader**>(
                    reinterpret_cast<uintptr_t>(ptr) + *offsets++);
                KRef ref = *location;
                if (ref != nullptr) {
                    return ref;
                }
            }
        }
        return nullptr;
    }
};

#endif

#endif // RTGC_PRIVATE_H
