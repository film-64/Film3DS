#pragma once
/* Frozen evidence transport only; never creates/repeats a camera task.
 * One response at a time, no request queue. Forward only bytes accepted by
 * send(). EAGAIN/poll timeout permits at most eight no-progress iterations;
 * any other error fails the response. No evidence is altered or discarded.
 * Client independently checks frozen index, ranges, native CRC and SHA-256.
 */
enum { RAM_SEND_CHUNK=4096, RAM_SEND_POLL_MS=1000, RAM_SEND_IDLE_LIMIT=8 };
