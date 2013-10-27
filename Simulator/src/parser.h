#ifndef PARSER_H_
#define PARSER_H_

#include "other.h"

struct ParserResult
{
	struct pCPU cpu;
	int virtualCpuCount;
	struct vCPU* virtualCpus;
	int eventCount;
	struct SimulationEvent* events;
};

/**
 * Parses the file at the specified location
 * The file have to exist and be correct
 * The format is line based, elements are separated with whitespaces
 * The vCpu names are limited to 9 characters
 * 		cpu capacity
 *		count of virtual cpus
 *		vCpu name, requested budget, period
 *		...
 *		count of simulation events
 *		target vCpu name, required processing time, arrival time
 * @param path
 * @return The parsed data
 */
struct ParserResult parseFile(const char* path);

/**
 * Frees any memory allocated within the given ParserResult
 * @param result
 */
void freeResult(struct ParserResult* result);

#endif /* PARSER_H_ */
