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

#ifndef NDEBUG
#define SEDF_STATS
#define CHECK(_p)                                           \
    do {                                                    \
        if ( !(_p) )                                        \
            printk("Check '%s' failed, line %d, file %s\n", \
                   #_p , __LINE__, __FILE__);               \
    } while ( 0 )
#else
#define CHECK(_p) ((void)0)
#endif

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
    /* Parameters for CBS/EDF */
    s_time_t cbs_period;       /* Replaces: period */
    s_time_t cbs_deadline;     /* Replaces: deadl_abs */
    s_time_t cbs_max_budget;   /* Replaces: slice */

    /* Status of domain */
    int       status;

    /* Bookkeeping */
    s_time_t  sched_start_abs; /* Time current budget started */
    s_time_t  cputime;         /* ~= cbs_budget (inverted, i.e. accumulated time vs. consumed time */
    s_time_t  cbs_budget;      /* Analagous to cpu_time, consumed time vs. accumulated time */
    s_time_t  block_abs;

    /* Statistics */
#ifdef SEDF_STATS
    s_time_t  block_time_tot;
    s_time_t  penalty_time_tot;
    int       block_tot;
#endif
};

struct sedf_cpu_info {
    struct   list_head runnableq;
    struct   list_head waitq;
    s_time_t current_slice_expires;
};

#define SEDF_PRIV(_ops) \
    ((struct sedf_priv_info *)((_ops)->sched_data))
#define VCPU_INFO(v)   ((struct sedf_vcpu_info *)((v)->sched_priv))
#define CPU_INFO(cpu)  \
    ((struct sedf_cpu_info *)per_cpu(schedule_data, cpu).sched_priv)
#define LIST(v)        (&VCPU_INFO(v)->list)
#define RUNQ(cpu)      (&CPU_INFO(cpu)->runnableq)
#define WAITQ(cpu)     (&CPU_INFO(cpu)->waitq)
#define IDLETASK(cpu)  (idle_vcpu[cpu])

#define PERIOD_BEGIN(inf) ((inf)->cbs_deadline - (inf)->cbs_period)

#define DIV_UP(x,y) (((x) + (y) - 1) / y)

#define sedf_runnable(vcpu)  (!(VCPU_INFO(vcpu)->status & SEDF_ASLEEP))


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

static inline int __vcpu_on_queue(struct vcpu *v)
{
    printk("CBS: vcpu_on_queue, vpu=%d\n", v->vcpu_id);
    return (((LIST(v))->next != NULL) && (LIST(v)->next != LIST(v)));
}

static inline void __del_from_queue(struct vcpu *v)
{
    struct list_head *list = LIST(v);
    printk("CBS: del_from_queue, vcpu=%d\n", v->vcpu_id);
    ASSERT(__vcpu_on_queue(v));
    list_del(list);
    list->next = NULL;
    ASSERT(!__vcpu_on_queue(v));
}

typedef int(*list_comparer)(struct list_head* el1, struct list_head* el2);

static inline void list_insert_sort(
    struct list_head *list, struct list_head *element, list_comparer comp)
{
    struct list_head     *cur;
    printk("CBS: list_insert_sort\n");
    
    /* Iterate through all elements to find our "hole" */
    list_for_each( cur, list )
        if ( comp(element, cur) < 0 )
            break;

    /* cur now contains the element, before which we'll enqueue */
    list_add(element, cur->prev);
}

/********** Add to WaitQ ************
 * Adds a domain to the queue of processes which wait for the beginning of the
 * next period; this list is therefore sorted by this time, which is simply
 * absol. deadline - period.
 */
DOMAIN_COMPARER(waitq, list, PERIOD_BEGIN(d1), PERIOD_BEGIN(d2));

static inline void __add_to_waitqueue_sort(struct vcpu *v)
{
    printk("CBS: add_to_waitqueue, vcpu=%d\n", v->vcpu_id);
    ASSERT(!__vcpu_on_queue(v));
    list_insert_sort(WAITQ(v->processor), LIST(v), waitq_comp);
    ASSERT(__vcpu_on_queue(v));
}

/********** Add to RunQ *************
 * Adds a domain to the queue of processes which have started their current
 * period and are runnable (i.e. not blocked, dieing,...). The first element
 * on this list is running on the processor, if the list is empty the idle
 * task will run. As we are implementing EDF, this list is sorted by deadlines.
 */
DOMAIN_COMPARER(runq, list, d1->cbs_deadline, d2->cbs_deadline);

static inline void __add_to_runqueue_sort(struct vcpu *v)
{
    printk("CBS: add_to_runqueue_sort, vcpu=%d\n", v->vcpu_id);
    list_insert_sort(RUNQ(v->processor), LIST(v), runq_comp);
}

/* Required function to satisfy scheduler interface */
static void sedf_insert_vcpu(const struct scheduler *ops, struct vcpu *v)
{
    printk("CBS: sedf_insert_vcpu, vcpu=%d\n", v->vcpu_id);
    if (is_idle_vcpu(v))
    {
        VCPU_INFO(v)->cbs_deadline = 0;
        VCPU_INFO(v)->status &= ~SEDF_ASLEEP;
    }
}

static void *sedf_alloc_vdata(const struct scheduler *ops, struct vcpu *v, void *dd) /* dd = domain data */
{
    struct sedf_vcpu_info *inf;
    printk("CBS: sedf_alloc_vdata, vcpu=%d\n", v->vcpu_id);
   
    inf = xzalloc(struct sedf_vcpu_info);
    if ( inf == NULL )
        return NULL;

    inf->vcpu = v;
    
    /* TODO: Add invalid default CBS parameters so user is forced to set parameters */
    /* Default CBS Parameters */
    inf->cbs_period     = MILLISECS(10);
    inf->cbs_max_budget = MILLISECS(10);
    inf->cputime        = 0;
    inf->cbs_deadline   = 0;
    inf->cbs_budget     = inf->cbs_max_budget - inf->cputime;

    /* Every VCPU starts asleep */
    inf->status         = SEDF_ASLEEP;

    INIT_LIST_HEAD(&(inf->list));

    SCHED_STAT_CRANK(vcpu_init);

    return inf;
}

static void *
sedf_alloc_pdata(const struct scheduler *ops, int cpu)
{
    struct sedf_cpu_info *spc;
    printk("CBS: sedf_alloc_pdata, cpu=%d\n", cpu);
    
    spc = xzalloc(struct sedf_cpu_info);
    BUG_ON(spc == NULL);
    INIT_LIST_HEAD(&spc->waitq);
    INIT_LIST_HEAD(&spc->runnableq);

    return (void *)spc;
}

static void
sedf_free_pdata(const struct scheduler *ops, void *spc, int cpu)
{
    printk("CBS: sedf_free_pdata, cpu=%d\n", cpu);
    if ( spc == NULL )
        return;

    xfree(spc);
}

static void sedf_free_vdata(const struct scheduler *ops, void *priv)
{
    printk("CBS: sedf_free_vdata\n");
    xfree(priv);
}

static void *
sedf_alloc_domdata(const struct scheduler *ops, struct domain *d)
{
    printk("CBS: sedf_alloc_domdata, dom=%d\n", d->domain_id);
    return xzalloc(struct sedf_dom_info);
}

static int sedf_init_domain(const struct scheduler *ops, struct domain *d)
{
    printk("CBS: sedf_init_domain, dom=%d\n", d->domain_id);
    d->sched_priv = sedf_alloc_domdata(ops, d);
    if ( d->sched_priv == NULL )
        return -ENOMEM;

    return 0;
}

static void sedf_free_domdata(const struct scheduler *ops, void *data)
{
    printk("CBS: sedf_free_domdata\n");
    xfree(data);
}

static void sedf_destroy_domain(const struct scheduler *ops, struct domain *d)
{
    printk("CBS: sedf_destroy_domain, dom=%d\n", d->domain_id);
    sedf_free_domdata(ops, d->sched_priv);
}


/*
** CBS TODO: Will likely need to use this for multiprocessor support
*/
static int sedf_pick_cpu(const struct scheduler *ops, struct vcpu *v)
{
    cpumask_t online_affinity;
    cpumask_t *online;

    printk("CBS: sedf_pick_cpu, vcpu=%d\n", v->vcpu_id);
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
    struct sedf_vcpu_info* inf = VCPU_INFO(v);
    
    printk("CBS: desched_edf_dom, vcpu=%d\n", v->vcpu_id);
    
    /* Assure incoming vcpu is coming from a queue */
    ASSERT(__vcpu_on_queue(v));

    /* Update cputime for the vcpu */
    inf->cputime += now - inf->sched_start_abs;

    /* Update cbs_budget for the vcpu*/
    if(inf->cbs_max_budget >= inf->cputime){
        inf->cbs_budget = inf->cbs_max_budget - inf->cputime;
    }
    else {
        inf->cbs_budget = 0;
        /* TODO: Note: The original SEDF allowed a minimum slice time of 5us,
         * therefore it us our assumption that the latency (the time between
         * when it was "supposed" to stop running, i.e. the time when the 
         * budget would be consumed, and when it actually stopped and called
         * the do_schedule function) of the vcpu should be < 5us */
        /* Print warning if budget was overrun by more than 5us */
        printk("CBS: Budget was exceeded by %"PRIu64" ns \n",
              (inf->cputime - inf->cbs_max_budget));
    }
    
    /* Scheduling decisions which don't remove the running vcpu from the runq */
    /* TODO: Update comparison to cbs_budget instead of slice */
    /* Ensures that at least 5us are available in budget before using it,
     * this should avoid overruns due to latency */
    if ((inf->cbs_budget > MICROSECS(5)) && sedf_runnable(v)){
        printk("CBS: desched_edf_dom-internal: %"PRIu64"ns still remain in budget, returning... \n", inf->cbs_budget); 
        return;
    }
       
    /* If the above check didn't return it means we either have a blocked vcpu
     * (not runnable) or its server budget has been consumed */
    __del_from_queue(v);

    /*
     * Manage bookkeeping (i.e. calculate next deadline, replenish
     * CBS budget) of finished domains.
     */
    /* TODO: Change comparisons to cbs values instead */
    /* TODO: Move cbs_update functionality to this function? */ // Yes
    /* Update server parameters as appropriate */    

    /* Replensish budget */ 
    inf->cputime = 0;
    inf->cbs_budget = inf->cbs_max_budget;
    /* Update server deadline */
    inf->cbs_deadline = now + inf->cbs_period;
    
    /* TODO: Under a CBS paradigm, we should add to runq if runnable */
    /* Add a runnable domain to the waitqueue */
    if (sedf_runnable(v))
    {
        printk("CBS: desched_edf_dom-internal, vcpu was runnable!\n");
        __add_to_runqueue_sort(v);
    }
    else
    {
        printk("CBS: desched_edf_dom-internal, vcpu was NOT runnable!\n");
        __add_to_waitqueue_sort(v);
    }

    ASSERT(__vcpu_on_queue(v));
    
    printk("\t\t cbs_period=%"PRIu64"\n", inf->cbs_period);
    printk("\t\t cbs_deadline=%"PRIu64"\n", inf->cbs_deadline);
    printk("\t\t cbs_max_budget=%"PRIu64"\n", inf->cbs_max_budget);
    printk("\t\t cbs_budget=%"PRIu64"\n", inf->cbs_budget);
    printk("\t\t period=%"PRIu64"\n", inf->cbs_period);
    printk("\t\t status=%d\n", inf->status);
    printk("\t\t sched_start_abs=%"PRIu64"\n", inf->sched_start_abs);
    printk("\t\t cputime=%"PRIu64"\n", inf->cputime);
}

/* Update all elements on the queues */
static void update_queues(
    s_time_t now, struct list_head *runq, struct list_head *waitq)
{
    struct list_head     *cur, *tmp;
    struct sedf_vcpu_info *curinf;

    printk("CBS: update_queues, now=%"PRIu64"\n", now);

    /* TODO: Verify waitq is processed with proper logic for CBS */
    list_for_each_safe ( cur, tmp, waitq )
    {
        curinf = list_entry(cur, struct sedf_vcpu_info, list);
        /* Check if vcpu is runnable, if it is, set as awake */
        if (vcpu_runnable(curinf->vcpu)){
            printk("CBS: update_queues-internal, vcpu IS runnable, set as awake\n");
            curinf->status &= ~SEDF_ASLEEP; //Set status as "awake"
            if(curinf->cbs_budget >= (curinf->cbs_deadline - now) *
                                      (curinf->cbs_max_budget / curinf->cbs_period)) {
		        curinf->cbs_deadline = now + curinf->cbs_period;
            }
        }
        else
            printk("CBS: update_queues-internal, vcpu NOT runnable, leave as asleep\n");
            
        /* If vcpu is asleep, keep on wait queue */
        if(curinf->status & SEDF_ASLEEP)
        {
            printk("CBS: update_queues-internal, vcpu was asleep\n");
            continue;
        }
        /* Else, vcpu is awake, move to run queue */
        __del_from_queue(curinf->vcpu);
        __add_to_runqueue_sort(curinf->vcpu);
    }

    /* Process the runq, find domains that are on the runq that shouldn't be*/
    list_for_each_safe ( cur, tmp, runq )
    {
        curinf = list_entry(cur,struct sedf_vcpu_info,list);

        /* TODO: Monitor if this happens, it shouldn't */
        if (unlikely(curinf->cbs_max_budget == 0))
        {
            printk("CBS: update_queues internal, cbs_max_buddget was == 0!\n");
            /* Ignore vcpu with empty max budget */
            __del_from_queue(curinf->vcpu);

            /* Move them to their next period */
            curinf->cbs_deadline += curinf->cbs_period;

            /* Ensure that the start of the next period is in the future */
            if (unlikely(PERIOD_BEGIN(curinf) < now)){
                curinf->cbs_deadline +=
                    (DIV_UP(now - PERIOD_BEGIN(curinf),
                            curinf->cbs_period)) * curinf->cbs_period;
                curinf->cbs_deadline +=
                    (DIV_UP(now - PERIOD_BEGIN(curinf),
                            curinf->cbs_period)) * curinf->cbs_period;
            }

            /* Put vcpu back into the queue */
            __add_to_waitqueue_sort(curinf->vcpu);
        }
        
        /* TODO: This also should never happen with CBS working,
         * except in the case of a parameter change (sedf_adjust)
         * 1) Decide how problem should be handled
         * 2) update variables to CBS variables */
        else if (unlikely((curinf->cbs_deadline < now) ||
                           (curinf->cputime > curinf->cbs_max_budget)) )
        {
            /*
             * We missed the deadline or the slice was already finished.
             * Might happen because of dom_adj (sedf_adjust).
             */
            printk("\tDomain %i on vcpu %i exceeded it's deadline/"
                   "max budget (%"PRIu64" / %"PRIu64") now: %"PRIu64
                   " cputime: %"PRIu64"\n",
                   curinf->vcpu->domain->domain_id,
                   curinf->vcpu->vcpu_id,
                   curinf->cbs_deadline, curinf->cbs_max_budget, now,
                   curinf->cputime);
            __del_from_queue(curinf->vcpu);

            /* Common case: we miss one period */
            curinf->cbs_deadline += curinf->cbs_period;

            /*
             * If we are still behind: modulo arithmetic, force deadline
             * to be in future and aligned to period borders.
             */
            if ( unlikely(curinf->cbs_deadline < now) )
                curinf->cbs_deadline +=
                    DIV_UP(now - curinf->cbs_deadline,
                           curinf->cbs_period) * curinf->cbs_period;
            ASSERT(curinf->cbs_deadline >= now);

            /* Give a fresh slice */
            curinf->cputime = 0; //Same as: cbs_budget = cbs_max_budget
            curinf->cbs_budget = curinf->cbs_max_budget;
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

    printk("CBS: sedf_init\n");
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

    printk("CBS: sedf_deinit\n");
    prv = SEDF_PRIV(ops);
    if ( prv != NULL )
        xfree(prv);
}

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
    struct list_head      *runq     = RUNQ(cpu);
    struct list_head      *waitq    = WAITQ(cpu);
    struct sedf_vcpu_info *inf     = VCPU_INFO(current); /* TODO: find the origin of current */
    struct sedf_vcpu_info *runinf;
    struct task_slice      ret;

    printk("CBS: sedf_do_schedule\n");
    printk("\t\t cpu=%d\n", cpu);
    printk("\t\t \"current\" vcpu=%d\n", inf->vcpu->vcpu_id);
    printk("\t\t now=%"PRIu64"\n", now);
    printk("\t\t cbs_period=%"PRIu64"\n", inf->cbs_period);
    printk("\t\t cbs_deadline=%"PRIu64"\n", inf->cbs_deadline);
    printk("\t\t cbs_max_budget=%"PRIu64"\n", inf->cbs_max_budget);
    printk("\t\t cbs_budget=%"PRIu64"\n", inf->cbs_budget);
    printk("\t\t status=%d\n", inf->status);
    printk("\t\t sched_start_abs=%"PRIu64"\n", inf->sched_start_abs);
    printk("\t\t cputime=%"PRIu64"\n", inf->cputime);
    SCHED_STAT_CRANK(schedule);

    /* Idle tasks don't need any of the following stuff */
    if (is_idle_vcpu(current)){
        printk("CBS: sedf_do_schedule-internal, domain was idle, taking goto\n");
        goto do_updateq;
    }

    /*
     * Create local state of the status of the domain, in order to avoid
     * inconsistent state during scheduling decisions, because data for
     * vcpu_runnable is not protected by the scheduling lock!
     */
    if ( !vcpu_runnable(current) ){
        printk("CBS: sedf_do_schedule-internal, vcpu was NOT runnable, set as asleep\n");
        inf->status |= SEDF_ASLEEP; //Set status as "Asleep"
    }

    if ( inf->status & SEDF_ASLEEP )
        inf->block_abs = now;

    /* Update CBS parameters (budget and deadlines) and remove vcpus from run
     * queue vcpus that are blocking or have consumed their server budget for
     * this period */
    desched_edf_dom(now, current);

 do_updateq:
    
    update_queues(now, runq, waitq);

    /*
     * Now simply pick the first domain from the runqueue, which has the
     * earliest deadline, because the list is sorted
     *
     * Tasklet work (which runs in idle VCPU context) overrides all else.
     */

     /* TODO: Check if this tasklet work can be handled more efficiently */
     /* TODO: Verify - This should schedule idle task if runq is empty.
      * Removed check for empty waitq, we only care if runq is empty.  Waitq
      * should trigger an interrupt/wake when a domain is no longer blocking
      * and will then get moved to the runq anyway. */
    if ( tasklet_work_scheduled || list_empty(runq) ||
         unlikely(!cpumask_test_cpu(cpu,
                   cpupool_scheduler_cpumask(per_cpu(cpupool, cpu)))) )
    {
        printk("CBS: Tasklet work was scheduled!\n");
        ret.task = IDLETASK(cpu);
        ret.time = MILLISECS(1);
    }
    /* RunQ is not empty and tasklet work was no needed */
    else
    {
        runinf   = list_entry(runq->next,struct sedf_vcpu_info,list);
        ret.task = runinf->vcpu;

        /* TODO: Note: runinf->cbs_max_budget - runinf->cputime = runinf->cbs_budget;  */
        ret.time = runinf->cbs_budget;
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
        printk("Ouch! We are seriously BEHIND schedule! %"PRIi64"\n",
               ret.time);
        runinf   = list_entry(runq->next,struct sedf_vcpu_info,list);
        ret.task = runinf->vcpu;
        ret.time = MICROSECS(500);
    }

    ret.migrated = 0;

    VCPU_INFO(ret.task)->sched_start_abs = now;
    CHECK(ret.time > 0);
    ASSERT(sedf_runnable(ret.task)); //***** DIES HERE
    CPU_INFO(cpu)->current_slice_expires = now + ret.time;
    return ret;
}

static void sedf_sleep(const struct scheduler *ops, struct vcpu *v)
{
    printk("CBS: sedf_sleep, vcpu=%d\n", v->vcpu_id);
    if ( is_idle_vcpu(v) )
        return;

    VCPU_INFO(v)->status |= SEDF_ASLEEP;

    if ( per_cpu(schedule_data, v->processor).curr == v )
    {
        cpu_raise_softirq(v->processor, SCHEDULE_SOFTIRQ);
    }
    else if ( __vcpu_on_queue(v) )
            __del_from_queue(v);
}

/* Print a lot of useful information about a domains in the system */
static void sedf_dump_domain(struct vcpu *v)
{
    printk("CBS: sedf_dump_domain, vcpu=%d\n", v->vcpu_id);
    printk("%i.%i has=%c ", v->domain->domain_id, v->vcpu_id,
           v->is_running ? 'T':'F');
    printk("p=%"PRIu64" sl=%"PRIu64" ddl=%"PRIu64"",
           VCPU_INFO(v)->cbs_period, VCPU_INFO(v)->cbs_max_budget, VCPU_INFO(v)->cbs_deadline);

#ifdef SEDF_STATS
    if ( VCPU_INFO(v)->block_time_tot != 0 )
        printk(" pen=%"PRIu64"%%", (VCPU_INFO(v)->penalty_time_tot * 100) /
               VCPU_INFO(v)->block_time_tot);
#endif
    printk("\n");
}

/*
 * Compares two vcpu's in the relation of whether the one is allowed to
 * interrupt the others execution.
 * It returns true (!=0) if a switch to the other vcpu is good,
 * i.e. current vcpu is idle
 */
static inline int should_switch(struct vcpu *cur,
                                struct vcpu *other,
                                s_time_t now)
{
    struct sedf_vcpu_info *cur_inf, *other_inf;
    cur_inf   = VCPU_INFO(cur);
    other_inf = VCPU_INFO(other);
    
    printk("CBS: should_switch, cur=%d, other=%d, now=%"PRIu64"\n", cur->vcpu_id, other->vcpu_id, now);

    /* Check whether we need to make an earlier scheduling decision */
    /* TODO: Verify -  if this decision is relevant in a CBS paradigm 
    if ( PERIOD_BEGIN(other_inf) <
         CPU_INFO(other->processor)->current_slice_expires )
        return 1;
    */

    /* No timing-based switches need to be taken into account here */
    if (is_idle_vcpu(cur)) {
        /* Vcpu is idle, ok to switch */
        printk("CBS: should_switch - yes\n");
        return 1;
    }
    else {
        /* vcpu is NOT idle, do not switch */
        printk("CBS: should_switch - no\n");
        return 0;
    }
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
static void sedf_wake(const struct scheduler *ops, struct vcpu *v)
{
    s_time_t               now = NOW();
    struct sedf_vcpu_info* inf = VCPU_INFO(v);

    printk("CBS: sedf_wake, vcpu=%d\n", v->vcpu_id);
    
    if ( unlikely(is_idle_vcpu(v)) )
        return;

    if ( unlikely(__vcpu_on_queue(v)) )
        return;

    ASSERT(!sedf_runnable(v));
    inf->status &= ~SEDF_ASLEEP;  //Set status as awake (Not asleep)
    
    /* TODO: Removal of this function was likely causing integer wrap-around
     * in the if statement below so cbs_deadline was not being initialized */
    if ( unlikely(inf->cbs_deadline == 0) )
    {
        /* Initial setup of the deadline */
        inf->cbs_deadline = now + inf->cbs_period;
    }
    
#ifdef SEDF_STATS
    inf->block_tot++;
#endif

    /* TODO: Verify the accuracy of this if statement and its location */
    /* Recalculate deadline if using it would disrupt other vcpu's deadlines */
	if(inf->cbs_budget >= (inf->cbs_deadline - now) *
                                      (inf->cbs_max_budget / inf->cbs_period)) {
		inf->cbs_deadline = now + inf->cbs_period;
    }
	/* Else we can keep the current deadline */
	
    /* TODO: Verify - In CBS vcpu should only be on waitq if it's blocking, 
     * i.e. not runnable (which was already determined false by) 
     * therefore we should always add a waking vcpu to the runq. The vcpu waking
     * up should be coming from the waitq except at startup
     */
    __add_to_runqueue_sort(v);

/* TODO: These statistics are likely no longer relevant with a CBS */
#ifdef SEDF_STATS
    /* Do some statistics here... */
    if ( inf->block_abs != 0 )
    {
        inf->block_time_tot += now - inf->block_abs;
        inf->penalty_time_tot +=
            PERIOD_BEGIN(inf) + inf->cputime - inf->block_abs;
    }
#endif

     /* Check whether the awakened task needs to invoke the do_schedule
      * routine. Try to avoid unnecessary runs but:
      * Safe approximation: Always switch to scheduler! */
    ASSERT(v->processor >= 0);
    ASSERT(v->processor < nr_cpu_ids);
    ASSERT(per_cpu(schedule_data, v->processor).curr);

    /* TODO: Check if this relevant anymore with the CBS, I don't think it is */
    if (should_switch(per_cpu(schedule_data, v->processor).curr, v, now))
        cpu_raise_softirq(v->processor, SCHEDULE_SOFTIRQ);
}

/* Dumps all domains on the specified cpu */
static void sedf_dump_cpu_state(const struct scheduler *ops, int i)
{
    struct list_head      *list, *queue, *tmp;
    struct sedf_vcpu_info *v_inf;
    struct domain         *d;
    struct vcpu    *v;
    int loop = 0;
 
    printk("CBS: sedf_dump_cpu_state, cpu=%d\n", i);
    printk("now=%"PRIu64"\n",NOW());
    queue = RUNQ(i);
    printk("RUNQ rq %lx   n: %lx, p: %lx\n",  (unsigned long)queue,
           (unsigned long) queue->next, (unsigned long) queue->prev);
    list_for_each_safe ( list, tmp, queue )
    {
        printk("%3d: ",loop++);
        v_inf = list_entry(list, struct sedf_vcpu_info, list);
        sedf_dump_domain(v_inf->vcpu);
    }
 
    queue = WAITQ(i); loop = 0;
    printk("\nWAITQ rq %lx   n: %lx, p: %lx\n",  (unsigned long)queue,
           (unsigned long) queue->next, (unsigned long) queue->prev);
    list_for_each_safe ( list, tmp, queue )
    {
        printk("%3d: ",loop++);
        v_inf = list_entry(list, struct sedf_vcpu_info, list);
        sedf_dump_domain(v_inf->vcpu);
    }

    loop = 0;
    printk("\nnot on Q\n");

    rcu_read_lock(&domlist_read_lock);
    for_each_domain ( d )
    {
        if ( (d->cpupool ? d->cpupool->sched : &sched_sedf_def) != ops )
            continue;
        for_each_vcpu(d, v)
        {
            if ( !__vcpu_on_queue(v) && (v->processor == i) )
            {
                printk("%3d: ",loop++);
                sedf_dump_domain(v);
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
    /* TODO: Variable  containing number of CPU's, was used for weight version
     * of the SEDF scheduler, not used at this point, but may want it at a
     * later date for other CBS purposes 
    unsigned int nr_cpus = cpumask_last(&cpu_online_map) + 1; */
    struct vcpu *v;
    int rc = 0;

    printk("CBS: sedf_adjust\n");
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
        if ( !op->u.sedf.cbs_period )
        {
            rc = -EINVAL;
            goto out;
        }

        /*
         * Sanity checking parameters
         */
        /* TODO: Add CBS parameters */
        if ( (op->u.sedf.cbs_period > PERIOD_MAX) ||
             (op->u.sedf.cbs_period < PERIOD_MIN) ||
             (op->u.sedf.cbs_max_budget  > op->u.sedf.cbs_period) ||
             (op->u.sedf.cbs_max_budget  < SLICE_MIN) )
        {
            rc = -EINVAL;
            goto out;
        }

        /* Time-driven domains */
        for_each_vcpu ( p, v )
        {
            spinlock_t *lock = vcpu_schedule_lock(v);
            /* TODO: Add CBS parameters */
            VCPU_INFO(v)->cbs_period  = op->u.sedf.cbs_period;
            VCPU_INFO(v)->cbs_max_budget   = op->u.sedf.cbs_max_budget;

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

        op->u.sedf.cbs_period    = VCPU_INFO(p->vcpu[0])->cbs_period;
        op->u.sedf.cbs_max_budget     = VCPU_INFO(p->vcpu[0])->cbs_max_budget;
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
    .sched_data     = &_sedf_priv,

    .init_domain    = sedf_init_domain,
    .destroy_domain = sedf_destroy_domain,

    .insert_vcpu    = sedf_insert_vcpu,
    .sleep          = sedf_sleep,
    .wake           = sedf_wake,

    .alloc_vdata    = sedf_alloc_vdata,
    .free_vdata     = sedf_free_vdata, 
    .alloc_pdata    = sedf_alloc_pdata,
    .free_pdata     = sedf_free_pdata,
    .alloc_domdata  = sedf_alloc_domdata,
    .free_domdata   = sedf_free_domdata,

    .init           = sedf_init,
    .deinit         = sedf_deinit,

    .do_schedule    = sedf_do_schedule,
    .pick_cpu       = sedf_pick_cpu,
    .dump_cpu_state = sedf_dump_cpu_state,
    .adjust         = sedf_adjust,
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
