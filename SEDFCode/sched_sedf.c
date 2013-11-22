/******************************************************************************
 * Simple EDF scheduler for xen
 *
 * by Stephan Diestelhorst (C)  2004 Cambridge University
 * based on code by Mark Williamson (C) 2004 Intel Research Cambridge
 */

#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/sched-if.h>
#include <xen/timer.h>
#include <xen/softirq.h>
#include <xen/time.h>
#include <xen/errno.h>

#define SEDF_ASLEEP (16)

#define PERIOD_MAX MILLISECS(10000) /* 10s  */
#define PERIOD_MIN (MICROSECS(10))  /* 10us */
#define SLICE_MIN (MICROSECS(5))    /*  5us */

#define EQ(a, b) ((!!(a)) == (!!(b)))

struct sedf_dom_info {
    struct domain  *domain;
};

struct sedf_priv_info {
    /* lock for the whole pluggable scheduler, nests inside cpupool_lock */
    spinlock_t lock;
};

struct sedf_vcpu_info {

    struct vcpu *vcpu;
    struct list_head list;

/* Example command line input: xl sedf vcpu1 --period=10 --budget=3 */
    /*TODO: Verify and rename CBS variables as needed
     *TODO: Combine these variables with the existing SEDF variables
     *      using well thought-out names*/
    /* Parameters for CBS */
    s_time_t cbs_period;
    s_time_t cbs_current_deadline;
    s_time_t cbs_max_budget;
    s_time_t cbs_current_budget;

    /* Parameters for EDF */
    /* TODO: verify if we can refactor period to deadline? */
    s_time_t  period;  /* = relative deadline */ /* ~= cbs_period */
    s_time_t  slice;   /* = worst case execution time */ /* ~= cbs_max_budget */

    /* Status of domain */
    int       status;

    /* Bookkeeping */
    s_time_t  deadl_abs; /* ~= cbs_current_deadline */
    s_time_t  sched_start_abs;
    s_time_t  cputime;   /* ~= cbs_current_budget (inveted, i.e. accumulated time vs. consumed time */
    s_time_t  block_abs;
    
    /* Statistics */
#ifdef SEDF_STATS
    s_time_t  block_time_tot;
    s_time_t  penalty_time_tot;
#endif
};

struct sedf_cpu_info {
    struct   list_head runnableq;
    struct   list_head waitq;
    s_time_t current_slice_expires;
};

#define SEDF_PRIV(_ops) \
    ((struct sedf_priv_info *)((_ops)->sched_data))
#define EDOM_INFO(d)   ((struct sedf_vcpu_info *)((d)->sched_priv))
#define CPU_INFO(cpu)  \
    ((struct sedf_cpu_info *)per_cpu(schedule_data, cpu).sched_priv)
#define LIST(d)        (&EDOM_INFO(d)->list)
#define RUNQ(cpu)      (&CPU_INFO(cpu)->runnableq)
#define WAITQ(cpu)     (&CPU_INFO(cpu)->waitq)
#define IDLETASK(cpu)  (idle_vcpu[cpu])

#define PERIOD_BEGIN(inf) ((inf)->deadl_abs - (inf)->period)

#define DIV_UP(x,y) (((x) + (y) - 1) / y)

#define sedf_runnable(edom)  (!(EDOM_INFO(edom)->status & SEDF_ASLEEP))


static void sedf_dump_cpu_state(const struct scheduler *ops, int i);

#define DOMAIN_COMPARER(name, field, comp1, comp2)                      \
static int name##_comp(struct list_head* el1, struct list_head* el2)    \
{                                                                       \
    struct sedf_vcpu_info *d1, *d2;                                     \
    d1 = list_entry(el1,struct sedf_vcpu_info, field);                  \
    d2 = list_entry(el2,struct sedf_vcpu_info, field);                  \
    if ( (comp1) == (comp2) )                                           \
        return 0;                                                       \
    if ( (comp1) < (comp2) )                                            \
        return -1;                                                      \
    else                                                                \
        return 1;                                                       \
}

static inline void __del_from_queue(struct vcpu *d)
{
    struct list_head *list = LIST(d);
    ASSERT(__task_on_queue(d));
    list_del(list);
    list->next = NULL;
    ASSERT(!__task_on_queue(d));
}

typedef int(*list_comparer)(struct list_head* el1, struct list_head* el2);

static inline void list_insert_sort(
    struct list_head *list, struct list_head *element, list_comparer comp)
{
    struct list_head     *cur;

    /* Iterate through all elements to find our "hole" */
    list_for_each( cur, list )
        if ( comp(element, cur) < 0 )
            break;

    /* cur now contains the element, before which we'll enqueue */
    list_add(element, cur->prev);
}

/********** Add to WaitQ ************
 * Adds a domain to the queue of processes which wait for the beginning of the
 * next period; this list is therefore sortet by this time, which is simply
 * absol. deadline - period.
 */
DOMAIN_COMPARER(waitq, list, PERIOD_BEGIN(d1), PERIOD_BEGIN(d2));

static inline void __add_to_waitqueue_sort(struct vcpu *v)
{
    ASSERT(!__task_on_queue(v));
    list_insert_sort(WAITQ(v->processor), LIST(v), waitq_comp);
    ASSERT(__task_on_queue(v));
}

/********** Add to RunQ *************
 * Adds a domain to the queue of processes which have started their current
 * period and are runnable (i.e. not blocked, dieing,...). The first element
 * on this list is running on the processor, if the list is empty the idle
 * task will run. As we are implementing EDF, this list is sorted by deadlines.
 */
DOMAIN_COMPARER(runq, list, d1->deadl_abs, d2->deadl_abs);

static inline void __add_to_runqueue_sort(struct vcpu *v)
{
    list_insert_sort(RUNQ(v->processor), LIST(v), runq_comp);
}

static inline int __task_on_queue(struct domain *d) {
	return (((LIST(d))->next != NULL) && (LIST(d)->next != LIST(d)));
}

/* Required function to satisfy scheduler interface */
static void sedf_insert_vcpu(const struct scheduler *ops, struct vcpu *v)
{
    if (is_idle_vcpu(v))
    {
        EDOM_INFO(v)->deadl_abs = 0;
        EDOM_INFO(v)->status &= ~SEDF_ASLEEP;
    }
}

static void *sedf_alloc_vdata(const struct scheduler *ops, struct vcpu *v, void *dd) /* dd = domain data */
{
    struct sedf_vcpu_info *inf;

    inf = xzalloc(struct sedf_vcpu_info);
    if ( inf == NULL )
        return NULL;

    inf->vcpu = v;

    /* Every VCPU starts asleep */
    inf->deadl_abs   = 0;
    inf->status      = SEDF_ASLEEP;

    /* TODO: Add invalid default CBS parameters so user is forced to set parameters */
    /* Default CBS Parameters */
    inf->cbs_period           = MILLISECS(10);
    inf->cbs_max_budget       = MILLISECS(10);
    inf->cbs_current_budget   = 0;
    inf->cbs_current_deadline = 0;
    
    /* TODO: Remove these variables as soon as safely possible */
    inf->period      = inf->cbs_period;
    inf->slice       = inf->cbs_max_budget;

    inf->period_orig = inf->period; inf->slice_orig = inf->slice;
    INIT_LIST_HEAD(&(inf->list));

    SCHED_STAT_CRANK(vcpu_init);

    return inf;
}

static void *
sedf_alloc_pdata(const struct scheduler *ops, int cpu)
{
    struct sedf_cpu_info *spc;

    spc = xzalloc(struct sedf_cpu_info);
    BUG_ON(spc == NULL);
    INIT_LIST_HEAD(&spc->waitq);
    INIT_LIST_HEAD(&spc->runnableq);

    return (void *)spc;
}

static void
sedf_free_pdata(const struct scheduler *ops, void *spc, int cpu)
{
    if ( spc == NULL )
        return;

    xfree(spc);
}

static void sedf_free_vdata(const struct scheduler *ops, void *priv)
{
    xfree(priv);
}

static void *
sedf_alloc_domdata(const struct scheduler *ops, struct domain *d)
{
    return xzalloc(struct sedf_dom_info);
}

static int sedf_init_domain(const struct scheduler *ops, struct domain *d)
{
    d->sched_priv = sedf_alloc_domdata(ops, d);
    if ( d->sched_priv == NULL )
        return -ENOMEM;

    return 0;
}

static void sedf_free_domdata(const struct scheduler *ops, void *data)
{
    xfree(data);
}

static void sedf_destroy_domain(const struct scheduler *ops, struct domain *d)
{
    sedf_free_domdata(ops, d->sched_priv);
}


/*
** CBS TODO: Might need to use this for multiprocessor support
*/
static int sedf_pick_cpu(const struct scheduler *ops, struct vcpu *v)
{
    cpumask_t online_affinity;
    cpumask_t *online;

    online = cpupool_scheduler_cpumask(v->domain->cpupool);
    cpumask_and(&online_affinity, v->cpu_affinity, online);
    return cpumask_cycle(v->vcpu_id % cpumask_weight(&online_affinity) - 1,
                         &online_affinity);
}

/*
 * Handles the rescheduling & bookkeeping of domains running in their
 * guaranteed timeslice.
 */
static void desched_edf_dom(s_time_t now, struct vcpu* v)
{
    struct sedf_vcpu_info* inf = EDOM_INFO(v);

    /* Current domain is running in real time mode */
    ASSERT(__task_on_queue(v));

    /* Update the vcpu's cputime */
    /* TODO: Update cbs_current_budget here instead */
    inf->cputime += now - inf->sched_start_abs;

    /* Scheduling decisions which don't remove the running domain from the runq */
    /* TODO: Update comparison to cbs_current_budget instead of slice */
    if ( (inf->cputime < inf->slice) && sedf_runnable(v) )
        return;

    /* We have a blocked realtime task or its server budget has been consumed */
    __del_from_queue(v);

    /*
     * Manage bookkeeping (i.e. calculate next deadline, memorise
     * overrun-time of slice) of finished domains.
     */
    /* TODO: Change comparisons to cbs values instead */
    if ( inf->cputime >= inf->slice )
    {
        /* TODO: Update CBS parameters instead */
        inf->cputime -= inf->slice;

        /* Set next deadline */
        inf->deadl_abs += inf->period;
    }

    /* Add a runnable domain to the waitqueue */
    if ( sedf_runnable(v) )
    {
        __add_to_waitqueue_sort(v);
    }

    /*TODO: Move cbs_update functionality to this function? */
    cbs_update(v, now);

    ASSERT(EQ(sedf_runnable(v), __task_on_queue(v)));
    ASSERT(sedf_runnable(v));
}

/* TODO: Update function as code is refactored (remove/rename variables) */
static void cbs_update(struct sedf_vcpu_info *inf, s_time_t now)
{
    if(inf->cbs_current_budget <= 0)
    {
        if(inf->cbs_current_budget < 0){
            printk("CBS: Budget was exceeded by %"PRIu64" ns \n",
                    inf->cbs_current_budget * -1);
        }
        /* Update CBS parameters */
        inf->cbs_current_budget = inf->cbs_max_budget;
        //inf->cbs_current_deadline += inf->cbs_period;
        inf->cbs_current_deadline = now + inf->cbs_period;
        /* Update SEDF parameters */
        inf->deadl_abs = inf->cbs_current_deadline; /* Will likely be redundant when code is refactored  */
	}
}

/* Update all elements on the queues */
static void update_queues(
    s_time_t now, struct list_head *runq, struct list_head *waitq)
{
    struct list_head     *cur, *tmp;
    struct sedf_vcpu_info *curinf;

    /*
     * Check for the first elements of the waitqueue, whether their
     * next period has already started.
     */
    list_for_each_safe ( cur, tmp, waitq )
    {
        curinf = list_entry(cur, struct sedf_vcpu_info, list);
        if ( PERIOD_BEGIN(curinf) > now )
            break;
        __del_from_queue(curinf->vcpu);
        __add_to_runqueue_sort(curinf->vcpu);
    }

    /* Process the runq, find domains that are on the runq that shouldn't be*/
    list_for_each_safe ( cur, tmp, runq )
    {
        curinf = list_entry(cur,struct sedf_vcpu_info,list);
        
        /* TODO: Update to CBS variables */
        if ( unlikely(curinf->slice == 0) )
        {
            /* TODO: update to "ignore vcpu's with empty max_budget */
            /* Ignore domains with empty slice */
            __del_from_queue(curinf->vcpu);

            /* Move them to their next period */
            /* TODO: update to just CBS variables instead */
            curinf->deadl_abs += curinf->period;
            curinf->cbs_current_deadline += curinf->cbs_period;

            /* Ensure that the start of the next period is in the future */
            /* TODO: update to just CBS variables instead */
            if ( unlikely(PERIOD_BEGIN(curinf) < now) ){
                curinf->deadl_abs +=
                    (DIV_UP(now - PERIOD_BEGIN(curinf),
                            curinf->period)) * curinf->period;
            }

            /* Put them back into the queue */
            __add_to_waitqueue_sort(curinf->vcpu);
        }
        /* TODO: This also should never happen with CBS working,
         * except in the case of a parameter change (sedf_adjust)
         * 1) Decide how problem should be handled 
         * 2) update variables to CBS variables */
        else if ( unlikely((curinf->deadl_abs < now) ||
                           (curinf->cputime > curinf->slice)) )
        {
            /*
             * We missed the deadline or the slice was already finished.
             * Might happen because of dom_adj (sedf_adjust).
             */
            printk("\tDomain %i.%i exceeded it's deadline/"
                   "slice (%"PRIu64" / %"PRIu64") now: %"PRIu64
                   " cputime: %"PRIu64"\n",
                   curinf->vcpu->domain->domain_id,
                   curinf->vcpu->vcpu_id,
                   curinf->deadl_abs, curinf->slice, now,
                   curinf->cputime);
            __del_from_queue(curinf->vcpu);

            /* Common case: we miss one period */
            curinf->deadl_abs += curinf->period;

            /*
             * If we are still behind: modulo arithmetic, force deadline
             * to be in future and aligned to period borders.
             */
            if ( unlikely(curinf->deadl_abs < now) )
                curinf->deadl_abs +=
                    DIV_UP(now - curinf->deadl_abs,
                           curinf->period) * curinf->period;
            ASSERT(curinf->deadl_abs >= now);

            /* Give a fresh slice */
            curinf->cputime = 0; //Same as: cbs_current_budget = cbs_max_budget
            if ( PERIOD_BEGIN(curinf) > now )
                __add_to_waitqueue_sort(curinf->vcpu);
            else
                __add_to_runqueue_sort(curinf->vcpu);
        }
        else
            break;
    }
}

static int sedf_init(struct scheduler *ops)
{
    struct sedf_priv_info *prv;

    prv = xzalloc(struct sedf_priv_info);
    if ( prv == NULL )
        return -ENOMEM;

    ops->sched_data = prv;
    spin_lock_init(&prv->lock);

    return 0;
}

static void sedf_deinit(const struct scheduler *ops)
{
    struct sedf_priv_info *prv;

    prv = SEDF_PRIV(ops);
    if ( prv != NULL )
        xfree(prv);
}

#define MIN(x,y) (((x)<(y))?(x):(y))
/*
 * Main scheduling function
 * Reasons for calling this function are:
 * -timeslice for the current period used up
 * -domain on waitqueue has started it's period 
*/
static struct task_slice sedf_do_schedule(
    const struct scheduler *ops, s_time_t now, bool_t tasklet_work_scheduled)
{
    int                   cpu      = smp_processor_id();
    struct list_head     *runq     = RUNQ(cpu);
    struct list_head     *waitq    = WAITQ(cpu);
    struct sedf_vcpu_info *inf     = EDOM_INFO(current); /* TODO: find the origin of current */
    struct list_head      *extraq[] = {
        EXTRAQ(cpu, EXTRA_PEN_Q), EXTRAQ(cpu, EXTRA_UTIL_Q)};
    struct sedf_vcpu_info *runinf, *waitinf, *budgetinf;
    struct task_slice      ret;

    SCHED_STAT_CRANK(schedule);

    /*******CBS Related tasks *********
    * TODO UPDATE: This should not be needed here as it wll
    * be handled in the desched_edf function.
    *TODO: current_budget -= runinf->cputime (amount of time vcpu has run on the pcpu)*/
    /* Decrement CBS budget based on time vCPU has run*/
    budgetinf  = list_entry(runq->next,struct sedf_vcpu_info,list);
    budgetinf->cbs_current_budget -= (now - budgetinf->sched_start_abs);

    /* Idle tasks don't need any of the following stuff */
    if ( is_idle_vcpu(current) )
        goto check_waitq;

    /*
     * Create local state of the status of the domain, in order to avoid
     * inconsistent state during scheduling decisions, because data for
     * vcpu_runnable is not protected by the scheduling lock!
     */
    if ( !vcpu_runnable(current) )
        inf->status |= SEDF_ASLEEP;

    if ( inf->status & SEDF_ASLEEP )
        inf->block_abs = now;

    /* Remove 
    desched_edf_dom(now, current);
    
 check_waitq:
    update_queues(now, runq, waitq);

    /*
     * Now simply pick the first domain from the runqueue, which has the
     * earliest deadline, because the list is sorted
     *
     * Tasklet work (which runs in idle VCPU context) overrides all else.
     */

     /*TODO: Check if this tasklet work can be handled more efficiently*/
    if ( tasklet_work_scheduled ||
         (list_empty(runq) && list_empty(waitq)) ||
         unlikely(!cpumask_test_cpu(cpu,
                   cpupool_scheduler_cpumask(per_cpu(cpupool, cpu)))) )
    {
        ret.task = IDLETASK(cpu);
        ret.time = SECONDS(1);
    }
    /* RunQ is not empty  */
    else if ( !list_empty(runq) )
    {

        runinf   = list_entry(runq->next,struct sedf_vcpu_info,list);
        ret.task = runinf->vcpu;

        /* WaitQ is not empty */
        if ( !list_empty(waitq) )
        {
            waitinf  = list_entry(waitq->next,
                                  struct sedf_vcpu_info,list);
            /*
             * Rerun scheduler, when scheduled domain reaches it's
             * end of slice or the first domain from the waitqueue
             * gets ready.
             */
            /* TODO: runinf->slice - runinf->cputime = runinf->cbs_current_budget; */
            ret.time = MIN(now + runinf->slice - runinf->cputime,
                           PERIOD_BEGIN(waitinf)) - now;
        }
        /* WaitQ is empty */
        else
        {
            /* TODO: runinf->slice - runinf->cputime = runinf->cbs_current_budget;  */
            ret.time = runinf->slice - runinf->cputime;
        }
    }
    /* RunQ is empty  */
    else
    {
        /*TODO: Use BUG_ON or trace to see if do_extra_schedule happens when CBS is implemented,
         *      we think that it will not be used */ 
        //BUG_ON(1);
        waitinf  = list_entry(waitq->next,struct sedf_vcpu_info, list);
        /*
         * We could not find any suitable domain
         * => all domains must be idle or blocked
         */
    }

    /*
     * TODO: Do something USEFUL when this happens and find out, why it
     * still can happen!!!
     *
     * CBS TODO: Should not have this problem with CBS; Add BugOn to check
     *           because if we do have this problem it's much more serious
     */
    if ( ret.time < 0)
    {
        BUG_ON(1);
        printk("Ouch! We are seriously BEHIND schedule! %"PRIi64"\n",
               ret.time);
        ret.time = EXTRA_QUANTUM;
    }

    ret.migrated = 0;

    EDOM_INFO(ret.task)->sched_start_abs = now;
    CHECK(ret.time > 0);
    ASSERT(sedf_runnable(ret.task));
    CPU_INFO(cpu)->current_slice_expires = now + ret.time;
    return ret;
}

static void sedf_sleep(const struct scheduler *ops, struct vcpu *d)
{
    if ( is_idle_vcpu(d) )
        return;

    EDOM_INFO(d)->status |= SEDF_ASLEEP;

    if ( per_cpu(schedule_data, d->processor).curr == d )
    {
        cpu_raise_softirq(d->processor, SCHEDULE_SOFTIRQ);
    }
    else if ( __task_on_queue(d) )
            __del_from_queue(d);
    }
}

#define DOMAIN_EDF   1
#define DOMAIN_IDLE   4
static inline int get_run_type(struct vcpu* d)
{
    struct sedf_vcpu_info* inf = EDOM_INFO(d);
    if (is_idle_vcpu(d))
        return DOMAIN_IDLE;

    return DOMAIN_EDF;
}

/* Print a lot of useful information about a domains in the system */
static void sedf_dump_domain(struct vcpu *d)
{
    printk("%i.%i has=%c ", d->domain->domain_id, d->vcpu_id,
           d->is_running ? 'T':'F');
    printk("p=%"PRIu64" sl=%"PRIu64" ddl=%"PRIu64" w=%hu"
           " sc=%i xtr(%s)=%"PRIu64" ew=%hu",
           EDOM_INFO(d)->period, EDOM_INFO(d)->slice, EDOM_INFO(d)->deadl_abs,
           
#ifdef SEDF_STATS
    if ( EDOM_INFO(d)->block_time_tot != 0 )
        printk(" pen=%"PRIu64"%%", (EDOM_INFO(d)->penalty_time_tot * 100) /
               EDOM_INFO(d)->block_time_tot);
    if ( EDOM_INFO(d)->block_tot != 0 )
        printk("\n   blks=%u sh=%u (%u%%) (shex=%i "\
               "shexsl=%i) l=%u (%u%%) avg: b=%"PRIu64" p=%"PRIu64"",
               EDOM_INFO(d)->block_tot, EDOM_INFO(d)->short_block_tot,
               (EDOM_INFO(d)->short_block_tot * 100) / EDOM_INFO(d)->block_tot,
               EDOM_INFO(d)->pen_extra_blocks,
               EDOM_INFO(d)->pen_extra_slices,
               EDOM_INFO(d)->long_block_tot,
               (EDOM_INFO(d)->long_block_tot * 100) / EDOM_INFO(d)->block_tot,
               (EDOM_INFO(d)->block_time_tot) / EDOM_INFO(d)->block_tot,
               (EDOM_INFO(d)->penalty_time_tot) / EDOM_INFO(d)->block_tot);
#endif
    printk("\n");
}

/*
 * Compares two domains in the relation of whether the one is allowed to
 * interrupt the others execution.
 * It returns true (!=0) if a switch to the other domain is good.
 * Current Priority scheme is as follows:
 *  EDF > idle-domain
 */
static inline int should_switch(struct vcpu *cur,
                                struct vcpu *other,
                                s_time_t now)
{
    struct sedf_vcpu_info *cur_inf, *other_inf;
    cur_inf   = EDOM_INFO(cur);
    other_inf = EDOM_INFO(other);

    /* Check whether we need to make an earlier scheduling decision */
    if ( PERIOD_BEGIN(other_inf) <
         CPU_INFO(other->processor)->current_slice_expires )
        return 1;

    /* No timing-based switches need to be taken into account here */
    switch ( get_run_type(cur) )
    {
    case DOMAIN_EDF:
        /* Do not interrupt a running EDF domain */
        return 0;
    case DOMAIN_IDLE:
        return 1;
    }

    return 1;
}

/* This function wakes up a domain, i.e. moves them into the waitqueue
 * things to mention are: admission control is taking place nowhere at
 * the moment, so we can't be sure, whether it is safe to wake the domain
 * up at all. Anyway, even if it is safe (total cpu usage <=100%) there are
 * some considerations on when to allow the domain to wake up and have it's
 * first deadline...
 * I detected 3 cases, which could describe the possible behaviour of the scheduler,
 * and I'll try to make them more clear:
 *
 * 1. Very conservative
 *     -when a blocked domain unblocks, it is allowed to start execution at
 *      the beginning of the next complete period
 *      (D..deadline, R..running, B..blocking/sleeping, U..unblocking/waking up
 *
 *      DRRB_____D__U_____DRRRRR___D________ ... 
 *
 *     -this causes the domain to miss a period (and a deadlline)
 *     -doesn't disturb the schedule at all
 *     -deadlines keep occuring isochronous
 *
 * 2. Conservative Part 1
 *     -when a domain unblocks in the same period as it was blocked it unblocks and
 *      may consume the rest of it's original time-slice minus the time it was blocked
 *      (assume period=9, slice=5)
 *
 *      DRB_UR___DRRRRR___D...
 *
 *     -this also doesn't disturb scheduling, but might lead to the fact, that the domain
 *      can't finish it's workload in the period
 *
 *    Part 2a
 *     -it is obvious that such accounting of block time, applied when
 *      unblocking is happening in later periods, works fine aswell
 *     -the domain is treated as if it would have been running since the start
 *      of its new period
 *
 *      DRB______D___UR___D...
 *
 *    Part 2b
 *     -if one needs the full slice in the next period, it is necessary to
 *      treat the unblocking time as the start of the new period, i.e. move
 *      the deadline further back (later)
 *     -this doesn't disturb scheduling as well, because for EDF periods can
 *      be treated as minimal inter-release times and scheduling stays
 *      correct, when deadlines are kept relative to the time the process
 *      unblocks
 *
 *      DRB______D___URRRR___D...<prev [Thread] next>
 *                       (D) <- old deadline was here
 *     -problem: deadlines don't occur isochronous anymore
 *    
 *     Part 2c (Improved Atropos design)
 *     -when a domain unblocks it is given a very short period (=latency hint)
 *      and slice length scaled accordingly
 *     -both rise again to the original value (e.g. get doubled every period)
 *
 * 3. Unconservative (i.e. incorrect)
 *     -to boost the performance of I/O dependent domains it would be possible
 *      to put the domain into the runnable queue immediately, and let it run
 *      for the remainder of the slice of the current period
 *      (or even worse: allocate a new full slice for the domain)
 *     -either behaviour can lead to missed deadlines in other domains as
 *      opposed to approaches 1,2a,2b
 */
static void sedf_wake(const struct scheduler *ops, struct vcpu *d)
{
    s_time_t               now = NOW();
    struct sedf_vcpu_info* inf = EDOM_INFO(d);

    if ( unlikely(is_idle_vcpu(d)) )
        return;

    if ( unlikely(__task_on_queue(d)) )
        return;

    ASSERT(!sedf_runnable(d));
    inf->status &= ~SEDF_ASLEEP;

    /* TODO: Throw away extra deadline variable, update as needed */
    /* Moved from cbs_wake();	
    /* If current deadline cannot be used recalculate it */
	if(inf->cbs_current_budget >= (inf->cbs_current_deadline - now) *
                                      (inf->cbs_max_budget / inf->cbs_period)) {
		inf->cbs_current_deadline = now + inf->cbs_period;
        inf->deadl_abs = inf->cbs_current_deadline;
    }
	/* else we can keep the current deadline */

#ifdef SEDF_STATS
    inf->block_tot++;
#endif

    /* TODO: Update to use CBS variables instead.
     * Circumstance should be unlikely, report if it occurs */
    if ( PERIOD_BEGIN(inf) > now )
        __add_to_waitqueue_sort(d);
    else
        __add_to_runqueue_sort(d);

#ifdef SEDF_STATS
    /* Do some statistics here... */
    if ( inf->block_abs != 0 )
    {
        inf->block_time_tot += now - inf->block_abs;
        inf->penalty_time_tot +=
            PERIOD_BEGIN(inf) + inf->cputime - inf->block_abs;
    }
#endif

    /*
     * Check whether the awakened task needs to invoke the do_schedule
     * routine. Try to avoid unnecessary runs but:
     * Save approximation: Always switch to scheduler!
     */
    ASSERT(d->processor >= 0);
    ASSERT(d->processor < nr_cpu_ids);
    ASSERT(per_cpu(schedule_data, d->processor).curr);
    
    if ( should_switch(per_cpu(schedule_data, d->processor).curr, d, now) )
        cpu_raise_softirq(d->processor, SCHEDULE_SOFTIRQ);
}

/* Dumps all domains on the specified cpu */
static void sedf_dump_cpu_state(const struct scheduler *ops, int i)
{
    struct list_head      *list, *queue, *tmp;
    struct sedf_vcpu_info *d_inf;
    struct domain         *d;
    struct vcpu    *ed;
    int loop = 0;
    struct sedf_dom_info *d_inf;

    printk("now=%"PRIu64"\n",NOW());
    queue = RUNQ(i);
    printk("RUNQ rq %lx   n: %lx, p: %lx\n",  (unsigned long)queue,
           (unsigned long) queue->next, (unsigned long) queue->prev);
    list_for_each_safe ( list, tmp, queue )
    {
        printk("%3d: ",loop++);
        d_inf = list_entry(list, struct sedf_vcpu_info, list);
        sedf_dump_domain(d_inf->vcpu);
    }

    queue = WAITQ(i); loop = 0;
    printk("\nWAITQ rq %lx   n: %lx, p: %lx\n",  (unsigned long)queue,
           (unsigned long) queue->next, (unsigned long) queue->prev);
    list_for_each_safe ( list, tmp, queue )
    {
        printk("%3d: ",loop++);
        d_inf = list_entry(list, struct sedf_vcpu_info, list);
        sedf_dump_domain(d_inf->vcpu);
    }

    loop = 0;
    printk("\nnot on Q\n");

    rcu_read_lock(&domlist_read_lock);
    for_each_domain ( d )
    {
        if ( (d->cpupool ? d->cpupool->sched : &sched_sedf_def) != ops )
            continue;
        for_each_vcpu(d, ed)
        {
            if ( !__task_on_queue(ed) && (ed->processor == i) )
            {
                printk("%3d: ",loop++);
                sedf_dump_domain(ed);
            }
        }
    }
    rcu_read_unlock(&domlist_read_lock);
}

/* Set or fetch domain scheduling parameters */
static int sedf_adjust(const struct scheduler *ops, struct domain *p, struct xen_domctl_scheduler_op *op)
{
    struct sedf_priv_info *prv = SEDF_PRIV(ops);
    unsigned long flags;
    unsigned int nr_cpus = cpumask_last(&cpu_online_map) + 1;
    struct vcpu *v;
    int rc = 0;

    /*
     * Serialize against the pluggable scheduler lock to protect from
     * concurrent updates. We need to take the runq lock for the VCPUs
     * as well, since we are touching slice and
     * period. As in sched_credit2.c, runq locks nest inside the
     * pluggable scheduler lock.
     */
    spin_lock_irqsave(&prv->lock, flags);

    if ( op->cmd == XEN_DOMCTL_SCHEDOP_putinfo )
    {

/* TODO: verify that realtime schedulability is still fullfilled if the new domain would be inserted */
        /* Check for sane parameters */
        if ( !op->u.sedf.period )
        {
            rc = -EINVAL;
            goto out;
        }

        /*
         * Sanity checking parameters
         */
        if ( (op->u.sedf.period > PERIOD_MAX) ||
             (op->u.sedf.period < PERIOD_MIN) ||
             (op->u.sedf.slice  > op->u.sedf.period) ||
             (op->u.sedf.slice  < SLICE_MIN) )
        {
            rc = -EINVAL;
            goto out;
        }

        /* Time-driven domains */
        for_each_vcpu ( p, v )
        {
            spinlock_t *lock = vcpu_schedule_lock(v);

            EDOM_INFO(v)->period_orig =
                EDOM_INFO(v)->period  = op->u.sedf.period;
            EDOM_INFO(v)->slice_orig  =
                EDOM_INFO(v)->slice   = op->u.sedf.slice;
            vcpu_schedule_unlock(lock, v);
        }
    }
    else if ( op->cmd == XEN_DOMCTL_SCHEDOP_getinfo )
    {
        if ( p->vcpu[0] == NULL )
        {
            rc = -EINVAL;
            goto out;
        }

        op->u.sedf.period    = EDOM_INFO(p->vcpu[0])->period;
        op->u.sedf.slice     = EDOM_INFO(p->vcpu[0])->slice;
        /* TODO: Add CBS parameters */
    }

out:
    spin_unlock_irqrestore(&prv->lock, flags);

    return rc;
}

static struct sedf_priv_info _sedf_priv;

const struct scheduler sched_sedf_def = {
    .name           = "Simple EDF Scheduler",
    .opt_name       = "sedf",
    .sched_id       = XEN_SCHEDULER_SEDF,
    .sched_data     = &_sedf_priv,/*** NEW ***/

    .init_domain    = sedf_init_domain,/*** NEW ***/
    .destroy_domain = sedf_destroy_domain,/*** NEW ***/

    .insert_vcpu    = sedf_insert_vcpu,/*** NEW ***/
    .sleep          = sedf_sleep,
    .wake           = sedf_wake,

    .alloc_vdata    = sedf_alloc_vdata,/*** NEW, may be equiv to alloc_task***/
    .free_vdata     = sedf_free_vdata, /*** NEW ***/
    .alloc_pdata    = sedf_alloc_pdata,/*** NEW ***/
    .free_pdata     = sedf_free_pdata,/*** NEW ***/
    .alloc_domdata  = sedf_alloc_domdata,/*** NEW ***/
    .free_domdata   = sedf_free_domdata,/*** NEW ***/

    .init           = sedf_init,/*** NEW ***/
    .deinit         = sedf_deinit,/*** NEW ***/

    .do_schedule    = sedf_do_schedule,
    .pick_cpu       = sedf_pick_cpu,/*** NEW ***/
    .dump_cpu_state = sedf_dump_cpu_state,
    .adjust         = sedf_adjust, /*** NEW, may be equive to sedf_adjdom ***/
};

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
