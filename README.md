Implementation of the paper, "Endurable Transient Inconsistency in Byte-Addressable Persistent B+-Tree".

The paper is to appear in FAST 2018.

Failure-Atomic ShifT(FAST) and Failure-Atomic In-place Rebalancing(FAIR) are simple and novel algorithms that make B+-Tree tolerant againt system failures without expensive COW or logging for Non-Volatile Memory(NVM). A B+-Tree with FAST and FAIR can achieve high performance comparing to the-state-of-the-art data structures for NVM. Because the B+-Tree supports the sorted order of keys like a legacy B+-Tree, it is also beneficial for range queries.

In addition, a read query can detect a transient inconsistency in a B+-Tree node during a write query is modifying it. It allows read threads to search keys without any lock. That is, the B+-Tree with FAST and FAIR increases throughputs of multi-threaded application with lock-free search.

We strongly recommend to refer to the paper for the details.

Directories

single - a single thread version without lock
concurrent - a multi-threaded version with std::mutex in C++11
How to run (single)

git clone https://github.com/DICL/FAST_FAIR.git
cd FAST_FAIR/single
make
./btree -n [the # of data] -w [write latency of NVM] -i [path] (e.g. ./btree -n 10000 -w 300 -i ~/input.txt)
How to run (concurrent)
git clone https://github.com/DICL/FAST_FAIR.git
cd FAST_FAIR/concurrent
make
There are two versions of concurrent test programs - One is only search and only insertion, the other is a mixed workload.
./btree_concurrent -n [the # of data] -w [write latency of NVM] -i [input path] -t [the # of threads] (e.g. ./btree -n 10000 -w 300 -i ~/input.txt -t 16)
./btree_concurrent_mixed -n [the # of data] -w [write latency of NVM] -i [input path] -t [the # of threads] (e.g. ./btree -n 10000 -w 300 -i ~/input.txt -t 16)
