#include <inc/assert.h>
#include <inc/x86.h>
#include <kern/spinlock.h>
#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/monitor.h>

void sched_halt(void);
extern struct spinlock sched_lock;
/*
 * Choose a user environment to run and run it.
 */
void sched_yield(void)
{
    struct env *env;
		size_t cur_cycle = 1;
		spin_lock(&sched_lock);
		lock_env();
    /*
     * Implement simple round-robin scheduling.
     *
     * Search through 'envs' for an ENV_RUNNABLE environment in
     * circular fashion starting just after the env this CPU was
     * last running.  Switch to the first such environment found.
     *
     * If no envs are runnable, but the environment previously
     * running on this CPU is still ENV_RUNNING, it's okay to
     * choose that environment.
     *
     * Never choose an environment that's currently running (on
     * another CPU, if we had, ie., env_status == ENV_RUNNING).
     * If there are
     * no runnable environments, simply drop through to the code
     * below to halt the cpu.
     *
     * LAB 5: Your code here.
     */
		 env = curenv && curenv->env_link? curenv->env_link : env_run_list;
    //  if(runnable_envs == 2)
    //   goto halt;
		 if(curenv && (read_tsc() - curenv->env_ts) < DEFAULT_ENV_TS)
			 env = curenv; // Continue doing current env

		 while(env){
			 	if(env == curenv)
					goto run;
				if(runnable_envs < cur_cycle++)
					goto halt;
				//  As far as env - is located in env_run list it can has either ENV_RUNNING or ENV_RUNNABLE status
				int status = xchg(&env->env_status, ENV_RUNNING);
			 	if( status == ENV_RUNNABLE || status == ENV_DYING){
					env->env_ts = read_tsc();
					goto run;
				}

				if(env->env_link)
					env = env->env_link;
				else
					env = env_run_list;
		 }
		 /* sched_halt never returns */
		halt:
			unlock_env();
			spin_unlock(&sched_lock);
		 	sched_halt();
		run:
			unlock_env();
			spin_unlock(&sched_lock);
			env_run(env);
}

/*
 * Halt this CPU when there is nothing to do. Wait until the timer interrupt
 * wakes it up. This function never returns.
 */
void sched_halt(void)
{
    int i;
    /* For debugging and testing purposes, if there are no runnable
     * environments in the system, then drop into the kernel monitor. */
    for (i = 0; i < NENV; i++) {
        if ((envs[i].env_status == ENV_RUNNABLE ||
             envs[i].env_status == ENV_RUNNING ||
             envs[i].env_status == ENV_DYING))
            break;
    }
		/* Release the big kernel lock as if we were "leaving" the kernel */
		if (i == NENV) {
        cprintf("No runnable environments in the system!\n");
        while (1)
            monitor(NULL);
    }
    /* Mark that no environment is running on this CPU */
    curenv = NULL;
    lcr3(PADDR(kern_pgdir));
    /* Mark that this CPU is in the HALT state, so that when
     * timer interupts come in, we know we should re-acquire the
     * big kernel lock */
    xchg(&thiscpu->cpu_status, CPU_HALTED);

    /* Reset stack pointer, enable interrupts and then halt. */
    asm volatile (
        "movl $0, %%ebp\n"
        "movl %0, %%esp\n"
        "pushl $0\n"
        "pushl $0\n"
        "sti\n"
        "hlt\n"
    : : "a" (thiscpu->cpu_ts.ts_esp0));
}
