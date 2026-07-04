#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "task.h"
#include "node_details.h"
#include "../../parallax/state_message.h"
#include "../../parallax/parallax_param.h"

/*
 * Describes the computation to execute.
 * Contains input data and the function identifier.
 */
typedef struct {
    char function_name[64];
} task_descriptor;

/*
 * Represents a scheduled execution unit.
 * Maps a task to the node selected to execute it.
 */
typedef struct {
    worker_node *target_node;
    task_descriptor *task;
    chunk_data *chunk;
} task_assignment;

task_assignment *
create_assignments(
    ParallaxParam *params,
    int param_count,
    const char *function,
    MachineMetrics *metrics,
    int node_count,
    int align
);

MachineMetrics *get_mock_machine_metrics();

#endif