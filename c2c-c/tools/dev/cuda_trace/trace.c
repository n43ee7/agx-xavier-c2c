#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "names.h"
#define N (sizeof(kNames)/sizeof(kNames[0]))
static void* real[N]; static unsigned long hits[N]; static void* lib;
extern void* tramp_table[];
static void init(void) {
  if (lib) return;
  lib = dlopen(getenv("CUTRACE_REAL")?getenv("CUTRACE_REAL"):"/usr/lib/aarch64-linux-gnu/tegra/libcuda.so.1.1", RTLD_NOW|RTLD_LOCAL);
  for (unsigned i=0;i<N;i++) real[i]=dlsym(lib,kNames[i]);
}
static int seqn; void* trace_hit(unsigned long i){ init(); if(getenv("CUTRACE_SEQ") && seqn++<400) fprintf(stderr,"CALL %s\n",kNames[i]); /*seqlog*/ __atomic_add_fetch(&hits[i],1,__ATOMIC_RELAXED); return real[i]; }
int cuGetExportTable(const void** tbl, const void* uuid);
typedef int (*gpa_t)(const char*, void**, int, unsigned long long);
static unsigned long gpa_hits;
#define ET 24
#define ES 96
extern void* et_tramps[];
static unsigned char et_uuid[ET][16]; static void** et_real[ET]; static unsigned long et_n[ET]; static void* et_copy[ET][ES+1]; static unsigned long et_hits[ET][ES]; static int n_et;
static FILE* elog; static int elogn[ET*ES];
unsigned long et_ret(unsigned long id, unsigned long rv, unsigned long* a){
  if(!elog) elog=fopen("/tmp/cut/et.log","w");
  if(elog && elogn[id]++<8) { Dl_info di={0}; dladdr((void*)a[-1],&di);
    fprintf(elog,"ET%lu[%lu](%lx,%lx,%lx,%lx,%lx,%lx) = %lx  from %s+%lx\n",id/96,id%96,a[0],a[1],a[2],a[3],a[4],a[5],rv, di.dli_fname?di.dli_fname:"?", (unsigned long)a[-1]-(unsigned long)di.dli_fbase);
    for(int k=0;k<3;k++){ unsigned long v=a[k]; if(v>0x10000 && (v>>48)==0 && v%8==0){ fprintf(elog,"   *a%d:",k); unsigned long* p=(unsigned long*)v; for(int j=0;j<8;j++) fprintf(elog," %lx",p[j]); fprintf(elog,"\n");} }
    fflush(elog);}
  return rv; }
void* et_hit(unsigned long t, unsigned long s){ __atomic_add_fetch(&et_hits[t][s],1,__ATOMIC_RELAXED); return et_real[t][s]; }
typedef int (*get_t)(const void**, const void*);
int cuGetExportTable(const void** tbl, const void* uuid) {
  init();
  int r=((get_t)dlsym(lib,"cuGetExportTable"))(tbl,uuid);
  if (r||!tbl||!*tbl) return r;
  for (int t=0;t<n_et;t++) if(!memcmp(et_uuid[t],uuid,16)){ *tbl=et_copy[t]; return r; }
  if (n_et>=ET) return r;
  int t=n_et++; memcpy(et_uuid[t],uuid,16); void** rt=(void**)*tbl; et_real[t]=rt;
  unsigned long sz=(unsigned long)rt[0]; unsigned long n = (sz>8 && sz<8*ES && sz%8==0)? sz/8 : 0;
  et_n[t]=n; et_copy[t][0]=rt[0];
  if (n) { for(unsigned long s=1;s<n;s++) et_copy[t][s]=et_tramps[t*ES+s]; }
  else { /* no size header: guess, wrap first 64 entries that look like code pointers */ for(unsigned long s=0;s<64;s++) et_copy[t][s]=et_tramps[t*ES+s]; et_n[t]=64; }
  *tbl=et_copy[t];
  return r;
}
int cuGetProcAddress(const char* sym, void** pfn, int ver, unsigned long long flags) {
  init(); gpa_hits++;
  int r = ((gpa_t)dlsym(lib,"cuGetProcAddress"))(sym,pfn,ver,flags);
  int m=-1;
  if (r==0 && pfn && !strcmp(sym,"cuGetProcAddress")) *pfn=(void*)cuGetProcAddress;
  if (r==0 && pfn && !strcmp(sym,"cuGetExportTable")) *pfn=(void*)cuGetExportTable;
  { extern int cuCtxSetCurrent(void*); extern int cuCtxGetCurrent(void**); extern int cuDevicePrimaryCtxRetain(void**,int); extern int cuCtxGetDevice(int*);
    if (r==0&&pfn&&!strcmp(sym,"cuCtxSetCurrent")) { *pfn=(void*)cuCtxSetCurrent; return r; }
    if (r==0&&pfn&&!strcmp(sym,"cuCtxGetCurrent")) { *pfn=(void*)cuCtxGetCurrent; return r; }
    if (r==0&&pfn&&!strcmp(sym,"cuDevicePrimaryCtxRetain")) { *pfn=(void*)cuDevicePrimaryCtxRetain; return r; }
    if (r==0&&pfn&&!strcmp(sym,"cuCtxGetDevice")) { *pfn=(void*)cuCtxGetDevice; return r; } }
  if (r==0 && pfn && *pfn) {
    for (unsigned i=0;i<N;i++) if (real[i]==*pfn) { *pfn = tramp_table[i]; m=i; break; }
  }
  if (getenv("CUTRACE_GPA")) fprintf(stderr,"GPA %s ver=%d flags=%llu r=%d match=%s\n",sym,ver,flags,r,m>=0?kNames[m]:"-");
  return r;
}
__attribute__((destructor)) static void dump(void){
  FILE* f=fopen(getenv("CUTRACE_OUT")?getenv("CUTRACE_OUT"):"/tmp/cutrace.txt","w"); if(!f)return;
  fprintf(f,"cuGetProcAddress %lu\n",gpa_hits);
  for(unsigned i=0;i<N;i++) if(hits[i]) fprintf(f,"%s %lu\n",kNames[i],hits[i]);
  for(int t=0;t<n_et;t++){ fprintf(f,"ET%d uuid=",t); for(int k=0;k<16;k++) fprintf(f,"%02x",et_uuid[t][k]); fprintf(f," size=%lu:",(unsigned long)et_real[t][0]); for(unsigned long s=0;s<et_n[t];s++) if(et_hits[t][s]) fprintf(f," [%lu]=%lu",s,et_hits[t][s]); fprintf(f,"\n"); }
  fclose(f);
}
typedef int (*f_sc)(void*); typedef int (*f_gc)(void**); typedef int (*f_pr)(void**,int); typedef int (*f_gd)(int*);
int cuCtxSetCurrent(void* c){ init(); int r=((f_sc)dlsym(lib,"cuCtxSetCurrent"))(c); fprintf(stderr,"  cuCtxSetCurrent(%p)=%d\n",c,r); return r; }
int cuCtxGetCurrent(void** c){ init(); int r=((f_gc)dlsym(lib,"cuCtxGetCurrent"))(c); fprintf(stderr,"  cuCtxGetCurrent()=%d -> %p\n",r,c?*c:0); return r; }
int cuDevicePrimaryCtxRetain(void** c,int d){ init(); int r=((f_pr)dlsym(lib,"cuDevicePrimaryCtxRetain"))(c,d); fprintf(stderr,"  cuDevicePrimaryCtxRetain(dev %d)=%d -> %p\n",d,r,c?*c:0); return r; }
int cuCtxGetDevice(int* d){ init(); int r=((f_gd)dlsym(lib,"cuCtxGetDevice"))(d); fprintf(stderr,"  cuCtxGetDevice()=%d -> %d\n",r,d?*d:-1); return r; }
