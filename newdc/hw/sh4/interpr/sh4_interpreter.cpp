/*
	Highly inefficient and boring interpreter. Nothing special here
*/

#include "types.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_rom.h"
#include "../sh4_if.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/aica/aica_if.h"
#include "../modules/dmac.h"
#include "hw/gdrom/gdrom_if.h"
#include "hw/maple/maple_if.h"
#include "../sh4_interrupts.h"
#include "../modules/tmu.h"
#include "hw/sh4/sh4_mem.h"
#include "../modules/ccn.h"
#include "profiler/profiler.h"
#include "../dyna/blockmanager.h"
#include "../sh4_sched.h"

#include <time.h>
#include <float.h>

#define SH4_TIMESLICE (448)
#define CPU_RATIO      (8)

//uh uh
#define GetN(str) ((str>>8) & 0xf)
#define GetM(str) ((str>>4) & 0xf)

void Sh4_int_Run()
{
	sh4_int_bCpuRun=true;

	s32 l=SH4_TIMESLICE;

	do
	{
		do
		{
			u32 op=ReadMem16(next_pc);
			next_pc+=2;

			OpPtr[op](op);
			l-=CPU_RATIO;
		} while(l>0);
		l+=SH4_TIMESLICE;
		UpdateSystem_INTC();

	} while(sh4_int_bCpuRun);

	sh4_int_bCpuRun=false;
}

void Sh4_int_Stop()
{
	if (sh4_int_bCpuRun)
	{
		sh4_int_bCpuRun=false;
	}
}

void Sh4_int_Step()
{
	if (sh4_int_bCpuRun)
	{
		printf("Sh4 Is running , can't step\n");
	}
	else
	{
		u32 op=ReadMem16(next_pc);
		next_pc+=2;
		ExecuteOpcode(op);
	}
}

void Sh4_int_Skip()
{
	if (sh4_int_bCpuRun)
	{
		printf("Sh4 Is running, can't Skip\n");
	}
	else
	{
		next_pc+=2;
	}
}

void Sh4_int_Reset(bool Manual)
{
	if (sh4_int_bCpuRun)
	{
		printf("Sh4 Is running, can't Reset\n");
	}
	else
	{
		next_pc = 0xA0000000;

		memset(r,0,sizeof(r));
		memset(r_bank,0,sizeof(r_bank));

		gbr=ssr=spc=sgr=dbr=vbr=0;
		mac.full=pr=fpul=0;

		sr.SetFull(0x700000F0);
		old_sr.status=sr.status;
		UpdateSR();

		fpscr.full = 0x0004001;
		old_fpscr=fpscr;
		UpdateFPSCR();

		//Any more registers have default value ?
		printf("Sh4 Reset\n");
	}
}

bool Sh4_int_IsCpuRunning()
{
	return sh4_int_bCpuRun;
}

//TODO : Check for valid delayslot instruction
void ExecuteDelayslot()
{
	u32 op=IReadMem16(next_pc);
	next_pc+=2;
	if (op!=0)
		ExecuteOpcode(op);
}

void ExecuteDelayslot_RTE()
{
	sr.SetFull(ssr);

	ExecuteDelayslot();
}

//General update

//3584 Cycles
#define AICA_SAMPLE_GCM 441
#define AICA_SAMPLE_CYCLES (SH4_MAIN_CLOCK/(44100/AICA_SAMPLE_GCM)*32)

int aica_schid;
int rtc_schid;

//14336 Cycles

const int AICA_TICK=145124;

int AicaUpdate(int tag, int c, int j)
{
	//gpc_counter=0;
	//bm_Periodical_14k();

	//static int aica_sample_cycles=0;
	//aica_sample_cycles+=14336*AICA_SAMPLE_GCM;

	//if (aica_sample_cycles>=AICA_SAMPLE_CYCLES)
	{
		UpdateArm(512*32);
		UpdateAica(1*32);
		//aica_sample_cycles-=AICA_SAMPLE_CYCLES;
	}

	return AICA_TICK;
}

#ifdef LIMIT_TIME
	#define TIMELIM 640

	int timelim=TIMELIM;
#endif

#ifdef LIMIT_TIME
int rseed = 192;
#define RAND_NUM(x) (x)
#endif

int DreamcastSecond(int tag, int c, int j)
{
	settings.dreamcast.RTC++;

#if 1 //HOST_OS==OS_WINDOWS
	prof_periodical();
#endif

#if !defined(HOST_NO_REC)
	bm_Periodical_1s();
#endif

	//printf("%d ticks\n",sh4_sched_intr);
	sh4_sched_intr=0;

	#ifdef LIMIT_TIME
		extern u32 LastAddr;

		if (--timelim<0)
		{
			//crash -> "Out of time"
			timelim+=TIMELIM-RAND_NUM(0)%64;
			(u32&)p_sh4rcb->cntx.interrupt_pend=RAND_NUM((u8*)0 - (u8*)p_sh4rcb);
			//(u32&)p_sh4rcb->cntx.pr=RAND_NUM(0);
			//(u32&)do_sqw_nommu=RAND_NUM(0);
			(u32&)LastAddr=RAND_NUM( 9873123456^((u8*)p_sh4rcb - (u8*)0) );
		}
	#endif

	return SH4_MAIN_CLOCK;
}

int UpdateSystem_rec()
{
	//WIP
	if (Sh4cntx.sh4_sched_next<0)
		sh4_sched_tick(448);

	return Sh4cntx.interrupt_pend;
}

//448 Cycles (fixed)
int UpdateSystem()
{
	//this is an optimisation (mostly for ARM)
	//makes scheduling easier !
	//update_fp* tmu=pUpdateTMU;
	
	Sh4cntx.sh4_sched_next-=448;
	if (Sh4cntx.sh4_sched_next<0)
		sh4_sched_tick(448);

	return Sh4cntx.interrupt_pend;
}

int UpdateSystem_INTC()
{
	UpdateSystem();
	return UpdateINTC();
}

void sh4_int_resetcache() { }
//Get an interface to sh4 interpreter
void Get_Sh4Interpreter(sh4_if* rv)
{
	rv->Run=Sh4_int_Run;
	rv->Stop=Sh4_int_Stop;
	rv->Step=Sh4_int_Step;
	rv->Skip=Sh4_int_Skip;
	rv->Reset=Sh4_int_Reset;
	rv->Init=Sh4_int_Init;
	rv->Term=Sh4_int_Term;
	rv->IsCpuRunning=Sh4_int_IsCpuRunning;

	rv->ResetCache=sh4_int_resetcache;
}

void Sh4_int_Init()
{
	#ifdef LIMIT_TIME
		timelim-=rand()%64;
	#endif

	verify(sizeof(Sh4cntx)==448);

	aica_schid=sh4_sched_register(0,&AicaUpdate);
	sh4_sched_request(aica_schid,AICA_TICK);

	rtc_schid=sh4_sched_register(0,&DreamcastSecond);
	sh4_sched_request(rtc_schid,SH4_MAIN_CLOCK);
}

void Sh4_int_Term()
{
	Sh4_int_Stop();
	printf("Sh4 Term\n");
}
