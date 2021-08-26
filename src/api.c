#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <limits.h>
#include <libgen.h>
#include <dirent.h>
#include "../headers/api.h"

#define MSG_SIZE 1024
#define MAX_CNT_LEN 1000
#define UNIX_MAX_STANDARD_FILENAME_LENGHT 108 /* man 7 unix */

int c_state = 0;//flag indicante lo stato di connessione del client
static size_t last_w_size = 0;
static size_t last_rN_size = 0;
int fd_s; //fd del socket ->sc
char sck_name[UNIX_MAX_STANDARD_FILENAME_LENGHT]; //nome del socket
char message[MSG_SIZE]; //stringa usata come comunicazione tra server-client

//funzioni utili
size_t get_last_w_size ()
{
    return last_w_size;
}
size_t get_last_rN_size ()
{
    return last_rN_size;
}
int msSleep(long time){
    if(time < 0){
        errno = EINVAL;
    }
    int res;
    struct timespec t;
    t.tv_sec = time/1000;
    t.tv_nsec = (time % 1000) * 1000000;

    do {
        res = nanosleep(&t, &t);
    }while(res && errno == EINTR);

    return res;
}
int compareTime(struct timespec a, struct timespec b){
    clock_gettime(CLOCK_REALTIME, &a);

    if(a.tv_sec == b.tv_sec){
        if(a.tv_nsec > b.tv_nsec)
            return 1;
        else if(a.tv_nsec == b.tv_nsec)
            return 0;
        else
            return -1;
    } else if(a.tv_sec > b.tv_sec)
        return 1;
    else
        return -1;
}
/* Read "n" bytes from a descriptor. */
ssize_t readn(int fd, void *vptr, size_t n)
{
    size_t  nleft;
    ssize_t nread;
    char   *ptr;

    ptr = vptr;
    nleft = n;
    while (nleft > 0)
    {
        if ( (nread = read(fd, ptr, nleft)) < 0)
        {
            if (errno == EINTR) nread = 0; /* and call read() again */
            else return (-1);
        }
        else
        if (nread == 0) break;  /* EOF */
        nleft -= nread;
        ptr += nread;
    }
    return (n - nleft);         /* return >= 0 */
}

/* Write "n" bytes to a descriptor. */
ssize_t writen(int fd, const void *vptr, size_t n)
{
    size_t nleft;
    ssize_t nwritten;
    const char *ptr;

    ptr = vptr;
    nleft = n;
    while (nleft > 0)
    {
        if ( (nwritten = write(fd, ptr, nleft)) <= 0)
        {
            if (nwritten < 0 && errno == EINTR) nwritten = 0;   /* and call write() again */
            else return (-1);    /* error */
        }

        nleft -= nwritten;
        ptr += nwritten;
    }
    return (n);
}

/*
char* reverse(char* str){
    if(str == NULL){
        errno = EINVAL;
        return NULL;
    }
    size_t n = strlen(str);
    int i;
    for (i = 0; i < n / 2; i++){
        char ch = str[i];
        str[i] = str[n - i - 1];
        str[n - i - 1] = ch;
    }
    return str;
}
*/
int openConnection(const char* nome_sock, int msec, const struct timespec abstime) {

    // nome_sock -> nome del socket a cui il client vuole connettersi
    // msec -> ogni quanto si riprova la connessione
    // abstime -> tempo massimo per la connessione

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    strncpy(sa.sun_path, nome_sock, UNIX_MAX_STANDARD_FILENAME_LENGHT);
    sa.sun_family = AF_UNIX;

    if ((fd_s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
    {// otteniamo il file descriptor del socket
        errno = EINVAL;
        return -1;
    }

    struct timespec time;
    while (connect(fd_s,(struct sockaddr*)&sa,sizeof(sa)) == -1 && compareTime(time, abstime) == -1)
    {// quando la connessione fallisce e siamo ancora entro il tempo massimo facciamo un' attesa di msec secondi
        msSleep(msec);
    }

    if (compareTime(time, abstime) > 0)
    {// se la connessione non è riuscita entro il tempo massimo abbiamo un errore di timeout
        errno = ETIMEDOUT;
        return -1;
    }

    // printf("Operazione Completata : openConnection\n");
    c_state = 1;// la flag di connessione viene settata ad 1
    strcpy(sck_name, nome_sock);// il nome del socket viene memorizzato in una variabile globale
    return 0;
}
int closeConnection(const char* nome_sock)
{
    if (c_state == 0) // nel caso in cui il client non è connesso abbiamo un errore
    {
        errno = EPERM;
        return -1;
    }

    if (strcmp(sck_name,nome_sock) == 0)
    {// il socket a cui il client è connesso corrisponde a quello parametro
        if (close(fd_s) == -1) // la connessione viene effettivamente chiusa
        {
            errno = EREMOTEIO;
            return -1;
        }
        //printf("Operazione Completata : closeConnection\n");
        return 0;
    }
    else
    {// il socket parametro non è valido
        errno = EINVAL;
        return -1;
    }
}
int openFile(const char* path, int flags) {

    if (c_state == 0)// il client è disconnesso -> errore
    {
        errno = ENOTCONN;
        return -1;
    }

    char* save = NULL;
    char buffer [MSG_SIZE];
    memset(buffer,0,MSG_SIZE);
    snprintf(buffer, MSG_SIZE,"openFile;%s;%d;",path, flags);// il comando viene scritto sulla stringa buffer

    if(writen(fd_s, buffer, MSG_SIZE) == -1)// il comando viene scritto nel canale con il server
    {
        errno = EREMOTEIO;
        return -1;
    }
    if(readn(fd_s, message, MSG_SIZE) == -1)// lettura della risposta del server -> scrittura in message
    {
        errno = EREMOTEIO;
        return -1;
    }

    char* token;
    token = strtok_r(message,";", &save);

    if (strcmp(token, "-1") == 0)
    { //l'operazione eseguita dal server è fallita
        token = strtok_r(NULL,";", &save);
        errno = (int)strtol(token, NULL, 10);
        return -1;
    }
    else
    { //l'operazione eseguita dal server è stata completata correttamente
        // printf("Operazione Completata : openFile\n");
        return 0;
    }

}
int closeFile(const char* path) {
    if (c_state == 0) // il client è disconnesso
    {
        errno = ENOTCONN;
        return -1;
    }

    char buffer[MSG_SIZE];
    memset(buffer,0,MSG_SIZE);
    sprintf(buffer, "closeFile;%s;", path);
    fflush(stdout);
    if(writen(fd_s, buffer, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }
    if(readn(fd_s, message, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }

    char* token;
    char* save = NULL;
    token = strtok_r(message, ";", &save);

    if (strcmp(token, "-1") == 0) // operazione terminata con fallimento
    {
        token = strtok_r(NULL,";", &save);
        errno = (int)strtol(token, NULL, 10);
        return -1;
    }
    else // operazione terminata con successo
    {
        //printf("Operazione Completata : closeFile\n");
        return 0;
    }
}
int removeFile(const char* path) {

    if (c_state == 0) // il client è disconnesso
    {
        errno = ENOTCONN;
        return -1;
    }

    char buffer [MSG_SIZE];
    memset(buffer,0,MSG_SIZE);
    sprintf(buffer, "removeFile;%s;", path);

    if(writen(fd_s, buffer, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }
    if(readn(fd_s, message, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }

    char* token;
    char* save = NULL;
    token = strtok_r(message, ";", &save);

    if (strcmp(token, "-1") == 0)
    { // operazione fallita
        token = strtok_r(NULL,";",&save);
        errno = (int)strtol(token, NULL, 10);
        return -1;
    }
    else
    {// operazione terminata con successo
        // printf("Operazione Completata : removeFile\n");
        return 0;
    }
}
int lockFile(const char* path)
{
    if (c_state == 0) // il client è disconnesso
    {
        errno = ENOTCONN;
        return -1;
    }

    char buffer [MSG_SIZE];
    memset(buffer,0,MSG_SIZE);
    sprintf(buffer, "lockFile;%s;", path);

    if(writen(fd_s, buffer, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }
    if(readn(fd_s, message, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }

    char* token;
    char* save = NULL;
    token = strtok_r(message, ";", &save);

    if (strcmp(token, "-1") == 0)
    { //operazione fallita
        token = strtok_r(NULL,";",&save);
        errno = (int)strtol(token, NULL, 10);
        return -1;
    }
    else
    { // operazione terminata con successo
        // printf("Operazione Completata : lockFile\n");
        return 0;
    }
}
int unlockFile(const char* path)
{
    if (c_state == 0) {
        errno = ENOTCONN;
        return -1;
    }

    char buffer [MSG_SIZE];
    memset(buffer,0,MSG_SIZE);
    sprintf(buffer, "unlockFile;%s", path);

    if(writen(fd_s, buffer, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }
    if(readn(fd_s, message, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }

    char* token;
    char* save = NULL;
    token = strtok_r(message, ";", &save);

    if (strcmp(token, "-1") == 0)
    { //operazione fallita
        token = strtok_r(NULL,";", &save);
        errno = (int)strtol(token, NULL, 10);
        return -1;
    }
    else
    { // operazione con successo
        // printf("Operazione Completata : unlockFile\n");
        return 0;
    }
}
int writeFile(const char* path, const char* dir)
{
    if (c_state == 0) // il client è disconnesso
    {
        errno = ENOTCONN;
        return -1;
    }

    if (dir != NULL)
    {// se la directory non esiste ne viene creata una nuova
        if (mkdir(dir, S_IRWXU) == -1)
        {
            if (errno != EEXIST) return -1;
        }
    }

    FILE *file_pointer;
    if ((file_pointer = fopen(path, "r")) == NULL) {// file non trovato
        errno = ENOENT;
        return -1;
    }

    char cnt[MAX_CNT_LEN];
    cnt[0] = '\0';
    char line[MAX_CNT_LEN];
    while (fgets(line, MAX_CNT_LEN, file_pointer)) {
        // leggiamo riga per riga il contenuto del file (ogni riga è registrata in line) e ne facciamo la append in cnt
        strcat(cnt, line);
    }
    fclose(file_pointer);
    last_w_size = strnlen(cnt,MAX_CNT_LEN);
    // preparaione del comando per il server
    char buffer[MSG_SIZE];
    memset(buffer, 0, MSG_SIZE);
    sprintf(buffer, "writeFile;%s;%s;", path, cnt);

    if (writen(fd_s, buffer, MSG_SIZE) == -1) {
        errno = EREMOTEIO;
        return -1;
    }
    if (readn(fd_s, message, MSG_SIZE) == -1) {
        errno = EREMOTEIO;
        return -1;
    }

    char *token;
    char *save = NULL;
    token = strtok_r(message, ";", &save);

    if (strcmp(token, "-1") == 0) { //l'operazione nel server non è andata a buon fine
        token = strtok_r(NULL, ";", &save);
        errno = (int) strtol(token, NULL, 10);
        return -1;
    }
    else
    {
        int rem_n = (int) strtol(token, NULL, 10);
        int i = 0;
        if (writen(fd_s, "1", MSG_SIZE) == -1) {
            errno = EREMOTEIO;
            return -1;
        }
        while (i < rem_n)
        {
            if (readn(fd_s, message, MSG_SIZE) == -1)
            {
                errno = EREMOTEIO;
                return -1;
            }

            char* save1 = NULL;
            char* complete_path = strtok_r(message,";",&save1); // la prima parte del messaggio del server è il path assoluto del file
            char* file_cnt = strtok_r(NULL,";",&save1); // la seconda parte è invece il contenuto del file
            // con le seguenti operazioni avremo che file_name conterrà solo il nome del file
            /*
            reverse(complete_path);
            char* save2 = NULL;
            char* file_name = strtok_r(complete_path,"/",&save2);
            reverse(file_name);
             */
            char* file_name = basename(complete_path);

            if (dir!=NULL)
            {   //salvataggio del file nella cartella specificata
                char true_path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
                memset(true_path,0,UNIX_MAX_STANDARD_FILENAME_LENGHT);
                sprintf(true_path,"%s/%s",dir,file_name);

                //se il file non esiste esso viene creato
                FILE *true_file;
                true_file = fopen(true_path, "w");
                if (true_file == NULL) {
                    printf("Errore nell'apertura del file\n");
                    return -1;
                } else {
                    fprintf(true_file, "%s", file_cnt);
                    fclose(true_file);
                }
            }
            i++;
        }
        // printf("Operazione Completata : writeFile\n");
        return 0;
    }
}
int appendToFile(const char* path, void* buf, size_t size, const char* dir)

{
    if (c_state == 0) // il client è disconnesso
    {
        errno = ENOTCONN;
        return -1;
    }

    if (dir != NULL)
    {// se la directory non esiste ne viene creata una nuova
        if (mkdir(dir, S_IRWXU) == -1)
        {
            if (errno != EEXIST) return -1;
        }
    }

    char cnt[MAX_CNT_LEN];
    cnt[0] = '\0';
    strncat(cnt,(char*)buf,size);

    // preparazione del comando per il server
    char buffer[MSG_SIZE];
    memset(buffer, 0, MSG_SIZE);
    sprintf(buffer, "appendFile;%s;%s;", path, cnt);

    if (writen(fd_s, buffer, MSG_SIZE) == -1) {
        errno = EREMOTEIO;
        return -1;
    }
    if (readn(fd_s, message, MSG_SIZE) == -1) {
        errno = EREMOTEIO;
        return -1;
    }


    char *token;
    char *save = NULL;
    token = strtok_r(message, ";", &save);

    if (strcmp(token, "-1") == 0)
    { //l'operazione nel server non è andata a buon fine
        token = strtok_r(NULL, ";", &save);
        errno = (int) strtol(token, NULL, 10);
        return -1;
    }
    else
    {
        int rem_n = (int) strtol(token, NULL, 10);
        int i = 0;
        while (i < rem_n) {
            if (readn(fd_s, message, MSG_SIZE) == -1) {
                errno = EREMOTEIO;
                return -1;
            }

            char* save1 = NULL;
            char* complete_path = strtok_r(message,";",&save1); // la prima parte del messaggio del server è il path assoluto del file
            char* file_cnt = strtok_r(NULL,";",&save1); // la seconda parte è invece il contenuto del file
            // con le seguenti operazioni avremo che file_name conterrà solo il nome del file
            /*
            reverse(complete_path);
            char* save2 = NULL;
            char* file_name = strtok_r(complete_path,"/",&save2);
            reverse(file_name);
             */
            char* file_name = basename(complete_path);

            if (dir!=NULL)
            {   //salvataggio del file nella cartella specificata
                char true_path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
                memset(true_path,0,UNIX_MAX_STANDARD_FILENAME_LENGHT);
                sprintf(true_path,"%s/%s",dir,file_name);

                //se il file non esiste esso viene creato
                FILE *true_file;
                true_file = fopen(true_path, "w");
                if (true_file == NULL) {
                    printf("Errore nell'apertura del file\n");
                    return -1;
                } else {
                    fprintf(true_file, "%s", file_cnt);
                    fclose(true_file);
                }
            }
            i++;
        }
        // printf("Operazione Completata : appendToFile\n");
        return 0;
    }
}
int readFile(const char* path, void** buf, size_t* size)
{
    if (c_state == 0)
    {
        errno = ENOTCONN;
        return -1;
    }
    /*
    if(buf[0] == NULL)
    {
        errno = EINVAL;
        return -1;
    }
*/
    char buffer [MSG_SIZE];
    memset(buffer,0,MSG_SIZE);
    sprintf(buffer, "readFile;%s;",path);

    if(writen(fd_s, buffer, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }
    if(readn(fd_s, message, MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }

    char* token;
    char* save = NULL;
    token = strtok_r(message, ";", &save);

    if (strcmp(token, "-1") == 0)
    {
        token = strtok_r(NULL, ";",&save);
        errno = (int)strtol(token, NULL, 10);
        return -1;
    }else
    {
        char* pass_cnt = malloc(sizeof(char)*MAX_CNT_LEN);
        if (pass_cnt == NULL)
        {
            free(pass_cnt);
            errno = ENOMEM;
            return -1;
        }
        strcpy(pass_cnt, token);
        token = strtok_r(NULL, ";", &save);
        size_t sizeFile = (int) strtol(token, NULL, 10);
        *size = sizeFile;
        *buf = (void*) pass_cnt;
        return 0;
    }
}
int readNFiles(int N, const char* dir)
{
    if (c_state==0)
    {
        errno=ENOTCONN;
        return -1;
    }

    if (dir != NULL)
    {// se la directory non esiste ne viene creata una nuova
        if (mkdir(dir, S_IRWXU) == -1)
        {
            if (errno != EEXIST) return -1;
        }
    }

    char cmd [MSG_SIZE];
    memset(cmd,0,MSG_SIZE);
    sprintf(cmd, "readNFiles;%d;",N);
    if (writen(fd_s,cmd,MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }

    char message [MSG_SIZE];
    memset(message,0,MSG_SIZE);
    if(readn(fd_s,message,MSG_SIZE)==-1)
    {
        errno = EREMOTEIO;
        return -1;
    }
    char* save = NULL;
    char* token = strtok_r(message,";",&save);
    if (strcmp(token,"-1") == 0)
    { //operazione fallita
        token = strtok_r(NULL,";",&save);
        errno = (int)strtol(token, NULL, 10);
        return -1;
    }

    int file_N = (int)strtol(token, NULL, 10);// file_N è il numero di files  letti dal server
    int i;
    last_rN_size = 0;

    //riceviamo i files secondo la quantità concordata
    for (i=0;i<file_N;i++)
    {
        //la api si occupa di inviare la conferma della propria disponibilità al server
        if (writen(fd_s,"1;",MSG_SIZE) == -1)
        {
            errno = EREMOTEIO;
            return -1;
        }

        //lettura di un file
        char readed [MAX_CNT_LEN+UNIX_MAX_STANDARD_FILENAME_LENGHT+1];
        memset(readed,0,MAX_CNT_LEN+UNIX_MAX_STANDARD_FILENAME_LENGHT+1);
        if (readn(fd_s,readed,MAX_CNT_LEN+UNIX_MAX_STANDARD_FILENAME_LENGHT+1) == -1)
        {
            errno = EREMOTEIO;
            return -1;
        }

        char* save1 = NULL;
        char* complete_path = strtok_r(readed,";",&save1); // la prima parte del messaggio del server è il path assoluto del file
        char* file_cnt = strtok_r(NULL,";",&save1); // la seconda parte è invece il contenuto del file
        // con le seguenti operazioni avremo che file_name conterrà solo il nome del file
        /*
        reverse(complete_path);
        char* save2 = NULL;
        char* file_name = strtok_r(complete_path,"/",&save2);
        reverse(file_name);
        */
        char* file_name = basename(complete_path);
        last_rN_size = last_rN_size + strnlen(file_cnt,MAX_CNT_LEN);

        if (dir!=NULL)
        {   //salvataggio del file nella cartella specificata
            char true_path[UNIX_MAX_STANDARD_FILENAME_LENGHT];
            memset(true_path,0,UNIX_MAX_STANDARD_FILENAME_LENGHT);
            sprintf(true_path,"%s/%s",dir,file_name);

            //se il file non esiste esso viene creato
            FILE* true_file;
            true_file = fopen(true_path,"w");
            if (true_file == NULL)
            {
                printf("Errore nell'apertura del file\n");
                return -1;
            }
            else
            {
                fprintf(true_file,"%s",file_cnt);
                fclose(true_file);
            }
        }
    }
    // la api notifica al server che non vi saranno altre letture per questa operazione
    if (writen(fd_s,"0;",MSG_SIZE) == -1)
    {
        errno = EREMOTEIO;
        return -1;
    }
    // printf("Operazione Completata : readNFile\n");
    return file_N;
}

// UPDATE: 22/08 pomeriggio 2 fine client
