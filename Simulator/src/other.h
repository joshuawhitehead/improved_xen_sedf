/*
 * Currently consisting only of data structures
 * that are to small to deserve a own header file.
 */

#ifndef OTHER_H_
#define OTHER_H_

struct Job
{
	int requiredProcessingTime;
	int arrivalTime;
};

struct EDFJob
{
	int workTime;
	int deadline;
};

struct vCPU
{
	char* name;
	int requestedBudget;
	int period;
};

struct pCPU
{
	int capacity;
};

struct SimulationEvent
{
	char* vCpuName;
	struct Job job;
};

#endif /* OTHER_H_ */
