/*
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dump.c#5 $
 */

#include "dump.h"

#include <linux/module.h>

#include "memoryAlloc.h"
#include "typeDefs.h"

#include "constants.h"
#include "vdo.h"

#include "dedupeIndex.h"
#include "histogram.h"
#include "ioSubmitter.h"
#include "logger.h"

enum dumpOptions {
  // WorkQueues
  SHOW_ALBIREO_QUEUE,
  SHOW_BIO_ACK_QUEUE,
  SHOW_BIO_QUEUE,
  SHOW_CPU_QUEUES,
  SHOW_REQUEST_QUEUE,
  // MemoryPools
  SHOW_VIO_POOL,
  // Others
  SHOW_VDO_STATUS,
  // This one means an option overrides the "default" choices, instead
  // of altering them.
  SKIP_DEFAULT
};

enum dump_option_flags {
  // WorkQueues
  FLAG_SHOW_ALBIREO_QUEUE = (1 << SHOW_ALBIREO_QUEUE),
  FLAG_SHOW_BIO_ACK_QUEUE = (1 << SHOW_BIO_ACK_QUEUE),
  FLAG_SHOW_BIO_QUEUE     = (1 << SHOW_BIO_QUEUE),
  FLAG_SHOW_CPU_QUEUES    = (1 << SHOW_CPU_QUEUES),
  FLAG_SHOW_REQUEST_QUEUE = (1 << SHOW_REQUEST_QUEUE),
  // MemoryPools
  FLAG_SHOW_VIO_POOL      = (1 << SHOW_VIO_POOL),
  // Others
  FLAG_SHOW_VDO_STATUS    = (1 << SHOW_VDO_STATUS),
  // Special
  FLAG_SKIP_DEFAULT       = (1 << SKIP_DEFAULT)
  };

enum {
  FLAGS_ALL_POOLS    = (FLAG_SHOW_VIO_POOL),
  FLAGS_ALL_QUEUES   = (FLAG_SHOW_REQUEST_QUEUE
                        | FLAG_SHOW_ALBIREO_QUEUE
                        | FLAG_SHOW_BIO_ACK_QUEUE
                        | FLAG_SHOW_BIO_QUEUE
                        | FLAG_SHOW_CPU_QUEUES),
  FLAGS_ALL_THREADS  = (FLAGS_ALL_QUEUES),
  DEFAULT_DUMP_FLAGS = (FLAGS_ALL_THREADS | FLAG_SHOW_VDO_STATUS)
};

/**********************************************************************/
static inline bool is_arg_string(const char *arg, const char *thisOption)
{
  // device-mapper convention seems to be case-independent options
  return strncasecmp(arg, thisOption, strlen(thisOption)) == 0;
}

/**********************************************************************/
static void do_dump(KernelLayer  *layer,
                    unsigned int  dump_options_requested,
                    const char   *why)
{
  logInfo("%s dump triggered via %s", THIS_MODULE->name, why);
  // XXX Add in number of outstanding requests being processed by vdo
  uint32_t active, maximum;
  get_limiter_values_atomically(&layer->requestLimiter, &active, &maximum);
  int64_t outstanding = atomic64_read(&layer->biosSubmitted)
                        - atomic64_read(&layer->biosCompleted);
  logInfo("%" PRIu32 " device requests outstanding (max %" PRIu32 "), "
          "%" PRId64 " bio requests outstanding, poolName '%s'",
          active, maximum, outstanding, layer->deviceConfig->poolName);
  if ((dump_options_requested & FLAG_SHOW_REQUEST_QUEUE) != 0) {
    dumpKVDOWorkQueue(&layer->kvdo);
  }
  if ((dump_options_requested & FLAG_SHOW_BIO_QUEUE) != 0) {
    dumpBioWorkQueue(layer->ioSubmitter);
  }
  if (useBioAckQueue(layer)
      && ((dump_options_requested & FLAG_SHOW_BIO_ACK_QUEUE) != 0)) {
    dump_work_queue(layer->bioAckQueue);
  }
  if ((dump_options_requested & FLAG_SHOW_CPU_QUEUES) != 0) {
    dump_work_queue(layer->cpuQueue);
  }
  dumpDedupeIndex(layer->dedupeIndex,
                  (dump_options_requested & FLAG_SHOW_ALBIREO_QUEUE) != 0);
  dump_buffer_pool(layer->dataKVIOPool,
                   (dump_options_requested & FLAG_SHOW_VIO_POOL) != 0);
  if ((dump_options_requested & FLAG_SHOW_VDO_STATUS) != 0) {
    // Options should become more fine-grained when we have more to
    // display here.
    dumpKVDOStatus(&layer->kvdo);
  }
  reportMemoryUsage();
  logInfo("end of %s dump", THIS_MODULE->name);
}

/**********************************************************************/
static int parse_dump_options(unsigned int  argc,
                              char * const *argv,
                              unsigned int *dump_options_requested_ptr)
{
  unsigned int dump_options_requested = 0;

  static const struct {
    const char   *name;
    unsigned int  flags;
  } option_names[] = {
    // Should "albireo" mean sending queue + receiving thread + outstanding?
    { "dedupe",      FLAG_SKIP_DEFAULT | FLAG_SHOW_ALBIREO_QUEUE },
    { "dedupeq",     FLAG_SKIP_DEFAULT | FLAG_SHOW_ALBIREO_QUEUE },
    { "kvdodedupeq", FLAG_SKIP_DEFAULT | FLAG_SHOW_ALBIREO_QUEUE },
    { "bioack",      FLAG_SKIP_DEFAULT | FLAG_SHOW_BIO_ACK_QUEUE },
    { "kvdobioackq", FLAG_SKIP_DEFAULT | FLAG_SHOW_BIO_ACK_QUEUE },
    { "bioackq",     FLAG_SKIP_DEFAULT | FLAG_SHOW_BIO_ACK_QUEUE },
    { "bio",         FLAG_SKIP_DEFAULT | FLAG_SHOW_BIO_QUEUE },
    { "kvdobioq",    FLAG_SKIP_DEFAULT | FLAG_SHOW_BIO_QUEUE },
    { "bioq",        FLAG_SKIP_DEFAULT | FLAG_SHOW_BIO_QUEUE },
    { "cpu",         FLAG_SKIP_DEFAULT | FLAG_SHOW_CPU_QUEUES },
    { "kvdocpuq",    FLAG_SKIP_DEFAULT | FLAG_SHOW_CPU_QUEUES },
    { "cpuq",        FLAG_SKIP_DEFAULT | FLAG_SHOW_CPU_QUEUES },
    { "request",     FLAG_SKIP_DEFAULT | FLAG_SHOW_REQUEST_QUEUE },
    { "kvdoreqq",    FLAG_SKIP_DEFAULT | FLAG_SHOW_REQUEST_QUEUE },
    { "reqq",        FLAG_SKIP_DEFAULT | FLAG_SHOW_REQUEST_QUEUE },
    { "viopool",     FLAG_SKIP_DEFAULT | FLAG_SHOW_VIO_POOL },
    { "vdo",         FLAG_SKIP_DEFAULT | FLAG_SHOW_VDO_STATUS },

    { "pools",   FLAG_SKIP_DEFAULT | FLAGS_ALL_POOLS },
    { "queues",  FLAG_SKIP_DEFAULT | FLAGS_ALL_QUEUES },
    { "threads", FLAG_SKIP_DEFAULT | FLAGS_ALL_THREADS },
    { "default", FLAG_SKIP_DEFAULT | DEFAULT_DUMP_FLAGS },
    { "all",      ~0 },
  };

  bool options_okay = true;
  for (int i = 1; i < argc; i++) {
    int j;
    for (j = 0; j < COUNT_OF(option_names); j++) {
      if (is_arg_string(argv[i], option_names[j].name)) {
        dump_options_requested |= option_names[j].flags;
        break;
      }
    }
    if (j == COUNT_OF(option_names)) {
      logWarning("dump option name '%s' unknown", argv[i]);
      options_okay = false;
    }
  }
  if (!options_okay) {
    return -EINVAL;
  }
  if ((dump_options_requested & FLAG_SKIP_DEFAULT) == 0) {
    dump_options_requested |= DEFAULT_DUMP_FLAGS;
  }
  *dump_options_requested_ptr = dump_options_requested;
  return 0;
}

/**********************************************************************/
int vdo_dump(KernelLayer  *layer,
             unsigned int  argc,
             char * const *argv,
             const char   *why)
{
  unsigned int dump_options_requested = 0;
  int result = parse_dump_options(argc, argv, &dump_options_requested);
  if (result != 0) {
    return result;
  }
  do_dump(layer, dump_options_requested, why);
  return 0;
}

/**********************************************************************/
void vdo_dump_all(KernelLayer *layer, const char *why)
{
  do_dump(layer, ~0, why);
}
