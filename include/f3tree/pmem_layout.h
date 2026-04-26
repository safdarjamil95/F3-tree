#pragma once

// PMDK-backed persistent layout for F3-tree.
//
// This header defines:
//   - PmemRef: 8-byte pool-relative offset wrapper used in place of raw
//     pointers inside on-pool structures (page::header fields,
//     future_Node link fields, entry.ptr when it addresses a page).
//   - PmemRoot: root schema stored at the pool's libpmemobj root,
//     persisted across process restart.
//   - Pool context globals: one pool per process; set by the btree
//     constructor and consumed by PmemRef conversions.
//
// Only meaningful when F3_PMDK is defined. Under a DRAM build this
// header expands to nothing.

#ifdef F3_PMDK

#include <cstdint>

#include <libpmemobj.h>
#include <libpmemobj++/make_persistent.hpp>
#include <libpmemobj++/make_persistent_array.hpp>
#include <libpmemobj++/mutex.hpp>
#include <libpmemobj++/p.hpp>
#include <libpmemobj++/persistent_ptr.hpp>
#include <libpmemobj++/pool.hpp>
#include <libpmemobj++/transaction.hpp>

// Forward declarations — defined in core/btree.h inside #ifdef F3_PMDK.
class page;
class future_Node;

namespace f3tree {
namespace pmdk {

// Schema version for PmemRoot. Bump whenever PmemRoot / page / future_Node
// on-pool layout changes. Reopening with a mismatched version throws;
// in-place upgrades are not supported.
static constexpr std::uint64_t kPmemSchemaVersion = 1;

// libpmemobj pool layout string. Must be stable across versions of this
// code that are intended to open the same pool.
static constexpr const char *kPmemLayoutName = "f3tree_pool_v1";

// 8-byte pool-relative offset into the currently-open process pool.
// Offset 0 is "null" (libpmemobj reserves offset 0).
//
// PmemRef<T> is a typed ref converted to/from T* via implicit conversion,
// operator->, and assignment. page and future_Node fields each use their
// own instantiation to avoid ambiguous overloads.
struct PmemRefBase {
  std::uint64_t offset;

  PmemRefBase() : offset(0) {}
  explicit PmemRefBase(std::uint64_t o) : offset(o) {}

  static std::uint64_t &pool_uuid();

  static std::uint64_t from_raw(const void *p) {
    if (p == nullptr) return 0;
    PMEMoid oid = pmemobj_oid(p);
    return oid.off;
  }

  void *to_ptr() const {
    if (offset == 0) return nullptr;
    PMEMoid oid;
    oid.pool_uuid_lo = pool_uuid();
    oid.off = offset;
    return pmemobj_direct(oid);
  }

  bool is_null() const { return offset == 0; }
  explicit operator bool() const { return offset != 0; }
  bool operator==(std::nullptr_t) const { return offset == 0; }
  bool operator!=(std::nullptr_t) const { return offset != 0; }
};

template <typename T>
struct PmemRef : public PmemRefBase {
  PmemRef() : PmemRefBase() {}
  explicit PmemRef(std::uint64_t o) : PmemRefBase(o) {}
  PmemRef(std::nullptr_t) : PmemRefBase() {}
  PmemRef(T *p) : PmemRefBase(from_raw(p)) {}

  PmemRef &operator=(std::nullptr_t) { offset = 0; return *this; }
  PmemRef &operator=(T *p) { offset = from_raw(p); return *this; }

  T *get() const { return reinterpret_cast<T *>(to_ptr()); }
  // Implicit conversion to T* resolves comparisons with raw pointers
  // via pointer == pointer; explicit operator==/!=(T*) overloads
  // collided with the nullptr_t overloads on bare NULL / 0 / long 0,
  // so we rely on the conversion path only.
  operator T *() const { return get(); }
  T *operator->() const { return get(); }
  T &operator*() const { return *get(); }
};

// Back-compat alias for code that used the old untyped name. Callers
// must know the target type.
struct PmemRef_Untyped : public PmemRefBase {
  PmemRef_Untyped() : PmemRefBase() {}
  explicit PmemRef_Untyped(std::uint64_t o) : PmemRefBase(o) {}
  page *as_page() const { return reinterpret_cast<page *>(to_ptr()); }
  future_Node *as_future_node() const {
    return reinterpret_cast<future_Node *>(to_ptr());
  }
};

// Pool root schema — returned by pop.root() on open.
// All durable state anchors here.
struct PmemRoot {
  pmem::obj::p<std::uint64_t> schema_version;
  pmem::obj::p<std::uint64_t> seq_counter;      // durable next_ptfo_sequence
  pmem::obj::p<std::uint32_t> n_threads;        // producer count at create time
  pmem::obj::p<std::uint32_t> hash_capacity;    // directory dim at create
  pmem::obj::p<std::uint32_t> height;           // global-tree height
  // Root page of the global B+-tree. Persistent_ptr because this is
  // the canonical 16-byte typed pool pointer, held by the root
  // structure itself — the 8-byte-savings argument for PmemRef
  // doesn't apply to schema fields.
  pmem::obj::persistent_ptr<page> root_page;
  // Per-producer PTFO dummy-head and tail arrays. Dimensioned at pool
  // create time; must not be resized.
  pmem::obj::persistent_ptr<future_Node[]> local_fut;
  pmem::obj::persistent_ptr<future_Node[]> local_fut_tail;
};

using PmemPool = pmem::obj::pool<PmemRoot>;

// Process-global pool accessor. Set exactly once by the btree constructor
// when a persistent backend is selected; consumed by PmemRef conversions
// and by code that needs the pool handle to start transactions.
//
// One pool per process. Constructing a second btree with a persistent_path
// while another is already live throws in the btree constructor.
inline PmemPool *&current_pool() {
  static PmemPool *pool = nullptr;
  return pool;
}

inline std::uint64_t &PmemRefBase::pool_uuid() {
  static std::uint64_t uuid = 0;
  return uuid;
}

// Call exactly once after a successful pool open/create. uuid must be the
// uuid_lo of any oid in the pool (for instance pmemobj_oid(root).pool_uuid_lo).
inline void set_pool_context(PmemPool *pool, std::uint64_t uuid) {
  current_pool() = pool;
  PmemRefBase::pool_uuid() = uuid;
}

// Clear pool context on destructor (so a second btree instance can be
// created, e.g. in tests). Paired with set_pool_context.
inline void clear_pool_context() {
  current_pool() = nullptr;
  PmemRefBase::pool_uuid() = 0;
}

}  // namespace pmdk
}  // namespace f3tree

#endif  // F3_PMDK
