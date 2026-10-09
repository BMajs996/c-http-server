#include "deployment.h"
#include "config.h"
#include "router.h"
#include "metrics.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    config_defaults();
    metrics_init(1024);
    struct sockaddr_storage address; socklen_t length;
    assert(deployment_address("127.0.0.1",8080,&address,&length)==0 && address.ss_family==AF_INET);
    assert(deployment_address("::1",8080,&address,&length)==0 && address.ss_family==AF_INET6);
    assert(deployment_address("localhost",8080,&address,&length)<0);
    assert(deployment_address("::1%lo",8080,&address,&length)<0);
    struct http_request request={0};
    strcpy(request.method,"GET"); strcpy(request.target,"/ready");
    char buffer[16384]; route_response response;
    for(int state=0;state<3;state++) {
        deployment_set_ready(state==1);
        assert(route_dispatch(&request,NULL,buffer,sizeof buffer,&response));
        assert(response.status==(state==1?200:503));
        assert(!strcmp(response.body,state==1?"{\"status\":\"ready\"}\n":"{\"status\":\"not_ready\"}\n"));
        assert(route_no_store(request.target));
        assert(metrics_render(buffer,sizeof buffer)>0);
        assert(strstr(buffer,state==1?"c_http_ready 1\n":"c_http_ready 0\n"));
    }
    strcpy(request.target,"/health");
    assert(route_dispatch(&request,NULL,buffer,sizeof buffer,&response));
    assert(response.status==200);
    puts("Deployment unit tests passed");
    return 0;
}
