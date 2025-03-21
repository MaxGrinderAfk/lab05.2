#include "libs.h"

#define MAX_QUEUE_SIZE 1000
#define INITIAL_QUEUE_SIZE 100
#define MAX_MESSAGE_DATA 1024
#define MAX_PRODUCERS 10
#define MAX_CONSUMERS 10

typedef struct {
    int type;
    int size;
    unsigned char data[MAX_MESSAGE_DATA];
    unsigned short hash;
} Message;

typedef struct {
    Message* buffer;
    int head;
    int tail;
    int free;
    int current_size;
    unsigned long added;
    unsigned long extracted;
    int reserved_slots;
} Queue;

extern Queue* queue;
extern pthread_mutex_t queue_mutex;
extern pthread_cond_t cond_empty;
extern pthread_cond_t cond_full;

extern pthread_t* producers;
extern pthread_t* consumers;
extern int num_producers;
extern int num_consumers;
extern bool should_terminate;
extern pthread_mutex_t resize_mutex;
extern pthread_cond_t resize_cond;
extern bool resize_in_progress;
extern int pending_resize;
extern bool resize_decrease_pending;
extern int resize_target_size;

unsigned short calculate_hash(Message* msg);

void create_message(Message* msg);

void resize_queue(int new_size);

bool can_decrease_queue_size();

void check_and_perform_resize();

void* producer_thread(void* arg);

void* consumer_thread(void* arg);

void signal_handler(int sig);

int kbhit();

void init_queue();

void create_producer();

void create_consumer();

void show_status();

void stop_producer();

void stop_consumer();

void cleanup();