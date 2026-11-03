#include <cstdio>
#include <cuda.h>
#include <cuda_runtime.h>
#include <cstdlib>
#include <unistd.h>
int main(){
  void* p[4]; size_t sz[4]={1<<20, 256<<20, 4ull<<30, 64};
  for(int i=0;i<4;i++){ cudaMalloc(&p[i],sz[i]); printf("dev %zu -> %p\n",sz[i],p[i]); }
  void* h; cudaMallocHost(&h,1<<20); printf("pinned %p\n",h);
  int v=0; cuDeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED, 0); printf("VMM=%d\n",v);
  char cmd[64]; sprintf(cmd,"cat /proc/%d/maps",getpid()); 
  FILE* f=popen(cmd,"r"); char line[512]; while(fgets(line,512,f)){ unsigned long a,b; sscanf(line,"%lx-%lx",&a,&b); if(b-a>=(64ul<<20)) printf("MAP %s",line);} 
  for(int i=0;i<4;i++){ unsigned long a=(unsigned long)p[i]; f=popen(cmd,"r"); while(fgets(line,512,f)){unsigned long x,y; sscanf(line,"%lx-%lx",&x,&y); if(a>=x&&a<y) printf("  p%d in %s",i,line);} }
}
