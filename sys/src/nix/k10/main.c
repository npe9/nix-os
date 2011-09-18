#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "init.h"
#include "io.h"

Conf conf;			/* XXX - must go - gag */

extern void crapoptions(void);	/* XXX - must go */
extern void confsetenv(void);	/* XXX - must go */

static uintptr sp;		/* XXX - must go - user stack of init proc */

uintptr kseg0 = KZERO;
Sys* sys = nil;
usize sizeofSys = sizeof(Sys);

/*
 * Option arguments from the command line.
 * oargv[0] is the boot file.
 * Optionsinit() is called from multiboot() to
 * set it all up.
 */
static int oargc;
static char* oargv[20];
static char oargb[128];
static int oargblen;

char dbgflg[256];
static int vflag = 0;

void
optionsinit(char* s)
{
	oargblen = strecpy(oargb, oargb+sizeof(oargb), s) - oargb;
	oargc = tokenize(oargb, oargv, nelem(oargv)-1);
	oargv[oargc] = nil;
}

static void
options(int argc, char* argv[])
{
	char *p;
	int n, o;

	/*
	 * Process flags.
	 * Flags [A-Za-z] may be optionally followed by
	 * an integer level between 1 and 127 inclusive
	 * (no space between flag and level).
	 * '--' ends flag processing.
	 */
	while(--argc > 0 && (*++argv)[0] == '-' && (*argv)[1] != '-'){
		while(o = *++argv[0]){
			if(!(o >= 'A' && o <= 'Z') && !(o >= 'a' && o <= 'z'))
				continue;
			n = strtol(argv[0]+1, &p, 0);
			if(p == argv[0]+1 || n < 1 || n > 127)
				n = 1;
			argv[0] = p-1;
			dbgflg[o] = n;
		}
	}
	vflag = dbgflg['v'];
}

void
squidboy(int apicno)
{
	vlong hz;

	sys->machptr[m->machno] = m;

	/*
	 * Need something for initial delays
	 * until a timebase is worked out.
	 */
	m->cpuhz = 2000000000ll;
	m->cpumhz = 2000;
	m->perf.period = 1;

	m->nixtype = NIXAC;

	DBG("Hello Squidboy %d %d\n", apicno, m->machno);

	vsvminit(MACHSTKSZ, m->nixtype);

	/*
	 * Beware the Curse of The Non-Interruptable Were-Temporary.
	 */
	hz = archhz();
	if(hz == 0)
		ndnr();
	m->cpuhz = hz;
	m->cyclefreq = hz;
	m->cpumhz = hz/1000000ll;

	mmuinit();
	if(!apiconline())
		ndnr();

	fpuinit();

	acmodeset(m->nixtype);
	m->splpc = 0;
	m->online = 1;

	/*
	 * CAUTION: no time sync done, etc.
	 */
	DBG("Wait for the thunderbirds!\n");
	while(!active.thunderbirdsarego)
		;

	DBG("mach %d is go\n", m->machno);
	switch(m->nixtype){
	case NIXAC:
		acmmuswitch();
		acinit();
		adec(&active.nbooting);
//		ainc(&active.nonline);
		acsched();
		panic("squidboy");
		break;
	case NIXTC:
		/*
		 * We only need the idt and syscall entry point actually.
		 * At boot time the boot processor might set our role after
		 * we have decided to become an AC.
		 */
		vsvminit(MACHSTKSZ, NIXTC);

		timersinit();
		adec(&active.nbooting);
		ainc(&active.nonline);
ndnr();
		schedinit();
		break;
	}
	panic("squidboy returns (type %d)", m->nixtype);
}

static void
testiccs(void)
{
	int i;
	Mach *mp;
	extern void testicc(int);
	char *n[] = {
		[NIXAC] "AC",
		[NIXTC] "TC",
		[NIXKC]	"KC"
	};

	/* setup arguments for all */
	for(i = 1; i < MACHMAX; i++)
		if((mp = sys->machptr[i]) != nil && mp->online != 0){
			print("cpu%d machno %d role %s\n",
				i, mp->machno, n[mp->nixtype]);
		if(mp->nixtype == NIXAC)
			testicc(i);
	}
	print("bootcore: all cores done\n");
}

/*
 * Rendezvous with other cores. Set roles for those that came
 * up online, and wait until they are initialized.
 * We assume other processors that could boot had time to
 * set online to 1 by now.
 */
static void
nixsquids(void)
{
	Mach *mp;
	int i;
	uvlong now, start;

	for(i = 1; i < MACHMAX; i++)
		if((mp = sys->machptr[i]) != nil && mp->online != 0){
			/*
			 * Inter-core calls. A ensure *mp->iccall and mp->icargs
			 * go into different cache lines.
			 */
			mp->icc = mallocalign(sizeof *m->icc, ICCLNSZ, 0, 0);
			mp->icc->fn = nil;
			if(i < 4)
				mp->nixtype = NIXTC;
			ainc(&active.nbooting);
		}
	active.thunderbirdsarego = 1;
	start = fastticks2us(fastticks(nil));
	do{
		now = fastticks2us(fastticks(nil));
	}while(active.nbooting > 0 && now - start < 1000000)
		;
	if(active.nbooting > 0)
		print("cpu0: %d cores couldn't start\n", active.nbooting);
	active.nbooting = 0;
}

void
DONE(void)
{
	print("DONE\n");
	prflush();
	delay(10000);
	ndnr();
}

void
HERE(void)
{
	print("here\n");
	prflush();
	delay(5000);
}

void
main(u32int ax, u32int bx)
{
	vlong hz;

	memset(edata, 0, end - edata);

	/*
	 * ilock via i8250enable via i8250console
	 * needs m->machno, sys->machptr[] set, and
	 * also 'up' set to nil.
	 */
	cgapost(sizeof(uintptr)*8);
	memset(m, 0, sizeof(Mach));
	m->machno = 0;
	m->online = 1;
	m->nixtype = NIXTC;
	sys->machptr[m->machno] = &sys->mach;
	m->stack = PTR2UINT(sys->machstk);
	m->vsvm = sys->vsvmpage;
	up = nil;
	active.nonline = 1;
	active.exiting = 0;
	active.nbooting = 0;
	asminit();
	multiboot(ax, bx, 0);
	options(oargc, oargv);
	crapoptions();

	/*
	 * Need something for initial delays
	 * until a timebase is worked out.
	 */
	m->cpuhz = 2000000000ll;
	m->cpumhz = 2000;

	cgainit();
	i8250console("0");
	consputs = cgaconsputs;

	vsvminit(MACHSTKSZ, NIXTC);

	conf.nmach = 1;			

	fmtinit();
	print("\nNIX with 2M pages\n");
	if(vflag){
		print("&ax = %#p, ax = %#ux, bx = %#ux\n", &ax, ax, bx);
		multiboot(ax, bx, vflag);
	}

	m->perf.period = 1;
	if((hz = archhz()) != 0ll){
		m->cpuhz = hz;
		m->cyclefreq = hz;
		m->cpumhz = hz/1000000ll;
	}

	/*
	 * Mmuinit before meminit because it
	 * flushes the TLB via m->pml4->pa.
	 */
	mmuinit();

	ioinit();
	kbdinit();
	meminit();
	confinit();
	archinit();
	mallocinit();

	/*
	 * Acpiinit will cause the first malloc
	 * call to happen.
	 * If the system dies here it's probably due
	 * to malloc not being initialised
	 * correctly, or the data segment is misaligned
	 * (it's amazing how far you can get with
	 * things like that completely broken).
	 */
	acpiinit();
	umeminit();
	trapinit();
	printinit();

	/*
	 * This is necessary with GRUB and QEMU.
	 * Without it an interrupt can occur at a weird vector,
	 * because the vector base is likely different, causing
	 * havoc. Do it before any APIC initialisation.
	 */
	i8259init(32);

	mpsinit(32);	/* Use at most 32 cores */
	apiconline();
	sipi();

	timersinit();
	kbdenable();
	fpuinit();
	psinit(conf.nproc);
	initimage();
	links();
	devtabreset();
	pageinit();
	swapinit();
	userinit();
	nixsquids();
testiccs();	
	schedinit();
}

void
init0(void)
{
	char buf[2*KNAMELEN];

	up->nerrlab = 0;

//	if(consuart == nil)
//		i8250console("0");
	spllo();

	/*
	 * These are o.k. because rootinit is null.
	 * Then early kproc's will have a root and dot.
	 */
	up->slash = namec("#/", Atodir, 0, 0);
	pathclose(up->slash->path);
	up->slash->path = newpath("/");
	up->dot = cclone(up->slash);

	devtabinit();

	if(!waserror()){
		snprint(buf, sizeof(buf), "%s %s", "AMD64", conffile);
		ksetenv("terminal", buf, 0);
		ksetenv("cputype", "amd64", 0);
		if(cpuserver)
			ksetenv("service", "cpu", 0);
		else
			ksetenv("service", "terminal", 0);
		ksetenv("pgsz", "2097152", 0);
		confsetenv();
		poperror();
	}
	kproc("alarm", alarmkproc, 0);
	touser(sp);
}

void
bootargs(uintptr base)
{
	int i;
	ulong ssize;
	char **av, *p;

	/*
	 * Push the boot args onto the stack.
	 * Make sure the validaddr check in syscall won't fail
	 * because there are fewer than the maximum number of
	 * args by subtracting sizeof(up->arg).
	 */
	i = oargblen+1;
	p = UINT2PTR(STACKALIGN(base + BIGPGSZ - sizeof(up->arg) - i));
	memmove(p, oargb, i);

	/*
	 * Now push argc and the argv pointers.
	 * This isn't strictly correct as the code jumped to by
	 * touser in init9.[cs] calls startboot (port/initcode.c) which
	 * expects arguments
	 * 	startboot(char* argv0, char* argv[])
	 * not the usual (int argc, char* argv[]), but argv0 is
	 * unused so it doesn't matter (at the moment...).
	 */
	av = (char**)(p - (oargc+2)*sizeof(char*));
	ssize = base + BIGPGSZ - PTR2UINT(av);
	*av++ = (char*)oargc;
	for(i = 0; i < oargc; i++)
		*av++ = (oargv[i] - oargb) + (p - base) + (USTKTOP - BIGPGSZ);
	*av = nil;

	sp = USTKTOP - ssize;
}

void
userinit(void)
{
	Proc *p;
	Segment *s;
	KMap *k;
	Page *pg;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = smalloc(sizeof(Egrp));
	p->egrp->ref = 1;
	p->fgrp = dupfgrp(nil);
	p->rgrp = newrgrp();
	p->procmode = 0640;

	kstrdup(&eve, "");
	kstrdup(&p->text, "*init*");
	kstrdup(&p->user, eve);

	/*
	 * Kernel Stack
	 *
	 * N.B. make sure there's enough space for syscall to check
	 *	for valid args and
	 *	space for gotolabel's return PC
	 * AMD64 stack must be quad-aligned.
	 */
	p->sched.pc = PTR2UINT(init0);
	p->sched.sp = PTR2UINT(p->kstack+KSTACK-sizeof(up->arg)-sizeof(uintptr));
	p->sched.sp = STACKALIGN(p->sched.sp);

	/*
	 * User Stack
	 *
	 * Technically, newpage can't be called here because it
	 * should only be called when in a user context as it may
	 * try to sleep if there are no pages available, but that
	 * shouldn't be the case here.
	 */
	s = newseg(SG_STACK, USTKTOP-USTKSIZE, USTKSIZE/BIGPGSZ);
	p->seg[SSEG] = s;

	pg = newpage(1, 0, USTKTOP-BIGPGSZ, BIGPGSZ);
	segpage(s, pg);
	k = kmap(pg);
	bootargs(VA(k));
	kunmap(k);

	/*
	 * Text
	 */
	s = newseg(SG_TEXT, UTZERO, 1);
	s->flushme++;
	p->seg[TSEG] = s;
	pg = newpage(1, 0, UTZERO, BIGPGSZ);
	memset(pg->cachectl, PG_TXTFLUSH, sizeof(pg->cachectl));
	segpage(s, pg);
	k = kmap(s->map[0]->pages[0]);
	memmove(UINT2PTR(VA(k)), initcode, sizeof initcode);
	kunmap(k);

	ready(p);
}

void
confinit(void)
{
	int i;

	conf.npage = 0;
	for(i=0; i<nelem(conf.mem); i++)
		conf.npage += conf.mem[i].npage;

	conf.nproc = 100 + ((conf.npage*PGSZ)/MB)*5;
	if(cpuserver)
		conf.nproc *= 3;
	if(conf.nproc > 1000)
		conf.nproc = 1000;
	conf.nimage = 200;
	conf.nswap = conf.nproc*80;
	conf.nswppo = 4096;
}

static void
shutdown(int ispanic)
{
	int ms, once;

	lock(&active);
	if(ispanic)
		active.ispanic = ispanic;
	else if(m->machno == 0 && m->online == 0)
		active.ispanic = 0;
	once = m->online;
	m->online = 0;
	adec(&active.nonline);
	active.exiting = 1;
	unlock(&active);

	if(once)
		iprint("cpu%d: exiting\n", m->machno);
	spllo();
	for(ms = 5*1000; ms > 0; ms -= TK2MS(2)){
		delay(TK2MS(2));
		if(active.nonline == 0 && consactive() == 0)
			break;
	}

	if(active.ispanic && m->machno == 0){
		if(cpuserver)
			delay(30000);
		else
			for(;;)
				halt();
	}
	else
		delay(1000);
}

void
reboot(void*, void*, long)
{
	panic("reboot\n");
}

void
exit(int ispanic)
{
	shutdown(ispanic);
	archreset();
}
