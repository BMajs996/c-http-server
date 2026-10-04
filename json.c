#include "json.h"
#include <stdint.h>
#include <string.h>
typedef struct { const unsigned char *p,*end; } cursor;
static void space(cursor *c) {
    while(c->p<c->end && (*c->p==' ' || *c->p=='\t' || *c->p=='\r' || *c->p=='\n'))++c->p;
}
static int string(cursor *c) {
    if(c->p==c->end || *c->p++!='"')return 0;
    while(c->p<c->end) {
        unsigned ch=*c->p++;
        if(ch=='"')return 1;
        if(ch<32)return 0;
        if(ch=='\\') {
            if(c->p==c->end)return 0;
            ch=*c->p++;
            if(ch=='u') {
                for(int i=0;i<4;++i) {
                    if(c->p==c->end)return 0;
                    ch=*c->p++;
                    if(!((ch>='0' && ch<='9') || (ch>='a' && ch<='f') || (ch>='A' && ch<='F')))return 0;
                }
            } else if(!ch || !strchr("\"\\/bfnrt",(int)ch))return 0;
        } else if(ch>=128) {
            unsigned count;uint32_t code,minimum;
            if(ch>=0xc2 && ch<=0xdf){count=1;code=ch&31;minimum=128;}
            else if(ch>=0xe0 && ch<=0xef){count=2;code=ch&15;minimum=2048;}
            else if(ch>=0xf0 && ch<=0xf4){count=3;code=ch&7;minimum=65536;}
            else return 0;
            while(count--) {
                if(c->p==c->end || (*c->p&0xc0)!=0x80)return 0;
                code=(code<<6)|(*c->p++&63);
            }
            if(code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff))return 0;
        }
    }
    return 0;
}
static int digit(cursor *c) { return c->p<c->end && *c->p>='0' && *c->p<='9'; }
static int value(cursor *c,unsigned depth) {
    space(c);if(c->p==c->end || depth>32)return 0;
    unsigned ch=*c->p;
    if(ch=='"')return string(c);
    if(ch=='{' || ch=='[') {
        if(depth>=32)return 0;
        ++c->p;space(c);unsigned close=ch=='{'?'}':']';
        if(c->p<c->end && *c->p==close){++c->p;return 1;}
        for(;;) {
            if(ch=='{') {
                if(!string(c))return 0;
                space(c);if(c->p==c->end || *c->p++!=':')return 0;
            }
            if(!value(c,depth+1))return 0;
            space(c);if(c->p==c->end)return 0;
            if(*c->p==close){++c->p;return 1;}
            if(*c->p++!=',')return 0;
            space(c);
        }
    }
    const char *literal=ch=='t'?"true":ch=='f'?"false":ch=='n'?"null":NULL;
    if(literal) {
        size_t n=strlen(literal);
        if((size_t)(c->end-c->p)<n || memcmp(c->p,literal,n))return 0;
        c->p+=n;return 1;
    }
    if(ch=='-')++c->p;
    if(!digit(c))return 0;
    if(*c->p=='0')++c->p;else while(digit(c))++c->p;
    if(c->p<c->end && *c->p=='.'){++c->p;if(!digit(c))return 0;while(digit(c))++c->p;}
    if(c->p<c->end && (*c->p=='e' || *c->p=='E')) {
        ++c->p;if(c->p<c->end && (*c->p=='+' || *c->p=='-'))++c->p;
        if(!digit(c))return 0;
        while(digit(c))++c->p;
    }
    return 1;
}
int json_valid(const char *data,size_t length) {
    cursor c={(const unsigned char *)data,(const unsigned char *)data+length};
    if(!value(&c,0))return 0;
    space(&c);return c.p==c.end;
}
