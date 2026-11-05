/* Local Easy Netplay compatibility proof. No files or game data are served. */
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROOF_LENGTH 64U

static int proof_valid(const char *proof)
{
    return strlen(proof) == PROOF_LENGTH && strspn(proof, "0123456789abcdef") == PROOF_LENGTH;
}

static int listen_proof(int fd, const struct sockaddr_in *address, const char *proof)
{
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
        bind(fd, (const struct sockaddr *)address, sizeof(*address)) != 0 || listen(fd, 4) != 0)
        return 1;
    for (;;) {
        int peer = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (peer < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return 1;
        }
        (void)send(peer, proof, PROOF_LENGTH, MSG_NOSIGNAL);
        close(peer);
    }
}

static int query_proof(int fd, const struct sockaddr_in *address)
{
    struct pollfd pending = {.fd = fd, .events = POLLOUT};
    int error = 0;
    socklen_t length = sizeof(error);
    if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) != 0 && errno != EINPROGRESS)
        return 1;
    if (poll(&pending, 1, 1000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
        return 1;
    pending.events = POLLIN;
    char proof[PROOF_LENGTH + 2U];
    size_t used = 0;
    /* Bound both input length and the number of waits on a partial reply. */
    for (unsigned int reads = 0; reads < 3U; ++reads) {
        if (poll(&pending, 1, 400) != 1) return 1;
        ssize_t got = recv(fd, proof + used, sizeof(proof) - 1U - used, 0);
        if (got < 0) return 1;
        if (got == 0) {
            proof[used] = '\0';
            return used == PROOF_LENGTH && proof_valid(proof) && puts(proof) >= 0 ? 0 : 1;
        }
        used += (size_t)got;
        if (used > PROOF_LENGTH) return 1;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) return 2;
    int serve = argc == 5 && strcmp(argv[1], "listen") == 0 && proof_valid(argv[4]);
    if (!serve && (argc != 4 || strcmp(argv[1], "query") != 0)) return 2;
    struct sockaddr_in address = {.sin_family = AF_INET};
    char *end;
    unsigned long port = strtoul(argv[3], &end, 10);
    if (*argv[3] == '\0' || *end != '\0' || port < 1024U || port > 65535U ||
        inet_pton(AF_INET, argv[2], &address.sin_addr) != 1 || address.sin_addr.s_addr == INADDR_ANY)
        return 2;
    address.sin_port = htons((unsigned short)port);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | (serve ? 0 : SOCK_NONBLOCK), 0);
    if (fd < 0) return 1;
    int status = serve ? listen_proof(fd, &address, argv[4]) : query_proof(fd, &address);
    close(fd);
    return status;
}
