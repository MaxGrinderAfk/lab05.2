#include "func.h"

int main() {
    srand(time(NULL));

    producers = (pthread_t*)malloc(MAX_PRODUCERS * sizeof(pthread_t));
    consumers = (pthread_t*)malloc(MAX_CONSUMERS * sizeof(pthread_t));

    if (!producers || !consumers) {
        perror("malloc failed");
        exit(1);
    }

    init_queue();

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Система производителей и потребителей запущена\n");
    printf("Управление:\n");
    printf("p - создать производителя\n");
    printf("c - создать потребителя\n");
    printf("P - остановить производителя\n");
    printf("C - остановить потребителя\n");
    printf("s - показать статус\n");
    printf("+ - увеличить размер очереди\n");
    printf("- - уменьшить размер очереди\n");
    printf("q - выход\n");

    char cmd;
    while (!should_terminate) {
        if (kbhit()) {
            cmd = getchar();
            switch (cmd) {
                case 'p':
                    create_producer();
                    break;
                case 'c':
                    create_consumer();
                    break;
                case 'P':
                    stop_producer();
                    break;
                case 'C':
                    stop_consumer();
                    break;
                case 's':
                    show_status();
                    break;
                case '+':
                    pthread_mutex_lock(&queue_mutex);
                    int new_size = queue->current_size + 10;
                    pthread_mutex_unlock(&queue_mutex);
                    resize_queue(new_size);
                    break;
                case '-':
                    pthread_mutex_lock(&queue_mutex);
                    new_size = queue->current_size - 10;
                    pthread_mutex_unlock(&queue_mutex);
                    resize_queue(new_size);
                    break;
                case 'q':
                    printf("Завершение работы...\n");
                    should_terminate = true;

                    pthread_mutex_lock(&queue_mutex);
                    pthread_cond_broadcast(&cond_empty);
                    pthread_cond_broadcast(&cond_full);
                    pthread_mutex_unlock(&queue_mutex);
                    break;
            }
        }

        if (num_producers == 0 && num_consumers == 0 &&
            (queue->added > 0 || queue->extracted > 0)) {
            printf("Предотвращение тупика: создание производителя и потребителя\n");
            create_producer();
            create_consumer();
        }

        if (resize_decrease_pending) {
            check_and_perform_resize();
        }

        usleep(100000);
    }

    cleanup();

    printf("Программа завершена\n");
    return 0;
}
