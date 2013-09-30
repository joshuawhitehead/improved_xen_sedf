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
};

struct pCPU
{
	int capacity;
};

#endif /* OTHER_H_ */
