#include "edf.h"

struct CBS* GetServerWithED(struct EDFRunQueue* server)
{
	struct CBS* activeRequest = NULL;

	for(size_t i = 0; i < server->servers->length; i++)
	{
		struct CBS* current = vector_get(server->servers, i);

		if(current->state != IDLE && (activeRequest == NULL || activeRequest->deadline > current->deadline))
		{
			activeRequest = current;
		}
	}

	return activeRequest;
}

bool RegisterVCPU(struct EDFRunQueue* queue, struct vCPU* vcpu, int requestedBudget, int period)
{
	if(requestedBudget > queue->availableCapacity)
		return false;

	printf("[vCPU registered] vCPU: %s | requested budget: %d | period: %d\n", vcpu->name, requestedBudget, period);

	struct CBS client;
	client.cpu = vcpu;
	client.requestedBudget = requestedBudget;
	client.period = period;
	client.deadline = 0;
	client.currentBudget = requestedBudget;
	client.serverBandwith = requestedBudget / (double)period;
	client.currentJob.workTime = 0;
	client.state = IDLE;
	client.jobs = create_vector();

	queue->availableCapacity -= client.requestedBudget;

	vector_add(queue->servers, &client, sizeof(client));

	return true;
}
