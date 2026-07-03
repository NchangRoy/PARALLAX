#include"barrier.h"


barrier_t * barrier_init(int count){
    barrier_t * barrier=(barrier_t *)malloc(sizeof(barrier_t));
    atomic_init(&barrier->count,0);
    atomic_init(&barrier->generation,0);
    barrier->total=count;
    return barrier;
}


void barrier_wait(barrier_t * barrier){
    //first need to load the barrier value
    int generation = atomic_load(&barrier->generation);
    //increment old value and get old value
    int count = atomic_fetch_add(&barrier->count,1);
    count++;
    if(count == barrier->total){

        //reset count
        atomic_store(&barrier->count,0);
        //increase the generation count
        atomic_fetch_add(&barrier->generation,1);
        //we wake up threads waiting on the generation change
        futex_wake(&barrier->generation,barrier->total);

    }
    else{
        //cause thread to sleep if the generation has not changed.
        //Must wait on `generation` itself (not `count`): FUTEX_WAIT only
        //blocks if *addr still equals the expected value at the moment of
        //the syscall. Waiting on &count while comparing against `generation`
        //compares unrelated values, so the check almost always fails and
        //the call returns immediately instead of sleeping - turning this
        //into a busy-spin instead of a real wait.
        while( atomic_load(&barrier->generation) ==generation){

            futex_wait(&barrier->generation,generation);
        }
    }

}