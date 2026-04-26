/*
   Copyright (c) 2018, UNIST. All rights reserved.  The license is a free
   non-exclusive, non-transferable license to reproduce, use, modify and display
   the source code version of the Software, with or without modifications solely
   for non-commercial research, educational or evaluation purposes. The license
   does not entitle Licensee to technical support, telephone assistance,
   enhancements or updates to the Software. All rights, title to and ownership
   interest in the Software, including all intellectual property rights therein
   shall remain in UNIST.

   Please use at your own risk.
*/

#include <cassert>
#include <climits>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <vector>
#include "hash.h"

// Optional PMDK backend. When F3_PMDK is defined
// the page and future_Node types live in a libpmemobj pool at a path
// supplied via RuntimeConfig::persistent_path. Their layout uses 8-byte
// pool-relative offsets (PmemRef) in place of raw pointers and
// pmem::obj::mutex in place of std::mutex so reopening the pool in a
// fresh process yields a consistent tree. Under a non-PMDK build this
// header is a no-op.
#include "f3tree/pmem_layout.h"

#define PAGESIZE 512

#define CPU_FREQ_MHZ (1994)
#define DELAY_IN_NS (1000)
#define CACHE_LINE_SIZE 64
#define QUERY_NUM 25

#define IS_FORWARD(c) (c % 2 == 0)

using entry_key_t = int64_t;

pthread_mutex_t print_mtx;

static inline void cpu_pause() { __asm__ volatile("pause" ::: "memory"); }
static inline unsigned long read_tsc(void) {
  unsigned long var;
  unsigned int hi, lo;

  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  var = ((unsigned long long int)hi << 32) | lo;

  return var;
}

// Simulated write latency is machine-wide (the F3-tree design
// models hardware NVM latency, not per-tree state), but the previous
// non-atomic global was a genuine data race when two tree instances were
// constructed concurrently. Making it std::atomic removes the UB and lets
// apply_legacy_config safely overwrite the value from any thread. We
// intentionally do NOT make latency per-instance; that is a
// larger refactor tracked as follow-up.
std::atomic<unsigned long> write_latency_in_ns{0};
unsigned long long search_time_in_insert = 0;
unsigned int gettime_cnt = 0;
unsigned long long clflush_time_in_insert = 0;
unsigned long long update_time_in_insert = 0;
int clflush_cnt = 0;
int node_cnt = 0;

using namespace std;

inline void mfence() { asm volatile("mfence" ::: "memory"); }

inline void clflush(char *data, int len) {
  volatile char *ptr = (char *)((unsigned long)data & ~(CACHE_LINE_SIZE - 1));
  // Snapshot the simulated write latency once per call to avoid torn reads.
  const unsigned long latency_ns =
      write_latency_in_ns.load(std::memory_order_relaxed);
  mfence();
  for (; ptr < data + len; ptr += CACHE_LINE_SIZE) {
    unsigned long etsc =
        read_tsc() + (unsigned long)(latency_ns * CPU_FREQ_MHZ / 1000);
    asm volatile("clflush %0" : "+m"(*(volatile char *)ptr));
    while (read_tsc() < etsc)
      cpu_pause();
    //++clflush_cnt;
  }
  mfence();
}

class page;
class future_Node;

// Persistent-pointer typedefs.
//
// Under F3_PMDK the B+-tree page header and the PTFO future_Node use
// 8-byte pool-relative offsets (PmemRef) instead of raw pointers, and
// embed pmem::obj::mutex (restart-safe) instead of std::mutex. Under a
// non-PMDK build the same field names refer to raw pointers and
// std::mutex, so the shared code body reads identically.
//
// Implicit conversions on PmemRef (see include/f3tree/pmem_layout.h)
// make `hdr.leftmost_ptr = some_page;`, `page *p = hdr.leftmost_ptr;`,
// `hdr.leftmost_ptr->member`, `if (hdr.leftmost_ptr == NULL)` work
// unchanged across both builds.
#ifdef F3_PMDK
using page_link_t = f3tree::pmdk::PmemRef<page>;
using future_link_t = f3tree::pmdk::PmemRef<future_Node>;
using entry_ptr_t = std::uint64_t;
#else
using page_link_t = page *;
using future_link_t = future_Node *;
using entry_ptr_t = char *;
#endif

// Page header mutex. We use std::mutex unconditionally
// (not pmem::obj::mutex) because pmem::obj::mutex refuses to operate on
// storage that is not currently inside a libpmemobj pool, which would
// preclude the DRAM fallback path when persistent_path is empty under
// an F3_PMDK build. Cross-restart safety is handled by running
// placement-new on every header's mutex during pool recovery (see the
// reopen pass in btree::btree). std::mutex's default constructor re-initializes
// the internal pthread state, which is exactly the post-crash reset the
// mutex needs.
using page_mutex_t = std::mutex;

// Construct an entry_ptr_t from a page pointer (for internal-node
// records) or from a key value (for leaf records). Paired with
// entry_ptr_as_page() / entry_ptr_as_value() for reads.
inline entry_ptr_t make_entry_ptr_from_page(page *p) {
#ifdef F3_PMDK
  return f3tree::pmdk::PmemRefBase::from_raw(p);
#else
  return reinterpret_cast<entry_ptr_t>(p);
#endif
}

inline entry_ptr_t make_entry_ptr_from_value(entry_key_t k) {
#ifdef F3_PMDK
  return static_cast<entry_ptr_t>(k);
#else
  return reinterpret_cast<entry_ptr_t>(k);
#endif
}

inline page *entry_ptr_as_page(entry_ptr_t v) {
#ifdef F3_PMDK
  f3tree::pmdk::PmemRef_Untyped ref(v);
  return ref.as_page();
#else
  return reinterpret_cast<page *>(v);
#endif
}

inline entry_key_t entry_ptr_as_value(entry_ptr_t v) {
#ifdef F3_PMDK
  return static_cast<entry_key_t>(v);
#else
  return reinterpret_cast<entry_key_t>(v);
#endif
}

inline bool entry_ptr_is_null(entry_ptr_t v) {
#ifdef F3_PMDK
  return v == 0;
#else
  return v == NULL;
#endif
}

inline entry_ptr_t entry_ptr_null() {
#ifdef F3_PMDK
  return 0;
#else
  return NULL;
#endif
}

// Convert a char* API parameter to entry_ptr_t. Under DRAM
// this is an identity cast; under PMDK we reinterpret the bit pattern,
// assuming the caller is passing either (char*)key for leaf values or
// (char*)page_ptr for internal-node values. The only call site that
// passes a page pointer as char* is btree_insert_internal, which we
// handle separately below.
inline entry_ptr_t char_ptr_to_entry_ptr(char *p) {
#ifdef F3_PMDK
  return reinterpret_cast<entry_ptr_t>(p);
#else
  return p;
#endif
}

// Symmetric conversion back to char* (for tmp_ptr variables in
// linear_search). Under PMDK these vars must be typed entry_ptr_t, not
// char*, so this helper is only for the narrow set of places where the
// original code compared entry_ptr_t to NULL via a char* temp.
inline char *entry_ptr_to_char_ptr(entry_ptr_t p) {
#ifdef F3_PMDK
  return reinterpret_cast<char *>(p);
#else
  return p;
#endif
}

class header {
private:
  // page_link_t is PmemRef under F3_PMDK (8B offset)
  // and page* under the DRAM build (8B pointer). Identical size in both
  // builds; implicit conversions keep call sites unchanged.
  page_link_t leftmost_ptr;
  page_link_t sibling_ptr;
  uint32_t level;         // 4 bytes
  uint8_t switch_counter; // 1 bytes
  uint8_t is_deleted;     // 1 bytes
  int16_t last_index;     // 2 bytes
  // The mutex is embedded (not a pointer) so its storage
  // is pool-resident under PMDK and so DRAM and PM share a single lock
  // call shape. pmem::obj::mutex is reinitialized on pool open.
  page_mutex_t mtx;

  friend class page;
  friend class btree;

public:
  header() {
    // Use nullptr so the compiler picks the
    // unambiguous operator=(nullptr_t) overload on PmemRef; bare NULL
    // (= __null / long) is ambiguous between the nullptr and T* paths.
    leftmost_ptr = nullptr;
    sibling_ptr = nullptr;
    switch_counter = 0;
    last_index = -1;
    is_deleted = false;
    // mtx is default-constructed (std::mutex or pmem::obj::mutex).
  }

  // No destructor needed: mtx destructs automatically; no `new`-allocated
  // state remains after the mtx-pointer removal above.
};

class entry {
private:
  entry_key_t key;  // 8 bytes
  // Under F3_PMDK this is a pool-relative offset
  // (when the page is internal) or the leaf value itself (when the
  // page is a leaf). Discriminant: the enclosing page is a leaf iff
  // its header's leftmost_ptr is null. Under the DRAM build this
  // remains a char* carrying the same punned encoding.
  entry_ptr_t ptr;

public:
  entry() {
    key = LONG_MAX;
    ptr = entry_ptr_null();
  }

  friend class page;
  friend class btree;
};

const int cardinality = (PAGESIZE - sizeof(header)) / sizeof(entry);
const int count_in_line = CACHE_LINE_SIZE / sizeof(entry);

class future_Node{
    public:
        int64_t keys[cardinality];
        bool is_delete[cardinality];
        uint64_t sequence[cardinality];
        int entry_count;
        bool is_done;
        // Link pointers become PmemRef<future_Node>
        // under F3_PMDK (8-byte pool-relative offsets) so the doubly-linked
        // PTFO chain survives process restart. Implicit conversions keep
        // call sites unchanged.
        future_link_t next;
        future_link_t prev;

        future_Node(){
            keys[0] = 0;
            is_delete[0] = false;
            sequence[0] = 0;
            entry_count = 0;
            is_done = false;
            next = nullptr;
            prev = nullptr;
        }

#ifdef F3_PMDK
        // Route all `new future_Node()` through the
        // currently-open pool. make_persistent<future_Node> uses placement
        // new, but producer-side future_insert uses plain `new future_Node()`
        // which must hit pmemobj_alloc so the node lives in pool memory
        // and its PmemRef links are valid across restart.
        void *operator new(std::size_t size) {
          if (f3tree::pmdk::current_pool() == nullptr) {
            throw std::runtime_error(
                "F3_PMDK future_Node allocation requires an open pool");
          }
          PMEMoid oid;
          if (pmemobj_tx_stage() == TX_STAGE_WORK) {
            oid = pmemobj_tx_alloc(size, 1);  // type_num 1 distinguishes future_Node from page
            if (OID_IS_NULL(oid)) {
              throw std::runtime_error("pmemobj_tx_alloc failed for future_Node");
            }
          } else {
            PMEMobjpool *pop = f3tree::pmdk::current_pool()->handle();
            int rc = pmemobj_alloc(pop, &oid, size, 1, NULL, NULL);
            if (rc != 0 || OID_IS_NULL(oid)) {
              throw std::runtime_error("pmemobj_alloc failed for future_Node");
            }
          }
          return pmemobj_direct(oid);
        }
        void operator delete(void *p) {
          if (p == nullptr) return;
          if (f3tree::pmdk::current_pool() == nullptr) return;
          PMEMoid oid = pmemobj_oid(p);
          if (!OID_IS_NULL(oid)) {
            pmemobj_free(&oid);
          }
        }
        // Array new/delete — used by btree constructor to allocate
        // local_fut[n_threads] dummy heads when not using persistent_ptr
        // make_persistent_array. We keep array-new routed through the
        // same pool for symmetry.
        void *operator new[](std::size_t size) {
          if (f3tree::pmdk::current_pool() == nullptr) {
            throw std::runtime_error(
                "F3_PMDK future_Node[] allocation requires an open pool");
          }
          PMEMoid oid;
          PMEMobjpool *pop = f3tree::pmdk::current_pool()->handle();
          int rc = pmemobj_alloc(pop, &oid, size, 1, NULL, NULL);
          if (rc != 0 || OID_IS_NULL(oid)) {
            throw std::runtime_error("pmemobj_alloc failed for future_Node[]");
          }
          return pmemobj_direct(oid);
        }
        void operator delete[](void *p) {
          if (p == nullptr) return;
          if (f3tree::pmdk::current_pool() == nullptr) return;
          PMEMoid oid = pmemobj_oid(p);
          if (!OID_IS_NULL(oid)) {
            pmemobj_free(&oid);
          }
        }
        // Placement-new so make_persistent<future_Node> works.
        void *operator new(std::size_t, void *p) { return p; }
        void operator delete(void *, void *) {}
#endif

        friend class btree;
};

static inline void touch_future_node_allocation(future_Node *node) {
  if (node == NULL) {
    return;
  }

  volatile char *bytes = reinterpret_cast<volatile char *>(node);
  for (std::size_t offset = 0; offset < sizeof(future_Node); offset += 4096) {
    bytes[offset] = 0;
  }
  bytes[sizeof(future_Node) - 1] = 0;
}

enum future_entry_state {
  FUTURE_ABSENT = 0,
  FUTURE_PRESENT = 1,
  FUTURE_TOMBSTONE = 2,
};

struct future_buffered_op {
  entry_key_t key;
  bool is_delete;
  uint64_t sequence;
  int producer_id;
};

struct future_buffered_node {
  std::vector<future_buffered_op> ops;
  uint64_t first_sequence;
  uint64_t last_sequence;
  int producer_id;

  future_buffered_node() : first_sequence(0), last_sequence(0), producer_id(-1) {}
};

struct future_lookup_slot {
  bool occupied;
  entry_key_t key;
  int owner;
  future_entry_state state;
  uint64_t sequence;

  future_lookup_slot()
      : occupied(false),
        key(0),
        owner(-1),
        state(FUTURE_ABSENT),
        sequence(0) {}
};

struct future_lookup_entry {
  entry_key_t key;
  int owner;
  future_entry_state state;
  uint64_t sequence;
};


class btree {
private:
  int height;
  // root is a typed page*. Under DRAM this is a raw
  // pointer; under F3_PMDK the root identity lives in PmemRoot::root_page
  // and this field caches the direct pointer for fast access. setNewRoot
  // keeps both in sync.
  page *root;
#ifdef F3_PMDK
  // Pool handle owned by this btree instance.
  // Open on construction (create if file missing); close on destructor.
  // The process-global pool context (f3tree::pmdk::current_pool) points
  // at this handle between construction and destruction.
  f3tree::pmdk::PmemPool pool_handle_;
  std::string pool_path_;
  bool pool_auto_unlink_ = false;  // unlink the file on destructor
#endif
  future_Node *local_fut;
  future_Node *local_fut_tail;
  HashMapTable *hash;
  // Per-producer lookup directories. future_lookup_directory[tid] is owned
  // exclusively by producer tid: only producer tid ever writes to it, so
  // inserts need only acquire lookup_dir_mu[tid] — not the global tree lock.
  // Readers (contains, buffered_owner) acquire per-producer locks individually.
  std::vector<std::vector<future_lookup_slot>> future_lookup_directory;
  std::mutex *lookup_dir_mu;
  // Per-producer PTFO linked-list mutex.
  // Serializes any access to local_fut[tid] / local_fut_tail[tid] /
  // future_Node chain between the producer thread (future_insert) and the
  // evaluator thread (future_collect_*, future_clear_thread,
  // future_drain_thread_from_tail, future_hash_directed_state_via_ptfo).
  // Lock order: ptfo_mu[tid] is OUTSIDE lookup_dir_mu[tid] when both are
  // acquired (see future_insert). ptfo_mu[tid] must never be held while
  // acquiring another producer's ptfo_mu — the wrapper-level drain sorts
  // pids and acquires all locks in ascending order before replaying.
  std::mutex *ptfo_mu;
  int n_threads;
  int eval_threads;

  std::size_t future_lookup_index(entry_key_t, int tid, bool);

public:
  bool is_Done = false;
  // Constructor accepts a persistent path
  // (F3_PMDK builds only). Empty path under F3_PMDK auto-creates a
  // temporary tmpfs pool at /dev/shm and unlinks it on destruction,
  // preserving pre-R6 test semantics while still exercising the PM
  // allocation and recovery code paths. Under a non-PMDK build the
  // last two parameters are ignored.
  btree(int num_threads, int num_eval_threads, int hash_capacity,
        unsigned long write_latency_ns = 0,
        const std::string &persistent_path = "",
        std::uint64_t persistent_pool_bytes = 0);
  ~btree();
  // setNewRoot / btree_insert_internal / btree_delete_internal
  // carry typed page* instead of char*. Internal pointer semantics
  // must be unambiguous so the PMDK path can encode them as PmemRef
  // offsets rather than raw-cast char*.
  void setNewRoot(page *);
  void getNumberOfNodes();
  void btree_insert(entry_key_t, char *);
  void btree_insert_internal(page *, entry_key_t, page *, uint32_t);
  void btree_delete(entry_key_t);
  void btree_delete_internal(entry_key_t, page *, uint32_t, entry_key_t *,
                             bool *, page **);
  char *btree_search(entry_key_t);
  void btree_search_range(entry_key_t, entry_key_t, unsigned long *);
  void printAll();
  void future_insert(entry_key_t, int, bool, uint64_t = 0);
  future_entry_state future_state(entry_key_t, int);
  future_entry_state future_state_any(entry_key_t);
  future_entry_state future_latest_state(entry_key_t, int * = NULL, uint64_t * = NULL);
  future_entry_state future_lookup_state(entry_key_t, int * = NULL, uint64_t * = NULL);
  future_entry_state future_hash_directed_state(entry_key_t, int * = NULL, uint64_t * = NULL);
  // Hierarchical search via direct PTFO
  // linked-list traversal. Tier 1 = hash[tid]; tier 2 = scan local_fut[tid]
  // under ptfo_mu[tid]; caller handles tier 3 fallback to the global tree.
  future_entry_state future_hash_directed_state_via_ptfo(entry_key_t,
                                                          int * = NULL,
                                                          uint64_t * = NULL);
  int future_hash_directed_owner_via_ptfo(entry_key_t);
  int future_lookup_owner(entry_key_t);
  int future_hash_directed_owner(entry_key_t);
  void future_collect_lookup_entries(std::vector<future_lookup_entry> *);
  void future_collect_ops(int, std::vector<future_buffered_op> *);
  void future_collect_nodes(int, std::vector<future_buffered_node> *);
  void future_clear_thread(int);
  void future_clear_lookup_entries_for_producers(const std::vector<int> &);
  void future_clear_lookup_directory();
  void future_evaluate(btree *, int);
  void future_evaluate_execute(btree *, int, int);
  int future_drain_thread_from_tail(int tid);

  // Expose the per-producer PTFO mutex so wrapper-level
  // drain can hold it across a (collect -> apply -> clear) sequence without
  // introducing a second lock layer. Producers and lookup paths continue to
  // lock internally (they only need the lock for one call at a time).
  std::mutex &ptfo_mutex(int tid) { return ptfo_mu[tid]; }

  // After pool reopen + recovery scan, report the
  // sequence number the wrapper should continue from. The returned value
  // is strictly greater than every durable PTFO-entry sequence observed
  // at reopen. Under a non-PMDK build this is always 1.
  std::uint64_t seq_counter_resume() {
#ifdef F3_PMDK
    return pool_handle_.root()->seq_counter;
#else
    return 1;
#endif
  }

  // Snapshot the wrapper atomic into the durable
  // seq_counter after each drain completes. Called from the wrapper's
  // drain paths. Under a non-PMDK build this is a no-op.
  void persist_seq_counter(std::uint64_t v) {
#ifdef F3_PMDK
    auto root_ref = pool_handle_.root();
    root_ref->seq_counter = v;
    pool_handle_.persist(&root_ref->seq_counter,
                         sizeof(root_ref->seq_counter));
#else
    (void)v;
#endif
  }

  friend class page;
  friend class HashMapTable;
};

class page {
private:
  header hdr;                 // header in persistent memory, 16 bytes
  entry records[cardinality]; // slots in persistent memory, 16 bytes * n

public:
  friend class btree;

  page(uint32_t level = 0) {
    hdr.level = level;
    records[0].ptr = entry_ptr_null();
  }

  // this is called when tree grows
  page(page *left, entry_key_t key, page *right, uint32_t level = 0) {
    hdr.leftmost_ptr = left;
    hdr.level = level;
    records[0].key = key;
    // Encode the child page pointer as a pool offset
    // under PMDK; identity cast under DRAM.
    records[0].ptr = make_entry_ptr_from_page(right);
    records[1].ptr = entry_ptr_null();

    hdr.last_index = 0;

    clflush((char *)this, sizeof(page));
  }

#ifdef F3_PMDK
  // Under F3_PMDK, `new page(...)` routes to the
  // currently-open libpmemobj pool. The global pool is established by
  // btree::btree before any `new page` call. Allocation is
  // atomic (pmemobj_alloc) when no transaction is active, or transactional
  // (pmemobj_tx_alloc) when invoked inside a pmem::obj::transaction::run.
  void *operator new(std::size_t size) {
    if (f3tree::pmdk::current_pool() == nullptr) {
      throw std::runtime_error(
          "F3_PMDK page allocation requires an open libpmemobj pool "
          "(RuntimeConfig::persistent_path must be non-empty)");
    }
    PMEMoid oid;
    if (pmemobj_tx_stage() == TX_STAGE_WORK) {
      oid = pmemobj_tx_alloc(size, 0);
      if (OID_IS_NULL(oid)) {
        throw std::runtime_error("pmemobj_tx_alloc failed for page");
      }
    } else {
      PMEMobjpool *pop = f3tree::pmdk::current_pool()->handle();
      int rc = pmemobj_alloc(pop, &oid, size, 0, NULL, NULL);
      if (rc != 0 || OID_IS_NULL(oid)) {
        throw std::runtime_error("pmemobj_alloc failed for page");
      }
    }
    return pmemobj_direct(oid);
  }

  void operator delete(void *p) {
    if (p == nullptr) return;
    if (f3tree::pmdk::current_pool() == nullptr) {
      // Pool has already been closed; nothing to do — the OS reclaims
      // everything on process exit and libpmemobj persisted state is
      // still consistent.
      return;
    }
    PMEMoid oid = pmemobj_oid(p);
    if (!OID_IS_NULL(oid)) {
      pmemobj_free(&oid);
    }
  }

  // Placement-new overload required by pmem::obj::make_persistent<T> which
  // allocates storage via libpmemobj and then constructs the object in
  // place via `new (ptr) T(...)`. Returns the provided pointer unchanged.
  void *operator new(std::size_t, void *p) { return p; }
  void operator delete(void *, void *) {}
#else
  // Under DRAM: cache-line-aligned allocation via posix_memalign.
  void *operator new(std::size_t size) {
    void *ret;
    posix_memalign(&ret, 64, size);
    return ret;
  }
#endif

  inline int count() {
    uint8_t previous_switch_counter;
    int count = 0;
    do {
      previous_switch_counter = hdr.switch_counter;
      count = hdr.last_index + 1;

      while (count >= 0 && !entry_ptr_is_null(records[count].ptr)) {
        if (IS_FORWARD(previous_switch_counter))
          ++count;
        else
          --count;
      }

      if (count < 0) {
        count = 0;
        while (!entry_ptr_is_null(records[count].ptr)) {
          ++count;
        }
      }

    } while (previous_switch_counter != hdr.switch_counter);

    return count;
  }

  inline bool remove_key(entry_key_t key) {
    // Comprehensive clflush coverage for the delete path.
    // The previous implementation (a) did not flush the switch_counter
    // update, (b) used a cache-line-alignment heuristic that frequently
    // skipped the per-entry flush, and (c) did not flush hdr.last_index
    // after the decrement. Recovery therefore could observe a page whose
    // entries had been shifted but whose last_index / switch_counter were
    // not yet persistent.  See docs/PERSISTENCE_INVARIANTS.md
    // "Buffered deletes after drain".

    // Set the switch_counter (readers use this to detect in-flight shifts).
    if (IS_FORWARD(hdr.switch_counter))
      ++hdr.switch_counter;
    clflush((char *)&hdr.switch_counter, sizeof(uint8_t));

    bool shift = false;
    int i;
    for (i = 0; !entry_ptr_is_null(records[i].ptr); ++i) {
      if (!shift && records[i].key == key) {
        // Overwrite the deleted slot's ptr with the previous slot's ptr
        // (or leftmost_ptr for i==0). Persist the overwrite before the
        // following shifts so the page-level invariant
        // "records[i].ptr != previous.ptr iff (key,ptr) is live" holds at
        // every durable prefix.
        records[i].ptr =
            (i == 0) ? make_entry_ptr_from_page(hdr.leftmost_ptr) : records[i - 1].ptr;
        clflush((char *)&records[i].ptr, sizeof(entry_ptr_t));
        shift = true;
      }

      if (shift) {
        records[i].key = records[i + 1].key;
        records[i].ptr = records[i + 1].ptr;
        // Unconditional flush of the shifted entry. The old heuristic
        // flushed only when the entry happened to straddle a cache line
        // boundary, which regularly missed updates entirely.
        clflush((char *)&records[i], sizeof(entry));
      }
    }

    if (shift) {
      --hdr.last_index;
      clflush((char *)&hdr.last_index, sizeof(int16_t));
    }
    return shift;
  }

  bool remove(btree *bt, entry_key_t key, bool only_rebalance = false,
              bool with_lock = true) {
    hdr.mtx.lock();

    bool ret = remove_key(key);

    hdr.mtx.unlock();

    return ret;
  }

  /*
   * Although we implemented the rebalancing of B+-Tree, it is currently blocked
   * for the performance. Please refer to the follow. Chi, P., Lee, W. C., &
   * Xie, Y. (2014, August). Making B+-tree efficient in PCM-based main memory.
   * In Proceedings of the 2014 international symposium on Low power electronics
   * and design (pp. 69-74). ACM.
   */
  bool remove_rebalancing(btree *bt, entry_key_t key,
                          bool only_rebalance = false, bool with_lock = true) {
    if (with_lock) {
      hdr.mtx.lock();
    }
    if (hdr.is_deleted) {
      if (with_lock) {
        hdr.mtx.unlock();
      }
      return false;
    }

    if (!only_rebalance) {
      register int num_entries_before = count();

      // This node is root
      if (this == (page *)bt->root) {
        if (hdr.level > 0) {
          if (num_entries_before == 1 && !hdr.sibling_ptr) {
            bt->root = hdr.leftmost_ptr;
            clflush((char *)&(bt->root), sizeof(page *));

            hdr.is_deleted = 1;
          }
        }

        // Remove the key from this node
        bool ret = remove_key(key);

        if (with_lock) {
          hdr.mtx.unlock();
        }
        return true;
      }

      bool should_rebalance = true;
      // check the node utilization
      if (num_entries_before - 1 >= (int)((cardinality - 1) * 0.5)) {
        should_rebalance = false;
      }

      // Remove the key from this node
      bool ret = remove_key(key);

      if (!should_rebalance) {
        if (with_lock) {
          hdr.mtx.unlock();
        }
        return (hdr.leftmost_ptr == NULL) ? ret : true;
      }
    }

    // Remove a key from the parent node
    entry_key_t deleted_key_from_parent = 0;
    bool is_leftmost_node = false;
    page *left_sibling;
    bt->btree_delete_internal(key, this, hdr.level + 1,
                              &deleted_key_from_parent, &is_leftmost_node,
                              &left_sibling);

    if (is_leftmost_node) {
      if (with_lock) {
        hdr.mtx.unlock();
      }

      if (!with_lock) {
        hdr.sibling_ptr->hdr.mtx.lock();
      }
      hdr.sibling_ptr->remove(bt, hdr.sibling_ptr->records[0].key, true,
                              with_lock);
      if (!with_lock) {
        hdr.sibling_ptr->hdr.mtx.unlock();
      }
      return true;
    }

    if (with_lock) {
      left_sibling->hdr.mtx.lock();
    }

    while (left_sibling->hdr.sibling_ptr != this) {
      if (with_lock) {
        page *t = left_sibling->hdr.sibling_ptr;
        left_sibling->hdr.mtx.unlock();
        left_sibling = t;
        left_sibling->hdr.mtx.lock();
      } else
        left_sibling = left_sibling->hdr.sibling_ptr;
    }

    register int num_entries = count();
    register int left_num_entries = left_sibling->count();

    // Merge or Redistribution
    int total_num_entries = num_entries + left_num_entries;
    if (hdr.leftmost_ptr)
      ++total_num_entries;

    entry_key_t parent_key;

    if (total_num_entries > cardinality - 1) { // Redistribution
      register int m = (int)ceil(total_num_entries / 2);

      if (num_entries < left_num_entries) { // left -> right
        if (hdr.leftmost_ptr == nullptr) {
          for (int i = left_num_entries - 1; i >= m; i--) {
            insert_key(left_sibling->records[i].key,
                       left_sibling->records[i].ptr, &num_entries);
          }

          left_sibling->records[m].ptr = entry_ptr_null();
          clflush((char *)&(left_sibling->records[m].ptr), sizeof(entry_ptr_t));

          left_sibling->hdr.last_index = m - 1;
          clflush((char *)&(left_sibling->hdr.last_index), sizeof(int16_t));

          parent_key = records[0].key;
        } else {
          insert_key(deleted_key_from_parent, make_entry_ptr_from_page(hdr.leftmost_ptr),
                     &num_entries);

          for (int i = left_num_entries - 1; i > m; i--) {
            insert_key(left_sibling->records[i].key,
                       left_sibling->records[i].ptr, &num_entries);
          }

          parent_key = left_sibling->records[m].key;

          hdr.leftmost_ptr = entry_ptr_as_page(left_sibling->records[m].ptr);
          clflush((char *)&(hdr.leftmost_ptr), sizeof(page_link_t));

          left_sibling->records[m].ptr = entry_ptr_null();
          clflush((char *)&(left_sibling->records[m].ptr), sizeof(entry_ptr_t));

          left_sibling->hdr.last_index = m - 1;
          clflush((char *)&(left_sibling->hdr.last_index), sizeof(int16_t));
        }

        if (left_sibling == (bt->root)) {
          page *new_root =
              new page(left_sibling, parent_key, this, hdr.level + 1);
          bt->setNewRoot(new_root);
        } else {
          bt->btree_insert_internal(left_sibling, parent_key,
                                    this, hdr.level + 1);
        }
      } else { // from leftmost case
        hdr.is_deleted = 1;
        clflush((char *)&(hdr.is_deleted), sizeof(uint8_t));

        page *new_sibling = new page(hdr.level);
        new_sibling->hdr.mtx.lock();
        new_sibling->hdr.sibling_ptr = hdr.sibling_ptr;

        int num_dist_entries = num_entries - m;
        int new_sibling_cnt = 0;

        if (hdr.leftmost_ptr == nullptr) {
          for (int i = 0; i < num_dist_entries; i++) {
            left_sibling->insert_key(records[i].key, records[i].ptr,
                                     &left_num_entries);
          }

          for (int i = num_dist_entries; !entry_ptr_is_null(records[i].ptr); i++) {
            new_sibling->insert_key(records[i].key, records[i].ptr,
                                    &new_sibling_cnt, false);
          }

          clflush((char *)(new_sibling), sizeof(page));

          left_sibling->hdr.sibling_ptr = new_sibling;
          clflush((char *)&(left_sibling->hdr.sibling_ptr), sizeof(page_link_t));

          parent_key = new_sibling->records[0].key;
        } else {
          left_sibling->insert_key(deleted_key_from_parent,
                                   make_entry_ptr_from_page(hdr.leftmost_ptr), &left_num_entries);

          for (int i = 0; i < num_dist_entries - 1; i++) {
            left_sibling->insert_key(records[i].key, records[i].ptr,
                                     &left_num_entries);
          }

          parent_key = records[num_dist_entries - 1].key;

          new_sibling->hdr.leftmost_ptr =
              entry_ptr_as_page(records[num_dist_entries - 1].ptr);
          for (int i = num_dist_entries; !entry_ptr_is_null(records[i].ptr); i++) {
            new_sibling->insert_key(records[i].key, records[i].ptr,
                                    &new_sibling_cnt, false);
          }
          clflush((char *)(new_sibling), sizeof(page));

          left_sibling->hdr.sibling_ptr = new_sibling;
          clflush((char *)&(left_sibling->hdr.sibling_ptr), sizeof(page_link_t));
        }

        if (left_sibling == (bt->root)) {
          page *new_root =
              new page(left_sibling, parent_key, new_sibling, hdr.level + 1);
          bt->setNewRoot(new_root);
        } else {
          bt->btree_insert_internal(left_sibling, parent_key,
                                    new_sibling, hdr.level + 1);
        }

        new_sibling->hdr.mtx.unlock();
      }
    } else {
      hdr.is_deleted = 1;
      clflush((char *)&(hdr.is_deleted), sizeof(uint8_t));

      if (hdr.leftmost_ptr)
        left_sibling->insert_key(deleted_key_from_parent,
                                 make_entry_ptr_from_page(hdr.leftmost_ptr), &left_num_entries);

      for (int i = 0; !entry_ptr_is_null(records[i].ptr); ++i) {
        left_sibling->insert_key(records[i].key, records[i].ptr,
                                 &left_num_entries);
      }

      left_sibling->hdr.sibling_ptr = hdr.sibling_ptr;
      clflush((char *)&(left_sibling->hdr.sibling_ptr), sizeof(page_link_t));
    }

    if (with_lock) {
      left_sibling->hdr.mtx.unlock();
      hdr.mtx.unlock();
    }

    return true;
  }

  // ptr is entry_ptr_t (char* under DRAM, uint64_t
  // under PMDK). Callers supply an encoded value via
  // make_entry_ptr_from_page / make_entry_ptr_from_value.
  inline void insert_key(entry_key_t key, entry_ptr_t ptr, int *num_entries,
                         bool flush = true, bool update_last_index = true) {
    // update switch_counter
    if (!IS_FORWARD(hdr.switch_counter))
      ++hdr.switch_counter;
    // Persist the switch_counter update so concurrent
    // readers that observe subsequent entry writes also observe the matching
    // counter parity (the page-level consistency protocol relies on this).
    if (flush) {
      clflush((char *)&hdr.switch_counter, sizeof(uint8_t));
    }

    // FAST
    if (*num_entries == 0) { // this page is empty
      entry *new_entry = (entry *)&records[0];
      entry *array_end = (entry *)&records[1];
      new_entry->key = (entry_key_t)key;
      new_entry->ptr = ptr;

      array_end->ptr = entry_ptr_null();

      if (flush) {
        clflush((char *)this, CACHE_LINE_SIZE);
      }
    } else {
      int i = *num_entries - 1, inserted = 0, to_flush_cnt = 0;
      records[*num_entries + 1].ptr = records[*num_entries].ptr;
      if (flush) {
        if ((uint64_t) & (records[*num_entries + 1].ptr) % CACHE_LINE_SIZE == 0)
          clflush((char *)&(records[*num_entries + 1].ptr), sizeof(entry_ptr_t));
      }

      // FAST
      for (i = *num_entries - 1; i >= 0; i--) {
        if (key < records[i].key) {
          records[i + 1].ptr = records[i].ptr;
          records[i + 1].key = records[i].key;

          if (flush) {
            uint64_t records_ptr = (uint64_t)(&records[i + 1]);

            int remainder = records_ptr % CACHE_LINE_SIZE;
            bool do_flush =
                (remainder == 0) ||
                ((((int)(remainder + sizeof(entry)) / CACHE_LINE_SIZE) == 1) &&
                 ((remainder + sizeof(entry)) % CACHE_LINE_SIZE) != 0);
            if (do_flush) {
              clflush((char *)records_ptr, CACHE_LINE_SIZE);
              to_flush_cnt = 0;
            } else
              ++to_flush_cnt;
          }
        } else {
          records[i + 1].ptr = records[i].ptr;
          records[i + 1].key = key;
          records[i + 1].ptr = ptr;

          if (flush)
            clflush((char *)&records[i + 1], sizeof(entry));
          inserted = 1;
          break;
        }
      }
      if (inserted == 0) {
        records[0].ptr = make_entry_ptr_from_page(hdr.leftmost_ptr);
        records[0].key = key;
        records[0].ptr = ptr;
        if (flush)
          clflush((char *)&records[0], sizeof(entry));
      }
    }

    if (update_last_index) {
      hdr.last_index = *num_entries;
    }
    ++(*num_entries);
  }

  // Insert a new key - FAST and FAIR
  //
  // right is entry_ptr_t (was char*). Callers supply
  // the encoded value via make_entry_ptr_from_page / make_entry_ptr_from_value.
  page *store(btree *bt, char *left, entry_key_t key, entry_ptr_t right, bool flush,
              bool with_lock, page *invalid_sibling = NULL) {
    
    if (with_lock) {
      hdr.mtx.lock(); // Lock the write lock
    }
    if (hdr.is_deleted) {
      if (with_lock) {
        hdr.mtx.unlock();
      }

      return NULL;
    }

    // If this node has a sibling node,
    if (hdr.sibling_ptr && (hdr.sibling_ptr != invalid_sibling)) {
      // Compare this key with the first key of the sibling
      if (key > hdr.sibling_ptr->records[0].key) {
        if (with_lock) {
          hdr.mtx.unlock(); // Unlock the write lock
        }
        return hdr.sibling_ptr->store(bt, NULL, key, right, true, with_lock,
                                      invalid_sibling);
      }
    }

    register int num_entries = count();

    // FAST
    if (num_entries < cardinality - 1) {
      insert_key(key, right, &num_entries, flush);

      if (with_lock) {
        hdr.mtx.unlock(); // Unlock the write lock
      }

      return this;
    } else { // FAIR
      // overflow
      // create a new node
      page *sibling = new page(hdr.level);
      register int m = (int)ceil(num_entries / 2);
      entry_key_t split_key = records[m].key;

      // migrate half of keys into the sibling
      int sibling_cnt = 0;
      if (hdr.leftmost_ptr == NULL) { // leaf node
        for (int i = m; i < num_entries; ++i) {
          sibling->insert_key(records[i].key, records[i].ptr, &sibling_cnt,
                              false);
        }
      } else { // internal node
        for (int i = m + 1; i < num_entries; ++i) {
          sibling->insert_key(records[i].key, records[i].ptr, &sibling_cnt,
                              false);
        }
        sibling->hdr.leftmost_ptr = entry_ptr_as_page(records[m].ptr);
      }

      sibling->hdr.sibling_ptr = hdr.sibling_ptr;
      clflush((char *)sibling, sizeof(page));

      hdr.sibling_ptr = sibling;
      clflush((char *)&hdr, sizeof(hdr));

      // set to NULL
      if (IS_FORWARD(hdr.switch_counter))
        hdr.switch_counter += 2;
      else
        ++hdr.switch_counter;
      records[m].ptr = entry_ptr_null();
      clflush((char *)&records[m], sizeof(entry));

      hdr.last_index = m - 1;
      clflush((char *)&(hdr.last_index), sizeof(int16_t));

      num_entries = hdr.last_index + 1;

      page *ret;

      // insert the key
      if (key < split_key) {
        insert_key(key, right, &num_entries);
        ret = this;
      } else {
        sibling->insert_key(key, right, &sibling_cnt);
        ret = sibling;
      }

      // Set a new root or insert the split key to the parent
      if (bt->root == this) { // only one node can update the root ptr
        page *new_root =
            new page((page *)this, split_key, sibling, hdr.level + 1);
        bt->setNewRoot(new_root);

        if (with_lock) {
          hdr.mtx.unlock(); // Unlock the write lock
        }
      } else {
        if (with_lock) {
          hdr.mtx.unlock(); // Unlock the write lock
        }
        bt->btree_insert_internal(NULL, split_key, sibling,
                                  hdr.level + 1);
      }

      return ret;
    }
  }

  // Search keys with linear search
  void linear_search_range(entry_key_t min, entry_key_t max,
                           unsigned long *buf) {
    int i, off = 0;
    uint8_t previous_switch_counter;
    page *current = this;

    while (current) {
      int old_off = off;
      do {
        previous_switch_counter = current->hdr.switch_counter;
        off = old_off;

        entry_key_t tmp_key;
        // entry_ptr_t typedef picks char* (DRAM) or
        // uint64_t (PMDK) so the bit pattern can carry a leaf value on a
        // leaf page.
        entry_ptr_t tmp_ptr;

        if (IS_FORWARD(previous_switch_counter)) {
          if ((tmp_key = current->records[0].key) > min) {
            if (tmp_key < max) {
              if (!entry_ptr_is_null(tmp_ptr = current->records[0].ptr)) {
                if (tmp_key == current->records[0].key) {
                  if (!entry_ptr_is_null(tmp_ptr)) {
                    buf[off++] = (unsigned long)tmp_ptr;
                  }
                }
              }
            } else
              return;
          }

          for (i = 1; !entry_ptr_is_null(current->records[i].ptr); ++i) {
            if ((tmp_key = current->records[i].key) > min) {
              if (tmp_key < max) {
                if ((tmp_ptr = current->records[i].ptr) !=
                    current->records[i - 1].ptr) {
                  if (tmp_key == current->records[i].key) {
                    if (!entry_ptr_is_null(tmp_ptr))
                      buf[off++] = (unsigned long)tmp_ptr;
                  }
                }
              } else
                return;
            }
          }
        } else {
          for (i = count() - 1; i > 0; --i) {
            if ((tmp_key = current->records[i].key) > min) {
              if (tmp_key < max) {
                if ((tmp_ptr = current->records[i].ptr) !=
                    current->records[i - 1].ptr) {
                  if (tmp_key == current->records[i].key) {
                    if (!entry_ptr_is_null(tmp_ptr))
                      buf[off++] = (unsigned long)tmp_ptr;
                  }
                }
              } else
                return;
            }
          }

          if ((tmp_key = current->records[0].key) > min) {
            if (tmp_key < max) {
              if (!entry_ptr_is_null(tmp_ptr = current->records[0].ptr)) {
                if (tmp_key == current->records[0].key) {
                  if (!entry_ptr_is_null(tmp_ptr)) {
                    buf[off++] = (unsigned long)tmp_ptr;
                  }
                }
              }
            } else
              return;
          }
        }
      } while (previous_switch_counter != current->hdr.switch_counter);

      current = current->hdr.sibling_ptr;
    }
  }

  // linear_search returns an entry_ptr_t (opaque 8-byte
  // bucket holding either a page offset / pointer, or a leaf value encoded
  // into the same bucket). Callers that descend the tree pass the result
  // through entry_ptr_as_page(); callers validating a leaf hit only check
  // non-nullness. Return type was char*; under DRAM this is unchanged,
  // under PMDK this becomes uint64_t.
  entry_ptr_t linear_search(entry_key_t key) {
    int i = 1;
    uint8_t previous_switch_counter;
    entry_ptr_t ret = entry_ptr_null();
    entry_ptr_t t;
    entry_key_t k;

    if (hdr.leftmost_ptr == NULL) { // Search a leaf node
      do {
        previous_switch_counter = hdr.switch_counter;
        ret = entry_ptr_null();

        // search from left ro right
        if (IS_FORWARD(previous_switch_counter)) {
          if ((k = records[0].key) == key) {
            if (!entry_ptr_is_null(t = records[0].ptr)) {
              if (k == records[0].key) {
                ret = t;
                continue;
              }
            }
          }

          for (i = 1; !entry_ptr_is_null(records[i].ptr); ++i) {
            if ((k = records[i].key) == key) {
              if (records[i - 1].ptr != (t = records[i].ptr)) {
                if (k == records[i].key) {
                  ret = t;
                  break;
                }
              }
            }
          }
        } else { // search from right to left
          for (i = count() - 1; i > 0; --i) {
            if ((k = records[i].key) == key) {
              if (records[i - 1].ptr != (t = records[i].ptr) && !entry_ptr_is_null(t)) {
                if (k == records[i].key) {
                  ret = t;
                  break;
                }
              }
            }
          }

          if (entry_ptr_is_null(ret)) {
            if ((k = records[0].key) == key) {
              if (!entry_ptr_is_null(t = records[0].ptr)) {
                if (k == records[0].key) {
                  ret = t;
                  continue;
                }
              }
            }
          }
        }
      } while (hdr.switch_counter != previous_switch_counter);

      if (!entry_ptr_is_null(ret)) {
        return ret;
      }

      // Follow sibling pointer when key falls beyond this leaf. Encode the
      // sibling page pointer back into the entry_ptr_t return bucket so
      // the caller's cast path (entry_ptr_as_page or descent) works.
      page *sib = hdr.sibling_ptr;
      if (sib && key >= sib->records[0].key) {
        return make_entry_ptr_from_page(sib);
      }

      return entry_ptr_null();
    } else { // internal node
      do {
        previous_switch_counter = hdr.switch_counter;
        ret = entry_ptr_null();

        if (IS_FORWARD(previous_switch_counter)) {
          if (key < (k = records[0].key)) {
            entry_ptr_t leftmost_enc = make_entry_ptr_from_page(hdr.leftmost_ptr);
            if ((t = leftmost_enc) != records[0].ptr) {
              ret = t;
              continue;
            }
          }

          for (i = 1; !entry_ptr_is_null(records[i].ptr); ++i) {
            if (key < (k = records[i].key)) {
              if ((t = records[i - 1].ptr) != records[i].ptr) {
                ret = t;
                break;
              }
            }
          }

          if (entry_ptr_is_null(ret)) {
            ret = records[i - 1].ptr;
            continue;
          }
        } else { // search from right to left
          for (i = count() - 1; i >= 0; --i) {
            if (key >= (k = records[i].key)) {
              if (i == 0) {
                entry_ptr_t leftmost_enc = make_entry_ptr_from_page(hdr.leftmost_ptr);
                if (leftmost_enc != (t = records[i].ptr)) {
                  ret = t;
                  break;
                }
              } else {
                if (records[i - 1].ptr != (t = records[i].ptr)) {
                  ret = t;
                  break;
                }
              }
            }
          }
        }
      } while (hdr.switch_counter != previous_switch_counter);

      page *sib2 = hdr.sibling_ptr;
      if (sib2 != nullptr) {
        if (key >= sib2->records[0].key)
          return make_entry_ptr_from_page(sib2);
      }

      if (!entry_ptr_is_null(ret)) {
        return ret;
      } else
        return make_entry_ptr_from_page(hdr.leftmost_ptr);
    }

    return entry_ptr_null();
  }

  // print a node
  void print() {
    if (hdr.leftmost_ptr == NULL)
      printf("[%d] leaf %p \n", this->hdr.level, (void *)this);
    else
      printf("[%d] internal %p \n", this->hdr.level, (void *)this);
    printf("last_index: %d\n", hdr.last_index);
    printf("switch_counter: %d\n", hdr.switch_counter);
    printf("search direction: ");
    if (IS_FORWARD(hdr.switch_counter))
      printf("->\n");
    else
      printf("<-\n");

    if (hdr.leftmost_ptr != NULL)
      printf("%p ", (void *)static_cast<page *>(hdr.leftmost_ptr));

    for (int i = 0; !entry_ptr_is_null(records[i].ptr); ++i)
      printf("%ld,%lx ", records[i].key, (unsigned long)records[i].ptr);

    printf("%p ", (void *)static_cast<page *>(hdr.sibling_ptr));

    printf("\n");
  }

  void printAll() {
    if (hdr.leftmost_ptr == NULL) {
      printf("printing leaf node: ");
      print();
    } else {
      printf("printing internal node: ");
      print();
      static_cast<page *>(hdr.leftmost_ptr)->printAll();
      for (int i = 0; !entry_ptr_is_null(records[i].ptr); ++i) {
        entry_ptr_as_page(records[i].ptr)->printAll();
      }
    }
  }
};

/*
 * class btree
 */
btree::btree(int num_threads, int num_eval_threads, int hash_capacity,
             unsigned long write_latency_ns,
             const std::string &persistent_path,
             std::uint64_t persistent_pool_bytes) {
  n_threads = num_threads > 0 ? num_threads : 1;
  eval_threads = num_eval_threads > 0 ? num_eval_threads : 1;
  // write_latency_in_ns is std::atomic; store with a
  // relaxed memory order. All subsequent clflush calls load this atomically.
  write_latency_in_ns.store(write_latency_ns, std::memory_order_relaxed);

#ifdef F3_PMDK
  // Pool lifecycle: one pool per process at a time. If a second btree
  // is constructed with F3_PMDK while an earlier one is alive, throw a
  // clear error instead of silently overwriting the global pool context
  // (which would corrupt PmemRef conversions for the earlier tree).
  if (f3tree::pmdk::current_pool() != nullptr) {
    throw std::runtime_error(
        "F3_PMDK: only one btree instance per process is supported "
        "(DECISION-009). Destroy the first tree before constructing a "
        "second, or build without PMDK=1 for multi-instance testing.");
  }

  // Pick a pool path. If the caller supplied one, use it verbatim; else
  // auto-generate a tmpfs path for the lifetime of this instance.
  if (persistent_path.empty()) {
    // tmpfs-backed pool for the DRAM-equivalent default.
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "/dev/shm/f3tree_auto_%d_%p.pool",
                  static_cast<int>(::getpid()),
                  static_cast<void *>(this));
    pool_path_ = buf;
    pool_auto_unlink_ = true;
  } else {
    pool_path_ = persistent_path;
    pool_auto_unlink_ = false;
  }

  const std::uint64_t pool_bytes =
      persistent_pool_bytes > 0 ? persistent_pool_bytes
                                : (128ull * 1024ull * 1024ull);

  // Open or create the pool.
  const bool file_exists = (::access(pool_path_.c_str(), F_OK) == 0);
  if (file_exists) {
    pool_handle_ = f3tree::pmdk::PmemPool::open(
        pool_path_, f3tree::pmdk::kPmemLayoutName);
  } else {
    pool_handle_ = f3tree::pmdk::PmemPool::create(
        pool_path_, f3tree::pmdk::kPmemLayoutName, pool_bytes, 0600);
  }

  // Populate the process-global pool context used by PmemRef.
  const PMEMoid root_oid = pool_handle_.root().raw();
  f3tree::pmdk::set_pool_context(&pool_handle_, root_oid.pool_uuid_lo);

  // Initialize or validate the root schema.
  auto root_ref = pool_handle_.root();
  if (!file_exists) {
    // Fresh pool: initialize schema fields inside a transaction.
    pmem::obj::transaction::run(pool_handle_, [&] {
      root_ref->schema_version = f3tree::pmdk::kPmemSchemaVersion;
      root_ref->seq_counter = 0;
      root_ref->n_threads = static_cast<std::uint32_t>(n_threads);
      root_ref->hash_capacity =
          static_cast<std::uint32_t>(hash_capacity > 0 ? hash_capacity : 1);
      root_ref->height = 1;
      // Leave root_page null for now; constructed below outside the tx.
    });
  } else {
    // Reopen: validate schema. Strict version match required.
    if (root_ref->schema_version != f3tree::pmdk::kPmemSchemaVersion) {
      throw std::runtime_error(
          "F3_PMDK: pool schema version mismatch (see DECISION-013)");
    }
    if (root_ref->n_threads != static_cast<std::uint32_t>(n_threads)) {
      throw std::runtime_error(
          "F3_PMDK: pool n_threads mismatch with RuntimeConfig "
          "(resize on reopen is not supported)");
    }
  }
#else
  (void)persistent_path;
  (void)persistent_pool_bytes;
#endif

  height = 1;
#ifdef F3_PMDK
  // Recovery: on a fresh pool allocate root + PTFO
  // dummy-head arrays inside a single transaction (atomic init). On
  // reopen, pick up the persisted root + PTFO anchors and reinitialize
  // every page's mutex so no stale pthread state is carried over.
  bool pool_reopen = file_exists;
  {
    auto root_ref = pool_handle_.root();
    if (root_ref->root_page == nullptr) {
      pmem::obj::transaction::run(pool_handle_, [&] {
        root_ref->root_page = pmem::obj::make_persistent<page>();
        root_ref->local_fut =
            pmem::obj::make_persistent<future_Node[]>(n_threads);
        root_ref->local_fut_tail =
            pmem::obj::make_persistent<future_Node[]>(n_threads);
      });
      root = root_ref->root_page.get();
      pool_reopen = false;
    } else {
      root = root_ref->root_page.get();
      height = static_cast<int>(root_ref->height);
      // Walk every page once and reset its std::mutex in place; sibling
      // + leftmost chains fully tile the tree.
      page *row_start = root;
      while (row_start != nullptr) {
        page *node = row_start;
        while (node != nullptr) {
          new (&node->hdr.mtx) std::mutex();
          node = node->hdr.sibling_ptr;
        }
        row_start = row_start->hdr.leftmost_ptr;
      }
    }
    local_fut = root_ref->local_fut.get();
    local_fut_tail = root_ref->local_fut_tail.get();
  }
#else
  root = new page();
  local_fut = (future_Node *)new future_Node[n_threads];
  local_fut_tail = (future_Node *) new future_Node[n_threads];
#endif

  hash = (HashMapTable *) new HashMapTable[n_threads];
  for (int i = 0; i < n_threads; ++i) {
    hash[i].init(hash_capacity > 0 ? hash_capacity : 1);
  }
  // Allocate one lookup directory per producer thread.
  // Each producer exclusively owns its own directory, removing the need for
  // a global tree lock on the PTFO insert path.
  const std::size_t dir_cap = hash_capacity > 0 ? static_cast<std::size_t>(hash_capacity) : 1;
  future_lookup_directory.resize(n_threads);
  for (int i = 0; i < n_threads; ++i) {
    future_lookup_directory[i].resize(dir_cap);
  }
  lookup_dir_mu = new std::mutex[n_threads];
  // Allocate one PTFO mutex per producer.
  ptfo_mu = new std::mutex[n_threads];

#ifdef F3_PMDK
  // Recovery pass: on reopen, walk each producer's
  // PTFO chain (durable in the pool) and rebuild the volatile
  // hash[tid] and future_lookup_directory[tid] entries. The chain is
  // the source of truth; the side-caches exist purely to accelerate
  // lookups and are rebuilt every process start.
  //
  // Every PTFO entry is applied in arrival order. Last-writer-wins is
  // correct since sequences are globally unique and the lookup
  // directory keeps the highest-sequence entry per key.
  if (pool_reopen) {
    auto root_ref = pool_handle_.root();
    std::uint64_t max_seq = 0;
    for (int tid = 0; tid < n_threads; ++tid) {
      future_Node *current = local_fut[tid].next;
      int chain_entries = 0;
      while (current != nullptr) {
        for (int k = 0; k < current->entry_count; ++k) {
          const entry_key_t key = current->keys[k];
          const bool is_delete = current->is_delete[k];
          const std::uint64_t seq = current->sequence[k];
          // Tier 1: per-producer hashtable.
          hash[tid].Insert(key, tid);
          // Tier 2: per-producer lookup directory. Keep the highest
          // sequence per key via existing upsert semantics.
          {
            std::lock_guard<std::mutex> lock(lookup_dir_mu[tid]);
            std::vector<future_lookup_slot> &dir = future_lookup_directory[tid];
            std::size_t lookup_index = future_lookup_index(key, tid, true);
            if (lookup_index != dir.size()) {
              future_lookup_slot &slot = dir[lookup_index];
              if (!slot.occupied || seq > slot.sequence) {
                slot.occupied = true;
                slot.key = key;
                slot.owner = tid;
                slot.state = is_delete ? FUTURE_TOMBSTONE : FUTURE_PRESENT;
                slot.sequence = seq;
              }
            }
          }
          if (seq > max_seq) max_seq = seq;
          ++chain_entries;
        }
        current = current->next;
      }
      // Also restore the dummy-head's cached entry_count so async-drain
      // threshold predicates see a non-zero pending count on reopen.
      local_fut[tid].entry_count = chain_entries;
    }
    // Continue sequence allocation strictly above any durable sequence
    // so reopens cannot accidentally reuse a number. The
    // +1_000_000 gap is paranoid; any crash between "drain commit" and
    // the next seq_counter update still leaves us on the safe side.
    const std::uint64_t durable_seq = root_ref->seq_counter;
    const std::uint64_t resume_seq =
        (durable_seq > max_seq ? durable_seq : max_seq) + 1000000ull;
    root_ref->seq_counter = resume_seq;
    pool_handle_.persist(&root_ref->seq_counter,
                         sizeof(root_ref->seq_counter));
  }
#endif
}

btree::~btree() {
#ifdef F3_PMDK
  // local_fut / local_fut_tail are pool-resident (root_ref->local_fut).
  // Do NOT delete[] them; the pool owns their storage and frees it when
  // the pool file is unlinked. Reset pointers so we don't reuse stale
  // addresses after clear_pool_context().
  local_fut = nullptr;
  local_fut_tail = nullptr;
#else
  delete[] local_fut;
  delete[] local_fut_tail;
#endif
  delete[] hash;
  delete[] lookup_dir_mu;
  delete[] ptfo_mu;
#ifdef F3_PMDK
  // Release the global pool context BEFORE closing the pool so any lingering
  // PmemRef conversions see a null context rather than a dangling handle.
  f3tree::pmdk::clear_pool_context();
  if (pool_handle_.handle() != nullptr) {
    pool_handle_.close();
  }
  if (pool_auto_unlink_ && !pool_path_.empty()) {
    ::unlink(pool_path_.c_str());
  }
#endif
}

// Probe producer tid's lookup directory for key using open addressing.
// Returns the slot index if found/allocatable, or dir.size() if not found/full.
// Must be called with lookup_dir_mu[tid] held.
std::size_t btree::future_lookup_index(entry_key_t key, int tid, bool allocate_slot) {
  if (tid < 0 || tid >= n_threads) {
    return 0;
  }
  std::vector<future_lookup_slot>& dir = future_lookup_directory[tid];
  if (dir.empty()) {
    dir.resize(1);
  }

  std::size_t index =
      static_cast<std::size_t>((key % static_cast<entry_key_t>(dir.size()) +
                                static_cast<entry_key_t>(dir.size())) %
                               static_cast<entry_key_t>(dir.size()));
  const std::size_t start = index;

  do {
    if (!dir[index].occupied) {
      return allocate_slot ? index : dir.size();
    }
    if (dir[index].key == key) {
      return index;
    }
    index = (index + 1) % dir.size();
  } while (index != start);

  return dir.size();
}

void btree::setNewRoot(page *new_root) {
  this->root = new_root;
  clflush((char *)&(this->root), sizeof(page *));
  ++height;
}

char *btree::btree_search(entry_key_t key) {
  // Removed a dead pre-loop that probed hash->SearchKey
  // but never used the result. The per-producer hashtable belongs to the
  // PTFO (future_hash_directed_state_via_ptfo) tier-1 pre-filter, not to
  // the global-tree search path.
  page *p = root;

  while (p->hdr.leftmost_ptr != NULL) {
    p = entry_ptr_as_page(p->linear_search(key));
  }

  entry_ptr_t t_enc;
  entry_ptr_t sib_enc = make_entry_ptr_from_page(p->hdr.sibling_ptr);
  while ((t_enc = p->linear_search(key)) == sib_enc) {
    page *t_page = entry_ptr_as_page(t_enc);
    p = t_page;
    if (!p) {
      break;
    }
    sib_enc = make_entry_ptr_from_page(p->hdr.sibling_ptr);
  }

  if (entry_ptr_is_null(t_enc)) {
    return NULL;
  }

  // Return a non-null pointer in the "char*" shape.
  // Under DRAM this is the original char* value; under PMDK we
  // reinterpret the 64-bit bucket — the caller only tests null vs non-null.
  return entry_ptr_to_char_ptr(t_enc);
}

// insert the key in the leaf node
void btree::btree_insert(entry_key_t key, char *right) { // need to be string
  // right is a char* that encodes the leaf value via
  // the (char*)(key) cast. Convert once at the public boundary.
  const entry_ptr_t right_enc = char_ptr_to_entry_ptr(right);
  page *p = root;

  while (p->hdr.leftmost_ptr != NULL) {
    p = entry_ptr_as_page(p->linear_search(key));
  }

  if (!p->store(this, NULL, key, right_enc, true, true)) { // store
    btree_insert(key, right);
  }
}

// store the key into the node at the given level
void btree::btree_insert_internal(page *left, entry_key_t key, page *right,
                                  uint32_t level) {
  if (level > root->hdr.level)
    return;

  page *p = this->root;

  while (p->hdr.level > level)
    p = entry_ptr_as_page(p->linear_search(key));

  if (!p->store(this, NULL, key, make_entry_ptr_from_page(right), true, true)) {
    btree_insert_internal(left, key, right, level);
  }
}

void btree::btree_delete(entry_key_t key) {
  page *p = root;

  while (p->hdr.leftmost_ptr != NULL) {
    p = entry_ptr_as_page(p->linear_search(key));
  }

  entry_ptr_t t_enc;
  entry_ptr_t sib_enc = make_entry_ptr_from_page(p->hdr.sibling_ptr);
  while ((t_enc = p->linear_search(key)) == sib_enc) {
    p = entry_ptr_as_page(t_enc);
    if (!p)
      break;
    sib_enc = make_entry_ptr_from_page(p->hdr.sibling_ptr);
  }

  if (p) {
    if (!p->remove(this, key)) {
      btree_delete(key);
    }
  } else {
    printf("not found the key to delete %lu\n", key);
  }
}

void btree::btree_delete_internal(entry_key_t key, page *ptr, uint32_t level,
                                  entry_key_t *deleted_key,
                                  bool *is_leftmost_node, page **left_sibling) {
  if (level > this->root->hdr.level)
    return;

  page *p = this->root;

  while (p->hdr.level > level) {
    p = entry_ptr_as_page(p->linear_search(key));
  }

  p->hdr.mtx.lock();

  // Compare encoded page pointers. Under DRAM this is a raw pointer
  // compare; under PMDK this compares pool offsets (both fields stored
  // as entry_ptr_t).
  const entry_ptr_t ptr_enc = make_entry_ptr_from_page(ptr);
  if (make_entry_ptr_from_page(p->hdr.leftmost_ptr) == ptr_enc) {
    *is_leftmost_node = true;
    p->hdr.mtx.unlock();
    return;
  }

  *is_leftmost_node = false;

  for (int i = 0; !entry_ptr_is_null(p->records[i].ptr); ++i) {
    if (p->records[i].ptr == ptr_enc) {
      if (i == 0) {
        if (make_entry_ptr_from_page(p->hdr.leftmost_ptr) != p->records[i].ptr) {
          *deleted_key = p->records[i].key;
          *left_sibling = p->hdr.leftmost_ptr;
          p->remove(this, *deleted_key, false, false);
          break;
        }
      } else {
        if (p->records[i - 1].ptr != p->records[i].ptr) {
          *deleted_key = p->records[i].key;
          *left_sibling = entry_ptr_as_page(p->records[i - 1].ptr);
          p->remove(this, *deleted_key, false, false);
          break;
        }
      }
    }
  }

  p->hdr.mtx.unlock();
}

// Function to search keys from "min" to "max"
void btree::btree_search_range(entry_key_t min, entry_key_t max,
                               unsigned long *buf) {
  page *p = root;

  while (p) {
    if (p->hdr.leftmost_ptr != NULL) {
      // The current page is internal
      p = entry_ptr_as_page(p->linear_search(min));
    } else {
      // Found a leaf
      p->linear_search_range(min, max, buf);

      break;
    }
  }
}

void btree::printAll() {
  pthread_mutex_lock(&print_mtx);
  int total_keys = 0;
  page *leftmost = root;
  printf("root: %p\n", (void *)root);
  do {
    page *sibling = leftmost;
    while (sibling) {
      if (sibling->hdr.level == 0) {
        total_keys += sibling->hdr.last_index + 1;
      }
      sibling->print();
      sibling = sibling->hdr.sibling_ptr;
    }
    printf("-----------------------------------------\n");
    leftmost = leftmost->hdr.leftmost_ptr;
  } while (leftmost);

  printf("total number of keys: %d\n", total_keys);
  pthread_mutex_unlock(&print_mtx);
}

void btree::future_insert(entry_key_t key, int tid, bool isDone, uint64_t sequence_number){
  if(local_fut == NULL){
    local_fut = new future_Node[n_threads];  // defensive; normally set in constructor
  }

  // Serialize this producer's PTFO mutation against any
  // concurrent evaluator drain or PTFO-traversal reader. Lock order:
  // ptfo_mu[tid] (outer) -> lookup_dir_mu[tid] (inner, nested scope below).
  {
    std::lock_guard<std::mutex> ptfo_lock(ptfo_mu[tid]);

    if (local_fut[tid].next == NULL) {
      //Create a new node next to the dummy head node.
      future_Node *first_node = new future_Node();
      touch_future_node_allocation(first_node);
      first_node->keys[0] = key;
      first_node->is_delete[0] = isDone;
      first_node->sequence[0] = sequence_number;
      first_node->entry_count += 1;
      first_node->prev = &(local_fut[tid]);
      // Persist the node's own contents BEFORE the link
      // is written, so recovery never follows a head->next pointer to a
      // partially-initialized future_Node. The previous implementation
      // flushed the first cache line of the btree object (`this`), which
      // did not cover first_node at all.
      clflush((char *)first_node, sizeof(future_Node));
      local_fut[tid].next = first_node;
      local_fut_tail[tid].next = first_node;
      local_fut[tid].entry_count += 1;
      // Persist the head and tail pointer updates individually.
      clflush((char *)&local_fut[tid].next, sizeof(future_Node *));
      clflush((char *)&local_fut_tail[tid].next, sizeof(future_Node *));
      hash[tid].Insert(key, tid);
    } else if (local_fut[tid].next->entry_count == cardinality) {
      // Current head node is full — prepend a new node at the head.
      future_Node *new_node = new future_Node();
      touch_future_node_allocation(new_node);
      new_node->keys[0] = key;
      new_node->is_delete[0] = isDone;
      new_node->sequence[0] = sequence_number;
      new_node->entry_count += 1;
      new_node->prev = &(local_fut[tid]);
      new_node->next = local_fut[tid].next;
      // Persist new_node contents (including its prev/next
      // link fields set above) BEFORE splicing it into the chain.
      clflush((char *)new_node, sizeof(future_Node));
      // Splice in: update the old head's prev pointer, flush that field,
      // then move the dummy-head's next pointer and flush it.
      local_fut[tid].next->prev = new_node;
      clflush((char *)&local_fut[tid].next->prev, sizeof(future_Node *));
      local_fut[tid].next = new_node;
      clflush((char *)&local_fut[tid].next, sizeof(future_Node *));
      local_fut[tid].entry_count += 1;
      hash[tid].Insert(key, tid);
    } else {
      // Append into the current head node. Order: write entry fields, flush
      // them, then bump entry_count, flush entry_count. Recovery never sees
      // entry_count advanced past a partially-written entry.
      const int idx = local_fut[tid].next->entry_count;
      local_fut[tid].next->keys[idx] = key;
      local_fut[tid].next->is_delete[idx] = isDone;
      local_fut[tid].next->sequence[idx] = sequence_number;
      // Flush the just-written entry triple. We flush the
      // whole node here because keys[], is_delete[], and sequence[] are
      // separate arrays inside future_Node and the three updated slots may
      // fall on different cache lines; flushing sizeof(future_Node) is
      // simpler and correct. (The original code used an alignment heuristic
      // that frequently skipped the flush and did not cover entry_count.)
      clflush((char *)static_cast<future_Node *>(local_fut[tid].next), sizeof(future_Node));
      local_fut[tid].next->entry_count += 1;
      clflush((char *)&local_fut[tid].next->entry_count, sizeof(int));
      hash[tid].Insert(key, tid);
    }

    // Update this producer's lookup directory. Only producer tid writes to
    // future_lookup_directory[tid], so we hold only lookup_dir_mu[tid] here —
    // no global tree lock required.  Nested inside the ptfo_mu scope so the
    // established ptfo_mu[tid] -> lookup_dir_mu[tid] lock order is preserved.
    {
      std::lock_guard<std::mutex> lock(lookup_dir_mu[tid]);
      std::vector<future_lookup_slot> &dir = future_lookup_directory[tid];
      std::size_t lookup_index = future_lookup_index(key, tid, true);
      if (lookup_index != dir.size()) {
        dir[lookup_index].occupied = true;
        dir[lookup_index].key = key;
        dir[lookup_index].owner = tid;
        dir[lookup_index].state = isDone ? FUTURE_TOMBSTONE : FUTURE_PRESENT;
        dir[lookup_index].sequence = sequence_number;
      }
    }
  }
}

future_entry_state btree::future_state(entry_key_t key, int tid) {
  if (local_fut == NULL || tid < 0 || tid >= n_threads) {
    return FUTURE_ABSENT;
  }

  // Serialize against concurrent producers / drain.
  std::lock_guard<std::mutex> lock(ptfo_mu[tid]);
  future_Node *current = local_fut[tid].next;
  while (current != NULL) {
    for (int i = current->entry_count - 1; i >= 0; --i) {
      if (current->keys[i] == key) {
        return current->is_delete[i] ? FUTURE_TOMBSTONE : FUTURE_PRESENT;
      }
    }
    current = current->next;
  }

  return FUTURE_ABSENT;
}

future_entry_state btree::future_state_any(entry_key_t key) {
  if (local_fut == NULL) {
    return FUTURE_ABSENT;
  }

  // Each future_state call acquires its own ptfo_mu[tid]; safe to iterate.
  for (int tid = 0; tid < n_threads; ++tid) {
    future_entry_state state = future_state(key, tid);
    if (state != FUTURE_ABSENT) {
      return state;
    }
  }

  return FUTURE_ABSENT;
}

future_entry_state btree::future_latest_state(entry_key_t key,
                                              int *owner,
                                              uint64_t *latest_sequence) {
  if (owner != NULL) {
    *owner = -1;
  }
  if (latest_sequence != NULL) {
    *latest_sequence = 0;
  }
  if (local_fut == NULL) {
    return FUTURE_ABSENT;
  }

  future_entry_state latest_state = FUTURE_ABSENT;
  uint64_t best_sequence = 0;
  int best_owner = -1;

  for (int tid = 0; tid < n_threads; ++tid) {
    // Lock each producer's PTFO chain individually.
    std::lock_guard<std::mutex> lock(ptfo_mu[tid]);
    future_Node *current = local_fut[tid].next;
    while (current != NULL) {
      for (int i = 0; i < current->entry_count; ++i) {
        if (current->keys[i] != key) {
          continue;
        }
        if (latest_state == FUTURE_ABSENT || current->sequence[i] > best_sequence) {
          best_sequence = current->sequence[i];
          best_owner = tid;
          latest_state = current->is_delete[i] ? FUTURE_TOMBSTONE : FUTURE_PRESENT;
        }
      }
      current = current->next;
    }
  }

  if (owner != NULL) {
    *owner = best_owner;
  }
  if (latest_sequence != NULL) {
    *latest_sequence = best_sequence;
  }

  return latest_state;
}

future_entry_state btree::future_lookup_state(entry_key_t key,
                                              int *owner,
                                              uint64_t *latest_sequence) {
  if (owner != NULL) {
    *owner = -1;
  }
  if (latest_sequence != NULL) {
    *latest_sequence = 0;
  }

  // Scan every producer's directory independently, taking each per-producer
  // lock briefly. Per-producer locks are never held simultaneously, so this
  // cannot deadlock with drain (which holds tree_mu then acquires per-producer
  // locks one at a time).
  future_entry_state latest_state = FUTURE_ABSENT;
  uint64_t best_seq = 0;
  int best_owner = -1;

  for (int tid = 0; tid < n_threads; ++tid) {
    std::lock_guard<std::mutex> lock(lookup_dir_mu[tid]);
    const std::size_t index = future_lookup_index(key, tid, false);
    if (index == future_lookup_directory[tid].size()) {
      continue;
    }
    const future_lookup_slot& slot = future_lookup_directory[tid][index];
    if (latest_state == FUTURE_ABSENT || slot.sequence > best_seq) {
      best_seq = slot.sequence;
      best_owner = slot.owner;
      latest_state = slot.state;
    }
  }

  if (owner != NULL) {
    *owner = best_owner;
  }
  if (latest_sequence != NULL) {
    *latest_sequence = best_seq;
  }
  return latest_state;
}

int btree::future_lookup_owner(entry_key_t key) {
  int best_owner = -1;
  uint64_t best_seq = 0;

  for (int tid = 0; tid < n_threads; ++tid) {
    std::lock_guard<std::mutex> lock(lookup_dir_mu[tid]);
    const std::size_t index = future_lookup_index(key, tid, false);
    if (index == future_lookup_directory[tid].size()) {
      continue;
    }
    const future_lookup_slot& slot = future_lookup_directory[tid][index];
    if (best_owner == -1 || slot.sequence > best_seq) {
      best_seq = slot.sequence;
      best_owner = slot.owner;
    }
  }
  return best_owner;
}

// 3-tier hierarchical search.
// Tier 1 — hashtable: per-producer hash[tid].SearchKey(key) filters to producers
//   that have ever buffered this key. Producers with no hashtable entry are skipped
//   without acquiring any lock.
// Tier 2 — PTFO/lookup directory: for hashtable hits, the per-producer lookup
//   directory is checked under lookup_dir_mu[tid] to obtain the authoritative
//   current state (PRESENT, TOMBSTONE, or ABSENT if cleared after drain).
// Tier 3 — global tree: caller falls back when this returns FUTURE_ABSENT.
future_entry_state btree::future_hash_directed_state(entry_key_t key,
                                                     int *owner,
                                                     uint64_t *latest_sequence) {
  if (owner != NULL) {
    *owner = -1;
  }
  if (latest_sequence != NULL) {
    *latest_sequence = 0;
  }

  future_entry_state latest_state = FUTURE_ABSENT;
  uint64_t best_seq = 0;
  int best_owner = -1;

  for (int tid = 0; tid < n_threads; ++tid) {
    // Tier 1: hashtable pre-filter. If this producer has no entry for key,
    // skip without acquiring lookup_dir_mu[tid].
    if (hash[tid].SearchKey(key) == -1) {
      continue;
    }
    // Tier 2: lookup directory for authoritative buffered state.
    std::lock_guard<std::mutex> lock(lookup_dir_mu[tid]);
    const std::size_t index = future_lookup_index(key, tid, false);
    if (index == future_lookup_directory[tid].size()) {
      continue;
    }
    const future_lookup_slot& slot = future_lookup_directory[tid][index];
    if (latest_state == FUTURE_ABSENT || slot.sequence > best_seq) {
      best_seq = slot.sequence;
      best_owner = slot.owner;
      latest_state = slot.state;
    }
  }

  if (owner != NULL) {
    *owner = best_owner;
  }
  if (latest_sequence != NULL) {
    *latest_sequence = best_seq;
  }
  return latest_state;
}

int btree::future_hash_directed_owner(entry_key_t key) {
  int best_owner = -1;
  uint64_t best_seq = 0;

  for (int tid = 0; tid < n_threads; ++tid) {
    if (hash[tid].SearchKey(key) == -1) {
      continue;
    }
    std::lock_guard<std::mutex> lock(lookup_dir_mu[tid]);
    const std::size_t index = future_lookup_index(key, tid, false);
    if (index == future_lookup_directory[tid].size()) {
      continue;
    }
    const future_lookup_slot& slot = future_lookup_directory[tid][index];
    if (best_owner == -1 || slot.sequence > best_seq) {
      best_seq = slot.sequence;
      best_owner = slot.owner;
    }
  }
  return best_owner;
}

// 3-tier hierarchical search via direct PTFO linked-list traversal.
// Unlike future_hash_directed_state (which reads the per-producer
// lookup-directory proxy), this method walks the actual per-producer PTFO
// linked list (local_fut[tid] chain) for every producer whose hashtable hit —
// following the hashtable -> corresponding PTFO linked list -> global tree flow.
//
// Last-writer-wins is decided strictly on recorded sequence number, not on
// chain order: new nodes are PREPENDED at local_fut[tid].next (see
// future_insert), so chain-head is newest-first, but two entries on different
// nodes may still compare by sequence only. Sequence numbers are globally
// unique (assigned by the wrapper's atomic counter).
//
// Caller handles tier 3 fallback to the global tree when this returns
// FUTURE_ABSENT. See docs/MANUSCRIPT_CONFORMANCE_AUDIT.md §3.
future_entry_state btree::future_hash_directed_state_via_ptfo(
    entry_key_t key, int *owner, uint64_t *latest_sequence) {
  if (owner != NULL) {
    *owner = -1;
  }
  if (latest_sequence != NULL) {
    *latest_sequence = 0;
  }

  future_entry_state latest_state = FUTURE_ABSENT;
  uint64_t best_seq = 0;
  int best_owner = -1;

  for (int tid = 0; tid < n_threads; ++tid) {
    // Tier 1: hashtable pre-filter. Skip producers that have never buffered
    // this key — no PTFO lock acquired for these.
    if (hash[tid].SearchKey(key) == -1) {
      continue;
    }

    // Tier 2: walk local_fut[tid] linked list under the per-producer PTFO
    // mutex. The chain is doubly-linked and appended at local_fut[tid].next
    // (head-first). Traverse the whole chain per producer — the hashtable
    // may have false positives (tombstoned slots marked with DelNode still
    // return the producer id), so chain walk is the authoritative check.
    std::lock_guard<std::mutex> lock(ptfo_mu[tid]);
    future_Node *current = local_fut[tid].next;
    while (current != NULL) {
      for (int i = 0; i < current->entry_count; ++i) {
        if (current->keys[i] != key) {
          continue;
        }
        if (latest_state == FUTURE_ABSENT || current->sequence[i] > best_seq) {
          best_seq = current->sequence[i];
          best_owner = tid;
          latest_state = current->is_delete[i] ? FUTURE_TOMBSTONE : FUTURE_PRESENT;
        }
      }
      current = current->next;
    }
  }

  if (owner != NULL) {
    *owner = best_owner;
  }
  if (latest_sequence != NULL) {
    *latest_sequence = best_seq;
  }
  return latest_state;
}

int btree::future_hash_directed_owner_via_ptfo(entry_key_t key) {
  // Same 3-tier flow as future_hash_directed_state_via_ptfo, but only needs
  // the owner of the latest entry. Sequence ordering decides ownership.
  int best_owner = -1;
  uint64_t best_seq = 0;
  bool found = false;

  for (int tid = 0; tid < n_threads; ++tid) {
    if (hash[tid].SearchKey(key) == -1) {
      continue;
    }
    std::lock_guard<std::mutex> lock(ptfo_mu[tid]);
    future_Node *current = local_fut[tid].next;
    while (current != NULL) {
      for (int i = 0; i < current->entry_count; ++i) {
        if (current->keys[i] != key) {
          continue;
        }
        if (!found || current->sequence[i] > best_seq) {
          best_seq = current->sequence[i];
          best_owner = tid;
          found = true;
        }
      }
      current = current->next;
    }
  }
  return best_owner;
}

void btree::future_collect_lookup_entries(std::vector<future_lookup_entry> *entries) {
  if (entries == NULL) {
    return;
  }

  for (int tid = 0; tid < n_threads; ++tid) {
    std::lock_guard<std::mutex> lock(lookup_dir_mu[tid]);
    for (std::size_t i = 0; i < future_lookup_directory[tid].size(); ++i) {
      const future_lookup_slot& slot = future_lookup_directory[tid][i];
      if (!slot.occupied || slot.state == FUTURE_ABSENT) {
        continue;
      }
      future_lookup_entry entry;
      entry.key = slot.key;
      entry.owner = slot.owner;
      entry.state = slot.state;
      entry.sequence = slot.sequence;
      entries->push_back(entry);
    }
  }
}

void btree::future_collect_ops(int tid, std::vector<future_buffered_op> *ops) {
  if (ops == NULL || local_fut == NULL || tid < 0 || tid >= n_threads) {
    return;
  }

  future_Node *current = local_fut[tid].next;
  while (current != NULL) {
    for (int i = 0; i < current->entry_count; ++i) {
      future_buffered_op op;
      op.key = current->keys[i];
      op.is_delete = current->is_delete[i];
      op.sequence = current->sequence[i];
      op.producer_id = tid;
      ops->push_back(op);
    }
    current = current->next;
  }
}

void btree::future_collect_nodes(int tid, std::vector<future_buffered_node> *nodes) {
  if (nodes == NULL || local_fut == NULL || tid < 0 || tid >= n_threads) {
    return;
  }

  future_Node *current = local_fut[tid].next;
  while (current != NULL) {
    future_buffered_node node;
    node.producer_id = tid;
    for (int i = 0; i < current->entry_count; ++i) {
      future_buffered_op op;
      op.key = current->keys[i];
      op.is_delete = current->is_delete[i];
      op.sequence = current->sequence[i];
      op.producer_id = tid;
      if (node.ops.empty()) {
        node.first_sequence = op.sequence;
      }
      node.last_sequence = op.sequence;
      node.ops.push_back(op);
    }
    if (!node.ops.empty()) {
      nodes->push_back(node);
    }
    current = current->next;
  }
}

void btree::future_clear_thread(int tid) {
  if (local_fut == NULL || local_fut_tail == NULL || tid < 0 || tid >= n_threads) {
    return;
  }

  future_Node *current = local_fut[tid].next;
  while (current != NULL) {
    future_Node *next = current->next;
    delete current;
    current = next;
  }

  local_fut[tid].next = nullptr;
  local_fut[tid].entry_count = 0;
  local_fut[tid].is_done = true;
  local_fut_tail[tid].next = nullptr;
}

void btree::future_clear_lookup_entries_for_producers(const std::vector<int> &producer_ids) {
  if (producer_ids.empty()) {
    return;
  }

  // Each producer owns its own directory, so we can clear exactly the right
  // entries without scanning unrelated producers' directories.
  for (std::size_t pi = 0; pi < producer_ids.size(); ++pi) {
    const int tid = producer_ids[pi];
    if (tid < 0 || tid >= n_threads) {
      continue;
    }
    std::lock_guard<std::mutex> lock(lookup_dir_mu[tid]);
    for (std::size_t i = 0; i < future_lookup_directory[tid].size(); ++i) {
      future_lookup_directory[tid][i] = future_lookup_slot();
    }
  }
}

void btree::future_clear_lookup_directory() {
  for (int tid = 0; tid < n_threads; ++tid) {
    std::lock_guard<std::mutex> lock(lookup_dir_mu[tid]);
    for (std::size_t i = 0; i < future_lookup_directory[tid].size(); ++i) {
      future_lookup_directory[tid][i] = future_lookup_slot();
    }
  }
}

void btree::future_evaluate(btree *bt, int tid){
    //mutli-threaded Future Evaluate
    //printf("Future Evaluate\n");
    int prod_to_cons;
    do{
        if(local_fut[tid].entry_count == 0){
            //std::this_thread::sleep_for (std::chrono::microseconds(1000));
            continue;
        } else{
            if(tid == 0){
                prod_to_cons = ((n_threads/eval_threads)+(n_threads%eval_threads));
                future_evaluate_execute(bt, tid, prod_to_cons);
            }else{
                prod_to_cons = (n_threads/eval_threads);
                future_evaluate_execute(bt, tid, prod_to_cons);
            }
        }
        for(int i = tid; i < n_threads; i++){
            if(local_fut[i].is_done){
                bt->is_Done = true;
            }
        }
    }while(!bt->is_Done);
}

/*Singly linked list based
void btree::future_evaluate_execute(btree *bt, int tid, int total_t){
  printf("Evaluate Execute Starts\n");
  for(int i = (tid*total_t); i < ((tid+1) * total_t); i++){
    future_Node *last = NULL;
    while (last != bt->local_fut[i].next){
      future_Node *current = bt->local_fut[i].next;
      while(current->next != last)
        current = current->next;

        while (current->entry_count != 0) { 
          int k= current->entry_count;
          int64_t key = current->keys[k];
          bt->btree_insert(key, (char *)key);
          //Remove the key from Hash Table
          //bt->hash[i].Remove(current->keys[k], i);
          
          current->entry_count -= 1;
        }

        /*for(int k = 0; k < current->entry_count; ++k){
          bt->btree_insert(current->keys[k], (char *)current->keys[k]);
        }*/

        /*last = current;
        bt->local_fut[i].entry_count -= 1;
        delete current;

        if(bt->local_fut[i].entry_count == 0){
          bt->local_fut[i].is_done = true;
        }
      }
  }
  //printf("Evaluate Execute Returns\n");
}*/

//Using Tail Pointer.
void btree::future_evaluate_execute(btree *bt, int tid, int total_t){
  //printf("Evaluate Execute.\n");
  for(int i = (tid*total_t); i < ((tid+1) * total_t); i++){
    future_Node *last_node = NULL;  
    //printf("Last Node not NULL\n");
    while(last_node != bt->local_fut[i].next){
      future_Node *tail_node = bt->local_fut_tail[i].next;
        for(int k = 0; k < tail_node->entry_count; k++){
          if (tail_node->is_delete[k]) {
            if (bt->btree_search(tail_node->keys[k]) != NULL) {
              bt->btree_delete(tail_node->keys[k]);
            }
          } else {
            bt->btree_insert(tail_node->keys[k], (char *)tail_node->keys[k]);
          }
          //bt->hash[i].Remove(tail_node->keys[k]);
        }

        last_node = tail_node->prev;
        bt->local_fut_tail[i].next = tail_node->prev;
        bt->local_fut[i].entry_count -= 1;
        //Remove the key from Hash Table
        
        delete tail_node;

        

        if(bt->local_fut[i].entry_count == 0){
          bt->local_fut[i].is_done = true;
          break;
        }
      }
  }
  //printf("Evaluate Execute Returns\n");
}

// Drain a single producer's PTFO from TAIL to HEAD (oldest entry first).
//
// Unlike future_evaluate_execute, which stops before reaching the HEAD node
// (to allow concurrent producers to keep writing), this method drains ALL
// nodes including the current head, making it suitable for synchronous
// drain_pending() where no concurrent producers are active.
//
// The tail pointer is advanced and each node is freed AFTER its entries have
// been checkpointed to the global tree. This gives a durable checkpoint
// boundary: if interrupted, the tail pointer marks the recovery point and
// any nodes between the new tail and the old tail have already been applied.
//
// Returns the count of individual key operations applied.
int btree::future_drain_thread_from_tail(int tid) {
  if (local_fut == NULL || local_fut_tail == NULL || tid < 0 || tid >= n_threads) {
    return 0;
  }
  int applied = 0;
  while (local_fut_tail[tid].next != NULL) {
    future_Node *tail_node = local_fut_tail[tid].next;
    // Apply all entries in this node (oldest-first within the node array).
    for (int k = 0; k < tail_node->entry_count; k++) {
      if (tail_node->is_delete[k]) {
        if (btree_search(tail_node->keys[k]) != NULL) {
          btree_delete(tail_node->keys[k]);
        }
      } else {
        btree_insert(tail_node->keys[k], (char *)tail_node->keys[k]);
      }
      ++applied;
    }
    // Advance the tail pointer toward HEAD (prev = newer direction) before
    // freeing the node. This is the durable checkpoint: once the pointer is
    // written the entries cannot be replayed again on recovery.
    future_Node *next_tail = tail_node->prev;
    // prev of the oldest node is the HEAD dummy, not a real future_Node.
    // When we reach it, the chain is fully drained.
    if (next_tail == &local_fut[tid]) {
      next_tail = NULL;
    }
    local_fut_tail[tid].next = next_tail;
    if (local_fut[tid].entry_count > 0) {
      local_fut[tid].entry_count -= 1;
    }
    delete tail_node;
  }
  // Chain fully drained: reset the HEAD dummy so the PTFO is ready for reuse.
  local_fut[tid].next = nullptr;
  local_fut[tid].entry_count = 0;
  local_fut[tid].is_done = true;
  return applied;
}
