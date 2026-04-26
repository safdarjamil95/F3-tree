#include "btree.h"

using namespace std::chrono;

void clear_cache(){
    //remove cache
    int size = 256*1024*1024;
    char *garbage = new char[size];
    for (int i = 0; i < size; ++i)
        garbage[i] = i;
    for(int i = 100; i < size; ++i)
        garbage[i] += garbage[i-100];
    delete[] garbage;
}

int main(int argc, char **argv){
    //Parsing arguments
    int num_data = 0;
    int n_thrds = 1;
    char *input_path = (char *)std::string("../sample_input.txt").data();

    int c;
    while((c = getopt(argc, argv, "n:w:t:i:")) != -1){
        switch (c)
        {
        case 'n':
            num_data = atoi(optarg);
            break;
        case 'w':
            write_latency_in_ns = atol(optarg);
            break;
        case 't':
            n_thrds = atoi(optarg);
            break;
        case 'i':
            input_path = optarg;
        default:
            break;
        }
    }

    n_threads = n_thrds;
    T_S = num_data;
    eval_threads = 1;
    btree *bt;
    bt = new btree();

    struct timespec start, end;

    //reading data
    int64_t *keys = new int64_t[num_data];
    long long elapsedTime;

    ifstream ifs;
    ifs.open(input_path);

    if(!ifs)
        cout << "input loading error!"<<endl;

    for(int i = 0; i < num_data; ++i){
        ifs >> keys[i];
    }
    ifs.close();

    //Initializing stats
    clflush_cnt = 0;
    search_time_in_insert = 0;
    clflush_time_in_insert = 0;
    gettime_cnt = 0;
    


    vector<future<void>> futures(n_threads);
    long data_per_thread = num_data/n_threads;

    //insert
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    for(int tid = 0; tid < n_threads; tid++){
        int from = data_per_thread * tid;
        
        int to = (tid == n_threads-1) ? num_data : from + data_per_thread;

        auto f = async( [&bt, &keys, tid](int from, int to){
            for(int i = from; i < to; ++i)
                bt->future_insert(keys[i], tid);
        }, from, to);
       
        futures.push_back(move(f));
    }

    for(auto &&f : futures)
        if(f.valid())
            f.get();  

    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsedTime = (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
    //cout << "Per-thread insertion with " << n_threads << " threads (usec) : " <<elapsedTime / 1000 << endl;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for(int e_tid = 0; e_tid < eval_threads; e_tid++){       
        auto e = std::async(std::launch::async, &btree::future_evaluate, (*bt), bt, e_tid);
        futures.push_back(move(e));
    }  

    /*for(auto &&f : futures)
        if(f.valid())
            f.get();
    */
    for(auto &&e : futures)
        if(e.valid())   
            e.get();

    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsedTime = (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
    //cout << "Execution time with " << eval_threads+n_threads << " threads (usec) : " <<elapsedTime / 1000 << endl;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int tid = 0; tid < n_threads; tid++) {
        int from = data_per_thread * tid;
        int to = (tid == n_threads - 1) ? num_data : from + data_per_thread;

        auto f = async([&bt, &keys](int from, int to) {
                        for (int i = from; i < to; ++i)
                        bt->btree_search(keys[i]);
                    },
                    from, to);
        futures.push_back(move(f));
    }
    for (auto &&f : futures)
        if (f.valid())
        f.get();

    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsedTime =
        (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
    cout << "Concurrent searching with " << n_threads << " threads (usec) : " << elapsedTime / 1000 << endl;

    clear_cache();
    futures.clear();


    do{
        if(bt->is_Done){
            delete bt;
            delete[] keys; 
            break;
        }
    }while(!bt->is_Done);

    /*delete bt;
    delete[] keys;*/

    return 0;
}