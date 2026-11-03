#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/time.h>
#include <cuda_runtime.h>
__global__ void k(){}
static double now(){timeval t;gettimeofday(&t,0);return t.tv_sec+t.tv_usec*1e-6;}
int main(int argc,char**argv){ k<<<1,1>>>(); cudaDeviceSynchronize();
  int gaps[]={0,50,100,200,400,1000};
  for(int g: gaps){ double tot=0; int N=300; for(int i=0;i<N;i++){ if(g) usleep(g); double a=now(); k<<<1,1>>>(); cudaDeviceSynchronize(); tot+=now()-a;} printf("idle gap %4d us -> launch+sync %.1f us\n",g,tot/N*1e6);} }
