#include "parser.h"

#include <stdio.h>
#include <stdlib.h>

struct ParserResult parseFile(const char* path)
{
	struct ParserResult result;

	FILE* file = fopen(path, "r");

	{ //pCPU parsing
		fscanf(file, "%d", &result.cpu.capacity);
	}

	{ //vCPU parsing
		fscanf(file, "%d", &result.virtualCpuCount);

		result.virtualCpus = malloc(sizeof(struct vCPU) * result.virtualCpuCount);
		for(int i = 0; i < result.virtualCpuCount; ++i)
		{
			result.virtualCpus[i].name = malloc(sizeof(char) * 10);
			fscanf(file, "%s %d %d",
					result.virtualCpus[i].name,
					&result.virtualCpus[i].requestedBudget,
					&result.virtualCpus[i].period);
		}
	}

	{ //Event parsing
		fscanf(file, "%d", &result.eventCount);

		result.events = malloc(sizeof(struct SimulationEvent) * result.eventCount);
		for(int i = 0; i < result.eventCount; ++i)
		{
			result.events[i].vCpuName = malloc(sizeof(char) * 10);
			fscanf(file, "%s %d %d",
					result.events[i].vCpuName,
					&result.events[i].job.requiredProcessingTime,
					&result.events[i].job.arrivalTime);
		}
	}

	fclose(file);

	return result;
}

void freeResult(struct ParserResult* result)
{
	for(int i = 0; i < result->virtualCpuCount; ++i)
	{
		free(result->virtualCpus[i].name);
	}
	free(result->virtualCpus);

	for(int i = 0; i < result->eventCount; ++i)
	{
		free(result->events[i].vCpuName);
	}
	free(result->events);
}
