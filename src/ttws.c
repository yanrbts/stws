/*
 * Copyright (c) 2025-2025, yanruibinghxu@gmail.com All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <locale.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/epoll.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <inttypes.h>
#include <ctype.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <arpa/inet.h>
#include <libwebsockets.h>
#include "cJSON.h"
#include "list.h"

/* Log levels */
#define LL_DEBUG            0
#define LL_VERBOSE          1
#define LL_NOTICE           2
#define LL_WARNING          3
#define LL_NOTHING          4

#define LOG_MAX_LEN         1024 /* Default maximum length of syslog messages.*/
#define TW_MAX_LEN          4
#define TW_DEFAULT_TPORT    8888 /* Default TCP Port*/
#define TW_DEFAULT_WS_PORT  8765 /* Default websocket Port*/

/* Error codes */
#define C_OK                0
#define C_ERR               -1
#define AE_OK               0
#define AE_ERR              -1
#define AE_NONE             0   /* No events registered. */
#define AE_READABLE         1   /* Fire when descriptor is readable. */
#define AE_WRITABLE         2   /* Fire when descriptor is writable. */
#define AE_BARRIER          4   /* With WRITABLE, never fire the event if the
                                    READABLE event already fired in the same event
                                    loop iteration. Useful when you want to persist
                                    things to disk before sending replies, and want
                                    to do that in a group fashion. */

#define AE_FILE_EVENTS      1
#define AE_TIME_EVENTS      2
#define AE_ALL_EVENTS       (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT        4
#define AE_CALL_AFTER_SLEEP 8
#define AE_NOMORE           -1
#define AE_DELETED_EVENT_ID -1
/* Macros */
#define AE_NOTUSED(V)       ((void) V)

#define ANET_OK             0
#define ANET_ERR            -1
#define ANET_ERR_LEN        256
#define NET_IP_STR_LEN      46 /* INET6_ADDRSTRLEN is 46, but we need to be sure */

#define TW_VERSION                          "1.0.0"
#define CONFIG_DEFAULT_TCP_KEEPALIVE        300
#define CONFIG_DEFAULT_TCP_BACKLOG          511                 /* TCP listen backlog. */
#define PROTO_MAX_QUERYBUF_LEN              (1024*1024*1024)    /* 1GB max query buffer. */
#define PROTO_IOBUF_LEN                     (1024*16)           /* Generic I/O buffer size */
#define PROTO_REPLY_CHUNK_BYTES             (16*1024)           /* 16k output buffer */
#define NET_MAX_WRITES_PER_EVENT            (1024*64)
#define CONFIG_DEFAULT_FILE                 "./config.conf"
#define CONFIG_DEFAULT_PID_FILE             "/var/run/ttws.pid"
#define CONFIG_READ_LEN                     1024
#define MAX_LOG_FILE_SIZE                   (100 * 1024 * 1024) /* 10 MB */
#define MSG_TYPE                            1
#define TEMP_FILE                           "/tmp/wsmsgqueuefile"

#define MAX_MSG_SIZE                        (1024*16)           /* Maximum size of a message, 16k*/
#define WS_QUEUE_SIZE                       1024                /* Define queue size */
#define MAX_PARTS                           (MAX_MSG_SIZE / WS_QUEUE_SIZE) /* Maximum number of parts */
#define WS_MAX_NUM                          100                 /* Maximum number of websocket connections */

/* Use macro for checking log level to avoid evaluating arguments in cases log
 * should be ignored due to low level. */
#define serverLog(level, ...) do {      \
        _serverLog(level, __VA_ARGS__); \
    } while(0)

struct aeEventLoop;
/* Types and data structures */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* System V message queue operations
 * Message structure */
typedef struct {
    long msg_type;
    // int part_index;     /* Current shard index */
    // int total_parts;    /* Total number of shards */
    char data[WS_QUEUE_SIZE];
} Message;

/* TCP client data structure */
typedef struct Client {
    int fd;                 /* socket file handle */
    char *querybuf;         /* request data */
    size_t len;             /* length of request data */
    time_t ctime;           /* Client creation time. */
    struct list_head list;
    /* Response buffer */
    size_t sentlen;         /* Amount of bytes already sent in the current*/
    int bufpos;
    char buf[PROTO_REPLY_CHUNK_BYTES];
} Client;

/* File event structure */
typedef struct aeFileEvent {
    int mask; /* one of AE_(READABLE|WRITABLE|BARRIER) */
    aeFileProc *rfileProc;
    aeFileProc *wfileProc;
    void *clientData;
} aeFileEvent;

/* Time event structure */
typedef struct aeTimeEvent {
    long long id;           /* time event identifier. */
    long when_sec;          /* seconds */
    long when_ms;           /* milliseconds */
    aeTimeProc *timeProc;
    aeEventFinalizerProc *finalizerProc;
    void *clientData;
    struct aeTimeEvent *prev;
    struct aeTimeEvent *next;
} aeTimeEvent;

/* A fired event */
typedef struct aeFiredEvent {
    int fd;
    int mask;
} aeFiredEvent;

typedef struct aeApiState {
    int epfd;
    struct epoll_event *events;
} aeApiState;

/* State of an event based program */
typedef struct aeEventLoop {
    int maxfd;          /* highest file descriptor currently registered */
    int setsize;        /* max number of file descriptors tracked */
    int stop;
    void *apidata;      /* This is used for polling API specific data */
    long long timeEventNextId;
    time_t lastTime;     /* Used to detect system clock skew */
    aeFileEvent *events; /* Registered events */
    aeFiredEvent *fired; /* Fired events */
    aeTimeEvent *timeEventHead;
    aeBeforeSleepProc *beforesleep;
} aeEventLoop;

struct Server {
    char *configfile;       /* config file path */
    int daemonize;          /* True if running as a daemon */
    char *pidfile;          /* PID file path */
    char *wsip;             /* websocket address*/
    char *tcpip;            /* TCP address*/
    int wsport;             /* websocket port */
    uint32_t tcpport;       /* Server Tcp Port */
    int ipfd;               /* TCP socket file descriptors */
    int tcp_backlog;        /* TCP listen() backlog */
    int tcpkeepalive;       /* Set SO_KEEPALIVE if non-zero. */
    char *logfile;          /* Path of log file */
    int logfilesize;        /* logfile size*/
    struct list_head clist; /* List of client*/
    struct list_head clients_pending_write; /* There is to write or install handler. */
    uint32_t clist_size;                /* number of client */
    size_t client_max_querybuf_len;     /* Limit for client query buffer length */
    /* epoll */
    aeEventLoop *el;                    /* main event loop object*/
    /* websocket */
    struct lws *wslist[WS_MAX_NUM];     /* List of websocket client*/
    uint32_t wslist_size;               /* number of websocket client */
    struct lws_context *ws_context;
    int mqid;                           /* Message queue id */
} twserver;

static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
static int handleClientsWithPendingWrites(void);
static Client *createClient(int fd);
static void twServerFreeConfig(void);

/****************************MEMORY********************************/
static char *zstrdup(const char *s) {
    size_t l = strlen(s)+1;
    char *p = malloc(l);

    memcpy(p,s,l);
    return p;
}

static void *tw_realloc(void *ptr, size_t size) {
    void *newptr;

    if (ptr == NULL) return malloc(size);
    newptr = realloc(ptr, size);
    if (newptr == NULL) return NULL;
    return newptr;
}

/*********************************LOG*********************************/
static void rotate_log_file(const char *logfile) {
    char new_logfile[256];
    time_t now = time(NULL);
    struct tm now_tm;

    if (localtime_r(&now, &now_tm)) {
        strftime(new_logfile, sizeof(new_logfile), "%Y-%m-%d_%H-%M-%S.log", &now_tm);
        /* Rename log file */
        rename(logfile, new_logfile); 
    }
}

/* Detect if the file size exceeds 100 MB, 
 * rename the file to the current time, 
 * then create a new file, open it and write the log*/
static void sLogRaw(int level, const char *msg) {
    const char *c = ".-*#@";
    FILE *fp;
    time_t now;
    struct tm now_tm, *lt_ret;
	struct timeval tv;
    struct stat st;
	int off;
	char time_buf[64];
    int log_to_stdout = twserver.logfile[0] == '\0';

    /* If the log file is not empty, check the size */
    if (!log_to_stdout && stat(twserver.logfile, &st) == 0) {
        /* If it exceeds 100 MB, perform log rotation */
        if (st.st_size >= twserver.logfilesize) {
            rotate_log_file(twserver.logfile);
        }
    }

    fp = log_to_stdout ? stdout : fopen(twserver.logfile,"a");
    if (!fp) return;

    /* get current time */
	now = time(NULL);
	lt_ret = localtime_r(&now, &now_tm);
	gettimeofday(&tv,NULL);

    if(lt_ret) {
		off = strftime(time_buf, sizeof(time_buf), "%d %b %Y %H:%M:%S.", lt_ret);
		snprintf(time_buf+off, sizeof(time_buf)-off, "%03d",(int)tv.tv_usec/1000);
	} else {
		const char err_msg[] = "(NO TIME AVAILABLE)";
		memcpy(time_buf, err_msg, sizeof(err_msg));
	}
    fprintf(fp,"\033[90m[%d]:%s %c \033[0m%s\n",
            (int)getpid(), time_buf, c[level], msg);

    fflush(fp);
    if (!log_to_stdout) fclose(fp);
}

/* Like serverLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
static void _serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    sLogRaw(level,msg);
}

static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

static long long mstime(void) {
    return ustime()/1000;
}

/* Given the filename, return the absolute path as an SDS string, or NULL
 * if it fails for some reason. Note that "filename" may be an absolute path
 * already, this will be detected and handled correctly.
 *
 * The function does not try to normalize everything, but only the obvious
 * case of one or more "../" appearning at the start of "filename"
 * relative path. */
static char *getAbsolutePath(char *filename) {
    char *ptr = NULL;
    char absolute_path[PATH_MAX];
    struct stat st;

    if (stat(filename, &st) != 0) {
        serverLog(LL_DEBUG, "Error resolving file information");
        return NULL;
    }

    if (!S_ISREG(st.st_mode)) {
        serverLog(LL_DEBUG, "%s is neither a regular file nor a directory.", filename);
        return NULL;
    }

    if (realpath(filename, absolute_path) != NULL) {
        ptr = zstrdup(absolute_path);
        return ptr;
    } else {
        serverLog(LL_DEBUG, "Error resolving absolute path");
        return NULL;
    }
}

/*********************************EPOLL*****************************************/
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = malloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = malloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        free(state);
        return -1;
    }
    state->epfd = epoll_create(1024);
    if (state->epfd == -1) {
        free(state->events);
        free(state);
        return -1;
    }
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = realloc(state->events, sizeof(struct epoll_event)*setsize);
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    free(state->events);
    free(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    int op = eventLoop->events[fd].mask == AE_NONE ?
            EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    
    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) {
        serverLog(LL_DEBUG, "epoll_ctl error");
        return -1;
    }
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd, EPOLL_CTL_MOD, fd, &ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    retval = epoll_wait(state->epfd, state->events, eventLoop->setsize,
        tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    
    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    return numevents;
}

// static char *aeApiName(void) {
//     return "epoll";
// }

static void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

static aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    if ((eventLoop = malloc(sizeof(*eventLoop))) == NULL) goto err;
    eventLoop->events = malloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = malloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    eventLoop->setsize = setsize;
    eventLoop->lastTime = time(NULL);
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;

    if (aeApiCreate(eventLoop) == -1) goto err;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;

err:
    if (eventLoop) {
        free(eventLoop->events);
        free(eventLoop->fired);
        free(eventLoop);
    }
    return NULL;
}

static void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    free(eventLoop->events);
    free(eventLoop->fired);
    free(eventLoop);
}

static void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

static int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
    aeFileEvent *fe = &eventLoop->events[fd];

    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

static void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask) {
    if (fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return;

    /* We want to always remove AE_BARRIER if set when AE_WRITABLE
     * is removed. */
    if (mask & AE_WRITABLE) mask |= AE_BARRIER;

    aeApiDelEvent(eventLoop, fd, mask);
    fe->mask = fe->mask & (~mask);
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}

static int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}

static void aeGetTime(long *seconds, long *milliseconds) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = cur_sec + milliseconds/1000;
    when_ms = cur_ms + milliseconds%1000;
    if (when_ms >= 1000) {
        when_sec++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}

static long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = malloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->prev = NULL;
    te->next = eventLoop->timeEventHead;
    if (te->next)
        te->next->prev = te;
    eventLoop->timeEventHead = te;
    return id;
}

static int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    while (te) {
        if (te->id == id) {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    while (te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec &&
                 te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* Process time events */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while (te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    eventLoop->lastTime = now;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
    while (te) {
        long now_sec, now_ms;
        long long id;

        /* Remove events scheduled for deletion. */
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            if (te->prev)
                te->prev->next = te->next;
            else
                eventLoop->timeEventHead = te->next;
            if (te->next)
                te->next->prev = te->prev;
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            free(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            id = te->id;
            retval = te->timeProc(eventLoop, id, te->clientData);
            processed++;
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        te = te->next;
    }
    return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * if flags has AE_CALL_AFTER_SLEEP set, the aftersleep callback is called.
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */
static int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);
        if (shortest) {
            long now_sec, now_ms;

            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;

            /* How many milliseconds we need to wait for the next
             * time event to fire? */
            long long ms =
                (shortest->when_sec - now_sec)*1000 +
                shortest->when_ms - now_ms;
            
            if (ms > 0) {
                tvp->tv_sec = ms/1000;
                tvp->tv_usec = (ms % 1000)*1000;
            } else {
                tvp->tv_sec = 0;
                tvp->tv_usec = 0;
            }
        } else {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }

        /* Call the multiplexing API, will return only on timeout or when
         * some event fires. */
        numevents = aeApiPoll(eventLoop, tvp);

        for (j = 0; j < numevents; j++) {
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
            int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int fired = 0; /* Number of events fired for current fd. */

            /* Normally we execute the readable event first, and the writable
             * event laster. This is useful as sometimes we may be able
             * to serve the reply of a query immediately after processing the
             * query.
             *
             * However if AE_BARRIER is set in the mask, our application is
             * asking us to do the reverse: never fire the writable event
             * after the readable. In such a case, we invert the calls.
             * This is useful when, for instance, we want to do things
             * in the beforeSleep() hook, like fsynching a file to disk,
             * before replying to a client. */
            int invert = fe->mask & AE_BARRIER;

            /* Note the "fe->mask & mask & ..." code: maybe an already
             * processed event removed an element that fired and we still
             * didn't processed, so we check if the event is still valid.
             *
             * Fire the readable event if the call sequence is not
             * inverted. */
            if (!invert && fe->mask & mask & AE_READABLE) {
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                fired++;
            }

            /* Fire the writable event. */
            if (fe->mask & mask & AE_WRITABLE) {
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            /* If we have to invert the call, fire the readable event now
             * after the writable one. */
            if (invert && fe->mask & mask & AE_READABLE) {
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            processed++;
        }
    }
    /* Check time events */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);
        aeProcessEvents(eventLoop, AE_ALL_EVENTS|AE_CALL_AFTER_SLEEP);
    }
    twServerFreeConfig();
}

/****************************************PROC**********************************************/
static int anetV6Only(int s) {
    int yes = 1;
    if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) == -1) {
        serverLog(LL_DEBUG, "setsockopt: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetSetReuseAddr(int fd) {
    int yes = 1;
    /* Make sure connection-intensive things like the redis benchmark
     * will be able to close/open sockets a zillion of times */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        serverLog(LL_DEBUG, "setsockopt SO_REUSEADDR: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetListen(int s, struct sockaddr *sa, socklen_t len, int backlog) {
    if (bind(s,sa,len) == -1) {
        serverLog(LL_DEBUG, "bind: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }

    if (listen(s, backlog) == -1) {
        serverLog(LL_DEBUG, "listen: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static int _anetTcpServer(int port, char *bindaddr, int af, int backlog) {
    int s = -1, rv;
    char _port[6]; /* strlen("65535") */
    struct addrinfo hints, *servinfo, *p;

    snprintf(_port, 6, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* No effect if bindaddr != NULL */

    if ((rv = getaddrinfo(bindaddr,_port, &hints, &servinfo)) != 0) {
        serverLog(LL_DEBUG, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((s = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
            continue;
        
        if (af == AF_INET6 && anetV6Only(s) == ANET_ERR) goto err;
        if (anetSetReuseAddr(s) == ANET_ERR) goto err;
        if (anetListen(s, p->ai_addr, p->ai_addrlen, backlog) == ANET_ERR) s = ANET_ERR;
        goto end;
    }
    if (p == NULL) {
        serverLog(LL_DEBUG, "unable to bind socket, errno: %d", errno);
        goto err;
    }

err:
    if (s != -1) close(s);
    s = ANET_ERR;
end:
    freeaddrinfo(servinfo);
    return s;
}

static int anetSetBlock(int fd, int non_block) {
    int flags;

    /* Set the socket blocking (if non_block is zero) or non-blocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        serverLog(LL_DEBUG, "fcntl(F_GETFL): %s", strerror(errno));
        return ANET_ERR;
    }

    if (non_block)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) == -1) {
        serverLog(LL_DEBUG, "fcntl(F_SETFL,O_NONBLOCK): %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetNonBlock(int fd) {
    return anetSetBlock(fd,1);
}

static int anetBlock(int fd) {
    return anetSetBlock(fd,0);
}

/* Set TCP keep alive option to detect dead peers. The interval option
 * is only used for Linux as we are using Linux-specific APIs to set
 * the probe send time, interval, and count. */
static int anetKeepAlive(int fd, int interval) {
    int val = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1) {
        serverLog(LL_DEBUG, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }

#ifdef __linux__
    /* Default settings are more or less garbage, with the keepalive time
     * set to 7200 by default on Linux. Modify settings to make the feature
     * actually useful. */

    /* Send first probe after interval. */
    val = interval;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        serverLog(LL_DEBUG, "setsockopt TCP_KEEPIDLE: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Send next probes after the specified interval. Note that we set the
     * delay as interval / 3, as we send three probes before detecting
     * an error (see the next setsockopt call). */
    val = interval/3;
    if (val == 0) val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        serverLog(LL_DEBUG, "setsockopt TCP_KEEPINTVL: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Consider the socket in error state after three we send three ACK
     * probes without getting a reply. */
    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        serverLog(LL_DEBUG, "setsockopt TCP_KEEPCNT: %s\n", strerror(errno));
        return ANET_ERR;
    }
#else
    ((void) interval); /* Avoid unused var warning for non Linux systems. */
#endif
    return ANET_OK;
}

static int anetTcpServer(int port, char *bindaddr, int backlog) {
    return _anetTcpServer(port, bindaddr, AF_INET, backlog);
}

static int anetTcp6Server(int port, char *bindaddr, int backlog) {
    return _anetTcpServer(port, bindaddr, AF_INET6, backlog);
}

static int anetSetTcpNoDelay(int fd, int val)
{
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1)
    {
        serverLog(LL_DEBUG, "setsockopt TCP_NODELAY: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetEnableTcpNoDelay(int fd)
{
    return anetSetTcpNoDelay(fd, 1);
}

int anetDisableTcpNoDelay(int fd)
{
    return anetSetTcpNoDelay(fd, 0);
}

static int anetGenericAccept(int s, struct sockaddr *sa, socklen_t *len) {
    int fd;
    while (1) {
        fd = accept(s, sa, len);
        if (fd == -1) {
            if (errno == EINTR)
                continue;
            else {
                // serverLog(LL_DEBUG, "accept: %s", strerror(errno));
                return ANET_ERR;
            }
        }
        break;
    }
    return fd;
}

int anetTcpAccept(int s, char *ip, size_t ip_len, int *port) {
    int fd;
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(s,(struct sockaddr*)&sa,&salen)) == -1)
        return ANET_ERR;

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) inet_ntop(AF_INET,(void*)&(s->sin_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) inet_ntop(AF_INET6,(void*)&(s->sin6_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin6_port);
    }
    return fd;
}

static int getServerIp(int s) {
    struct sockaddr_storage addr;
    socklen_t addr_len;
    char ipstr[INET6_ADDRSTRLEN];

    addr_len = sizeof(addr);
    if (getsockname(s, (struct sockaddr*)&addr, &addr_len) == -1) {
        serverLog(LL_DEBUG, "getsockname failed: %s", strerror(errno));
        goto err;
    }

    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &ipv4->sin_addr, ipstr, sizeof(ipstr));
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&addr;
        inet_ntop(AF_INET6, &ipv6->sin6_addr, ipstr, sizeof(ipstr));
    }

    twserver.tcpip = zstrdup(ipstr);

    return C_OK;
err:
    return C_ERR;
}

/* Initialize a set of file descriptors to listen to the specified 'port'
 * binding the addresses specified in the server configuration.
 *
 * the server configuration
 * contains no specific addresses to bind, this function will try to
 * bind * (all addresses) for both the IPv4 and IPv6 protocols.
 *
 * On success the function returns C_OK.
 *
 * On error the function returns C_ERR. For the function to be on
 * error, at least one of the server.bindaddr addresses was
 * impossible to bind, or no bind addresses were specified in the server
 * configuration but the function is not able to bind * for at least
 * one of the IPv4 or IPv6 protocols. */
int listenToPort(int port, int *fd) {
    // int unsupported = 0;

    // *fd = anetTcp6Server(port, NULL, twserver.tcp_backlog);
    // if (*fd != ANET_ERR) {
    //     anetNonBlock(*fd);
    //     serverLog(LL_WARNING,"IPv6: supproted");
    // } else if (errno == EAFNOSUPPORT) {
    //     unsupported++;
    //     serverLog(LL_WARNING,"Not listening to IPv6: unsupproted");
    // }

    // if (unsupported) {
        /* Bind the IPv4 address as well. */
        *fd = anetTcpServer(port, NULL, twserver.tcp_backlog);
        if (*fd != ANET_ERR) {
            anetNonBlock(*fd);
        } else if (errno == EAFNOSUPPORT) {
            // unsupported++;
            serverLog(LL_WARNING,"Not listening to IPv4: unsupproted");
            return C_ERR;
        }
    // }
    return C_OK;
}

#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(int fd, int flags, char *ip) {
    Client *c;
    if ((c = createClient(fd)) == NULL) {
        serverLog(LL_WARNING,
            "Error registering fd event for the new client: %s (fd=%d)",
            strerror(errno),fd);
        close(fd); /* May be already closed, just ignore errors */
        return;
    }
}

void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    AE_NOTUSED(el);
    AE_NOTUSED(mask);
    AE_NOTUSED(privdata);

    while (max--) {
        cfd = anetTcpAccept(fd, cip, sizeof(cip), &cport);

        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", strerror(errno));
            return;
        }
        serverLog(LL_VERBOSE, "Accepted %s:%d", cip, cport);
        acceptCommonHandler(cfd, 0, cip);
    }
}

/* This is our timer interrupt, called server.hz times per second.
 * Here is where we do a number of things that need to be done asynchronously.
 * For instance:
 *
 * - Active expired keys collection (it is also performed in a lazy way on
 *   lookup).
 * - Software watchdog.
 * - Update some statistic.
 * - Incremental rehashing of the DBs hash tables.
 * - Triggering BGSAVE / AOF rewrite, and handling of terminated children.
 * - Clients timeout of different kinds.
 * - Replication reconnection.
 * - Many more...
 *
 * Everything directly called here will be called server.hz times per second,
 * so in order to throttle execution of things we want to do less frequently
 * a macro is used: run_with_period(milliseconds) { .... }
 */

int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    AE_NOTUSED(eventLoop);
    AE_NOTUSED(id);
    AE_NOTUSED(clientData);

    return C_OK;
}

void beforeSleep(struct aeEventLoop *eventLoop) {
    AE_NOTUSED(eventLoop);

    /* Handle writes with pending output buffers. */
    handleClientsWithPendingWrites();
}

static int yesnotoi(char *s) {
    if (!strcasecmp(s,"yes")) return 1;
    else if (!strcasecmp(s,"no")) return 0;
    else return -1;
}

static void loadConfigFile(void) {
    FILE *fp;
    FILE *logfp;
    char *err = NULL;
    char tmp[256] = {0};
    char buf[CONFIG_READ_LEN+1];

    fp = fopen(twserver.configfile, "r");
    if (fp == NULL) {
        fprintf(stderr, "Error open config file");
        exit(1);
    }

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        char *p = buf;
        /* Remove whitespace characters at the beginning of the line */
        while (isspace(*p))
            p++;
        /* Skip lines starting with # */
        if (*p == '#' || *p == '\0')
            continue;
        
        /* Remove newlines at the end of lines */
        p[strcspn(p, "\n")] = '\0';

        char *first = p;
        char *second = NULL;

        while (*p && !isspace(*p))
            p++;
        if (*p) {
            *p = '\0';
            second = p+1;
        }

        while (second && isspace(*second))
            second++;

        if (!first || !second) {
            fprintf(stderr, "Error: Invalid config line or missing parameter.\n");
            continue;
        }

        if (!strcasecmp(first, "tcpport")) {
            twserver.tcpport = atoi(second);
            if (twserver.tcpport < 0 || twserver.tcpport > 65535) {
                err = "Invalid port"; goto loaderr;
            }
        } else if (!strcasecmp(first, "wsport")) {
            twserver.wsport = atoi(second);
            if (twserver.wsport < 0 || twserver.wsport > 65535) {
                err = "Invalid UDP port"; goto loaderr;
            }
        } else if (!strcasecmp(first, "tcp-backlog")) {
            twserver.tcp_backlog = atoi(second);
            if (twserver.tcp_backlog < 0) {
                err = "Invalid backlog value"; goto loaderr;
            }
        } else if (!strcasecmp(first, "tcp-keepalive")) {
            twserver.tcpkeepalive = atoi(second);
            if (twserver.tcpkeepalive < 0) {
                err = "Invalid tcp-keepalive value"; goto loaderr;
            }
        } else if (!strcasecmp(first, "logfile")) {
            memset(tmp, 0, sizeof(tmp));
            free(twserver.logfile);
            twserver.logfile = zstrdup(second);
            if (twserver.logfile[0] != '\0') {
                /* Test if we are able to open the file. The server will not
                 * be able to abort just for this problem later... */
                logfp = fopen(twserver.logfile,"a");
                if (logfp == NULL) {
                    snprintf(tmp, sizeof(tmp), "Can't open the log file: %s", strerror(errno));
                    err = tmp;
                    goto loaderr;
                }
                fclose(logfp);
            }
        } else if (!strcasecmp(first, "daemonize")) {
            if ((twserver.daemonize = yesnotoi(second)) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(first, "pidfile")) {
            free(twserver.pidfile);
            twserver.pidfile = zstrdup(second);
        } else if (!strcasecmp(first, "logfilesize")) {
            twserver.logfilesize = atoi(second);
            if (twserver.logfilesize < 5 || twserver.logfilesize > 400) {
                err = "Invalid port, logfilesize requirement is between 5 MB and 400 MB"; 
                goto loaderr;
            }
        }
    }
    fclose(fp);
    return;
loaderr:
    fprintf(stderr, "%s\n", err);
    exit(1);
}

/*******************************SINGLE*********************************/
static void sigShutdownHandler(int sig) {
    char *msg;

    switch (sig) {
    case SIGINT:
        msg = "Received SIGINT scheduling shutdown...";
        break;
    case SIGTERM:
        msg = "Received SIGTERM scheduling shutdown...";
        break;
    default:
        msg = "Received shutdown signal, scheduling shutdown...";
    }

    /* Remove the pid file if possible and needed. */
    if (twserver.daemonize || twserver.pidfile) {
        serverLog(LL_NOTICE,"Removing the pid file.");
        unlink(twserver.pidfile);
    }

    // if (msgctl(twserver.mqid, IPC_RMID, NULL) == -1) {
    //     serverLog(LL_VERBOSE, "msgctl failed");
    //     exit(EXIT_FAILURE);
    // }

    /* SIGINT is often delivered via Ctrl+C in an interactive session.
     * If we receive the signal the second time, we interpret this as
     * the user really wanting to quit ASAP without waiting to persist
     * on disk. */
    if (sig == SIGINT) {
        serverLog(LL_WARNING, "You insist... exiting now.");
        aeStop(twserver.el);
        exit(1); /* Exit with an error since this was not a clean shutdown. */
    } else {
        serverLog(LL_WARNING, msg);
        exit(0);
    }
}

static void setupSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigShutdownHandler;
    /* This is the termination signal sent by the kill(1) command by default.
     * Because it can be caught by applications, using SIGTERM gives programs
     * a chance to terminate gracefully by cleaning up before exiting */
    sigaction(SIGTERM, &act, NULL);
    /* This signal is generated by the terminal driver when we press the
     * interrupt key (often DELETE or Control-C). This signal is sent to all
     * processes in the foreground process group . This
     * signal is often used to terminate a runaway program, especially when it’s
     * generating a lot of unwanted output on the screen.*/
    sigaction(SIGINT, &act, NULL);
    return;
}

static void createPidFile(void) {
    /* If pidfile requested, but no pidfile defined, use
     * default pidfile path */
    if (!twserver.pidfile) twserver.pidfile = zstrdup(CONFIG_DEFAULT_PID_FILE);

    /* Try to write the pid file in a best-effort way. */
    FILE *fp = fopen(twserver.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",(int)getpid());
        fclose(fp);
    }
}

static void daemonize(void) {
    int fd;

    if (fork() != 0) exit(0); /* parent exits */
    setsid(); /* create a new session */

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. */
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

/************************************DATA*************************************/
static int parseData(Client *c) {
    size_t total_len;
    // int part;

    if (c->querybuf == NULL || twserver.mqid == -1) {
        serverLog(LL_WARNING, "Invalid input or message queue ID");
        return C_ERR;
    }

    const char *ptr = c->querybuf;
    total_len = c->len;
    printf("strlen : %zu\n", total_len);
    /* Calculate the total number of fragments */
    // total_parts = (total_len+WS_QUEUE_SIZE-1) / WS_QUEUE_SIZE;

    // for (int part = 0; total_len > 0; part++) {
    //     Message msg;

    //     msg.msg_type = MSG_TYPE;
    //     msg.part_index = part;
    //     msg.total_parts = total_parts;

    //     size_t chunk_size = (total_len > sizeof(msg.data)-1) ? sizeof(msg.data)-1 : total_len;
    //     strncpy(msg.data, ptr, chunk_size);
    //     msg.data[chunk_size] = '\0';

    //     ptr += chunk_size;
    //     total_len -= chunk_size;

    //     int retry = 5;
    //     while (retry-- > 0) {
    //         if (msgsnd(twserver.mqid, &msg, sizeof(Message)-sizeof(long), IPC_NOWAIT) == -1) {
    //             if (errno == EAGAIN) {
    //                 /* wait 1 ms */
    //                 usleep(1000);
    //                 continue;
    //             } else {
    //                 serverLog(LL_WARNING, "msgsnd error %s", strerror(errno));
    //                 return C_ERR;
    //             }
    //         }
    //         printf("Sent part %d/%d, size: %zu\n", msg.part_index+1, total_parts, chunk_size);
    //         break;
    //     }
    // }

    while (total_len > 0) {
        Message msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_type = MSG_TYPE;

        size_t chunk_size = (total_len > sizeof(msg.data)-1) ? sizeof(msg.data)-1 : total_len;
        strncpy(msg.data, ptr, chunk_size);
        msg.data[chunk_size] = '\0';

        ptr += chunk_size;
        total_len -= chunk_size;

        int retry = 5;
        while (retry-- > 0) {
            if (msgsnd(twserver.mqid, &msg, sizeof(msg.data), IPC_NOWAIT) == -1) {
                if (errno == EAGAIN) {
                    /* wait 1 ms */
                    usleep(1000);
                    continue;
                } else {
                    serverLog(LL_WARNING, "msgsnd error %s", strerror(errno));
                    return C_ERR;
                }
            }
            // printf("Sent part %d, size: %zu\n", ++part, chunk_size);
            break;
        }
    }

    return C_OK;
}

/************************************CLIENT***************************************/
static Client *createClient(int fd) {
    Client *c = malloc(sizeof(Client));

    if (c == NULL) return NULL;

    if (fd != -1) {
        anetNonBlock(fd);
        anetEnableTcpNoDelay(fd);
        if (twserver.tcpkeepalive)
            anetKeepAlive(fd, twserver.tcpkeepalive);
        
        if (aeCreateFileEvent(twserver.el,fd,AE_READABLE,
            readQueryFromClient, c) == AE_ERR)
        {
            close(fd);
            free(c);
            return NULL;
        }
    }

    c->ctime = time(NULL);
    c->fd = fd;
    c->querybuf = NULL;
    c->len = 0;
    c->bufpos = 0;
    c->sentlen = 0;
    list_add(&c->list, &twserver.clist);
    twserver.clist_size++;

    return c;
}

static void freeClient(Client *c) {
    list_del(&c->list);

    if (c->fd != -1) {
        aeDeleteFileEvent(twserver.el, c->fd, AE_READABLE);
        aeDeleteFileEvent(twserver.el, c->fd, AE_WRITABLE);
        close(c->fd);
        c->fd = -1;
    }
    if (c->querybuf != NULL) free(c->querybuf);
    free(c);
    twserver.clist_size--;
}

static int isJSONStructureValid(const char *jsonStr) {
    int braceCount = 0, bracketCount = 0, quoteCount = 0;
    int inString = 0;

    for (size_t i = 0; jsonStr[i] != '\0'; i++) {
        char c = jsonStr[i];

        if (c == '"' && (i == 0 || jsonStr[i - 1] != '\\')) {  // 处理字符串，忽略转义引号
            inString = !inString;
            quoteCount++;
        }
        if (!inString) {
            if (c == '{') braceCount++;
            if (c == '}') braceCount--;
            if (c == '[') bracketCount++;
            if (c == ']') bracketCount--;
        }

        if (braceCount < 0 || bracketCount < 0) {
            return 0;  // 结构错误
        }
    }

    return (braceCount == 0 && bracketCount == 0 && (quoteCount % 2 == 0));
}


static int isCompleteJSON(const char *jsonStr) {
    /* First check whether the structure is closed */
    if (!isJSONStructureValid(jsonStr)) {
        return 0;
    }

    /* Then use cJSON parsing to check */
    cJSON *json = cJSON_Parse(jsonStr);
    if (json == NULL) {
        return 0;  
    }

    cJSON_Delete(json);
    return 1;
}

static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    Client *c = (Client*)privdata;
    int nread, readlen;
    size_t qblen = 0;
    AE_NOTUSED(el);
    AE_NOTUSED(mask);
    
    readlen = PROTO_IOBUF_LEN;

    if (c->querybuf != NULL) {
        qblen = c->len;
    }

    /* +1 for adding terminator*/
    c->querybuf = tw_realloc(c->querybuf, qblen+readlen+1);
    if (c->querybuf == NULL) {
        serverLog(LL_VERBOSE, "Memory allocation failed");
        freeClient(c);
        return;
    }

    nread = read(fd, c->querybuf+qblen, readlen);
    if (nread == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            serverLog(LL_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    } else if (nread == 0) {
        serverLog(LL_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }

    c->len += nread;

    if (c->len >= twserver.client_max_querybuf_len) {
        serverLog(LL_WARNING,"Closing client that reached max query buffer length: %lu", c->len);
        freeClient(c);
        return;
    }

    /* If the data is relatively large and the fragment is accepted several times, 
     * \0 is added each time, but every time the data is appended, it starts from \0. 
     * This ensures that the data acceptance is completed and only the 
     * \0 terminator will be added at the end to ensure the integrity of the data. */
    c->querybuf[c->len] = '\0';

    /* Start parsing commands */
    if (isCompleteJSON(c->querybuf)) {
        parseData(c);

        /* Release the cache to prevent confusion 
         * with data received next time*/
        free(c->querybuf);
        c->querybuf = NULL;
        c->len = 0;
    }
}

/* Write data in output buffers to client. Return C_OK if the client
 * is still valid after the call, C_ERR if it was freed. */
static int writeToClient(Client *c, int handler_installed) {
    ssize_t nwritten = 0, totwritten = 0;

    while (c->bufpos != 0) {
        if (c->bufpos > 0) {
            nwritten = write(c->fd, c->buf+c->sentlen, c->bufpos-c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            if ((int)c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        } else {
            /* Note that we avoid to send more than NET_MAX_WRITES_PER_EVENT
             * bytes, in a single threaded server it's a good idea to serve
             * other clients as well */
            if (totwritten > NET_MAX_WRITES_PER_EVENT) {
                serverLog(LL_WARNING, "Send data larger than 16k");
                break;
            }
        }
    }

    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            serverLog(LL_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return C_ERR;
        }
    }

    if (c->bufpos != 0) {
        c->sentlen = 0;
        if (handler_installed) aeDeleteFileEvent(twserver.el, c->fd, AE_WRITABLE);
        freeClient(c);
        return C_ERR;
    }
    return C_OK;
}

/* Write event handler. Just send data to the client. */
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    AE_NOTUSED(el);
    AE_NOTUSED(mask);
    writeToClient(privdata, 1);
}

/* This function is called just before entering the event loop, in the hope
 * we can just write the replies to the client output buffer without any
 * need to use a syscall in order to install the writable event handler,
 * get it called, and so forth. */
static int handleClientsWithPendingWrites(void) {
    Client *c, *t;

    list_for_each_entry_safe(c, t, &twserver.clients_pending_write, list)
    {
        /* Try to write buffers to the client socket. */
        if (writeToClient(c, 0) == C_ERR) continue;

        /* Connect the client to the record list */
        list_del(&c->list);
        list_add(&c->list, &twserver.clist);

        /* If after the synchronous writes above we still have data to
         * output to the client, we need to install the writable handler. */
        if (c->bufpos != 0) {
            if (aeCreateFileEvent(twserver.el, c->fd, AE_WRITABLE,
                sendReplyToClient, c) == AE_ERR) {
                freeClient(c);
            }
        }
    }
    return 0;
}

static int prepareClientToWrite(Client *c) {
    if (c->fd <= 0) return C_ERR;

    /* Schedule the client to write the output buffers to the socket, unless
     * it should already be setup to do so (it has already pending data). */
    if (c->bufpos == 0) {
        /* Add to reply list */
        list_del(&c->list);
        list_add(&c->list, &twserver.clients_pending_write);
    }
    /* Authorize the caller to queue in the output buffer of this client. */
    return C_OK;
}

/* Low level functions to add more data to output buffers. */
static int _addReplyToBuffer(Client *c, const char *s, size_t len) {
    size_t available = sizeof(c->buf)-c->bufpos;

    /* Check that the buffer has enough space available for this string. */
    if (len > available) return C_ERR;

    memcpy(c->buf+c->bufpos, s, len);
    c->bufpos += len;
    return C_OK;
}

/* This low level function just adds whatever protocol you send it to the
 * client buffer, trying the static buffer initially, and using the string
 * of objects if not possible.
 */
static void addReplyString(Client *c, const char *s, size_t len) {
    if (prepareClientToWrite(c) != C_OK) return;
    if (_addReplyToBuffer(c,s,len) != C_OK) {
        serverLog(LL_WARNING, "Add Reply Error");
        return;
    }
}

/*************************************INIT***********************************/
static int createMsgQueue() {
    key_t key;
    int status;
    struct stat buffer;
    struct msqid_ds buf;

    status = stat(TEMP_FILE, &buffer);
    if (status == -1) {
        if (errno == ENOENT) {
            FILE *fp = fopen(TEMP_FILE, "w");
            if (fp == NULL) {
                serverLog(LL_WARNING, "Create temp file failed, %s", strerror(errno));
                exit(EXIT_FAILURE);
            }
            fclose(fp);
        } else {
            serverLog(LL_WARNING, "Stat() temp file failed, %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
    } else {
        serverLog(LL_WARNING, "Temp file already exists %s", TEMP_FILE);
    }

    /* convert a pathname and a project identifier to a System V IPC key
     * The  ftok()  function  uses the identity of the file named by the given pathname 
     * (which must refer to an existing,accessible file) and the least significant 
     * 8 bits of proj_id (which must be nonzero) to generate a key_t type System V IPC key*/
    key = ftok(TEMP_FILE, 65);
    if (key == -1) {
        serverLog(LL_WARNING,
            "Failed ftok(). Error message: '%s'",
            strerror(errno));
        exit(EXIT_FAILURE);
    }

    twserver.mqid = msgget(key, 0666 | IPC_CREAT);
    if (twserver.mqid == -1) {
        serverLog(LL_WARNING, "Message queue creation failed, %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (msgctl(twserver.mqid, IPC_STAT, &buf) == 0) {
        buf.msg_qbytes = MAX_MSG_SIZE;
        msgctl(twserver.mqid, IPC_SET, &buf);
    }

    return C_OK;
}

static void initServerConfig() {
    INIT_LIST_HEAD(&twserver.clist);
    INIT_LIST_HEAD(&twserver.clients_pending_write);
    twserver.tcpport = TW_DEFAULT_TPORT;
    twserver.clist_size = 0;
    twserver.logfile = zstrdup("");
    twserver.wsip = zstrdup("");
    twserver.tcpip = NULL;
    twserver.wsport = TW_DEFAULT_WS_PORT;
    twserver.tcpkeepalive = CONFIG_DEFAULT_TCP_KEEPALIVE;
    twserver.tcp_backlog = CONFIG_DEFAULT_TCP_BACKLOG;
    twserver.client_max_querybuf_len = PROTO_MAX_QUERYBUF_LEN;
    twserver.configfile = zstrdup(CONFIG_DEFAULT_FILE);
    twserver.daemonize = 0;
    twserver.pidfile = NULL;
    twserver.logfilesize = MAX_LOG_FILE_SIZE;

    memset(twserver.wslist, 0, sizeof(twserver.wslist));
    twserver.wslist_size = 0;
    twserver.ws_context = NULL;
    twserver.mqid = 0;
}

static void twServerFreeConfig(void) {
    free(twserver.logfile);
    free(twserver.wsip);
    free(twserver.tcpip);
    free(twserver.configfile);
}

static void twInitServer(void) {
    FILE *fp;
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();

    createMsgQueue();

    twserver.el = aeCreateEventLoop(1024);
    if (twserver.el == NULL) {
        serverLog(LL_WARNING,
            "Failed creating the event loop. Error message: '%s'",
            strerror(errno));
        exit(1);
    }

    /* Open the TCP listening socket for the user commands. */
    if (twserver.tcpport != 0 && listenToPort(twserver.tcpport, &twserver.ipfd) == C_ERR)
        exit(1);
    
    (void)getServerIp(twserver.ipfd);

    /* Create the timer callback, this is our way to process many background
     * operations incrementally, like clients timeout, eviction of unaccessed
     * expired keys and so forth. */
    if (aeCreateTimeEvent(twserver.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverLog(LL_DEBUG, "Can't create event loop timers.");
        exit(1);
    }

    if (aeCreateFileEvent(twserver.el, twserver.ipfd, 
        AE_READABLE, acceptTcpHandler,NULL) == AE_ERR)
    {
        serverLog(LL_DEBUG, "Can't create event loop timers.");
        exit(1);
    }
}
/************************************WebSocket*****************************/
static int wsCallback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) 
{
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        {
            if (twserver.wslist_size < WS_MAX_NUM) {
                twserver.wslist[twserver.wslist_size++] = wsi;
                serverLog(LL_DEBUG, "WebSocket Client connected, total: %d", twserver.wslist_size);
            }
            /* lws_callback_on_writable() - 
             * Request a callback when this socket 
             * becomes able to be written to without blocking */
            lws_callback_on_writable(wsi);
        }
        break;
    case LWS_CALLBACK_SERVER_WRITEABLE:
        {
            Message msg;
            size_t total_len = 0;
            unsigned char buf[LWS_PRE+MAX_MSG_SIZE];
            
            /* Loop through messages until complete data arrives */
            while (total_len < MAX_MSG_SIZE) {
                ssize_t recvlen = msgrcv(twserver.mqid, &msg, MAX_MSG_SIZE-total_len, MSG_TYPE, IPC_NOWAIT);
                if (recvlen > 0) {
                    /* Add the received partial data to the buffer */
                    memcpy(&buf[LWS_PRE+total_len], msg.data, recvlen);
                    total_len += recvlen;
                } else {
                    /* If there is no more data, break out of the loop */
                    break;
                }
            }

            if (total_len > 0) {
                /* Sent to all connected WebSocket clients */
                for (int i = 0; i < twserver.wslist_size; i++) {
                    if (twserver.wslist[i] != NULL) {
                        printf("RECV: %ld\n", total_len);

                        lws_write(twserver.wslist[i], &buf[LWS_PRE], total_len, LWS_WRITE_TEXT);
                    }
                }
            }

            /* Ensure that WebSocket can always enter the write 
            * callback to avoid getting stuck */
            for (int i = 0; i < twserver.wslist_size; i++) {
                if (twserver.wslist[i] != NULL) {
                    /* lws_callback_on_writable() - 
                    * Request a callback when this socket becomes 
                    * able to be written to without blocking */
                    lws_callback_on_writable(twserver.wslist[i]);
                }
            }
        }
        break;
    case LWS_CALLBACK_RECEIVE:
        serverLog(LL_DEBUG, "WebSocket Received: %s", (char *)in);
        break;
    case LWS_CALLBACK_CLOSED:
        for (int i = 0; i < twserver.wslist_size; i++) {
            if (twserver.wslist[i] == wsi) {
                twserver.wslist[i] = NULL;
                twserver.wslist[i] = twserver.wslist[--twserver.wslist_size];
                serverLog(LL_DEBUG, "WebSocket Client disconnected, total: %d", twserver.wslist_size);
                break;
            }
        }
        break;
    default:
        break;  
    }
    return 0;
}

static void wsServerProcess() {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);

    static struct lws_protocols protocols[] = {
        { "http-only", wsCallback, 0, WS_QUEUE_SIZE },
        { NULL, NULL, 0, 0 }
    };

    info.port = twserver.wsport;
    info.protocols = protocols;

    twserver.ws_context = lws_create_context(&info);
    if (twserver.ws_context == NULL) {
        serverLog(LL_WARNING, "Failed to create WebSocket context.");
        exit(EXIT_FAILURE);
    }

    serverLog(LL_VERBOSE, "WebSocket Server listening on port %d", twserver.wsport);

    while (1) {
        /* to 0; ignored; for backward compatibility */
        lws_service(twserver.ws_context, 100);
    }
}

/************************************MAIN**********************************/
static void version(void) {
    printf("ttws server v=%s\n", TW_VERSION);
    exit(0);
}

static void usage(void) {
    fprintf(stderr,"Usage: ./ttws [/path/to/config.conf]\n");
    fprintf(stderr,"       ./ttws -v or --version\n");
    fprintf(stderr,"       ./ttws -h or --help\n");
    fprintf(stderr,"Examples:\n");
    fprintf(stderr,"       ./ttws (run the server with default conf)\n");
    fprintf(stderr,"       ./ttws /etc/ttws/config.conf\n");
    exit(1);
}

int main(int argc, char *argv[]) {
    int j;
    pid_t wspid;

    /* The setlocale() function is used to set or query the program's current locale.
     * 
     * The function is used to set the current locale of the program and the 
     * collation of the specified locale. Specifically, the LC_COLLATE parameter
     * represents the collation of the region. By setting it to an empty string,
     * the default locale collation is used.*/
    setlocale(LC_COLLATE, "");

	/* The  tzset()  function initializes the tzname variable from the TZ environment variable.  
     * This function is automati‐cally called by the other time conversion functions 
     * that depend on the timezone.*/
    tzset();
    initServerConfig();

    if (argc >= 2) {
        j = 1;
        char *configfile = NULL;
        char *tp = NULL;
        /* Handle special options --help and --version */
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0) version();
        if (strcmp(argv[1], "--help") == 0 ||
            strcmp(argv[1], "-h") == 0) usage();
        
        /* First argument is the config file name? */
        if (argv[j][0] != '-' || argv[j][1] != '-') {
            configfile = argv[j];
            if ((tp = getAbsolutePath(configfile)) != NULL) {
                free(twserver.configfile);
                twserver.configfile = tp;
            } else {
                serverLog(LL_WARNING, "Warning: no config file specified, using the default config.");
            }
        }
    }

    loadConfigFile();
    if (twserver.daemonize)
        daemonize();

    if (twserver.daemonize || twserver.pidfile)
        createPidFile();

    twInitServer();

    wspid = fork();
    if (wspid < 0) {
        serverLog(LL_WARNING, "Fork WebSocket process failed");
        exit(EXIT_FAILURE);
    }

    /* websocket child process*/
    if (wspid == 0) {
        wsServerProcess();
        exit(0);
    }

    aeSetBeforeSleepProc(twserver.el, beforeSleep);
    aeMain(twserver.el);
    aeDeleteEventLoop(twserver.el);

    /* Delete message queue */
    if (msgctl(twserver.mqid, IPC_RMID, NULL) == -1) {
        serverLog(LL_VERBOSE, "msgctl failed");
        exit(EXIT_FAILURE);
    }
    
    return 0;
}

