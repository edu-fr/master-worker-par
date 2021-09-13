#include <stdio.h>
#include <unistd.h> // getopt lib
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>

#define handleErrorNumber(error_num, msg) \
do { errno = error_num; perror(msg); exit(EXIT_FAILURE); } while (0)

#define handleError(msg) \
do { perror(msg); exit(EXIT_FAILURE); } while (0)

typedef struct {
    pthread_t thread_id;
    int thread_num;
    bool idle;
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
pthread_mutex_t lock_last_node, lock_linked_list, lock_current_node, lock_done, lock_numbers, lock_idle_workers,
                lock_work_can_be_started, lock_can_be_awakened;
pthread_cond_t cond_current_note,  cond_worker_initialized, cond_work_can_be_started, cond_can_be_awakened,
                cond_worker_got_idle;
bool done = false, work_can_be_started = false;

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

    pthread_mutex_lock(&lock_numbers);
    // update aggregate variables
    sum += number;
    if (number % 2 == 1) {
        odd++;
    }
    if (number < min) {
        min = number;
    }
    if (number > max) {
        max = number;
    }
    pthread_mutex_unlock(&lock_numbers);

    printf("Terminou de mudar!\n");
}

static void * threadStartMaster(void *arg) {
    thread_info *t_info = arg;
    // printf("Thread mestre! num %d\n", t_info->thread_num);

    /* Wait for all the workers to be active to create the linked list */
    pthread_mutex_lock(&lock_idle_workers);
    while(idle_workers < num_threads - 1) {
        // printf("Mestre esperando os outros %d trabalhadores serem iniciados!\n", num_threads - idle_workers - 1);
        pthread_cond_wait(&cond_worker_initialized, &lock_idle_workers);
    }
    pthread_mutex_unlock(&lock_idle_workers);

    pthread_mutex_lock(&lock_work_can_be_started);
    work_can_be_started = true;
    pthread_mutex_unlock(&lock_work_can_be_started);

    pthread_cond_broadcast(&cond_work_can_be_started); // Ativa todos os trabalhadores (?)

    instruction_linked_list = malloc(sizeof(node));
    instruction_linked_list->next = NULL;
    instruction_linked_list->value = -1;
    last_node = instruction_linked_list;
    
    for (int i = 0; i < file_size; ++i) {
        if(strcmp(task_list[i].instruction_type, "e") == 0) {
            // printf("Master sleeping for %d seconds!\n", task_list[i].value);
            sleep(task_list[i].value);
            // printf("Waked up!\n");
        } else {
            if(instruction_linked_list->next != NULL) { // linked list already initialized
                node *new_node = malloc(sizeof(node));

                pthread_mutex_lock(&lock_last_node); // LOCK last node
                last_node->next = new_node;
                last_node->next->value = task_list[i].value;
                last_node->next->next = NULL;
                last_node = last_node->next;
                pthread_mutex_unlock(&lock_last_node); // UNLOCK last node

                pthread_mutex_lock(&lock_current_node); // LOCK current node
                if(current_node == NULL) {
                    current_node = last_node;
                }
                pthread_mutex_unlock(&lock_current_node); // UNLOCK current node

                printf("New job available!\n");
                pthread_cond_signal(&cond_current_note);
            } else {
                node *new_node = malloc(sizeof(node));

                pthread_mutex_lock(&lock_linked_list); // LOCK linked list
                instruction_linked_list->next = new_node;
                instruction_linked_list->next->value = task_list[i].value;
                instruction_linked_list->next->next = NULL;
                pthread_mutex_unlock(&lock_linked_list); // UNLOCK linked list

                pthread_mutex_lock(&lock_last_node); // LOCK last node
                last_node = new_node;
                pthread_mutex_unlock(&lock_last_node); // UNLOCK last node

                pthread_mutex_lock(&lock_current_node); // LOCK current node
                current_node = new_node;
                pthread_mutex_unlock(&lock_current_node); // UNLOCK current node

                // printf("New job available!\n");
                pthread_cond_signal(&cond_current_note);
            }
        }
    }

    printf("\n\nTERMINOU DE INSERIR NAS LISTAS!!\n\n");

    pthread_mutex_lock(&lock_idle_workers);
    while(idle_workers < num_threads - 1) { // If condition is met, end the program
        // printf("Mestre esperando os outros %d trabalhadores terminarem seus trabalhos!\n", num_threads - idle_workers - 1);
        pthread_mutex_lock(&lock_current_node);
        if(current_node != NULL) {
            pthread_mutex_unlock(&lock_current_node);
            pthread_cond_signal(&cond_can_be_awakened);
        }
        pthread_mutex_unlock(&lock_current_node);
        pthread_cond_wait(&cond_worker_got_idle, &lock_idle_workers); // Only verifies when a new worker is idle
    }
    printf("IDLE WORKERS: %d\n", idle_workers);
    pthread_mutex_unlock(&lock_idle_workers);

    printf("Fim do programa!\n");
    printf("Desativando trabalhadores...\n");

    pthread_mutex_lock(&lock_done);
    done = true;
    pthread_mutex_unlock(&lock_done);
    printf("Broadcastei!\n");
    pthread_cond_broadcast(&cond_can_be_awakened);
    /* Print instruction linked list */
    // int k = 1;
    // node *current_node = instruction_linked_list->next;
    // do {
    //     printf("Nó %d: %d\n", k, current_node->value);
    //     current_node = current_node->next;
    //     k++;
    // } while(current_node->next != NULL);
    // printf("Nó %d: %d\n", k, current_node->value);
    return NULL;
}

static void * threadStartWorker(void *arg) {
    thread_info *t_info = arg;

    /* Activate a new worker */
    pthread_mutex_lock(&lock_idle_workers);
    idle_workers++;
    pthread_mutex_unlock(&lock_idle_workers);
    pthread_cond_signal(&cond_worker_initialized);

    /* Wait for the command to start working */

    pthread_mutex_lock(&lock_work_can_be_started);
    while(!work_can_be_started)
        pthread_cond_wait(&cond_work_can_be_started, &lock_work_can_be_started);
    pthread_mutex_unlock(&lock_work_can_be_started);

    node* being_worked_node;

    pthread_mutex_lock(&lock_done);
    while(!done) {
        pthread_mutex_unlock(&lock_done);

        pthread_mutex_lock(&lock_idle_workers);
        idle_workers--;
        printf("Decrementado! Trabalhador %d está trabalhando! Ainda ociosos: %d\n", t_info->thread_num , idle_workers);
        pthread_mutex_unlock(&lock_idle_workers);

        pthread_mutex_lock(&lock_current_node);  // LOCK current node
        do {
            // pthread_mutex_lock(&lock_current_node); // LOCK current node
            while (current_node == NULL) {
                printf("No tasks for worker %d. Waiting...\n", t_info->thread_num);
                pthread_cond_wait(&cond_current_note, &lock_current_node); // WAIT current node
            }
            printf("Worker %d executing task: %d seconds to finish!\n", t_info->thread_num, current_node->value);
            being_worked_node = current_node;
            current_node = current_node->next;
            pthread_mutex_unlock(&lock_current_node); // UNLOCK current node
            update(being_worked_node->value);
            pthread_mutex_lock(&lock_current_node); // LOCK current node
        } while(current_node != NULL);
        pthread_mutex_unlock(&lock_current_node);

        pthread_mutex_lock(&lock_idle_workers);
        idle_workers++;
        pthread_mutex_unlock(&lock_idle_workers);

        pthread_cond_signal(&cond_worker_got_idle);

        pthread_mutex_lock(&lock_current_node);
        pthread_mutex_lock(&lock_done);
        while(current_node == NULL && !done) {
            pthread_mutex_unlock(&lock_done);
            pthread_cond_wait(&cond_can_be_awakened, &lock_current_node);
        }
        pthread_mutex_unlock(&lock_current_node);
        pthread_mutex_unlock(&lock_done);

        pthread_mutex_lock(&lock_done);
    }

    printf("Trabalhador %d desativado!\n", t_info->thread_num);

    return NULL;
}

int main(int argc, char *argv[]) {
    int opt, i = 0;
    char *file_name = NULL, buffer[4], *token;
    FILE *file;
    thread_info *t_info;
    pthread_attr_t attr;
    void *res;

    pthread_t *t = (pthread_t *)malloc(sizeof(pthread_t));

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
                file_name = (char*) malloc(strlen(optarg) * sizeof(char));
                strcpy(file_name, optarg);
                break;

            case 'h':
                printf("=== Master Worker ===\n\nArguments:\n-t Number of workers (ex: -t 8)\n"
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
        fprintf(stderr, "Error opening file '%s'\n", file_name);
        exit(EXIT_FAILURE);
    }

    /* Count number of lines in the file */
    for (char c = getc(file); c != EOF; c = getc(file))
        if (c == '\n') // Increment count if this character is newline
            file_size = file_size + 1;
    file_size++;
    fseek(file, 0, SEEK_SET);

    /* Allocating the list with all tasks */
    task_list = malloc(file_size * sizeof(instruction));
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
        t_info[thread_num].idle = true;
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
        // printf("Joined with thread %d; returned value was %s\n", t_info[thread_num].thread_num, (char *) res);
        free(res);      /* Free memory allocated by thread */
    }

    fclose(file);
    free(task_list);

    // print results
    printf("%ld %ld %ld %ld\n", sum, odd, min, max);

    // clean up and return
    return (EXIT_SUCCESS);
}