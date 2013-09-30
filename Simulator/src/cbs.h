#ifndef CBS_H_
#define CBS_H_

#include "vector.h"
#include "other.h"
#include "helper.h"

/**
 * Enum containing the possible states a CBS can have
 */
enum CBSState
{
	IDLE, //!< IDLE The server has currently no job to serve
	ACTIVE//!< ACTIVE The server has at least one job to serve
};

struct CBS
{
	enum CBSState state;
	struct vCPU* cpu;
	struct EDFJob currentJob;
	int requestedBudget;
	int currentBudget;
	int period;
	int deadline;
	double serverBandwith;
	vector_p jobs;
};

/**
 * Adds the given job to the CBS for processing
 * @param server
 * @param job
 */
void AddJob(struct CBS* server, struct Job* job);

/**
 * Do one piece of work for the currently active job of the CBS
 * May only be called when the CBS is not IDLE
 * @param server
 */
void DoWork(struct CBS* server);

#endif /* CBS_H_ */
