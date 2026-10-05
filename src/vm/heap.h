// Garbage-collected heap: precise, non-moving, stop-the-world mark & sweep.
//
// Roots are supplied by the VM through a callback (value stack, globals,
// frames' closures, open upvalues, string constants, native temp roots).
// Marking uses an explicit gray stack, so deep or cyclic object graphs never
// recurse on the C++ stack. Collection is triggered when the accounted heap
// size passes a threshold that doubles relative to the live size after each
// collection, or on every allocation in stress mode.
#pragma once

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "vm/value.h"

namespace ember::vm {

struct HeapStats {
    size_t collections = 0;
    size_t bytes_allocated_total = 0;
    size_t objects_allocated_total = 0;
    size_t objects_freed_total = 0;
    size_t live_bytes = 0;     // accounted bytes currently allocated
    size_t peak_bytes = 0;
    double gc_seconds = 0;
};

struct OutOfMemory {};

class Heap {
public:
    using RootMarker = std::function<void(Heap&)>;

    Heap(size_t max_bytes, bool stress) : max_bytes_(max_bytes), stress_(stress) {}
    ~Heap();
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    void set_root_marker(RootMarker m) { roots_ = std::move(m); }

    // Allocate a new object. May collect first; all values the caller still
    // needs must be reachable from the roots at this point. Throws
    // OutOfMemory if the limit would be exceeded even after collecting.
    template <typename T, typename... Args>
    T* alloc(size_t payload_bytes, Args&&... args) {
        size_t bytes = sizeof(T) + payload_bytes;
        reserve(bytes);
        T* o = new T(std::forward<Args>(args)...);
        o->bytes = bytes;
        o->next = objects_;
        objects_ = o;
        stats_.live_bytes += bytes;
        stats_.bytes_allocated_total += bytes;
        ++stats_.objects_allocated_total;
        if (stats_.live_bytes > stats_.peak_bytes) stats_.peak_bytes = stats_.live_bytes;
        return o;
    }

    // Account for payload growth of an existing (rooted) object, e.g. an
    // array push. May collect; throws OutOfMemory over the limit.
    void grow(Obj* o, size_t extra) {
        reserve(extra);
        o->bytes += extra;
        stats_.live_bytes += extra;
        stats_.bytes_allocated_total += extra;
        if (stats_.live_bytes > stats_.peak_bytes) stats_.peak_bytes = stats_.live_bytes;
    }

    // Account for payload shrinking (e.g. an array pop).
    void shrink(Obj* o, size_t less) {
        if (less > o->bytes) less = o->bytes;
        o->bytes -= less;
        stats_.live_bytes -= less;
    }

    // Called by the root marker.
    void mark_value(const Value& v) {
        if (v.tag == Tag::Ref && v.o) mark_object(v.o);
    }
    void mark_object(Obj* o) {
        if (!o || o->marked) return;
        o->marked = true;
        gray_.push_back(o);
    }

    void collect();
    const HeapStats& stats() const { return stats_; }
    size_t max_bytes() const { return max_bytes_; }

private:
    void reserve(size_t bytes);
    void trace(Obj* o);
    static void destroy(Obj* o);

    Obj* objects_ = nullptr;
    std::vector<Obj*> gray_;
    RootMarker roots_;
    size_t max_bytes_;
    size_t next_gc_ = 1 << 20;
    bool stress_;
    HeapStats stats_;
};

}  // namespace ember::vm
