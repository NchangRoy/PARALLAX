#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <time.h>
#include "node.h"

/**
 * Initialize persistence (creates directories, starts flusher thread)
 */
void flusher_start(void);

/**
 * Stop persistence thread gracefully
 */
void flusher_stop(void);

/**
 * Save a single node's snapshot (hardware + current state) to disk in JSON format
 */
void persist_node_snapshot(const NodeInfo* node);

/**
 * Push metrics to memory buffer (flushes to disk automatically if full)
 */
void metrics_buf_push(const char* uuid, time_t ts, const NodeMetrics* m);

/**
 * Force flush of in-memory metrics to CSV files on disk
 */
void metrics_flush_now(void);

#endif /* PERSISTENCE_H */
