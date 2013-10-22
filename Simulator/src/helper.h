/*
 * Containing some basic includes used almost everywhere
 * and provides some commonly used defines and helper methods.
 */

#ifndef HELPER_H_
#define HELPER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>
#include "list.h"

#define ASSERT assert

int max(int a, int b);

int min(int a, int b);

enum RunAction
{
	NewJob,
	Work,
	JobEnd
};

struct RunData
{
	struct list_head list;
	int time;
	enum RunAction action;
};

#endif /* HELPER_H_ */
