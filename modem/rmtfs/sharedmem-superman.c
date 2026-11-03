// SPDX-License-Identifier: BSD-3-Clause
/* Only the standard kernel RMTFS device. No /dev/mem or address fallback. */
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "rmtfs.h"
struct rmtfs_mem { int fd; uint64_t base,size; };
static int attr(const char *name,uint64_t *value) {
 char path[128],line[64],*end;unsigned long long v;
 snprintf(path,sizeof(path),"/sys/class/rmtfs/qcom_rmtfs_mem1/%s",name);
 FILE *f=fopen(path,"re");if(!f)return -1;
 if(!fgets(line,sizeof(line),f)){fclose(f);return -1;}fclose(f);
 errno=0;v=strtoull(line,&end,16);
 if(errno||end==line||(*end!='\n'&&*end!='\0'))return -1;
 *value=v;return 0;
}
struct rmtfs_mem *rmtfs_mem_open(void) {
 struct stat st;uint64_t base,size;
 if(attr("phys_addr",&base)||attr("size",&size)||base!=0x0fd80000||size!=0x180000){
  fprintf(stderr,"RMTFS exact reserved-memory device is unavailable\n");return NULL;
 }
 int fd=open("/dev/qcom_rmtfs_mem1",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
 if(fd<0)return NULL;
 if(fstat(fd,&st)||!S_ISCHR(st.st_mode)){close(fd);return NULL;}
 struct rmtfs_mem *m=calloc(1,sizeof(*m));if(!m){close(fd);return NULL;}
 m->fd=fd;m->base=base;m->size=size;return m;
}
void rmtfs_mem_close(struct rmtfs_mem *m){if(m){close(m->fd);free(m);}}
int64_t rmtfs_mem_alloc(struct rmtfs_mem *m,size_t size) {
 return m&&size&&size<=m->size?(int64_t)m->base:-EINVAL;
}
void rmtfs_mem_free(struct rmtfs_mem *m){(void)m;}
static int valid(struct rmtfs_mem *m,unsigned long addr,ssize_t n) {
 return m&&n>=0&&(uint64_t)addr>=m->base&&
        (uint64_t)addr-m->base<=m->size&&(uint64_t)n<=m->size-((uint64_t)addr-m->base);
}
ssize_t rmtfs_mem_read(struct rmtfs_mem *m,unsigned long addr,void *buf,ssize_t n) {
 if(!buf||!valid(m,addr,n)){errno=EINVAL;return -1;}
 return pread(m->fd,buf,(size_t)n,(off_t)((uint64_t)addr-m->base));
}
ssize_t rmtfs_mem_write(struct rmtfs_mem *m,unsigned long addr,const void *buf,ssize_t n) {
 if(!buf||!valid(m,addr,n)){errno=EINVAL;return -1;}
 return pwrite(m->fd,buf,(size_t)n,(off_t)((uint64_t)addr-m->base));
}
