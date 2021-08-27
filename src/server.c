#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include <math.h>

#define MSG_SIZE 2048
#define MAX_CNT_LEN 1024 // grandezza massima del contenuto di un file: 1 KB
#define UNIX_MAX_STANDARD_FILENAME_LENGHT 108 /* man 7 unix */
#define SOCKET_NAME "./ssocket.sk"
#define LOG_NAME "./log.txt"

/* FILE DI LOG:
 *
 * STRUTTURA MACRO : START...opx \n opy \n opz \n ...END;stats;s1;s2;...;sk;
 * STRUTTURA GENERICA: op/Thrd_id/Op_num/Succ/File_Path... \n
 *
 * open_Connection : <Thrd_id/1/(0|1)/Sock_Name>;
 * close_Connection : <Thrd_id/2/(0|1)/Sock_Name>;
 * open_File : <Thrd_id/3/(0|1)/File_Path/flags>;
 * read_File : <Thrd_id/4/(0|1)/File_Path/size>;
 * read_NFiles : <Thrd_id/5/(0|1)/Read_no/size>;
 * write_File : <Thrd_id/6/(0|1)/File_Path/size/Rplc(0|1)/Rplc_no/Rplc_size>;
 * append_to_File : <Thrd_id/7/(0|1)/File_Path/size/Rplc(0|1)/Rplc_no/Rplc_size>;
 * lock_File : <Thrd_id/8/(0|1)/File_Path>;
 * unlock_File : <Thrd_id/9/(0|1)/File_Path>;
 * close_File : <Thrd_id/10/(0|1)/File_Path>;
 * remove_File : <Thrd_id/11/(0|1)/File_Path>;
 */
FILE* log_file; // puntatore al file di log
pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER; // mutex per mutua esclusione sul file di log

typedef struct scnode
{
    size_t c_pid;
    struct scnode* next;
    struct scnode* prec;
} c_node;

typedef struct sclist
{
    c_node* head;
    c_node* tail;
} c_list;

typedef struct sfile
{
    char* path;
    char* cnt;
    c_list* whoop;
    size_t lock_owner;
    size_t op;
    struct sfile* next;
    struct sfile* prec;
} file;

typedef struct sfilelst
{
    file* head;
    file* tail;
    size_t size;
    pthread_mutex_t mtx;
}f_list;

typedef struct sfifonode
{
    char* path;
    struct sfifonode* next;
    struct sfifonode* prec;
}fifo_node;

typedef struct sfifo
{
    fifo_node* head;
    fifo_node* tail;
} fifo;

typedef struct shash
{
    f_list** lists;
    size_t lst_no;
}hash;

//  VARIABILI GLOBALI SULLA GESTIONE DELLO STORAGE  //
static size_t thread_no;    // numero di thread worker del server
static size_t max_size;    // dimensione massima dello storage
static size_t max_no;      // numero massimo di files nello storage
static size_t curr_size = 0;   // dimensione corrente dello storage
static size_t curr_no = 0;     // numero corrente di files nello storage

static hash* storage = NULL;    // struttura dati in cui saranno raccolti i files amministratidal server
pthread_mutex_t strg_mtx = PTHREAD_MUTEX_INITIALIZER; // mutex per garantire l'atomicità di "append_File"

static fifo* queue = NULL;      // struttura dati di appoggio per la gestione dei rimpizzamenti con politica fifo
pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER; // mutex per mutua esclusione sulla coda fifo

static c_list* coda = NULL;     // struttura dati di tipo coda FIFO per la comunicazione Master/Worker (uso improprio della nomenclatura "c_pid")
pthread_mutex_t coda_mtx = PTHREAD_MUTEX_INITIALIZER; // mutex per mutua esclusione sugli accessi alla coda
pthread_cond_t wait_coda = PTHREAD_COND_INITIALIZER;

volatile sig_atomic_t t = 0; // variabile settata dal signal handler

//  VARIABILI PER LE STATISTICHE //
pthread_mutex_t stats_mtx = PTHREAD_MUTEX_INITIALIZER; // mutex per mutua esclusione sugli accessi alle var. per stats.

static size_t max_size_reached = 0;    // dimensione massima dello storage raggiunta
static size_t max_size_reachedMB = 0;    // dimensione massima dello storage raggiunta in MB
static size_t max_no_reached = 0;      // numero massimo di files nello storage reggiunto
static size_t replace_no = 0;
static size_t replace_alg_no = 0;
static size_t read_no = 0;
static size_t total_read_size = 0;
static size_t media_read_size = 0;
static size_t write_no = 0;
static size_t total_write_size = 0;
static size_t media_write_size = 0;
static size_t lock_no = 0;
static size_t unlock_no = 0;
static size_t openlock_no = 0;
static size_t close_no = 0;
static size_t max_connections_no = 0;   // numero massimo di connessioni contemporanee raggiunto
static size_t currconnections_no = 0;   // numero di connessioni correnti

//    FUNZIONI PER MUTUA ESCLUSIONE    //
static void Pthread_mutex_lock (pthread_mutex_t* mtx)
{
    int err;
    if ((err=pthread_mutex_lock(mtx)) != 0 )
    {
        errno = err;
        perror("lock");
        pthread_exit((void*)errno);
    }
}
static void Pthread_mutex_unlock (pthread_mutex_t* mtx)
{
    int err;
    if ((err=pthread_mutex_unlock(mtx)) != 0 )
    {
        errno = err;
        perror("unlock");
        pthread_exit((void*)errno);
    }
}

//    FUNZIONI PER NODI DI C_PID   //
static c_node* c_node_init (size_t c_pid)
{
    if (c_pid == 0)
    {
        errno = EINVAL;
        return NULL;
    }

    c_node* tmp = malloc(sizeof(c_node));
    if (tmp == NULL)
    {
        free(tmp);
        errno = ENOMEM;
        return NULL;
    }

    tmp->next = NULL;
    tmp->prec = NULL;
    tmp->c_pid = c_pid;

    return tmp;

}

//    FUNZIONI PER LISTE DI NODI DI C_PID   // (USATE anche per comunicazione Main Thread/WorkerThread)
static c_list* c_list_init ()
{
    c_list* tmp = malloc(sizeof(c_list));
    if (tmp == NULL)
    {
        errno = ENOMEM;
        free(tmp);
        return NULL;
    }
    tmp->head = NULL;
    tmp->tail = NULL;

    return tmp;
}
static void c_list_free (c_list* lst)
{
    if (lst == NULL)
    {
        errno = EINVAL;
        return;
    }

    c_node* tmp = lst->head;

    while (tmp != NULL)
    {
        lst->head = lst->head->next;
        free(tmp);
        tmp = lst->head;
    }

    free(lst);
}
static int c_list_cont_node (c_list* lst, size_t c_pid)
{
    if (lst == NULL || c_pid == 0)
    {
        errno = EINVAL;
        return -1;
    }

    c_node* cursor = lst->head;
    while (cursor != NULL)
    {
        if (cursor->c_pid == c_pid) return 1;
        cursor = cursor->next;
    }

    errno = ENOENT;
    return 0;
}
static int c_list_add_head (c_list* lst, size_t c_pid)
{
    if (lst == NULL || c_pid == 0)
    {
        errno = EINVAL;
        return -1;
    }

    c_node* tmp = c_node_init(c_pid);
    if (lst->head == NULL)
    {
        lst->head = tmp;
        lst->tail = tmp;
    }
    else
    {
        lst->head->prec = tmp;
        tmp->next = lst->head;
        lst->head = tmp;
    }

    return 1;
}
static int c_list_pop (c_list* lst)
{
    if (lst == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (lst->tail == NULL) return 0;

    size_t out = lst->tail->c_pid;

    if (lst->head == lst->tail)
    {
        free(lst->tail);
        lst->tail = NULL;
        lst->head = NULL;
        return out;
    }
    else
    {
        c_node* tmp = lst->tail;
        lst->tail = lst->tail->prec;
        lst->tail->next = NULL;
        free(tmp);
        return out;
    }
}
static int c_list_pop_worker (c_list* lst)
{
    if (lst == NULL)
    {
        errno = EINVAL;
        return -2;
    }
    while (lst->head == NULL)
    {
        pthread_cond_wait(&wait_coda,&coda_mtx);
    }

    size_t out = lst->tail->c_pid;

    if (lst->head == lst->tail)
    {
        free(lst->tail);
        lst->tail = NULL;
        lst->head = NULL;
        return out;
    }
    else
    {
        c_node* tmp = lst->tail;
        lst->tail = lst->tail->prec;
        lst->tail->next = NULL;
        free(tmp);
        return out;
    }

}
static int c_list_rem_node (c_list* lst, size_t c_pid)
{
    if (lst == NULL || c_pid == 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (lst->head == NULL)
    {
        return 0;
    }

    c_node* cursor = lst->head;
    // eliminazione di un nodo in testa
    if ( lst->head != NULL)
    {
        if (c_pid == lst->head->c_pid) {
            if (lst->head->next != NULL)
            {
                lst->head->next->prec = NULL;
                lst->head = lst->head->next;

                free(cursor);
                return 1;
            }
            else
            {
                lst->head = NULL;
                lst->tail = NULL;

                free(cursor);
                return 1;
            }
        }
        cursor = lst->head->next;
    }

    while (cursor != NULL)
    {
        if (cursor->c_pid == c_pid)
        {
            if (lst->tail->c_pid == c_pid) {
                lst->tail->prec->next = NULL;
                lst->tail = lst->tail->prec;
                free(cursor);
                return 1;
            }
            else
            {
                cursor->prec->next = cursor->next;
                cursor->next->prec = cursor->prec;
                free(cursor);
                return 1;
            }
        }
        cursor = cursor->next;
    }

    return 0;
}

//    FUNZIONI PER AMMINISTRARE I FILE    //
static file* file_init (char* path, char* cnt, size_t lo)
{
    if (path == NULL || cnt == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    size_t path_len = strnlen(path,UNIX_MAX_STANDARD_FILENAME_LENGHT);
    size_t cnt_len = strlen(cnt);
    if (path_len > UNIX_MAX_STANDARD_FILENAME_LENGHT)
    {
        errno = ENAMETOOLONG;
        return NULL;
    }
    if (cnt_len > MAX_CNT_LEN)
    {
        errno = ENAMETOOLONG;
        return NULL;
    }

    file* tmp = malloc(sizeof(file));
    if (tmp == NULL)
    {
        errno = ENOMEM;
        free(tmp);
        return NULL;
    }

    tmp->path = malloc(sizeof(char)*UNIX_MAX_STANDARD_FILENAME_LENGHT);
    strcpy(tmp->path,path);
    tmp->cnt = malloc(sizeof(char)*MAX_CNT_LEN);
    strcpy(tmp->cnt,cnt);

    tmp->lock_owner = lo;
    tmp->op = 0;
    tmp->whoop = c_list_init();
    if (tmp->whoop == NULL) return NULL;

    tmp->next = NULL;
    tmp->prec = NULL;

    return tmp;
}
static void file_free (file* file1)
{
    if (file1 == NULL)  return;
    c_list_free(file1->whoop);
    free(file1->path);
    free(file1->cnt);
    free(file1);
}
static file* file_copy (file* file1)
{
    if (file1 == NULL) return NULL;

    file* copy = malloc(sizeof(file));
    if (copy == NULL)
    {
        errno = ENOMEM;
        free(copy);
        return NULL;
    }
    size_t path_len = strnlen(file1->path,UNIX_MAX_STANDARD_FILENAME_LENGHT);
    size_t cnt_len = strlen(file1->cnt);
    copy->next = NULL;
    copy->prec = NULL;
    copy->whoop = NULL;
    copy->lock_owner = file1->lock_owner;
    copy->path = malloc(sizeof(char)*(path_len+1));
    if (copy->path == NULL)
    {
        errno = ENOMEM;
        free(copy->path);
        return NULL;
    }
    strcpy(copy->path,file1->path);

    copy->cnt = malloc(sizeof(char)*(cnt_len+1));
    if (copy->cnt == NULL)
    {
        errno = ENOMEM;
        free(copy->cnt);
        return NULL;
    }
    strcpy(copy->cnt,file1->cnt);

    return copy;
}

//    FUNZIONI PER AMMINISTRARE LA POLITICA FIFO DELLA "FIFO* QUEUE"   //
static fifo_node* fifo_node_init (char* path)
{
    fifo_node* tmp = malloc(sizeof(fifo_node));
    if (tmp == NULL)
    {
        errno = ENOMEM;
        free(tmp);
        return NULL;
    }
    tmp->next = NULL;
    tmp->prec = NULL;
    tmp->path = malloc(sizeof(char)*UNIX_MAX_STANDARD_FILENAME_LENGHT);
    if(tmp->path == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }
    strcpy(tmp->path,path);

    return tmp;
}
static void fifo_node_free (fifo_node* node1)
{
    if (node1 == NULL) return;
    free(node1->path);
    free(node1);

    return;
}

static fifo* fifo_init ()
{
    fifo* tmp = malloc(sizeof(fifo));
    if (tmp == NULL)
    {
        errno = ENOMEM;
        free(tmp);
        return NULL;
    }
    tmp->head = NULL;
    tmp->tail = NULL;

    return tmp;
}
static void fifo_free (fifo* lst)
{
    if (lst == NULL)
    {
        errno = EINVAL;
        return;
    }

    fifo_node* tmp = lst->head;

    while (tmp != NULL)
    {
        lst->head = lst->head->next;
        fifo_node_free(tmp);
        tmp = lst->head;
    }

    lst->tail = NULL;

    free(lst);
}
static int fifo_push (fifo* lst, fifo_node* file1)
{
    Pthread_mutex_lock(&queue_mtx);

    if (lst == NULL || file1 == NULL)
    {
        errno = EINVAL;
        Pthread_mutex_unlock(&queue_mtx);
        return -1;
    }

    if (lst->head == NULL)
    {
        lst->head = file1;
        lst->tail = file1;
    }
    else
    {
        lst->head->prec = file1;
        file1->next = lst->head;
        lst->head = file1;
    }

    Pthread_mutex_unlock(&queue_mtx);

    return 1;
}
static char* fifo_pop (fifo* lst)
{
    Pthread_mutex_lock(&queue_mtx);

    if (lst == NULL || lst->tail == NULL)
    {
        errno = EINVAL;
        Pthread_mutex_unlock(&queue_mtx);
        return NULL;
    }

    size_t path_len = strnlen(lst->tail->path,UNIX_MAX_STANDARD_FILENAME_LENGHT);
    char* path = malloc(sizeof(char)*path_len);
    if (path == NULL)
    {
        errno = ENOMEM;
        free(path);
        Pthread_mutex_unlock(&queue_mtx);
        return NULL;
    }
    strcpy(path,lst->tail->path);

    if (lst->head == lst->tail)
    {
        fifo_node_free(lst->tail);
        lst->tail = NULL;
        lst->head = NULL;
        Pthread_mutex_unlock(&queue_mtx);
        return path;
    }
    else
    {
        fifo_node* tmp = lst->tail;
        lst->tail = lst->tail->prec;
        lst->tail->next = NULL;
        fifo_node_free(tmp);
        Pthread_mutex_unlock(&queue_mtx);
        return path;
    }
}
static int fifo_rem_file (fifo* lst, char* path)
{
    Pthread_mutex_lock(&queue_mtx);

    if (lst == NULL || path == NULL)
    {
        errno = EINVAL;
        Pthread_mutex_unlock(&queue_mtx);
        return -1;
    }

    if (lst->head == NULL)
    {
        errno = ENOENT;
        Pthread_mutex_unlock(&queue_mtx);
        return -1;
    }

    fifo_node * cursor = lst->head;
    // eliminazione di un nodo in testa
    if (!strncmp(lst->head->path, path,UNIX_MAX_STANDARD_FILENAME_LENGHT)) {
        if(lst->head->next != NULL)
        {
            lst->head->next->prec = NULL;
            lst->head = lst->head->next;

            fifo_node_free(cursor);
            Pthread_mutex_unlock(&queue_mtx);
            return 1;
        }
        else
        {
            lst->head = NULL;
            lst->tail = NULL;

            fifo_node_free(cursor);
            Pthread_mutex_unlock(&queue_mtx);
            return 1;
        }
    }

    cursor = lst->head->next;
    while (cursor != NULL)
    {
        if (!strncmp(cursor->path,path,UNIX_MAX_STANDARD_FILENAME_LENGHT))
        {
            if (!strncmp(lst->tail->path, path,UNIX_MAX_STANDARD_FILENAME_LENGHT)) {
                lst->tail->prec->next = NULL;
                lst->tail = lst->tail->prec;
                fifo_node_free(cursor);
                Pthread_mutex_unlock(&queue_mtx);
                return 1;
            }
            else
            {
                cursor->prec->next = cursor->next;
                cursor->next->prec = cursor->prec;
                fifo_node_free(cursor);
                Pthread_mutex_unlock(&queue_mtx);
                return 1;
            }
        }
        cursor = cursor->next;
    }

    errno = ENOENT;
    Pthread_mutex_unlock(&queue_mtx);
    return -1;
}
static void fifo_print (fifo* lst)
{
    if (lst == NULL)
    {
        printf("NULL\n");
        return;
    }

    printf("%lu ||  ", curr_no);

    fifo_node* cursor = lst->head;

    while (cursor != NULL)
    {
        printf("%s <-> ", cursor->path);
        cursor = cursor->next;
    }

    printf("END\n");

    return;
}

//    FUNZIONI PER AMMINISTRARE LISTE DI FILE    //
static f_list* f_list_init ()
{
    f_list* tmp = malloc(sizeof(f_list));
    if (tmp == NULL)
    {
        errno = ENOMEM;
        free(tmp);
        return NULL;
    }
    tmp->head = NULL;
    tmp->tail = NULL;
    tmp->size = 0;
    tmp->mtx = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;

    return tmp;
}
static void f_list_free (f_list* lst)
{
    if (lst == NULL)
    {
        errno = EINVAL;
        return;
    }

    file* tmp = lst->head;

    while (tmp != NULL)
    {
        lst->head = lst->head->next;
        file_free(tmp);
        tmp = lst->head;
    }

    lst->tail = NULL;

    free(lst);
}
static void f_list_print (f_list* lst)
{
    if (lst == NULL)
    {
        printf("NULL\n");
        return;
    }

    printf("%ld // ", lst->size);

    file* cursor = lst->head;

    while (cursor != NULL)
    {
        printf("%s <-> ", cursor->path);
        cursor = cursor->next;
    }

    printf("END\n");

    return;
}
static file* f_list_get_file (f_list* lst, char* path)
{
    if (lst == NULL || path == NULL )
    {
        errno = EINVAL;
        return NULL;
    }

    file* cursor = lst->head;
    while (cursor != NULL)
    {
        if (!strncmp(cursor->path,path,UNIX_MAX_STANDARD_FILENAME_LENGHT)) return cursor;

        cursor = cursor->next;
    }

    return NULL;
}
static int f_list_cont_file (f_list* lst, char* path)
{
    if (lst == NULL || path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    file* cursor = lst->head;
    while (cursor != NULL)
    {
        if (!strncmp(cursor->path,path,UNIX_MAX_STANDARD_FILENAME_LENGHT)) return 1;
        cursor = cursor->next;
    }

    return 0;
}
static int f_list_add_head (f_list* lst, file* file1)
{
    if (lst == NULL || file1 == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    int lcf = f_list_cont_file(lst,file1->path);
    if (lcf == -1) return -1;

    if (lcf) return 0;

    if (lst->head == NULL)
    {
        lst->head = file1;
        lst->tail = file1;
    }
    else
    {
        lst->head->prec = file1;
        file1->next = lst->head;
        lst->head = file1;
    }

    lst->size++;
    return 1;
}
static file* f_list_rem_file (f_list* lst, char* path)
{
    if (lst == NULL || path == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    if (lst->head == NULL)
    {
        errno = ENOENT;
        return NULL;
    }
    file* cursor = lst->head;
    // eliminazione di un nodo in testa
    if (!strncmp(lst->head->path, path, UNIX_MAX_STANDARD_FILENAME_LENGHT)) {
        if(lst->head->next != NULL)
        {
            lst->head->next->prec = NULL;
            lst->head = lst->head->next;
            file* out = file_copy(cursor);
            file_free(cursor);
            lst->size--;
            return out;
        }
        else
        {
            lst->head = NULL;
            lst->tail = NULL;
            file* out = file_copy(cursor);
            file_free(cursor);
            lst->size--;
            return out;
        }
    }

    cursor = lst->head->next;
    while (cursor != NULL)
    {
        if (!strncmp(cursor->path,path,UNIX_MAX_STANDARD_FILENAME_LENGHT))
        {
            if (!strncmp(lst->tail->path, path,UNIX_MAX_STANDARD_FILENAME_LENGHT)) {
                lst->tail->prec->next = NULL;
                lst->tail = lst->tail->prec;
                file* out = file_copy(cursor);
                file_free(cursor);
                lst->size--;
                return out;
            }
            else
            {
                cursor->prec->next = cursor->next;
                cursor->next->prec = cursor->prec;
                file* out = file_copy(cursor);
                file_free(cursor);
                lst->size--;
                return out;
            }
        }
        cursor = cursor->next;
    }

    errno = ENOENT;
    return NULL;
}

//    FUNZIONI PER AMMINISTRARE TABELLE HASH    //
static hash* hash_init (size_t lst_no)
{
    if (lst_no == 0)
    {
        errno = EINVAL;
        return NULL;
    }

    hash* tmp = malloc(sizeof(hash));
    if (tmp == NULL)
    {
        errno = ENOMEM;
        free(tmp);
        return NULL;
    }
    tmp->lst_no = lst_no;
    tmp->lists = malloc(sizeof(f_list*)*lst_no);
    if (tmp->lists == NULL)
    {
        errno = ENOMEM;
        free(tmp->lists);
        return NULL;
    }

    int i;
    for (i=0;i<lst_no;i++)
    {
        tmp->lists[i] = f_list_init();
        if (tmp->lists[i] == NULL)
        {
            int j = 0;
            while (j != i)
            {
                f_list_free(tmp->lists[j]);
                j++;
            }
            free(tmp->lists);
            free(tmp);
            errno = ENOMEM;
            return NULL;
        }

    }

    return tmp;
}
static long long hash_function (char* str)
{
    if (str == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    const int p = 47;
    const int m = 1e9 + 9;
    long long h_v = 0;
    long long p_pow = 1;

    int i = 0;
    while(str[i] != '\0')
    {
        h_v = (h_v + (str[i] - 'a' + 1) * p_pow) % m;
        p_pow = (p_pow * p) % m;
        i++;
    }
    return h_v;
}
static int hash_add_file (hash* tbl, file* file1)
{
    if (tbl == NULL || file1 == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    size_t hsh = hash_function(file1->path) % tbl->lst_no;
    if(hsh != -1 && f_list_add_head(tbl->lists[hsh],file1))
    {
        fifo_node* newfile_placeholder = fifo_node_init(file1->path);
        if (fifo_push(queue,newfile_placeholder))
        {
            Pthread_mutex_lock(&stats_mtx);
            curr_no++;
            curr_size = curr_size + strlen(file1->cnt);
            if(curr_size > max_size_reached) max_size_reached = curr_size;
            Pthread_mutex_unlock(&stats_mtx);
            return 1;
        }
    }

   return -1;
}
static file* hash_rem_file1 (hash* tbl, file* file1)
{
    if (tbl == NULL || file1 == NULL)
    {
        errno = EINVAL;
        return NULL;
    }
    size_t hsh = hash_function(file1->path) % tbl->lst_no;
    if(hsh != -1 )
    {
        file* tmp = f_list_rem_file(tbl->lists[hsh],file1->path);
        if (tmp != NULL && fifo_rem_file(queue,tmp->path))
        {
            Pthread_mutex_lock(&stats_mtx);
            curr_no--;
            curr_size = curr_size - strlen(tmp->cnt);
            Pthread_mutex_unlock(&stats_mtx);
            return tmp;
        }
    }

    return NULL;
}
static file* hash_rem_file2 (hash* tbl, char* path)
{
    if (tbl == NULL || path == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    size_t hsh = hash_function(path) % tbl->lst_no;

    if(hsh != -1 )
    {
        file* tmp = f_list_rem_file(tbl->lists[hsh],path);
        if (tmp != NULL && fifo_rem_file(queue,tmp->path))
        {
            Pthread_mutex_lock(&stats_mtx);
            curr_no--;
            curr_size = curr_size - strlen(tmp->cnt);
            Pthread_mutex_unlock(&stats_mtx);
            return tmp;
        }
    }

    return NULL;
}
static file* hash_get_file (hash* tbl, char* path)
{
    if (tbl == NULL || path == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    size_t hsh = hash_function(path) % tbl->lst_no;

    if(hsh != -1)
    {
        return f_list_get_file(tbl->lists[hsh], path);
    }
    return NULL;
}
static f_list* hash_get_list (hash* tbl, char* path)
{
    if (tbl == NULL || path == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    size_t hsh = hash_function(path) % tbl->lst_no;

    if(hsh != -1)
    {
        return tbl->lists[hsh];
    }
    return NULL;
}
static void hash_free (hash* tbl)
{
    if (tbl == NULL) return;

    size_t i;
    for (i=0; i < tbl->lst_no; i++)
    {
        f_list_free(tbl->lists[i]);
    }
    free(tbl->lists);
    free(tbl);
}
static void hash_print (hash* tbl)
{
    if(tbl == NULL)
    {
        printf("NULL\n");
        return;
    }

    printf("%ld\n", tbl->lst_no);

    int i;
    for(i = 0; i<tbl->lst_no; i++)
    {
        if(tbl->lists[i]->size > 0) f_list_print(tbl->lists[i]);
    }

    printf("END\n");
}
static int hash_cont_file (hash* tbl, char* path)
{
    if (tbl == NULL || path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    size_t hsh = hash_function(path) % tbl->lst_no;

    if(hsh != -1)
    {
        return f_list_cont_file(tbl->lists[hsh], path);
    }
    return -1;
}
static f_list* hash_replace (hash* tbl, char* path, size_t c_pid)
{
    if (tbl == NULL || c_pid == 0 || path == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    f_list* replaced = f_list_init();
    if (replaced == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }

    int bool = 0;
    fifo_node* p_kill_ff = NULL;

    while (curr_size > max_size)
    {
        bool = 1;
        if (p_kill_ff == NULL) p_kill_ff = queue->tail;
        if (p_kill_ff == NULL)
        {
            f_list_free(replaced);
            return NULL;
        }

        file* p_kill_file = hash_get_file(storage,p_kill_ff->path);
        if (p_kill_file == NULL)
        {
            f_list_free(replaced);
            return NULL;
        }

        while ((p_kill_file->lock_owner != 0 && p_kill_file->lock_owner != c_pid) || (p_kill_ff->path == path))
        {
            p_kill_ff = p_kill_ff->prec;
            if (p_kill_ff == NULL)
            {
                errno = EFBIG;
                f_list_free(replaced);
                return NULL;
            }

            file* p_kill_file = hash_get_file(storage,p_kill_ff->path);
            if (p_kill_file == NULL)
            {
                f_list_free(replaced);
                return NULL;
            }
        }

        file* copy = file_copy(p_kill_file);
        if (copy == NULL) return NULL;
        p_kill_ff = p_kill_ff->prec;
        if (hash_rem_file1(storage,p_kill_file) == NULL) return NULL;
        if (f_list_add_head(replaced,copy) == -1) return NULL;
        Pthread_mutex_lock(&stats_mtx);
        replace_no++;
        Pthread_mutex_unlock(&stats_mtx);
    }

    Pthread_mutex_lock(&stats_mtx);
    if (bool == 1) replace_alg_no++;
    if (curr_size > max_size_reached) max_size_reached = curr_size;
    Pthread_mutex_unlock(&stats_mtx);
    return replaced;
}
static void hash_lo_reset (hash* tbl, size_t fd_c)
{
    if (tbl == NULL) return;

    int i;
    file* cursor;

    for(i=0; i<tbl->lst_no; i++)
    {
        Pthread_mutex_lock(&(tbl->lists[i]->mtx));
        cursor = tbl->lists[i]->head;

        while(cursor != NULL)
        {
            if (cursor->lock_owner == fd_c) cursor->lock_owner = 0;
            cursor = cursor->next;
        }
        Pthread_mutex_unlock(&(tbl->lists[i]->mtx));
    }
}

//    FUNZIONI PER IL SERVER   //
static int isNumber(const char* s, long* n)
{
    if (s == NULL) return 0;
    if (strlen(s) == 0) return 0;
    char* e = NULL;
    errno=0;
    long val = strtol(s, &e, 10);
    if (errno == ERANGE) return 2;    // overflow
    if (e != NULL && *e == (char)0) {
        *n = val;
        return 1;   // successo
    }
    return 0;   // non e' un numero
}
static void t_gstr (int sgnl)
{
    if (sgnl == SIGINT || sgnl == SIGQUIT) t = 1; //SIGINT,SIGQUIT -> TERMINA SUBITO (GENERA STATISTICHE)
    else if (sgnl == SIGHUP) t = 2; //SIGHUP -> NON ACCETTA NUOVI CLIENT, ASPETTA CHE I CLIENT COLLEGATI CHIUDANO CONNESSIONE
}
static int max_up (fd_set set, int fdmax)
{
    int i;
    for(i=(fdmax-1); i>=0; --i)
        if (FD_ISSET(i, &set)) return i;
    assert(1==0);
    return -1;
}

/**
 *   @brief Funzione che permette di fare la read in modo che, se è interrotta da un segnale, riprende
 *
 *   @param fd     descrittore della connessione
 *   @param msg    puntatore al messaggio da inviare
 *
 *   @return Il numero di bytes letti, -1 se c'e' stato un errore
 */
int readn(long fd, void *buf, size_t size) {
    int readn = 0, r = 0;

    while ( readn < size ){

        if ( (r = read(fd, buf, size)) == -1 ){
            if( errno == EINTR )
                // se la read è stata interrotta da un segnale riprende
                continue;
            else{
                perror("Readn");
                return -1;
            }
        }
        if ( r == 0 )
            return readn; // Nessun byte da leggere rimasto

        readn += r;
    }

    return readn;
}

/**
 *   @brief Funzione che permette di fare la write in modo che, se è interrotta da un segnale, riprende
 *
 *   @param fd     descrittore della connessione
 *   @param msg    puntatore al messaggio da inviare
 *
 *   @return Il numero di bytes scritti, -1 se c'è stato un errore
 */
int writen(long fd, const void *buf, size_t nbyte){
    int writen = 0, w = 0;

    while ( writen < nbyte ){
        if ( (w = write(fd, buf, nbyte) ) == -1 ){
            /* se la write è stata interrotta da un segnale riprende */
            if ( errno == EINTR )
                continue;
            else if ( errno == EPIPE )
                break;
            else{
                perror("Writen");
                return -1;
            }
        }
        if( w == 0 )
            return writen;

        writen += w;
    }

    return writen;
}

/* open_FILE : FLAGS
 * 0 -> 00 -> O_CREATE = 0 && O_LOCK = 0
 * 1 -> 01 -> O_CREATE = 0 && O_LOCK = 1
 * 2 -> 10 -> O_CREATE = 1 && O_LOCK = 0
 * 3 -> 11 -> O_CREATE = 1 && O_LOCK = 1
 */
static int open_File (char* path, int flags, size_t c_pid)
{
    if (path == NULL || flags<0 || flags>3)
    {
        errno = EINVAL;
        return -1;
    }

    int ex = hash_cont_file(storage,path);
    if (ex == -1) return -1;

    switch (flags)
    {
        case 0 :
        {
            if (!ex)
            {
                errno = ENOENT;
                return -1;
            }

            file* tmp = hash_get_file(storage,path);
            if (tmp == NULL) return -1;
            f_list* tmp_lst = hash_get_list(storage,path);
            if (tmp_lst == NULL) return -1;

            Pthread_mutex_lock(&(tmp_lst->mtx));

            sleep(2);

            if(c_list_rem_node(tmp->whoop,c_pid) == -1)
            {
                Pthread_mutex_unlock(&(tmp_lst->mtx));
                return -1;
            }
            if (tmp->lock_owner == 0 || tmp->lock_owner == c_pid) tmp->op = 1;
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            return 0;

        }

        case 1 :
        {
            if (!ex)
            {
                errno = ENOENT;
                return -1;
            }

            file* tmp = hash_get_file(storage,path);
            if(tmp == NULL) return -1;

            f_list* tmp_lst = hash_get_list(storage,path);
            if(tmp_lst == NULL) return -1;

            Pthread_mutex_lock(&(tmp_lst->mtx));
            Pthread_mutex_lock(&stats_mtx);
            openlock_no++;
            lock_no++;
            Pthread_mutex_unlock(&stats_mtx);
            tmp->lock_owner = c_pid;
            if(c_list_add_head(tmp->whoop,c_pid) == -1)
            {
                Pthread_mutex_unlock(&(tmp_lst->mtx));
                return -1;
            }
            tmp->op = 1;

            Pthread_mutex_unlock(&(tmp_lst->mtx));
            return 0;

        }

        case 2 :
        {
            if (ex)
            {
                errno = EEXIST;
                return -1;
            }

            int cond = 0;
            Pthread_mutex_lock(&stats_mtx);
            if (curr_no < max_no) cond = 1;
            Pthread_mutex_unlock(&stats_mtx);

            if (cond)
            {
                file *tmp = file_init(path, "", 0);
                if (tmp == NULL) return -1;

                tmp->op = 1;
                if(c_list_add_head(tmp->whoop,c_pid) == -1) return -1;

                f_list* tmp_lst = hash_get_list(storage,path);
                if (tmp_lst == NULL) return -1;

                Pthread_mutex_lock(&tmp_lst->mtx);

                if (hash_add_file(storage, tmp) == -1)
                {
                    Pthread_mutex_unlock(&tmp_lst->mtx);
                    return -1;
                }

                Pthread_mutex_lock(&stats_mtx);
                if (curr_no>max_no_reached) max_no_reached = curr_no;
                Pthread_mutex_unlock(&stats_mtx);

                Pthread_mutex_unlock(&tmp_lst->mtx);
                return 0;
            }

            errno = ENFILE;
            return -1;
        }

        case 3 :
        {
            if (ex)
            {
                errno = EEXIST;
                return -1;
            }

            int cond = 0;
            Pthread_mutex_lock(&stats_mtx);
            if (curr_no < max_no) cond = 1;
            Pthread_mutex_unlock(&stats_mtx);

            if (cond)
            {
                file *tmp = file_init(path, "", c_pid);
                if (tmp == NULL) return -1;

                f_list *tmp_lst = hash_get_list(storage,path);
                if (tmp_lst == NULL) return -1;

                Pthread_mutex_lock(&(tmp_lst->mtx));

                tmp->op = 1;
                if(c_list_add_head(tmp->whoop,c_pid) == -1)
                {
                    Pthread_mutex_unlock(&(tmp_lst->mtx));
                    return -1;
                }

                if (hash_add_file(storage, tmp) == -1)
                {
                    Pthread_mutex_unlock(&(tmp_lst->mtx));
                    return -1;
                }

                Pthread_mutex_lock(&stats_mtx);
                openlock_no++;
                lock_no++;
                if (curr_no>max_no_reached) max_no_reached = curr_no;
                Pthread_mutex_unlock(&stats_mtx);

                Pthread_mutex_unlock(&(tmp_lst->mtx));
                return 0;
            }

            errno = ENFILE;
            return -1;
         }

        default :
         {
             errno = EINVAL;
             return -1;
         }
    }
}
static int read_File (char* path, char* buf, size_t* size, size_t c_pid)
{
    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    file* tmp = hash_get_file(storage,path);
    if (tmp == NULL) return -1;
    f_list* tmp_lst = hash_get_list(storage,path);
    if (tmp == NULL) return -1;

    Pthread_mutex_lock(&tmp_lst->mtx);

    if (hash_cont_file(storage,path))
    {
        if (tmp->lock_owner == 0 || tmp->lock_owner == c_pid)
        {
            *size = strlen((char*)tmp->cnt);

            strcpy(buf, tmp->cnt);

            Pthread_mutex_lock(&stats_mtx);
            read_no++;
            total_read_size = total_read_size + (*size);
            Pthread_mutex_unlock(&stats_mtx);

            if(c_list_rem_node(tmp->whoop,c_pid) == -1)
            {
                Pthread_mutex_unlock(&tmp_lst->mtx);
                return -1;
            }

            Pthread_mutex_unlock(&tmp_lst->mtx);
            return 1;
        }
        else
        {
            errno = EPERM;
            Pthread_mutex_unlock(&tmp_lst->mtx);
            return -1;
        }
    }

    errno = ENOENT;
    Pthread_mutex_unlock(&tmp_lst->mtx);
    return -1;
}
static f_list* read_N_File (int N, int* count, size_t c_pid)
{
    if ((*count) != 0)
    {
        errno = EINVAL;
        return NULL;
    }

    int i;
    int r_num = 0;
    size_t total = 0;
    f_list* out = f_list_init();
    if (out == NULL)
    {
        return NULL;
    }

    if (N <= 0)
    {
        for (i = 0; i<storage->lst_no; i++)
        {
            file* cursor = storage->lists[i]->head;
            Pthread_mutex_lock(&storage->lists[i]->mtx);

            while(cursor != NULL)
            {
                //anche i files chiusi possno essere letti, ma questo non vale per i files lockati
                if(cursor->lock_owner == 0 || cursor->lock_owner == c_pid)
                {
                    file* copy = file_copy(cursor);
                    if(copy == NULL) return NULL;
                    total = total + strlen(copy->cnt);
                    if(f_list_add_head(out, copy) == -1) return NULL;
                    if(c_list_rem_node(cursor->whoop,c_pid) == -1) return NULL;
                    r_num++;
                }
                cursor = cursor->next;
            }

            Pthread_mutex_unlock(&storage->lists[i]->mtx);
        }
    }
    else
    {
        int pkd = 0;
        i = 0;
        while(i<storage->lst_no && pkd<N)
        {
            file* cursor = storage->lists[i]->head;
            Pthread_mutex_lock(&storage->lists[i]->mtx);

            while(cursor != NULL && pkd<N)
            {//anche i files chiusi possno essere letti, ma questo non vale per i files lockati
                if (cursor->lock_owner == 0 || cursor->lock_owner == c_pid)
                {
                    file* copy = file_copy(cursor);
                    if(copy == NULL)
                    {
                        Pthread_mutex_lock(&storage->lists[i]->mtx);
                        return NULL;
                    }
                    total = total + strlen(copy->cnt);
                    if(f_list_add_head(out, copy) == -1)
                    {
                        Pthread_mutex_lock(&storage->lists[i]->mtx);
                        return NULL;
                    }
                    if(c_list_rem_node(cursor->whoop,c_pid) == -1)
                    {
                        Pthread_mutex_lock(&storage->lists[i]->mtx);
                        return NULL;
                    }
                    pkd++;
                    r_num++;
                    total = total + strlen(copy->cnt);
                }
                cursor = cursor->next;
            }

            Pthread_mutex_unlock(&storage->lists[i]->mtx);
            i++;
        }
    }

    Pthread_mutex_lock(&stats_mtx);
    total_read_size = total_read_size + total;
    read_no = read_no + r_num;
    Pthread_mutex_unlock(&stats_mtx);
    (*count) = r_num;
    return out;
}
static f_list* write_File (char* path, char* cnt, size_t c_pid)
{
    if (path == NULL || cnt == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    file* tmp = hash_get_file(storage,path);
    if (tmp == NULL) return NULL;
    f_list* tmp_lst = hash_get_list(storage,path);
    if (tmp == NULL) return NULL;

    Pthread_mutex_lock(&(tmp_lst->mtx));

    if (!tmp->op)
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }
    if (tmp->lock_owner == 0 || tmp->lock_owner != c_pid)
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }

    int prec_op = c_list_cont_node(tmp->whoop, c_pid);
    if (prec_op == -1)
    {
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }
    if (prec_op == 0)
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }

    size_t size_o = strlen(tmp->cnt);
    free(tmp->cnt);
    size_t size_na = strlen(cnt);
    tmp->cnt = malloc(sizeof(char)*MAX_CNT_LEN);
    if (strcpy(tmp->cnt,cnt) == NULL)//size_na
    {
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }
    Pthread_mutex_lock(&stats_mtx);
    curr_size = curr_size + size_na - size_o;
    write_no++;
    total_write_size = total_write_size + size_na;
    Pthread_mutex_unlock(&stats_mtx);

    f_list* out = hash_replace(storage,path,c_pid);

    Pthread_mutex_unlock(&(tmp_lst->mtx));
    return out;

}
static f_list* append_to_File (char* path, char* cnt, size_t c_pid)
{
    if (path == NULL || cnt == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    file* tmp = hash_get_file(storage,path);
    if(tmp == NULL) return NULL;
    f_list* tmp_lst = hash_get_list(storage,path);
    if(tmp_lst == NULL) return NULL;

    Pthread_mutex_lock(&(tmp_lst->mtx));

    if (tmp == NULL)
    {
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }
    if (!tmp->op)
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }
    if (tmp->lock_owner == 0 || tmp->lock_owner != c_pid)
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }

    int prec_op = c_list_cont_node(tmp->whoop, c_pid);
    if (prec_op == -1)
    {
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }
    if (prec_op == 0)
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }

    size_t size_o = strlen(tmp->cnt);
    size_t size_na = strlen(cnt);

    if (strncat(tmp->cnt,cnt,size_o+size_na) == NULL)
    {
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }

    Pthread_mutex_lock(&stats_mtx);
    curr_size = curr_size + size_na;
    write_no++;
    total_write_size = total_write_size + size_na;
    Pthread_mutex_unlock(&stats_mtx);

    f_list* out = hash_replace(storage,path,c_pid);
    Pthread_mutex_unlock(&(tmp_lst->mtx));
    return out;
}
static int lock_File (char* path, size_t c_pid)
{
    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    file* tmp = hash_get_file(storage,path);
    if(tmp == NULL) return -1;

    f_list* tmp_lst = hash_get_list(storage,path);
    if(tmp_lst == NULL) return -1;

    Pthread_mutex_lock(&(tmp_lst->mtx));
    tmp->lock_owner = c_pid;

    Pthread_mutex_lock(&stats_mtx);
    lock_no++;
    Pthread_mutex_unlock(&stats_mtx);

    if(c_list_rem_node(tmp->whoop,c_pid) == -1)
    {
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return -1;
    }
    Pthread_mutex_unlock(&(tmp_lst->mtx));
    return 0;
}
static int unlock_File (char* path, size_t c_pid)
{
    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    file* tmp = hash_get_file(storage,path);
    if (tmp == NULL) return -1;

    f_list* tmp_lst = hash_get_list(storage,path);
    if (tmp_lst == NULL) return -1;

    if (tmp->lock_owner == c_pid)
    {
        Pthread_mutex_lock(&(tmp_lst->mtx));
        tmp->lock_owner = 0;

        Pthread_mutex_lock(&stats_mtx);
        unlock_no++;
        Pthread_mutex_unlock(&stats_mtx);

        if(c_list_rem_node(tmp->whoop,c_pid) == -1)
        {
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            return -1;
        }
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return 0;
    }
    else
    {
        errno = EPERM;
        return -1;
    }

}
static int close_File (char* path, size_t c_pid)
{
    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    file* tmp = hash_get_file(storage,path);
    if (tmp == NULL) return -1;
    f_list* tmp_lst = hash_get_list(storage,path);
    if (tmp_lst == NULL) return -1;

    Pthread_mutex_lock(&(tmp_lst->mtx));

    if (tmp->op && (tmp->lock_owner == c_pid || tmp->lock_owner == 0))
    {
        tmp->op = 0;

        Pthread_mutex_lock(&stats_mtx);
        close_no++;
        Pthread_mutex_unlock(&stats_mtx);

        if(c_list_rem_node(tmp->whoop,c_pid) == -1)
        {
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            return -1;
        }
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return 0;
    }
    else
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return -1;
    }

}
static int remove_File (char* path, size_t c_pid)
{
    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    file* tmp = hash_get_file(storage,path);
    if (tmp == NULL) return -1;
    f_list* tmp_lst = hash_get_list(storage,path);
    if (tmp_lst == NULL) return -1;

    Pthread_mutex_lock(&(tmp_lst->mtx));

    if (tmp->lock_owner == c_pid)
    {
        file* dummy = hash_rem_file2(storage,path);
        if (dummy == NULL)
        {
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            return -1;
        }
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        file_free(dummy);
        return 0;
    }
    else
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return -1;
    }

}
//STRUTTURA DI QUEST: FUN_NAME;ARG1;ARG2;...
static void do_a_Job (char* quest, int fd_c, int fd_pipe, int* end)
{
    if (quest == NULL || fd_c < 1 || fd_pipe < 1)
    {
        errno = EINVAL;
        return;
    }

    char out[MSG_SIZE];
    memset(out,0,MSG_SIZE);
    char* token = NULL;
    char* save = NULL;

    token = strtok_r(quest,";",&save);// il token contiene un'operazione che è stata richiesta al server || NULL
    if (token == NULL) // tutte le richieste sono state esaudite
    {
        hash_lo_reset(storage,fd_c);
        *end = 1;
        if (write(fd_pipe, &fd_c, sizeof(fd_c)) == -1)
        {
            perror("Worker : scrittura nella pipe");
            exit(EXIT_FAILURE);
        }
        if (write(fd_pipe, end, sizeof(*end)) == -1)
        {
            perror("Worker : scrittura nella pipe");
            exit(EXIT_FAILURE);
        }
    }

    if (strcmp(token,"openFile") == 0)
    {
        // struttura tipica del comando: openFile;pathname;flags;

        // tokenizzazione degli argomenti
        token = strtok_r(NULL,";",&save);
        char path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
        strcpy(path,token);
        token = strtok_r(NULL,";",&save);
        int flags = (size_t) atoi(token);
        /*
        token = strtok_r(NULL,";",&save);
        size_t c_pid = (size_t) atoi(token);
        */

        // esecuzione della richiesta
        int res;// valore resituito in output dalla openFile di server.c
        int log_res;
        res = open_File(path,flags,fd_c);
        if (res == -1)
        {
            log_res = 0;
            sprintf(out,"-1;%d;",errno);
        }
        else
        {
            log_res = 1;
            sprintf(out,"0");
        }
        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            perror("Worker : scrittura nel socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            return;
        }

        // UPDATE DEL FILE DI LOG
        // open_File : op/Thrd_id/3/(0|1)/File_Path/flags
        Pthread_mutex_lock(&log_mtx);
        fprintf(log_file,"op/%lu/3/%d/%s/%d\n",pthread_self(),log_res,path,flags);
        Pthread_mutex_unlock(&log_mtx);
    }
    else
    if (strcmp(token,"closeFile") == 0)
    {
        // struttura tipica del comando: closeFile;pathname;

        // tokenizzazione degli argomenti
        token = strtok_r(NULL,";",&save);
        char path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
        strcpy(path,token);
        /*
        token = strtok_r(NULL,";",&save);
        size_t c_pid = (size_t) atoi(token);
        */
        // esecuzione della richiesta
        int res;
        int log_res;
        res = close_File(path,fd_c);
        if (res == -1)
        {
            log_res = 0;
            sprintf(out,"-1;%d;",errno);
        }
        else
        {
            log_res = 1;
            sprintf(out,"0");
        }
        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            return;
        }

        // UPDATE DEL FILE DI LOG
        // close_File : op/Thrd_id/10/(0|1)/File_Path
        Pthread_mutex_lock(&log_mtx);
        fprintf(log_file,"op/%lu/10/%d/%s\n",pthread_self(),log_res,path);
        Pthread_mutex_unlock(&log_mtx);
    }
    else
    if (strcmp(token,"lockFile") == 0)
    {
        // struttura tipica del comando: lockFile;pathname;

        // tokenizzazione degli argomenti
        token = strtok_r(NULL,";",&save);
        char path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
        strcpy(path,token);
        /*
        token = strtok_r(NULL,";",&save);
        size_t c_pid = (size_t) atoi(token);
        */

        // esecuzione della richiesta
        int res;
        int log_res;
        res = lock_File(path,fd_c);
        if (res == -1)
        {
            log_res = 0;
            sprintf(out,"-1;%d;",errno);
        }
        else
        {
            log_res = 1;
            sprintf(out,"0");
        }
        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            perror("Worker : scrittura nel socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            return;
        }

        // UPDATE DEL FILE DI LOG
        // lock_File : op/Thrd_id/8/(0|1)/File_Path
        Pthread_mutex_lock(&log_mtx);
        fprintf(log_file,"op/%lu/8/%d/%s\n",pthread_self(),log_res,path);
        Pthread_mutex_unlock(&log_mtx);
    }
    else
    if (strcmp(token,"unlockFile") == 0)
    {
        // struttura tipica del comando: unlockFile;pathname;

        // tokenizzazione degli argomenti
        token = strtok_r(NULL,";",&save);
        char path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
        strcpy(path,token);
        /*
        token = strtok_r(NULL,";",&save);
        size_t c_pid = (size_t) atoi(token);
        */

        // esecuzione della richiesta
        int res;
        int log_res;
        res = unlock_File(path,fd_c);
        if (res == -1)
        {
            log_res = 0;
            sprintf(out,"-1;%d;",errno);
        }
        else
        {
            log_res = 1;
            sprintf(out,"0");
        }
        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            perror("Worker : scrittura nel socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            return;
        }

        // UPDATE DEL FILE DI LOG
        // unlock_File : op/Thrd_id/9/(0|1)/File_Path
        Pthread_mutex_lock(&log_mtx);
        fprintf(log_file,"op/%lu/9/%d/%s\n",pthread_self(),log_res,path);
        Pthread_mutex_unlock(&log_mtx);
    }
    else
    if (strcmp(token,"removeFile") == 0)
    {
        // struttura tipica del comando: removeFile;pathname;

        // tokenizzazione degli argomenti
        token = strtok_r(NULL,";",&save);
        char path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
        strcpy(path,token);
        /*
        token = strtok_r(NULL,";",&save);
        size_t c_pid = (size_t) atoi(token);
        */
        int res = remove_File(path,fd_c);
        int log_res;

        if (res == -1)
        {
            log_res = 0;
            sprintf(out,"-1;%d;",errno);
        }
        else
        {
            log_res = 1;
            sprintf(out,"0");
        }

        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            perror("Worker : scrittura nel socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            return;
        }

        // UPDATE DEL FILE DI LOG
        // remove_File : op/Thrd_id/11/(0|1)/File_Path
        Pthread_mutex_lock(&log_mtx);
        fprintf(log_file,"op/%lu/11/%d/%s\n",pthread_self(),log_res,path);
        Pthread_mutex_unlock(&log_mtx);
    }
    else
    if (strcmp(token,"writeFile") == 0)
    {
        // struttura tipica del comando: writeFile;pathname;cnt;

        // tokenizzazione degli argomenti
        token = strtok_r(NULL,";",&save);
        char path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
        strcpy(path,token);
        token = strtok_r(NULL,";",&save);
        char cnt[MSG_SIZE];
        strcpy(cnt,token);
        size_t cnt_size = strnlen(cnt,MSG_SIZE);
        /*
        token = strtok_r(NULL,";",&save);
        size_t c_pid = (size_t) atoi(token);
         */
        // esecuzione della richiesta
        errno = 0;
        int log_res;
        f_list* tmp = write_File(path,cnt,fd_c);

        if (errno != 0)
        {
            log_res = 0;
            sprintf(out,"-1;%d;",errno);
        }
        else
        {
            log_res = 1;
            sprintf(out,"%lu;",(tmp->size));
        }

        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            perror("Worker : scrittura nel socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            f_list_free(tmp);
            return;
        }

        char bck[MSG_SIZE];
        if (readn(fd_c,bck,MSG_SIZE) == -1)
        {
            perror("Worker : lettura dal socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            f_list_free(tmp);
            return;
        }

        int true_bck = (int)strtol(bck, NULL, 10);
        file* cursor = tmp->head;
        size_t rpl_size = 0;
        size_t rpl_no = 0;
        int rpl;
        if (tmp->size > 0) rpl = 1;
        else rpl = 0;

        while (true_bck == 1 && cursor != NULL)
        {
            sprintf(out,"%s;%s",cursor->path,cursor->cnt);
            rpl_size = rpl_size + strnlen(cursor->cnt,MSG_SIZE);
            rpl_no++;

            if (writen(fd_c,out,MSG_SIZE) == -1)
            {
                perror("Worker : scrittura nel socket");
                *end = 1;
                write(fd_pipe,&fd_c,sizeof(fd_c));
                write(fd_pipe,end,sizeof(*end));
                f_list_free(tmp);
                return;
            }

            if (readn(fd_c,bck,MSG_SIZE) == -1)
            {
                perror("Worker : lettura dal socket");
                *end = 1;
                write(fd_pipe,&fd_c,sizeof(fd_c));
                write(fd_pipe,end,sizeof(*end));
                f_list_free(tmp);
                return;
            }
            true_bck = (int)strtol(bck, NULL, 10);
            cursor = cursor->next;
        }

        // UPDATE DEL FILE DI LOG
        // write_File : op/Thrd_id/6/(0|1)/File_Path/size/Rplc(0|1)/Rplc_no/Rplc_size
        Pthread_mutex_lock(&log_mtx);
        fprintf(log_file,"op/%lu/6/%d/%s/%lu/%d/%lu/%lu\n",pthread_self(),log_res,path,cnt_size,rpl,rpl_no,rpl_size);
        Pthread_mutex_unlock(&log_mtx);
        f_list_free(tmp);
    }
    else
    if (strcmp(token,"appendToFile") == 0)
    {
        // struttura tipica del comando: appendToFile;pathname;cnt;

        // tokenizzazione degli argomenti
        token = strtok_r(NULL,";",&save);
        char path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
        strcpy(path,token);
        token = strtok_r(NULL,";",&save);
        char cnt[MSG_SIZE];
        strcpy(cnt,token);
        size_t cnt_size = strnlen(cnt,MSG_SIZE);
        /*
        token = strtok_r(NULL,";",&save);
        size_t c_pid = (size_t) atoi(token);
         */

        // esecuzione della richiesta
        errno = 0;
        f_list* tmp = append_to_File(path,cnt,fd_c);
        int log_res;

        if (errno != 0)
        {
            log_res = 0;
            sprintf(out,"-1;%d;",errno);
        }
        else
        {
            log_res = 1;
            sprintf(out,"%lu",(tmp->size));
        }

        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            perror("Worker : scrittura nel socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            f_list_free(tmp);
            return;
        }

        char bck[MSG_SIZE];
        if (readn(fd_c,bck,MSG_SIZE) == -1)
        {
            perror("Worker : lettura dal socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            f_list_free(tmp);
            return;
        }
        int true_bck = (int)strtol(bck, NULL, 10);
        file* cursor = tmp->head;
        size_t rpl_size = 0;
        size_t rpl_no = 0;
        int rpl;
        if (tmp->size > 0) rpl = 1;
        else rpl = 0;

        while (true_bck == 1 && cursor != NULL)
        {
            sprintf(out,"%s;%s",cursor->path,cursor->cnt);
            rpl_size = rpl_size + strnlen(cursor->cnt,MSG_SIZE);
            rpl_no++;

            if (writen(fd_c,out,UNIX_MAX_STANDARD_FILENAME_LENGHT + MSG_SIZE + 1) == -1)
            {
                perror("Worker : scrittura nel socket");
                *end = 1;
                write(fd_pipe,&fd_c,sizeof(fd_c));
                write(fd_pipe,end,sizeof(*end));
                f_list_free(tmp);
                return;
            }

            if (readn(fd_c,bck,MSG_SIZE) == -1)
            {
                perror("Worker : lettura dal socket");
                *end = 1;
                write(fd_pipe,&fd_c,sizeof(fd_c));
                write(fd_pipe,end,sizeof(*end));
                f_list_free(tmp);
                return;
            }
            true_bck = (int)strtol(bck, NULL, 10);
        }

        // UPDATE DEL FILE DI LOG
        // append_to_File : op/Thrd_id/7/(0|1)/File_Path/size/Rplc(0|1)/Rplc_no/Rplc_size
        Pthread_mutex_lock(&log_mtx);
        fprintf(log_file,"op/%lu/7/%d/%s/%lu/%d/%lu/%lu\n",pthread_self(),log_res,path,cnt_size,rpl,rpl_no,rpl_size);
        Pthread_mutex_unlock(&log_mtx);
        f_list_free(tmp);
    }
    else
    if (strcmp(token,"readFile") == 0)
    {
        // struttura tipica del comando: readFile;pathname;

        // tokenizzazione degli argomenti
        token = strtok_r(NULL,";",&save);
        char path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
        strcpy(path,token);
        /*
        token = strtok_r(NULL,";",&save);
        size_t size = (size_t) atoi(token);

        token = strtok_r(NULL,";",&save);
        size_t c_pid = (size_t) atoi(token);
         */

        char* buf = malloc(sizeof(char)*MAX_CNT_LEN);
        if (buf == NULL)
        {
            errno = ENOMEM;
            free(buf);
            return;
        }

        size_t size;

        int res = read_File(path,buf,&size,fd_c);
        int log_res;

        if (res == -1)
        {
            log_res = 0;
            sprintf(out,"-1;%d;",errno);
        }
        else
        {
            log_res = 1;
            sprintf(out,"%s;%ld",buf,size);
        }

        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            perror("Worker : scrittura nel socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            free(buf);
            return;
        }

        // UPDATE DEL FILE DI LOG
        // read_File : op/Thrd_id/4/(0|1)/File_Path/size
        Pthread_mutex_lock(&log_mtx);
        fprintf(log_file,"op/%lu/4/%d/%s/%ld\n",pthread_self(),log_res,path,size);
        Pthread_mutex_unlock(&log_mtx);
        free(buf);
    }
    else
    if (strcmp(token,"readNFiles")==0)
    {
        // struttura tipica del comando: readNFile;N;

        // tokenizzazione degli argomenti
        token = strtok_r(NULL,";",&save);
        int N = (int)strtol(token, NULL, 10);
        /*
        token = strtok_r(NULL,";",&save);
        size_t c_pid = (size_t) atoi(token);
        */

        int count = 0;

        errno = 0;
        int log_res;
        f_list* tmp = read_N_File(N,&count,fd_c);

        if (errno != 0)
        {
            log_res = 0;
            sprintf(out,"-1;%d;",errno);
        }
        else
        {
            log_res = 1;
            sprintf(out,"%d;",count);
        }

        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            perror("Worker : scrittura nel socket");
            *end = 1;
            write(fd_pipe,&fd_c,sizeof(fd_c));
            write(fd_pipe,end,sizeof(*end));
            f_list_free(tmp);
            return;
        }

        char bck[MSG_SIZE];
        if (readn(fd_c, bck, MSG_SIZE) == -1) {
            perror("Worker : lettura dal socket");
            *end = 1;
            write(fd_pipe, &fd_c, sizeof(fd_c));
            write(fd_pipe, end, sizeof(*end));
            f_list_free(tmp);
            return;
        }
        int true_bck = (int)strtol(bck, NULL, 10);
        file* cursor = tmp->head;

        while (true_bck == 1 && cursor != NULL)
        {
            char readN_out[UNIX_MAX_STANDARD_FILENAME_LENGHT + MAX_CNT_LEN + 1];
            memset(readN_out,0,UNIX_MAX_STANDARD_FILENAME_LENGHT + MAX_CNT_LEN + 1);
            sprintf(readN_out,"%s;%s",cursor->path,cursor->cnt);
            if (writen(fd_c,readN_out,UNIX_MAX_STANDARD_FILENAME_LENGHT + MAX_CNT_LEN + 1) == -1)
            {
                perror("Worker : scrittura nel socket");
                *end = 1;
                write(fd_pipe,&fd_c,sizeof(fd_c));
                write(fd_pipe,end,sizeof(*end));
                f_list_free(tmp);
                return;
            }

            if (readn(fd_c,bck,MSG_SIZE) == -1)
            {
                perror("Worker : lettura dal socket");
                *end = 1;
                write(fd_pipe,&fd_c,sizeof(fd_c));
                write(fd_pipe,end,sizeof(*end));
                f_list_free(tmp);
                return;
            }
            true_bck = (int)strtol(bck, NULL, 10);

            cursor = cursor->next;
        }

        // UPDATE DEL FILE DI LOG
        // read_NFiles : op/Thrd_id/5/(0|1)/Read_no/size
        Pthread_mutex_lock(&log_mtx);
        fprintf(log_file,"op/%lu/5/%d/%d/",pthread_self(),log_res,count);

        file* curs = tmp->head;
        size_t tot_size = 0;

        while(curs != NULL)
        {
            //fprintf(log_file,"%s/",curs->path);
            tot_size = tot_size + strlen(curs->cnt);
            curs = curs->next;
        }

        fprintf(log_file,"%lu\n",tot_size);
        Pthread_mutex_unlock(&log_mtx);
        f_list_free(tmp);
    }
    else
    if (strcmp(token,"disconnect")==0)
    {// il client è disconnesso, il worker attenderà il prossimo
        *end = 1;
        if (write(fd_pipe, &fd_c, sizeof(fd_c)) == -1)
        {
            perror("Worker : scrittura nella pipe");
            exit(EXIT_FAILURE);
        }
        if (write(fd_pipe, end, sizeof(*end)) == -1)
        {
            perror("Worker : scrittura nella pipe");
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        // funzione non implementata <-> ENOSYS
        sprintf(out,"-1;%d",ENOSYS);
        if (writen(fd_c,out,MSG_SIZE) == -1)
        {
            perror("Worker : scrittura nel socket");
            *end = 1;
            if (write(fd_pipe, &fd_c, sizeof(fd_c)) == -1)
            {
                perror("Worker : scrittura nella pipe");
                exit(EXIT_FAILURE);
            }
            if (write(fd_pipe, end, sizeof(*end)) == -1)
            {
                perror("Worker : scrittura nella pipe");
                exit(EXIT_FAILURE);
            }
            return;
        }
    }
}
static void* w_routine (void* arg)
{
    int fd_pipe = *((int*)arg);
    int fd_c;
    int end = 0; //valore indicante la terminazione del client
    while (1)
    {
        //un client viene espulso dalla coda secondo la politica fifo
        Pthread_mutex_lock(&coda_mtx);
        fd_c = c_list_pop_worker(coda);
        Pthread_mutex_unlock(&coda_mtx);

        if (fd_c == -2) return (void*) -1;

        if (fd_c == -1)
        {
            return (void*) 0;
        }

        while (end != 1)
        {
            char quest [MSG_SIZE];
            memset(quest,0,MSG_SIZE);

            //il client viene servito dal worker in ogni sua richiesta sino alla disconnessione
            int len = readn(fd_c, quest, MSG_SIZE);

            //printf("\n il comando letto è : %s",quest);
            if (len == -1)
            {// il client è disconnesso, il worker attenderà il prossimo
                end = 1;
                if (write(fd_pipe, &fd_c, sizeof(fd_c)) == -1)
                {
                    perror("Worker : scrittura nella pipe");
                    exit(EXIT_FAILURE);
                }
                if (write(fd_pipe, &end, sizeof(end)) == -1)
                {
                    perror("Worker : scrittura nella pipe");
                    exit(EXIT_FAILURE);
                }
            }
            else
            {// richiesta del client ricevuta correttamente
                if (len != 0) do_a_Job(quest, fd_c, fd_pipe, &end);
            }
        }
    }
    return (void*) 0;
}

int main(int argc, char* argv[])
{
    // argv[0] server.c | argv[1] config.txt_path | ... | argv[argc] NULL

    int o;
    int i;
    coda = c_list_init();
    int t_soft = 0;
    char socket_name[100];

    {
        max_no = 20;
        max_size = 20 * 1024 * 1024;
        thread_no = 5;
        strcpy(socket_name,SOCKET_NAME);
    } // valori standard

    {
        char *path_config = NULL;
        if (argc == 3) {
            if (strcmp(argv[1], "-cnfg") == 0) {
                path_config = argv[2];
            }
        }
        // parsing file config.txt -- attributo=valore -- se trovo errore uso attributi di default
        if (path_config != NULL)
        {
            char string[200]; //->str
            FILE *f_point; //->fp
            f_point = fopen(path_config, "r");
            if (f_point == NULL)
            {
                perror("Errore nell'apertura del file di configurazione");
                exit(EXIT_FAILURE);
            }

            char campo[100]; // campo da configurare ->arg
            char valore[100]; // valore del campo ->val
            while (fgets(string, 200, f_point) != NULL)
            {
                if (string[0] != '\n')
                {
                    int nf;
                    nf = sscanf(string, "%[^=]=%s", campo, valore);

                    if (nf != 2)
                    {
                        printf("Errore di Configurazione : formato non corretto\n");
                        printf("Il Server è stato avviato con parametri DEFAULT\n");
                        break;
                    }
                        if (strcmp(campo, "thread_no") == 0)
                        {
                            long n;
                            int out = isNumber(valore, &n);
                            if (out == 2 || out == 0 || n <= 0)
                            {
                                printf("Errore di Configurazione : thread_no deve essere >= 0\n");
                                printf("Il Server è stato avviato con parametri DEFAULT\n");
                                break;
                            }
                            else thread_no = (size_t) n;
                        }
                        else
                            if (strcmp(campo, "socket_name") == 0)
                            {
                                strcpy(socket_name,valore);
                            }
                            else
                                if (strcmp(campo, "max_no") == 0)
                            {
                                long n;
                                int out = isNumber(valore, &n);
                                if (out == 2 || out == 0 || n <= 0)
                                {
                                    printf("Errore di Configurazione : max_no deve essere >= 0\n");
                                    printf("Il Server è stato avviato con parametri DEFAULT\n");
                                    break;
                                }
                                else max_no = (size_t) n;
                            }
                                else
                                    if (strcmp(campo, "max_size") == 0)
                                {
                                    long n;
                                    int out = isNumber(valore, &n);
                                    if (out == 2 || out == 0 || n <= 0)
                                    {
                                        printf("Errore di Configurazione : max_size deve essere >= 0\n");
                                        printf("Il Server è stato avviato con parametri DEFAULT\n");
                                        break;
                                    }
                                    else max_size = (size_t) n;
                                }

                }
            }
            fclose(f_point);
        }
        else printf("Server avviato con parametri di DEFAULT\n");
        printf("Server INFO: socket_name:%s / num_thread:%lu / max_files:%lu / max_size:%lu\n", socket_name, thread_no,
               max_no,
               max_size);
    } // parsing del file di config

    {
        sigset_t set;
        o = sigfillset(&set);
        if (o == -1)
        {
            perror("sigfillset");
            exit(EXIT_FAILURE);
        }
        o = pthread_sigmask(SIG_SETMASK, &set, NULL);
        if (o == -1)
        {
            perror("pthread_sigmask");
            exit(EXIT_FAILURE);
        }

        struct sigaction s;
        memset(&s, 0, sizeof(s)); // setta tutta la struct a 0
        s.sa_handler = t_gstr;

        o = sigaction(SIGINT, &s, NULL);  // imposto l'handler per SIGINT
        if (o == -1)
        {
            perror("sigaction");
            exit(EXIT_FAILURE);
        }
        o = sigaction(SIGQUIT, &s, NULL);  // imposto l'handler per SIGQUIT
        if (o == -1)
        {
            perror("sigaction");
            exit(EXIT_FAILURE);
        }
        o = sigaction(SIGHUP, &s, NULL);  // imposto l'handler per SIGHUP
        if (o == -1)
        {
            perror("sigaction");
            exit(EXIT_FAILURE);
        }

        // gestisco SIGPIPE ignorandolo
        s.sa_handler = SIG_IGN;
        o = sigaction(SIGPIPE, &s, NULL);
        if (o == -1)
        {
            perror("sigaction");
            exit(EXIT_FAILURE);
        }

        // setto la maschera del thread a 0
        o = sigemptyset(&set);
        if (o == -1)
        {
            perror("sigemptyset");
            exit(EXIT_FAILURE);
        }
        o = pthread_sigmask(SIG_SETMASK, &set, NULL);
        if (o == -1)
        {
            perror("pthread_sigmask");
            fflush(stdout);
            exit(EXIT_FAILURE);
        }
    } // gestione dei segnali

    {
        // STRUTTURE PRINCIPALI
        size_t hash_list_no = max_no * 2;
        storage = hash_init(hash_list_no);
        if (storage == NULL)
        {
            errno = ENOMEM;
            return -1;
        }

        queue = fifo_init();
        if (queue == NULL)
        {
            errno = ENOMEM;
            return -1;
        }

        // FILE DI LOG
        log_file = fopen(LOG_NAME,"w"); // apro il file di log in scrittura
        fflush(log_file);   // ripulisco il file aperto da precedenti scritture
        fprintf(log_file,"START\n"); // scrivo l'avvio del server sul file di log

        // COMUNICAZIONE MAIN <-> WORKER
        int pip[2];
        o = pipe(pip);
        if (o == -1)
        {
            perror("creazione pipe");
            exit(EXIT_FAILURE);
        }

        // INIZIALIZZAZIONE THREAD POOL
        pthread_t* t_pool = malloc(sizeof(pthread_t)*thread_no);
        if (t_pool == NULL)
        {
            perror("inizializzazione thread_pool");
            exit(EXIT_FAILURE);
        }
        for (i=0; i<thread_no; i++)
        {
            // il thread worker i-esimo riceve l'indirizzo del lato scrittura della pipe
            o = pthread_create(&t_pool[i],NULL,w_routine,(void*)(&pip[1]));
            if (o == -1)
            {
                perror("creazione pthread");
                fflush(stdout);
                exit(EXIT_FAILURE);
            }
        }

        //SOCKET
        int fd_skt;
        int fd_c;
        int fd_num = 0;
        int fd;
        fd_set set;
        fd_set rd_set;
        struct sockaddr_un sa;
        strncpy(sa.sun_path,socket_name,UNIX_MAX_STANDARD_FILENAME_LENGHT);
        sa.sun_family = AF_UNIX;

        if ((fd_skt = socket(AF_UNIX,SOCK_STREAM,0)) == -1)
        {
            perror("creazione del socket");
            exit(EXIT_FAILURE);
        }

        o = bind(fd_skt,(struct sockaddr*)&sa,sizeof(sa));
        if (o == -1)
        {
            perror("bind del socket");
            exit(EXIT_FAILURE);
        }

        listen(fd_skt,SOMAXCONN);
        if (o == -1)
        {
            perror("listen del socket");
            exit(EXIT_FAILURE);
        }

        //fd_num ha il valore del massimo descrittore attivo -> utile per la select
        if (fd_skt > fd_num) fd_num = fd_skt;
        //registrazione del socket
        FD_ZERO(&set);
        FD_SET(fd_skt,&set);
        //registrazione della pipe
        if (pip[0] > fd_num) fd_num = pip[0];
        FD_SET(pip[0],&set);

        printf("Attesa dei Clients\n");

        while (1)
        {
            rd_set = set;//ripristino il set di partenza
            if (select(fd_num+1,&rd_set,NULL,NULL,NULL) == -1)
            {//gestione errore
                if (t==1) break;//chiusura violenta
                else if (t==2) { //chiusura soft
                    if (currconnections_no==0) break;
                    else {
                        printf("Chiusura Soft\n");
                        FD_CLR(fd_skt,&set);//rimozione del fd del socket dal set, non accetteremo altre connessioni
                        if (fd_skt == fd_num) fd_num = max_up(set,fd_num);//aggiorno l'indice massimo
                        close(fd_skt);//chiusura del socket
                        rd_set = set;
                        o = select(fd_num+1,&rd_set,NULL,NULL,NULL);
                        if (o == -1)
                        {
                            perror("select");
                            break;
                        }
                    }
                }else {//fallimento select
                    perror("select");
                    break;
                }
            }
            //controlliamo tutti i file descriptors
            for (fd=0; fd<= fd_num; fd++) {
                if (FD_ISSET(fd,&rd_set))
                {
                    if (fd == fd_skt) //il socket è pronto per accettare una nuova richiesta di connessine
                    {
                        if ((fd_c = accept(fd_skt,NULL,0)) == -1)
                        {
                            if (t==1) break;//terminazione violenta
                            else if (t==2) {//terminazione soft
                                if (currconnections_no==0) break;
                            }else {
                                perror("Errore dell' accept");
                            }
                        }
                        FD_SET(fd_c,&set);//il file del client è pronto in lettura
                        if (fd_c > fd_num) fd_num = fd_c;//tengo aggiornato l'indice massimo
                        currconnections_no++;//aggiornamento variabili per le statistiche
                        if(currconnections_no > max_connections_no) max_connections_no = currconnections_no;
                        //printf ("SERVER : Connessione con il Client completata\n");
                    }
                    else
                        if (fd == pip[0])
                        {// il client è pronto in lettura
                        int fd_c1;
                        int l;
                        int flag;
                        if ((l = read(pip[0],&fd_c1,sizeof(fd_c1))) > 0)
                        { //lettura del fd di un client
                            o = read(pip[0],&flag,sizeof(flag));
                            if (o == -1)
                            {
                                perror("errore nel dialogo Master/Worker");
                                exit(EXIT_FAILURE);
                            }
                            if (flag == 1)
                            {//il client è terminato, il suo fd deve essere rimosso dal set
                                printf("Chiusura della connessione\n");
                                FD_CLR(fd_c1,&set);//rimozione del fd del client termianto dal set
                                if (fd_c1 == fd_num) fd_num = max_up(set,fd_num);//aggiorno l'indice massimo
                                close(fd_c1);//chiusura del client
                                currconnections_no--;//aggiornamento delle variabili per le statistiche
                                if (t==2 && currconnections_no==0)
                                {
                                    printf("Terminazione Soft\n");
                                    t_soft = 1;
                                    break;
                                }
                            }
                            else
                            {//la richiesta di c1 è stata soddisfatta, aggiorno lo stato del client come pronto
                                FD_SET(fd_c1,&set);
                                if (fd_c1 > fd_num) fd_num = fd_c1;//mi assicuro che fd_num contenga l'indice massimo
                            }
                        }
                        else
                            if (l == -1)
                            {
                                perror("errore nel dialogo Master/Worker");
                                exit(EXIT_FAILURE);
                            }
                    }
                        else
                        {
                            //il fd individuato è quello del canale di comunicazione client/server
                            //il client è pronto in lettura
                            Pthread_mutex_lock(&coda_mtx);
                            c_list_add_head(coda,fd);
                            pthread_cond_signal(&wait_coda);
                            Pthread_mutex_unlock(&coda_mtx);

                            FD_CLR(fd,&set);
                        }
                }
            }
            if (t_soft==1) break;
        }

        printf("\nChiusura del Server...\n");

        Pthread_mutex_lock(&coda_mtx);
        for (i=0;i<thread_no;i++)
        {
            c_list_add_head(coda,-1);
            pthread_cond_signal(&wait_coda);
        }
        Pthread_mutex_unlock(&coda_mtx);

        for (i=0;i<thread_no;i++)
        {
            if (pthread_join(t_pool[i],NULL) != 0)
            {
                perror("Errore in thread join");
                exit(EXIT_FAILURE);
            }
        }
        free(t_pool);
        remove(socket_name);
    } // server core

    {
        size_t fake_read_no = 1;
        size_t fake_write_no = 1;
        if (read_no != 0) fake_read_no = read_no;
        if (write_no != 0) fake_write_no = write_no;
        media_read_size = (float) total_read_size/fake_read_no;
        media_write_size = (float) total_write_size/fake_write_no;
        max_size_reachedMB = (float) max_size_reached/(1024*1024);

    } // elaborazioni per il file delle statistiche

    {
        fprintf(log_file,"END\n");
        fprintf(log_file,"SUNTO DELLE STATISTICHE:\n");
        fprintf(log_file,"-Numero di read: %lu;\n-Size media delle letture in bytes: %lu;\n-Numero di write: %lu;\n-Size media delle scritture in bytes: %lu;\n-Numero di lock: %lu;\n-Numero di openlock: %lu;\n-Numero di unlock: %lu;\n-Numero di close: %lu;\n-Dimensione massima dello storage in MB: %lu;\n-Dimensione massima dello storage in numero di files: %lu;\n-Numero di replace per selezionare un file vittima: %lu;\n-Massimo numero di connessioni contemporanee: %lu;\n",read_no,media_read_size,write_no,media_write_size,lock_no,openlock_no,unlock_no,close_no,max_size_reached,max_no_reached,replace_no,max_connections_no);
        // il numero di richieste soddisfatte da ogni thread è lasciato al parsing in statistiche.sh
    } // chiusura del file di log

    {
        printf("SERVER INFO:\n");
        printf("Numero Massimo di files raggiunto: %lu\n", max_no_reached);
        printf("Dimensione Massima raggiunta dallo storage in MByte: %lu\n", max_size_reachedMB);
        printf("Dimensione Massima raggiunta dallo storage in Byte: %lu\n", max_size_reached);
        printf("Dimensione media delle read effettuate: %lu\n", media_read_size);
        printf("Dimensione media delle write effettuate: %lu\n", media_write_size);
        printf("Numero di volte in cui è stato avvio l'algoritmo: %lu\n\n", replace_alg_no);
        printf("Di seguito è riportato lo stato dello storage al momento della chiusura:\n");
        hash_print(storage);
    } // sunto sull'esecuzione

    {
        hash_free(storage);
        fifo_free(queue);
        c_list_free(coda);
        fclose(log_file);

    } // ultime free

}

// UPDATE: TEST 1 SUCCESSO