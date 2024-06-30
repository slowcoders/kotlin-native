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
    enum Type {
        Object,
        Array,
        Chain
    };

    union {
        ObjHeader* ptr;
        CyclicNode* cylicNode;
    };
    union {
        const int32_t* offsets;
        KRef* pItem;
        GCRefChain* chain;
    };
    int idxField;
    Type type;

    ReferentIterator(CyclicNode* cylicNode) {
        this->cylicNode = cylicNode;
        this->chain = cylicNode->getGarbageTestList()->topChain();
        this->type = Chain;
    }

    ReferentIterator(ObjHeader* obj) {
        this->ptr = obj;
        const TypeInfo* typeInfo = obj->type_info();
        this->type = typeInfo == theArrayTypeInfo ? Array : Object;
        if (this->type == Array) {
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
        switch (this->type) {
        case Array:
            while (--idxField >= 0) {
                KRef ref = *pItem++;
                if (ref != nullptr) {
                    return ref;
                }
            }
            break;
        case Object:
            while (--idxField >= 0) {
                ObjHeader** location = reinterpret_cast<ObjHeader**>(
                    reinterpret_cast<uintptr_t>(ptr) + *offsets++);
                KRef ref = *location;
                if (ref != nullptr) {
                    return ref;
                }
            }
            break;
        case Chain:
            GCRefChain* c = this->chain;
            while (c != nullptr) {
                ContainerHeader* container = c->obj();
                c = c->next();
                if (!container->isDestroyed()) {
                    this->chain = c;
                    KRef ref = (KRef)(container + 1);
                    konan::consolePrintf("pop supected garbage in cycle %p\n", container);
                    return ref;
                }
            }
            this->chain = nullptr;
            cylicNode->dealloc();
        }
        return nullptr;
    }
};

#endif

#endif // RTGC_PRIVATE_H
