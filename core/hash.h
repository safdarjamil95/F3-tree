#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <mutex>
using namespace std;
class HashTable {
   public:
      int64_t k;
      int v;
      HashTable(int64_t k, int v) {
         this->k = k;
         this->v = v;
      }
};
class DelNode:public HashTable {
   private:
      static DelNode *en;
      DelNode():HashTable(-1, -1) {}
   public:
      static DelNode *getNode() {
         if (en == NULL)
            en = new DelNode();
         return en;
      }
};

DelNode *DelNode::en = NULL;

class HashMapTable {
   private:
      HashTable **ht;
      int capacity;
      //std::mutex *hash_mtx;
   public:
      HashMapTable() : ht(nullptr), capacity(0) {
         //hash_mtx = new std::mutex();
      }
      void init(int cap) {
         capacity = cap > 0 ? cap : 1;
         ht = new HashTable* [capacity];
         for (int i = 0; i < capacity; i++) {
            ht[i] = NULL;
         }
      }
      int HashFunc(int64_t k) {
         if (capacity <= 0) return 0;
         // Use unsigned remainder so negative keys still map to [0, capacity).
         return static_cast<int>(
             static_cast<uint64_t>(k < 0 ? -k : k) % static_cast<uint64_t>(capacity));
      }

      void Insert(int64_t k, int v) {
         if (ht == nullptr || capacity == 0) return;
         //hash_mtx->lock();
         int hash_val = HashFunc(k);
         int init = -1;
         int delindex = -1;
         while (hash_val != init && (ht[hash_val]  == DelNode::getNode() || ht[hash_val] != NULL && ht[hash_val]->k != k)) {
            if (init == -1)
               init = hash_val;
            if (ht[hash_val] == DelNode::getNode())
               delindex = hash_val;
               hash_val = HashFunc(hash_val + 1);
         }
         if (ht[hash_val] == NULL || hash_val == init) {
            if(delindex != -1)
               ht[delindex] = new HashTable(k, v);
            else
               ht[hash_val] = new HashTable(k, v);
         }
         if(init != hash_val) {
            if (ht[hash_val] != DelNode::getNode()) {
               if (ht[hash_val] != NULL) {
                  if (ht[hash_val]->k== k)
                     ht[hash_val]->v = v;
               }
            } else
            ht[hash_val] = new HashTable(k, v);
         }
         //hash_mtx->unlock();
      }

      int SearchKey(int64_t k) {
         if (ht == nullptr || capacity == 0) return -1;
         int hash_val = HashFunc(k);
         int init = -1;
         while (hash_val != init && (ht[hash_val] == DelNode::getNode() || ht[hash_val] != NULL && ht[hash_val]->k!= k)) {
            if (init == -1)
               init = hash_val;
               hash_val = HashFunc(hash_val + 1);
         }
         if (ht[hash_val] == NULL || hash_val == init)
            return -1;
         else
            return ht[hash_val]->v;
      }

      void Remove(int64_t k) {
         if (ht == nullptr || capacity == 0) return;
         //hash_mtx->lock();
         int hash_val = HashFunc(k);
         int init = -1;
         while (hash_val != init && (ht[hash_val] == DelNode::getNode() || ht[hash_val] != NULL && ht[hash_val]->k!= k)) {
            if (init == -1)
               init = hash_val;
               hash_val = HashFunc(hash_val + 1);
         }
        
         if (hash_val != init && ht[hash_val] != NULL) {
            delete ht[hash_val];
            ht[hash_val] = DelNode::getNode();
         }
         //hash_mtx->unlock();
      }

      ~HashMapTable() {
         if (ht != nullptr) {
            delete[] ht;
         }
         //delete hash_mtx;
      }
};
