/*
 * This file is distributed as part of the SkySQL Gateway.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright Percona LLC 2014
 */
 
#include <time.h>
#include <librdkafka/rdkafka.h>

/**
 * @file kafka.h - handles high volume logging to kafka brokers
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			   Description
 * 18/11/14	Yves Trudeau	Initial implementation
 *
 * @endverbatim
 */

#define KAFKA_INSTANCE_NAME_LENGTH 100

extern void kafkaInit();
extern void kafkaShutdown();
extern int  kafkaProduce(char *);
extern char * kafkaGetApplName();
