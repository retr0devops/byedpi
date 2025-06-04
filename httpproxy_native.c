#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <nghttp2/nghttp2.h>

#define LISTEN_IP "127.0.0.1"
#define LISTEN_PORT 8085
#define SOCKS_IP "127.0.0.1"
#define SOCKS_PORT 1080

struct buffer {
    char *data;
    size_t size;
};

struct header {
    char *name;
    char *value;
};

static int connect_socks(const char *host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(SOCKS_PORT);
    inet_pton(AF_INET, SOCKS_IP, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    unsigned char buf[512];
    buf[0]=5; buf[1]=1; buf[2]=0;
    if (send(fd, buf, 3, 0)!=3) { close(fd); return -1; }
    if (recv(fd, buf, 2, 0)!=2 || buf[1]!=0) { close(fd); return -1; }
    size_t hl=strlen(host);
    buf[0]=5; buf[1]=1; buf[2]=0; buf[3]=3; buf[4]=hl;
    memcpy(buf+5, host, hl);
    buf[5+hl]=port>>8; buf[6+hl]=port&0xff;
    size_t bl=7+hl;
    if (send(fd, buf, bl, 0)!=bl) { close(fd); return -1; }
    if (recv(fd, buf, 4, 0)!=4 || buf[1]!=0) { close(fd); return -1; }
    int r=4;
    if (buf[3]==1) r+=4;
    else if (buf[3]==4) r+=16;
    else if (buf[3]==3) { recv(fd, buf+4,1,0); r+=1+buf[4]; }
    recv(fd, buf+4, r-4, 0);
    return fd;
}

static int recv_line(int fd, char **line) {
    size_t cap=128,len=0; char *buf=malloc(cap); if(!buf) return -1; while(1){ char c; if(recv(fd,&c,1,0)<=0){ free(buf); return -1;} if(len+1>=cap){ cap*=2; buf=realloc(buf,cap); if(!buf) return -1;} buf[len++]=c; if(len>=2&&buf[len-2]=='\r'&&buf[len-1]=='\n') break; } buf[len]=0; *line=buf; return 0; }

static int read_chunked(int fd, struct buffer *body) {
    body->data=NULL; body->size=0; while(1){ char *line; if(recv_line(fd,&line)) return -1; size_t len=strtoul(line,0,16); free(line); if(!len){ recv_line(fd,&line); free(line); break; } char *tmp=realloc(body->data, body->size+len); if(!tmp) return -1; body->data=tmp; size_t r=0; while(r<len){ ssize_t n=recv(fd, body->data+body->size+r, len-r,0); if(n<=0) return -1; r+=n;} body->size+=len; recv_line(fd,&line); free(line); } return 0; }

static int read_body(int fd, size_t len, struct buffer *body) {
    body->data=malloc(len?len:1); body->size=len; size_t r=0; while(r<len){ ssize_t n=recv(fd, body->data+r, len-r,0); if(n<=0) return -1; r+=n;} return 0; }

struct h2_stream { struct buffer headers; struct buffer body; int done; int32_t id; };

struct conn { SSL *ssl; };

static ssize_t send_cb(nghttp2_session *s, const uint8_t *data, size_t len, int f, void *ud){ struct conn *c=ud; return SSL_write(c->ssl,data,len); }
static ssize_t recv_cb(nghttp2_session *s, uint8_t *buf, size_t len, int f, void *ud){ struct conn *c=ud; return SSL_read(c->ssl,buf,len); }
static int on_hdr(nghttp2_session*s,const nghttp2_frame*f,const uint8_t*n,size_t nl,const uint8_t*v,size_t vl,uint8_t fl,void*ud){ struct h2_stream*st=ud; if(f->hd.stream_id==st->id){ st->headers.data=realloc(st->headers.data,st->headers.size+nl+vl+4); memcpy(st->headers.data+st->headers.size,n,nl); st->headers.size+=nl; memcpy(st->headers.data+st->headers.size,": ",2); st->headers.size+=2; memcpy(st->headers.data+st->headers.size,v,vl); st->headers.size+=vl; memcpy(st->headers.data+st->headers.size,"\r\n",2); st->headers.size+=2;} return 0; }
static int on_data(nghttp2_session*s,uint8_t fl,int32_t id,const uint8_t*d,size_t l,void*ud){ struct h2_stream*st=ud; if(id==st->id){ st->body.data=realloc(st->body.data,st->body.size+l); memcpy(st->body.data+st->body.size,d,l); st->body.size+=l;} return 0; }
static int on_close(nghttp2_session*s,int32_t id,uint32_t e,void*ud){ struct h2_stream*st=ud; if(id==st->id) st->done=1; return 0; }
static ssize_t data_read_cb(nghttp2_session*s,int32_t id,uint8_t *buf,size_t len,uint32_t *d, nghttp2_data_source *src,void*ud){ struct buffer *b=src->ptr; if(!b->size) return NGHTTP2_ERR_EOF; size_t n=len<b->size?len:b->size; memcpy(buf,b->data,n); b->data+=n; b->size-=n; return n; }

static int send_http2(int fd, const char *host, const char *method, const char *path, struct header *hdrs, size_t hcnt, struct buffer body, struct buffer *resp) {
    SSL_CTX *ctx=SSL_CTX_new(TLS_client_method());
    unsigned char alpn[]={2,'h','2'}; SSL_CTX_set_alpn_protos(ctx,alpn,sizeof(alpn));
    SSL *ssl=SSL_new(ctx); SSL_set_tlsext_host_name(ssl,host); SSL_set_fd(ssl,fd); if(SSL_connect(ssl)<=0){ SSL_free(ssl); SSL_CTX_free(ctx); return -1; }
    nghttp2_session_callbacks *cbs; nghttp2_session_callbacks_new(&cbs); nghttp2_session_callbacks_set_send_callback(cbs,send_cb); nghttp2_session_callbacks_set_recv_callback(cbs,recv_cb); nghttp2_session_callbacks_set_on_header_callback(cbs,on_hdr); nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs,on_data); nghttp2_session_callbacks_set_on_stream_close_callback(cbs,on_close); struct conn c={ssl}; nghttp2_session *sess; nghttp2_session_client_new(&sess,cbs,&c); nghttp2_submit_settings(sess,NGHTTP2_FLAG_NONE,NULL,0);
    #define NV(N,V) (nghttp2_nv){(uint8_t*)N,(uint8_t*)V,sizeof(N)-1,strlen(V),NGHTTP2_NV_FLAG_NONE}
    nghttp2_nv nva[hcnt+4]; size_t idx=0;
    char scheme[]="https"; nva[idx++]=NV(":method",method); nva[idx++]=NV(":path",path); nva[idx++]=NV(":scheme",scheme); nva[idx++]=NV(":authority",host); for(size_t i=0;i<hcnt;i++) nva[idx++]=NV(hdrs[i].name,hdrs[i].value);
    struct h2_stream st={0}; nghttp2_data_provider prd={.source.ptr=&body,.read_callback=data_read_cb}; st.id=nghttp2_submit_request(sess,NULL,nva,idx,body.size?&prd:NULL,&st);
    while(nghttp2_session_want_read(sess)||nghttp2_session_want_write(sess)){
        nghttp2_session_send(sess); fd_set rf; FD_ZERO(&rf); FD_SET(fd,&rf); struct timeval tv={1,0}; if(select(fd+1,&rf,NULL,NULL,&tv)>0){ unsigned char buf[4096]; ssize_t n=SSL_read(ssl,buf,sizeof(buf)); if(n>0){ nghttp2_session_mem_recv(sess,buf,n);} }
        if(st.done) break; }
    resp->size=st.headers.size+2+st.body.size; resp->data=malloc(resp->size); memcpy(resp->data,st.headers.data,st.headers.size); memcpy(resp->data+st.headers.size,"\r\n",2); memcpy(resp->data+st.headers.size+2,st.body.data,st.body.size);
    free(st.headers.data); free(st.body.data); nghttp2_session_del(sess); nghttp2_session_callbacks_del(cbs); SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); return 0; }

static int handle_request(int fd) {
    char *line; if(recv_line(fd,&line)) return -1; char method[16], path[4096], ver[16]; if(sscanf(line,"%15s %4095s %15s",method,path,ver)!=3){ free(line); return -1;} free(line); struct header *hdrs=NULL; size_t hcnt=0; int close_conn=0; size_t clen=0; int chunked=0; while(1){ if(recv_line(fd,&line)){ for(size_t i=0;i<hcnt;i++){ free(hdrs[i].name); free(hdrs[i].value);} free(hdrs); return -1;} if(!strcmp(line,"\r\n")){ free(line); break;} char *p=strchr(line,':'); if(p){ *p++=0; while(*p==' '||*p=='\t') p++; if(!strcasecmp(line,"Content-Length")) clen=strtoul(p,0,10); else if(!strcasecmp(line,"Transfer-Encoding")&&!strncasecmp(p,"chunked",7)) chunked=1; else if(!strcasecmp(line,"Connection")&&!strncasecmp(p,"close",5)) close_conn=1; hdrs=realloc(hdrs,(hcnt+1)*sizeof(*hdrs)); hdrs[hcnt].name=strdup(line); hdrs[hcnt].value=strdup(p); hcnt++; } free(line); }
    struct buffer body={0}; if(chunked){ if(read_chunked(fd,&body)){ goto fail; } } else if(clen){ if(read_body(fd,clen,&body)){ goto fail; } }
    const char *host=NULL; for(size_t i=0;i<hcnt;i++){ if(!strcasecmp(hdrs[i].name,"Host")){ host=hdrs[i].value; break; }} if(!host){ goto fail; }
    char *colon=strchr(host,':'); uint16_t port= colon?atoi(colon+1):443; if(colon) *colon=0;
    int s=connect_socks(host,port); if(s<0) goto fail;
    struct buffer resp={0}; if(send_http2(s,host,method,path,hdrs,hcnt,body,&resp)){ shutdown(s,SHUT_RDWR); close(s); goto fail; }
    shutdown(s,SHUT_RDWR); close(s); send(fd,ver,strlen(ver),0); send(fd," 200 OK\r\n",9,0); send(fd,resp.data,resp.size,0); free(resp.data);
    for(size_t i=0;i<hcnt;i++){ free(hdrs[i].name); free(hdrs[i].value);} free(hdrs); free(body.data); return close_conn||!strcasecmp(ver,"HTTP/1.0");
fail:
    for(size_t i=0;i<hcnt;i++){ free(hdrs[i].name); free(hdrs[i].value);} free(hdrs); free(body.data); return -1; }

static void *client_thr(void *arg){ int fd=(int)(intptr_t)arg; while(1){ int r=handle_request(fd); if(r) break; } shutdown(fd,SHUT_RDWR); close(fd); return NULL; }

int main(){ SSL_library_init(); SSL_load_error_strings(); int s=socket(AF_INET,SOCK_STREAM,0); int o=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&o,sizeof(o)); struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(LISTEN_PORT); inet_pton(AF_INET,LISTEN_IP,&a.sin_addr); bind(s,(struct sockaddr*)&a,sizeof(a)); listen(s,16); while(1){ int c=accept(s,NULL,NULL); if(c<0) continue; pthread_t t; pthread_create(&t,NULL,client_thr,(void*)(intptr_t)c); pthread_detach(t);} close(s); return 0; }
