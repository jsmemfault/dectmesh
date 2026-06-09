/*
 * p9do -- persistent-session 9P client. Holds ONE connection (version+attach)
 * and runs a sequence of ops over it, so we do NOT open/close the serial port
 * (toggle DTR) per command. The relay's DTR-gated session pool is size 1, so
 * per-command `9p` invocations churn it (teardown+recreate+upstream re-attach
 * every op) -> "Device not configured"/Broken pipe right after a big write.
 * One held session = one relay session = no churn. (aether_test never wedges
 * for exactly this reason.)
 *
 * Build:  cc -O2 -o p9do tools/p9do.c
 * Usage:  p9do <unixsock> <cmd> [<cmd> ...]
 *   rd:PATH         read PATH, print "PATH => <text>"
 *   wf:PATH:FILE    write FILE's bytes to PATH (chunked) -- e.g. an OTA image
 *   ws:PATH:STR     write STR to PATH (e.g. ws:dev/reboot9151:1)
 *   sleep:N         sleep N seconds (between, e.g., reboot and confirm)
 * Paths are relative to the attach root (no leading slash): dev/fw9151, etc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

enum { Tversion=100,Rversion=101,Tattach=104,Rattach=105,Rerror=107,
       Twalk=110,Rwalk=111,Topen=112,Ropen=113,Tread=116,Rread=117,
       Twrite=118,Rwrite=119,Tclunk=120,Rclunk=121 };
#define NOFID 0xffffffffu
#define NOTAG 0xffff
#define MSIZE 8192
#define CHUNK 4096            /* < msize - Twrite header */

static int fd;
static uint8_t mb[MSIZE];
static char lerr[160];

static void p16(uint8_t*b,uint16_t v){b[0]=v;b[1]=v>>8;}
static void p32(uint8_t*b,uint32_t v){b[0]=v;b[1]=v>>8;b[2]=v>>16;b[3]=v>>24;}
static uint16_t g16(const uint8_t*b){return b[0]|(b[1]<<8);}
static uint32_t g32(const uint8_t*b){return b[0]|(b[1]<<8)|(b[2]<<16)|((uint32_t)b[3]<<24);}

static int readn(uint8_t*b,int n){int g=0;while(g<n){int r=read(fd,b+g,n-g);if(r<=0)return -1;g+=r;}return g;}

static int rpc(int len){
	lerr[0]=0; p32(mb,len);
	if(write(fd,mb,len)!=len) return -1;
	uint8_t h[4]; if(readn(h,4)<0) return -1;
	uint32_t sz=g32(h); if(sz<7||sz>MSIZE) return -1;
	if(readn(mb,sz-4)<0) return -1;
	if(mb[0]==Rerror){uint16_t n=g16(mb+3); snprintf(lerr,sizeof lerr,"%.*s",(int)n,(char*)mb+5);}
	return mb[0];
}
static int t_version(void){int o=4;mb[o++]=Tversion;p16(mb+o,NOTAG);o+=2;p32(mb+o,MSIZE);o+=4;p16(mb+o,6);o+=2;memcpy(mb+o,"9P2000",6);o+=6;return rpc(o)==Rversion?0:-1;}
static int t_attach(uint32_t f){int o=4;mb[o++]=Tattach;p16(mb+o,1);o+=2;p32(mb+o,f);o+=4;p32(mb+o,NOFID);o+=4;p16(mb+o,1);o+=2;mb[o++]='t';p16(mb+o,0);o+=2;return rpc(o)==Rattach?0:-1;}
static int t_walk(uint32_t f,uint32_t nf,const char*path){
	char tmp[128];strncpy(tmp,path,sizeof tmp-1);tmp[sizeof tmp-1]=0;
	const char*pp[16];int np=0;
	for(char*p=strtok(tmp,"/");p&&np<16;p=strtok(NULL,"/"))pp[np++]=p;
	int o=4;mb[o++]=Twalk;p16(mb+o,1);o+=2;p32(mb+o,f);o+=4;p32(mb+o,nf);o+=4;p16(mb+o,np);o+=2;
	for(int i=0;i<np;i++){int l=strlen(pp[i]);p16(mb+o,l);o+=2;memcpy(mb+o,pp[i],l);o+=l;}
	return rpc(o)==Rwalk?0:-1;
}
static int t_open(uint32_t f,uint8_t m){int o=4;mb[o++]=Topen;p16(mb+o,1);o+=2;p32(mb+o,f);o+=4;mb[o++]=m;return rpc(o)==Ropen?0:-1;}
static void t_clunk(uint32_t f){int o=4;mb[o++]=Tclunk;p16(mb+o,1);o+=2;p32(mb+o,f);o+=4;rpc(o);}
static int t_read(uint32_t f,uint64_t off,uint8_t*out,int cap){
	int o=4;mb[o++]=Tread;p16(mb+o,1);o+=2;p32(mb+o,f);o+=4;p32(mb+o,off);o+=4;p32(mb+o,off>>32);o+=4;p32(mb+o,cap);o+=4;
	if(rpc(o)!=Rread)return -1; uint32_t n=g32(mb+3); if((int)n>cap)n=cap; memcpy(out,mb+7,n); return n;
}
static int t_write(uint32_t f,uint64_t off,const void*d,int l){
	int o=4;mb[o++]=Twrite;p16(mb+o,1);o+=2;p32(mb+o,f);o+=4;p32(mb+o,off);o+=4;p32(mb+o,off>>32);o+=4;p32(mb+o,l);o+=4;memcpy(mb+o,d,l);o+=l;
	if(rpc(o)!=Rwrite)return -1; return (int)g32(mb+3);
}

static int cmd_rd(const char*path){
	if(t_walk(0,1,path)){printf("%s => WALK FAIL (%s)\n",path,lerr);return -1;}
	if(t_open(1,0)){printf("%s => OPEN FAIL (%s)\n",path,lerr);t_clunk(1);return -1;}
	char buf[512];int n=t_read(1,0,(uint8_t*)buf,sizeof buf-1);t_clunk(1);
	if(n<0){printf("%s => READ FAIL (%s)\n",path,lerr);return -1;}
	buf[n]=0; for(int i=0;i<n;i++)if(buf[i]=='\n')buf[i]=' ';
	printf("%s => %s\n",path,buf); return 0;
}
static int cmd_ws(const char*path,const char*str){
	if(t_walk(0,1,path)){printf("ws %s => WALK FAIL (%s)\n",path,lerr);return -1;}
	if(t_open(1,1)){printf("ws %s => OPEN FAIL (%s)\n",path,lerr);t_clunk(1);return -1;}
	int r=t_write(1,0,str,strlen(str));t_clunk(1);
	printf("ws %s '%s' => %s\n",path,str,r>=0?"ok":lerr); return r>=0?0:-1;
}
static int cmd_wf(const char*path,const char*file){
	int in=open(file,O_RDONLY); if(in<0){printf("wf: cannot open %s\n",file);return -1;}
	if(t_walk(0,1,path)){printf("wf %s => WALK FAIL (%s)\n",path,lerr);close(in);return -1;}
	if(t_open(1,2)){printf("wf %s => OPEN FAIL (%s)\n",path,lerr);t_clunk(1);close(in);return -1;}
	uint8_t chunk[CHUNK]; uint64_t off=0; int rd,bad=0;
	while((rd=read(in,chunk,CHUNK))>0){
		int w=t_write(1,off,chunk,rd);
		if(w!=rd){printf("wf %s => WRITE FAIL at off %llu (%s)\n",path,(unsigned long long)off,lerr);bad=1;break;}
		off+=rd;
	}
	t_clunk(1); close(in);
	if(!bad) printf("wf %s <= %s : %llu bytes ok\n",path,file,(unsigned long long)off);
	return bad?-1:0;
}

int main(int argc,char**argv){
	if(argc<3){fprintf(stderr,"usage: %s <unixsock> <cmd>...\n",argv[0]);return 2;}
	struct sockaddr_un sa={0}; sa.sun_family=AF_UNIX; strncpy(sa.sun_path,argv[1],sizeof sa.sun_path-1);
	fd=socket(AF_UNIX,SOCK_STREAM,0);
	if(fd<0||connect(fd,(void*)&sa,sizeof sa)<0){perror("connect");return 2;}
	if(t_version()||t_attach(0)){fprintf(stderr,"9P setup failed (%s)\n",lerr);return 2;}
	int rc=0;
	for(int i=2;i<argc;i++){
		char*c=argv[i];
		if(!strncmp(c,"rd:",3)) rc|=cmd_rd(c+3);
		else if(!strncmp(c,"wf:",3)){char*p=strdup(c+3),*f=strchr(p,':');if(!f){printf("bad wf\n");rc=1;}else{*f++=0;rc|=cmd_wf(p,f);}free(p);}
		else if(!strncmp(c,"ws:",3)){char*p=strdup(c+3),*s=strchr(p,':');if(!s){printf("bad ws\n");rc=1;}else{*s++=0;rc|=cmd_ws(p,s);}free(p);}
		else if(!strncmp(c,"sleep:",6)){int n=atoi(c+6);printf("-- sleep %d --\n",n);fflush(stdout);struct timespec ts={n,0};nanosleep(&ts,0);}
		else printf("unknown cmd '%s'\n",c);
	}
	t_clunk(0); close(fd);
	return rc?1:0;
}
