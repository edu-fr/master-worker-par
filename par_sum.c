#include <stdio.h>
#include <unistd.h> // getopt lib
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <limits.h>
#include <getopt.h>

#define handleErrorNumber(error_num, msg) \
do { errno = error_num; perror(msg); exit(EXIT_FAILURE); } while (0)

#define handleError(msg) \
do { perror(msg); exit(EXIT_FAILURE); } while (0)

typedef struct {
    pthread_t thread_id;
    int thread_num;
} thread_info;

typedef struct node node;

struct node {
    int value;
    node* next;
};

typedef struct {
    int value;
    char instruction_type[1];
} instruction;

/* Global variables */
int file_size = 0, idle_workers = 0, num_threads;
instruction* task_list;
node *instruction_linked_list, *last_node, *current_node;
pthread_mutex_t lock_current_node, lock_sum, lock_odd, lock_min, lock_max, lock_idle_workers, lock_insertion_done;
pthread_cond_t cond_current_node, cond_worker_got_idle;
bool insertion_done = false;

long sum = 0;
long odd = 0;
long min = INT_MAX;
long max = INT_MIN;

// function prototypes
void update(long number);

/*
 * update global aggregate variables given a number
 */
void update(long number)
{
    // simulate computation
    sleep(number);

    // update aggregate variables
    pthread_mutex_lock(&lock_sum);
    sum += number;
    pthread_mutex_unlock(&lock_sum);

    if (number % 2 == 1) {
        pthread_mutex_lock(&lock_odd);
        odd++;
        pthread_mutex_unlock(&lock_odd);
    }
    pthread_mutex_lock(&lock_min);
    if (number < min) {
        min = number;
    }
    pthread_mutex_unlock(&lock_min);

    pthread_mutex_lock(&lock_max);
    if (number > max) {
        max = number;
    }
    pthread_mutex_unlock(&lock_max);
}

static void * threadStartMaster(void *arg) {
    // thread_info *t_info = arg;

    /* Waiting for all the workers to be initialized */
    pthread_mutex_lock(&lock_idle_workers);
    while(idle_workers < num_threads - 1) {
        pthread_cond_wait(&cond_worker_got_idle, &lock_idle_workers);
    }
    pthread_mutex_unlock(&lock_idle_workers);

    /* Inserting on linked list */
    instruction_linked_list = malloc(sizeof(node));
    if (instruction_linked_list == NULL)
        handleError("instruction_linked_list malloc");
    instruction_linked_list->next = NULL;
    instruction_linked_list->value = -1;
    last_node = instruction_linked_list;
    
    for (int i = 0; i < file_size; i++) {
        if(strcmp(task_list[i].instruction_type, "w") == 0) {
            sleep(task_list[i].value);
        } else if(strcmp(task_list[i].instruction_type, "p") == 0){
            if(instruction_linked_list->next != NULL) { // linked list already initialized
                node *new_node = malloc(sizeof(node));
                if (new_node == NULL) {
                    fprintf(stderr, "error on new_node malloc. Iteration %d\n", i);
                    exit(EXIT_FAILURE);
                }
                last_node->next = new_node;
                last_node->next->value = task_list[i].value;
                last_node->next->next = NULL;
                last_node = last_node->next;

                if(current_node == NULL) {
                    pthread_mutex_lock(&lock_current_node);
                    current_node = last_node;
                    pthread_mutex_unlock(&lock_current_node);
                }
                pthread_cond_signal(&cond_current_node);
            } else {
                node *new_node = malloc(sizeof(node));
                if (new_node == NULL) {
                    fprintf(stderr, "error on the first new_node malloc\n");
                    exit(EXIT_FAILURE);
                }
                instruction_linked_list->next = new_node;
                instruction_linked_list->next->value = task_list[i].value;
                instruction_linked_list->next->next = NULL;
                last_node = new_node;
                pthread_mutex_lock(&lock_current_node); // LOCK current node
                current_node = new_node;
                pthread_mutex_unlock(&lock_current_node); // UNLOCK current node
                pthread_cond_signal(&cond_current_node);
            }
        }
    }
    pthread_mutex_lock(&lock_insertion_done);
    insertion_done = true;
    pthread_mutex_unlock(&lock_insertion_done);

    /* Activate all */
    pthread_cond_broadcast(&cond_current_node);

    /* Wait the program to be finished */
    pthread_mutex_lock(&lock_idle_workers);
    while (idle_workers > 0) {
        pthread_cond_wait(&cond_worker_got_idle, &lock_idle_workers);
    }
    pthread_mutex_unlock(&lock_idle_workers);

    pthread_cond_broadcast(&cond_current_node);
    return NULL;
}

static void * threadStartWorker(void *arg) {
    // thread_info *t_info = arg;
    int being_worked_value;

    pthread_mutex_lock(&lock_idle_workers);
    idle_workers++;
    pthread_mutex_unlock(&lock_idle_workers);
    pthread_cond_signal(&cond_worker_got_idle);

    pthread_mutex_lock(&lock_current_node);
    while(!insertion_done || (current_node != NULL)) {
        while (current_node == NULL) {
            if(insertion_done) {
                break;
            }
            pthread_cond_wait(&cond_current_node, &lock_current_node);
        }
        if(insertion_done && (current_node == NULL)) {
            pthread_mutex_unlock(&lock_current_node);
            break;
        }
        if(current_node != NULL) {
            being_worked_value = current_node->value;
            current_node = current_node->next;
            pthread_mutex_unlock(&lock_current_node);
            update(being_worked_value);
        }
        pthread_mutex_lock(&lock_current_node);
    }
    pthread_mutex_unlock(&lock_current_node);

    pthread_mutex_lock(&lock_idle_workers);
    idle_workers--;
    pthread_mutex_unlock(&lock_idle_workers);
    pthread_cond_signal(&cond_worker_got_idle);

    return NULL;
}

int main(int argc, char *argv[]) {
    int opt, i = 0;
    char *file_name = NULL, buffer[8192], *token;
    FILE *file;
    thread_info *t_info;
    pthread_attr_t attr;
    void *res;
    node *next_node;

    /* Get opt */
    while ((opt = getopt(argc, argv, "t:f:h")) != -1) {
        switch(opt) {
            case 't':
                num_threads = (int) strtoul(optarg, NULL, 0);
                if(num_threads < 2) {
                    fprintf(stderr, "Insufficient number of threads: %d. Expected value: 2 or more\n", num_threads);
                    exit(EXIT_FAILURE);
                }
                break;

            case 'f':
                file_name = malloc(strlen(optarg) + 1);
                if(file_name == NULL)
                    handleError("file_name malloc");
                strcpy(file_name, optarg);
                break;

            case 'h':
                printf("=== Master Worker ===\n\nArguments:\n-t Number of threads (ex: -t 8)\n"
                       "-f File name (ex: -f file.txt)\n-h Help\n");
                break;

            default:
                fprintf(stderr, "Usage: %s [-m vector size] arg...\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    /* Reading file */
    file = fopen(file_name, "r");
    if(file == NULL) {
        handleError("can't open file");
    }

    /* Count number of lines in the file */
    while(fgets(buffer , 8192, file) != NULL) {
        file_size++;
    }
    fseek(file, 0, SEEK_SET);

    /* Allocating the list with all tasks */
    task_list = malloc(file_size * sizeof(instruction));
    if (task_list == NULL)
        handleError("task_list malloc");
    while(fgets(buffer , 8192, file) != NULL) {
        token = strtok(buffer, " ");
        strcpy(task_list[i].instruction_type, token);
        token = strtok(NULL, "\n");
        task_list[i].value = (int) strtoul(token, NULL, 0);
        i++;
    }

    /* Creating threads */
    int s = pthread_attr_init(&attr);
    if (s != 0) {
        handleErrorNumber(s, "pthread_attr_init");
    }

    t_info = calloc(num_threads, sizeof(thread_info));
    if(t_info == NULL) {
        handleError("t_info calloc");
    }

    t_info[0].thread_num = 1;
    s = pthread_create(&t_info[0].thread_id, &attr, &threadStartMaster, &t_info[0]);
    if(s != 0)
        handleErrorNumber(s, "pthread_create_master");

    for (int thread_num = 1; thread_num < num_threads; thread_num++) {
        t_info[thread_num].thread_num = thread_num + 1;
        s = pthread_create(&t_info[thread_num].thread_id, &attr, &threadStartWorker, &t_info[thread_num]);
        if(s != 0)
            handleErrorNumber(s, "pthread_create_worker");
    }

    s = pthread_attr_destroy(&attr);
    if(s != 0)
        handleErrorNumber(s, "pthread_attr_destroy");

    for (int thread_num = 0; thread_num < num_threads; thread_num++) {
        s = pthread_join(t_info[thread_num].thread_id, &res);
        if(s != 0)
            handleErrorNumber(s, "pthread_join");
        free(res);      /* Free memory allocated by thread */
    }

    fclose(file);

    current_node = instruction_linked_list;
    while(current_node != NULL) {
        next_node = current_node->next;
        free(current_node);
        current_node = next_node;
    }
    free(current_node);

    free(task_list);
    free(t_info);

    // print results
    printf("%ld %ld %ld %ld\n", sum, odd, min, max);

    // clean up and return
    return EXIT_SUCCESS;
}