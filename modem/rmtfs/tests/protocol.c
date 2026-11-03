#define _GNU_SOURCE
#include <assert.h>
#define main rmtfs_program_main
#include "../upstream/rmtfs.c"
#undef main
#define valid storage_valid
#include "../storage-superman.c"
#undef valid
#include "../sharedmem-superman.c"

static unsigned char response[4096];
static struct qrtr_packet reply = {.data = response};
int qrtr_sendto(int sock, uint32_t node, uint32_t port, const void *data, unsigned sz)
{
 (void)sock; assert(node == 0 && port == 12 && sz <= sizeof(response));
 memcpy(response, data, sz); reply.data_len = sz; return sz;
}
static void request(struct qrtr_packet *pkt, int id, const void *req,
                    struct qmi_elem_info *ei)
{
 assert(qmi_encode_message(pkt,QMI_REQUEST,id,42,req,ei)>0);
 pkt->node=0;pkt->port=12;
}
static void check_reply(int id, void *resp, struct qmi_elem_info *ei)
{
 unsigned txn=0;assert(qmi_decode_message(resp,&txn,&reply,QMI_RESPONSE,id,ei)>=0);
 assert(txn==42);
}
static void device_error(unsigned id, bool success)
{
 DEFINE_QRTR_PACKET(pkt,256);
 struct rmtfs_dev_error_req req={.caller_id=id};struct rmtfs_dev_error_resp resp={0};
 request(&pkt,QMI_RMTFS_GET_DEV_ERROR,&req,rmtfs_dev_error_req_ei);
 rmtfs_get_dev_error(-1,&pkt);
 check_reply(QMI_RMTFS_GET_DEV_ERROR,&resp,rmtfs_dev_error_resp_ei);
 assert(resp.result.result==(success?QMI_RMTFS_RESULT_SUCCESS:QMI_RMTFS_RESULT_FAILURE));
 assert(resp.status_valid==success);if(success)assert(resp.status==0);
}
static void iovec(unsigned sector,unsigned phys,unsigned count,unsigned direction,bool success)
{
 DEFINE_QRTR_PACKET(pkt,4096);
 struct rmtfs_iovec_req req={.caller_id=0,.direction=direction,.iovec_len=1,
  .iovec={{.sector_addr=sector,.phys_offset=phys,.num_sector=count}}};
 struct rmtfs_iovec_resp resp={0};
 request(&pkt,QMI_RMTFS_RW_IOVEC,&req,rmtfs_iovec_req_ei);
 rmtfs_iovec(-1,&pkt);
 check_reply(QMI_RMTFS_RW_IOVEC,&resp,rmtfs_iovec_resp_ei);
 assert(resp.result.result==(success?QMI_RMTFS_RESULT_SUCCESS:QMI_RMTFS_RESULT_FAILURE));
}
int main(void)
{
 assert(storage_init(PF29_STORAGE_ROOT,true,false)==0);
 assert(storage_open(0,"/boot/modem_fs1",""));
 device_error(0,true);device_error(1,false);device_error(UINT32_MAX,false);
 char mempath[512];snprintf(mempath,sizeof(mempath),"%s/shared-test",PF29_STORAGE_ROOT);
 FILE *f=fopen(mempath,"w+b");assert(f);assert(ftruncate(fileno(f),0x180000)==0);
 struct rmtfs_mem mem={.fd=fileno(f),.base=0x0fd80000,.size=0x180000};rmem=&mem;
 iovec(1,mem.base+512,1,0,true);
 unsigned char b[512];assert(pread(mem.fd,b,512,512)==512&&b[0]==0x33);
 memset(b,0x5a,512);assert(pwrite(mem.fd,b,512,512)==512);
 iovec(1,mem.base+512,1,1,true);
 assert(storage_pread(&files[0],b,512,512)==512&&b[0]==0x5a);
 iovec(1,mem.base+512,1,2,false);
 iovec(0x800000,mem.base,1,0,false); /* Used to wrap to sector zero. */
 iovec(UINT32_MAX,mem.base,1,0,false);
 iovec(0,UINT32_MAX-511,1,0,false);
 iovec(0,0x7fffff00,2,0,false);
 iovec(0,mem.base,UINT32_MAX,0,false);
 iovec(3071,mem.base+0x180000-512,1,0,true);
 iovec(3072,mem.base,1,0,false);
 iovec(0,mem.base+0x180000,1,0,false);
 for(size_t n=0;n<7;n++) {
  unsigned char *shortbuf=malloc(n?n:1);assert(shortbuf);memset(shortbuf,0,n?n:1);
  struct qrtr_packet pkt={.data=shortbuf,.data_len=n};unsigned id,txn;
  struct rmtfs_dev_error_req req={0};
  assert(qmi_decode_header(&pkt,&id)<0);
  assert(qmi_decode_message(&req,&txn,&pkt,QMI_REQUEST,QMI_RMTFS_GET_DEV_ERROR,rmtfs_dev_error_req_ei)<0);
  free(shortbuf);
 }
 DEFINE_QRTR_PACKET(pkt,256);struct rmtfs_dev_error_req req={.caller_id=0};
 request(&pkt,QMI_RMTFS_GET_DEV_ERROR,&req,rmtfs_dev_error_req_ei);
 pkt.data_len--;unsigned txn;
 assert(qmi_decode_message(&req,&txn,&pkt,QMI_REQUEST,QMI_RMTFS_GET_DEV_ERROR,rmtfs_dev_error_req_ei)<0);
 fclose(f);assert(unlink(mempath)==0);storage_exit();
 puts("PASS: actual RMTFS device-error/iovec handlers, real QMI codec, short headers, ARM32 wrapping, bounds, RAM-only I/O");
 return 0;
}
