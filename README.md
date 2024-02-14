Implementation of the paper, "Scalable NUMA-aware Persistent B+-Tree for Non-Volatile Memory Devices".

The paper is to appear in Cluster Computing Journal 2022.

This work is an extension of FAST and FAIR based Persistent B+-tree published in USENIX FAST 2018. In this work, we identified the limitation of FAST&FAIR atop manycore machines and proposed a Future-based per-core (thread) buffer on top of B+-tree and let the application directly perform operation to the per-code buffer while the dedicated asynchronous threads are responsible for flushing the data from per-core buffers to global B+-tree. The rest of the explanation is same the FAST&FAIR B+-tree. 

Failure-Atomic ShifT(FAST) and Failure-Atomic In-place Rebalancing(FAIR) are simple and novel algorithms that make B+-Tree tolerant againt system failures without expensive COW or logging for Non-Volatile Memory(NVM). A B+-Tree with FAST and FAIR can achieve high performance comparing to the-state-of-the-art data structures for NVM. Because the B+-Tree supports the sorted order of keys like a legacy B+-Tree, it is also beneficial for range queries.

In addition, a read query can detect a transient inconsistency in a B+-Tree node during a write query is modifying it. It allows read threads to search keys without any lock. That is, the B+-Tree with FAST and FAIR increases throughputs of multi-threaded application with lock-free search.

We strongly recommend to refer to the paper for the details.

git clone https://github.com/safdarjamil95/F3-tree.git
cd F3-tree
make
There are two versions of concurrent test programs - One is only search and only insertion, the other is a mixed workload.
./fbtree_concurrent -n [the # of data] -w [write latency of NVM] -i [input path] -t [the # of threads] (e.g. ./btree -n 10000 -w 300 -i ~/input.txt -t 16)
