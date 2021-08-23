//
// Created by xubuntu on 21/08/21.
//
#include "api.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <libgen.h>


#define MSG_SIZE 1024
#define MAX_CNT_LEN 1000
#define UNIX_MAX_STANDARD_FILENAME_LENGHT 108 /* man 7 unix */

int flag_stampa = 0;
static int num_files = 0;
int tms;

// il nodo della lista dei comandi:
typedef struct node
{
    char * cmd; // stringa in cui sarà identificato il comando richiesto
    char * arg; // stringa nella quale saranno inseriti i vari argomenti utili al comando
    struct node* next; // puntatore al prossimo comando della lista
    struct node* prec; // puntatore al comando precedente nella lista

} node;

int msSleep(long time)
{
    if(time < 0) errno = EINVAL;

    int res;
    struct timespec t;
    t.tv_sec = time/1000;
    t.tv_nsec = (time % 1000) * 1000000;

    do
    {
        res = nanosleep(&t, &t);
    }while(res && errno == EINTR);

    return res;
}
long isNumber(const char* s)
{
    char* e = NULL;
    long val = strtol(s, &e, 0);
    if (e != NULL && *e == (char)0) return val;
    return -1;
}
/*
 * dirname -> nome della cartella src
 * dest_dirname -> nome della cartella destinazione per gli overflow
 */
void w_exec (char* dirname, int n, char* dest_dirname)
{
    DIR * dir;
    struct dirent* entry;

    if ((dir = opendir(dirname)) == NULL || num_files == n)
    {
        return;
    }

    while ((entry = readdir(dir)) != NULL && (num_files < n))
    {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);

        struct stat info;
        if (stat(path,&info)==-1) {
            perror("ERRORE: stat");
            exit(EXIT_FAILURE);
        }

        //SE FILE E' UNA DIRECTORY
        if (S_ISDIR(info.st_mode))
        {
            if (strcmp(entry->d_name,".") == 0 || strcmp(entry->d_name,"..") == 0) continue;// . -> stessa cartella / .. -> cartella prec
            w_exec(path,n,dest_dirname);
        }
        else
        {
            char * resolvedPath = NULL;
            if ((resolvedPath = realpath(path,resolvedPath))==NULL)
            {
                perror("ERRORE: realpath");
            }
            else
            {
                errno = 0;

                if (openFile(resolvedPath,0) == -1)
                {
                    if (errno == ENOENT)
                    {
                        if (openFile(resolvedPath,2) == -1)
                        {
                            if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",resolvedPath);
                            perror("ERRORE: apertura del file fallita");
                        }
                        else
                        {// scrittura nel file appena creato all'interno del server
                            num_files++;
                            //scrittura nel file
                            if (writeFile(resolvedPath,dest_dirname) == -1)
                            {
                                if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",resolvedPath);
                                perror("ERRORE: scrittura nel  file");
                            }
                            if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Dimensione totale della scrittura: %lu Esito : successo\n",resolvedPath,get_last_w_size());
                            //chiusura del file
                            if (closeFile(resolvedPath)==-1)
                            {
                                if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",resolvedPath);
                                perror("ERRORE: chiusura del file");
                            }
                        }
                    }
                    else
                    {
                        if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",resolvedPath);
                        perror("ERRORE: apertura del file fallita");
                    }
                }
                else
                {// scrittura nel file aperto all'interno del server e già esistente in esso
                    num_files++;
                    //scrittura nel file
                    if (writeFile(resolvedPath,dest_dirname) == -1)
                    {
                        if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",resolvedPath);
                        perror("ERRORE: scrittura nel  file");
                    }
                    if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Dimensione totale della scrittura: %lu Esito : successo\n",resolvedPath,get_last_w_size());
                    //chiusura del file
                    if (closeFile(resolvedPath)==-1)
                    {
                        if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",resolvedPath);
                        perror("ERRORE: chiusura del file");
                    }
                }
                if (resolvedPath!=NULL) free(resolvedPath);
            }
        }
    }
    if ((closedir(dir))==-1)
    {
        perror("ERRORE: closedir");
        exit(EXIT_FAILURE);
    }
} //scorre i files presenti in un dato percorso fino a scriverne n nel server
void addLast (node** lst, char* cmd, char* arg)
{
    node * new = malloc (sizeof(node)); // spazio per memorizzare il nodo comando
    if (new==NULL)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    new->cmd = malloc(sizeof(cmd)); // spazio per memorizzare la stringa comando
    if (new->cmd==NULL)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    strcpy(new->cmd,cmd);

    if (arg!=NULL)
    {
        new->arg = malloc(PATH_MAX*sizeof(char)); // spazio per memorizzare la stringa di argomenti
        if (new->arg==NULL)
        {
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        strcpy(new->arg,arg);
    }
    else new->arg = NULL;
    new->next = NULL;

    node * last = *lst;

    if (*lst == NULL)
    {
        *lst = new;
        return;
    }

    while (last->next!=NULL)
    {
        last = last->next;
    }

    last->next = new;
    new->prec = last;
} // aggiunge un nodo di comando in coda alla lista di comandi
/*
 * return 1 <-> la lista parametro contiene il comando cercato
 * return 0 <-> la lista parametro non contiene il comando cercato
 *
 * NOTA: se il comando è stato trovato e se gli argomenti di questo sono presenti, essi vengono restituiti in "arg"
*/
 int containCMD (node** lst, char* cmd, char** arg)
{
    node * curr = *lst;
    int found = 0;

    // ricerca del comando cmd
    while (curr != NULL && found == 0)
    {
        if (strcmp(curr->cmd,cmd) == 0) found = 1;
        else
        {
            curr = curr->next;
        }
    }

    // il comando è stato trovato -> rimozione
    if (found == 1)
    {
        if (curr->arg != NULL)
        {
            *arg = malloc(sizeof(curr->arg));
            strcpy(*arg,curr->arg);
        }
        else *arg = NULL;

        if (curr->prec == NULL)
        {
            curr->next->prec = NULL;
            *lst = (*lst)->next;
            free(curr->arg);
            free(curr->cmd);
            free(curr);
        }
        else
        {
            curr->prec->next = curr->next;
            curr->next->prec = curr->prec;
            free(curr->arg);
            free(curr->cmd);
            free(curr);
        }
    }
    return found;
}
void freeList (node ** lst)
{
    node * tmp;
    while (*lst != NULL)
    {
        tmp = *lst;
        free((*lst)->arg);
        free((*lst)->cmd);
        (*lst)=(*lst)->next;
        free(tmp);
    }
} // funzione per liberare la memoria allocata per la lista di comandi
void printList (node ** lst)
{
    node * cursor = *lst;
    while (cursor != NULL)
    {
        printf("CMD=%s ARG=%s \n",cursor->cmd,cursor->arg);
        cursor = cursor->next;
    }

} // funzione per l'eventuale stampa della lista di comandi (debugging)

int main (int argc, char * argv[])
{
    char opt; // carattere per memorizzare l'operatore di un dato comando

    // stringhe per ospitare gli argomenti dei vari comandi
    char *farg, *warg, *Warg, *rarg, *Rarg, *darg, *Darg, *larg, *uarg, *targ, *carg;

    // flags per determinare se il comando non ripetibile è stato richiesto
    int h_f = 0;
    int f_f = 0;
    int p_f = 0;

    char* dir = NULL; // cartella impostata per le read
    char* Dir = NULL; // cartella impostata per gli overflow in write
    tms = 0; // numero di secondi da attendere tra un comando ed un altro

    node* listCmd = NULL; // lista dei comandi richiesti
    char* resolvedPath = NULL; // stringa per il path assoluto

    // la lista dei comandi viene popolata secondo gli argomenti di avvio
    while ((opt = getopt(argc,argv,"hpf:w:W:r:R:d:t:c:")) != -1)
    {
        switch (opt)
        {
            // i seguenti 3 comandi possono essere richiesti al più una volta, pertanto solo il primo verrà considerato valido
            case 'h':
            {
                if (h_f == 0)
                {
                    h_f = 1;
                    addLast(&listCmd, "h", NULL);
                }
                else
                {
                    printf("IMPRECISIONE DEL COMANDO: L'opzione -h può essere richiesta al più una volta\n");
                }
                break;
            }
            case 'p':
            {
                if (p_f == 0)
                {
                    p_f = 1;
                    addLast(&listCmd, "p", NULL);
                }
                else
                {
                    printf("IMPRECISIONE DEL COMANDO: L'opzione -p può essere richiesta al più una volta\n");
                }
                break;
            }
            case 'f':
            {
                if (f_f == 0)
                {
                    f_f = 1;
                    farg = optarg;
                    addLast(&listCmd, "f", farg);
                }
                else
                {
                    printf("IMPRECISIONE DEL COMANDO: L'opzione -f può essere richiesta al più una volta\n");
                }
                break;
            }

            // i seguenti comandi possono essere ripetuti più volte
            case 'w':
            {
                warg = optarg;
                addLast(&listCmd, "w", warg);
                break;
            }
            case 'W':
            {
                Warg = optarg;
                addLast(&listCmd,"W",Warg);
                break;
            }
            case 'D':
            {
                Darg = optarg;
                addLast(&listCmd, "D", Darg);
                break;
            }
            case 'r':
            {
                rarg = optarg;
                addLast(&listCmd, "r", rarg);
                break;
            }
            case 'R':
            {
                Rarg = optarg;
                addLast(&listCmd, "R", Rarg);
                break;
            }
            case 'd':
            {
                darg = optarg;
                addLast(&listCmd, "d", darg);
                break;
            }
            case 't':
            {
                targ = optarg;
                addLast(&listCmd, "t", targ);
                break;
            }
            case 'l':
            {
                larg = optarg;
                addLast(&listCmd, "l", larg);
                break;
            }
            case 'u':
            {
                uarg = optarg;
                addLast(&listCmd, "u", uarg);
                break;
            }
            case 'c':
            {
                carg = optarg;
                addLast(&listCmd, "c", carg);
                break;
            }
            case '?':
            {// comando non riconosciuto
                fprintf(stderr, "digitare : %s -h per ottenere la lista dei comandi\n", argv[0]);
                break;
            }
            case ':':
            {// argomenti non adeguati
                printf("l'opzione '-%c' richiede almeno un argomento", optopt);
                break;
            }
            default:;
        }
    }

    // controlli per la presenza delle opzioni non ripetibili: -h -p -f
    char* arg=NULL;
    if (containCMD(&listCmd,"h",&arg) == 1)
    {
        printf("OPERAZIONI SUPPORTATE: \n");
        printf("-h\n-f filename\n-w dirname[,n=0]\n-W file1[,file2]\n");
        printf("-D dirname\n-r file1[,file2]\n-R [n=0]\n-d dirname\n-t time\n");
        printf("-l file1[,file2]\n-u file1[,file2]\n-c file1[,file2]\n-p\n");
        freeList(&listCmd);
        if (resolvedPath!=NULL) free(resolvedPath);
        return 0;// il processo termina immediatamente dopo la stampa
    }
    if (containCMD(&listCmd,"p",&arg) == 1)
    {
        flag_stampa = 1;
        printf("OP : -p (abilta stampe) Esito : successo\n");
    }
    if (containCMD(&listCmd,"f",&arg) == 1)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME,&ts);
        ts.tv_sec = ts.tv_sec+60;
        // tentativo di connessione con tempo tra i tentativi di 1 secondi
        if (openConnection(farg,1000,ts)==-1)
        {
            if (flag_stampa==1) printf("OP : -f (connessione) File : %s Esito : fallimento\n",farg);
            perror("ERRORE: apertura della connessione");
        }
        else
        {
            if (flag_stampa==1) printf("OP : -f (connessione) File : %s Esito : successo\n",farg);
        }
    }

    // le operazioni restanti verranno ora eseguite: -w -W -D -r -R -d -t -l -u -c
    node * curr = listCmd;
    while (curr!=NULL)
    {
        msSleep(tms); // attesa tra due operazioni consecutive
        if (strcmp(curr->cmd,"w") == 0)
        {
            Dir = NULL;
            if (curr->next != NULL)
            {
                if(strcmp(curr->next->cmd,"D") == 0) Dir = curr->next->arg;
            }

            char* save1 = NULL;
            char* token1 = strtok_r(curr->arg,",",&save1);
            char* namedir = token1;
            int n;

            struct stat info_dir;
            if (stat(namedir,&info_dir)==-1)
            {
                if (flag_stampa==1) printf("OP : -w (scrivi directory) Directory : %s Esito : fallimento\n",namedir);
                printf("%s non e' una directory valida\n",namedir);
            }
            else
            {
                if (S_ISDIR(info_dir.st_mode))
                {
                    token1 = strtok_r(NULL,",",&save1);
                    if (token1!=NULL)
                    {
                        n = isNumber(token1);
                    }
                    else n=0;

                    if (n>0)
                    {
                        w_exec(namedir,n,Dir);
                        if (flag_stampa==1) printf("Operazione : -w (scrivi directory) Directory : %s Esito : positivo\n",namedir);
                    }
                    else
                        if (n==0)
                        {
                            w_exec(namedir,INT_MAX,Dir);
                            if (flag_stampa==1) printf("Operazione : -w (scrivi directory) Directory : %s Esito : positivo\n",namedir);
                        }
                        else
                        {
                            if (flag_stampa==1) printf("Operazione : -w (scrivi directory) Directory : %s Esito : negativo\n",namedir);
                            printf("Utilizzo : -w dirname[,n]\n");
                        }
                }
                else
                {
                        if (flag_stampa==1) printf("OP : -w (scrivi directory) Directory : %s Esito : fallimento\n",namedir);
                    printf("%s non e' una directory valida\n",namedir);
                }
            }

        }
        else if (strcmp(curr->cmd,"W") == 0)
            {
                Dir = NULL;
                if (curr->next != NULL)
                {
                    if(strcmp(curr->next->cmd,"D") == 0) Dir = curr->next->arg;
                }

                char* save2 = NULL;
                char* token2 = strtok_r(curr->arg,",",&save2);

                while(token2)
                {
                    char* file = token2;
                    // per ogni file passato come argomento sarà eseguita la serie "open-write-close"

                    if ((resolvedPath = realpath(file,resolvedPath)) == NULL)
                    {
                        if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",file);
                        printf("ERRORE: Il file %s non esiste\n",file);
                    }
                    else
                    {
                        struct stat info_file;
                        stat(resolvedPath,&info_file);

                        if (S_ISREG(info_file.st_mode))
                        {
                            errno = 0;

                            if (openFile(resolvedPath,0) == -1)
                            {
                                if (errno == ENOENT)
                                {
                                    if (openFile(resolvedPath,2) == -1)
                                    {
                                        if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",file);
                                        perror("ERRORE: apertura del file fallita");
                                    }
                                    else
                                    {// scrittura nel file appena creato all'interno del server
                                        num_files++;
                                        //scrittura nel file
                                        if (writeFile(resolvedPath,Dir) == -1)
                                        {
                                            if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",file);
                                            perror("ERRORE: scrittura nel  file");
                                        }
                                        if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Dimensione totale della scrittura: %lu Esito : successo\n",file,get_last_w_size());
                                        //chiusura del file
                                        if (closeFile(resolvedPath)==-1)
                                        {
                                            if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",file);
                                            perror("ERRORE: chiusura del file");
                                        }
                                    }
                                }
                                else
                                {
                                    if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",file);
                                    perror("ERRORE: apertura del file fallita");
                                }
                            }
                            else
                            {// scrittura nel file aperto all'interno del server e già esistente in esso
                                num_files++;
                                //scrittura nel file
                                if (writeFile(resolvedPath,Dir) == -1)
                                {
                                    if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",file);
                                    perror("ERRORE: scrittura nel  file");
                                }
                                if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Dimensione totale della scrittura: %lu Esito : successo\n",file,get_last_w_size());
                                //chiusura del file
                                if (closeFile(resolvedPath)==-1)
                                {
                                    if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",file);
                                    perror("ERRORE: chiusura del file");
                                }
                            }
                            if (resolvedPath!=NULL) free(resolvedPath);
                        }
                        else
                        {
                            if (flag_stampa==1) printf("OP : -W (scrivi file) File : %s Esito : fallimento\n",file);
                            printf("ERRORE: %s non e' un file regolare\n",file);
                        }
                    }
                token2 = strtok_r(NULL,",",&save2);
                }
            }
        else if (strcmp(curr->cmd,"D") == 0)
        {
            if (curr->prec != NULL )
            {
                if (strcmp(curr->prec->cmd,"w") == 0 || strcmp(curr->prec->cmd,"W") == 0)
                {
                    if (flag_stampa==1) printf("OP : -D (seleziona directory di destinazione per -w o -W) Esito : successo\n");
                }
                else if (flag_stampa==1) printf("OP : -D (seleziona directory di destinazione per -w o -W) Esito : fallimento\n");
            }
            else if (flag_stampa==1) printf("OP : -D (seleziona directory di destinazione per -w o -W) Esito : fallimento\n");
        }
        else if (strcmp(curr->cmd,"r")==0)
        {
            dir = NULL;
            if (curr->next != NULL)
            {
                if(strcmp(curr->next->cmd,"d") == 0) dir = curr->next->arg;
            }

            char * save3 = NULL;
            char * token3 = strtok_r(curr->arg,",",&save3);

            while(token3 != NULL)
            {
                char * file = token3;
                //per ogni file passato come argomento sarà eseguita la serie open-read-close

                if (openFile(file,0) == -1)
                {
                    if (flag_stampa==1) printf("OP: -r (leggi file) File : %s Esito : fallimento\n",file);
                    perror("ERRORE: apertura del file");
                }
                else
                {
                    //READ FILE
                    char * buf = NULL;
                    size_t size;
                    if (readFile(file,(void**)&buf,&size)==-1)
                    {
                        if (flag_stampa==1) printf("OP: -r (leggi file) File : %s Esito : fallimento\n",file);
                        perror("ERRORE: lettura del file");
                    }
                    else
                    {
                        if (dir != NULL)
                        {
                            //il file letto sarà salvato nella directory impostata
                            char path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
                            memset(path,0,UNIX_MAX_STANDARD_FILENAME_LENGHT);
                            char * file_name = basename(file);
                            sprintf(path,"%s/%s",dir,file_name);

                            //se la directory non esiste essa viene creata
                            mkdir(dir,S_IRWXU);
                            //se il file non esiste esso viene creato
                            FILE* of;
                            of = fopen(path,"w");
                            if (of==NULL)
                            {
                                if (flag_stampa==1) printf("OP: -r (leggi file) File : %s Esito : fallimento\n",file);
                                perror("ERRORE: salvataggio del file\n");
                            }
                            else
                            {
                                fprintf(of,"%s",buf);
                                fclose(of);
                            }
                        }
                    }
                    if (closeFile(file)==-1)
                    {
                        if (flag_stampa==1) printf("OP: -r (leggi file) File : %s Esito : fallimento\n",file);
                        perror("ERRORE: chiusura file");
                    }
                    else
                    {
                        if (flag_stampa==1) printf("OP: -r (leggi file) File : %s Dimensione Letta: %lu Esito : successo\n",file,
                                                   strnlen(buf,MAX_CNT_LEN));
                    }
                    free(buf);
                }
                token3 = strtok_r(NULL,",",&save3);
            }
            if (token3!=NULL) free(token3);
        }
        else if (strcmp(curr->cmd,"R")==0)
        {
            dir = NULL;
            if (curr->next != NULL)
            {
                if(strcmp(curr->next->cmd,"d") == 0) dir = curr->next->arg;
            }

            int N;
            if ((N = isNumber(curr->arg))==-1)
            {
                if (flag_stampa==1) printf("OP : -R (leggi N file) Esito : fallimento\n");
                printf("L'opzione -R vuole un numero come argomento\n");
            }
            else
            {
                int n;
                if ((n = readNFiles(N,dir))==-1)
                {
                    if (flag_stampa==1) printf("OP : -R (leggi N file) Esito : fallimento\n");
                    perror("Errore lettura file");
                }
                else
                {
                    if (flag_stampa==1) printf("OP : -R (leggi N file) File Letti : %d Dimensione Letta: %lu Esito : successo \n",n,get_last_rN_size());
                }
            }

        }
        else if (strcmp(curr->cmd,"d") == 0)
        {
            if (curr->prec != NULL )
            {
                if (strcmp(curr->prec->cmd,"r") == 0 || strcmp(curr->prec->cmd,"R") == 0)
                {
                    if (flag_stampa==1) printf("OP : -d (seleziona directory di destinazione per -r o -R) Esito : successo\n");
                }
                else if (flag_stampa==1) printf("OP : -d (seleziona directory di destinazione per -r o -R) Esito : fallimento\n");
            }
            else if (flag_stampa==1) printf("OP : -d (seleziona directory di destinazione per -r o -R) Esito : fallimento\n");
        }
        else if (strcmp(curr->cmd,"c")==0)
        {
            char * save4 = NULL;
            char * token4 = strtok_r(curr->arg,",",&save4);

            while(token4) {
                char * file = token4;

                //per ogni file passato come argomento sarà eseguita remove
                if (removeFile(file)==-1)
                {
                    if (flag_stampa==1) printf("OP : -c (rimuovi file) File : %s Esito : fallimento\n",file);
                    perror("ERRORE: rimozione del file");
                }else{
                    if (flag_stampa==1) printf("OP : -c (rimuovi file) File : %s Esito : successo\n",file);
                }

                token4 = strtok_r(NULL,",",&save4);
            }

        }
        else if (strcmp(curr->cmd,"l")==0)
        {
            char * save4 = NULL;
            char * token4 = strtok_r(curr->arg,",",&save4);

            while(token4)
            {
                char * file = token4;

                //per ogni file passato come argomento sarà eseguita lock
                if (lockFile(file)==-1)
                {
                    if (flag_stampa==1) printf("OP : -l (ottieni la mutua esclusione sul file) File : %s Esito : fallimento\n",file);
                    perror("ERRORE: lock del file");
                }else{
                    if (flag_stampa==1) printf("OP : -l (ottieni la mutua esclusione sul file) File : %s Esito : successo\n",file);
                }

                token4 = strtok_r(NULL,",",&save4);
            }

        }
        else if (strcmp(curr->cmd,"u")==0)
        {
            char * save4 = NULL;
            char * token4 = strtok_r(curr->arg,",",&save4);

            while(token4) {
                char * file = token4;

                //per ogni file passato come argomento sarà eseguita unlock
                if (unlockFile(file)==-1)
                {
                    if (flag_stampa==1) printf("OP : -u (rilascia la mutua esclusione sul file) File : %s Esito : fallimento\n",file);
                    perror("ERRORE: unlock del file");
                }else{
                    if (flag_stampa==1) printf("OP : -u (rilascia la mutua esclusione sul file file) File : %s Esito : successo\n",file);
                }

                token4 = strtok_r(NULL,",",&save4);
            }

        }
        else if (strcmp(curr->cmd,"t")==0)
        {
            char * time_s = curr->arg;
            if (time_s == NULL)
            {
                if (flag_stampa==1) printf("OP : -t (imposta il tempo che intercorre tra due operazioni) Esito : fallimento\n");
            }
            else
            {
                tms = (int)strtol(time_s, NULL, 10);
                if (tms == 0)
                {
                    if (flag_stampa==1) printf("OP : -t (imposta il tempo che intercorre tra due operazioni) Esito : fallimento\n");
                }
                else if (flag_stampa==1) printf("OP : -t (imposta il tempo che intercorre tra due operazioni) Esito : successo\n");
            }
        }

        curr = curr->next;
    }

    freeList(&listCmd);
    free(resolvedPath);
    free(arg);
    //una volta che tutte le operazioni richieste sono state eseguite la connessione al server viene chiusa
    closeConnection(farg);

    return 0;
}

// UPDATE: 22/08 pomeriggio 2 fine client
