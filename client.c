/*
 *   stunnel       Universal SSL tunnel
 *   Copyright (c) 1998-2001 Michal Trojnara <Michal.Trojnara@mirt.net>
 *                 All Rights Reserved
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* Non-blocking sockets are disabled by default */
/* #define USE_NBIO */

/* I/O buffer size */
#define BUFFSIZE 16384

/* Undefine if you have problems with make_sockets() */
#define INET_SOCKET_PAIR

#include "common.h"
#include "prototypes.h"
#include "client.h"

#ifndef SHUT_RD
#define SHUT_RD 0
#endif
#ifndef SHUT_WR
#define SHUT_WR 1
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif

/* TCP wrapper */
#ifdef USE_LIBWRAP
#include <tcpd.h>
int allow_severity=LOG_NOTICE;
int deny_severity=LOG_WARNING;
#endif

#if SSLEAY_VERSION_NUMBER >= 0x0922
static unsigned char *sid_ctx=(unsigned char *)"stunnel SID";
    /* const allowed here */
#endif

extern SSL_CTX *ctx; /* global SSL context defined in ssl.c */
extern server_options options;

    /* SSL functions */
static void init_local(CLI *);
static void init_remote(CLI *);
static void init_ssl(CLI *);
static void transfer(CLI *);
static void nbio(CLI *, unsigned long);
static void cleanup_ssl(CLI *);
static void cleanup_remote(CLI *);
static void cleanup_local(CLI *);

static void print_cipher(CLI *);
static int auth_libwrap(CLI *);
static int auth_user(CLI *);
static int connect_local(CLI *c);
#ifndef USE_WIN32
static void wait_local(CLI *c);
static int make_sockets(int [2]);
#endif
static int connect_remote(CLI *c);
static int waitforsocket(int, int);
static void reset(int, int, char *);

FD d[MAX_FD];

void *client(void *local) {
    CLI *c;

    c=calloc(1, sizeof(CLI));
    if(!c) {
        log(LOG_ERR, "malloc failed");
        closesocket((int)local);
        return NULL;
    }
    if((int)local==STDIO_FILENO) { /* Read from STDIN, write to STDOUT */
        c->local_rfd=0;
        c->local_wfd=1;
    } else
        c->local_rfd=c->local_wfd=(int)local;
    c->error=0;
    init_local(c);
    if(!c->error) {
        init_remote(c);
        if(!c->error) {
            init_ssl(c);
            if(!c->error)
                nbio(c, 1);
                transfer(c);
                nbio(c, 0);
                log(LOG_NOTICE,
                    "Connection %s: %d bytes sent to SSL, %d bytes sent to socket",
                     c->error ? "reset" : "closed", c->ssl_bytes, c->sock_bytes);
                cleanup_ssl(c);
        }
        cleanup_remote(c);
    }
    cleanup_local(c);
#ifndef USE_WIN32
    if(!(options.option&OPT_REMOTE))
        wait_local(c);
#endif /* USE_WIN32 */
    free(c);
#ifndef USE_FORK
    enter_critical_section(CRIT_CLIENTS); /* for multi-cpu machines */
    log(LOG_DEBUG, "%s finished (%d left)", options.servname,
        --options.clients);
    leave_critical_section(CRIT_CLIENTS);
#endif
    return NULL;
}

static void init_local(CLI *c) {
    int addrlen;

    log(LOG_DEBUG, "%s started", options.servname);
    addrlen=sizeof(c->addr);

    if(getpeername(c->local_rfd, (struct sockaddr *)&c->addr, &addrlen)<0) {
        d[c->local_rfd].is_socket=0;
        d[c->local_wfd].is_socket=0; /* TODO: It's not always true */
        if(options.option&OPT_TRANSPARENT || get_last_socket_error()!=ENOTSOCK) {
            sockerror("getpeerbyname");
            c->error=1;
            return;
        }
        /* Ignore ENOTSOCK error so 'local' doesn't have to be a socket */
    } else {
        d[c->local_rfd].is_socket=1;
        d[c->local_wfd].is_socket=1; /* TODO: It's not always true */
        /* It's a socket - lets setup options */
        if(set_socket_options(c->local_rfd, 1)<0) {
            c->error=1;
            return;
        }
        if(auth_libwrap(c)<0) {
            c->error=1;
            return;
        }
        if(auth_user(c)<0) {
            enter_critical_section(CRIT_NTOA); /* inet_ntoa is not mt-safe */
            log(LOG_WARNING, "Connection from %s:%d REFUSED by IDENT",
                inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));
            leave_critical_section(CRIT_NTOA);
            c->error=1;
            return;
        }
        enter_critical_section(CRIT_NTOA); /* inet_ntoa is not mt-safe */
        log(LOG_NOTICE, "%s connected from %s:%d", options.servname,
            inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));
        leave_critical_section(CRIT_NTOA);
    }

    /* create connection to host/service */
    if(options.local_ip)
        c->ip=*options.local_ip;
    else if(options.option&OPT_TRANSPARENT)
        c->ip=c->addr.sin_addr.s_addr;
    else
        c->ip=0;
    /* Setup c->remote_fd, now */
    if(options.option&OPT_REMOTE) { /* remote host */
        if((c->remote_fd=connect_remote(c))<0) {
            c->error=1; /* Failed to connect remote server */
            return;
        }
        log(LOG_DEBUG, "Remote host connected");
        d[c->remote_fd].is_socket=1;
    } else { /* local service */
        if((c->remote_fd=connect_local(c))<0) {
            c->error=1; /* Failed to spawn local service */
            return;
        }
        d[c->remote_fd].is_socket=1;
        log(LOG_DEBUG, "Local service connected");
    }
}

static void init_remote(CLI *c) {
    if(set_socket_options(c->remote_fd, 2)<0) {
        c->error=1;
        return;
    }

    /* negotiate protocol */
    if(negotiate(options.protocol, options.option&OPT_CLIENT,
            c->local_rfd, c->local_wfd, c->remote_fd) <0) {
        log(LOG_ERR, "Protocol negotiations failed");
        c->error=1;
        return;
    }

    /* do the job */
    if(!(c->ssl=SSL_new(ctx))) {
        sslerror("SSL_new");
        c->error=1;
        return;
    }
#if SSLEAY_VERSION_NUMBER >= 0x0922
    SSL_set_session_id_context(c->ssl, sid_ctx, strlen(sid_ctx));
#endif
    if(options.option&OPT_CLIENT) {
        c->sock_rfd=c->local_rfd;
        c->sock_wfd=c->local_wfd;
        c->ssl_rfd=c->ssl_wfd=c->remote_fd;
        /* Attempt to use the most recent id in the session cache */
        if(ctx->session_cache_head)
            if(!SSL_set_session(c->ssl, ctx->session_cache_head))
                log(LOG_WARNING, "Cannot set SSL session id to most recent used");
        SSL_set_fd(c->ssl, c->remote_fd);
        SSL_set_connect_state(c->ssl);
    } else {
        c->sock_rfd=c->sock_wfd=c->remote_fd;
        c->ssl_rfd=c->local_rfd;
        c->ssl_wfd=c->local_wfd;
        if(c->local_rfd==c->local_wfd)
            SSL_set_fd(c->ssl, c->local_rfd);
        else {
           /* Does it make sence to have SSL on STDIN/STDOUT? */
            SSL_set_rfd(c->ssl, c->local_rfd);
            SSL_set_wfd(c->ssl, c->local_wfd);
        }
        SSL_set_accept_state(c->ssl);
    }
}

static void init_ssl(CLI *c) {
    if(options.option&OPT_CLIENT) {
        if(SSL_connect(c->ssl)<=0) {
            sslerror("SSL_connect");
            c->error=1;
            return;
        }
    } else {
        if(SSL_accept(c->ssl)<=0) {
            sslerror("SSL_accept");
            c->error=1;
            return;
        }
    }
    print_cipher(c);
}

static void transfer(CLI *c) { /* transfer data */
    fd_set rd_set, wr_set;
    int num, fdno;
    int check_SSL_pending;
    int ready;
    struct timeval tv;

    fdno=c->sock_rfd;
    if(c->sock_wfd>fdno) fdno=c->sock_wfd;
    if(c->ssl_rfd>fdno) fdno=c->ssl_rfd;
    if(c->ssl_wfd>fdno) fdno=c->ssl_wfd;
    fdno+=1;

    c->sock_ptr=c->ssl_ptr=0;
    sock_rd=sock_wr=ssl_rd=ssl_wr=1;
    c->sock_bytes=c->ssl_bytes=0;

    while(((sock_rd||c->sock_ptr)&&ssl_wr)||((ssl_rd||c->ssl_ptr)&&sock_wr)) {

        FD_ZERO(&rd_set); /* Setup rd_set */
        if(sock_rd && c->sock_ptr<BUFFSIZE) /* socket input buffer not full*/
            FD_SET(c->sock_rfd, &rd_set);
        if(ssl_rd && (c->ssl_ptr<BUFFSIZE || /* SSL input buffer not full */
                (c->sock_ptr && SSL_want_read(c->ssl))
                /* I want to SSL_write but read from the underlying */
                /* socket needed for the SSL protocol */
                )) {
            FD_SET(c->ssl_rfd, &rd_set);
        }

        FD_ZERO(&wr_set); /* Setup wr_set */
        if(sock_wr && c->ssl_ptr) /* SSL input buffer not empty */
            FD_SET(c->sock_wfd, &wr_set);
        if (ssl_wr && (c->sock_ptr || /* socket input buffer not empty */
                (c->ssl_ptr<BUFFSIZE && SSL_want_write(c->ssl))
                /* I want to SSL_read but write to the underlying */
                /* socket needed for the SSL protocol */
                )) {
            FD_SET(c->ssl_wfd, &wr_set);
        }

        /* socket open for read -> set timeout to 1 hour */
        /* socket closed for read -> set timeout to 10 seconds */
        tv.tv_sec=sock_rd ? 3600 : 10;
        tv.tv_usec=0;

        do { /* Skip "Interrupted system call" errors */
            ready=select(fdno, &rd_set, &wr_set, NULL, &tv);
        } while(ready<0 && get_last_socket_error()==EINTR);
        if(ready<0) { /* Break the connection for others */
            sockerror("select");
            c->error=1;
            return;
        }
        if(!ready) { /* Timeout */
            if(sock_rd) { /* No traffic for a long time */
                log(LOG_DEBUG, "select timeout - connection reset");
                c->error=1;
                return;
            } else { /* Timeout waiting for SSL close_notify */
                log(LOG_DEBUG, "select timeout waiting for SSL close_notify");
                break; /* Leave the while() loop */
            }
        }

        /* Set flag to try and read any buffered SSL data if we made */
        /* room in the buffer by writing to the socket */
        check_SSL_pending = 0;

        if(sock_wr && FD_ISSET(c->sock_wfd, &wr_set)) {
            num=writesocket(c->sock_wfd, c->ssl_buff, c->ssl_ptr);
            if(num<0) {
                sockerror("write");
                c->error=1;
                return;
            }
            if(num) {
                memcpy(c->ssl_buff, c->ssl_buff+num, c->ssl_ptr-num);
                if(c->ssl_ptr==BUFFSIZE)
                    check_SSL_pending=1;
                c->ssl_ptr-=num;
                c->sock_bytes+=num;
                if(!ssl_rd && !c->ssl_ptr) {
                    shutdown(c->sock_wfd, SHUT_WR);
                    log(LOG_DEBUG,
                        "Socket write shutdown (no more data to send)");
                    sock_wr=0;
                }
            }
        }

        if(ssl_wr && ( /* SSL sockets are still open */
                (c->sock_ptr && FD_ISSET(c->ssl_wfd, &wr_set)) ||
                /* See if application data can be written */
                (SSL_want_read(c->ssl) && FD_ISSET(c->ssl_rfd, &rd_set))
                /* I want to SSL_write but read from the underlying */
                /* socket needed for the SSL protocol */
                )) {
            num=SSL_write(c->ssl, c->sock_buff, c->sock_ptr);

            switch(SSL_get_error(c->ssl, num)) {
            case SSL_ERROR_NONE:
                memcpy(c->sock_buff, c->sock_buff+num, c->sock_ptr-num);
                c->sock_ptr-=num;
                c->ssl_bytes+=num;
                if(!sock_rd && !c->sock_ptr && ssl_wr) {
                    SSL_shutdown(c->ssl); /* Send close_notify */
                    log(LOG_DEBUG,
                        "SSL write shutdown (no more data to send)");
                    ssl_wr=0;
                }
                break;
            case SSL_ERROR_WANT_WRITE:
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_X509_LOOKUP:
                log(LOG_DEBUG, "SSL_write returned WANT_ - retry");
                break;
            case SSL_ERROR_SYSCALL:
                if(num<0) { /* not EOF */
                    sockerror("SSL_write (ERROR_SYSCALL)");
                    c->error=1;
                    return;
                }
                log(LOG_DEBUG, "SSL socket closed on SSL_write");
                ssl_rd=ssl_wr=0;
                break;
            case SSL_ERROR_ZERO_RETURN: /* close_notify received */
                log(LOG_DEBUG, "SSL closed on SSL_write");
                ssl_rd=ssl_wr=0;
                break;
            case SSL_ERROR_SSL:
                sslerror("SSL_write");
                c->error=1;
                return;
            }
        }

        if(sock_rd && FD_ISSET(c->sock_rfd, &rd_set)) {
            num=readsocket(c->sock_rfd, c->sock_buff+c->sock_ptr, BUFFSIZE-c->sock_ptr);

            if(num<0 && get_last_socket_error()==ECONNRESET) {
                log(LOG_NOTICE, "IPC reset (child died)");
                break; /* close connection */
            }
            if (num<0 && get_last_socket_error()!=EIO) {
                sockerror("read");
                c->error=1;
                return;
            } else if (num>0) {
                c->sock_ptr += num;
            } else { /* close */
                log(LOG_DEBUG, "Socket closed on read");
                sock_rd=0;
                if(!c->sock_ptr && ssl_wr) {
                    SSL_shutdown(c->ssl); /* Send close_notify */
                    log(LOG_DEBUG,
                        "SSL write shutdown (output buffer empty)");
                    ssl_wr=0;
                }
            }
        }

        if(ssl_rd && ( /* SSL sockets are still open */
                (c->ssl_ptr<BUFFSIZE && FD_ISSET(c->ssl_rfd, &rd_set)) ||
                /* See if there's any application data coming in */
                (SSL_want_write(c->ssl) && FD_ISSET(c->ssl_wfd, &wr_set)) ||
                /* I want to SSL_read but write to the underlying */
                /* socket needed for the SSL protocol */
                (check_SSL_pending && SSL_pending(c->ssl))
                /* Write made space from full buffer */
                )) {
            num=SSL_read(c->ssl, c->ssl_buff+c->ssl_ptr, BUFFSIZE-c->ssl_ptr);

            switch(SSL_get_error(c->ssl, num)) {
            case SSL_ERROR_NONE:
                c->ssl_ptr+=num;
                break;
            case SSL_ERROR_WANT_WRITE:
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_X509_LOOKUP:
                log(LOG_DEBUG, "SSL_read returned WANT_ - retry");
                break;
            case SSL_ERROR_SYSCALL:
                if(num<0) { /* not EOF */
                    sockerror("SSL_read (SSL_ERROR_SYSCALL)");
                    c->error=1;
                    return;
                }
                log(LOG_DEBUG, "SSL socket closed on SSL_read");
                ssl_rd=ssl_wr=0;
                break;
            case SSL_ERROR_ZERO_RETURN: /* close_notify received */
                log(LOG_DEBUG, "SSL closed on SSL_read");
                ssl_rd=0;
                if(!c->sock_ptr && ssl_wr) {
                    SSL_shutdown(c->ssl); /* Send close_notify back */
                    log(LOG_DEBUG,
                        "SSL write shutdown (output buffer empty)");
                    ssl_wr=0;
                }
                if(!c->ssl_ptr && sock_wr) {
                    shutdown(c->sock_wfd, SHUT_WR);
                    log(LOG_DEBUG,
                        "Socket write shutdown (output buffer empty)");
                    sock_wr=0;
                }
                break;
            case SSL_ERROR_SSL:
                sslerror("SSL_read");
                c->error=1;
                return;
            }
        }
    }
}

static void nbio(CLI *c, unsigned long l) {
#if defined FIONBIO && defined USE_NBIO
    log(LOG_DEBUG, "Seting non-blocking mode %s", l ? "on" : "off");
    if(sock_rd && ioctlsocket(c->sock_rfd, FIONBIO, &l)<0)
        sockerror("ioctlsocket (c->sock_rfd)"); /* non-critical */
    if(sock_wr && c->sock_wfd!=c->sock_rfd && ioctlsocket(c->sock_wfd, FIONBIO, &l)<0)
        sockerror("ioctlsocket (c->sock_wfd)"); /* non-critical */
    if(ssl_rd && ioctlsocket(c->ssl_rfd, FIONBIO, &l)<0)
        sockerror("ioctlsocket (c->ssl_rfd)"); /* non-critical */
    if(ssl_wr && c->ssl_wfd!=c->ssl_rfd && ioctlsocket(c->ssl_wfd, FIONBIO, &l)<0)
        sockerror("ioctlsocket (c->ssl_wfd)"); /* non-critical */
#endif
}

static void cleanup_ssl(CLI *c) {
    SSL_set_shutdown(c->ssl, SSL_SENT_SHUTDOWN|SSL_RECEIVED_SHUTDOWN);
    SSL_free(c->ssl);
    ERR_remove_state(0);
    return;
}

static void cleanup_remote(CLI *c) {
    reset(c->error, c->remote_fd, "linger (remote_fd)");
    closesocket(c->remote_fd);
}

static void cleanup_local(CLI *c) {
    if(c->local_rfd==c->local_wfd) {
        reset(c->error, c->local_rfd, "linger (local)");
        closesocket(c->local_rfd);
    } else {
        reset(c->error, c->local_rfd, "linger (local_rfd)");
        reset(c->error, c->local_wfd, "linger (local_wfd)");
    }
}

static void print_cipher(CLI *c) { /* print negotiated cipher */
#if SSLEAY_VERSION_NUMBER > 0x0800
    SSL_CIPHER *cipher;
    char *ver;
    int bits;
#endif

#if SSLEAY_VERSION_NUMBER <= 0x0800
    log(LOG_INFO, "%s opened with SSLv%d, cipher %s",
        options.servname, ssl->session->ssl_version, SSL_get_cipher(c->ssl));
#else
    switch(c->ssl->session->ssl_version) {
    case SSL2_VERSION:
        ver="SSLv2"; break;
    case SSL3_VERSION:
        ver="SSLv3"; break;
    case TLS1_VERSION:
        ver="TLSv1"; break;
    default:
        ver="UNKNOWN";
    }
    cipher=SSL_get_current_cipher(c->ssl);
    SSL_CIPHER_get_bits(cipher, &bits);
    log(LOG_INFO, "%s opened with %s, cipher %s (%u bits)",
        options.servname, ver, SSL_CIPHER_get_name(cipher), bits);
#endif
}

static int auth_libwrap(CLI *c) {
#ifdef USE_LIBWRAP
    struct request_info request;
    int result;

    enter_critical_section(CRIT_LIBWRAP); /* libwrap is not mt-safe */
    request_init(&request, RQ_DAEMON, options.servname, RQ_FILE, c->local_rfd, 0);
    fromhost(&request);
    result=hosts_access(&request);
    leave_critical_section(CRIT_LIBWRAP);
    if (!result) {
        enter_critical_section(CRIT_NTOA); /* inet_ntoa is not mt-safe */
        log(LOG_WARNING, "Connection from %s:%d REFUSED by libwrap",
            inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));
        leave_critical_section(CRIT_NTOA);
        log(LOG_DEBUG, "See hosts_access(5) for details");
        return -1; /* FAILED */
    }
#endif
    return 0; /* OK */
}

static int auth_user(CLI *c) {
    struct servent *s_ent;    /* structure for getservbyname */
    struct sockaddr_in ident; /* IDENT socket name */
    int fd;                   /* IDENT socket descriptor */
    char name[STRLEN];
    int retval;

    if(!options.username)
        return 0; /* -u option not specified */
    if((fd=socket(AF_INET, SOCK_STREAM, 0))<0) {
        sockerror("socket (auth_user)");
        return -1;
    }
#if defined FIONBIO && defined USE_NBIO
    {
        unsigned long l=1; /* ON */
        if(ioctlsocket(fd, FIONBIO, &l)<0)
            sockerror("ioctlsocket(FIONBIO)"); /* non-critical */
    }
#endif
    memcpy(&ident, &c->addr, sizeof(ident));
    s_ent=getservbyname("auth", "tcp");
    if(!s_ent) {
        log(LOG_WARNING, "Unknown service 'auth' - using default 113");
        ident.sin_port=htons(113);
    } else {
        ident.sin_port=s_ent->s_port;
    }
    if(connect(fd, (struct sockaddr *)&ident, sizeof(ident))<0) {
        if(get_last_socket_error()==EINPROGRESS) {
            switch(waitforsocket(fd, 1 /* write */)) {
            case -1: /* Error */
                sockerror("select");
                return -1;
            case 0: /* Timeout */
                log(LOG_ERR, "Select timeout (auth_user)");
                return -1;
            }
            if(connect(fd, (struct sockaddr *)&ident, sizeof(ident))<0) {
                sockerror("connect#2 (auth_user))");
                closesocket(fd);
                return -1;
            }
            log(LOG_DEBUG, "IDENT server connected (#2)");
        } else {
            sockerror("connect#1 (auth_user)");
            closesocket(fd);
            return -1;
        }
    } else
        log(LOG_DEBUG, "IDENT server connected (#1)");
    if(fdprintf(fd, "%u , %u",
            ntohs(c->addr.sin_port), ntohs(options.localport))<0) {
        sockerror("fdprintf (auth_user)");
        closesocket(fd);
        return -1;
    }
    if(fdscanf(fd, "%*[^:]: USERID :%*[^:]:%s", name)!=1) {
        log(LOG_ERR, "Incorrect data from IDENT server");
        closesocket(fd);
        return -1;
    }
    closesocket(fd);
    retval=strcmp(name, options.username) ? -1 : 0;
    safestring(name);
    log(LOG_INFO, "IDENT resolved remote user to %s", name);
    return retval;
}

static int connect_local(CLI *c) { /* spawn local process */
#ifdef USE_WIN32
    log(LOG_ERR, "LOCAL MODE NOT SUPPORTED ON WIN32 PLATFORM");
    return -1;
#else
    struct in_addr addr;
    char text[STRLEN];
    int fd[2];

    if (options.option & OPT_PTY) {
        char tty[STRLEN];

        if(pty_allocate(fd, fd+1, tty, STRLEN)) {
            return -1;
        }
        log(LOG_DEBUG, "%s allocated", tty);
    } else {
        if(make_sockets(fd))
            return -1;
    }
    switch(c->pid=(unsigned long)fork()) {
    case -1:    /* error */
        closesocket(fd[0]);
        closesocket(fd[1]);
        ioerror("fork");
        return -1;
    case  0:    /* child */
        closesocket(fd[0]);
        dup2(fd[1], 0);
        dup2(fd[1], 1);
        if (!options.foreground)
            dup2(fd[1], 2);
        closesocket(fd[1]);
        if (c->ip) {
            putenv("LD_PRELOAD=" libdir "/stunnel.so");
            /* For Tru64 _RLD_LIST is used instead */
            putenv("_RLD_LIST=" libdir "/stunnel.so:DEFAULT");
            addr.s_addr = c->ip;
            safecopy(text, "REMOTE_HOST=");
            enter_critical_section(CRIT_NTOA); /* inet_ntoa is not mt-safe */
            safeconcat(text, inet_ntoa(addr));
            leave_critical_section(CRIT_NTOA);
            putenv(text);
        }
        execvp(options.execname, options.execargs);
        ioerror(options.execname); /* execv failed */
        _exit(1);
    }
    /* parent */
    log(LOG_INFO, "Local mode child started (PID=%lu)", c->pid);
    closesocket(fd[1]);
#ifdef FD_CLOEXEC
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
#endif
    return fd[0];
#endif /* USE_WIN32 */
}

#ifndef USE_WIN32

static void wait_local(CLI *c) {
    int status;

#ifdef HAVE_WAITPID
    switch(waitpid(c->pid, &status, WNOHANG)) {
    case -1: /* Error */
        if(get_last_socket_error()==ECHILD)
            return; /* No zombie created */
        ioerror("waitpid#1");
        return;
    case 0: /* Child is still alive */
        log(LOG_DEBUG, "Killing local mode child (PID=%lu)", c->pid);
        if(kill(c->pid, 9)<0) {
            ioerror("kill");
            return;
        }
        if(waitpid(c->pid, &status, 0)<0) {
            ioerror("waitpid#2");
            return;
        }
    default: /* Child is dead */
        break;
    }
#else /* HAVE WAITPID */
    log(LOG_DEBUG, "Killing local mode child (PID=%lu)", c->pid);
    if(kill(c->pid, 9)<0) {
        ioerror("kill");
        return;
    }
    if(wait(&status)<0) {
        ioerror("wait");
        return;
    }
#endif /* HAVE_WAITPID */
#ifdef WIFSIGNALED
    if(WIFSIGNALED(status)) {
        log(LOG_DEBUG, "Local process %s (PID=%lu) terminated on signal %d",
            options.servname, c->pid, WTERMSIG(status));
    } else {
        log(LOG_DEBUG, "Local process %s (PID=%lu) finished with code %d",
            options.servname, c->pid, WEXITSTATUS(status));
    }
#else
    log(LOG_DEBUG, "Local process %s (PID=%lu) finished with status %d",
        options.servname, c->pid, status);
#endif
}

static int make_sockets(int fd[2]) { /* make pair of connected sockets */
#ifdef INET_SOCKET_PAIR
    struct sockaddr_in addr;
    int addrlen;
    int s; /* temporary socket awaiting for connection */

    if((s=socket(AF_INET, SOCK_STREAM, 0))<0) {
        sockerror("socket#1");
        return -1;
    }
    if((fd[1]=socket(AF_INET, SOCK_STREAM, 0))<0) {
        sockerror("socket#2");
        return -1;
    }
    addrlen=sizeof(addr);
    memset(&addr, 0, addrlen);
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    addr.sin_port=0; /* dynamic port allocation */
    if(bind(s, (struct sockaddr *)&addr, addrlen))
        log(LOG_DEBUG, "bind#1: %s (%d)",
            strerror(get_last_socket_error()), get_last_socket_error());
    if(bind(fd[1], (struct sockaddr *)&addr, addrlen))
        log(LOG_DEBUG, "bind#2: %s (%d)",
            strerror(get_last_socket_error()), get_last_socket_error());
    if(listen(s, 5)) {
        sockerror("listen");
        return -1;
    }
    if(getsockname(s, (struct sockaddr *)&addr, &addrlen)) {
        sockerror("getsockname");
        return -1;
    }
    if(connect(fd[1], (struct sockaddr *)&addr, addrlen)) {
        sockerror("connect");
        return -1;
    }
    if((fd[0]=accept(s, (struct sockaddr *)&addr, &addrlen))<0) {
        sockerror("accept");
        return -1;
    }
    closesocket(s); /* don't care about the result */
#else
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, fd)) {
        sockerror("socketpair");
        return -1;
    }
#endif
    return 0;
}
#endif

static int connect_remote(CLI *c) { /* connect to remote host */
    struct sockaddr_in addr;
    int s; /* destination socket */
    u32 *list; /* destination addresses list */

    if((s=socket(AF_INET, SOCK_STREAM, 0))<0) {
        sockerror("remote socket");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family=AF_INET;

    if(c->ip) { /* transparent proxy */
        addr.sin_addr.s_addr=c->ip;
        addr.sin_port=htons(0);
        if(bind(s, (struct sockaddr *)&addr, sizeof(addr))<0) {
            sockerror("bind transparent");
            return -1;
        }
    }

    addr.sin_port=options.remoteport;

    /* connect each host from the list */
    for(list=options.remotenames; *list!=-1; list++) {
        addr.sin_addr.s_addr=*list;
        enter_critical_section(CRIT_NTOA); /* inet_ntoa is not mt-safe */
        log(LOG_DEBUG, "%s connecting %s:%d", options.servname,
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        leave_critical_section(CRIT_NTOA);
        if(!connect(s, (struct sockaddr *) &addr, sizeof(addr)))
            return s; /* success */
    }
    sockerror("remote connect");
    return -1;
}

int fdprintf(int fd, char *format, ...) {
    va_list arglist;
    char line[STRLEN], logline[STRLEN];
    char crlf[]="\r\n";
    int len, ptr, written, towrite;

    va_start(arglist, format);
#ifdef HAVE_VSNPRINTF
    len=vsnprintf(line, STRLEN, format, arglist);
#else
    len=vsprintf(line, format, arglist);
#endif
    va_end(arglist);
    safeconcat(line, crlf);
    len+=2;
    for(ptr=0, towrite=len; towrite>0; ptr+=written, towrite-=written) {
        switch(waitforsocket(fd, 1 /* write */)) {
        case -1: /* Error */
            sockerror("select");
            return -1;
        case 0: /* Timeout */
            log(LOG_ERR, "Select timeout (fdprintf)");
            return -1;
        }
        written=writesocket(fd, line+ptr, towrite);
        if(written<0) {
            sockerror("writesocket (fdprintf)");
            return -1;
        }
    }
    safecopy(logline, line);
    safestring(logline);
    log(LOG_DEBUG, " -> %s", logline);
    return len;
}

int fdscanf(int fd, char *format, char *buffer) {
    char line[STRLEN], logline[STRLEN];
    int ptr;

    for(ptr=0; ptr<STRLEN-1; ptr++) {
        switch(waitforsocket(fd, 0 /* read */)) {
        case -1: /* Error */
            sockerror("select");
            return -1;
        case 0: /* Timeout */
            log(LOG_ERR, "Select timeout (fdscanf)");
            return -1;
        }
        switch(readsocket(fd, line+ptr, 1)) {
        case -1: /* error */
            sockerror("readsocket (fdscanf)");
            return -1;
        case 0: /* EOF */
            log(LOG_ERR, "Unexpected socket close (fdscanf)");
            return -1;
        }
        if(line[ptr]=='\r')
            continue;
        if(line[ptr]=='\n')
            break;
    }
    line[ptr]='\0';
    safecopy(logline, line);
    safestring(logline);
    log(LOG_DEBUG, " <- %s", logline);
    return sscanf(line, format, buffer);
}

static int waitforsocket(int fd, int dir) {
    /* dir: 0 for read, 1 for write */
    struct timeval tv;
    fd_set set;
    int ready;

    tv.tv_sec=60; /* One minute */
    tv.tv_usec=0;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    do { /* Skip "Interrupted system call" errors */
        ready=select(fd+1, dir ? NULL : &set, dir ? &set : NULL, NULL, &tv);
    } while(ready<0 && get_last_socket_error()==EINTR);
    return ready;
}

static void reset(int err, int fd, char *txt) { /* Set lingering on a socket */
    struct linger l;

    if(!err || !d[fd].is_socket)
        return; /* No need to set lingering option */
    l.l_onoff=1;
    l.l_linger=0;
    if(setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&l, sizeof(l)))
        sockerror(txt);
}

/* End of client.c */