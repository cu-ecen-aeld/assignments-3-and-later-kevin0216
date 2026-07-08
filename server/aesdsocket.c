#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define BUF_SIZE 1024

static volatile sig_atomic_t exit_requested = 0;

static void handle_signal(int signo)
{
    (void)signo;
    exit_requested = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return -1;
    }
    return 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t written = write(fd, buf, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += written;
        len -= (size_t)written;
    }
    return 0;
}

static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t sent = send(fd, buf, len, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += sent;
        len -= (size_t)sent;
    }
    return 0;
}

static int append_packet(const char *buf, size_t len)
{
    int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        syslog(LOG_ERR, "Could not open %s for append: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    if (write_all(fd, buf, len) != 0) {
        syslog(LOG_ERR, "Could not append to %s: %s", DATA_FILE, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int send_file(int connfd)
{
    char buf[BUF_SIZE];
    int fd = open(DATA_FILE, O_RDONLY);

    if (fd < 0) {
        syslog(LOG_ERR, "Could not open %s for read: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    for (;;) {
        ssize_t nread = read(fd, buf, sizeof(buf));
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "Could not read %s: %s", DATA_FILE, strerror(errno));
            close(fd);
            return -1;
        }
        if (nread == 0) {
            break;
        }
        if (send_all(connfd, buf, (size_t)nread) != 0) {
            syslog(LOG_ERR, "Could not send %s content: %s", DATA_FILE, strerror(errno));
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

static void client_ip_string(const struct sockaddr_storage *addr, char *out, size_t outlen)
{
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)addr;
        if (inet_ntop(AF_INET, &ipv4->sin_addr, out, outlen) != NULL) {
            return;
        }
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)addr;
        if (inet_ntop(AF_INET6, &ipv6->sin6_addr, out, outlen) != NULL) {
            return;
        }
    }

    strncpy(out, "unknown", outlen);
    out[outlen - 1] = '\0';
}

static void handle_connection(int connfd)
{
    char *packet = NULL;
    size_t packet_len = 0;
    char chunk[BUF_SIZE];

    while (!exit_requested) {
        char *grown;
        char *newline;
        ssize_t nrecv = recv(connfd, chunk, sizeof(chunk), 0);

        if (nrecv == 0) {
            break;
        }
        if (nrecv < 0) {
            if (errno == EINTR) {
                break;
            }
            syslog(LOG_ERR, "Could not receive from client: %s", strerror(errno));
            break;
        }

        grown = realloc(packet, packet_len + (size_t)nrecv);
        if (grown == NULL) {
            syslog(LOG_ERR, "Could not allocate %zu bytes, discarding packet",
                   packet_len + (size_t)nrecv);
            free(packet);
            packet = NULL;
            packet_len = 0;
            continue;
        }
        packet = grown;
        memcpy(packet + packet_len, chunk, (size_t)nrecv);
        packet_len += (size_t)nrecv;

        while ((newline = memchr(packet, '\n', packet_len)) != NULL) {
            size_t complete_len = (size_t)(newline - packet) + 1;

            if (append_packet(packet, complete_len) == 0) {
                send_file(connfd);
            }

            memmove(packet, packet + complete_len, packet_len - complete_len);
            packet_len -= complete_len;
        }
    }

    free(packet);
}

static int become_daemon(int sockfd)
{
    int devnull;
    pid_t pid = fork();

    if (pid == -1) {
        syslog(LOG_ERR, "Could not fork: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        close(sockfd);
        closelog();
        exit(EXIT_SUCCESS);
    }

    if (setsid() == -1) {
        syslog(LOG_ERR, "Could not setsid: %s", strerror(errno));
        return -1;
    }
    if (chdir("/") == -1) {
        syslog(LOG_ERR, "Could not chdir to /: %s", strerror(errno));
        return -1;
    }

    devnull = open("/dev/null", O_RDWR);
    if (devnull == -1) {
        syslog(LOG_ERR, "Could not open /dev/null: %s", strerror(errno));
        return -1;
    }
    if (dup2(devnull, STDIN_FILENO) == -1 ||
        dup2(devnull, STDOUT_FILENO) == -1 ||
        dup2(devnull, STDERR_FILENO) == -1) {
        syslog(LOG_ERR, "Could not redirect standard streams: %s", strerror(errno));
        close(devnull);
        return -1;
    }
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    struct addrinfo hints;
    struct addrinfo *servinfo = NULL;
    int sockfd;
    int yes = 1;
    int rc;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return -1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (install_signal_handlers() != 0) {
        syslog(LOG_ERR, "Could not install signal handlers: %s", strerror(errno));
        closelog();
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(NULL, PORT, &hints, &servinfo);
    if (rc != 0) {
        syslog(LOG_ERR, "Could not resolve bind address: %s", gai_strerror(rc));
        closelog();
        return -1;
    }

    sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (sockfd == -1) {
        syslog(LOG_ERR, "Could not create socket: %s", strerror(errno));
        freeaddrinfo(servinfo);
        closelog();
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        syslog(LOG_ERR, "Could not set SO_REUSEADDR: %s", strerror(errno));
        close(sockfd);
        freeaddrinfo(servinfo);
        closelog();
        return -1;
    }

    if (bind(sockfd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        syslog(LOG_ERR, "Could not bind to port %s: %s", PORT, strerror(errno));
        close(sockfd);
        freeaddrinfo(servinfo);
        closelog();
        return -1;
    }

    freeaddrinfo(servinfo);

    if (listen(sockfd, BACKLOG) == -1) {
        syslog(LOG_ERR, "Could not listen on port %s: %s", PORT, strerror(errno));
        close(sockfd);
        closelog();
        return -1;
    }

    if (daemon_mode && become_daemon(sockfd) != 0) {
        close(sockfd);
        closelog();
        return -1;
    }

    while (!exit_requested) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        char client_ip[INET6_ADDRSTRLEN];
        int connfd = accept(sockfd, (struct sockaddr *)&client_addr, &addr_len);

        if (connfd == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "Could not accept connection: %s", strerror(errno));
            continue;
        }

        client_ip_string(&client_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        handle_connection(connfd);

        close(connfd);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    syslog(LOG_INFO, "Caught signal, exiting");

    close(sockfd);
    if (unlink(DATA_FILE) == -1 && errno != ENOENT) {
        syslog(LOG_ERR, "Could not remove %s: %s", DATA_FILE, strerror(errno));
    }
    closelog();

    return 0;
}
