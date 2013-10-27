#ifndef EDF_H_
#define EDF_H_

#include "vector.h"
#include "helper.h"
#include "other.h"
#include "cbs.h"

struct EDFRunQueue
{
	struct pCPU* cpu;
	int availableCapacity;
	vector_p servers;
};

/**
 * Returns the server with the lowest deadline that is currently active.
 * @param server
 * @return Either the server with the lowest deadline or NULL if none is active.
 */
struct CBS* GetServerWithED(struct EDFRunQueue* server);

/**
 * Tries to register the given vCPU with the parameters and if successful
 * creates the appropriate CBS for the vCPU.
 * The definition when it is successful is not completely implemented yet but
 * we can say that the pCPU should at least have enough remaining capacity
 * to serve the vCPU.
 * @param queue
 * @param vcpu
 * @return True if successful and false if not.
 */
bool RegisterVCPU(struct EDFRunQueue* queue, struct vCPU* vcpu);

#endif /* EDF_H_ */
