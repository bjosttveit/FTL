/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Signal processing routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#include "signals.h"
// logg()
#include "log.h"
// free()
#include "memory.h"
// ls_dir()
#include "files.h"
// gettid()
#include "daemon.h"
// Eventqueue routines
#include "events.h"

#define BINARY_NAME "pihole-FTL"

volatile sig_atomic_t killed = 0;
static volatile pid_t mpid = -1;
static time_t FTLstarttime = 0;
extern volatile int exit_code;

// Return the (null-terminated) name of the calling thread
// The name is stored in the buffer as well as returned for convenience
static char * __attribute__ ((nonnull (1))) getthread_name(char buffer[16])
{
	prctl(PR_GET_NAME, buffer, 0, 0, 0);
	return buffer;
}

#if defined(__GLIBC__)
static void print_addr2line(const char *symbol, const void *address, const int j, const void *offset)
{
	// Only do this analysis for our own binary (skip trying to analyse libc.so, etc.)
	if(strstr(symbol, BINARY_NAME) == NULL)
		return;

	// Find first occurence of '(' or ' ' in the obtaned symbol string and
	// assume everything before that is the file name. (Don't go beyond the
	// string terminator \0)
	int p = 0;
	while(symbol[p] != '(' && symbol[p] != ' ' && symbol[p] != '\0')
		p++;

	// Compute address cleaned by binary offset
	void *addr = (void*)(address-offset);

	// Invoke addr2line command and get result through pipe
	char addr2line_cmd[256];
	snprintf(addr2line_cmd, sizeof(addr2line_cmd), "addr2line %p -e %.*s", addr, p, symbol);
	FILE *addr2line = NULL;
	char linebuffer[256];
	if((addr2line = popen(addr2line_cmd, "r")) != NULL &&
	   fgets(linebuffer, sizeof(linebuffer), addr2line) != NULL)
	{
		char *pos;
		// Strip possible newline at the end of the addr2line output
		if ((pos=strchr(linebuffer, '\n')) != NULL)
			*pos = '\0';
	}
	else
	{
		snprintf(linebuffer, sizeof(linebuffer), "N/A (%p)", addr);
	}
	// Log result
	logg("L[%04i]: %s", j, linebuffer);

	// Close pipe
	pclose(addr2line);
}
#endif

static void __attribute__((noreturn)) signal_handler(int sig, siginfo_t *si, void *unused)
{
	logg("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
	logg("---------------------------->  FTL crashed!  <----------------------------");
	logg("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
	logg("Please report a bug at https://github.com/pi-hole/FTL/issues");
	logg("and include in your report already the following details:");

	if(FTLstarttime != 0)
	{
		logg("FTL has been running for %lli seconds", (long long)time(NULL) - FTLstarttime);
	}
	log_FTL_version(true);
	char namebuf[16];
	logg("Process details: MID: %i", mpid);
	logg("                 PID: %i", getpid());
	logg("                 TID: %i", gettid());
	logg("                 Name: %s", getthread_name(namebuf));

	logg("Received signal: %s", strsignal(sig));
	logg("     at address: %p", si->si_addr);

	// Segmentation fault - program crashed
	if(sig == SIGSEGV)
	{
		switch (si->si_code)
		{
			case SEGV_MAPERR:  logg("     with code:  SEGV_MAPERR (Address not mapped to object)"); break;
			case SEGV_ACCERR:  logg("     with code:  SEGV_ACCERR (Invalid permissions for mapped object)"); break;
#ifdef SEGV_BNDERR
			case SEGV_BNDERR:  logg("     with code:  SEGV_BNDERR (Failed address bound checks)"); break;
#endif
#ifdef SEGV_PKUERR
			case SEGV_PKUERR:  logg("     with code:  SEGV_PKUERR (Protection key checking failure)"); break;
#endif
#ifdef SEGV_ACCADI
			case SEGV_ACCADI:  logg("     with code:  SEGV_ACCADI (ADI not enabled for mapped object)"); break;
#endif
#ifdef SEGV_ADIDERR
			case SEGV_ADIDERR: logg("     with code:  SEGV_ADIDERR (Disrupting MCD error)"); break;
#endif
#ifdef SEGV_ADIPERR
			case SEGV_ADIPERR: logg("     with code:  SEGV_ADIPERR (Precise MCD exception)"); break;
#endif
			default:           logg("     with code:  Unknown (%i)", si->si_code); break;
		}
	}

	// Bus error - memory manager problem
	else if(sig == SIGBUS)
	{
		switch (si->si_code)
		{
			case BUS_ADRALN:    logg("     with code:  BUS_ADRALN (Invalid address alignment)"); break;
			case BUS_ADRERR:    logg("     with code:  BUS_ADRERR (Non-existant physical address)"); break;
			case BUS_OBJERR:    logg("     with code:  BUS_OBJERR (Object specific hardware error)"); break;
			case BUS_MCEERR_AR: logg("     with code:  BUS_MCEERR_AR (Hardware memory error: action required)"); break;
			case BUS_MCEERR_AO: logg("     with code:  BUS_MCEERR_AO (Hardware memory error: action optional)"); break;
			default:            logg("     with code:  Unknown (%i)", si->si_code); break;
		}
	}

	// Illegal error - Illegal instruction detected
	else if(sig == SIGILL)
	{
		switch (si->si_code)
		{
			case ILL_ILLOPC:   logg("     with code:  ILL_ILLOPC (Illegal opcode)"); break;
			case ILL_ILLOPN:   logg("     with code:  ILL_ILLOPN (Illegal operand)"); break;
			case ILL_ILLADR:   logg("     with code:  ILL_ILLADR (Illegal addressing mode)"); break;
			case ILL_ILLTRP:   logg("     with code:  ILL_ILLTRP (Illegal trap)"); break;
			case ILL_PRVOPC:   logg("     with code:  ILL_PRVOPC (Privileged opcode)"); break;
			case ILL_PRVREG:   logg("     with code:  ILL_PRVREG (Privileged register)"); break;
			case ILL_COPROC:   logg("     with code:  ILL_COPROC (Coprocessor error)"); break;
			case ILL_BADSTK:   logg("     with code:  ILL_BADSTK (Internal stack error)"); break;
#ifdef ILL_BADIADDR
			case ILL_BADIADDR: logg("     with code:  ILL_BADIADDR (Unimplemented instruction address)"); break;
#endif
			default:           logg("     with code:  Unknown (%i)", si->si_code); break;
		}
	}

	// Floating point exception error
	else if(sig == SIGFPE)
	{
		switch (si->si_code)
		{
			case FPE_INTDIV:   logg("     with code:  FPE_INTDIV (Integer divide by zero)"); break;
			case FPE_INTOVF:   logg("     with code:  FPE_INTOVF (Integer overflow)"); break;
			case FPE_FLTDIV:   logg("     with code:  FPE_FLTDIV (Floating point divide by zero)"); break;
			case FPE_FLTOVF:   logg("     with code:  FPE_FLTOVF (Floating point overflow)"); break;
			case FPE_FLTUND:   logg("     with code:  FPE_FLTUND (Floating point underflow)"); break;
			case FPE_FLTRES:   logg("     with code:  FPE_FLTRES (Floating point inexact result)"); break;
			case FPE_FLTINV:   logg("     with code:  FPE_FLTINV (Floating point invalid operation)"); break;
			case FPE_FLTSUB:   logg("     with code:  FPE_FLTSUB (Subscript out of range)"); break;
#ifdef FPE_FLTUNK
			case FPE_FLTUNK:   logg("     with code:  FPE_FLTUNK (Undiagnosed floating-point exception)"); break;
#endif
#ifdef FPE_CONDTRAP
			case FPE_CONDTRAP: logg("     with code:  FPE_CONDTRAP (Trap on condition)"); break;
#endif
			default:           logg("     with code:  Unknown (%i)", si->si_code); break;
		}
	}

// Check GLIBC availability as MUSL does not support live backtrace generation
#if defined(__GLIBC__)
	// Try to obtain backtrace. This may not always be helpful, but it is better than nothing
	void *buffer[255];
	const int calls = backtrace(buffer, sizeof(buffer)/sizeof(void *));
	logg("Backtrace:");

	char ** bcktrace = backtrace_symbols(buffer, calls);
	if(bcktrace == NULL)
		logg("Unable to obtain backtrace symbols!");

	// Try to compute binary offset from backtrace_symbols result
	void *offset = NULL;
	for(int j = 0; j < calls; j++)
	{
		void *p1 = NULL, *p2 = NULL;
		char *pend = NULL;
		if((pend = strrchr(bcktrace[j], '(')) != NULL &&
		   strstr(bcktrace[j], BINARY_NAME) != NULL &&
		   sscanf(pend, "(+%p) [%p]", &p1, &p2) == 2)
		   offset = (void*)(p2-p1);
	}

	for(int j = 0; j < calls; j++)
	{
		logg("B[%04i]: %s", j,
		     bcktrace != NULL ? bcktrace[j] : "---");

		if(bcktrace != NULL)
			print_addr2line(bcktrace[j], buffer[j], j, offset);
	}
	if(bcktrace != NULL)
		free(bcktrace);
#else
	logg("!!! INFO: pihole-FTL has not been compiled with glibc/backtrace support, not generating one !!!");
#endif
	// Print content of /dev/shm
	ls_dir("/dev/shm");

	logg("Please also include some lines from above the !!!!!!!!! header.");
	logg("Thank you for helping us to improve our FTL engine!");

	// Terminate main process if crash happened in a TCP worker
	if(mpid != getpid())
	{
		// This is a forked process
		logg("Asking parent pihole-FTL (PID %i) to shut down", (int)mpid);
		kill(mpid, SIGRTMIN+2);
		logg("FTL fork terminated!");
	}
	else
	{
		// This is the main process
		logg("FTL terminated!");
	}

	// Terminate process indicating failure
	exit(EXIT_FAILURE);
}

static void SIGRT_handler(int signum, siginfo_t *si, void *unused)
{ 
	// Ignore real-time signals outside of the main process (TCP forks)
	if(mpid != getpid())
		return;

	int rtsig = signum - SIGRTMIN;
	logg("Received: %s (%d -> %d)", strsignal(signum), signum, rtsig);

	if(rtsig == 0)
	{
		// Reload
		// - gravity
		// - exact whitelist
		// - regex whitelist
		// - exact blacklist
		// - exact blacklist
		// WITHOUT wiping the DNS cache itself
		set_event(RELOAD_GRAVITY);

		// Reload the privacy level in case the user changed it
		set_event(RELOAD_PRIVACY_LEVEL);
	}
	else if(rtsig == 2)
	{
		// Terminate FTL indicating failure
		exit_code = EXIT_FAILURE;
		kill(0, SIGTERM);
	}
	else if(rtsig == 3)
	{
		// Reimport alias-clients from database
		set_event(REIMPORT_ALIASCLIENTS);
	}
	else if(rtsig == 4)
	{
		// Re-resolve all clients and forward destinations
		set_event(RERESOLVE_HOSTNAMES);
	}
	else if(rtsig == 5)
	{
		// Parse neighbor cache
		set_event(PARSE_NEIGHBOR_CACHE);
	}
}

// Register SIGSEGV handler
void handle_SIGSEGV(void)
{
	struct sigaction old_action;

	// Catch SIGSEGV
	sigaction (SIGSEGV, NULL, &old_action);
	if(old_action.sa_handler != SIG_IGN)
	{
		struct sigaction SEGVaction;
		memset(&SEGVaction, 0, sizeof(struct sigaction));
		SEGVaction.sa_flags = SA_SIGINFO;
		sigemptyset(&SEGVaction.sa_mask);
		SEGVaction.sa_sigaction = &signal_handler;
		sigaction(SIGSEGV, &SEGVaction, NULL);
	}

	// Catch SIGBUS
	sigaction (SIGBUS, NULL, &old_action);
	if(old_action.sa_handler != SIG_IGN)
	{
		struct sigaction SBUGaction;
		memset(&SBUGaction, 0, sizeof(struct sigaction));
		SBUGaction.sa_flags = SA_SIGINFO;
		sigemptyset(&SBUGaction.sa_mask);
		SBUGaction.sa_sigaction = &signal_handler;
		sigaction(SIGBUS, &SBUGaction, NULL);
	}

	// Catch SIGILL
	sigaction (SIGILL, NULL, &old_action);
	if(old_action.sa_handler != SIG_IGN)
	{
		struct sigaction SBUGaction;
		memset(&SBUGaction, 0, sizeof(struct sigaction));
		SBUGaction.sa_flags = SA_SIGINFO;
		sigemptyset(&SBUGaction.sa_mask);
		SBUGaction.sa_sigaction = &signal_handler;
		sigaction(SIGILL, &SBUGaction, NULL);
	}

	// Catch SIGFPE
	sigaction (SIGFPE, NULL, &old_action);
	if(old_action.sa_handler != SIG_IGN)
	{
		struct sigaction SBUGaction;
		memset(&SBUGaction, 0, sizeof(struct sigaction));
		SBUGaction.sa_flags = SA_SIGINFO;
		sigemptyset(&SBUGaction.sa_mask);
		SBUGaction.sa_sigaction = &signal_handler;
		sigaction(SIGFPE, &SBUGaction, NULL);
	}

	// Log start time of FTL
	FTLstarttime = time(NULL);

}

// Register real-time signal handler
void handle_realtime_signals(void)
{
	// This function is only called once (after forking), store the PID of
	// the main process
	mpid = getpid();

	// Catch all real-time signals
	for(int signum = SIGRTMIN; signum <= SIGRTMAX; signum++)
	{
		struct sigaction SIGACTION;
		memset(&SIGACTION, 0, sizeof(struct sigaction));
		SIGACTION.sa_flags = SA_SIGINFO;
		sigemptyset(&SIGACTION.sa_mask);
		SIGACTION.sa_sigaction = &SIGRT_handler;
		sigaction(signum, &SIGACTION, NULL);
	}
}

// Return PID of the main FTL process
pid_t main_pid(void)
{
	return mpid;
}
