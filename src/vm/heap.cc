#include "vm/heap.h"

#include <chrono>

namespace ember::vm {

Heap::~Heap() {
    Obj* o = objects_;
    while (o) {
        Obj* n = o->next;
        destroy(o);
        o = n;
    }
}

void Heap::destroy(Obj* o) {
    switch (o->type) {
        case ObjType::String: delete static_cast<StringObj*>(o); break;
        case ObjType::Array: delete static_cast<ArrayObj*>(o); break;
        case ObjType::Struct: delete static_cast<StructObj*>(o); break;
        case ObjType::Enum: delete static_cast<EnumObj*>(o); break;
        case ObjType::Closure: delete static_cast<ClosureObj*>(o); break;
        case ObjType::Upvalue: delete static_cast<UpvalueObj*>(o); break;
    }
}

void Heap::reserve(size_t bytes) {
    if (stress_ || stats_.live_bytes + bytes > next_gc_) collect();
    if (bytes > max_bytes_ || stats_.live_bytes + bytes > max_bytes_) {
        // One more full collection before giving up (stress mode already did).
        if (!stress_) collect();
        if (bytes > max_bytes_ || stats_.live_bytes + bytes > max_bytes_) throw OutOfMemory{};
    }
}

void Heap::trace(Obj* o) {
    switch (o->type) {
        case ObjType::String: break;
        case ObjType::Array:
            for (const Value& v : static_cast<ArrayObj*>(o)->items) mark_value(v);
            break;
        case ObjType::Struct:
            for (const Value& v : static_cast<StructObj*>(o)->fields) mark_value(v);
            break;
        case ObjType::Enum:
            for (const Value& v : static_cast<EnumObj*>(o)->fields) mark_value(v);
            break;
        case ObjType::Closure:
            for (UpvalueObj* u : static_cast<ClosureObj*>(o)->upvalues) mark_object(u);
            break;
        case ObjType::Upvalue: {
            auto* u = static_cast<UpvalueObj*>(o);
            // An open upvalue's stack slot is a root already; a closed one
            // owns its value.
            mark_value(u->closed);
            break;
        }
    }
}

void Heap::collect() {
    auto start = std::chrono::steady_clock::now();
    ++stats_.collections;
    if (roots_) roots_(*this);
    while (!gray_.empty()) {
        Obj* o = gray_.back();
        gray_.pop_back();
        trace(o);
    }
    Obj** link = &objects_;
    while (*link) {
        Obj* o = *link;
        if (o->marked) {
            o->marked = false;
            link = &o->next;
        } else {
            *link = o->next;
            stats_.live_bytes -= o->bytes;
            ++stats_.objects_freed_total;
            destroy(o);
        }
    }
    size_t target = stats_.live_bytes * 2;
    next_gc_ = target < (1u << 20) ? (1u << 20) : target;
    stats_.gc_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace ember::vm
