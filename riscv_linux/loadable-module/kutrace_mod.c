/*
 * kutrace_mod.c
 *
 * Author: Richard Sites <dick.sites@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Signed-off-by: Richard Sites <dick.sites@gmail.com>
 */

/*
 * A module that implements kernel/user tracing
 * dsites 2019.02.19
 *
 * See include/linux/kutrace.h for struct definitions
 *
 * Most patches will be something like
 *   kutrace1(event, arg) which calls trace_1 here
 *
 */


/*
 * kutrace.c -- kernel/user tracing implementation
 * dsites 2019.02.14 Reworked for the 4.19 kernel, from dclab_trace.c 
 * dsites 2020.02.04 fixed getclaim(n) bug for n > 1 
 * dsites 2020.04.02 added code for timecounter wraparound (e.g. Armv7) 
 * dsites 2020.05.01 use #if to cover Intel x86-64, AMD, Pi-4B, Pi-zero 
 * dsites 2020.10.30 Add packet trace parameters
 *  use something like 
 *  sudo insmod kutrace_mod.ko tracemb=20 pktmask=0x0000000f pktmatch=0xd1c517e5
 *  default is the above
 *  pktmask=0 traces nothing, pktmask=-1 traces all (no pktmatch needed)
 * dsites 2021.09.25 Add Rpi-4B 64-bit support
 *
 * dsites 2022.03.10 Start on riscv support
 */

#include <linux/kutrace.h>

#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/types.h>	/* u64, among others */
#include <linux/vmalloc.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard L Sites");


// GCC compiler options to distinguish build targets
// Use to get these:
//   gcc -dM -E -march=native - < /dev/null 
//   gcc -dM -E -march=rv64id - < /dev/null 

// #define __ARM_ARCH 6		pizero
// #define __ARM_ARCH 8		pi4
// #if defined(__aarch64__)	neither
// #if defined( __ARM_ARCH_ISA_ARM )  all
// #define __core_avx2 1	i3
// #define __SSE4A__ 1		ryzen
// #define __x86_64 1 		both
// new look at data 2021.04.05
// i3-7100 __haswell
// ryzen 2200G __znver1	(e.g. ryzen version 1)
// hal's i5-3570 and i7-4790 __k8
// hal's atom 330 and D2500 __atom __bonnell
// __riscv HiFive Unmatched


/* Add others as you find and test them */
/* Some early Intel x86-64 builds identify as k8 */
#define Isx86_64	defined(__x86_64)
#define IsIntel64	(!defined(ryzen)) && (defined(__haswell) || defined(__k8) || defined(__bonnell))
#define IsAmd64		defined(ryzen) || defined(__znver1) 

#define IsRPi4		defined(__ARM_ARCH) && (__ARM_ARCH == 8)
#define IsRPi0 		defined(__ARM_ARCH) && (__ARM_ARCH == 6)
#define IsArm32		defined(__ARM_ARCH_ISA_ARM) && !defined(__aarch64__)
#define IsArm64		defined(__aarch64__)
#define IsRPi4_32	IsRPi4 && IsArm32
#define IsRPi4_64	IsRPi4 && IsArm64
#define IsRiscv		defined(__riscv)


/* CPU-specific includes          */
/*--------------------------------*/
#if IsRiscv
#include <asm/timex.h>	/* for get_cycles */
#endif

/* AMD-specific defines           */
/*--------------------------------*/
/* From Open-Source Register Reference For AMD Family 17h Processors */
/*   Models 00h-2Fh */

/* rdtsc counts cycles, no setup needed */

/* IRPerfCount counts instructions retired, once set up */
#define IRPerfCount		0xC00000E9

#define RYZEN_HWCR 		0xC0010015
#define IRPerfEn		(1L << 30)

/* PStateStat<2:0> gives current P-state of a core */
/* PStateDefn<13:8> Did gives frequency divisor in increments of 1/8 */
/* PStateDefn<7:0> Fid gives frequency in increments of 25 */
/* I think all this boils down to freq = Fid * 200 / Did, but it could be 266.67 */
#define PStateStat 		0xC0010063
#define PStateDef0		0xC0010064
#define PStateDef1		0xC0010065
#define PStateDef2		0xC0010066
#define PStateDef3		0xC0010067
#define PStateDef4		0xC0010068
#define PStateDef5		0xC0010069
#define PStateDef6		0xC001006A
#define PStateDef7		0xC001006B
#define PStat_MASK		0x07LU
#define CpuDid_SHIFT		8
#define CpuDid_MASK		0x3FLU
#define CpuFid_SHIFT		0
#define CpuFid_MASK		0xFFLU

// amd notes
// FIDVID_STATUS HwPstate
// MSRC001_006[4...B] [P-state [7:0]] (Core::X86::Msr::PStateDef)
// freq = <7:0> * 25 MHz * CpuDid in <13:8> VCO
// From https://developer.amd.com/wp-content/resources/56255_3_03.PDF
// sudo watch -n 1 cpupower monitor


/* Intel-specific defines         */
/*--------------------------------*/
/* From Intel® 64 and IA-32 Architectures Software Developer’s Manual */
/*   Volume 4: Model-Specific Registers */

/* rdtsc counts cycles, no setup needed */

/* IA32_FIXED_CTR0 counts instructions retired, once set up */
#define IA32_FIXED_CTR0		0x309

#define IA32_FIXED_CTR_CTRL	0x38D
#define EN0_OS			(1L << 0)
#define EN0_Usr			(1L << 1)
#define EN0_Anythread		(1L << 2)
#define EN0_PMI			(1L << 3)
#define EN0_ALL			(EN0_OS | EN0_Usr | EN0_Anythread | EN0_PMI)

#define IA32_PERF_GLOBAL_CTRL	0x38F
#define EN_FIXED_CTR0		(1L << 32)

/* MSR_IA32_PERF_STATUS<15:8> gives current CPU frequency in increments of 100 MHz */
#define MSR_PERF_STATUS		0x198
#define FID_SHIFT		8
#define FID_MASK		0xFFL


/* Arm-speficic defines           */
/*--------------------------------*/
/* From linux-4.19.19/arch/arm/kernel/perf_event_v6.c */

#define ARMV6_PMCR_ENABLE		(1 << 0)  /* enable all counters */
#define ARMV6_PMCR_CTR01_RESET		(1 << 1)
#define ARMV6_PMCR_CCOUNT_RESET		(1 << 2)
#define ARMV6_PMCR_CCOUNT_DIV		(1 << 3)  /* 1 = div 64 */
#define ARMV6_PMCR_COUNT0_IEN		(1 << 4)  /* instr ena irq */
#define ARMV6_PMCR_COUNT1_IEN		(1 << 5)
#define ARMV6_PMCR_CCOUNT_IEN		(1 << 6)  /* CCNT ena irq */
#define ARMV6_PMCR_COUNT0_OVERFLOW	(1 << 8)
#define ARMV6_PMCR_COUNT1_OVERFLOW	(1 << 9)
#define ARMV6_PMCR_CCOUNT_OVERFLOW	(1 << 10)
#define ARMV6_PMCR_EVT_COUNT0_SHIFT	20
#define ARMV6_PMCR_EVT_COUNT0_MASK	(0xFF << ARMV6_PMCR_EVT_COUNT0_SHIFT)
#define ARMV6_PMCR_EVT_COUNT1_SHIFT	12
#define ARMV6_PMCR_EVT_COUNT1_MASK	(0xFF << ARMV6_PMCR_EVT_COUNT1_SHIFT)

enum armv6_perf_types {
	ARMV6_PERFCTR_ICACHE_MISS	    = 0x0,
	ARMV6_PERFCTR_IBUF_STALL	    = 0x1,
	ARMV6_PERFCTR_DDEP_STALL	    = 0x2,
	ARMV6_PERFCTR_ITLB_MISS		    = 0x3,
	ARMV6_PERFCTR_DTLB_MISS		    = 0x4,
	ARMV6_PERFCTR_BR_EXEC		    = 0x5,
	ARMV6_PERFCTR_BR_MISPREDICT	    = 0x6,
	ARMV6_PERFCTR_INSTR_EXEC	    = 0x7,
	ARMV6_PERFCTR_DCACHE_HIT	    = 0x9,
	ARMV6_PERFCTR_DCACHE_ACCESS	    = 0xA,
	ARMV6_PERFCTR_DCACHE_MISS	    = 0xB,
	ARMV6_PERFCTR_DCACHE_WBACK	    = 0xC,
	ARMV6_PERFCTR_SW_PC_CHANGE	    = 0xD,
	ARMV6_PERFCTR_MAIN_TLB_MISS	    = 0xF,
	ARMV6_PERFCTR_EXPL_D_ACCESS	    = 0x10,
	ARMV6_PERFCTR_LSU_FULL_STALL	    = 0x11,
	ARMV6_PERFCTR_WBUF_DRAINED	    = 0x12,
	ARMV6_PERFCTR_CPU_CYCLES	    = 0xFF,
	ARMV6_PERFCTR_NOP		    = 0x20,
};

#define ARMV7_PERFCTR_PMNC_SW_INCR			0x00
#define ARMV7_PERFCTR_L1_ICACHE_REFILL			0x01
#define ARMV7_PERFCTR_ITLB_REFILL			0x02
#define ARMV7_PERFCTR_L1_DCACHE_REFILL			0x03
#define ARMV7_PERFCTR_L1_DCACHE_ACCESS			0x04
#define ARMV7_PERFCTR_DTLB_REFILL			0x05
#define ARMV7_PERFCTR_MEM_READ				0x06
#define ARMV7_PERFCTR_MEM_WRITE				0x07
#define ARMV7_PERFCTR_INSTR_EXECUTED			0x08
#define ARMV7_PERFCTR_EXC_TAKEN				0x09
#define ARMV7_PERFCTR_EXC_EXECUTED			0x0A
#define ARMV7_PERFCTR_CID_WRITE				0x0B

#define ARMV7_PMNC_E		(1 << 0) /* Enable all counters */
#define ARMV7_PMNC_P		(1 << 1) /* Reset all counters */
#define ARMV7_PMNC_C		(1 << 2) /* Cycle counter reset */
#define ARMV7_PMNC_D		(1 << 3) /* CCNT counts every 64th cpu cycle */
#define ARMV7_PMNC_X		(1 << 4) /* Export to ETM */
#define ARMV7_PMNC_DP		(1 << 5) /* Disable CCNT if non-invasive debug*/
#define	ARMV7_PMNC_N_SHIFT	11	 /* Number of counters supported */
#define	ARMV7_PMNC_N_MASK	0x1f
#define	ARMV7_PMNC_MASK		0x3f	 /* Mask for writable bits */

#define	ARMV7_IDX_CYCLE_COUNTER	0
#define	ARMV7_IDX_COUNTER0	1

#define	ARMV7_EVTYPE_MASK	0xc80000ff	/* Mask for writable bits */
#define	ARMV7_EVTYPE_EVENT	0xff		/* Mask for EVENT bits */



#if IsArm32

/* This is for 32-bit ARM */
typedef long long int int64;
typedef long long unsigned int uint64;
#define FLX "%016llx"
#define FLD "%lld"
#define FUINTPTRX "%08lx"
#define CL(x) x##LL
#define CLU(x) x##LLU
#define ATOMIC_READ atomic_read
#define ATOMIC_SET atomic_set
#define ATOMIC_ADD_RETURN atomic_add_return

#elif IsArm64

/* This is for 64-bit ARM */
typedef long long int int64;
typedef long long unsigned int uint64;
#define FLX "%016llx"
#define FLD "%lld"
#define FUINTPTRX "%016lx"
#define CL(x) x##LL
#define CLU(x) x##LLU
#define ATOMIC_READ atomic64_read
#define ATOMIC_SET atomic64_set
#define ATOMIC_ADD_RETURN atomic64_add_return

#elif IsRiscv

/* This is for 64-bit RISC-V */
typedef long long int int64;
typedef long long unsigned int uint64;
#define FLX "%016llx"
#define FLD "%lld"
#define FUINTPTRX "%016lx"
#define CL(x) x##LL
#define CLU(x) x##LLU
#define ATOMIC_READ arch_atomic64_read
#define ATOMIC_SET arch_atomic64_set
#define ATOMIC_ADD_RETURN arch_atomic64_add_return_relaxed
#define ATOMIC_FETCH_ADD arch_atomic64_fetch_add_relaxed

#elif Isx86_64

/* This is for 64-bit X86 */
typedef long int int64;
typedef long unsigned int uint64;
#define FLX "%016lx"
#define FLD "%ld"
#define FUINTPTRX "%016lx"
#define CL(x) x##L
#define CLU(x) x##LU
#define ATOMIC_READ atomic64_read
#define ATOMIC_SET atomic64_set
#define ATOMIC_ADD_RETURN atomic64_add_return

#else

#error Need type defines for your architecture

#endif


#if IsIntel64
#define BCLK_FREQ 100LU		/* CPU Intel base clock assume 100 MHz */

#elif IsAmd64
#define BCLK_FREQ 200LU		/* CPU Ryzen base clock assume 25 MHz * 8 */

#elif IsRiscv
#define BCLK_FREQ 1196LU	/* CPU riscv base clock assume 1196 GHz (26*46) */

#else
#define BCLK_FREQ 0LU	/* CPU RPi frequency sampling not implemented -- change notifications used */

#endif

/* Forward declarations */
static u64 kutrace_control(u64 command, u64 arg);
static int __init kutrace_mod_init(void);

/* For the flags byte in traceblock[1] */
#define IPC_Flag CLU(0x80)
#define WRAP_Flag CLU(0x40)

/* Incoming arg to do_reset  */
#define DO_IPC 1
#define DO_WRAP 2

/* Module parameter: default how many MB of kernel trace memory to reserve */
/* This is for the standalone, non-module version */
/* static const long int kTraceMB = 32; */

/* Version number of this kernel tracing code */
static const u64 kModuleVersionNumber = 3;

/* The time counter may wrap around while we are tracing if it */
/* is only 32 or 40 bits wide. This constant is added at new traceblock */
/* initialization of full timestamp if that happens. */
/* Set up for 32-bit Armv7. */

#if IsArm32
static const u64 kCounterWrapIncrease = 0x0000000100000000LLU;
static const u64 kCounterWrapMask =     0xFFFFFFFF00000000LLU;
#else
static const u64 kCounterWrapIncrease = 0x0000000000000000LLU;
static const u64 kCounterWrapMask =     0x0000000000000000LLU;
#endif


/* A few global variables */

/* Previous blockinit counter value, for detecting wraparound */
static u64 prior_block_init_counter;	/* Initially zero */

/* IPC Inctructions per cycle flag */
static bool do_ipc;	/* Initially false */

/* Wraparound tracing vs. stop when buffer is full */
static bool do_wrap;	/* Initially false */

/* Module parameter: default how many MB of kernel trace memory to reserve */
static long int tracemb = 2;

/* Module parameters: packet filtering. Initially match just dclab RPC markers */
static long int pktmask  = 0x0000000f;
static long int pktmatch = 0xd1c517e5;

module_param(tracemb, long, S_IRUSR);
MODULE_PARM_DESC(tracemb, "MB of kernel trace memory to reserve");

module_param(pktmask, long, S_IRUSR);
MODULE_PARM_DESC(pktmask, "Bit-per-byte of which bytes to use in hash");
module_param(pktmatch, long, S_IRUSR);
MODULE_PARM_DESC(pktmatch, "Matching hash value");



/* These four are exported by our patched kernel. 
 * See linux-4.19.19/kernel/kutrace/kutrace.c
 */
extern bool kutrace_tracing;
extern struct kutrace_ops kutrace_global_ops;
extern u64* kutrace_pid_filter;
DECLARE_PER_CPU(struct kutrace_traceblock, kutrace_traceblock_per_cpu);


/*
 * Individual trace entries are at least one u64, with this format:
 *
 *  +-------------------+-----------+-------+-------+-------+-------+
 *  | timestamp         | event     | delta | retval|      arg0     |
 *  +-------------------+-----------+-------+-------+-------+-------+
 *           20              12         8       8           16
 *
 * timestamp: low 20 bits of some free-running time counter in the
 *   10-40 MHz range. For ARM, this is the 32 MHz cntvct_el0.
 * event: traced event number, syscall N, sysreturn N, etc.
 *   See user-mode kutrace_lib.h for the full set.
 *   matching call and return events differ just in one event bit.
 * delta: for optimized call-return, return timestamp - call timestamp,
 *   else zero.
 * retval: for optimized call-return, the low 8 bits of the return value,
 *   else zero.
 * arg0: for syscall, the low 16 bits of the first argument to the syscall,
 *   else zero. CALLER does the AND
 *
 * Multi-u64 entries have a count 1-8 in the middle 4 bits of event.
 *   These events are all in the range 0x000 to 0x1ff with the middle
 *   four bits non-zero.
 *
 * The first word of each 64KB block has this format:
 *  +-------+-------------------------------------------------------+
 *  |  cpu# |  full timestamp                                       |
 *  +-------+-------------------------------------------------------+
 *        56                                                       0
 *
 * The second word of each 64KB block has this format:
 *  +-------+-------------------------------------------------------+
 *  | flags |  gettimeofday() value to be filled in by user code    |
 *  +-------+-------------------------------------------------------+
 *        56                                                       0
 *
 */

#define ARG0_MASK      CLU(0x000000000000ffff)
#define RETVAL_MASK    CLU(0x0000000000ff0000)
#define DELTA_MASK     CLU(0x00000000ff000000)
#define EVENT_MASK     CLU(0x00000fff00000000)
#define TIMESTAMP_MASK CLU(0xfffff00000000000)
#define EVENT_DELTA_RETVAL_MASK (EVENT_MASK | DELTA_MASK | RETVAL_MASK)
#define EVENT_RETURN_BIT        CLU(0x0000020000000000)
#define EVENT_LENGTH_FIELD_MASK CLU(0x000000000000000f)

#define UNSHIFTED_RETVAL_MASK CLU(0x00000000000000ff)
#define UNSHIFTED_DELTA_MASK  CLU(0x00000000000000ff)
#define UNSHIFTED_EVENT_MASK  CLU(0x0000000000000fff)
#define UNSHIFTED_TIMESTAMP_MASK   CLU(0x00000000000fffff)
#define UNSHIFTED_EVENT_RETURN_BIT CLU(0x0000000000000200)
#define UNSHIFTED_EVENT_HAS_RETURN_MASK CLU(0x0000000000000c00)

#define MIN_EVENT_WITH_LENGTH CLU(0x010)
#define MAX_EVENT_WITH_LENGTH CLU(0x1ff)
#define MAX_DELTA_VALUE 255
#define MAX_PIDNAME_LENGTH 16

#define RETVAL_SHIFT 16
#define DELTA_SHIFT 24
#define EVENT_SHIFT 32
#define TIMESTAMP_SHIFT 44
#define EVENT_LENGTH_FIELD_SHIFT 4

#define FULL_TIMESTAMP_MASK CLU(0x00ffffffffffffff)
#define CPU_NUMBER_SHIFT 56

#define GETTIMEOFDAY_MASK   CLU(0x00ffffffffffffff)
#define FLAGS_SHIFT 56


/*
 * Trace memory is consumed backward, high to low
 * This allows valid test for full block even if an interrupt routine
 * switches to a new block mid-test. The condition tracebase == NULL
 * means that initialization needs to be called.
 *
 * Per-CPU trace blocks are 64KB, contining 8K u64 items. A trace entry is
 * 1-8 items. Trace entries do not cross block boundaries.
 *
 */
char *tracebase;	/* Initially NULL address of kernel trace memory */
u64 *traceblock_high;		/* just off high end of trace memory */
u64 *traceblock_limit;		/* at low end of trace memory */
u64 *traceblock_next;		/* starts at high, moves down to limit */
bool did_wrap_around;

/*
 * Trace memory layout without IPC tracing.
 *  tracebase
 *  traceblock_limit          traceblock_next                traceblock_high
 *  |                               |                                |
 *  v                               v                                v
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *  | / / / / / / / / / / / / / / / |                               |
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *                                  <==== allocated blocks grow down
 *
 *
 * Trace memory layout with IPC tracing. IPC bytes go into lower 1/8.
 *  tracebase
 *  |    traceblock_limit     traceblock_next                traceblock_high
 *  |       |                       |                                |
 *  v       v                       v                                v
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *  |////|  | / / / / / / / / / / / |                               |
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *       <==                        <==== allocated blocks grow down
 *       IPC bytes
 */

DEFINE_RAW_SPINLOCK(kutrace_lock);

/* Trace block size in bytes = 64KB */
#define KUTRACEBLOCKSHIFT (16)
#define KUTRACEBLOCKSIZE (1 << KUTRACEBLOCKSHIFT)

/* Trace block size in u64 words */
#define KUTRACEBLOCKSHIFTU64 (KUTRACEBLOCKSHIFT - 3)
#define KUTRACEBLOCKSIZEU64 (1 << KUTRACEBLOCKSHIFTU64)

/* IPC block size in u8 bytes */
#define KUIPCBLOCKSHIFTU8 (KUTRACEBLOCKSHIFTU64 - 3)
#define KUIPCBLOCKSIZEU8 (1 << KUIPCBLOCKSHIFTU8)

/* IPC design */
/* Map IPC * 8 [0.0 .. 3.75] into sorta-log value */
static const u64 kIpcMapping[64] = {
  0,1,2,3, 4,5,6,7, 8,8,9,9, 10,10,11,11, 
  12,12,12,12, 13,13,13,13, 14,14,14,14, 15,15,15,15,
  15,15,15,15, 15,15,15,15, 15,15,15,15, 15,15,15,15,
  15,15,15,15, 15,15,15,15, 15,15,15,15, 15,15,15,15
};

/* Map IPC= inst_retired / cycles to sorta-log four bits */
/* NOTE: delta_cycles is in increments of cycles/64. The arithmetic */
/*       below compensates for this. */
/* 0, 1/8, 1/4, 3/8,  1/2, 5/8, 3/4, 7/8,  1, 5/4, 3/2, 7/4,  2, 5/2, 3, 7/2 */
inline u64 get_granular(u64 delta_inst, u64 delta_cycles) {
  u32 del_inst, del_cycles, ipc;

  if ((delta_cycles & ~1) == 0) return 0; /* Too small to matter; avoid zdiv */
  /* Do 32-bit divide to save ~10 CPU cycles vs. 64-bit */
  /* With ~20ms guaranteed max interval, no overflow problems */
  del_inst = (u32)delta_inst;

#if IsRPi4_64
  /* "cycle" counter is 54MHz, cycles are 1500 MHz, so one count = 1500/54 = 27.778 cycles */
  /* Call it 28 (less than 1% error). To get 8*inst/cycles for the divide below, we mul by 8/28 = 2/7 */
  del_inst *= 2;
  del_cycles = (u32)(delta_cycles * 7);   /* cycles/28 to cycles/4 */

#elif IsRiscv
/* We expect about 1200 instructions per 1 usec "cycle" */
/* If del_inst = 2400 and del_cycles = 2, we want to return 1.0 IPC */
/* so multiply cycles by 1200 */
/* On top of all that, we want 1 IPC to map to a quotient of 8 on our 1/8ths scale */
/* so multiply instead by 150 */
  del_cycles = (u32)(delta_cycles * 150);
  
#else
  del_cycles = (u32)(delta_cycles << 3);  /* cycles/64 to cycles/8 */
  
#endif

  ipc = del_inst / del_cycles;	          /* gives IPC*8 */
  return kIpcMapping[ipc & 0x3F];	  /* Truncate unexpected IPC >= 8.0 */
}


/* Machine-specific register Access utilities */
/*----------------------------------------------------------------------------*/
#if IsIntel64 || IsAmd64
/* RDMSR Read a 64-bit value from a MSR. */
/* The A constraint stands for concatenation of registers EAX and EDX. */
static inline u64 rdMSR(u32 msr) {
   u32 lo, hi;
   asm volatile( "rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) );
   return ((u64)lo) | (((u64)hi) << 32);
}

/* WRMSR Write a 64-bit value to a MSR. */
/* The A constraint stands for concatenation of registers EAX and EDX. */
static inline void wrMSR(u32 msr, u64 value)
{
  u32 lo = value;
  u32 hi = value >> 32;
  asm volatile( "wrmsr" : : "a"(lo), "d"(hi), "c"(msr) );
}
#endif

#if IsRPi0
static inline unsigned long
armv6_pmcr_read(void)
{
	u32 val;
	asm volatile("mrc   p15, 0, %0, c15, c12, 0" : "=r"(val));
	return val;
}

static inline void
armv6_pmcr_write(unsigned long val)
{
	asm volatile("mcr   p15, 0, %0, c15, c12, 0" : : "r"(val));
}
#endif

#if IsRPi4_32
/* UNUSED */
static inline void armv7_pmnc_select_counter(int idx)
{
	u32 counter = ARMV7_IDX_TO_COUNTER(idx);
	asm volatile("mcr p15, 0, %0, c9, c12, 5" : : "r" (counter));
	/* isb(); */
}

/* UNUSED */
static inline u64 armv7pmu_read_counter(struct perf_event *event)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;
	u32 value = 0;

	if (!armv7_pmnc_counter_valid(cpu_pmu, idx)) {
		pr_err("CPU%u reading wrong counter %d\n",
			smp_processor_id(), idx);
	} else if (idx == ARMV7_IDX_CYCLE_COUNTER) {
		asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r" (value));
	} else {
		armv7_pmnc_select_counter(idx);
		asm volatile("mrc p15, 0, %0, c9, c13, 2" : "=r" (value));
	}

	return value;
}

/* UNUSED */
static inline u32 armv7_pmnc_read(void)
{
	u32 val;
	asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(val));
	return val;
}

/* UNUSED */
static inline void armv7_pmnc_write(u32 val)
{
	val &= ARMV7_PMNC_MASK;
	/* isb(); */
	asm volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(val));
}

/* UNUSED */
static inline void armv7_pmnc_enable_counter(int idx)
{
	u32 counter = ARMV7_IDX_TO_COUNTER(idx);
	asm volatile("mcr p15, 0, %0, c9, c12, 1" : : "r" (BIT(counter)));
}

/* Time counter frequency in Hz */
static inline u32 timer_get_cntfrq(void)
{
	u32 val;
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r" (val));
	return val;
}

#endif

#if IsRPi4_64
/* Time counter frequency in Hz */
static inline u32 timer_get_cntfrq(void)
{
        /* returns 54000000 */
	u32 val;
	asm volatile("mrs %x0, CNTFRQ_EL0" : "=r" (val));
	return val;
}
#endif


/* Set up global state for reading time, retired, freq */
/*----------------------------------------------------------------------------*/

/* Set up global state for reading scaled CPU cycles */
/* This needs to run once on each CPU core */
/* For ARM, make sure it increments every 64 cycles, not 1 */
void ku_setup_timecount(void)
{
#if IsArm64
	/* No setup needed for cntvct */
#elif IsRPi0
	/* Count every 64 cycles for ccnt is the default */
#elif IsRPi4_32
	/* Count every 64 cycles for ccnt is the default */
#elif IsRPi4_64
	/* Count every 1 cycle for ccnt is the default */
#elif IsRiscv
	/* no setup needed */
#elif Isx86_64
	/* No setup needed for rdtsc */
#else
#endif
}


/* Set up global state for reading instructions retired */
/* This needs to run once on each CPU core */
void ku_setup_inst_retired(void)
{
#if IsAmd64

	u64 inst_ret_enable;
	/* Enable fixed inst_ret counter  */
	inst_ret_enable = rdMSR(RYZEN_HWCR);
	printk(KERN_INFO "  kutrace_mod rdMSR(RYZEN_HWCR) = %016llx\n", inst_ret_enable);
	inst_ret_enable |= IRPerfEn;
	wrMSR(RYZEN_HWCR, inst_ret_enable);

#elif IsIntel64

	u64 inst_ret_ctrl;
	u64 inst_ret_enable;
	/* cpuCount_HW_INSTRUCTIONS = 1<<30 */

	/* Configure fixed inst_ret counter in IA32_FIXED_CTR_CTRL */
	/*   count both kernel and user, count per-CPU-thread, no interrupt */
	inst_ret_ctrl = rdMSR(IA32_FIXED_CTR_CTRL);
	printk(KERN_INFO "  kutrace_mod rdMSR(IA32_FIXED_CTR_CTRL) = %016llx\n", inst_ret_ctrl);
	inst_ret_ctrl &= ~EN0_ALL;
	inst_ret_ctrl |=  (EN0_OS | EN0_Usr);
	wrMSR(IA32_FIXED_CTR_CTRL, inst_ret_ctrl);

	/* Enable fixed inst_ret counter in IA32_PERF_GLOBAL_CTRL */
	inst_ret_enable = rdMSR(IA32_PERF_GLOBAL_CTRL);
	printk(KERN_INFO "  kutrace_mod rdMSR(IA32_PERF_GLOBAL_CTRL) = %016llx\n", inst_ret_enable);
	inst_ret_enable |= EN_FIXED_CTR0;
	wrMSR(IA32_PERF_GLOBAL_CTRL, inst_ret_enable);

#elif IsRPi0

	unsigned long val, mask, evt;
	mask = 0;
	/* Enable counters, set CCOUNT every 64 cycles */
	evt = ARMV6_PMCR_CCOUNT_DIV | ARMV6_PMCR_ENABLE;
	/* Count instructions in counter0 */
	evt |= (ARMV6_PERFCTR_INSTR_EXEC << ARMV6_PMCR_EVT_COUNT0_SHIFT);
	/* Unused counter, set to nop */
	evt |= (ARMV6_PERFCTR_NOP << ARMV6_PMCR_EVT_COUNT1_SHIFT);
	val = armv6_pmcr_read();
	printk(KERN_INFO "  kutrace_mod pmcr = %08lx\n", val);
	val &= ~mask;
	val |= evt;
	armv6_pmcr_write(val);

#elif IsRPi4_32
	/* No setup needed for inst counting ?? */
	/* 54MHz counter is already readable in user space */
	/*printk(KERN_INFO "  kutrace_mod timer_get_cntfrq(Hz) = %d\n", timer_get_cntfrq());*/

	/* TODO: Setup needed for instruction counting */

#elif IsRPi4_64
	/* Setup needed for instruction counting */
	/* set up pmevtyper0<15:0> to count INST_RETIRED =0x08 */
	/* set up pmcntenset<0>=1 to enable */
	u64 evtcount = 8;	/* INST_RETIRED */
	u64 r = 0;
asm volatile("mrs %x0, pmcr_el0" : "=r" (r));
printk(KERN_INFO "pmcr_el0       = %016llx\n", r);

asm volatile("mrs %x0, pmevtyper2_el0" : "=r" (r));
printk(KERN_INFO "pmevtyper2_el0 = %016llx\n", r);

asm volatile("mrs %x0, pmcntenset_el0" : "=r" (r));
printk(KERN_INFO "pmcntenset_el0 = %016llx\n", r);

asm volatile("mrs %x0, pmevcntr2_el0" : "=r" (r));
printk(KERN_INFO "pmevcntr2_el0  = %016llx\n", r);
asm volatile("mrs %x0, cntvct_el0" : "=r"(r));
printk(KERN_INFO "cntvct_el0     = %016llx\n", r);

asm volatile("mrs %x0, pmevcntr2_el0" : "=r" (r));
printk(KERN_INFO "pmevcntr2_el0  = %016llx\n", r);
asm volatile("mrs %x0, cntvct_el0" : "=r"(r));
printk(KERN_INFO "cntvct_el0     = %016llx\n", r);

printk(KERN_INFO "\n");
r = 0;

	asm volatile("mrs %x0, pmcr_el0" : "=r" (r));
	asm volatile("msr pmcr_el0, %x0" : : "r" (r | 1));	/* enable pmu */

	asm volatile("msr pmevtyper2_el0, %x0" : : "r" (evtcount));	/* count inst_retired */

	asm volatile("mrs %x0, pmcntenset_el0" : "=r" (r));
	asm volatile("msr pmcntenset_el0, %x0" : : "r" (r|1<<2));	/* enable cntr[2] */

/***
(1) PMSELR select counter <4:0> = 0 to select counter0
MRC p15, 0, <Rt>, c9, c12, 5 ; Read PMSELR into Rt
MCR p15, 0, <Rt>, c9, c12, 5 ; Write Rt to PMSELR

(2) PMXEVCNTR
MRC p15, 0, <Rt>, c9, c13, 2 : Read PMXEVCNTR into Rt
MCR p15, 0, <Rt>, c9, c13, 2 : Write Rt to PMXEVCNTR

(3) PMUSERENR <0> enable user-mode access
MRC p15, 0, <Rt>, c9, c14, 0 : Read PMUSERENR into Rt
MCR p15, 0, <Rt>, c9, c14, 0 : Write Rt to PMUSERENR

(4) PMCR 1<<5 DP disable PMCCNTR in debug
     1<<4 X export enable
     1<<3 D divider PMCCNTR 64 cy
     1<<2 C reset PMCCNTR
     1<<1 P reset other counters
     1<<0 E enable counters
MRC p15, 0, <Rt>, c9, c12, 0 ; Read PMCR into Rt
MCR p15, 0, <Rt>, c9, c12, 0 ; Write Rt to PMCR

(5) PMCNTENSET 1<<31 C, pmcctr enable
           1<<0  counter0 enable
MRC p15, 0, <Rt>, c9, c12, 1 ; Read PMCNTENSET into Rt
MCR p15, 0, <Rt>, c9, c12, 1 ; Write Rt to PMCNTENSET
***/

#elif IsRiscv
	/* no setup needed */

#else
#error Define ku_setup_inst_retired for your architecture

#endif
}

/* Set up global state for reading CPU frequency */
/* This needs to run once on each CPU core */
void ku_setup_cpu_freq(void)
{
	/* No setup for AMD, Intel */
}


/*x86-64 or Arm-specific time counter, ideally 30-60 MHz (16-32 nsec) */
/* Arm64 returns 32MHz counts: 31.25 ns each, CPU clock/64 @ 2GHz */
/* Arm32 Raspberry Pi-4B returns 32-bit constant 54MHz counts */
/* Arm32 Raspberry Pi-Zero returns 32-bit 16MHz, CPU clock/64 @ 1GHz */
/* x86-64 version returns constant rdtsc() >> 6 to give ~20ns resolution */
/* Unsupported x86-32 would also be constant rdtsc() >> 6 */


/* Read a time counter */
/* This is performance critical -- every trace entry  */
/* Ideally, this counts at a constant rate of 16-32 nsec per count.           */
/*----------------------------------------------------------------------------*/
inline u64 ku_get_timecount(void)
{
	u64 timer_value;
#if IsArm64
	asm volatile("mrs %x0, cntvct_el0" : "=r"(timer_value));
#elif IsRPi0
	/* This 32-bit PiZero result wraps about every 250 seconds */
	asm volatile("mrc  p15, 0, %0, c15, c12, 1" : "=r"(timer_value));
	timer_value &= CLU(0x00000000ffffffff);	/* clear garbage high bits */
#elif IsRPi4_32
	/* This 32-bit Pi-4B result wraps about every 75 seconds */
	asm volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r" (timer_value));
	timer_value &= CLU(0x00000000ffffffff);	/* clear garbage high bits */
#elif Isx86_64
	timer_value = rdtsc() >> 6;		/* Both AMD and Intel */
#elif IsRiscv
	timer_value = get_cycles();
#else
#error Define the time counter for your architecture
	timer_value = 0;
#endif
	return timer_value;
}


/* Read instructions retired counter */
/* This is performance critical -- every trace entry if tracking IPC */
/*----------------------------------------------------------------------------*/
inline u64 ku_get_inst_retired(void)
{
#if IsAmd64
	u32 a = 0, d = 0;
	int ecx = IRPerfCount;		/* What counter it selects, AMD */
	 __asm __volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(ecx));
	return ((u64)a) | (((u64)d) << 32);

#elif IsIntel64
	u32 a = 0, d = 0;
	int ecx = IA32_FIXED_CTR0;	/* What counter it selects, Intel */
	__asm __volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(ecx));
	return ((u64)a) | (((u64)d) << 32);

#elif IsRPi0
	u32 value = 0;
	asm volatile("mrc   p15, 0, %0, c15, c12, 2" : "=r"(value));
	return value & 0x00000000FFFFFFFFLU;

#elif IsRPi4_32
	/* TBD: leave PMSELR to counter0; read */
	u32 value = 0;
	/* armv7_pmnc_select_counter(idx=0); */
	asm volatile("mrc p15, 0, %0, c9, c13, 2" : "=r" (value));
	return value & 0x00000000FFFFFFFFLU;

#elif IsRPi4_64
	u64 value = 0;
	/* set up pmevtyper2<15:0> to count INST_RETIRED =0x08 */
	/* set up pmcntenset<0>=1<<2 to enable */
	asm volatile("mrs %x0, pmevcntr2_el0" : "=r" (value));
	return value;

#elif IsRiscv
	return csr_read(CSR_INSTRET);

#else
#error Define inst_retired for your architecture
	return 0;
#endif

}

/* Read current CPU frequency in MHz */
/* Not performance critical -- once every timer interrupt                     */
/*----------------------------------------------------------------------------*/
inline u64 ku_get_cpu_freq(void) {
#if !BCLK_FREQ
	return 0;

#elif IsAmd64
        /* Sample the CPU clock frequency and include with PC sample */
	u64 curr = rdMSR(PStateStat) & PStat_MASK;
        u64 freq = rdMSR(PStateDef0 + curr);
	u64 fid = (freq >> CpuFid_SHIFT) & CpuFid_MASK;
	u64 did = (freq >> CpuDid_SHIFT) & CpuDid_MASK;
        freq = (fid * BCLK_FREQ) / did;
	return freq;

#elif IsIntel64
	u64 freq = rdMSR(MSR_PERF_STATUS);
        freq = (freq >> FID_SHIFT) & FID_MASK;
        freq *= BCLK_FREQ;	/* base clock in MHz */
	return freq;

#elif IsRiscv
	return 1196;	/* constant on Unmatched board */

#else
#error Define cpu_freq for your architecture
	return 0;

#endif
}



/* Make sure name length fits in 1..8 u64's */
//* Return true if out of range */
inline bool is_bad_len(int len)
{
	return (len < 1) | (len > 8);
}


/* Turn off tracing. (We cannot wait here) */
/* Return tracing bit */
static u64 do_trace_off(void)
{
	kutrace_tracing = false;
	return kutrace_tracing;
}

/* Turn on tracing. We can only get here if all is set up */
/* Trace buffer must be allocated and initialized */
/* Return tracing bit */
static u64 do_trace_on(void)
{
	kutrace_tracing = true;
	return kutrace_tracing;
}

/* Flush all partially-filled trace blocks, filling them up */
/* Tracing must be off */
/* Return number of words zeroed */
static u64 do_flush(void)
{
	u64 *p;
	int cpu;
	int zeroed = 0;

	kutrace_tracing = false;	/* Should already be off */
	for_each_online_cpu(cpu)
	{
		struct kutrace_traceblock *tb =
			&per_cpu(kutrace_traceblock_per_cpu, cpu);
		u64 *next_item = (u64 *)ATOMIC_READ(&tb->next);
		u64 *limit_item = tb->limit;

		if (next_item == NULL)
			continue;
		if (limit_item == NULL)
			continue;
		for (p = next_item; p < limit_item; ++p)
		{
			*p = 0;
			++zeroed;
		}

		ATOMIC_SET(&tb->next, (uintptr_t)limit_item);
	}
	return zeroed;
}


/* Return number of filled trace blocks */
/* Next can overshoot limit when we are full */
/* Tracing will usually be on */
/* NOTE: difference of two u64* values is 1/8 of what you might be thinking */
static u64 do_stat(void)
{
	if (did_wrap_around || (traceblock_next < traceblock_limit))
		return (u64)(traceblock_high -
				traceblock_limit) >> KUTRACEBLOCKSHIFTU64;
	else
		return (u64)(traceblock_high -
				traceblock_next) >> KUTRACEBLOCKSHIFTU64;
}

/* Return number of filled trace words */
/* Tracing must be off and flush must have been called */
/* NOTE: difference of two u64* values is 1/8 of what you might be thinking */
static u64 get_count(void)
{
	u64 retval;

	kutrace_tracing = false;
	if (did_wrap_around || (traceblock_next < traceblock_limit))
		retval = (u64)(traceblock_high - traceblock_limit);
	else
		retval = (u64)(traceblock_high - traceblock_next);

	return retval;
}

/* Read and return one u64 word of trace data, working down from top.
 * This is called 1M times to dump 1M trace words (8MB), but it is called
 * by a user program that is writing all this to disk, thus is constrained
 * by disk I/O speed. So we don't care that this is somewhat inefficient
 *
 *  traceblock_limit          traceblock_next                traceblock_high
 *  |                               |                                |
 *  v                               v                                v
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *  | / / / / / / / / / / / / / / / |   3       2       1       0   |
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *                                  <==== allocated blocks grow down
 */


/* Read and return one u64 word of trace data, working down from top.
 * This is called 1M times to dump 1M trace words (8MB), but it is called
 * by a user program that is writing all this to disk, so is constrained
 * by disk I/O speed. So we don't care that this is somewhat inefficient
 */
/* Tracing must be off and flush must have been called */
static u64 get_word(u64 subscr)
{
	u64 blocknum, u64_within_block;
	u64 *blockp;

	kutrace_tracing = false;	/* Should already be off */
	if (subscr >= get_count()) return 0;
	blocknum = subscr >> KUTRACEBLOCKSHIFTU64;
	u64_within_block = subscr & ((1 << KUTRACEBLOCKSHIFTU64) - 1);
	blockp = traceblock_high - ((blocknum + 1) << KUTRACEBLOCKSHIFTU64);
/* printk(KERN_INFO "get_word[%lld] %016llx\n", subscr, blockp[u64_within_block]); */
	return blockp[u64_within_block];
}

/* Read and return one u64 word of IPC data, working down from top.
 *
 * Trace memory layout with IPC tracing. IPC bytes go into lower 1/8.
 *  tracebase
 *  |    traceblock_limit     traceblock_next                traceblock_high
 *  |       |                       |                                |
 *  v       v                       v                                v
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *  |////|  | / / / / / / / / / / / |                               |
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *       <==                        <==== allocated blocks grow down
 *       IPC bytes
 */

/* Tracing must be off and flush must have been called */
/* We map linear IPCword numbers 0..get_count-1 to IPC block and offset, */
/* with blocks growing downward. If mains trace blocks are 64KB, */
/* IPC blocks are 8KB */
/* Even though they are byte entries, we read them out as u64's */
static u64 get_ipc_word(u64 subscr)
{
	u64 blocknum, u64_within_block;
	u64 *blockp;

	kutrace_tracing = false;
	/* IPC word count is 1/8 of main trace count */
	if (subscr >= (get_count() >> 3))
		return 0;
	blocknum = subscr >> KUIPCBLOCKSHIFTU8;
	u64_within_block = subscr & ((1 << KUIPCBLOCKSHIFTU8) - 1);
	/* IPC blocks count down from traceblock_limit */
	blockp = traceblock_limit - ((blocknum + 1) << KUIPCBLOCKSHIFTU8);
	return blockp[u64_within_block];
}



/* We are called with preempt disabled */
/* We are called with interrupts disabled */
/* We are called holding the lock that guards traceblock_next */
/* Return first real entry slot */
static u64 *initialize_trace_block(u64 *init_me, bool very_first_block,
	struct kutrace_traceblock *tb)
{
	u64 *myclaim = NULL;
	u64 cpu = smp_processor_id();

	/* For every traceblock, insert current process ID and name. This */
	/* gives the proper context when wraparound is enabled */
	struct task_struct *curr = current;

	/* First word is rdtsc (time counter) with CPU# placed in top byte */
	u64 block_init_counter = ku_get_timecount();
	/* Supply high bits in case it wrapped previously */
	block_init_counter |= (prior_block_init_counter & kCounterWrapMask);
	/* Adjust if the counter wrapped around at 32 or 40 bits */
	if (block_init_counter < prior_block_init_counter) {
		/* We wrapped so add 1 */
		block_init_counter += kCounterWrapIncrease;
        }
	prior_block_init_counter = block_init_counter;

	/* Now we can use the time counter */
	init_me[0] = (block_init_counter & FULL_TIMESTAMP_MASK) |
		(cpu << CPU_NUMBER_SHIFT);

	/* Second word is going to be corresponding gettimeofday(), */
	/* filled in via postprocessing */
	/* We put some flags in the top byte, though. x080 = do_ipc bit */
	init_me[1] = 0;
	if (do_ipc)
		init_me[1] |= (IPC_Flag << FLAGS_SHIFT);
	if (do_wrap)
		init_me[1] |= (WRAP_Flag << FLAGS_SHIFT);
	/* We don't know if we actually wrapped until the end. */
	/* See KUTRACE_CMD_GETCOUNT */

	/* For very first trace block, also insert six NOPs at [2..7]. */
	/* The dump to disk code will overwrite the first pair with */
	/* start timepair and the next with stop timepair. [6..7] unused */
	if (very_first_block) {
		init_me[2] = CLU(0);
		init_me[3] = CLU(0);
		init_me[4] = CLU(0);
		init_me[5] = CLU(0);
		init_me[6] = CLU(0);
		init_me[7] = CLU(0);
		myclaim = &init_me[8];
	} else {
		myclaim = &init_me[2];
	}

	/* Every block has PID and pidname at the front */
	/* This requires a change for V3 in postprocessing */
	/* I feel like I should burn one more word here to make 4, */
	/* so entire front is 12/6 entries instead of 11/5... */
	myclaim[0] = curr->pid;
	myclaim[1] = 0;
	memcpy(&myclaim[2], curr->comm, MAX_PIDNAME_LENGTH);
	myclaim += 4;

	/* Next len words are the claimed space for an entry */

	/* Last 8 words of a block set to NOPs (0) */
	init_me[KUTRACEBLOCKSIZEU64 - 8] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 7] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 6] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 5] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 4] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 3] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 2] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 1] = 0;

	/* If this is the very first traceblock for this CPU, set up the MSRs */
	/* If there are 12 CPU cores (6 physical 2x hyperthreaded) this will happen 12 times */
	{
		bool first_block_per_cpu = (tb->prior_cycles == 0);
		if (first_block_per_cpu) {
			ku_setup_timecount();
			ku_setup_inst_retired();
			ku_setup_cpu_freq();
			tb->prior_cycles = 1;	/* mark it as initialized */
#if IsRPi4_64
			{
			struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);
			/* For Rpi4, put current CPU freq (MHz) into block at high half of myclaim[-4] */
			if (policy) {
				u64 cpu_freq_mhz = policy->cur / 1000;	/* Khz to MHz */
				myclaim[-4] |= (cpu_freq_mhz << 32);
/*printk(KERN_INFO "cpu %lld freq = %lld MHz\n", cpu, cpu_freq_mhz);*/
			}
			}
#endif

		}
	}

	return myclaim;
}

/* We are called with preempt disabled */
/* We are called with interrupts disabled */
/* We are called holding the lock that guards traceblock_next */
static u64 *really_get_slow_claim(int len, struct kutrace_traceblock *tb)
{
	u64 *myclaim = NULL;
	bool very_first_block = (traceblock_next == traceblock_high);

	/* Allocate a new traceblock. Allocations grow downward. */
	traceblock_next -= KUTRACEBLOCKSIZEU64;

	if (traceblock_next < traceblock_limit) {
		if (do_wrap) {
			/* Wrap to traceblock[1], not [0] */
			did_wrap_around = true;
			traceblock_next = traceblock_high -
				2 * KUTRACEBLOCKSIZEU64;
			/* Clear pid filter. */
			/* It is unfortunate to do this while holding a */
			/* lock and also holding off interrupts... */
			memset(kutrace_pid_filter, 0, 1024 * sizeof(u64));
		} else {
			/* All full. Stop and get out. */
			kutrace_tracing = false;
			return myclaim;
		}
	}

	/* Need to do this before setting next/limit if same CPU could get */
	/* an interrupt and use uninitilized block */
	/* It is unfortunate to do this while holding a lock and also */
	/* holding off interrupts... */
	/* Most of the cost is two cache misses, so maybe 200 nsec */
	myclaim = initialize_trace_block(traceblock_next, very_first_block, tb);

	/* Set up the next traceblock pointers, reserving */
	/* first N + len words */
	ATOMIC_SET(&tb->next, (uintptr_t)(myclaim + len));
	tb->limit = traceblock_next + KUTRACEBLOCKSIZEU64;
	return myclaim;
}

/* Reserve space for one entry of 1..8 u64 words */
/* If trace buffer is full, return NULL or wrap around */
/* We allow this to be used with tracing off so we can initialize a */
/* trace file */
/* In that case, tb->next and tb->limit are NULL */
/* We are called with preempt disabled */
static u64 *get_slow_claim(int len, struct kutrace_traceblock *tb)
{
	unsigned long flags;
	u64 *limit_item;
	u64 *myclaim = NULL;

	if (is_bad_len(len)) {
		kutrace_tracing = false;
printk(KERN_INFO "is_bad_len 1\n");
		return NULL;
	}

	/* This gets the lock that protects traceblock_next and */
	/* disables interrupts */
	raw_spin_lock_irqsave(&kutrace_lock, flags);
	/* Nothing else can be touching tb->limit now */
	limit_item = tb->limit;
	/* add_return returns the updated pointer; we want the prior */
	/* so subtract len */
	myclaim = ((u64 *)ATOMIC_ADD_RETURN(len * sizeof(u64), 
	                                    (atomic64_t*)&tb->next)) - len;
	/* FIXED BUG: myclaim + len */
	if (((myclaim + len) >= limit_item) || (limit_item == NULL)) {
		/* Normal case: */
		/* the claim we got still doesn't fit in its block */
		myclaim = really_get_slow_claim(len, tb);
	}
	/* Rare: If some interrupt already allocated a new traceblock, */
	/* fallthru to here */
	/* Free lock; re-enable interrupts if they were enabled on entry */
	raw_spin_unlock_irqrestore(&kutrace_lock, flags);

	return myclaim;
}


/* Reserve space for one entry of 1..8 u64 words, normally lockless */
/* If trace buffer is full, return NULL. Caller MUST check */
/* We allow this to be used with tracing off so we can initialize a */
/* trace file */
static u64 *get_claim(int len)
{
	struct kutrace_traceblock *tb;
	u64 *limit_item = NULL;
	u64 *limit_item_again = NULL;
	u64 *myclaim = NULL;

	if (is_bad_len(len)) {
		kutrace_tracing = false;
printk(KERN_INFO "is_bad_len 2\n");
		return NULL;
	}

	/* Fast path */
	/* We may get interrupted at any point here and the interrupt routine
	 * may create a trace entry, and it may even allocate a new
	 * traceblock.
	 * This code must carefully either reserve an exclusive area to use or
	 * must call the slow path.
	 */

	/* Note that next and limit may both be NULL at initial use. */
	/* If they are, take the slow path without accessing. */

	/* get_cpu_var disables preempt ************************************/
	tb = &get_cpu_var(kutrace_traceblock_per_cpu);
	do {
		limit_item = tb->limit;
		if (limit_item == NULL)
			break;
		/* add_return returns the updated pointer; we want the */
		/* prior so subtract len */
		
		myclaim = ((u64 *)ATOMIC_ADD_RETURN(len * sizeof(u64), 
	                                        (atomic64_t*)&tb->next)) - len;

		limit_item_again = tb->limit;

		if (limit_item == limit_item_again)
			break;	/* All is good */
		/* An interrupt occurred *and* changed blocks */
		if ((myclaim < limit_item_again) &&
			((limit_item_again - KUTRACEBLOCKSIZEU64) <= myclaim))
			/* Claim is in new block -- use it */
			break;
		/* Else claim is at end of old block -- abandon it, */
		/* try again */
	} while (true);

	/* FIXEDBUG: myclaim + len */
	if ((myclaim + len) >= limit_item_again) {
		/* The claim we got doesn't fit in its block */
		myclaim = get_slow_claim(len, tb);
	}
	put_cpu_var(kutrace_traceblock_per_cpu);
	/* put_cpu_var re-enables preempt **********************************/

	return myclaim;
}



/* Return prior trace word for this CPU or NULL */
static u64 *get_prior(void)
{
	struct kutrace_traceblock *tb;
	u64 *next_item;
	u64 *limit_item;

	/* Note that next and limit may both be NULL at initial use. */
	/* If they are, or any other problem, return NULL */
	/* get_cpu_var disables preempt */
	tb = &get_cpu_var(kutrace_traceblock_per_cpu);
	next_item = (u64 *)ATOMIC_READ(&tb->next);
	limit_item = tb->limit;
	put_cpu_var(kutrace_traceblock_per_cpu);

	if (next_item < limit_item)
		return next_item - 1;	/* ptr to prior entry */
	return NULL;
}


/*
 *  arg1: (arrives with timestamp = 0x00000)
 *  +-------------------+-----------+---------------+-------+-------+
 *  | timestamp         | event     | delta | retval|      arg0     |
 *  +-------------------+-----------+---------------+-------+-------+
 *           20              12         8       8           16
 */


#if 0
//VERYTEMP
static int inst_ret_count = 0;
static u64 inst_ret_cpu[32];
static u64 inst_ret_value[32];
static u64 inst_ret_delinst[32];
static u64 inst_ret_delcycl[32];
#endif


/* Insert one u64 trace entry, for current CPU */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_1(u64 arg1)
{
	u64 *claim;
	u64 now = ku_get_timecount();

	claim = get_claim(1);
	if (claim != NULL) {
		claim[0] = arg1 | (now << TIMESTAMP_SHIFT);
		/* IPC option. Changes CPU overhead from ~1/4% to ~3/4% */
		if (do_ipc) {
			struct kutrace_traceblock* tb;	/* IPC, access to prior values for this CPU */
			u64 inst_ret;
			u64 delta_cycles;
			u64 delta_inst;
			u8* ipc_byte_addr;
//if (first_printk) {printk(KERN_INFO "insert_1 do_ipc\n");}

			/* There will be random large differences the first time; we don't care. */
			tb = &get_cpu_var(kutrace_traceblock_per_cpu);	/* hold off preempt */
			delta_cycles = now - tb->prior_cycles;
			tb->prior_cycles = now;
//if (first_printk) {printk(KERN_INFO "insert_1 prior_cycles\n");}

			inst_ret = ku_get_inst_retired();
			delta_inst = inst_ret - tb->prior_inst_retired;
			tb->prior_inst_retired = inst_ret;
			put_cpu_var(kutrace_traceblock_per_cpu);		/* release preempt */

			/* NOTE: pointer arithmetic divides claim-tracebase by 8, giving the byte offset we want */
			ipc_byte_addr = (u8*)(tracebase) + (claim - (u64*)(tracebase));
			ipc_byte_addr[0] = get_granular(delta_inst, delta_cycles);
		}
		return 1;
	}
	return 0;
}

/* Insert one u64 Return trace entry with small retval, for current CPU */
/* Optimize by combining with just-previous entry if the matching call */
/* and delta_t fits. The optimization is likely, so we don't worry about */
/* the overhead if we can't optimize */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_1_retopt(u64 arg1)
{
	u64 now = ku_get_timecount();
	u64 *prior_entry = get_prior();

	if (prior_entry != NULL) {
		/* Want N=matching call, high two bytes of arg = 0 */
		u64 diff = (*prior_entry ^ arg1) & EVENT_DELTA_RETVAL_MASK;
		u64 prior_t = *prior_entry >> TIMESTAMP_SHIFT;
		u64 delta_t = (now - prior_t) & UNSHIFTED_TIMESTAMP_MASK;

		/* make nonzero to flag there is an opt ret */
		if (delta_t == 0)
			delta_t = 1;
		/* EVENT_RETURN_BIT distinguishes call from return */
		if ((diff == EVENT_RETURN_BIT) &&
			(delta_t <= MAX_DELTA_VALUE))
		{
			/* Combine */
			u64 opt_ret;

			opt_ret = (delta_t << DELTA_SHIFT) |
				      ((arg1 & UNSHIFTED_RETVAL_MASK) << RETVAL_SHIFT);
			*prior_entry |= opt_ret;
			/* IPC option. Changes CPU overhead from ~1/4% to ~3/4% */
			if (do_ipc) {
				struct kutrace_traceblock* tb;	/* IPC, access to prior values for this CPU */
				u64 inst_ret;
				u64 delta_cycles;
				u64 delta_inst;
				u8* ipc_byte_addr;

				/* There will be random large differences the first time; we don't care. */
				tb = &get_cpu_var(kutrace_traceblock_per_cpu);	/* hold off preempt */
				delta_cycles = now - tb->prior_cycles;
				tb->prior_cycles = now;

				inst_ret = ku_get_inst_retired();
				delta_inst = inst_ret - tb->prior_inst_retired;
				tb->prior_inst_retired = inst_ret;
				put_cpu_var(kutrace_traceblock_per_cpu);		/* release preempt */	

				/* NOTE: pointer arithmetic divides claim-base by 8, giving the byte offset we want */
				ipc_byte_addr = (u8*)(tracebase) + (prior_entry - (u64*)(tracebase));
				/* IPC for entry..return goes into high 4 bits of IPC byte */
				ipc_byte_addr[0] |= (get_granular(delta_inst, delta_cycles) << 4);
			}
			return 0;
		}
	}

	/* Otherwise, fall into normal insert_1 */
	return insert_1(arg1);
}


/* Insert one pair u64 trace entry, for current CPU */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_2(u64 arg1, u64 arg2)
{
	u64 now = ku_get_timecount();
	u64 *claim = get_claim(2);

	if (claim != NULL)
	{
		claim[0] = arg1 | (now << TIMESTAMP_SHIFT);
		claim[1] = arg2;
		return 2;
	}
	return 0;
}

/* For event codes 010..1FF, length is middle hex digit. All others 1 */
static u64 entry_len(u64 word)
{
	u64 n = (word >> EVENT_SHIFT) & UNSHIFTED_EVENT_MASK;

	if (n > MAX_EVENT_WITH_LENGTH)
		return 1;
	if (n < MIN_EVENT_WITH_LENGTH)
		return 1;
	return (n >> EVENT_LENGTH_FIELD_SHIFT) & EVENT_LENGTH_FIELD_MASK;
}


/* Insert one trace entry of 1..8 u64 words, for current CPU */
/* word is actually a const u64* pointer to kernel space array of */
/* exactly len u64 */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_n_krnl(u64 word)
{
	const uintptr_t tempword = word;	/* 32- or 64-bit pointer */
	const u64 *krnlptr = (const u64 *)tempword;
	u64 len;
	u64 now;
	u64 *claim;

	len = entry_len(krnlptr[0]);	/* length in u64, 1..8 */

	/* Turn off tracing if bogus length */
	if (is_bad_len(len)) {
		kutrace_tracing = false;
printk(KERN_INFO "is_bad_len 3\n");
		return 0;
	}

	now = ku_get_timecount();
	claim = get_claim(len);
	if (claim != NULL) {
		claim[0] = krnlptr[0] | (now << TIMESTAMP_SHIFT);
		memcpy(&claim[1], &krnlptr[1], (len - 1) * sizeof(u64));
		return len;
	}
	return 0;
}

/* Insert one trace entry of 1..8 u64 words, for current CPU */
/* word is actually a const u64* pointer to user space array of exactly */
/* eight u64 */
/* Tracing may be otherwise off */
/* Return number of words inserted */
static u64 insert_n_user(u64 word)
{
	const uintptr_t tempword = word;	/* 32- or 64-bit pointer */
	const u64 *userptr = (const u64 *)tempword;
	u64 temp[8];
	u64 len;
	u64 now;
	u64 *claim;
	u64 uncopied_bytes;

	/* This call may sleep or otherwise context switch */
	/* It may fail if passed a bad user-space pointer. Don't do that. */
	temp[0] = 0;
	uncopied_bytes = raw_copy_from_user(temp, userptr, 8 * sizeof(u64));
	if (uncopied_bytes > 0)
		return 0;

	len = entry_len(temp[0]);	/* length in u64, 1..8 */

	/* Turn off tracing if bogus length */
	if (is_bad_len(len)) {
		kutrace_tracing = false;
printk(KERN_INFO "is_bad_len 4\n");
		return 0;
	}

	now = ku_get_timecount();
	claim = get_claim(len);
	if (claim != NULL) {
		temp[0] |= (now << TIMESTAMP_SHIFT);
		memcpy(claim, temp, len * sizeof(u64));
		return len;
	}
	return 0;
}

/*
 * pid filter is an array of 64K bits, arranged as 1024 u64. It
 * cleared. When tracing context switches in kernel/sched/core.c, the
 * intended use is to check if the bit corresponding to next->pid & 0xffff is
 * off and if so put the process name next->comm[TASK_COMM_LEN]; from
 * task_struct into the trace as a pid_name entry, then set the bit.
 */

/* Reset tracing state to start a new clean trace */
/* Tracing must be off. tracebase must be non-NULL */
/* traceblock_next always points *just above* the next block to use */
/* When empty, traceblock_next == traceblock_high */
/* when full, traceblock_next == traceblock_limit */
/* Return 0 */
static u64 do_reset(u64 flags)
{
	int cpu;

	/* printk(KERN_INFO "  kutrace_trace reset(%016llx) called\n", flags); */
	/* Turn off tracing -- should already be off */
	kutrace_tracing = false;	/* Should already be off */
	do_ipc = ((flags & DO_IPC) != 0);
	do_wrap = ((flags & DO_WRAP) != 0);

	/* Clear pid filter */
	memset(kutrace_pid_filter, 0, 1024 * sizeof(u64));

	/* Set up trace buffer into a series of blocks of 64KB each */
	traceblock_high = (u64 *)(tracebase + (tracemb << 20));
	traceblock_limit = (u64 *)(tracebase);
	/* First trace item inserted will cause first new block */
	traceblock_next = traceblock_high;	
	did_wrap_around = false;

	if (do_ipc) {
		/* Reserve lower 1/8 of trace buffer for IPC bytes */
		/* Strictly speaking, this should be 1/9. We waste a little space. */
		traceblock_limit = (u64*)(tracebase + (tracemb << (20 - 3)));
	}

	/* Set up spinlock as available */
	raw_spin_lock_init(&kutrace_lock);

	/* Set up per-CPU limits to immediately allocate a block */
	for_each_online_cpu(cpu) {
		struct kutrace_traceblock *tb =
			&per_cpu(kutrace_traceblock_per_cpu, cpu);

		ATOMIC_SET(&tb->next, (uintptr_t)NULL);
		tb->limit = NULL;
		tb->prior_cycles = 0;		// IPC design
		tb->prior_inst_retired = 0;	// IPC design
	}

	/* Initialize prior_counter so first traceblock does not wrap */
	prior_block_init_counter = 0;

	return 0;
}


/* Called from kernel patches */
/* CALLER is responsible for making sure event fits in 12 bits and */
/*  arg fits in 16 bits for syscall/ret and 32 bits otherwise */
static /*asmlinkage*/ void trace_1(u64 event, u64 arg)
{
    u64 event_arg;
    if (!kutrace_tracing)
		return;

	event_arg = (event << EVENT_SHIFT) | (arg & CLU(0xffffffff));
	/* Check for possible return optimization */
	if (((event & UNSHIFTED_EVENT_RETURN_BIT) != 0) &&
		((event & UNSHIFTED_EVENT_HAS_RETURN_MASK) != 0))
	{
		/* We have a return entry 011x, 101x, 111x: 6/7, a/b, e/f */
		if (((arg + 128l) & ~UNSHIFTED_RETVAL_MASK) == 0) {
			/* Signed retval fits into a byte, [-128..127] */
			insert_1_retopt(event_arg);
			return;
		}
	}

	/* Non-optimized insert */
	insert_1(event_arg);
}

/* Called from kernel patches */
/* ONLY called to insert PC sample at timer interrupt */
/* arg1 is unused (0), arg2 is the 64-bit PC sample */
static void trace_2(u64 event, u64 arg1, u64 arg2)
{
	u64 freq;
	if (!kutrace_tracing)
		return;

/* dsites 2021.04.05 insert CPU frequency */
	freq = ku_get_cpu_freq();
	insert_2((event << EVENT_SHIFT) | freq, arg2);
}

/* Called from kernel patches */
static void trace_many(u64 event, u64 len, const char *arg)
{
	uintptr_t tempptr;	/* 32- or 64-bit address */
	u64 temp[8];

	if (!kutrace_tracing)
		return;
	/* Turn off tracing if bogus length */
	if (is_bad_len(len)) {
		kutrace_tracing = false;
		return;
	}
	memcpy(temp, arg, len * sizeof(u64));
	temp[0] |= (event | (len << EVENT_LENGTH_FIELD_SHIFT)) << EVENT_SHIFT;
	tempptr = (uintptr_t)&temp[0];
	insert_n_krnl((u64)tempptr);
}


/* Syscall from user space via kernel patch */
static u64 kutrace_control(u64 command, u64 arg)
{
#if 0
  /* For bringup, so we can see that we got here */
  /* Don't trace the thousands of getword at tend to dump to disk */
  if (command != KUTRACE_CMD_GETWORD) {
    printk(KERN_INFO "  kutrace_control: %08x %08x %08x %08x\n",
      (u32)(command & 0xFFFFFFFF), (u32)(command >> 32),
      (u32)(arg & 0xFFFFFFFF), (u32)(arg >> 32));
  }
#endif
 
	if (tracebase == NULL) {
		/* Error! */
		printk(KERN_INFO "  kutrace_control called with no trace buffer.\n");
		kutrace_tracing = false;
		return ~CLU(0);
	}

	if (command == KUTRACE_CMD_OFF) {
		return do_trace_off();
	} else if (command == KUTRACE_CMD_ON) {
		return do_trace_on();
	} else if (command == KUTRACE_CMD_FLUSH) {
		return do_flush();
	} else if (command == KUTRACE_CMD_RESET) {
		return do_reset(arg);
	} else if (command == KUTRACE_CMD_STAT) {
		return do_stat();
	} else if (command == KUTRACE_CMD_GETCOUNT) {
		if (did_wrap_around) {
			/* Convey that we actually wrapped */
			return ~get_count();
		} else {
			return get_count();
		}
	} else if (command == KUTRACE_CMD_GETWORD) {
		return get_word(arg);
	} else if (command == KUTRACE_CMD_GETIPCWORD) {
		return get_ipc_word(arg);
	} else if (command == KUTRACE_CMD_INSERT1) {
		/* If not tracing, insert nothing */
		if (!kutrace_tracing)
			return 0;
		return insert_1(arg);
	} else if (command == KUTRACE_CMD_INSERTN) {
		/* If not tracing, insert nothing */
		if (!kutrace_tracing)
			return 0;
		return insert_n_user(arg);
	} else if (command == KUTRACE_CMD_TEST) {
		return kutrace_tracing;	/* Just 0/1 for tracing off/on */
	} else if (command == KUTRACE_CMD_VERSION) {
		return kModuleVersionNumber;
	} else if (command == ~KUTRACE_CMD_INSERT1) {
		/* Allow kutrace_control to insert entries with tracing off */
		return insert_1(arg);
	} else if (command == ~KUTRACE_CMD_INSERTN) {
		/* Allow kutrace_control to insert entries with tracing off */
		return insert_n_user(arg);
	}

	/* Else quietly return -1 */
	return ~CLU(0);
}


/*
 * For the compiled-into-the-kernel design, call this at first
 * kutrace_control call to set up trace buffers, etc.
 */
static int __init kutrace_mod_init(void)
{
	printk(KERN_INFO "\nkutrace_trace hello =====================\n");
	kutrace_tracing = false;

	kutrace_pid_filter = (u64 *)vmalloc(1024 * sizeof(u64));
	printk(KERN_INFO "  vmalloc kutrace_pid_filter " FUINTPTRX "\n",
		(uintptr_t)kutrace_pid_filter);
	if (!kutrace_pid_filter)
		return -1;

	tracebase =  vmalloc(tracemb << 20);
	printk(KERN_INFO "  vmalloc kutrace_tracebase(%ld MB) " FUINTPTRX " %s\n",
		tracemb,
		(uintptr_t)tracebase,
		(tracebase == NULL) ? "FAIL" : "OK");
	if (!tracebase) {
		vfree(kutrace_pid_filter);
		return -1;
	}

	/* Set up TCP packet filter */
	/* Filter forms a hash over masked first N=24 bytes of packet payload */
	/* and looks for zero result. The hash is just u32 XOR along with */
	/* an initial value. pktmask gives mask bit-per-byte, and pktmatch */
	/* gives the expected result over those bytes. It is the */
	/* inital hash value, to give a simple zero test at the end. */
	if (pktmask == 0) {
		// Match nothing
		kutrace_net_filter.hash_mask[0] = 0LLU;
		kutrace_net_filter.hash_mask[1] = 0LLU;
		kutrace_net_filter.hash_mask[2] = 0LLU;
		kutrace_net_filter.hash_init = 1;	// hash will always be zero
	} else if (pktmask == -1) {
		// Match nothing
		kutrace_net_filter.hash_mask[0] = 0LLU;
		kutrace_net_filter.hash_mask[1] = 0LLU;
		kutrace_net_filter.hash_mask[2] = 0LLU;
		kutrace_net_filter.hash_init = 0;	// hash will always be zero

	} else {
		int i;
		u8 *msk = (u8*)(kutrace_net_filter.hash_mask);
		for (i = 0; i < 24; ++i) {
			if ((pktmask >> i) & 1) {msk[i] = 0xFF;}
			else {msk[i] = 0x00;}
		}
		kutrace_net_filter.hash_init = (u64)(pktmatch);
	}
	printk(KERN_INFO "  mask %016llx", kutrace_net_filter.hash_mask[0]);
	printk(KERN_INFO "  mask %016llx", kutrace_net_filter.hash_mask[1]);
	printk(KERN_INFO "  mask %016llx", kutrace_net_filter.hash_mask[2]);
	printk(KERN_INFO "   ==  %016llx", kutrace_net_filter.hash_init);

#if IsAmd64
	printk(KERN_INFO "IsAmd64");
#endif
#if IsIntel64
	printk(KERN_INFO "IsIntel64");
#endif
#if IsRiscv
	printk(KERN_INFO "IsRiscv");
#endif

	/* Set up global tracing data state */
	/* Very first traceblock alloc per CPU will do this, but we need */
	/* the timecount set up before we write teh first trace entry */
	ku_setup_timecount();
	ku_setup_inst_retired();
	ku_setup_cpu_freq();
	do_reset(0);
	printk(KERN_INFO "  kutrace_tracing = %d\n", kutrace_tracing);

	/* Finally, connect up the routines that can change the state */
	kutrace_global_ops.kutrace_trace_1 = &trace_1;
	kutrace_global_ops.kutrace_trace_2 = &trace_2;
	kutrace_global_ops.kutrace_trace_many = &trace_many;
	kutrace_global_ops.kutrace_trace_control = &kutrace_control;

	printk(KERN_INFO "  &kutrace_global_ops: " FUINTPTRX "\n", (uintptr_t)(&kutrace_global_ops));
	printk(KERN_INFO "  kutrace_trace All done init successfully!\n");
	return 0;
}

static void __exit kutrace_mod_exit(void)
{
	int cpu;
	printk(KERN_INFO "kutrace_mod Winding down =====================\n");
	/* Turn off tracing and quiesce */
	kutrace_tracing = false;
	msleep(20);	/* wait 20 msec for any pending tracing to finish */
	printk(KERN_INFO "  kutrace_tracing=false\n");

	/* Disconnect allthe routiens that can change state */
	kutrace_global_ops.kutrace_trace_1 = NULL;
	kutrace_global_ops.kutrace_trace_2 = NULL;
	kutrace_global_ops.kutrace_trace_many = NULL;
	kutrace_global_ops.kutrace_trace_control = NULL;
	printk(KERN_INFO "  kutrace_global_ops = NULL\n");

	/* Clear out all the pointers to trace data */
	for_each_online_cpu(cpu) {
		struct kutrace_traceblock* tb = &per_cpu(kutrace_traceblock_per_cpu, cpu);
		printk(KERN_INFO "  kutrace_traceblock_per_cpu[%d] = NULL\n", cpu);
		ATOMIC_SET(&tb->next, (uintptr_t)NULL);
		tb->limit = NULL;
		tb->prior_cycles = 0;		// IPC design
		tb->prior_inst_retired = 0;	// IPC design
	}

	traceblock_high = NULL;
	traceblock_limit = NULL;
	traceblock_next = NULL;

	/* Now that nothing points to it, free memory */
	if (tracebase) {vfree(tracebase);}
	if (kutrace_pid_filter) {vfree(kutrace_pid_filter);}
	kutrace_pid_filter = NULL;

	printk(KERN_INFO "  kutrace_tracebase = NULL\n");
	printk(KERN_INFO "  kutrace_pid_filter = NULL\n");

	printk(KERN_INFO "kutrace__mod Goodbye\n");
}


module_init(kutrace_mod_init);
module_exit(kutrace_mod_exit);


