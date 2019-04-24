#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <memory.h>
#include <pthread.h>
#include <threads.h>
#include <assert.h>
#include <unistd.h>
#include "prodcon.h"
/*
    this is no such a full only when all the producers are finished
    then the consumers consume
    each consumer has its own lock and cond each producer uses the lock and cond as well
    all producer threads push string into the linked list(queue) parallel
    When seeing sentinel, it means all producers have finished, so we push sentinel after 
    pthread_join for all the producers threads
*/

// double pointer: https://www.eskimo.com/~scs/cclass/int/sx8.html
struct llist_node {
    struct llist_node *next;
    char *str;
};

static struct llist_node **heads; // *heads[] have a linked list for each of the consumers
static assign_consumer_f assign_consumer;
static int producer_count;
static int consumer_count;
thread_local int my_consumer_number; // so that each consumer thread has its own thread number
static run_producer_f run_producer;
static run_consumer_f run_consumer;
static char *sentinel; // this sentine string does not matter.

pthread_mutex_t *lock;
pthread_cond_t *cond;

typedef struct __myarg_t {
    int p_i;
    int c_i;
    int producer_count;
    int consumer_count;
    void *produce;
    void *consume;
    int new_argc;
    char **new_argv;
} myarg_t;

/**
 * pop a node off the start of the list.
 *
 * @param phead the head of the list. this will be modified by the call unless the list is empty
 * (*phead == NULL).
 * @return NULL if list is empty or a pointer to the string at the top of the list. the caller is
 * incharge of calling free() on the pointer when finished with the string.
 */
char *pop(struct llist_node **phead)
{
    
    char *s = (*phead)->str;
    
    if(s == sentinel) // compare the pointer but not the string itself
    { 
        return NULL; // once we see the sentinel, return NULL
    }
   
    struct llist_node *next = (*phead)->next;
    free(*phead);
    *phead = next;
    
    return s;
}

/**
 * push a node onto the start of the list. a copy of the string will be made.
 * @param phead the head of the list. this will be modified by this call to point to the new node
 * being added for the string.
 * @param s the string to add. a copy of the string will be made and placed at the beginning of
 * the list.
 */
void push(struct llist_node **phead, const char *s, int consumer)
{
    pthread_mutex_lock(&lock[consumer]);
    struct llist_node *new_node = malloc(sizeof(*new_node));
    struct llist_node *curr_node = *phead; // starting from the beiginning
    new_node->next = *phead;

    if(s == sentinel) { // add the sentinel  to the end of the list
        if(curr_node == NULL)   // this case is to handle if it just happens to have all strings consumed
        {                       // and the head pointer pointing to that list is NULL
            new_node->str = (char*)s;
            *phead = new_node;
        }else{  // this case is to handle there is at least one node in this list, so we add after it
            //int count = 0;
            while(curr_node->next != NULL) // loop throug to get the end of the list 
            {
                curr_node = curr_node->next;
                //count++;
            }
            new_node->str = (char*)s;  // we just want to compare the sentinel pointer 
            curr_node->next = new_node; 
        }
    }else{ // this case is to handle we no sentinel encountered
        new_node->str = strdup(s);
        *phead = new_node;
    }

    pthread_cond_signal(&cond[consumer]); // because now we produce one job, so wake up the consumer
    pthread_mutex_unlock(&lock[consumer]);   
}
// put into a queue into each consumer
// consumer has the linkes list, producer generate
// the array of list heads. the size should be equal to the number of consumers
// number of linked list == number of consumers == number of queues

void queue(int consumer, const char *str) // the nth consumer and the str 
{
    push(&heads[consumer], str, consumer);
}

// push these words into the list with string. Which list? ans: the consumer
// number we get by using assign_consumer function
// note: the producer not just pushes to one queue, it pushes strings to all these queues randomly
void produce(const char *buffer) 
{
    int hash = assign_consumer(consumer_count, buffer); 
    queue(hash, buffer); // to push the char * s into the buffer list
}

char *consume() { // remove   

    pthread_mutex_lock(&lock[my_consumer_number]); 
    while (heads[my_consumer_number] == NULL) { //
        pthread_cond_wait(&cond[my_consumer_number], &lock[my_consumer_number]);
    }   
    char *str = pop(&heads[my_consumer_number]);  // if the char* str == sentinel pointer
    pthread_cond_signal(&cond[my_consumer_number]);
    pthread_mutex_unlock(&lock[my_consumer_number]);
    return str; // return it to the consume() function
}

void do_usage(char *prog)
{
    printf("USAGE: %s shared_lib consumer_count producer_count ....\n", prog);
    exit(1);
}

// run_producer(i, producer_count, produce, new_argc, new_argv);
// run_consumer(i, consume, new_argc, new_argv);
void *start_produce_thread(void *args){ 
    myarg_t *m = (myarg_t *) args;  
    run_producer(m->p_i, m->producer_count, m->produce, m->new_argc, m->new_argv);
    return NULL;
}

void *start_consume_thread(void *args){    
    myarg_t *m = (myarg_t *) args;  
    my_consumer_number = m->c_i;     
    run_consumer(m->c_i, m->consume, m->new_argc, m->new_argv);
    return NULL;
}

int main(int argc, char **argv) 
{
    if (argc < 4) {
        do_usage(argv[0]);
    }  

    char *shared_lib = argv[1];
    producer_count = strtol(argv[2], NULL, 10); // string to long
    consumer_count = strtol(argv[3], NULL, 10);

    char **new_argv = &argv[4];
    int new_argc = argc - 4;
    setlinebuf(stdout);

    if (consumer_count <= 0 || producer_count <= 0) {
        do_usage(argv[0]);
    }

    void *dh = dlopen(shared_lib, RTLD_LAZY);
   
    // load the producer, consumer, and assignment functions from the library
    run_producer = dlsym(dh, "run_producer");
    run_consumer = dlsym(dh, "run_consumer");
    assign_consumer = dlsym(dh, "assign_consumer");
    if (run_producer == NULL || run_consumer == NULL || assign_consumer == NULL) {
        printf("Error loading functions: prod %p cons %p assign %p\n", run_producer,
                run_consumer, assign_consumer);
        exit(2);
    }

    // when we know how many consumer we have, we allocated memory
    heads = calloc(consumer_count, sizeof(*heads)); // each node is sizeof(*heads) = 8 bytes
    // locks_t locks[consumer_count];
    lock = calloc(consumer_count, sizeof(*lock));
    cond = calloc(consumer_count, sizeof(*cond));
    
    // initialize locks and conds
    for(int i = 0; i < consumer_count; i++)
        pthread_mutex_init(&lock[i], NULL);
    for(int i = 0; i < consumer_count; i++)
        pthread_cond_init(&cond[i], NULL);
    
    // producers and consumers need its own args passing into start_x_thread
    myarg_t pro_args[producer_count];
    myarg_t con_args[consumer_count];
    
    for(int i = 0; i < producer_count; i++)
    {   
        pro_args[i].new_argc = new_argc;
        pro_args[i].new_argv = new_argv;
        pro_args[i].produce = produce;
        pro_args[i].producer_count = producer_count;
    }
    for(int i = 0; i < consumer_count; i++)
    {   
        con_args[i].new_argc = new_argc;
        con_args[i].new_argv = new_argv;
        con_args[i].consume = consume;
    }
    pthread_t pro_threads[producer_count];
    pthread_t con_threads[consumer_count];
    int rc;
    // each thread has a list 
    for (int i = 0; i < producer_count; i++) {
        pro_args[i].p_i = i;
        rc = pthread_create(&pro_threads[i], NULL, start_produce_thread, (void*)&pro_args[i]);
        assert(rc == 0);
    }


    for (int i = 0; i < consumer_count; i++) {
        con_args[i].c_i = i;
        rc = pthread_create(&con_threads[i], NULL, start_consume_thread, (void*)&con_args[i]);
        assert(rc == 0);
    }

    for(int i = 0; i < producer_count; i++)
    {
        rc = pthread_join(pro_threads[i], NULL); // this is how we know the producer is done
        assert(rc == 0); 
    }
    // at this point in code, all producers have finished their jobs
    // now it is time to send the sentinel to each queue
    for(int i = 0; i < consumer_count; i++)
    {
        push(&heads[i], sentinel, i); // is the sentinel
    }

    // wait for all consumers to finish
    for(int i = 0; i < consumer_count; i++)
    {
        rc = pthread_join(con_threads[i], NULL);
        assert(rc == 0);
    }

    return 0;
}