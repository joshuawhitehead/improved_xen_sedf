#include <string.h>
#include <stdlib.h>

#include "vector.h"
#include "helper.h"
#include "other.h"
#include "cbs.h"
#include "edf.h"

struct SimulationEvent
{
	struct vCPU* cpu;
	struct Job job;
};

/**
 * Starts the simulation
 * @param queue
 * @param events An array of SimulationEvents that are used define the course of the simulation.
 * 				 It is not required to provide them in ascending order they are sorted at the start of the simulation.
 * @param count Number of elements in the events array.
 */
void StartSimulation(struct EDFRunQueue* queue, struct SimulationEvent events[], size_t count);

/**
 * Method for qsort() for sorting SimulationEvents based on the arrival time of the jobs.
 * @param elem1
 * @param elem2
 * @return
 */
int AscendingArrivalTime(const void* elem1, const void* elem2);

int main(void)
{
	struct pCPU pcpu = {100};
	struct EDFRunQueue queue = {&pcpu, pcpu.capacity, create_vector()};

	struct vCPU vcpu1 = {"one"};
	struct vCPU vcpu2 = {"two"};

	struct SimulationEvent events[] = {{&vcpu1, {50, 5}}, {&vcpu2, {10, 5}}, {&vcpu1, {20, 15}}, {&vcpu2, {40, 15}}};

	RegisterVCPU(&queue, &vcpu1, 30, 10);
	RegisterVCPU(&queue, &vcpu2, 30, 5);

	StartSimulation(&queue, events, 4);

	for(size_t i = 0; i < queue.servers->length; i++)
	{
		destroy_vector(((struct CBS*)vector_get(queue.servers, i))->jobs);
	}

	destroy_vector(queue.servers);

	return EXIT_SUCCESS;
}

void StartSimulation(struct EDFRunQueue* queue, struct SimulationEvent events[], size_t count)
{
	int currentTime = 0;
	size_t i = 0;

	qsort(events, count, sizeof(struct SimulationEvent), AscendingArrivalTime);

	while(true)
	{
		for(struct SimulationEvent* event = &events[i]; events[i].job.arrivalTime <= currentTime && i < count; i++, event = &events[i])
		{
			for(size_t j = 0; j < queue->servers->length; j++)
			{
				struct CBS* server = vector_get(queue->servers, j);

				if(memcmp(server->cpu, event->cpu, sizeof(struct vCPU)))
				{
					AddJob(server, &(event->job));
				}
			}
		}

		struct CBS* activeServer = GetServerWithED(queue);

		if(activeServer == NULL)
		{
			printf("[idle] until: %d\n", events[i].job.arrivalTime);

			currentTime = events[i].job.arrivalTime;
			continue;
		}

		printf("[processing] vCPU: %s | remaining work: %d | remaining budget: %d\n", activeServer->cpu->name, activeServer->currentJob.workTime, activeServer->currentBudget);

		DoWork(activeServer);

		currentTime++;

		// All events are processed now we need to wait for all jobs to be completed
		if(i >= count)
		{
			bool finished = true;

			for(size_t j = 0; j < queue->servers->length; j++)
			{
				if(((struct CBS*)vector_get(queue->servers, j))->state == ACTIVE)
				{
					finished = false;
					break;
				}
			}

			if(finished)
			{
				break;
			}
		}
	}
}

int AscendingArrivalTime(const void* elem1, const void* elem2)
{
	const struct SimulationEvent* first = (const struct SimulationEvent*)elem1;
	const struct SimulationEvent* second = (const struct SimulationEvent*)elem2;

	return first->job.arrivalTime - second->job.arrivalTime;
}
