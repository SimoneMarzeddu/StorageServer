#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <assert.h>

#define MSG_SIZE 2048   // dimensione di alcuni messaggi scambiati tra server ed API
#define MAX_CNT_LEN 1024 // grandezza massima del contenuto di un file: 1 KB
#define UNIX_MAX_STANDARD_FILENAME_LENGHT 108 /* man 7 unix */
#define SOCKET_NAME "./ssocket.sk"  // nome di default per il socket
#define LOG_NAME "./log.txt"    // nome di default per il file di log

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
    size_t c_info;
    struct scnode* next;
    struct scnode* prec;
} c_node;

typedef struct sclist
{
    c_node* head;
    c_node* tail;
} c_list;

typedef struct sp_queue_node
{
    char* path;
    struct sp_queue_node* next;
    struct sp_queue_node* prec;
}p_queue_node;

typedef struct sp_queue
{
    p_queue_node* head;
    p_queue_node* tail;
} p_queue;

typedef struct sfile
{
    char* path;
    char* cnt;
    c_list* whoop;
    size_t lock_owner;
    size_t op;
    struct sfile* next;
    struct sfile* prec;
    p_queue_node* queue_pointer;
} file;

typedef struct sfilelst
{
    file* head;
    file* tail;
    size_t size;
    pthread_mutex_t mtx;
}f_list;

typedef struct shash
{
    f_list** lists;
    size_t lst_no;
}hash;

//  VARIABILI GLOBALI SULLA GESTIONE DELLO STORAGE  //

/*
 * 0 -> FIFO
 * 1 -> LRU
*/
static size_t politic = 0; // flag indicante la politica adottata

static size_t thread_no;    // numero di thread worker del server
static size_t max_size;    // dimensione massima dello storage
static size_t max_no;      // numero massimo di files nello storage
static size_t curr_size = 0;   // dimensione corrente dello storage
static size_t curr_no = 0;     // numero corrente di files nello storage

static hash* storage = NULL;    // struttura dati in cui saranno raccolti i files amministratidal server

static p_queue* queue = NULL;      // struttura dati di appoggio per la gestione dei rimpizzamenti con politica p_queue
pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER; // mutex per mutua esclusione sulla coda p_queue

static c_list* coda = NULL;     // struttura dati di tipo coda FIFO per la comunicazione Master/Worker (uso improprio della nomenclatura "c_info")
pthread_mutex_t coda_mtx = PTHREAD_MUTEX_INITIALIZER; // mutex per mutua esclusione sugli accessi alla coda
pthread_cond_t wait_coda = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lock_mtx = PTHREAD_MUTEX_INITIALIZER; // mutex per attesa di ottenimento della lock
pthread_cond_t wait_server_lock = PTHREAD_COND_INITIALIZER;

volatile sig_atomic_t t = 0; // variabile settata dal signal handler

//  VARIABILI PER LE STATISTICHE //
pthread_mutex_t stats_mtx = PTHREAD_MUTEX_INITIALIZER; // mutex per mutua esclusione sugli accessi alle var. per stats.

static size_t max_size_reached = 0;    // dimensione massima dello storage raggiunta
static size_t max_size_reachedMB = 0;    // dimensione massima dello storage raggiunta in MB
static size_t max_no_reached = 0;      // numero massimo di files nello storage reggiunto
static size_t replace_no = 0;   // numero di rimpiazzamenti per file terminati con successo
static size_t replace_alg_no = 0;   // numero di volte in cui l'algoritmo di rimpiazzamento è stato attivato
static size_t read_no = 0;  // numero di operazioni read terminate con successo
static size_t total_read_size = 0;  // dimensione totale delle letture terminate con successo
static size_t media_read_size = 0;  // dimensione media delle letture terminate con successo
static size_t write_no = 0; // numero di operazioni write terminate con successo
static size_t total_write_size = 0; // dimensione totale delle scritture terminate con successo
static size_t media_write_size = 0; // dimensione media delle scritture terminate con successo
static size_t lock_no = 0; // numero di operazioni lock terminate con successo
static size_t unlock_no = 0; // numero di operazioni unlock terminate con successo
static size_t openlock_no = 0; // numero di operazioni open con flag O_LOCK == 1 terminate con successo
static size_t close_no = 0; // numero di operazioni close avvenute con successo
static size_t max_connections_no = 0;   // numero massimo di connessioni contemporanee raggiunto
static size_t currconnections_no = 0;   // numero di connessioni correnti

//    FUNZIONI PER MUTUA ESCLUSIONE -> Versione vista a lezione   //
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

//    FUNZIONI PER NODI DI c_info   //
/**
 *   @brief Funzione che inizializza un c_node
 *
 *   @param c_info  descrittore della connessione con un client
 *
 *   @return puntatore al c_node inizializato, NULL in caso di fallimento
 */
static c_node* c_node_init (size_t c_info)
{
    if (c_info == 0)
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
    tmp->c_info = c_info;

    return tmp;

}

//    FUNZIONI PER LISTE DI NODI DI c_info   // (USATE anche per comunicazione Main Thread/WorkerThread)
/**
 *   @brief Funzione che inizializza una lista di c_node
 *
 *   @param //
 *
 *   @return puntatore alla c_list inizializata, NULL in caso di fallimento
 */
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
/**
 *   @brief Funzione che libera la memoria allocata per una c_list
 *
 *   @param lst  puntatore alla c_list di cui fare la free
 *
 *   @return //
 */
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
/**
 *   @brief Funzione che verifica la presenza di un c_info in una c_list
 *
 *   @param lst  puntatore alla c_list
 *   @param c_info  descrittore della connessione con un client
 *
 *   @return 0 -> esito negativo, 1 -> esito positivo, -1 -> fallimento
 */
static int c_list_cont_node (c_list* lst, size_t c_info)
{
    if (lst == NULL || c_info == 0)
    {
        errno = EINVAL;
        return -1;
    }

    c_node* cursor = lst->head;
    while (cursor != NULL)
    {
        if (cursor->c_info == c_info) return 1;
        cursor = cursor->next;
    }

    //errno = ENOENT;
    return 0;
}
/**
 *   @brief Funzione che aggiunge un nodo in testa ad una c_list
 *
 *   @param lst  puntatore alla c_list
 *   @param c_info  descrittore della connessione con un client
 *
 *   @return 0 -> 1 -> esito positivo, -1 -> fallimento
 */
static int c_list_add_head (c_list* lst, size_t c_info)
{
    if (lst == NULL || c_info == 0)
    {
        errno = EINVAL;
        return -1;
    }

    c_node* tmp = c_node_init(c_info);
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
/**
 *   @brief Funzione chiamata dai thread worker per ottenere il descrittore della connessione con il prossimo client
 *
 *   @param lst  puntatore alla c_list
 *
 *   @return int: descrittore -> successo, -2 -> fallimento
 */
static int c_list_pop_worker (c_list* lst)
{
    if (lst == NULL)
    {
        errno = EINVAL;
        return -2;
    }
    while (lst->head == NULL)
    {
        pthread_cond_wait(&wait_coda,&coda_mtx); // attesa del segnale inviato dal thread main
    }

    size_t out = lst->tail->c_info;

    if (lst->head == lst->tail)
    {
        free(lst->tail);
        lst->tail = NULL;
        lst->head = NULL;
        return (int)out;
    }
    else
    {
        c_node* tmp = lst->tail;
        lst->tail = lst->tail->prec;
        lst->tail->next = NULL;
        free(tmp);
        return (int)out;
    }

}
/**
 *   @brief Funzione che elimina un dato descrittore dalla c_list
 *
 *   @param lst  puntatore alla c_list
 *   @param c_info  descrittore della connessione con un client
 *
 *   @return 0 -> esito negativo, 1 -> esito positivo, -1 -> fallimento
 */
static int c_list_rem_node (c_list* lst, size_t c_info)
{
    if (lst == NULL || c_info == 0)
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
        if (c_info == lst->head->c_info) {
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
        if (cursor->c_info == c_info)
        {
            if (lst->tail->c_info == c_info) {
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
/**
 *   @brief Funzione che inizializza un file
 *
 *   @param path  path assoluto del file
 *   @param cnt contenuto del file
 *   @param lo descrittore del lock owner
 *
 *   @return puntatore al file inizializzato, NULL in caso di errore
 */
static file* file_init (char* path, char* cnt, size_t lo)
{
    if (path == NULL || cnt == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    size_t path_len = strlen(path);
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
    tmp->queue_pointer = NULL;

    return tmp;
}
/**
 *   @brief Funzione che libera la memoria allocata per un file
 *
 *   @param file1  puntatore al file
 *
 *   @return //
 */
static void file_free (file* file1)
{
    if (file1 == NULL)  return;
    c_list_free(file1->whoop);
    free(file1->path);
    free(file1->cnt);
    free(file1);
}
/**
 *   @brief Funzione che effettua la copia di un file
 *
 *   @param file1  puntatore al file
 *
 *   @return puntatore al file copia, NULL per fallimento
 */
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

//    FUNZIONI PER AMMINISTRARE LE POLITICHE DI RIMPIAZZAMENTO"   //
/**
 *   @brief Funzione che inizializza un nodo per la coda di gestione per la politica dei rimpiazzamenti
 *
 *   @param path  path assoluto univoco del file/nodo
 *
 *   @return puntatore al p_queue_node, NULL per fallimento
 */
static p_queue_node* p_queue_node_init (char* path)
{
    p_queue_node* tmp = malloc(sizeof(p_queue_node));
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
/**
 *   @brief Funzione che libera lo spazio allocato per un nodo di p_queue
 *
 *   @param node1  puntatore al p_queue_node
 *
 *   @return //
 */
static void p_queue_node_free (p_queue_node* node1)
{
    if (node1 == NULL) return;
    free(node1->path);
    free(node1);
}

/**
 *   @brief Funzione che inizializza una coda p_queue
 *
 *   @param //
 *
 *   @return puntatore alla coda, NULL in caso di fallimento
 */
static p_queue* p_queue_init ()
{
    p_queue* tmp = malloc(sizeof(p_queue));
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
/**
 *   @brief Funzione libera lo spazio allocato per una coda p_queue
 *
 *   @param puntatore alla coda p_queue
 *
 *   @return //
 */
static void p_queue_free (p_queue* lst)
{
    if (lst == NULL)
    {
        errno = EINVAL;
        return;
    }

    p_queue_node* tmp = lst->head;

    while (tmp != NULL)
    {
        lst->head = lst->head->next;
        p_queue_node_free(tmp);
        tmp = lst->head;
    }

    lst->tail = NULL;

    free(lst);
}
/**
 *   @brief Funzione che esegue la push di un nodo in una coda p_queue
 *
 *   @param lst puntatore alla coda p_queue
 *   @param file1 puntatore alla nodo
 *
 *   @return 1 -> successo, -1 -> fallimento
 */
static int p_queue_push (p_queue* lst, p_queue_node* file1)
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
/**
 *   @brief Funzione che rimuove un file da una coda p_queue
 *
 *   @param lst puntatore alla coda p_queue
 *   @param path    path univoco del nodo da rimuovere
 *
 *   @return 0 -> file non trovato, 1 -> successo, -1 -> fallimento
 */
static int p_queue_rem_file (p_queue* lst, char* path)
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
        //errno = ENOENT;
        Pthread_mutex_unlock(&queue_mtx);
        return 0;
    }

    p_queue_node * cursor = lst->head;
    // eliminazione di un nodo in testa
    if (!strncmp(lst->head->path, path,UNIX_MAX_STANDARD_FILENAME_LENGHT)) {
        if(lst->head->next != NULL)
        {
            lst->head->next->prec = NULL;
            lst->head = lst->head->next;

            p_queue_node_free(cursor);
            Pthread_mutex_unlock(&queue_mtx);
            return 1;
        }
        else
        {
            lst->head = NULL;
            lst->tail = NULL;

            p_queue_node_free(cursor);
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
                p_queue_node_free(cursor);
                Pthread_mutex_unlock(&queue_mtx);
                return 1;
            }
            else
            {
                cursor->prec->next = cursor->next;
                cursor->next->prec = cursor->prec;
                p_queue_node_free(cursor);
                Pthread_mutex_unlock(&queue_mtx);
                return 1;
            }
        }
        cursor = cursor->next;
    }

    //errno = ENOENT;
    Pthread_mutex_unlock(&queue_mtx);
    return 0;
}

/*
static void p_queue_print (p_queue* lst)
{
    Pthread_mutex_lock(&queue_mtx);

    if (lst == NULL || lst->head == NULL)
    {
        errno = EINVAL;
        Pthread_mutex_unlock(&queue_mtx);
        return;
    }

    printf("\nSTART QUEUE \n");
    p_queue_node* cursor = lst->head;
    while (cursor != NULL)
    {
        printf("%s ->\n",cursor->path);
        cursor = cursor->next;
    }
    printf("\nEND QUEUE \n");

    Pthread_mutex_unlock(&queue_mtx);
}
*/


//    FUNZIONI PER AMMINISTRARE POLITICHE NON FIFO    //

static int queue_lru(p_queue* q, p_queue_node* node)
{
    Pthread_mutex_lock(&queue_mtx);

    if (q == NULL || node == NULL || q->head == NULL)
    {
        errno = EINVAL;
        Pthread_mutex_unlock(&queue_mtx);
        return -1;
    }

    if (q->head == node )
    {
        Pthread_mutex_unlock(&queue_mtx);
        return 1;
    }

    if (q->tail == node)
    {
        q->tail->prec->next = NULL;
        q->tail = q->tail->prec;
    }
    else
    {
        node->next->prec = node->prec;
        node->prec->next = node->next;
    }

    q->head->prec = node;
    node->next = q->head;
    q->head = node;

    Pthread_mutex_unlock(&queue_mtx);
    return 1;
}


//    FUNZIONI PER AMMINISTRARE LISTE DI FILE    //
/**
 *   @brief Funzione che inizializza una lista di files
 *
 *   @param //
 *
 *   @return puntatore alla lista, NULL per fallimento
 */
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
/**
 *   @brief Funzione che libera lo spazio allocato per una lista di files
 *
 *   @param puntatore alla lista
 *
 *   @return //
 */
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
/**
 *   @brief Funzione che stampa una rappresentazione di una lista di files
 *
 *   @param puntatore alla lista
 *
 *   @return //
 */
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
}
/**
 *   @brief Funzione che recupera il puntatore ad un file da una lista di files
 *
 *   @param lst puntatore alla lista
 *   @param path    path assoluto del file da estrarre
 *
 *   @return puntatore al file, NULL in caso di fallimento
 */
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
/**
 *   @brief Funzione che determina se un file è presente in una lista di files
 *
 *   @param lst puntatore alla lista
 *   @param path    path assoluto del file da ricercare
 *
 *   @return 1 -> esito positivo, 0 -> esito negativo, -1 -> errore
 */
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
/**
 *   @brief Funzione che aggiunge un file ad una lista di files se questa non lo contiene
 *
 *   @param lst puntatore alla lista
 *   @param file1   puntatore al file
 *
 *   @return 1 -> esito positivo, 0 -> esito negativo, -1 -> errore
 */
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
/**
 *   @brief Funzione che rimuove un file da una lista di files
 *
 *   @param lst puntatore alla lista
 *   @param path    path assoluto del file da rimuovere
 *
 *   @return puntatore alla copia del file rimosso, NULL in caso di errore
 */
static file* f_list_rem_file (f_list* lst, char* path)
{
    if (lst == NULL || path == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    if (lst->head == NULL)
    {
        //errno = ENOENT;
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

    //errno = ENOENT;
    return NULL;
}

//    FUNZIONI PER AMMINISTRARE TABELLE HASH    //
/**
 *   @brief funzione che inizializza una tabella hash
 *
 *   @param lst_no  numero di liste componenti la tabella hash
 *
 *   @return puntatore alla tabella hash, NULL in caso di errore
*/
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
/**
 *   @brief funzione restituisce un valore utile alla determinazione dell'indice in cui un dato file sarà inserito nella tabella hash
 *
 *   @param str stringa in base alla quale sarà calcolato l'output
 *
 *   @return valore di cui si farà poi il %list_no per ottenere l'indice, -1 in caso di errore
*/
static long long hash_function (const char* str)
{
    if (str == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    const int p = 47;
    const int m = (int) 1e9 + 9;
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
/**
 *   @brief funzione che aggiuge un file ad una tabella hash
 *
 *   @param tbl puntatore alla tabella hash
 *   @param file1   puntatore al file
 *
 *   @return 1 -> successo, -1 -> fallimento
*/
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
        p_queue_node* newfile_placeholder = p_queue_node_init(file1->path);
        file1->queue_pointer = newfile_placeholder;

        if (p_queue_push(queue, newfile_placeholder))
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
/**
 *   @brief funzione che rimuove un file ad una tabella hash
 *
 *   @param tbl puntatore alla tabella hash
 *   @param file1   puntatore al file
 *
 *   @return puntatore alla copia del file rimosso dalla tabella hash -> successo, NULL -> fallimento
*/
static file* hash_rem_file (hash* tbl, file* file1)
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
        if (tmp != NULL && p_queue_rem_file(queue, tmp->path))
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
/**
 *   @brief funzione che ottiene il puntatore ad un file di una tabella hash
 *
 *   @param tbl puntatore alla tabella hash
 *   @param path    path assoluto del file
 *
 *   @return puntatore al file richiesto -> successo, NULL -> fallimento
*/
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
/**
 *   @brief funzione che ottiene il puntatore ad una lista contenente il file di una tabella hash
 *
 *   @param tbl puntatore alla tabella hash
 *   @param path    path assoluto del file
 *
 *   @return puntatore alla lista richiesta -> successo, NULL -> fallimento
*/
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
/**
 *   @brief funzione che libera lo spazio allocato per una tabella hash
 *
 *   @param tbl puntatore alla tabella hash
 *
 *   @return //
*/
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
/**
 *   @brief funzione che stampa un rappresentazione di una tabella hash
 *
 *   @param tbl puntatore alla tabella hash
 *
 *   @return //
*/
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
/**
 *   @brief funzione che individua la presenza di un file in una tabella hash
 *
 *   @param tbl puntatore alla tabella hash
 *   @param path    path assoluto del file
 *
 *   @return -1 -> errore, 0 -> file non trovato, 1 -> successo
*/
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
static int hash_lock_all_lists (hash* tbl)
{
    if (tbl == NULL || tbl->lists == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    int i;
    for (i = 0; i< tbl->lst_no; i++) Pthread_mutex_lock(&(tbl->lists[i]->mtx));
    return 0;
}
static int hash_unlock_all_lists (hash* tbl)
{
    if (tbl == NULL || tbl->lists == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    int i;
    for (i = 0; i< tbl->lst_no; i++) Pthread_mutex_unlock(&(tbl->lists[i]->mtx));
    return 0;
}

/**
 *   @brief funzione che controlla la presenza di un superamento dei limiti massimi di memoria e se presente applica il rimpiazzamento
 *
 *   @param tbl puntatore alla tabella hash
 *   @param path    path assoluto del file che potrebbe aver causato il superamento del limite massimo di memoria
 *   @param c_info  descrittore del client che ha richiesto l'operazione causante potenziale overflow
 *
 *   @return un puntatore ad una f_list contenente i files rimossi, NULL altrimenti
*/
static f_list* hash_replace (hash* tbl, const char* path, size_t c_info)
{
    if (tbl == NULL || c_info == 0 || path == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    //fifo_print(queue);

    f_list* replaced = f_list_init(); // inizializzazione della lista output
    if (replaced == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }

    int bool = 0; // flag per evidenziare l'avvio o meno dell'algoritmo per i rimpiazzamenti
    p_queue_node* p_kill_ff = NULL; // punatatore al nodo della coda p_queue che conterrà il path del file potenzialmente rimpiazzabile

    Pthread_mutex_lock(&stats_mtx);
    while (curr_size > max_size) // fin quando i valori non sono rientrati entro i limiti
    {
        Pthread_mutex_unlock(&stats_mtx);
        bool = 1; // la flag viene impostata  per indicare che l'algoritmo di rimpiazzamento è di fatto partito
        if (p_kill_ff == NULL) p_kill_ff = queue->tail; // la coda p_queue verrà effettivamente percorsa dal primo elemento inserito verso l'ultimo
        if (p_kill_ff == NULL)
        {
            f_list_free(replaced);
            return NULL;
        }

        file* p_kill_file = hash_get_file(storage,p_kill_ff->path); // il punatatore al file individuato con politica FIFO viene ottenuto
        if (p_kill_file == NULL)
        {
            f_list_free(replaced);
            return NULL;
        }

        while ((p_kill_file->lock_owner != 0 && p_kill_file->lock_owner != c_info) || (p_kill_ff->path == path))
        { // fin quando il file rimovibile individuato non rispetta le caratteristiche per poter essere rimosso esso viene ignorato
            // il file non deve essere lockato da altri client e inoltre non deve essere quello causa del superamento dei limiti
            p_kill_ff = p_kill_ff->prec;
            if (p_kill_ff == NULL)
            {
                errno = EFBIG;
                f_list_free(replaced);
                return NULL;
            }

            p_kill_file = hash_get_file(storage,p_kill_ff->path);
            if (p_kill_file == NULL)
            {
                f_list_free(replaced);
                return NULL;
            }
        }

        file* copy = file_copy(p_kill_file); // il file rimovibile può essere rimosso, ma prima viene copiato
        if (copy == NULL)
        {
            f_list_free(replaced);
            return NULL;
        }
        p_kill_ff = p_kill_ff->prec; // il puntatore verso l'ultimo elemento analizzato nella coda p_queue viene aggiornato

        if (hash_rem_file(storage, p_kill_file) == NULL) // il file rimovibile viene rimosso
        {
            f_list_free(replaced);
            return NULL;
        }

        if (f_list_add_head(replaced,copy) == -1) // la copia del file rimosso viene inserita nella lista di output
        {
            f_list_free(replaced);
            return NULL;
        }

        Pthread_mutex_lock(&stats_mtx);
        replace_no++;   // le statistiche vengono aggiornate
    }

    if (bool == 1) replace_alg_no++;// le statistiche vengono aggiornate
    if (curr_size > max_size_reached) max_size_reached = curr_size;// le statistiche vengono aggiornate
    Pthread_mutex_unlock(&stats_mtx);

    Pthread_mutex_lock(&lock_mtx);
    pthread_cond_broadcast(&wait_server_lock);
    Pthread_mutex_unlock(&lock_mtx);

    return replaced;
}
/**
 *   @brief funzione che rimuove tutte le lock in una tabella hash dopo la disconnessione del client
 *
 *   @param tbl puntatore alla tabella hash
 *   @param fd_c    descrittore della connessione
 *
 *   @return //
*/
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

    Pthread_mutex_lock(&lock_mtx);
    pthread_cond_broadcast(&wait_server_lock);
    Pthread_mutex_unlock(&lock_mtx);

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
/**
 *   @brief funzione per la gestione dei segnali
 *
 *   @param sgnl segnale ricevuto
 *
 *   @return //
*/
static void t_gstr (int sgnl)
{
    if (sgnl == SIGINT || sgnl == SIGQUIT) t = 1; //SIGINT,SIGQUIT -> TERMINA SUBITO (GENERA STATISTICHE)
    else if (sgnl == SIGHUP) t = 2; //SIGHUP -> NON ACCETTA NUOVI CLIENT, ASPETTA CHE I CLIENT COLLEGATI CHIUDANO CONNESSIONE
}
/**
 *   @brief funzione per l'aggiornamento del file descriptor massimo
 *
 *   @param fdmax descrittore massimo attuale
 *
 *   @return file descriptor massimo -> successo, -1 altrimenti
*/
static int max_up (fd_set set, int fdmax)
{
    int i;
    for(i=(fdmax-1); i>=0; --i)
        if (FD_ISSET(i, &set)) return i;
    assert(1==0);
    return -1;
}

/**
 *   @brief Funzione che permette di effettuare la read completandola in seguito alla ricezione di un segnale
 *
 *   @param fd     descrittore della connessione
 *   @param buf    puntatore al messaggio da inviare
 *
 *   @return Il numero di bytes letti, -1 se c'e' stato un errore
 */
int readn(long fd, void *buf, size_t size) {
    int readn = 0;
    int r = 0;

    while ( readn < size ){

        if ( (r = (int)read((int)fd, buf, size)) == -1 ){
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
 *   @brief Funzione che permette di effettuare la write completandola in seguito alla ricezione di un segnale
 *
 *   @param fd     descrittore della connessione
 *   @param buf    puntatore al messaggio da inviare
 *
 *   @return Il numero di bytes scritti, -1 se c'è stato un errore
 */
int writen(long fd, const void *buf, size_t nbyte){
    int writen = 0;
    int w = 0;

    while ( writen < nbyte ){
        if ( (w = (int)write((int)fd, buf, nbyte) ) == -1 ){
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


// Le seguenti funzioni sono di fatto speculari a quanto implementato dalla api //
/* open_FILE : FLAGS
 * 0 -> 00 -> O_CREATE = 0 && O_LOCK = 0
 * 1 -> 01 -> O_CREATE = 0 && O_LOCK = 1
 * 2 -> 10 -> O_CREATE = 1 && O_LOCK = 0
 * 3 -> 11 -> O_CREATE = 1 && O_LOCK = 1
 */
static int open_File (char* path, int flags, size_t c_info)
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
        case 0 : {
            if (!ex) {
                errno = ENOENT;
                return -1;
            }

            f_list *tmp_lst = hash_get_list(storage, path);
            if (tmp_lst == NULL) return -1;

            Pthread_mutex_lock(&(tmp_lst->mtx));

            file *tmp = hash_get_file(storage, path);
            if (tmp == NULL) return -1;


            if (tmp->lock_owner == 0 || tmp->lock_owner == c_info)
            {
                if (c_list_rem_node(tmp->whoop, c_info) == -1)
                {
                    Pthread_mutex_unlock(&(tmp_lst->mtx));
                    return -1;
                }
                tmp->op = 1;
                Pthread_mutex_unlock(&(tmp_lst->mtx));

                if (politic == 1) queue_lru(queue, tmp->queue_pointer);
                return 0;
            }
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            errno = EPERM;
            return -1;
        }

        case 1 :
        {
            if (!ex)
            {
                errno = ENOENT;
                return -1;
            }

            f_list* tmp_lst = hash_get_list(storage,path);
            if(tmp_lst == NULL) return -1;

            Pthread_mutex_lock(&(tmp_lst->mtx));

            file* tmp = hash_get_file(storage,path);
            if(tmp == NULL) return -1;


            Pthread_mutex_lock(&stats_mtx);
            openlock_no++;
            lock_no++;
            Pthread_mutex_unlock(&stats_mtx);

            if( tmp->lock_owner == 0 || tmp->lock_owner == c_info)
            {
                tmp->lock_owner = c_info;
                if (c_list_add_head(tmp->whoop, c_info) == -1) {
                    Pthread_mutex_unlock(&(tmp_lst->mtx));
                    return -1;
                }
                tmp->op = 1;
                Pthread_mutex_unlock(&(tmp_lst->mtx));

                if (politic == 1) queue_lru(queue, tmp->queue_pointer);
                return 0;
            }
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            errno = EPERM;
            return -1;

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
                if(c_list_add_head(tmp->whoop,c_info) == -1) return -1;

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
                file *tmp = file_init(path, "", c_info);
                if (tmp == NULL) return -1;

                f_list *tmp_lst = hash_get_list(storage,path);
                if (tmp_lst == NULL) return -1;

                Pthread_mutex_lock(&(tmp_lst->mtx));

                tmp->op = 1;
                if(c_list_add_head(tmp->whoop,c_info) == -1)
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
static int read_File (char* path, char* buf, size_t* size, size_t c_info)
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
        if (tmp->lock_owner == 0 || tmp->lock_owner == c_info)
        {
            *size = strlen((char*)tmp->cnt);

            strcpy(buf, tmp->cnt);

            Pthread_mutex_lock(&stats_mtx);
            read_no++;
            total_read_size = total_read_size + (*size);
            Pthread_mutex_unlock(&stats_mtx);

            if(c_list_rem_node(tmp->whoop,c_info) == -1)
            {
                Pthread_mutex_unlock(&tmp_lst->mtx);
                return -1;
            }

            Pthread_mutex_unlock(&tmp_lst->mtx);
            if (politic == 1) queue_lru(queue, tmp->queue_pointer);
            return 0;
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
static f_list* read_N_File (int N, int* count, size_t c_info)
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
                if(cursor->lock_owner == 0 || cursor->lock_owner == c_info)
                {
                    file* copy = file_copy(cursor);
                    if(copy == NULL) return NULL;
                    total = total + strlen(copy->cnt);
                    if(f_list_add_head(out, copy) == -1) return NULL;
                    if(c_list_rem_node(cursor->whoop,c_info) == -1) return NULL;
                    r_num++;
                    if (politic == 1) queue_lru(queue, cursor->queue_pointer);
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
                if (cursor->lock_owner == 0 || cursor->lock_owner == c_info)
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
                    if(c_list_rem_node(cursor->whoop,c_info) == -1)
                    {
                        Pthread_mutex_lock(&storage->lists[i]->mtx);
                        return NULL;
                    }
                    pkd++;
                    r_num++;
                    if (politic == 1) queue_lru(queue, cursor->queue_pointer);
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
static f_list* write_File (char* path, char* cnt, size_t c_info)
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
    if (tmp->lock_owner == 0 || tmp->lock_owner != c_info)
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }

    int prec_op = c_list_cont_node(tmp->whoop, c_info);
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
    Pthread_mutex_unlock(&(tmp_lst->mtx));

    if (hash_lock_all_lists(storage) != 0) return NULL;
    f_list* out = hash_replace(storage,path,c_info);
    if (hash_unlock_all_lists(storage) != 0) return NULL;

    if (politic == 1) queue_lru(queue, tmp->queue_pointer);
    return out;

}
static f_list* append_to_File (char* path, char* cnt, size_t c_info)
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
    if (tmp->lock_owner == 0 || tmp->lock_owner != c_info)
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return NULL;
    }

    int prec_op = c_list_cont_node(tmp->whoop, c_info);
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

    Pthread_mutex_unlock(&(tmp_lst->mtx));
    f_list* out = hash_replace(storage,path,c_info);

    if (politic == 1) queue_lru(queue, tmp->queue_pointer);
    return out;
}
static int lock_File (char* path, size_t c_info)
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
    if(tmp->lock_owner == 0 || tmp->lock_owner == c_info)
    {
        tmp->lock_owner = c_info;

        Pthread_mutex_lock(&stats_mtx);
        lock_no++;
        Pthread_mutex_unlock(&stats_mtx);

        if (c_list_rem_node(tmp->whoop, c_info) == -1) {
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            return -1;
        }
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        if (politic == 1) queue_lru(queue, tmp->queue_pointer);
        return 0;
    }
    Pthread_mutex_unlock(&(tmp_lst->mtx));
    return -2; // valore speciale, quando sarà ricevuto dalla do_a_job la lock verrà ritentata
}
static int unlock_File (char* path, size_t c_info)
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

    if (tmp->lock_owner == c_info)
    {
        Pthread_mutex_lock(&(tmp_lst->mtx));
        tmp->lock_owner = 0;

        Pthread_mutex_lock(&stats_mtx);
        unlock_no++;
        Pthread_mutex_unlock(&stats_mtx);

        if(c_list_rem_node(tmp->whoop,c_info) == -1)
        {
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            return -1;
        }
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        if (politic == 1) queue_lru(queue, tmp->queue_pointer);

        Pthread_mutex_lock(&lock_mtx);
        pthread_cond_broadcast(&wait_server_lock);
        Pthread_mutex_unlock(&lock_mtx);

        return 0;
    }
    else
    {
        errno = EPERM;
        return -1;
    }

}
static int close_File (char* path, size_t c_info)
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

    if (tmp->op && (tmp->lock_owner == c_info || tmp->lock_owner == 0))
    {
        tmp->op = 0;

        Pthread_mutex_lock(&stats_mtx);
        close_no++;
        Pthread_mutex_unlock(&stats_mtx);

        if(c_list_rem_node(tmp->whoop,c_info) == -1)
        {
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            return -1;
        }
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        if (politic == 1) queue_lru(queue, tmp->queue_pointer);
        return 0;
    }
    else
    {
        errno = EPERM;
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        return -1;
    }

}
static int remove_File (char* path, size_t c_info)
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

    if (tmp->lock_owner == c_info)
    {
        file* dummy = hash_rem_file(storage,tmp);
        if (dummy == NULL)
        {
            Pthread_mutex_unlock(&(tmp_lst->mtx));
            return -1;
        }
        Pthread_mutex_unlock(&(tmp_lst->mtx));
        file_free(dummy);

        Pthread_mutex_lock(&lock_mtx);
        pthread_cond_broadcast(&wait_server_lock);
        Pthread_mutex_unlock(&lock_mtx);

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
/**
 *   @brief Funzione che interpreta ed esegue le operazioni richieste dai client
 *
 *   @param fd_c    descrittore della connessione
 *   @param fd_pipe descrittore della pipe
 *   @param quest   richiesta
 *   @param end     puntatore alla flag indicante l'avvenuta chiusura di una connessione
 *
 *   @return Il numero di bytes scritti, -1 se c'è stato un errore
 */
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
        int flags = (int) strtol(token,NULL,10);

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

        // esecuzione della richiesta
        int res = -2;
        int log_res;


        Pthread_mutex_lock(&lock_mtx);
        while (res == -2) // il thread attende che la lock sia ottenuta o che si verifichi un errore letale
        {
            res = lock_File(path,fd_c);
            if (res == -2) pthread_cond_wait(&wait_server_lock,&lock_mtx); // attesa del segnale inviato dal thread main

        }
        Pthread_mutex_unlock(&lock_mtx);

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

        // esecuzione della richiesta
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

        char* buf = malloc(sizeof(char)*MAX_CNT_LEN);
        if (buf == NULL)
        {
            errno = ENOMEM;
            free(buf);
            return;
        }

        size_t size;

        // esecuzione della richiesta
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

    while (1)
    {
        int end = 0; //valore indicante la terminazione del client
        //un client viene espulso dalla coda secondo la politica p_queue
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
            char string[200];
            FILE *f_point;
            f_point = fopen(path_config, "r"); // apertura del file di config
            if (f_point == NULL)
            {
                perror("Errore nell'apertura del file di configurazione");
                exit(EXIT_FAILURE);
            }

            char campo[100]; // campo da configurare
            char valore[100]; // valore del campo
            while (fgets(string, 200, f_point) != NULL)
            {
                if (string[0] != '\n')
                {
                    int nf;
                    nf = sscanf(string, "%[^=]=%s", campo, valore); // scansione dei due campi attesi ad ogni riga valida (campo -> scansione sino a '=' / valore -> scansione dopo '=')

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
                                    else
                                    if (strcmp(campo, "replace_politic") == 0)
                                    {
                                        if (strcmp(valore,"FIFO") == 0)
                                        {
                                            politic = 0;
                                        }
                                        else
                                        if (strcmp(valore,"LRU") == 0)
                                        {
                                            politic = 1;
                                        }
                                        else
                                        {
                                            printf("Errore di Configurazione : politiche ammesse: FIFO, LRU\n");
                                            printf("Il Server è stato avviato con parametri DEFAULT\n");
                                            break;
                                        }
                                    }
                }
            }
            fclose(f_point);
        }
        else printf("Server avviato con parametri di DEFAULT\n");
        printf("Server INFO: socket_name:%s / num_thread:%lu / max_files:%lu / max_size:%lu / politic: %lu\n", socket_name, thread_no,
               max_no,
               max_size,politic);
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

        queue = p_queue_init();
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
                        if ((l = (int)read(pip[0],&fd_c1,sizeof(fd_c1))) > 0)
                        { //lettura del fd di un client
                            o = (int)read(pip[0],&flag,sizeof(flag));
                            if (o == -1)
                            {
                                perror("errore nel dialogo Master/Worker");
                                exit(EXIT_FAILURE);
                            }
                            if (flag == 1)
                            {//il client è terminato, il suo fd deve essere rimosso dal set
                                //printf("Chiusura della connessione\n");
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
        media_read_size = total_read_size/fake_read_no;
        media_write_size =  total_write_size/fake_write_no;
        max_size_reachedMB =  max_size_reached/(1024*1024);

    } // elaborazioni per il file delle statistiche

    {
        fprintf(log_file,"END\n");
        fprintf(log_file,"SUNTO DELLE STATISTICHE:\n");
        fprintf(log_file,"-Numero di read: %lu;\n-Size media delle letture in bytes: %lu;\n-Numero di write: %lu;\n-Size media delle scritture in bytes: %lu;\n-Numero di lock: %lu;\n-Numero di openlock: %lu;\n-Numero di unlock: %lu;\n-Numero di close: %lu;\n-Dimensione massima dello storage in MB: %lu;\n-Dimensione massima dello storage in numero di files: %lu;\n-Numero di replace per selezionare un file vittima: %lu;\n-Massimo numero di connessioni contemporanee: %lu;\n",read_no,media_read_size,write_no,media_write_size,lock_no,openlock_no,unlock_no,close_no,max_size_reachedMB,max_no_reached,replace_no,max_connections_no);
        // il numero di richieste soddisfatte da ogni thread è lasciato al parsing in statistiche.sh
    } // chiusura del file di log

    {
        printf("SERVER INFO:\n");
        printf("Numero Massimo di files raggiunto: %lu\n", max_no_reached);
        printf("Dimensione Massima raggiunta dallo storage in MByte: %lu\n", max_size_reachedMB);
        printf("Dimensione Massima raggiunta dallo storage in Byte: %lu\n", max_size_reached);
        printf("Dimensione media delle read effettuate: %lu\n", media_read_size);
        printf("Dimensione media delle write effettuate: %lu\n", media_write_size);
        printf("Numero di volte in cui è stato avviato l'algoritmo di rimpiazzamento: %lu\n", replace_alg_no);
        printf("Numero di files rimossi tramite rimpiazzamento: %lu\n\n",replace_no);
        printf("Di seguito è riportato lo stato dello storage al momento della chiusura:\n");
        hash_print(storage);
    } // sunto sull'esecuzione

    {
        hash_free(storage);
        p_queue_free(queue);
        c_list_free(coda);
        fclose(log_file);

    } // ultime free

    return 0;
}

// UPDATE: 14/10