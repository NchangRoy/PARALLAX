#ifndef PARALLAX_TEAM_H
#define PARALLAX_TEAM_H

#include"linked_list.h"
#include"pthread.h"
#include"barrier.h"
#include<stdbool.h>

typedef void * (* thread_func) (void *);

typedef struct {
    bool useGPU;
    int nbCPUS;


} compute_spec;


typedef struct {
    int tid;
    barrier_t * barrier;
    void * args;
} worker_context;

typedef  struct {
    char ip[16];
    int port;
    char uuid[64];
} worker_node;

typedef struct {
    pthread_t tid;
    int id;
    thread_func func;
    worker_context * context;
    worker_node * exec_node;
    

} worker_t;


typedef struct {
    
    char * name;
    void * data;
    int thread_id;

} task_t;   

typedef struct {
    worker_t  * workers;
    task_t * tasks;

    barrier_t * barrier;
    void * (* reduce_fxn)(void * ,void *);
    int num_workers;
    void * * results;//will have an array with size =num_threads


} team;


team * team_init(int num_threads);
int team_start(team * team);
int team_wait(team * team);
void team_destroy(team * team);





#endif