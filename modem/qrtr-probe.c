// SPDX-License-Identifier: Apache-2.0
/* Bounded, read-only QRTR discovery and DMS/UIM status requests. No RIL/NV writes. */
#include <sys/socket.h>
#include <linux/qrtr.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <endian.h>
#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static int64_t now_ms(void) {
 struct timespec t; if (clock_gettime(CLOCK_MONOTONIC,&t)) return -1;
 return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int response(const uint8_t *p,size_t n,uint16_t id,int print) {
 if(n<7 || p[0]!=2 || le16(p+1)!=1 || le16(p+3)!=id || le16(p+5)!=n-7) return -1;
 size_t off=7;int result_seen=0,rc=-1;
 while(off<n) {
  if(n-off<3) return -1;
  uint8_t type=p[off];size_t len=le16(p+off+1);off+=3;
  if(len>n-off) return -1;
  if(type==2) {
   if(len!=4 || result_seen) return -1;
   result_seen=1;rc=le16(p+off)==0?0:1;
  }
  off+=len;
 }
 if(!result_seen) return -1;
 if(print) {
  printf("{\"message\":%u,\"qmi_success\":%s,\"tlvs\":[",id,rc==0?"true":"false");
  off=7;int first=1;
  while(off<n) {
   uint8_t type=p[off];size_t len=le16(p+off+1);off+=3;
   printf("%s{\"type\":%u,\"bytes\":%zu,\"hex\":\"",first?"":",",type,len);first=0;
   for(size_t i=0;i<len && i<256;i++)printf("%02x",p[off+i]);
   printf("\"}");off+=len;
  }
  printf("]}\n");
 }
 return rc;
}
static int request(int fd,struct sockaddr_qrtr peer,uint16_t id) {
 uint8_t req[7]={0,1,0,(uint8_t)id,(uint8_t)(id>>8),0,0};
 if(sendto(fd,req,sizeof(req),0,(struct sockaddr*)&peer,sizeof(peer))!=sizeof(req))return -1;
 int64_t until=now_ms()+3000;
 for(;;) {
  int64_t left=until-now_ms();if(left<=0)return -1;
  struct pollfd p={.fd=fd,.events=POLLIN};int ret=poll(&p,1,(int)left);
  if(ret<0&&errno==EINTR)continue;if(ret<=0)return -1;
  uint8_t data[4096];struct sockaddr_qrtr from;socklen_t len=sizeof(from);
  ssize_t n=recvfrom(fd,data,sizeof(data),MSG_TRUNC,(struct sockaddr*)&from,&len);
  if(n<0)return -1;
  if(len!=sizeof(from)||from.sq_node!=peer.sq_node||from.sq_port!=peer.sq_port)continue;
  if((size_t)n>sizeof(data))return -1;
  return response(data,(size_t)n,id,1);
 }
}
static int self_test(void) {
 uint8_t good[]={2,1,0,0x23,0,7,0,2,4,0,0,0,0,0};
 assert(response(good,sizeof(good),0x23,0)==0);
 for(size_t n=0;n<sizeof(good);n++)assert(response(good,n,0x23,0)<0);
 assert(response(good,sizeof(good),0x24,0)<0);
 good[0]=0;assert(response(good,sizeof(good),0x23,0)<0);good[0]=2;
 good[1]=2;assert(response(good,sizeof(good),0x23,0)<0);good[1]=1;
 good[8]=5;assert(response(good,sizeof(good),0x23,0)<0);good[8]=4;
 good[10]=1;assert(response(good,sizeof(good),0x23,0)==1);
 puts("PASS QMI header, transaction, length, TLV bounds and result checks");return 0;
}
int main(int argc,char **argv) {
 if(argc==2&&!strcmp(argv[1],"--self-test"))return self_test();
 if(argc!=1)return 2;
 int fd=socket(AF_QIPCRTR,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0){perror("socket");return 1;}
 struct sockaddr_qrtr local={.sq_family=AF_QIPCRTR};socklen_t len=sizeof(local);
 if(getsockname(fd,(struct sockaddr*)&local,&len)||bind(fd,(struct sockaddr*)&local,sizeof(local))||getsockname(fd,(struct sockaddr*)&local,&len)){perror("bind");close(fd);return 1;}
 struct sockaddr_qrtr ns={.sq_family=AF_QIPCRTR,.sq_node=local.sq_node,.sq_port=QRTR_PORT_CTRL};
 struct qrtr_ctrl_pkt lookup={.cmd=htole32(QRTR_TYPE_NEW_LOOKUP)};
 if(sendto(fd,&lookup,sizeof(lookup),0,(struct sockaddr*)&ns,sizeof(ns))!=sizeof(lookup)){perror("lookup");close(fd);return 1;}
 struct sockaddr_qrtr dms={.sq_family=AF_QIPCRTR},uim=dms;int done=0;int64_t until=now_ms()+3000;
 while(now_ms()<until) {
  struct pollfd p={.fd=fd,.events=POLLIN};int ret=poll(&p,1,(int)(until-now_ms()));
  if(ret<0&&errno==EINTR)continue;if(ret<=0)break;
  struct qrtr_ctrl_pkt pkt;struct sockaddr_qrtr from;socklen_t flen=sizeof(from);
  ssize_t n=recvfrom(fd,&pkt,sizeof(pkt),MSG_TRUNC,(struct sockaddr*)&from,&flen);
  if(n!=sizeof(pkt)||flen!=sizeof(from)||from.sq_node!=ns.sq_node||from.sq_port!=ns.sq_port||le32toh(pkt.cmd)!=QRTR_TYPE_NEW_SERVER)continue;
  uint32_t service=le32toh(pkt.server.service),instance=le32toh(pkt.server.instance),node=le32toh(pkt.server.node),port=le32toh(pkt.server.port);
  if(!node&&!port){done=1;break;}
  printf("{\"service\":%u,\"instance\":%u,\"node\":%u,\"port\":%u}\n",service,instance,node,port);
  /* QRTR modem node 0; exclude similarly named DSP services. */
  if(node==0&&service==2){dms.sq_node=node;dms.sq_port=port;}
  if(node==0&&service==11){uim.sq_node=node;uim.sq_port=port;}
 }
 int rc=done?0:1;
 if(dms.sq_port){puts("DMS_REVISION");if(request(fd,dms,0x23))rc=1;}
 if(uim.sq_port){puts("UIM_CARD_STATUS");if(request(fd,uim,0x2f))rc=1;}
 close(fd);return rc;
}
