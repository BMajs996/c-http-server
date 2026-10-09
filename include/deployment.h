#ifndef DEPLOYMENT_H
#define DEPLOYMENT_H
#include <sys/socket.h>
/* Numeric IPv4/IPv6 only. No DNS lookup or scoped interface addresses. */
int deployment_address(const char *host,unsigned port,struct sockaddr_storage *address,socklen_t *length);
/* Reactor-owned readiness; initialization defaults to unready. */
void deployment_set_ready(int ready);
int deployment_ready(void);
/* Optional nonblocking systemd notification; 0 success/disabled, -1 failure. */
int deployment_notify(const char *message);
/* Validate startup inputs without a listener, threads, or creating log files. */
int deployment_check_config(void);
#endif
