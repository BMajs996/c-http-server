#include "http_response.h"
#include "http_syntax.h"
#include <string.h>
#include <strings.h>
#include <stdint.h>
const char *http_field_name(const struct http_fields *f,size_t i){return f->text+f->fields[i].name;}
const char *http_field_value(const struct http_fields *f,size_t i){return f->text+f->fields[i].value;}
int http_fields_parse(struct http_fields *f,const char *data,size_t length) {
    if(length<4 || length>HEADER_LIMIT || memchr(data,0,length) || memcmp(data+length-4,"\r\n\r\n",4))return -1;
    memset(f,0,sizeof *f);memcpy(f->text,data,length);
    char *end=strstr(f->text,"\r\n");if(!end)return -1;
    for(char *p=f->text;p<end;p++)if((unsigned char)*p<32 || (unsigned char)*p==127)return -1;
    *end=0;char *line=end+2;
    while(*line) {
        end=strstr(line,"\r\n");if(!end)return -1;
        if(end==line)return end+2==f->text+length?0:-1;
        *end=0;char *value;
        if(f->count==HTTP_FIELD_MAX || http_split_field(line,&value)<0)return -1;
        f->fields[f->count++]=(struct http_field){(unsigned short)(line-f->text),(unsigned short)(value-f->text)};
        line=end+2;
    }
    return -1;
}
static int critical(const char *name,size_t n) {
    const char *names[]={"host","content-length","transfer-encoding"};
    for(size_t i=0;i<3;i++)if(strlen(names[i])==n && !strncasecmp(name,names[i],n))return 1;
    return 0;
}
int http_connection_valid(const struct http_fields *f) {
    for(size_t i=0;i<f->count;i++)if(!strcasecmp(http_field_name(f,i),"Connection")) {
        const char *p=http_field_value(f,i);
        for(;;) {
            while(*p==' ' || *p=='\t')p++;
            const char *start=p;while(http_token((unsigned char)*p))p++;
            if(p==start || critical(start,(size_t)(p-start)))return 0;
            while(*p==' ' || *p=='\t')p++;
            if(!*p)break;
            if(*p++!=',')return 0;
        }
    }
    return 1;
}
static int connection_has(const struct http_fields *f,const char *name) {
    size_t length=strlen(name);
    for(size_t i=0;i<f->count;i++)if(!strcasecmp(http_field_name(f,i),"Connection")) {
        const char *p=http_field_value(f,i);
        while(*p) {
            while(*p==' ' || *p=='\t' || *p==',')p++;
            const char *start=p;while(http_token((unsigned char)*p))p++;
            if(length==(size_t)(p-start) && !strncasecmp(name,start,length))return 1;
            if(p==start && *p)p++;
        }
    }
    return 0;
}
int http_field_hop(const struct http_fields *f,size_t index) {
    const char *name=http_field_name(f,index);
    const char *fixed[]={"Connection","Keep-Alive","Proxy-Connection","Proxy-Authenticate","Proxy-Authorization","TE","Trailer","Transfer-Encoding","Upgrade"};
    for(size_t i=0;i<sizeof fixed/sizeof fixed[0];i++)if(!strcasecmp(name,fixed[i]))return 1;
    return connection_has(f,name);
}
static int decimal(const char *p,uint64_t *value) {
    if(!*p)return -1;
    *value=0;
    for(;*p;p++) {if(*p<'0'||*p>'9'||*value>(UINT64_MAX-(unsigned)(*p-'0'))/10)return -1;*value=*value*10+(unsigned)(*p-'0');}
    return 0;
}
int http_response_parse(struct http_response *r,const char *data,size_t length,int head) {
    memset(r,0,sizeof *r);
    if(http_fields_parse(&r->headers,data,length)<0 || !http_connection_valid(&r->headers))return -1;
    const char *line=r->headers.text;
    if(strlen(line)<13 || (strncmp(line,"HTTP/1.1 ",9) && strncmp(line,"HTTP/1.0 ",9)) || line[12]!=' ')return -1;
    for(int i=9;i<12;i++)if(line[i]<'0'||line[i]>'9')return -1;
    r->status=(line[9]-'0')*100+(line[10]-'0')*10+line[11]-'0';
    if(r->status<100 || r->status>599 || r->status==101)return -1;
    r->persistent=!strncmp(line,"HTTP/1.1 ",9) && !connection_has(&r->headers,"close");
    unsigned lengths=0,transfers=0;
    for(size_t i=0;i<r->headers.count;i++) {
        const char *name=http_field_name(&r->headers,i),*value=http_field_value(&r->headers,i);
        if(!strcasecmp(name,"Content-Length")) {if(++lengths>1 || decimal(value,&r->length)<0)return -1;}
        if(!strcasecmp(name,"Transfer-Encoding")) {if(++transfers>1 || strcasecmp(value,"chunked"))return -1;}
    }
    if(lengths && transfers)return -1;
    if(r->status==205 && lengths && r->length)return -1;
    if((r->status<200 || r->status==204) && (lengths || transfers))return -1;
    r->framing=(head || r->status<200 || r->status==204 || r->status==304)?HTTP_NONE:
        transfers?HTTP_CHUNKED:lengths?HTTP_LENGTH:HTTP_EOF;
    return 0;
}
static int chunk_size(struct http_chunks *c) {
    const unsigned char *p=(unsigned char *)c->line;uint64_t size=0;unsigned digits=0;
    while((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')) {
        unsigned v=*p<='9'?*p-'0':(*p|32)-'a'+10;
        if(size>(UINT64_MAX-v)/16)return -1;
        size=size*16+v;p++;digits++;
    }
    if(!digits)return -1;
    while(*p) {
        while(*p==' '||*p=='\t')p++;
        if(*p++!=';')return -1;
        while(*p==' '||*p=='\t')p++;
        unsigned n=0;while(http_token(*p)){n++;p++;}if(!n)return -1;
        while(*p==' '||*p=='\t')p++;
        if(*p=='=') {
            p++;while(*p==' '||*p=='\t')p++;
            if(*p=='"') {
                p++;
                while(*p && *p!='"') {
                    if(*p=='\\'){p++;if(!*p)return -1;}
                    if((*p<32 && *p!='\t') || *p==127)return -1;
                    p++;
                }
                if(*p++!='"')return -1;
            } else {n=0;while(http_token(*p)){n++;p++;}if(!n)return -1;}
        }
    }
    c->remaining=size;c->state=size?CHUNK_DATA:CHUNK_TRAILERS;return 0;
}
int http_chunks_decode(struct http_chunks *c,const char *input,size_t *in,char *output,size_t *out) {
    size_t available=*in,capacity=*out,i=0,o=0;int rc=0;
    while(i<available && c->state!=CHUNK_DONE) {
        if(c->state==CHUNK_DATA) {
            size_t n=available-i;if(n>capacity-o)n=capacity-o;if((uint64_t)n>c->remaining)n=(size_t)c->remaining;
            if(!n)break;
            memcpy(output+o,input+i,n);i+=n;o+=n;c->remaining-=n;if(!c->remaining)c->state=CHUNK_CR;
        } else if(c->state==CHUNK_CR || c->state==CHUNK_LF) {
            if(input[i++]!=(c->state==CHUNK_CR?'\r':'\n')){rc=-1;break;}
            c->state=c->state==CHUNK_CR?CHUNK_LF:CHUNK_SIZE;
        } else {
            unsigned char b=(unsigned char)input[i++];
            if(c->state==CHUNK_TRAILERS && ++c->trailer_bytes>HEADER_LIMIT){rc=-1;break;}
            if(!b || c->used+1>=sizeof c->line){rc=-1;break;}
            c->line[c->used++]=(char)b;
            if(b=='\n') {
                if(c->used<2 || c->line[c->used-2]!='\r'){rc=-1;break;}
                c->line[c->used-2]=0;
                if(c->state==CHUNK_SIZE) {if(chunk_size(c)<0){rc=-1;break;}}
                else if(c->used==2)c->state=CHUNK_DONE;
                else {
                    char *value;
                    if(++c->trailer_count>HTTP_FIELD_MAX || http_split_field(c->line,&value)<0 || critical(c->line,strlen(c->line)) || !strcasecmp(c->line,"Connection") || !strcasecmp(c->line,"Trailer")){rc=-1;break;}
                }
                c->used=0;
            }
        }
    }
    *in=i;*out=o;return rc<0?-1:c->state==CHUNK_DONE?1:0;
}
