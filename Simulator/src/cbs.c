#include "cbs.h"

/**
 * Assigns the given job as the currently active job for the CBS
 * @param server
 * @param job
 */
void AssignJob(struct CBS* server, struct Job* job);

void AddJob(struct CBS* server, struct Job* job)
{
	printf("[new job] vCPU: %s | time: %d | work: %d \n", server->cpu->name, job->arrivalTime, job->requiredProcessingTime);

	if(server->state == IDLE)
	{
		if(server->currentBudget >= (server->deadline - job->arrivalTime) * server->serverBandwith)
		{
			printf("[new deadline] old: %d | new: %d\n", server->deadline, job->arrivalTime + server->period);

			server->deadline = job->arrivalTime + server->period;
			server->currentBudget = server->requestedBudget;
		}

		AssignJob(server, job);

		printf("[state change] vCPU: %s | to: ACTIVE\n", server->cpu->name);
	}
	else
	{
		vector_add(server->jobs, job, sizeof(struct Job));
	}
}

bool DoWork(struct CBS* server)
{
	ASSERT(server->state != IDLE);
	bool jobFinished = false;

	server->currentJob.workTime -= 1;
	server->currentBudget -= 1;

	if(server->currentJob.workTime == 0)
	{
		if(server->jobs->length > 0)
		{
			 AssignJob(server, vector_get(server->jobs, 0));
			 vector_remove(server->jobs, 0);
		}
		else
		{
			server->state = IDLE;

			printf("[state change] vCPU: %s | to: IDLE\n", server->cpu->name);
		}

		jobFinished = true;
	}

	if(server->currentBudget == 0)
	{
		printf("[new budget] vCpu: %s | amount: %d | new deadline: %d\n", server->cpu->name, server->requestedBudget, server->deadline + server->period);

		server->currentBudget = server->requestedBudget;
		server->deadline += server->period;
		server->currentJob.deadline = server->deadline;
	}

	return jobFinished;
}

void AssignJob(struct CBS* server, struct Job* job)
{
	ASSERT(server->currentJob.workTime == 0);

	printf("[new front job] time: %d | deadline: %d\n", job->requiredProcessingTime, server->deadline);

	server->currentJob.workTime = job->requiredProcessingTime;
	server->currentJob.deadline = server->deadline;
	server->state = ACTIVE;
}
