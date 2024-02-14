#include <cassert>
#include <climits>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>
#include <future>
#include "hash.h"

int main(int argc, char **argv){
    //Parsing arguments
    int num_data = 0;
    int n_threads = 1;
    int index;
    char *input_path = (char *)std::string("../sample_input.txt").data();

    int c;
    while((c = getopt(argc, argv, "n:i:t")) != -1){
        switch (c)
        {
        case 'n':
            num_data = atoi(optarg);
            break;
        case 't':
            n_threads = atoi(optarg);
            break;
        case 'i':
            input_path = optarg;

        default:
            break;
        }
    }

    int64_t *keys = new int64_t[num_data];
    T_S = num_data;
    HashMapTable *hash;
    
    ifstream ifs;
    ifs.open(input_path);

    if(!ifs)
        cout << "input loading error!"<<endl;

    for(int i = 0; i < num_data; ++i){
        ifs >> keys[i];
    }
    ifs.close();

    vector<future<void>> futures(n_threads);

    long data_per_thread = num_data / n_threads;

    for (int tid = 0; tid < n_threads; tid++) {
        int threadID = tid;
        int from = data_per_thread * tid;
        int to = (tid == n_threads - 1) ? num_data : from + data_per_thread;

        /*auto f = async(launch::async,
                    [&hash, &keys, &tid](int from, int to) {
                        for (int i = from; i < to; ++i)
                        hash->Insert(keys[i], tid);
                    },
                    from, to);*/
        for(int i = from; i < to; ++i){
            auto f = std::async(std::launch::deferred, &HashMapTable::Insert, (*hash), keys[i], tid);
            futures.push_back(move(f));
        }
        
  }

    cout<< "Keys inserted!"<<endl;

    for(int j = num_data-1; j >= 0; --j){
        index = hash->SearchKey(keys[j]);
        if(index != -1)
            cout<<"Element at Key"<< index<<endl;
        else
            cout<<"No element found!"<<endl;
    }

    for(int k = 0; k < num_data; k++){
        hash->Remove(keys[k]);
    }
    cout<<"Keys Removed!"<<endl;

    return 0;
}