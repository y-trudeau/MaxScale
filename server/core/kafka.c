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

/**
 * @file kafka.c  - handles high volume logging to kafka brokers
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			   Description
 * 18/11/14	Yves Trudeau	Initial implementation
 *
 * @endverbatim
 */
 
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <syslog.h>
#include <ini.h>
#include <config.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <mysql.h>
#include <sys/time.h>
#include <librdkafka/rdkafka.h>

static void kafkaMsgDelivered (rd_kafka_t *,void *, size_t, int, void *, void *);
static void kafkaLogger (const rd_kafka_t *,int,const char *, const char *);
void kafkaOptions();

/* Global variables */
rd_kafka_topic_t *rkt;
rd_kafka_conf_t *conf;
rd_kafka_topic_conf_t *topic_conf;
rd_kafka_t *rk = NULL;
char  *brokers = NULL;
char  *topicName = NULL;
char  *applName = NULL;
int   partition = -1;
int   hasKafka = 0;

extern int lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;

/**
 * Initialise the kafka handle
 */
void
kafkaInit()
{
   char errstr[512];
   
   /* Process the options */
   kafkaOptions();
	
   /* Set up a message delivery report callback.
    * It will be called once for each message, either on successful
    * delivery to broker, or upon failure to deliver to broker. */
   rd_kafka_conf_set_dr_cb(conf, kafkaMsgDelivered);
   
   /* Create Kafka handle */
   if (!(rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf,
      errstr, sizeof(errstr)))) {
      LOGIF(LE, (skygw_log_write_flush(
         LOGFILE_ERROR,
         "Error : Creation of the Kafka handle : %s "
         , errstr)));
      exit(1);
   }

   /* Set logger */
   rd_kafka_set_logger(rk, kafkaLogger);
   rd_kafka_set_log_level(rk, LOG_DEBUG);

   /* Add brokers */
   if (rd_kafka_brokers_add(rk, brokers) == 0) {
      LOGIF(LE, (skygw_log_write_flush(
         LOGFILE_ERROR,
         "Error : Kafka, no valid brokers specified : %s "
         , brokers)));
      exit(1);
   }

   /* Create topic */
   rkt = rd_kafka_topic_new(rk, topicName, topic_conf);
   if (!rkt) {
         LOGIF(LE, (skygw_log_write_flush(
         LOGFILE_ERROR,
         "Error : Kafka, unable to create the topic : %s "
         , topicName)));
      exit(1);
   }
   
   struct timeval		tv;
   int i;
   
   /* Is the partition assigned */
   if (partition < 1) 
   {
      const struct rd_kafka_metadata *metadata;
      const struct rd_kafka_metadata_topic *t;
      rd_kafka_resp_err_t err;
      
      /* Fetch metadata */
      err = rd_kafka_metadata(rk, rkt ? 0 : 1, rkt,
                        &metadata, 2000);
      if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            fprintf(stderr,
                  "%% Failed to acquire metadata: %s\n",
                  rd_kafka_err2str(err));                  
      }
      else 
      {
         i = 0;
         while (i < metadata->topic_cnt) {
            t = &metadata->topics[i];
            if (!strcmp(topicName,t->topic))
               break;
            i++;
         }

         if (i == metadata->topic_cnt) {
            LOGIF(LE, (skygw_log_write_flush(
               LOGFILE_ERROR,
               "Error : Kafka, couldn't find the topic to set the partition : %s "
               , topicName)));
            rd_kafka_metadata_destroy(metadata);
            exit(1);
         }
         
         gettimeofday(&tv, NULL);
         partition = tv.tv_sec % t->partition_cnt; /* modulo to ts as a randomizer */
         if (!rd_kafka_topic_partition_available (rkt,
                  partition))
            partition = -1;
         else 
         {
            /* let's try the other partitions in order */
            i = 0;
            while (i < t->partition_cnt && partition < 0) {
               if (rd_kafka_topic_partition_available (rkt,
                     i))
                  partition = i;
               i++;
            }
         }
      }
      rd_kafka_metadata_destroy(metadata);
   }
      
   if (partition < 0) partition = 0; /* fallback on 0 */
   
   hasKafka = 1;
}

/**
 * Shutdown the kafka handle
 */
void
kafkaShutdown()
{
   int i;
   /* Poll to handle delivery reports */
	rd_kafka_poll(rk, 0);

	/* Wait for messages to be delivered */
   i = 0;
	while (rd_kafka_outq_len(rk) > 0 && i < 10) {
		rd_kafka_poll(rk, 100);
      i++;
   }

	/* Destroy the handle */
	rd_kafka_destroy(rk);
      
   /* Let background threads clean up and terminate cleanly. */
	rd_kafka_wait_destroyed(2000);
}
/**
 * Parse the kafka_options.  See here for available options:
 * https://github.com/edenhill/librdkafka/blob/master/CONFIGURATION.md
 * 
 * 
 */
void
kafkaOptions()
{
   char *options;
   char *optionItem;
   char *name, *val;
   rd_kafka_conf_res_t res;
   char errstr[512];
   
   /* Kafka configuration */
	conf = rd_kafka_conf_new();

	/* Topic configuration */
	topic_conf = rd_kafka_topic_conf_new();
   
   /* get the kafka_options parameter */
   options = config_kafka_options();
   
   LOGIF(LM, (skygw_log_write_flush(
         LOGFILE_ERROR,
         "INFO : Kafka configuration. "
         " options returned = %s\n"
         , options)));
   
   optionItem = strtok(options, ";");
   while (optionItem)
   {
      name = optionItem;

      if (!(val = strchr(name, '='))) {
         LOGIF(LE, (skygw_log_write_flush(
              LOGFILE_ERROR,
              "Error : Kafka configuration. "
              " Expected property=value format for %s\n"
              , name)));
         exit(1);
      }

      *val = '\0';
		val++;
      
      LOGIF(LT, (skygw_log_write_flush(
         LOGFILE_TRACE,
         "TRACE : Kafka configuration. "
         " processing %s with value %s\n"
         , name,val)));
   
      res = RD_KAFKA_CONF_UNKNOWN;
      if (!strcmp(name, "brokers")) 
      {
         if (brokers) free(brokers);
         brokers = strdup(val);
      } 
      else if (!strcmp(name, "topicName"))
      {
         if (topicName) free(topicName);
         topicName = strdup(val);
      } 
      else if (!strcmp(name, "applName"))
      {
         if (applName) free(applName);
         applName = strdup(val);
      }
      else if (!strncmp(name, "topic.", strlen("topic.")))
      {
			res = rd_kafka_topic_conf_set(topic_conf,
		      name+
		      strlen("topic."),
		      val,
            errstr,
            sizeof(errstr));
      } 
      else
      {
         if (res == RD_KAFKA_CONF_UNKNOWN)
            res = rd_kafka_conf_set(conf, name, val,
                     errstr, sizeof(errstr));

         if (res != RD_KAFKA_CONF_OK) {
            LOGIF(LE, (skygw_log_write_flush(
               LOGFILE_ERROR,
               "Error : Unknown kafka option '%s', %s.",
               name, errstr)));
            exit(1);
         }
      }
      optionItem = strtok(NULL, ";");
   }
   
}

/**
 * Message delivery report callback.
 * Called once for each message.
 * See rdkafka.h for more information.
 */
static void kafkaMsgDelivered (rd_kafka_t *rk,
			   void *payload, size_t len,
			   int error_code,
			   void *opaque, void *msg_opaque) {

	if (error_code)
   {
      LOGIF(LE, (skygw_log_write_flush(
         LOGFILE_ERROR,
         "Error :  Message delivery failed: %s",
         rd_kafka_err2str(error_code))));
   }
	else 
   {
      LOGIF(LT, (skygw_log_write_flush(
         LOGFILE_TRACE,
         "TRACE : Kafka message delivered "
         "(%zd bytes)", len)));
   }
}

/**
 * Kafka logger callback (optional)
 */
static void kafkaLogger (const rd_kafka_t *rk, int level,
		    const char *fac, const char *buf) {             
   LOGIF(LE, (skygw_log_write_flush(
      LOGFILE_ERROR,
      "Error : RDKAFKA-%i-%s: %s: %s",
      level, fac, rd_kafka_name(rk), buf)));
}

/**
 * Kafka produce message
 * Queue a message for the brokers
 */
int kafkaProduce(char *buf)
{
   int errno;
   /* Produce message. */
   if ((errno = rd_kafka_produce(rkt, partition,
              RD_KAFKA_MSG_F_FREE,
              /* Payload and length */
              buf, strlen(buf),
              /* Optional key and its length */
              NULL, 0,
              /* Message opaque, provided in
               * delivery report callback as
               * msg_opaque. */
              NULL)) == -1) {
      
      LOGIF(LE, (skygw_log_write_flush(
         LOGFILE_ERROR,
         "Error : Failed to produce to topic %s "
         "partition %i",
         rd_kafka_topic_name(rkt), partition,
         rd_kafka_err2str(
            rd_kafka_errno2err(errno)))));
      /* Poll to handle delivery reports */
      rd_kafka_poll(rk, 0);
      return 0;
   }
   /* Poll to handle delivery reports */
   rd_kafka_poll(rk, 0);
   return 1;
}

/**
 * Return the ApplName used for kafka, convinience function
 * 
 */
char * kafkaGetApplName()
{
   return applName;
}
