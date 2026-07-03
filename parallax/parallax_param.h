#ifndef PARALLAX_PARAM_H
#define PARALLAX_PARAM_H

#include <stddef.h>

/* =========================================================================
 * parallax_param.h
 *
 * Defines how each parameter of a Parallax-annotated function is described
 * when passed to the distributed runtime (execute_fxn / orchestrator).
 * ========================================================================= */

/* How a parameter is distributed across worker nodes */
typedef enum {
    PARALLAX_SCATTER    = 0,   /* chunk-and-scatter: data is split across nodes          */
    PARALLAX_BROADCAST  = 1,   /* sent to every node as-is                               */
    PARALLAX_REDUCE     = 2,   /* output parameter — collected and reduced back           */
    PARALLAX_SIZE_OF    = 3,   /* this param is the byte-size of the preceding ptr param */
} ParallaxDistribution;

/* Descriptor for a single function parameter */
typedef struct {
    void               *data;           /* pointer to the parameter value        */
    size_t              size;           /* byte size of *data (0 = ptr, unknown) */
    ParallaxDistribution distribution;  /* how this param is sent to nodes       */
    int                 index;          /* 0-based position in the original sig  */
    char                type_name[64];  /* C type string, e.g. "int *" or "int" */
} ParallaxParam;

/* Default node cap when user does not specify a node count (0 = auto) */
#define PARALLAX_DEFAULT_NODE_CAP 8

/* Execution context containing metadata for execution */
typedef struct {
    int expected_node_count;  /* 0 = auto (capped at PARALLAX_DEFAULT_NODE_CAP) */
    char aggregator_name[64];
} ParallaxExecutionCtx;

#endif /* PARALLAX_PARAM_H */
