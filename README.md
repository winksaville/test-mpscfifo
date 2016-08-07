test mpscfifo
===

Test Dmitry Vyukov's [non-intrusive mpsc fifo](http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue).

I contacted Dmitry and [asked](https://groups.google.com/forum/#!topic/lock-free/TC5uxElsQ3g/discussion)
what he thought the problem might be, he said:
```
This algorithm is not lock-free and is not linearizable.
If a producer is preempted between XCHG and next link update, the
linked list of nodes is temporary broken and all subsequent enqueues
are not visible to consumer until the list is restored.
This condition can be detected by looking at head/tail/next, and then
consumer can spin if necessary.
```

So the solution is to spin/loop if that is desired. What I've done is created
two versions of remove, rmv and rmv_non_stalling:
```
void add(MpscFifo_t *pQ, Msg_t *pMsg) {
  pMsg->pNext = NULL;
  void** ptr_pHead = (void*)&pQ->pHead;
  Msg_t* pPrev = __atomic_exchange_n(ptr_pHead, pMsg, __ATOMIC_SEQ_CST);
  // rmv will stall spinning if preemted at this critical spot
  pPrev->pNext = pMsg;
}

Msg_t *rmv_non_stalling(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if (pNext != NULL) {
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    return pTail;
  } else {
    return NULL;
  }
}

Msg_t *rmv(MpscFifo_t *pQ) {
  Msg_t* pTail = pQ->pTail;
  Msg_t* pNext = pTail->pNext;
  if ((pNext == NULL) && (pTail == pQ->pHead)) {
    // Q is empty
    return NULL;
  } else {
    if (pNext == NULL) {
      // Q is NOT empty but producer was preempted at the critical spot
      uint32_t i;
      for (i = 0; (pNext = pTail->pNext) == NULL; i++) {
        sched_yield();
      }
      DPF("rmv: Bad luck producer was prempted pQ=%p i=%d\n", pQ, i);
    }
    pTail->arg1 = pNext->arg1;
    pTail->arg2 = pNext->arg2;
    pQ->pTail = pNext;
    return pTail;
  }
}
```

In this version I'm still using _Atomic types and here is the generated code for clang:
```
0000000000400fd0 <add>:
  400fd0:       31 c0                   xor    %eax,%eax
  400fd2:       48 87 06                xchg   %rax,(%rsi)
  400fd5:       48 89 f0                mov    %rsi,%rax
  400fd8:       48 87 07                xchg   %rax,(%rdi)
  400fdb:       48 87 30                xchg   %rsi,(%rax)
  400fde:       c3                      retq   
  400fdf:       90                      nop

0000000000400fe0 <rmv_non_stalling>:
  400fe0:       48 8b 4f 08             mov    0x8(%rdi),%rcx
  400fe4:       48 8b 11                mov    (%rcx),%rdx
  400fe7:       31 c0                   xor    %eax,%eax
  400fe9:       48 85 d2                test   %rdx,%rdx
  400fec:       74 0f                   je     400ffd <rmv_non_stalling+0x1d>
  400fee:       0f 10 42 10             movups 0x10(%rdx),%xmm0
  400ff2:       0f 11 41 10             movups %xmm0,0x10(%rcx)
  400ff6:       48 87 57 08             xchg   %rdx,0x8(%rdi)
  400ffa:       48 89 c8                mov    %rcx,%rax
  400ffd:       c3                      retq   
  400ffe:       66 90                   xchg   %ax,%ax

0000000000401000 <rmv>:
  401000:       41 56                   push   %r14
  401002:       53                      push   %rbx
  401003:       50                      push   %rax
  401004:       49 89 fe                mov    %rdi,%r14
  401007:       49 8b 5e 08             mov    0x8(%r14),%rbx
  40100b:       48 8b 03                mov    (%rbx),%rax
  40100e:       48 85 c0                test   %rax,%rax
  401011:       75 1a                   jne    40102d <rmv+0x2d>
  401013:       49 8b 0e                mov    (%r14),%rcx
  401016:       31 c0                   xor    %eax,%eax
  401018:       48 39 cb                cmp    %rcx,%rbx
  40101b:       75 08                   jne    401025 <rmv+0x25>
  40101d:       eb 1d                   jmp    40103c <rmv+0x3c>
  40101f:       90                      nop
  401020:       e8 cb f7 ff ff          callq  4007f0 <sched_yield@plt>
  401025:       48 8b 03                mov    (%rbx),%rax
  401028:       48 85 c0                test   %rax,%rax
  40102b:       74 f3                   je     401020 <rmv+0x20>
  40102d:       0f 10 40 10             movups 0x10(%rax),%xmm0
  401031:       0f 11 43 10             movups %xmm0,0x10(%rbx)
  401035:       49 87 46 08             xchg   %rax,0x8(%r14)
  401039:       48 89 d8                mov    %rbx,%rax
  40103c:       48 83 c4 08             add    $0x8,%rsp
  401040:       5b                      pop    %rbx
  401041:       41 5e                   pop    %r14
  401043:       c3                      retq   
  401044:       66 2e 0f 1f 84 00 00    nopw   %cs:0x0(%rax,%rax,1)
  40104b:       00 00 00 
  40104e:       66 90                   xchg   %ax,%ax
```

And here is a run:
```
wink@wink-desktop:~/prgs/test-mpscfifo (master)
$ make clean ; make CC=clang && time make run client_count=12 loops=2000000 msg_count=10000
clang -Wall -std=c11 -O2 -g -pthread -c test.c -o test.o
clang -Wall -std=c11 -O2 -g -pthread -c mpscfifo.c -o mpscfifo.o
clang -Wall -std=c11 -O2 -g -pthread test.o mpscfifo.o -o test
objdump -d test > test.txt
test client_count=12 loops=2000000 msg_count=10000
multi_thread_msg:+client_count=12 loops=2000000 msg_count=10000
multi_thread_msg: &msgs[0]=0x7efc8d730010 &msgs[1]=0x7efc8d730030 sizeof(Msg_t)=32(0x20)
multi_thread_msg: &pool=0x7ffc32da8b88, &pHead=0x7ffc32da8b88, &pTail=0x7ffc32da8b90 sizeof(pool)=16(0x10)
multi_thread_msg: created 12 clients
multi_thread_msg: done, joining 12 clients
multi_thread_msg: msgs_processed=24000000 msgs_sent=24000000 no_msgs_count=0 not_ready_client_count=0
multi_thread_msg:-error=0

Success

real    0m29.440s
user    1m0.953s
sys     3m14.657s
```

With gcc it uses mfence instead of xchg instructions:
```
0000000000401030 <add>:
  401030:       48 c7 06 00 00 00 00    movq   $0x0,(%rsi)
  401037:       48 89 f0                mov    %rsi,%rax
  40103a:       0f ae f0                mfence 
  40103d:       48 87 07                xchg   %rax,(%rdi)
  401040:       48 89 30                mov    %rsi,(%rax)
  401043:       0f ae f0                mfence 
  401046:       c3                      retq   
  401047:       66 0f 1f 84 00 00 00    nopw   0x0(%rax,%rax,1)
  40104e:       00 00 

0000000000401090 <rmv_non_stalling>:
  401090:       48 8b 47 08             mov    0x8(%rdi),%rax
  401094:       48 8b 10                mov    (%rax),%rdx
  401097:       48 85 d2                test   %rdx,%rdx
  40109a:       74 1c                   je     4010b8 <rmv_non_stalling+0x28>
  40109c:       48 8b 4a 10             mov    0x10(%rdx),%rcx
  4010a0:       48 89 48 10             mov    %rcx,0x10(%rax)
  4010a4:       48 8b 4a 18             mov    0x18(%rdx),%rcx
  4010a8:       48 89 48 18             mov    %rcx,0x18(%rax)
  4010ac:       48 89 57 08             mov    %rdx,0x8(%rdi)
  4010b0:       0f ae f0                mfence 
  4010b3:       c3                      retq   
  4010b4:       0f 1f 40 00             nopl   0x0(%rax)
  4010b8:       31 c0                   xor    %eax,%eax
  4010ba:       c3                      retq   
  4010bb:       0f 1f 44 00 00          nopl   0x0(%rax,%rax,1)

00000000004010c0 <rmv>:
  4010c0:       55                      push   %rbp
  4010c1:       53                      push   %rbx
  4010c2:       48 89 fd                mov    %rdi,%rbp
  4010c5:       48 83 ec 08             sub    $0x8,%rsp
  4010c9:       48 8b 5f 08             mov    0x8(%rdi),%rbx
  4010cd:       48 8b 03                mov    (%rbx),%rax
  4010d0:       48 85 c0                test   %rax,%rax
  4010d3:       74 2b                   je     401100 <rmv+0x40>
  4010d5:       48 8b 50 10             mov    0x10(%rax),%rdx
  4010d9:       48 89 53 10             mov    %rdx,0x10(%rbx)
  4010dd:       48 8b 50 18             mov    0x18(%rax),%rdx
  4010e1:       48 89 53 18             mov    %rdx,0x18(%rbx)
  4010e5:       48 89 45 08             mov    %rax,0x8(%rbp)
  4010e9:       0f ae f0                mfence 
  4010ec:       48 83 c4 08             add    $0x8,%rsp
  4010f0:       48 89 d8                mov    %rbx,%rax
  4010f3:       5b                      pop    %rbx
  4010f4:       5d                      pop    %rbp
  4010f5:       c3                      retq   
  4010f6:       66 2e 0f 1f 84 00 00    nopw   %cs:0x0(%rax,%rax,1)
  4010fd:       00 00 00 
  401100:       48 8b 07                mov    (%rdi),%rax
  401103:       48 39 c3                cmp    %rax,%rbx
  401106:       75 0d                   jne    401115 <rmv+0x55>
  401108:       eb 15                   jmp    40111f <rmv+0x5f>
  40110a:       66 0f 1f 44 00 00       nopw   0x0(%rax,%rax,1)
  401110:       e8 7b f6 ff ff          callq  400790 <sched_yield@plt>
  401115:       48 8b 03                mov    (%rbx),%rax
  401118:       48 85 c0                test   %rax,%rax
  40111b:       74 f3                   je     401110 <rmv+0x50>
  40111d:       eb b6                   jmp    4010d5 <rmv+0x15>
  40111f:       31 db                   xor    %ebx,%ebx
  401121:       eb c9                   jmp    4010ec <rmv+0x2c>
  401123:       0f 1f 00                nopl   (%rax)
  401126:       66 2e 0f 1f 84 00 00    nopw   %cs:0x0(%rax,%rax,1)
  40112d:       00 00 00 
```

And is running slightly slower:
```
wink@wink-desktop:~/prgs/test-mpscfifo (master)
$ make clean ; make CC=gcc && time make run client_count=12 loops=2000000 msg_count=10000
gcc -Wall -std=c11 -O2 -g -pthread -c test.c -o test.o
gcc -Wall -std=c11 -O2 -g -pthread -c mpscfifo.c -o mpscfifo.o
gcc -Wall -std=c11 -O2 -g -pthread test.o mpscfifo.o -o test
objdump -d test > test.txt
test client_count=12 loops=2000000 msg_count=10000
multi_thread_msg:+client_count=12 loops=2000000 msg_count=10000
multi_thread_msg: &msgs[0]=0x7f729c65a010 &msgs[1]=0x7f729c65a030 sizeof(Msg_t)=32(0x20)
multi_thread_msg: &pool=0x7ffe10da45a0, &pHead=0x7ffe10da45a0, &pTail=0x7ffe10da45a8 sizeof(pool)=16(0x10)
multi_thread_msg: created 12 clients
multi_thread_msg: done, joining 12 clients
multi_thread_msg: msgs_processed=24000000 msgs_sent=24000000 no_msgs_count=0 not_ready_client_count=0
multi_thread_msg:-error=0

Success

real    0m35.483s
user    1m29.250s
sys     3m31.450s
```

My computer is running Arch Linux updated as of 7/6/2016 with a 4.6.3-1-ARCH kernel:
```
$ uname -a
Linux wink-desktop 4.6.3-1-ARCH #1 SMP PREEMPT Fri Jun 24 21:19:13 CEST 2016 x86_64 GNU/Linux
```

And the cpuinfo is:
```
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
CPU MHz:               1199.988
CPU max MHz:           3600.0000
CPU min MHz:           1200.0000
BogoMIPS:              6599.64
Virtualization:        VT-x
L1d cache:             32K
L1i cache:             32K
L2 cache:              256K
L3 cache:              15360K
NUMA node0 CPU(s):     0-11
Flags:                 fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse sse2 ss ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc arch_perfmon pebs bts rep_good nopl xtopology nonstop_tsc aperfmperf eagerfpu pni pclmulqdq dtes64 monitor ds_cpl vmx est tm2 ssse3 sdbg fma cx16 xtpr pdcm pcid dca sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand lahf_lm abm epb tpr_shadow vnmi flexpriority ept vpid fsgsbase tsc_adjust bmi1 avx2 smep bmi2 erms invpcid cqm xsaveopt cqm_llc cqm_occup_llc dtherm ida arat pln pts
```

And my meminfo is:
```
$ cat /proc/meminfo 
MemTotal:       16336308 kB
MemFree:        15454296 kB
MemAvailable:   15600908 kB
Buffers:            2620 kB
Cached:           402316 kB
SwapCached:            0 kB
Active:           420372 kB
Inactive:         253052 kB
Active(anon):     269004 kB
Inactive(anon):    17996 kB
Active(file):     151368 kB
Inactive(file):   235056 kB
Unevictable:          32 kB
Mlocked:              32 kB
SwapTotal:        249096 kB
SwapFree:         249096 kB
Dirty:                 0 kB
Writeback:             0 kB
AnonPages:        256288 kB
Mapped:           167288 kB
Shmem:             18512 kB
Slab:              62892 kB
SReclaimable:      28068 kB
SUnreclaim:        34824 kB
KernelStack:        4832 kB
PageTables:        10776 kB
NFS_Unstable:          0 kB
Bounce:                0 kB
WritebackTmp:          0 kB
CommitLimit:     8417248 kB
Committed_AS:    1312656 kB
VmallocTotal:   34359738367 kB
VmallocUsed:           0 kB
VmallocChunk:          0 kB
HardwareCorrupted:     0 kB
AnonHugePages:    124928 kB
HugePages_Total:       0
HugePages_Free:        0
HugePages_Rsvd:        0
HugePages_Surp:        0
Hugepagesize:       2048 kB
DirectMap4k:      145460 kB
DirectMap2M:     3948544 kB
DirectMap1G:    14680064 kB
```
