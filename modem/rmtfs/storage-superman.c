// SPDX-License-Identifier: BSD-3-Clause
/* Exact SD files opened read-only; modem writes exist only in bounded RAM. */
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include "rmtfs.h"
#ifndef PF29_STORAGE_ROOT
#define PF29_STORAGE_ROOT "/data/vendor/rmtfs-pf29"
#endif
struct rmtfd { unsigned id; uint8_t *data; size_t size; };
static struct rmtfd files[4];
static const char *const names[]={"modem_fs1","modem_fs2","modem_fsc","modem_fsg"};
static const size_t sizes[]={1572864,1572864,16384,1572864};
int storage_init(const char *root,bool read_only,bool partitions) {
 if(!root||strcmp(root,PF29_STORAGE_ROOT)||!read_only||partitions)return -EINVAL;
 for(unsigned i=0;i<4;i++)files[i].id=i;
 return 0;
}
struct rmtfd *storage_open(unsigned node,const char *path,const char *suffix) {
 char expected[64],file[256];unsigned i;int fd;struct stat st;size_t done=0;
 if(node!=0||!path||(suffix&&suffix[0]))return NULL;
 for(i=0;i<4;i++){snprintf(expected,sizeof(expected),"/boot/%s",names[i]);if(!strcmp(path,expected))break;}
 if(i==4)return NULL;
 struct rmtfd *f=&files[i];if(f->data)return f;
 snprintf(file,sizeof(file),"%s/%s",PF29_STORAGE_ROOT,names[i]);
 fd=open(file,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return NULL;
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=(off_t)sizes[i]){close(fd);return NULL;}
 uint8_t *data=malloc(sizes[i]);if(!data){close(fd);return NULL;}
 while(done<sizes[i]) {
  ssize_t n=read(fd,data+done,sizes[i]-done);
  if(n<0&&errno==EINTR)continue;
  if(n<=0){free(data);close(fd);return NULL;}done+=(size_t)n;
 }
 close(fd);f->data=data;f->size=sizes[i];return f;
}
struct rmtfd *storage_get(unsigned node,int id) {
 if(node!=0||id<0||id>=4||!files[id].data)return NULL;
 return &files[id];
}
void storage_close(struct rmtfd *f) {
 if(f&&f->data){memset(f->data,0,f->size);free(f->data);f->data=NULL;f->size=0;}
}
int storage_get_caller_id(const struct rmtfd *f){return f?(int)f->id:-1;}
int storage_get_error(const struct rmtfd *f){return f&&f->data?0:1;}
void storage_exit(void){for(unsigned i=0;i<4;i++)storage_close(&files[i]);}
static int valid(const struct rmtfd *f,size_t n,off_t off) {
 return f&&f->data&&off>=0&&(uint64_t)off<=f->size&&n<=f->size-(size_t)off;
}
ssize_t storage_pread(const struct rmtfd *f,void *buf,size_t n,off_t off) {
 if(!buf||!valid(f,n,off)){errno=EINVAL;return -1;}
 memcpy(buf,f->data+(size_t)off,n);return (ssize_t)n;
}
ssize_t storage_pwrite(struct rmtfd *f,const void *buf,size_t n,off_t off) {
 if(!buf||!valid(f,n,off)){errno=EINVAL;return -1;}
 memcpy(f->data+(size_t)off,buf,n);return (ssize_t)n;
}
int storage_sync(struct rmtfd *f){return f&&f->data?0:-EINVAL;}
