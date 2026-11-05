#ifndef SC_FILE_SERVICE_H
#define SC_FILE_SERVICE_H
#include "launcher/tools.h"
#include <sys/types.h>
struct sc_file_service { pid_t pid; int fd; bool term, ready, importing, imported, pc; char token[33], failure[161]; };
void sc_file_service_stop(struct sc_file_service *service);
void sc_file_service_reaped(struct sc_file_service *service,bool success);
void sc_file_service_request(struct sc_file_service *service, const char *card,
    const struct cl_tool_request *request, struct cl_tool_snapshot *snapshot);
#endif
