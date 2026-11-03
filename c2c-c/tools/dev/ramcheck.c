// Fills N GiB with a per-word unique pattern and reads it back, twice w/ different seeds.
// Fake/aliased capacity or bad cells show up as mismatches.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
static inline uint64_t mix(uint64_t x){x^=x>>33;x*=0xff51afd7ed558ccdULL;x^=x>>33;x*=0xc4ceb9fe1a85ec53ULL;x^=x>>33;return x;}
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int c,char**v){
  double gib=atof(v[1]); size_t n=(size_t)(gib*(1ULL<<30))/8;
  uint64_t*p=mmap(0,n*8,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
  if(p==MAP_FAILED){perror("mmap");return 2;}
  long bad=0;
  for(uint64_t seed=1;seed<=2;seed++){
    double t=now(); for(size_t i=0;i<n;i++)p[i]=mix(i^(seed<<56)); double w=now()-t;
    t=now(); for(size_t i=0;i<n;i++)if(p[i]!=mix(i^(seed<<56)))bad++; double r=now()-t;
    printf("pass %lu: %.2f GiB written %.2f GB/s, verified %.2f GB/s, errors %ld\n",seed,gib,n*8/w/1e9,n*8/r/1e9,bad);fflush(stdout);
  }
  return bad?1:0;}
