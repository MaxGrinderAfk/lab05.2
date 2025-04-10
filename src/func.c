#include "func.h"

Queue* queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond_full = PTHREAD_COND_INITIALIZER;

pthread_t* producers;
pthread_t* consumers;
int num_producers = 0;
int num_consumers = 0;
bool should_terminate = false;
bool* producer_running;
bool* consumer_running;
pthread_mutex_t resize_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t resize_cond = PTHREAD_COND_INITIALIZER;
bool resize_in_progress = false;
int pending_resize = 0;
bool resize_decrease_pending = false;
int resize_target_size = 0;

unsigned short calculate_hash(Message* msg) {
    unsigned short hash = 0;

    hash = (hash << 5) + hash + msg->type;
    hash = (hash << 5) + hash + msg->size;

    for (int i = 0; i < msg->size; i++) {
        hash = (hash << 5) + hash + msg->data[i];
    }

    return hash;
}

void create_message(Message* msg) {
    if (!msg) return;

    msg->type = rand() % 10;
    msg->size = rand() % 256;

    for (int i = 0; i < msg->size; i++) {
        msg->data[i] = rand() % 256;
    }

    int aligned_size = ((msg->size + 3) / 4) * 4;
    for (int i = msg->size; i < aligned_size; i++) {
        msg->data[i] = 0;
    }

    msg->hash = 0;
    msg->hash = calculate_hash(msg);
}

void resize_queue(int new_size) {
    pthread_mutex_lock(&resize_mutex);

    if (resize_in_progress) {
        pending_resize = new_size;
        pthread_mutex_unlock(&resize_mutex);
        return;
    }

    resize_in_progress = true;
    pthread_mutex_unlock(&resize_mutex);

    pthread_mutex_lock(&queue_mutex);

    if (new_size > MAX_QUEUE_SIZE) {
        new_size = MAX_QUEUE_SIZE;
        printf("Ограничение размера очереди до максимума: %d\n", MAX_QUEUE_SIZE);
    }

    if (new_size < 1) {
        new_size = 1;
        printf("Ограничение размера очереди до минимума: 1\n");
    }

    int occupied = queue->current_size - queue->free;

    if (new_size < queue->current_size) {
        if (new_size < occupied) {
            printf("Невозможно немедленно уменьшить очередь до %d (занято %d элементов)\n", new_size, occupied);
            printf("Запрос на уменьшение будет выполнен, когда освободится достаточно места\n");
            resize_decrease_pending = true;
            resize_target_size = new_size;
            queue->reserved_slots = queue->current_size - new_size;

            pthread_mutex_unlock(&queue_mutex);
            pthread_mutex_lock(&resize_mutex);
            resize_in_progress = false;
            pthread_mutex_unlock(&resize_mutex);
            return;
        }
    }

    Message* new_buffer = (Message*)malloc(new_size * sizeof(Message));
    if (!new_buffer) {
        perror("malloc failed during resize");
        pthread_mutex_unlock(&queue_mutex);

        pthread_mutex_lock(&resize_mutex);
        resize_in_progress = false;
        pthread_mutex_unlock(&resize_mutex);

        return;
    }

    int new_head = 0;
    int j = queue->head;

    for (int i = 0; i < occupied; i++) {
        memcpy(&new_buffer[i], &queue->buffer[j], sizeof(Message));
        j = (j + 1) % queue->current_size;
        new_head++;
    }

    Message* old_buffer = queue->buffer;
    queue->buffer = new_buffer;
    queue->head = 0;
    queue->tail = occupied % new_size;

    int old_size = queue->current_size;
    queue->current_size = new_size;
    queue->free = new_size - occupied;
    queue->reserved_slots = 0;

    free(old_buffer);

    printf("Очередь изменена: %d -> %d (занято: %d, свободно: %d)\n",
           old_size, new_size, occupied, queue->free);

    pthread_cond_broadcast(&cond_empty);
    pthread_cond_broadcast(&cond_full);

    pthread_mutex_unlock(&queue_mutex);

    pthread_mutex_lock(&resize_mutex);
    resize_in_progress = false;

    if (pending_resize != 0) {
        int temp = pending_resize;
        pending_resize = 0;
        pthread_mutex_unlock(&resize_mutex);
        resize_queue(temp);
    } else {
        pthread_mutex_unlock(&resize_mutex);
    }
}

bool can_decrease_queue_size() {
    pthread_mutex_lock(&queue_mutex);
    int occupied = queue->current_size - queue->free;
    int target_size = resize_target_size;
    bool can_decrease = (target_size >= occupied) && (queue->free > 0);
    pthread_mutex_unlock(&queue_mutex);
    return can_decrease;
}

void check_and_perform_resize() {
    if (resize_decrease_pending && can_decrease_queue_size()) {
        pthread_mutex_lock(&resize_mutex);
        if (resize_decrease_pending) {
            printf("Выполнение отложенного уменьшения размера очереди до %d\n", resize_target_size);
            pthread_mutex_unlock(&resize_mutex);
            resize_queue(resize_target_size);
            resize_decrease_pending = false;
        } else {
            pthread_mutex_unlock(&resize_mutex);
        }
    }
}

void* producer_thread(void* arg) {
    int id = *((int*)arg);
    free(arg);

    srand(time(NULL) ^ (id * 1000));

    while (!should_terminate && producer_running[id]) {
        Message local_msg;
        create_message(&local_msg);

        check_and_perform_resize();

        pthread_mutex_lock(&queue_mutex);

        while ((queue->free <= queue->reserved_slots || queue->free == 0) && !should_terminate && producer_running[id]) {
            printf("Производитель %d: нет доступных слотов %s\n",
                   id,
                   (queue->free <= queue->reserved_slots && resize_decrease_pending) ?
                   "(зарезервированы для уменьшения размера)" : "(очередь заполнена)");
            pthread_cond_wait(&cond_empty, &queue_mutex);
        }

        if (should_terminate || !producer_running[id]) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }

        memcpy(&queue->buffer[queue->tail], &local_msg, sizeof(Message));
        queue->tail = (queue->tail + 1) % queue->current_size;
        queue->free--;
        queue->added++;

        unsigned long current_added = queue->added;

        pthread_cond_signal(&cond_full);
        pthread_mutex_unlock(&queue_mutex);

        printf("Производитель %d: добавлено сообщение (тип=%d, размер=%d, хеш=%d), всего добавлено: %lu\n",
               id, local_msg.type, local_msg.size, local_msg.hash, current_added);

        usleep(rand() % 500000 + 100000);
    }

    printf("Производитель %d завершился\n", id);
    return NULL;
}

void* consumer_thread(void* arg) {
    int id = *((int*)arg);
    free(arg);

    srand(time(NULL) ^ ((id + 100) * 1000));

    while (!should_terminate && consumer_running[id]) {
        pthread_mutex_lock(&queue_mutex);

        while ((queue->current_size - queue->free) == 0 && !should_terminate && consumer_running[id]) {
            pthread_cond_wait(&cond_full, &queue_mutex);
        }

        if (should_terminate || !consumer_running[id]) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }

        Message local_msg;
        memcpy(&local_msg, &queue->buffer[queue->head], sizeof(Message));

        queue->head = (queue->head + 1) % queue->current_size;
        queue->free++;
        queue->extracted++;

        unsigned long current_extracted = queue->extracted;

        pthread_cond_signal(&cond_empty);
        pthread_mutex_unlock(&queue_mutex);

        if (resize_decrease_pending) {
            check_and_perform_resize();
        }

        unsigned short original_hash = local_msg.hash;
        local_msg.hash = 0;
        unsigned short calculated_hash = calculate_hash(&local_msg);

        bool hash_valid = (original_hash == calculated_hash);

        printf("Потребитель %d: извлечено сообщение (тип=%d, размер=%d, хеш=%d), проверка хеша: %s, всего извлечено: %lu\n",
               id, local_msg.type, local_msg.size, original_hash, hash_valid ? "OK" : "FAILED", current_extracted);

        usleep(rand() % 500000 + 200000);
    }

    printf("Потребитель %d завершился\n", id);
    return NULL;
}

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\nПолучен сигнал завершения.\n");
        should_terminate = true;

        pthread_mutex_lock(&queue_mutex);
        pthread_cond_broadcast(&cond_empty);
        pthread_cond_broadcast(&cond_full);
        pthread_mutex_unlock(&queue_mutex);
    }
}

int kbhit() {
    struct termios oldt, newt;
    int ch;
    int oldf;

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    ch = getchar();

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);

    if (ch != EOF) {
        ungetc(ch, stdin);
        return 1;
    }

    return 0;
}

void init_queue() {
    queue = (Queue*)malloc(sizeof(Queue));
    if (!queue) {
        perror("malloc failed for queue");
        exit(1);
    }

    queue->buffer = (Message*)malloc(INITIAL_QUEUE_SIZE * sizeof(Message));
    if (!queue->buffer) {
        perror("malloc failed for buffer");
        free(queue);
        exit(1);
    }

    queue->head = 0;
    queue->tail = 0;
    queue->free = INITIAL_QUEUE_SIZE;
    queue->current_size = INITIAL_QUEUE_SIZE;
    queue->added = 0;
    queue->extracted = 0;
    queue->reserved_slots = 0;
    
    producer_running = (bool*)malloc(MAX_PRODUCERS * sizeof(bool));
    consumer_running = (bool*)malloc(MAX_CONSUMERS * sizeof(bool));
    
    if (!producer_running || !consumer_running) {
        perror("malloc failed for thread status arrays");
        exit(1);
    }
    
    for (int i = 0; i < MAX_PRODUCERS; i++) {
        producer_running[i] = false;
    }
    
    for (int i = 0; i < MAX_CONSUMERS; i++) {
        consumer_running[i] = false;
    }
}

void create_producer() {
    if (num_producers >= MAX_PRODUCERS) {
        printf("Достигнуто максимальное количество производителей\n");
        return;
    }

    int* id = (int*)malloc(sizeof(int));
    if (!id) {
        perror("malloc");
        return;
    }

    *id = num_producers;
    producer_running[num_producers] = true;

    if (pthread_create(&producers[num_producers], NULL, producer_thread, id) != 0) {
        perror("pthread_create");
        free(id);
        producer_running[num_producers] = false;
        return;
    }

    num_producers++;
    printf("Создан производитель #%d\n", num_producers);
}

void create_consumer() {
    if (num_consumers >= MAX_CONSUMERS) {
        printf("Достигнуто максимальное количество потребителей\n");
        return;
    }

    int* id = (int*)malloc(sizeof(int));
    if (!id) {
        perror("malloc");
        return;
    }

    *id = num_consumers;
    consumer_running[num_consumers] = true;

    if (pthread_create(&consumers[num_consumers], NULL, consumer_thread, id) != 0) {
        perror("pthread_create");
        free(id);
        consumer_running[num_consumers] = false;
        return;
    }

    num_consumers++;
    printf("Создан потребитель #%d\n", num_consumers);
}

void show_status() {
    pthread_mutex_lock(&queue_mutex);

    printf("\n=== Состояние очереди ===\n");
    printf("Размер очереди: %d\n", queue->current_size);
    printf("Занято: %d\n", queue->current_size - queue->free);
    printf("Свободно: %d\n", queue->free);
    printf("Зарезервировано для уменьшения: %d\n", queue->reserved_slots);
    printf("Отложенное уменьшение: %s\n", resize_decrease_pending ? "Да" : "Нет");
    if (resize_decrease_pending) {
        printf("Целевой размер: %d\n", resize_target_size);
    }
    printf("Добавлено сообщений: %lu\n", queue->added);
    printf("Извлечено сообщений: %lu\n", queue->extracted);
    printf("Количество производителей: %d\n", num_producers);
    printf("Количество потребителей: %d\n", num_consumers);
    printf("==========================\n");

    pthread_mutex_unlock(&queue_mutex);
}

void stop_producer() {
    if (num_producers > 0) {
        num_producers--;
        producer_running[num_producers] = false;

        pthread_mutex_lock(&queue_mutex);
        pthread_cond_broadcast(&cond_empty);
        pthread_mutex_unlock(&queue_mutex);

        pthread_join(producers[num_producers], NULL);
        printf("Остановлен производитель #%d\n", num_producers + 1);
    } else {
        printf("Нет работающих производителей\n");
    }
}

void stop_consumer() {
    if (num_consumers > 0) {
        num_consumers--;
        consumer_running[num_consumers] = false;

        pthread_mutex_lock(&queue_mutex);
        pthread_cond_broadcast(&cond_full);
        pthread_mutex_unlock(&queue_mutex);

        pthread_join(consumers[num_consumers], NULL);
        printf("Остановлен потребитель #%d\n", num_consumers + 1);
    } else {
        printf("Нет работающих потребителей\n");
    }
}

void cleanup() {
    for (int i = 0; i < num_producers; i++) {
        producer_running[i] = false;
    }
    
    for (int i = 0; i < num_consumers; i++) {
        consumer_running[i] = false;
    }
    
    pthread_mutex_lock(&queue_mutex);
    pthread_cond_broadcast(&cond_empty);
    pthread_cond_broadcast(&cond_full);
    pthread_mutex_unlock(&queue_mutex);

    for (int i = 0; i < num_producers; i++) {
        pthread_join(producers[i], NULL);
    }

    for (int i = 0; i < num_consumers; i++) {
        pthread_join(consumers[i], NULL);
    }

    pthread_mutex_destroy(&queue_mutex);
    pthread_cond_destroy(&cond_empty);
    pthread_cond_destroy(&cond_full);
    pthread_mutex_destroy(&resize_mutex);
    pthread_cond_destroy(&resize_cond);

    free(queue->buffer);
    free(queue);
    free(producers);
    free(consumers);
    free(producer_running);
    free(consumer_running);
}
