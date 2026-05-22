#include"parallax_team.h"
#include<stdio.h>
#include<pthread.h>
#include<unistd.h>
#include "barrier.h"
#include<string.h>
#include<stdlib.h>



void * thread_func_test(void * arg){

    worker_context * param=(worker_context * ) arg;
    printf("Thread with tid %d started \n",param->tid);
    sleep(param->tid*3);

    
    printf("Thread  with tid %d resumed  and died\n",param->tid);
    barrier_wait(param->barrier);
    pthread_exit(NULL);
}




int main(){


   /*
    pthread_t thread1;
    pthread_t thread2;
    pthread_t thread3;
    barrier_t * barrier=barrier_init(3);
   
    thread_param param1,param2,param3;
    param1.tid=1;
    param1.barrier=barrier;
    param2.tid=2;
    param2.barrier=barrier;
    param3.tid=3;
    param3.barrier=barrier;
    
    printf("Main thread started\n");
    pthread_create(&thread1,NULL,thread_func_test, &param1);
    pthread_create(&thread2,NULL,thread_func_test, &param2);
    pthread_create(&thread3,NULL,thread_func_test, &param3);


    pthread_join(thread1,NULL);
    pthread_join(thread2,NULL);
    pthread_join(thread3,NULL);
    printf("Main thread Continued\n");
   */


   team * t=team_init(3);
   team_start(t);
   team_wait(t);
   team_destroy(t);

}


team *  team_init( int nb_threads) {

    team * new_team=(team *)malloc(sizeof(team));
    
    new_team->num_workers = nb_threads;

    new_team->workers =    malloc(sizeof(worker_t) * nb_threads);

    new_team->barrier = barrier_init(nb_threads);

    for (int i = 0; i < nb_threads; i++) {

        new_team->workers[i].id = i;

        new_team->workers[i].func = thread_func_test;
        

        worker_context * context=(worker_context * )malloc(sizeof(worker_context));

        context->tid=i;
        context->barrier=new_team->barrier;
        context->args=NULL;
        new_team->workers[i].context=context;
          
    }

    

    new_team->reduce_fxn = NULL;

    new_team->tasks = NULL;

    new_team->results =
        malloc(sizeof(void *) * nb_threads);

    return new_team;
}


int  team_start(team *team) {

    for (int i = 0; i < team->num_workers; i++) {

        pthread_create(
            &team->workers[i].tid,
            NULL,
            team->workers[i].func,
           (void *) team->workers[i].context
        );
    }

    return 0;
}

int team_wait(team * team){
    for(int i=0;i<team->num_workers;i++){
    pthread_join(team->workers[i].tid,NULL);
    
}
return 0;
}



void team_destroy(team *t){
    if (!t) return;

    for (int i = 0; i < t->num_workers; i++){
        free(t->workers[i].context);
    }

    free(t->workers);
    free(t->results);
    free(t->barrier);

    free(t);
}