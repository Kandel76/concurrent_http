#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "iowrapper.h"
#include "listener_socket.h"
#include "protocol.h"
#include "connection.h"
#include "response.h"
#include "request.h"
#include "queue.h"
#include "rwlock.h"

#define BUFFER_SIZE 2048
#define QUEUE_SIZE  520
//-------------------------------------------
#define NUM_LOCKS 1024
#define debug printf("%ld\t",pthread_self()); printf

rwlock_t* file_locks[NUM_LOCKS];  // Fixed-size array of locks

void init_locks(void* args) {
    (void)args;
    for (int i = 0; i < NUM_LOCKS; i++) {
        file_locks[i] =(rwlock_new(N_WAY,10));
    }
}

// Hash the filename into a fixed-size index
unsigned int lock_index(const char *filename) {
    unsigned int hash = 5381;
    while (*filename) {
        hash = ((hash << 5) + hash) + *filename++;
    }
    return hash % NUM_LOCKS;
}


//--------------------------------------------

pthread_mutex_t audit_mutex = PTHREAD_MUTEX_INITIALIZER;
char random_buffer[BUFFER_SIZE];
queue_t *queue;
pthread_mutex_t queue_mutex_t = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
void handle_connection(int connfd);
void handle_get(conn_t *conn);
void handle_put(conn_t *conn);
void handle_unsupported(conn_t *conn);
void *worker_thread(void *arg);

int main(int argc, char *argv[]) {
    int num_of_threads = 4;
    int opt;
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        switch (opt) {
        case 't':
            if (optarg == NULL || atoi(optarg) <= 0) {
                return EXIT_FAILURE;
            }
            num_of_threads = atoi(optarg);
            break;
        default: return EXIT_FAILURE;
        }
    }
    printf("number of threads: %d\n", num_of_threads);
    fflush(stdout);

    if (optind >= argc) {
        printf("optind failure\n");
        fflush(stdout);
        return EXIT_FAILURE;
    }

    //check port number
    char *endptr = NULL;
    size_t port = (size_t) strtoull(argv[optind], &endptr, 10);
    printf("port number: %zu", port);
    if (endptr && *endptr != '\0') {
        //warnx("invalid port number: %s", argv[optind]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    //create a listener socket
    Listener_Socket_t *listener = ls_new(port);
    if (!listener) {
        fprintf(stdout, "Cannot open socket\n");
        fflush(stdout);
        return EXIT_FAILURE;
    }

    //create new threads
    pthread_t threads[num_of_threads];
    for (int i = 0; i < num_of_threads; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }

    //create a new queue
    queue = queue_new(QUEUE_SIZE);
    //hashmap_init(&file_locks);
    init_locks(NULL);  // Call init_locks to initialize the locks
    while (1) {
        //get client socket

        ssize_t connfd = ls_accept(listener);
        if (connfd < 0) {
            fprintf(stdout, "ls_accept failed");
            fflush(stdout);
            continue;
        }
        //queue is thread safe
        if (!queue_push(queue, (void *) (intptr_t) connfd)) {
            printf("Queue Push Failed");
            fflush(stdout);
            close(connfd);
            continue;
        }
        pthread_cond_signal(&queue_cond); //boradcast before
    }

    //clear all the threads
    for (int i = 0; i < num_of_threads; i++) {
        pthread_join(threads[i], NULL);
    } 

    //delete listener socket
    ls_delete(&listener);
    queue_delete(&queue);

    return EXIT_SUCCESS;
}

int isDir(const char *path) {
    struct stat statbuf;
    if (stat(path, &statbuf) != 0)
        return 0;
    return S_ISDIR(statbuf.st_mode);
}


void *worker_thread(void *arg) {
    (void) arg;
    int connfd;
    while (1) {
        while (!queue_pop(queue, (void **) &connfd)) {
            continue;
        }
        handle_connection(connfd);
    }
    close(connfd);
}

pthread_mutex_t audit_lock = PTHREAD_MUTEX_INITIALIZER;

void send_audit(const char *method, const char *uri, const char *code, const char *request_id) {
    pthread_mutex_lock(&audit_lock);
    fprintf(stderr, "%s,%s,%s,%s\n", method, uri, code, request_id ? request_id : "0");
    fflush(stderr);
    pthread_mutex_unlock(&audit_lock);
}

void handle_connection(int connfd) {
    //create a connection struct
    debug("PROCESSING NEW CLIENT\n");
    conn_t *conn = conn_new(connfd);
    //create a response struct
    const Response_t *res = conn_parse(conn);
    bool dir = false;

    if (res != NULL) {
        conn_send_response(conn, res);

    } else {
        //get request
        const Request_t *req = conn_get_request(conn);

        //get request id
        char *request_id = conn_get_header(conn, "Request-Id");
        if (!request_id)
            request_id = "0";

        //check if dir
        char *uri = conn_get_uri(conn);
        if (isDir(uri) > 0) 
	dir = true;

	unsigned int index = lock_index(uri);
	debug("validation complete\n");
        //start operation
        if (req == &REQUEST_GET) {
		debug("processing get\n");
	    reader_lock(file_locks[index]);
            if (dir) {
                send_audit("GET", uri, "404", request_id);
                conn_send_response(conn, &RESPONSE_FORBIDDEN);
                goto end;
            }
	    handle_get(conn);
	    reader_unlock(file_locks[index]);
        } else if (req == &REQUEST_PUT) {
            	debug("processing put\n");
	    writer_lock(file_locks[index]);
	    
	    if (dir) {
                send_audit("PUT", uri, "404", request_id);
                conn_send_response(conn, &RESPONSE_FORBIDDEN);
                goto end;
            }  
	    handle_put(conn);
	    writer_unlock(file_locks[index]);
	    // unlok
        } else {
            //handle unsupported
            send_audit("NOT_IMPLEMENTED", uri, "505", request_id);
	    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
        }
    }
end:
    	debug("finished process\n");
    conn_delete(&conn);
    close(connfd);
}

void handle_get(conn_t *conn) {
	debug("in handle get\n");
    //get request id and uri
    char *uri = conn_get_uri(conn);
    
    char *request_id = conn_get_header(conn, "Request-Id");
    if (!request_id)
        request_id = "0";

    

    int fd = open(uri, O_RDONLY);
    if (fd < 0) {
	    debug("file not found\n");
    	send_audit("GET", uri, "404", request_id);
	    conn_send_response(conn, &RESPONSE_NOT_FOUND);
        return;
    }


    struct stat st;
    fstat(fd, &st);
    //send audit and then the message
    send_audit("GET", uri, "200", request_id);
    conn_send_file(conn, fd, st.st_size);
    close(fd);
    debug("handle get FINISHED\n");
}

bool file_existence(const char *uri) {
    struct stat buffer;
    return stat(uri, &buffer) == 0 ? true : false;
}

void handle_put(conn_t *conn) {
	debug("in handle PUT\n");
    
    // Get the target file URI
    char *uri = conn_get_uri(conn);

    // Read headers------------------------------------
    char *request_id = conn_get_header(conn, "Request-Id");
    if (!request_id) request_id = "0";
    char *content_length_str = conn_get_header(conn, "Content-Length");
    if (!content_length_str) {
        conn_send_response(conn, &RESPONSE_BAD_REQUEST);
	return;
    }
    int content_length = atoi(content_length_str);
    if (content_length <= 0) {
        conn_send_response(conn, &RESPONSE_BAD_REQUEST);
	return;
    }


    // Create a temporary filename
    char tmp_uri[BUFFER_SIZE];
    snprintf(tmp_uri, BUFFER_SIZE, "%s.tmp", uri); // Append ".tmp" to filename

    // Check if the target file already exists
    bool file_exists = file_existence(uri);

    // Open a temporary file instead of the final destination
    int fd = open(tmp_uri, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
	send_audit("PUT", uri, "403", request_id);
	conn_send_response(conn, &RESPONSE_FORBIDDEN);
        //writer_unlock(file_locks[index]);
	return;
    }
    
    // Write data from the request body into the temporary file
    conn_recv_file(conn, fd);
    close(fd);
    
    // Rename the temporary file to the actual URI (atomic operation)
    if (rename(tmp_uri, uri) != 0) {
        unlink(tmp_uri);  // Delete the temp file if rename fails
        conn_send_response(conn, &RESPONSE_INTERNAL_SERVER_ERROR);
	return;
    }
    
    // Send an appropriate response
    if (!file_exists) {
        send_audit("PUT", uri, "201", request_id);
        conn_send_response(conn, &RESPONSE_CREATED);
    } else {
        send_audit("PUT", uri, "200", request_id);
        conn_send_response(conn, &RESPONSE_OK);
    }

    debug("handle put finished\n");
    
}


