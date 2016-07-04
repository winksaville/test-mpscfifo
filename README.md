test mpscfifo
===

Test Dmitry Vyukov's (non-intrusive mpsc fifo)[http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue].
Currently its not working correctly I'm seeing NULL being returned when the fifo
is not empty. Below I sent 18M+ messages and 18 times I hit the bug as you can
see in the output.
```
$ make clean && make CC=gcc && time make run client_count=10 loops=2000000 msg_count=21gcc -Wall -std=c11 -O2 -g -pthread -c test.c -o test.o
gcc -Wall -std=c11 -O2 -g -pthread -c mpscfifo.c -o mpscfifo.o
gcc -Wall -std=c11 -O2 -g -pthread test.o mpscfifo.o -o test
objdump -d test > test.txt
test client_count=10 loops=2000000
multi_thread_msg:+client_count=10 loops=2000000 msg_count=21
multi_thread_msg: &msgs[0]=0x24d3420 &msgs[1]=0x24d3430 sizeof(Msg_t)=16(0x10)
multi_thread_msg: &fifo=0x7fffa22b0990, &pHead=0x7fffa22b0990, &pTail=0x7fffa22b0998 sizeof(fifo)=24(0x18)
multi_thread_msg: after creating pool fifo.count=21
multi_thread_msg: created 10 clients
rmv: BUG pNext == NULL but initial_count=16
rmv: BUG pNext == NULL but initial_count=17
rmv: BUG pNext == NULL but initial_count=17
rmv: BUG pNext == NULL but initial_count=18
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=19
rmv: BUG pNext == NULL but initial_count=15
rmv: BUG pNext == NULL but initial_count=20
rmv: BUG pNext == NULL but initial_count=20
rmv: BUG pNext == NULL but initial_count=20
multi_thread_msg: done, joining 10 clients
multi_thread_msg: fifo.count=21
multi_thread_msg: fifo had 21 msgs expected 21 fifo.count=0
multi_thread_msg: sum=20000000 expected_value=20000000
multi_thread_msg: msgs_count=18200098 no_msgs_count=18 not_ready_client_count=1799884
multi_thread_msg:-error=0

Success

real	1m5.492s
user	0m0.000s
sys	0m0.000s
```

This was compiled with gcc verion 6.1.1 on a desktop running Linux kernel 4.6.3.1
using Arch Linux up to date as of 7/4/2016 and an x86_64 i7-5820 cpu:
```
$ gcc --version
gcc (GCC) 6.1.1 20160602
Copyright (C) 2016 Free Software Foundation, Inc.
This is free software; see the source for copying conditions.  There is NO
warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

$ uname -a
Linux wink-desktop 4.6.3-1-ARCH #1 SMP PREEMPT Fri Jun 24 21:19:13 CEST 2016 x86_64 GNU/Linux

$ lscpu
Architecture:          x86_64
CPU op-mode(s):        32-bit, 64-bit
Byte Order:            Little Endian
CPU(s):                12
On-line CPU(s) list:   0-11
Thread(s) per core:    2
Core(s) per socket:    6
Socket(s):             1
NUMA node(s):          1
Vendor ID:             GenuineIntel
CPU family:            6
Model:                 63
Model name:            Intel(R) Core(TM) i7-5820K CPU @ 3.30GHz
Stepping:              2
CPU MHz:               3298.382
CPU max MHz:           3600.0000
CPU min MHz:           1200.0000
BogoMIPS:              6599.62
Virtualization:        VT-x
L1d cache:             32K
L1i cache:             32K
L2 cache:              256K
L3 cache:              15360K
NUMA node0 CPU(s):     0-11
Flags:                 fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse sse2 ss ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc arch_perfmon pebs bts rep_good nopl xtopology nonstop_tsc aperfmperf eagerfpu pni pclmulqdq dtes64 monitor ds_cpl vmx est tm2 ssse3 sdbg fma cx16 xtpr pdcm pcid dca sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand lahf_lm abm epb tpr_shadow vnmi flexpriority ept vpid fsgsbase tsc_adjust bmi1 avx2 smep bmi2 erms invpcid cqm xsaveopt cqm_llc cqm_occup_llc dtherm ida arat pln pts
```

I'm using _Atomic types for the critial pointers but have also used regular types and
__atomics__xxx and see the same problems. Here is the source for add & rmv:
```
void add(MpscFifo_t *pQ, Msg_t *pMsg) {
  pMsg->pNext = NULL;
  void** ptr_pHead = (void*)&pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_SEQ_CST);
  pPrev->pNext = pMsg;
  pQ->count += 1;
}

Msg_t *rmv(MpscFifo_t *pQ) {
  uint32_t initial_count = pQ->count;
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if (pNext != NULL) {
    pTail->data = pNext->data;
    pQ->pTail = pNext;
    pQ->count -= 1;
    return pTail;
  } else {
    if (initial_count != 0) {
      printf("rmv: BUG pNext == NULL but initial_count=%d\n", initial_count);
    }
    return NULL;
  }
}
```

And here is the generated code for add & rmv from test.txt and it looks
'correct' to me:
```
0000000000400ff0 <add>:
  400ff0:	48 c7 06 00 00 00 00 	movq   $0x0,(%rsi)
  400ff7:	48 89 f0             	mov    %rsi,%rax
  400ffa:	0f ae f0             	mfence 
  400ffd:	48 87 07             	xchg   %rax,(%rdi)
  401000:	48 89 30             	mov    %rsi,(%rax)
  401003:	0f ae f0             	mfence 
  401006:	f0 83 47 10 01       	lock addl $0x1,0x10(%rdi)
  40100b:	c3                   	retq   
  40100c:	0f 1f 40 00          	nopl   0x0(%rax)

0000000000401010 <rmv>:
  401010:	53                   	push   %rbx
  401011:	8b 77 10             	mov    0x10(%rdi),%esi
  401014:	48 8b 47 08          	mov    0x8(%rdi),%rax
  401018:	48 8b 18             	mov    (%rax),%rbx
  40101b:	48 85 db             	test   %rbx,%rbx
  40101e:	74 20                	je     401040 <rmv+0x30>
  401020:	48 8b 4b 08          	mov    0x8(%rbx),%rcx
  401024:	48 8d 57 10          	lea    0x10(%rdi),%rdx
  401028:	48 89 48 08          	mov    %rcx,0x8(%rax)
  40102c:	48 89 5f 08          	mov    %rbx,0x8(%rdi)
  401030:	0f ae f0             	mfence 
  401033:	f0 83 2a 01          	lock subl $0x1,(%rdx)
  401037:	48 89 c3             	mov    %rax,%rbx
  40103a:	48 89 d8             	mov    %rbx,%rax
  40103d:	5b                   	pop    %rbx
  40103e:	c3                   	retq   
  40103f:	90                   	nop
  401040:	85 f6                	test   %esi,%esi
  401042:	74 f6                	je     40103a <rmv+0x2a>
  401044:	bf a8 14 40 00       	mov    $0x4014a8,%edi
  401049:	31 c0                	xor    %eax,%eax
  40104b:	e8 30 f7 ff ff       	callq  400780 <printf@plt>
  401050:	48 89 d8             	mov    %rbx,%rax
  401053:	5b                   	pop    %rbx
  401054:	c3                   	retq   
  401055:	66 2e 0f 1f 84 00 00 	nopw   %cs:0x0(%rax,%rax,1)
  40105c:	00 00 00 
  40105f:	90                   	nop

```
