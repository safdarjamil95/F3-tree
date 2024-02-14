#include <cassert>
#include <climits>
#include <fstream>
#include <future>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <atomic>
#include "hash.h"


#define PAGESIZE 512  //Node size leaf+inner+futures
#define CACHELINE_SIZE 64
#define CPU_FREQ_MHZ (1994)

#define IS_FORWARD(c) (c % 2 == 0)
#define MAX_THREAD 10
int n_threads, eval_threads;

void do_flush(const void* addr, size_t len);

static inline void cpu_pause() { __asm__ volatile("pause" ::: "memory"); }
static inline unsigned long read_tsc(void) {
  unsigned long var;
  unsigned int hi, lo;

  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  var = ((unsigned long long int)hi << 32) | lo;

  return var;
}

unsigned long write_latency_in_ns = 0;
unsigned long long search_time_in_insert = 0;
unsigned int gettime_cnt = 0;
unsigned long long clflush_time_in_insert = 0;
unsigned long long update_time_in_insert = 0;
int clflush_cnt = 0;
int node_cnt = 0;

inline void mfence() { asm volatile("mfence" ::: "memory"); }

inline void clflush(char *data, int len) {
  volatile char *ptr = (char *)((unsigned long)data & ~(CACHELINE_SIZE - 1));
  mfence();
  for (; ptr < data + len; ptr += CACHELINE_SIZE) {
    unsigned long etsc =
        read_tsc() + (unsigned long)(write_latency_in_ns * CPU_FREQ_MHZ / 1000);
    asm volatile("clflush %0" : "+m"(*(volatile char *)ptr));
    while (read_tsc() < etsc)
      cpu_pause();
    //++clflush_cnt;
  }
  mfence();
}

class page;

class globalNode{
    public:    
        page *leftmost_ptr;
        page *sibling_ptr;      // 8 bytes
        uint32_t level;         // 4 bytes
        uint8_t switch_counter; // 1 bytes
        uint8_t is_deleted;     // 1 bytes
        int16_t last_index;     // 2 bytes
        std::mutex *mtx;        // 8 bytes

        friend class page;
        friend class fBtree;
        
        globalNode() {
            mtx = new std::mutex();
            leftmost_ptr = NULL;
            sibling_ptr = NULL;
            switch_counter = 0;
            last_index = -1;
            is_deleted = false;
        }

        ~globalNode() { delete mtx; }
};

class entry{
    private:
        int64_t keys;
        char *ptr;

    public:
        entry(){
            keys = __LONG_MAX__;
            ptr = NULL;
        }

        friend class page;
        friend class futNode;
        friend class globalNode;
        friend class fBtree;
};

const int cardinality = (PAGESIZE - sizeof(globalNode)) / sizeof(entry);

class future_Node{
    public:
        int64_t keys[cardinality];
        int entry_count;
        bool is_done;
        future_Node *next;
        future_Node *prev;

        future_Node(){  
            keys[0] = NULL;
            entry_count = 0;
            is_done = false;
            next = NULL;
            prev = NULL;
        }
        friend class fBtree;
};

class fBtree{
    private:
        int height;
        char *root;
        //std::atomic<bool> fb_lock;
        //futNode *fnode;
        future_Node *local_fut;
        future_Node *local_fut_tail;
        HashMapTable *hash;

    public:
        fBtree();
        bool is_Done = false;
        void setNewRoot(char *);
        void getNumberOfNodes();
        void fbtree_insert(int64_t, char *);
        void fbtree_insert(int64_t[], int);
        void fbtree_insert_internal(char *, int64_t, char *, uint32_t);
        void fbtree_delete(int64_t);
        void fbtree_delete_internal(int64_t, char *, uint32_t, int64_t *,
                             bool *, page **);
        char *fbtree_search(int64_t);
        void fbtree_search_range(int64_t, int64_t, unsigned long *);
        void printAll();
        void printLocalFutures(fBtree *, int);
        //void futEvaluate(fBtree *, int);
        //void futInsert(fBtree *bt, int64_t, char *, int, bool);
        void future_Insert(int64_t, int, bool);
        //void future_Evaluate(fBtree *, int);
        void fut_Evaluate(fBtree *, int);
        void fut_Evaluate_execute(fBtree *, int, int);
        void future_evaluate_execute(fBtree *, int, int);

        friend class page;
};

class page{
    private:
        globalNode gnode;
        entry records[cardinality];

    public:
        friend class fBtree;

        page(uint32_t level = 0){
            gnode.level = level;
            records[0].ptr = NULL;
        }

        //called to grow the global tree
        page(page *left, int64_t key, page *right, uint32_t level = 0){
            gnode.leftmost_ptr = left;
            gnode.level = level;
            records[0].keys = key;
            records[0].ptr = (char *)right;
            records[1].ptr = NULL;

            gnode.last_index = 0;

            clflush((char *)this, sizeof(page));
        }

        void *operator new(size_t size){
            void *ret;
            posix_memalign(&ret, 64, size);
            return ret;
        }

        inline int count(){
            uint8_t previous_switch_counter;
            int count = 0;

            do{
                previous_switch_counter = gnode.switch_counter;
                count = gnode.last_index + 1;

                while (count >= 0 && records[count].ptr != NULL)
                {
                    if(IS_FORWARD(previous_switch_counter))  ++count;
                    else  --count;
                }
                
                if(count < 0){
                    count = 0;
                    while (records[count].ptr != NULL)  +count;
                }
            } while(previous_switch_counter != gnode.switch_counter);
            
            return count;
        }

        inline void insert_key(int64_t key, char *ptr, int *num_entries, bool flush = true, bool update_last_index = true){
            if(!IS_FORWARD(gnode.switch_counter))
                ++gnode.switch_counter;

            if(*num_entries == 0){ //node is empty
                entry *new_entry = (entry *)&records[0];
                entry *array_end = (entry *)&records[1];
                new_entry->keys = (int64_t)key;
                new_entry->ptr = (char *)ptr;
                array_end->ptr = (char *)NULL;

                if(flush)   clflush((char *)this, CACHELINE_SIZE);
            } else{
                int i = *num_entries-1, inserted = 0, to_flush_cnt = 0;
                records[*num_entries+1].ptr = records[*num_entries].ptr;
                if(flush){
                    if((uint64_t) & (records[*num_entries+1].ptr) % CACHELINE_SIZE == 0)
                        clflush((char *)&(records[*num_entries+1].ptr), sizeof(char *));
                }

                //FAST
                for (i = *num_entries-1; i >= 0; i--)
                {
                    if (key < records[i].keys) {
                    records[i + 1].ptr = records[i].ptr;
                    records[i + 1].keys = records[i].keys;

                    if (flush) {
                        uint64_t records_ptr = (uint64_t)(&records[i + 1]);

                        int remainder = records_ptr % CACHELINE_SIZE;
                        bool do_flush =
                            (remainder == 0) ||
                            ((((int)(remainder + sizeof(entry)) / CACHELINE_SIZE) == 1) &&
                            ((remainder + sizeof(entry)) % CACHELINE_SIZE) != 0);
                        if (do_flush) {
                        clflush((char *)records_ptr, CACHELINE_SIZE);
                        to_flush_cnt = 0;
                        } else
                        ++to_flush_cnt;
                    }
                    } else {
                    records[i + 1].ptr = records[i].ptr;
                    records[i + 1].keys = key;
                    records[i + 1].ptr = ptr;

                    if (flush)
                        clflush((char *)&records[i + 1], sizeof(entry));
                    inserted = 1;
                    break;
                    }
                }
                if(inserted == 0){
                    records[0].ptr = (char *)gnode.leftmost_ptr;
                    records[0].keys = key;
                    records[0].ptr = ptr;
                    if(flush)  clflush((char *)&records[0], sizeof(entry));
                }
            }
            if(update_last_index){
                gnode.last_index = *num_entries;
            }
            ++(*num_entries);
        }

        //Key-based insertion
        page *store(fBtree *bt, char *left, int64_t key, char *right, bool flush, bool with_lock, page *invalide_sibling = NULL){
            //printf("GlobalStore. Key: %ld \n", key);
            if(with_lock)
                gnode.mtx->lock();
            if(gnode.is_deleted){
                if(with_lock)   gnode.mtx->unlock();
                return NULL;
            }

            if(gnode.sibling_ptr && (gnode.sibling_ptr != invalide_sibling)){
                if(key > gnode.sibling_ptr->records[0].keys){
                    if(with_lock)   gnode.mtx->unlock();
                    //printf("Recursive Calling Global Store\n");
                    return gnode.sibling_ptr->store(bt, NULL, key, right, true, with_lock, invalide_sibling);
                }
            }

            register int num_entries = count();

            //FAST
            if(num_entries < cardinality -1){
                insert_key(key, right, &num_entries, flush);

                if(with_lock)   gnode.mtx->unlock();
                return this;
            } else{ //FAIR
                page *sibling = new page(gnode.level);
                register int m = (int)ceil(num_entries/2);
                int64_t split_key = records[m].keys;

                //migrate half of the keys into the sibling.
                int sibling_cnt = 0;
                if(gnode.leftmost_ptr == NULL) {//leaf node
                    for(int i = m; i < num_entries; ++i){
                        sibling->insert_key(records[i].keys, records[i].ptr, &sibling_cnt, false);
                    }
                } else {
                    for(int i = m+1; i < num_entries; ++i){
                        sibling->insert_key(records[i].keys, records[i].ptr, &sibling_cnt, false);
                    }
                    sibling->gnode.leftmost_ptr = (page *)records[m].ptr;
                }

                sibling->gnode.sibling_ptr = gnode.sibling_ptr;
                clflush((char *)sibling, sizeof(page));

                gnode.sibling_ptr = sibling;
                clflush((char *)&gnode, sizeof(gnode));

                //set to NULL
                if(IS_FORWARD(gnode.switch_counter))
                    gnode.switch_counter += 2;
                else
                    ++gnode.switch_counter;
                
                records[m].ptr = NULL;
                clflush((char *)&records[m], sizeof(entry));

                gnode.last_index = m -1;
                clflush((char *)&gnode.last_index, sizeof(int16_t));

                num_entries = gnode.last_index+1;

                page *ret;

                //insert the key
                if(key <split_key){
                    insert_key(key, right, &num_entries);
                    ret = this;
                } else {
                    sibling->insert_key(key, right, &sibling_cnt);
                    ret = sibling;
                }

                //set a new root or insert the split key to the parent
                if(bt->root = (char*)this){
                    page *new_root = new page((page*)this, split_key, sibling, gnode.level+1);
                    bt->setNewRoot((char *)new_root);

                    if(with_lock)   gnode.mtx->unlock();
                } else{
                    if(with_lock)   gnode.mtx->unlock();
                    bt->fbtree_insert_internal(NULL, split_key, (char *)sibling, gnode.level+1);
                }
                return ret;
            }
        }

        //Node-based insertion.
        page *node_store(fBtree *fb, int64_t fut_rcd[], bool with_lock, page *invalide_sibling = NULL){
            //printf("Global Node Store\n");
            if (with_lock) {
                gnode.mtx->lock(); // Lock the write lock
                }
                if (gnode.is_deleted) {
                if (with_lock) {
                    gnode.mtx->unlock();
                }

                return NULL;
            }
            //To Do:
            // Simply implement it as linkedlist.
            if(gnode.sibling_ptr && (gnode.sibling_ptr != invalide_sibling)){
                if(fut_rcd[0] > gnode.sibling_ptr->records[0].keys 
                || fut_rcd[0] > gnode.sibling_ptr->records[gnode.last_index].keys){
                    //printf("Key is greater than sibling's key\n");
                    if(with_lock)
                        gnode.mtx->unlock();
                    return gnode.sibling_ptr->node_store(fb, fut_rcd, invalide_sibling);
                }
            }

            register int num_entries = count();
            
            page *new_sibling = new page(gnode.level);

            int sibling_cnt = 0;

            for (int i = 0; i < cardinality; i++){
                std::swap(new_sibling->records[i].keys, fut_rcd[i]);
                char *ptr = (char *)fut_rcd[i];
                std::swap(new_sibling->records[i].ptr, ptr);
                clflush((char *)&(new_sibling->records[i]), sizeof(entry));
            }//till here

            if(gnode.sibling_ptr != NULL){
                new_sibling->gnode.sibling_ptr = gnode.sibling_ptr;
                clflush((char *)new_sibling, sizeof(page));

                gnode.sibling_ptr = (page *)new_sibling;
                clflush((char *)&gnode, sizeof(gnode));

            } else{
                gnode.sibling_ptr = (page *)new_sibling;
                clflush((char *)&gnode, sizeof(gnode));
            }

            //set to NULL
            if(IS_FORWARD(gnode.switch_counter))
                gnode.switch_counter += 2;
            else
                ++gnode.switch_counter;

            register int m = (int)ceil(num_entries/2);
            records[m].ptr = NULL;
            clflush((char *)&records[m], sizeof(entry));

            gnode.last_index = m -1;
            clflush((char *)&gnode.last_index, sizeof(int16_t));

            num_entries = gnode.last_index+1;

            //set a new root or insert the split key to the parent
            if(fb->root = (char*)this){
                page *new_root = new page((page*)this, new_sibling->records[0].keys, new_sibling, gnode.level+1);
                fb->setNewRoot((char *)new_root);

                if(with_lock)
                    gnode.mtx->unlock();
            } else{
                    if(with_lock)   gnode.mtx->unlock();
                    fb->fbtree_insert_internal(NULL, new_sibling->records[0].keys, (char *)new_sibling, gnode.level+1);
            }
            return new_sibling;
        }

        void linear_search_range(int64_t min, int64_t max, unsigned long *buf){
            int i, off = 0;
            uint8_t previous_switch_count;
            page *current = this;

            while(current){
                int old_off = off;
                do{
                    previous_switch_count = current->gnode.switch_counter;
                    off = old_off;

                    int64_t tmp_key;
                    char *tmp_ptr;

                    if(IS_FORWARD(previous_switch_count)){
                        if((tmp_key = current->records[0].keys) > min){
                            if(tmp_key < max){
                                if((tmp_ptr = current->records[0].ptr) != NULL){
                                    if(tmp_key == current->records[0].keys){
                                        if(tmp_ptr) buf[off++] = (unsigned long) tmp_ptr;
                                    }
                                }
                            } else  return;
                        }
                        for(i = 1; current->records[i].ptr != NULL; i++){
                            if((tmp_key = current->records[i].keys) > min){
                                if(tmp_key < max){
                                    if((tmp_ptr = current->records[i].ptr) != current->records[i-1].ptr){
                                        if(tmp_key == current->records[i].keys){
                                            if(tmp_ptr) buf[off++] = (unsigned long)tmp_ptr;
                                        }
                                    }
                                } else  return;
                            }
                        }
                    } else {
                        for(i = count() -1; i > 0; --i){
                            if ((tmp_key = current->records[i].keys) > min) {
                                if (tmp_key < max) {
                                    if ((tmp_ptr = current->records[i].ptr) !=
                                        current->records[i - 1].ptr) {
                                    if (tmp_key == current->records[i].keys) {
                                        if (tmp_ptr)
                                        buf[off++] = (unsigned long)tmp_ptr;
                                    }
                                    }
                                } else
                                    return;
                            }
                        }

                        if ((tmp_key = current->records[0].keys) > min) {
                            if (tmp_key < max) {
                            if ((tmp_ptr = current->records[0].ptr) != NULL) {
                                if (tmp_key == current->records[0].keys) {
                                if (tmp_ptr) {
                                    buf[off++] = (unsigned long)tmp_ptr;
                                }
                                }
                            }
                            } else
                            return;
                        }
                    }
                } while(previous_switch_count != current->gnode.switch_counter);

                current = current->gnode.sibling_ptr;
            }
        }

        char *linear_search(int64_t key){
            int i =1;
            uint8_t previous_switch_counter;
            char *ret = NULL;
            char *t;
            int64_t k;

            if(gnode.leftmost_ptr == NULL){ //search a leaf node
                do{
                    previous_switch_counter = gnode.switch_counter;
                    ret = NULL;

                    //search from left to right
                    if(IS_FORWARD(previous_switch_counter)){
                        if((k = records[0].keys) == key){
                            if((t = records[0].ptr) != NULL){
                                if(k == records[0].keys){
                                    ret = t;
                                    continue;
                                }
                            }
                        }

                        for(i = 1; records[i].ptr != NULL; ++i){
                            if((k = records[i].keys) == key){
                                if(records[i-1].ptr != (t = records[i].ptr)){
                                    if(k == records[i].keys){
                                        ret = t;
                                        break;
                                    }
                                }
                            }
                        }
                    } else { //search from right to left
                        for(i = count() - 1; i > 0; --i){
                            if((k = records[i].keys) == key){
                                if(records[i-1].ptr != (t = records[i].ptr)){
                                    if(k == records[i].keys){
                                        ret = t;
                                        break;
                                    }
                                }
                            }
                        }

                        if(!ret){
                            if((k = records[0].keys) == key){
                                if(NULL != (t = records[0].ptr) && t){
                                    if(k == records[0].keys){
                                        ret = t;
                                        continue;
                                    }
                                }
                            }
                        }
                    }                    
                }while(gnode.switch_counter != previous_switch_counter);

                if(ret) return ret;

                if((t = (char *)gnode.sibling_ptr) && key >= ((page *)t)->records[0].keys)
                    return t;
                
                return NULL;
            } else { //internal node
                do{
                    previous_switch_counter = gnode.switch_counter;
                    ret = NULL;

                    if(IS_FORWARD(previous_switch_counter)){
                        if(key < (k = records[0].keys)){
                            if((t = (char *)gnode.leftmost_ptr) != records[0].ptr){
                                ret = t;
                                continue;
                            }
                        }

                        for(i = 1; records[i].ptr != NULL; ++i){
                            if(key < (k = records[i].keys)){
                                if((t = (char *)gnode.leftmost_ptr) != records[i].ptr){
                                    ret = t;
                                    break;
                                }
                            }
                        }

                        if(!ret){
                            ret = records[i-1].ptr;
                            continue;
                        }
                    } else{  //search from right to left
                        for(i = count() - 1; i >= 0; --i){
                            if(key > (k = records[i].keys)){
                                if(i == 0){
                                    if((char *)gnode.leftmost_ptr != (t = records[i].ptr)){
                                        ret = t;
                                        break;
                                    }
                                } else{
                                    if(records[i-1].ptr != (t = records[i].ptr)){
                                        ret = t;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                } while(gnode.switch_counter != previous_switch_counter);

                if((t = (char *)gnode.sibling_ptr) != NULL){
                    if(key >= ((page *)t)->records[0].keys)
                        return t;
                }

                if(ret){
                    return ret;
                } else 
                    return (char *)gnode.leftmost_ptr;
            }
            return NULL;
        }

    
};

fBtree::fBtree(){
    printf("FB Construct\n");
    root = (char *) new page();
    height = 1;
    local_fut = (future_Node *)new future_Node[n_threads];
    local_fut_tail = (future_Node *) new future_Node[n_threads];
    hash = (HashMapTable *)new HashMapTable[n_threads];
}

void fBtree::setNewRoot(char *new_root){
    this->root = (char *)new_root;
    clflush((char *)&(this->root), sizeof(char *));
    ++height;
}

char *fBtree::fbtree_search(int64_t key){
    page *p = (page *)root;

    while(p->gnode.leftmost_ptr != NULL){
        p = (page *)p->linear_search(key);
    }

    page *t;
    while ((t = (page *)p->linear_search(key)) == p->gnode.sibling_ptr){
        p = t;
        if(!p){
            break;
        }
    }

    //TODO: Add the search mechanism for per-thread futures as well.

    if(!t){
        printf("NOT FOUND %lu, t = %x\n", key, t);
        return NULL;
    }

    return (char *)t;
    
}

    
//Thread Local Futures linked list
void fBtree::future_Insert(int64_t key, int tid, bool isDone=false){
    //printf("Future insert starts\n");
  if(local_fut == NULL){
    local_fut = new future_Node[n_threads];
  } else{
    if(local_fut[tid].next == NULL){
      //Create a new node next to the dummy head node.
      future_Node *first_node = new future_Node();
      first_node->keys[0] = key;
      first_node->entry_count += 1;
      first_node->prev = &(local_fut[tid]);
      local_fut[tid].next = first_node;
      local_fut_tail[tid].next = first_node;
      local_fut[tid].entry_count += 1;
      hash[tid].Insert(key, tid);
      clflush((char *)this, CACHELINE_SIZE);
    } 
    else if(local_fut[tid].next->entry_count == cardinality ){
      //Create a new node and perform the evaluation on the previous node.
      //Once evaluation is done, free the previous node and move the head node's next pointer to the current node who's next pointer is NULL
      future_Node *new_node = new future_Node();

      new_node->keys[0] = key;
      new_node->entry_count += 1;
      new_node->prev = &(local_fut[tid]);
      new_node->next = local_fut[tid].next;
      local_fut[tid].next->prev = new_node;
      local_fut[tid].next = new_node;
      local_fut[tid].entry_count += 1;

      hash[tid].Insert(key, tid);

      clflush((char *)this, CACHELINE_SIZE);
    } 
    else{
      
      //printf("Sorting the keys\n");
      for(int i = local_fut[tid].next->entry_count-1; i >=0; i--){
          if(key < local_fut[tid].next->keys[i]){
            local_fut[tid].next->keys[i+1] = local_fut[tid].keys[i];

            //flush 
            uint64_t recd_ptr = (uint64_t)(&local_fut[tid].next->keys);
            int remainder = recd_ptr % CACHELINE_SIZE;
            bool do_flush = (remainder == 0) ||
                        ((((int)(remainder + sizeof(future_Node)) / CACHELINE_SIZE) == 1) &&
                        ((remainder + sizeof(future_Node)) % CACHELINE_SIZE) != 0);
            if(do_flush){
                clflush((char *)local_fut[tid].next->keys, CACHELINE_SIZE);
            } 
          } else{
              local_fut[tid].next->keys[i+1] = key;
              local_fut[tid].next->entry_count += 1;
              hash[tid].Insert(key, tid);

              clflush((char *)&local_fut[tid].next->keys[i+1], sizeof(entry));
              break;
            }
        }
    }
  }

  // printf("Entry Count: %ld, TId: %d\n", local_fut[tid].entry_count, tid);
  /*if(local_fut[tid].entry_count > 1000){
    //printf("Sleeping\n");
    std::this_thread::sleep_for (std::chrono::microseconds(200));
  }*/
 
}

//Key-based insertion
void fBtree::fbtree_insert(int64_t key, char *right){
    page *p = (page *)root;
    

    while(p->gnode.leftmost_ptr != NULL)
        p = (page *)p->linear_search(key);

    if(!p->store(this, NULL, key, right, true, true))
        fbtree_insert(key, right);
    
}

//node-based insertion
void fBtree::fbtree_insert(int64_t rcd[], int num_entries){
    //printf("Global NOde Insertion\n");
    page *p = (page *)root;
    //printf("Root Node: %x\n", root);

    while(p->gnode.leftmost_ptr != NULL)
        p = (page *)p->linear_search(rcd[0]);

    if(!p->node_store(this, rcd, true))
        fbtree_insert(rcd, num_entries);
}

void fBtree::fut_Evaluate(fBtree *fb, int tid){
    //mutli-threaded Future Evaluate
    int prod_to_cons;
    do{
        if(local_fut[tid].entry_count == 0){
            //std::this_thread::sleep_for (std::chrono::microseconds(1000));
            continue;
        } else{
            if(tid == 0){
                prod_to_cons = ((n_threads/eval_threads)+(n_threads%eval_threads));
                future_evaluate_execute(fb, tid, prod_to_cons);
            }else{
                prod_to_cons = (n_threads/eval_threads);
                future_evaluate_execute(fb, tid, prod_to_cons);
            }
        }
        for(int i = tid; i < n_threads; i++){
            if(local_fut[i].is_done){
                fb->is_Done = true;
            }
        }
    }while(!fb->is_Done);
}

void fBtree::fut_Evaluate_execute(fBtree *fb, int tid, int total_t){
    for(int i = tid; i < (tid + total_t); i++){
        //for(int i = tid; i < n_threads; i++){    
            future_Node *last = NULL;
            while (last != fb->local_fut[i].next){
                future_Node *current = fb->local_fut[i].next;
                while(current->next != last)
                    current = current->next;

                //fb->fbtree_insert(current->keys);
                //Key-based insertion
                /*while (current->entry_count != 0)
                {
                    int i = current->entry_count;
                    int64_t key = current->keys[i];
                    fb->fbtree_insert(key, (char *)key);
                    current->entry_count -= 1;
                }*/

                //Node-based insertion
                if(current->entry_count == cardinality-1){
                    fb->fbtree_insert(current->keys, current->entry_count);
                } 
                
                last = current;
                fb->local_fut[i].entry_count -= 1;
                delete current;
                if(fb->local_fut[i].entry_count == 0){
                    fb->local_fut[i].is_done = true;
                }           
        }
    }
}

//Using Tail Pointer.
void fBtree::future_evaluate_execute(fBtree *bt, int tid, int total_t){
  for(int i = (tid*total_t); i < ((tid+1) * total_t); i++){
    future_Node *last_node = NULL;  
    //printf("Last Node not NULL\n");
    while(last_node != bt->local_fut[i].next){
      future_Node *tail_node = bt->local_fut_tail[i].next;
        /*for(int k = 0; k <= tail_node->entry_count; k++)
          bt->fbtree_insert(tail_node->keys[k], (char *)tail_node->keys[k]);*/

        if(tail_node->entry_count == cardinality){
            bt->fbtree_insert(tail_node->keys, tail_node->entry_count);
        } 


        last_node = tail_node->prev;
        bt->local_fut_tail[i].next = tail_node->prev;
        bt->local_fut[i].entry_count -= 1;
        delete tail_node;

        if(bt->local_fut[i].entry_count == 0){
          bt->local_fut[i].is_done = true;
          break;
        }
      }
  }
  //printf("Evaluate Execute Returns\n");
}

void fBtree::printLocalFutures(fBtree *bt, int tid){
    future_Node *tmp = NULL;
    tmp = bt->local_fut[tid].next;

    while (tmp != NULL)
    {
        for(int i = 0; i < tmp->entry_count; i++){
            printf("Key: %lld \n", tmp->keys[i]);
        }
        tmp = tmp->next;
    }
    
}

void fBtree::fbtree_insert_internal(char *left, int64_t key, char *right, uint32_t level){
    if(level > ((page *)root)->gnode.level)
        return;
    
    page *p = (page *)this->root;

    while(p->gnode.level > level){
        p = (page *)p->linear_search(key);
    }

    if(!p->store(this, NULL, key, right, true, true)){
        fbtree_insert_internal(left, key, right, level);
    }
}

// Function to search keys from "min" to "max"
void fBtree::fbtree_search_range(int64_t min, int64_t max,
                               unsigned long *buf) {
  page *p = (page *)root;

  while (p) {
    if (p->gnode.leftmost_ptr != NULL) {
      // The current page is internal
      p = (page *)p->linear_search(min);
    } else {
      // Found a leaf
      p->linear_search_range(min, max, buf);

      break;
    }
  }
}

/*void fBtree::fbtree_delete(int64_t key) {
  page *p = (page *)root;

  while (p->gnode.leftmost_ptr != NULL) {
    p = (page *)p->linear_search(key);
  }

  page *t;
  while ((t = (page *)p->linear_search(key)) == p->gnode.sibling_ptr) {
    p = t;
    if (!p)
      break;
  }

  if (p) {
    if (!p->remove(this, key)) {
      btree_delete(key);
    }
  } else {
    printf("not found the key to delete %lu\n", key);
  }
}
*/