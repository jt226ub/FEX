// SPDX-License-Identifier: MIT
//
// From the independent review of the round 2 vector-ordering changes: whether a DMB ISHLD
// (or ISHST, ISH, none) between two plain stores keeps them in order on this machine.
// Build: clang -O2 -mcpu=apple-m1 -o MpBarrier Source/Tools/OrderingCostProbe/MpBarrier.c
// Run:   ./MpBarrier <mode> <iterations> [slow 0/1] [vec 0/1] [bg 0/1]; modes 0 none, 1 dmb ishld,
//        2 dmb ish, 3 ldr + dmb ishld, 4 dmb ishst. Reviewer result on the M4: 0 forbidden in 6M
//        with dmb ishld on performance cores and 2M on efficiency cores, about 10% with none.
//
// Native MP litmus: writer  str X ; <barrier> ; str Y   (plain stores, X optionally slowed)
//                   reader  ldar Y ; ldar X              (acquire loads, ordered)
// Forbidden (store->store reordered): Y==new && X==old.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread/qos.h>
#define LINE 128
static int mode, slow, vec; static long iters;
static volatile uint64_t *X, *Y, *Noise;
static volatile long go_w, go_r, done_w, done_r;
static uint64_t forbidden, seen_y;
static int qos_bg;
static void setqos(void){ pthread_set_qos_class_self_np(qos_bg?QOS_CLASS_BACKGROUND:QOS_CLASS_USER_INTERACTIVE,0); }
static void *writer(void *a){ setqos();
  for (long i=1;i<=iters;i++){ while(__atomic_load_n(&go_w,__ATOMIC_ACQUIRE)!=i); 
    uint64_t v=i; volatile uint64_t *x=X+((i&63)*LINE/8)*0, *y=Y;
    if (vec) {
      switch(mode){
      case 0: __asm__ volatile("dup v0.2d, %2\n str q0,[%0]\n str q0,[%1]\n"::"r"(x),"r"(y),"r"(v):"v0","memory");break;
      case 1: __asm__ volatile("dup v0.2d, %2\n str q0,[%0]\n dmb ishld\n str q0,[%1]\n"::"r"(x),"r"(y),"r"(v):"v0","memory");break;
      case 2: __asm__ volatile("dup v0.2d, %2\n str q0,[%0]\n dmb ish\n str q0,[%1]\n"::"r"(x),"r"(y),"r"(v):"v0","memory");break;
      case 3: __asm__ volatile("dup v0.2d, %2\n str q0,[%0]\n ldr q1,[%3]\n dmb ishld\n str q0,[%1]\n"::"r"(x),"r"(y),"r"(v),"r"(Noise+64):"v0","v1","memory");break;
      case 4: __asm__ volatile("dup v0.2d, %2\n str q0,[%0]\n dmb ishst\n str q0,[%1]\n"::"r"(x),"r"(y),"r"(v):"v0","memory");break;
      }
    } else {
      switch(mode){
      case 0: __asm__ volatile("str %2,[%0]\n str %2,[%1]\n"::"r"(x),"r"(y),"r"(v):"memory");break;
      case 1: __asm__ volatile("str %2,[%0]\n dmb ishld\n str %2,[%1]\n"::"r"(x),"r"(y),"r"(v):"memory");break;
      case 2: __asm__ volatile("str %2,[%0]\n dmb ish\n str %2,[%1]\n"::"r"(x),"r"(y),"r"(v):"memory");break;
      case 3: __asm__ volatile("str %2,[%0]\n ldr x9,[%3]\n dmb ishld\n str %2,[%1]\n"::"r"(x),"r"(y),"r"(v),"r"(Noise+64):"x9","memory");break;
      case 4: __asm__ volatile("str %2,[%0]\n dmb ishst\n str %2,[%1]\n"::"r"(x),"r"(y),"r"(v):"memory");break;
      }
    }
    __atomic_store_n(&done_w,i,__ATOMIC_RELEASE);} return 0; }
static void *reader(void *a){ setqos();
  for (long i=1;i<=iters;i++){ while(__atomic_load_n(&go_r,__ATOMIC_ACQUIRE)!=i);
    uint64_t y,x; long spin=0;
    do { __asm__ volatile("ldar %0,[%2]\n ldar %1,[%3]\n":"=&r"(y),"=&r"(x):"r"(Y),"r"(X):"memory"); } while (y!=(uint64_t)i && ++spin<200 && 1);
    if (y==(uint64_t)i){ seen_y++; if (x!=(uint64_t)i) forbidden++; }
    __atomic_store_n(&done_r,i,__ATOMIC_RELEASE);} return 0; }
static void *noiser(void *a){ setqos(); while(!__atomic_load_n(&done_w+0,__ATOMIC_RELAXED)||1){ if(slow==1) (void)*X; else if (slow==2) __atomic_fetch_add((uint64_t*)Noise,1,__ATOMIC_RELAXED); if(__atomic_load_n(&go_w,__ATOMIC_RELAXED)>iters) break;} return 0; }
int main(int c,char**v){ mode=atoi(v[1]); iters=atol(v[2]); slow=c>3?atoi(v[3]):0; vec=c>4?atoi(v[4]):0; qos_bg=c>5?atoi(v[5]):0;
  char *mem=aligned_alloc(16384, 16384*4); memset(mem,0,16384*4);
  X=(uint64_t*)(mem); Y=(uint64_t*)(mem+16384); Noise=(uint64_t*)(mem+32768);
  if (slow==2) Noise=X+2; /* same line as X, written by noiser: X store needs RFO */
  pthread_t a,b,n; pthread_create(&a,0,writer,0); pthread_create(&b,0,reader,0); if(slow) pthread_create(&n,0,noiser,0);
  setqos();
  for(long i=1;i<=iters;i++){ __atomic_store_n(&go_r,i,__ATOMIC_RELEASE); __atomic_store_n(&go_w,i,__ATOMIC_RELEASE);
    while(__atomic_load_n(&done_w,__ATOMIC_ACQUIRE)!=i||__atomic_load_n(&done_r,__ATOMIC_ACQUIRE)!=i);}
  __atomic_store_n(&go_w,iters+1,__ATOMIC_RELEASE);
  pthread_join(a,0); pthread_join(b,0); if(slow) pthread_join(n,0);
  const char*names[]={"none","dmb ishld","dmb ish","ldr+dmb ishld","dmb ishst"};
  printf("mode=%-14s slow=%d vec=%d bg=%d iters=%ld seenY=%llu forbidden=%llu\n",names[mode],slow,vec,qos_bg,iters,(unsigned long long)seen_y,(unsigned long long)forbidden); }
