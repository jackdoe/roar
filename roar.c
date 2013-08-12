#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <sys/inotify.h>
#include <stdio.h> 
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include "list.h"
extern int errno;
#define FORMAT(fmt,arg...) fmt " [%s():%s:%d @ %u]\n",##arg,__func__,__FILE__,__LINE__,(unsigned int) time(NULL)
#define E(fmt,arg...) fprintf(stderr,FORMAT(fmt,##arg))
#define D(fmt,arg...) printf(FORMAT(fmt,##arg))

#define SAYX(rc,fmt,arg...) do {   \
    E(fmt,##arg);                  \
    exit(rc);                      \
} while(0)

#define XFREE(x) do {   \
    if ((x))            \
        free((x));      \
    (x) = NULL;         \
} while (0)

#define XDUP(x,s) do {                                              \
    (x) = strdup((s));                                              \
    if ((x) == NULL)                                                \
        SAYX(EXIT_FAILURE,"not enough memory to dup %s",(s));       \
} while (0)

#define SAYPX(fmt,arg...) SAYX(EXIT_FAILURE,fmt " { %s }",##arg,errno ? strerror(errno) : "undefined error");

#define EVENT_LEN ( sizeof( struct inotify_event ) )
#define BUF_LEN ( 8192 * ( EVENT_LEN + 16 ) )

struct item {
    struct list_head list;
    int wd;
    char *path;
    char *__to;
    char *__from;
};

char *RECEIVER = NULL;
struct item HASH[65535];
static int INOTIFY;
    
inline unsigned int h_key(int wd);
inline struct item *h_bucket(int wd);
struct item *h_lookup(int wd);
struct item *h_find_by_path(char *path);
struct item *h_add(int wd, char *path);
void h_remove(int wd);
void h_init(void);
void execute(char *action,char *type, char *from, char *to);
void start_watching(char *dir);
void stop_watching(int wd);
void watch_recursive(char *root, char *parent);

#define A_MODIFY    0
#define A_DELETE    1
#define A_MOVE      2
#define T_DIR       0
#define T_FILE      1
char *ACTION[] = {
    "MODIFY",
    "DELETE",
    "MOVE"
};
char *TYPE[] = { "DIRECTORY", "FILE" };

void handler( struct inotify_event *event ) {
    char path[PATH_MAX];
    bzero(path,sizeof(path));
    char *from;
    char *to;
    char *action = NULL;
    if ((event->len > 0 && event->name[0] == '.') || event->mask & IN_IGNORED) {
        // D("ignoring %s",event->name);
        return;
    }

    struct item *e = h_lookup(event->wd);
    if (!e) {
        D("unexpected event without 'item struct' wd: %d name: %s mask: %d cookie: %d",
            event->wd,event->name,event->mask, event->cookie);
        inotify_rm_watch(INOTIFY,event->wd);
        return;
    }

    if (event->len > 0) {
        snprintf(path, sizeof(path), "%s/%s", e->path,event->name);
        from = path;
    } else
        from = e->path;

    if ((event->mask & (IN_ISDIR | IN_CREATE)) == (IN_ISDIR | IN_CREATE))
        start_watching(path);
    
    if (event->mask & IN_MOVED_FROM) {
        /* just ignore the event, wait for moved_to */
        XFREE(e->__from);
        XDUP(e->__from,path);
        D("from: %s ( %s -> %s)",path,e->__from,e->__to);
        /* do not set action */

    } else if (event->mask & IN_MOVED_TO) {
        XFREE(e->__to);
        XDUP(e->__to,path);
        D("to: %s ( %s -> %s)",path,e->__from,e->__to);

        action = ACTION[A_MOVE];
    } else if (event->mask & (IN_ATTRIB | IN_CREATE | IN_MODIFY)) {
        action = ACTION[A_MODIFY];
    } else if (event->mask & (IN_DELETE | IN_DELETE_SELF) ) {
        action = ACTION[A_DELETE];
    }

    if (action) {
        if (e->__to) {
            from = e->__from;
            to = e->__to;
            struct item *pair = h_find_by_path(from);
            if (pair) {
                SAYX(EXIT_FAILURE,"recursive move is not supported yet");
            }            
        }
        execute(action,(event->mask & (IN_ISDIR | IN_DELETE_SELF)) ? TYPE[T_DIR] : TYPE[T_FILE],from,to);
        XFREE(e->__from);
        XFREE(e->__to);
    }
    if (event->mask & (IN_DELETE_SELF))
        stop_watching(event->wd);
}

int main( int ac, char *av[] ) {
    int i;
    char buf[BUF_LEN];
    ssize_t n;
    char *p;
    struct inotify_event *event;
    char ROOT[PATH_MAX];

    if ( ac < 3 )
        SAYX(EXIT_FAILURE,"USAGE: %s receiver[example: notify.sh] path1 ..pathN\n", av[0]); 
    h_init();
    XDUP(RECEIVER,av[1]);
    INOTIFY = inotify_init();
    for (i = 2; i < ac; i++) {
        if (realpath(av[i],ROOT) != NULL)
            watch_recursive(ROOT,NULL);        
    }

    for (;;) {
        n = read(INOTIFY, buf, BUF_LEN);
        for (p = buf; p < buf + n; ) {
            event = (struct inotify_event *) p;
            handler(event);
            p += EVENT_LEN + event->len;
        }
    }

    XFREE(RECEIVER);
}

void execute(char *action,char *type, char *from, char *to) {
    pid_t  pid;
    int status;
    if ((pid = fork()) < 0) {
        exit(EXIT_FAILURE);
    }
    else if (pid == 0) {
        execl(RECEIVER,RECEIVER,action,type,from,to,NULL);
    } else {
        while (wait(&status) != pid)
           ;
    }
}

void h_init(void) {
    int i;
    for (i = 0; i < sizeof(HASH)/sizeof(HASH[0]); i++) 
        INIT_LIST_HEAD(&HASH[i].list);
}

inline unsigned int h_key(int wd) {
    return (wd % (sizeof(HASH)/sizeof(HASH[0])));
}

inline struct item *h_bucket(int wd) {
    return &HASH[h_key(wd)];
}

struct item *h_lookup(int wd) {
    struct item *bucket = h_bucket(wd),*elem;
    struct list_head *pos;
    list_for_each(pos, &bucket->list) {
        elem = list_entry(pos, struct item, list);
        if (elem->wd == wd)
            return elem;
    }
    return NULL;
}

struct item *h_find_by_path(char *path) {
    int i;
    struct item *elem;
    struct list_head *pos;
    for (i = 0; i < sizeof(HASH)/sizeof(HASH[0]); i++) {
        list_for_each(pos, &HASH[i].list) {
            elem = list_entry(pos, struct item, list);
            if (elem->path && strcmp(elem->path,path) == 0)
                return elem;
        }
    }
    return NULL;    
}

struct item *h_add(int wd, char *path) {
    struct item *elem = h_lookup(wd),*bucket = h_bucket(wd);
    D("adding %s to bucket %d",path,h_key(wd));
    if (elem)
        return elem;
    elem = malloc(sizeof(*elem));
    if (!elem)
        SAYPX("malloc");
    elem->wd = wd;
    elem->path = NULL;
    elem->__from = NULL;
    elem->__to = NULL;

    XDUP(elem->path,path);
    list_add_tail(&elem->list,&bucket->list);
    return elem;
}

void h_remove(int wd) {
    struct item *elem = h_lookup(wd);
    if (elem) {
        list_del(&elem->list);
        D("removing %s",elem->path);
        XFREE(elem->path);
        XFREE(elem);
    }
}

void start_watching(char *dir) {
    int wd = inotify_add_watch(INOTIFY, dir, (IN_ATTRIB | IN_CREATE | IN_MODIFY | IN_DELETE | IN_DELETE_SELF | IN_MOVED_TO | IN_MOVED_FROM | IN_MOVE_SELF));
    h_add(wd,dir);
}

void stop_watching(int wd) {
    h_remove(wd);
    inotify_rm_watch(INOTIFY,wd);
}

void watch_recursive(char *root, char *parent) {
    DIR *dir = opendir(root);
    struct dirent *entry;
    struct stat status;
    char path[PATH_MAX];

    if (!dir)
        SAYPX("opendir %s",root);

    start_watching(root);
    entry = readdir(dir);
    while (entry) {
        snprintf(path, sizeof(path), "%s/%s", root,entry->d_name);

        if (lstat(path, &status) != 0)
            SAYPX("stat");

        if (S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode) && entry->d_name[0] != '.')
            watch_recursive(path,root);

        entry = readdir(dir);
    }
    closedir(dir);
}


