/*
 * This file is distributed as part of MaxScale by SkySQL.  It is free
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
 * Copyright SkySQL Ab 2014, Percona LLC 2014
 *
 * 
 */

/**
 * @file genlogfilter.c - General log filter (genlog)
 * @verbatim
 *
 * The genlog filter uses regex matches for SQL, hosts and user.  It logs
 * the timestamp, the execution time, the user and the sql query to a single 
 * file.  File write goes through a buffer whose size can be set as an option.
 *
 * Date		Who		Description
 * 13/10/2014	Yves Trudea	 Creation of the filter, based on the topfilter
 *
 * @endverbatim
 */
#include <stdio.h>
#include <fcntl.h>
#include <filter.h>
#include <modinfo.h>
#include <modutil.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <regex.h>
#include <kafka.h>
#include <jansson.h>

#define USE_RE 0

extern int lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;
extern int hasKafka;

MODULE_INFO 	info = {
	MODULE_API_FILTER,
	MODULE_BETA_RELEASE,
	FILTER_VERSION,
	"A general query logging filter"
};

static char *version_str = "V1.0.1";

/*
 * The filter entry points
 */
static	FILTER	*createInstance(char **options, FILTER_PARAMETER **);
static	void	*newSession(FILTER *instance, SESSION *session);
static	void 	closeSession(FILTER *instance, void *session);
static	void 	freeSession(FILTER *instance, void *session);
static	void	setDownstream(FILTER *instance, void *fsession, DOWNSTREAM *downstream);
static	void	setUpstream(FILTER *instance, void *fsession, UPSTREAM *upstream);
static	int	routeQuery(FILTER *instance, void *fsession, GWBUF *queue);
static	int	clientReply(FILTER *instance, void *fsession, GWBUF *queue);
static	void	diagnostic(FILTER *instance, void *fsession, DCB *dcb);

static FILTER_OBJECT MyObject = {
    createInstance,
    newSession,
    closeSession,
    freeSession,
    setDownstream,
    setUpstream,
    routeQuery,
    clientReply,
    diagnostic,
};

/**
 * The Instance struct.
 * 
 */
typedef struct {
   	FILE  *fp;     /* Output file pointer */
	unsigned int	sessions;	/* Session count */
	char	*filepath;	/* Base of fielname to log into */
	int   buffer_size;  /* buffer size in MB */
	size_t  bufpos; /* Used amount in the buffer */
	char  *buffer;  /* Write buffer */
        long   queriesLogged;  /* number of queries logged */
	struct timeval	lastFlush; /* last flush ops */
        pthread_mutex_t  WriteLock; /* mutex protecting the write buffer and fp*/
	char	*host_re_def;	   /* host regex */
	char	*user_re_def;		/* user regex */
	char	*sql_re_def;		/* sql regex */
        char  hostname[60];
	regex_t	re_host;		/* Compiled regex for host */
        regex_t	re_user;		/* Compiled regex for user */
        regex_t	re_sql;		/* Compiled regex for sql */
        char  *applname;       /* a prefix to sort the source of the log*/
} GENLOG_INSTANCE;

/**
 * The session structure for this GENLOG filter.
 * This stores the downstream filter information, such that the
 * filter is able to pass the query on to the next filter (or router)
 * in the chain.
 *
 * It also holds the file descriptor to which queries are written.
 */
typedef struct {
	DOWNSTREAM	down;
	UPSTREAM	up;
        struct timeval start;
	int		active;
        int      isLogging;  /* bit set, 4 = user log, 2 = host log, 1 = sql log */
	unsigned int	sessionId;
	char		*clientHost;
	char		*userName;
	char		*current;
        json_t   *jsonObj;  /* allocated/deallocated in ClientReply */
        char     *writeBuffer; /* allocated/deallocated in ClientReply */
} GENLOG_SESSION;

/**
 * Implementation of the mandatory version entry point
 *
 * @return version string of the module
 */
char *
version()
{
	return version_str;
}

/**
 * The module initialisation routine, called when the module
 * is first loaded.
 */
void
ModuleInit()
{
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
FILTER_OBJECT *
GetModuleObject()
{
	return &MyObject;
}

/**
 * Create an instance of the filter for a particular service
 * within MaxScale.
 * 
 * @param options	The options for this filter
 * @param params	The array of name/value pair parameters for the filter
 *
 * @return The instance data for this new instance
 */
static	FILTER	*
createInstance(char **options, FILTER_PARAMETER **params)
{
	int		i;
	GENLOG_INSTANCE	*my_instance;

   LOGIF(LD, (skygw_log_write_flush(LOGFILE_DEBUG,
      "[GENLOG] createInstance")));

	if ((my_instance = calloc(1, sizeof(GENLOG_INSTANCE))) != NULL)
	{      
                my_instance->queriesLogged = 0;
		my_instance->buffer_size = 1;
	 	my_instance->bufpos = 0;
                gettimeofday(&my_instance->lastFlush, NULL);
		my_instance->filepath = strdup("/tmp/MaxScale_genlog.log");
                my_instance->applname = strdup("MaxScale");
		my_instance->buffer = NULL;
		my_instance->host_re_def = NULL;
		my_instance->user_re_def = NULL;
                my_instance->sql_re_def = NULL;
		pthread_mutex_init(&my_instance->WriteLock, NULL);
      
		for (i = 0; params && params[i]; i++)
		{
			if (!strcmp(params[i]->name, "buffer_size")) {
				my_instance->buffer_size = atoi(params[i]->value);
            			if (my_instance->buffer_size < 1) 
               			my_instance->buffer_size = 1;
         		}
			else if (!strcmp(params[i]->name, "filepath"))
			{
				free(my_instance->filepath);
				my_instance->filepath = strdup(params[i]->value);
			}
                        else if (!strcmp(params[i]->name, "applname"))
			{
                                free(my_instance->applname);
                                my_instance->applname = strdup(params[i]->value);
			}
			else if (!strcmp(params[i]->name, "host_re"))
			{
			        free(my_instance->host_re_def);
				my_instance->host_re_def = strdup(params[i]->value);
			}
			else if (!strcmp(params[i]->name, "user_re"))
			{
                                free(my_instance->user_re_def);
				my_instance->user_re_def = strdup(params[i]->value);
			}
			else if (!strcmp(params[i]->name, "sql_re"))
			{
                                free(my_instance->sql_re_def);
				my_instance->sql_re_def = strdup(params[i]->value);
			}
			else if (!filter_standard_parameter(params[i]->name))
			{
				LOGIF(LE, (skygw_log_write_flush(
					LOGFILE_ERROR,
					"genlogfilter: Unexpected parameter '%s'.\n",
					params[i]->name)));
			}
		}
		if (options)
		{
			LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,
				"genlogfilter: Options are not supported by this "
				" filter. They will be ignored\n")));
		}
		my_instance->sessions = 0;
		
		if (1) {
			if (my_instance->host_re_def &&
				regcomp(&my_instance->re_host, my_instance->host_re_def, REG_ICASE))
			{
				LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,
					"genlogfilter: Invalid regular expression '%s'"
					" for the host_re parameter.\n",
						my_instance->host_re_def)));
				free(my_instance->host_re_def);
				free(my_instance->user_re_def);
				free(my_instance->sql_re_def);
				free(my_instance->filepath);
		 		pthread_mutex_destroy(&my_instance->WriteLock);
				free(my_instance);
				return NULL;
			}
			if (my_instance->user_re_def &&
				regcomp(&my_instance->re_user, my_instance->user_re_def,
									REG_ICASE))
			{
				LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,
					"genfilter: Invalid regular expression '%s'"
					" for the user_re paramter.\n",
						my_instance->user_re_def)));
				regfree(&my_instance->re_host);
	  			free(my_instance->host_re_def);
				free(my_instance->user_re_def);
				free(my_instance->sql_re_def);
				free(my_instance->filepath);
		 		pthread_mutex_destroy(&my_instance->WriteLock);
				free(my_instance);
				return NULL;
			}
		
	      		if (my_instance->sql_re_def &&
				regcomp(&my_instance->re_sql, my_instance->sql_re_def,
									REG_ICASE))
			{
				LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,
					"genfilter: Invalid regular expression '%s'"
					" for the user_re paramter.\n",
						my_instance->sql_re_def)));
				regfree(&my_instance->re_host);
		 		regfree(&my_instance->re_user);
	  			free(my_instance->host_re_def);
				free(my_instance->user_re_def);
				free(my_instance->sql_re_def);
				free(my_instance->filepath);
		 		pthread_mutex_destroy(&my_instance->WriteLock);
				free(my_instance);
				return NULL;
			}
		}
                
                if ((my_instance->buffer = malloc(my_instance->buffer_size*1024+1)) 
                        == NULL) 
                {
			LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,
				"genfilter: Couldn't allocate the write buffer"
				" os size %i\n",
					my_instance->buffer_size)));
			if (USE_RE) {
				regfree(&my_instance->re_host);
		 		regfree(&my_instance->re_user);
			}
  			free(my_instance->host_re_def);
			free(my_instance->user_re_def);
			free(my_instance->sql_re_def);
			free(my_instance->filepath);
	 		pthread_mutex_destroy(&my_instance->WriteLock);
			free(my_instance);
			return NULL;
		}
	}
	return (FILTER *)my_instance;
}

/**
 * Associate a new session with this instance of the filter.
 *
 * Create the file to log to and open it.
 *
 * @param instance	The filter instance data
 * @param session	The session itself
 * @return Session specific data for this session
 */
static	void	*
newSession(FILTER *instance, SESSION *session)
{
	GENLOG_INSTANCE	*my_instance = (GENLOG_INSTANCE *)instance;
	GENLOG_SESSION	*my_session;
	char		*remote, *user;

   LOGIF(LD, (skygw_log_write_flush(LOGFILE_DEBUG,
      "[GENLOG] newSession")));
      
	if ((my_session = calloc(1, sizeof(GENLOG_SESSION))) != NULL)
	{
		my_session->sessionId = my_instance->sessions;
      my_instance->sessions++;
      my_session->isLogging = 1;
      my_session->jsonObj = NULL;

      if ((user = session_getUser(session)) != NULL)
         my_session->userName = strdup(user);
		else
			my_session->userName = NULL;

		if ((remote = session_get_remote(session)) != NULL)
			my_session->clientHost = strdup(remote);
		else
			my_session->clientHost = NULL;

		/* check is the user is excluded */
      if (my_instance->user_re_def != NULL)
         if (regexec(&my_instance->re_user, my_session->userName, 0, NULL, 0) == 0)
	 			my_session->isLogging =0;
      
		if (USE_RE) {
	   	if (my_instance->user_re_def != NULL)
				if (regexec(&my_instance->re_user, my_session->userName, 1, NULL, 0) == 0)
		 			my_session->isLogging |= 1 << 4;
	      	else
		 			my_session->isLogging &= ~(1 << 4);
			else
				my_session->isLogging |= 1 << 4;

         if (my_instance->host_re_def != NULL && my_session->isLogging == 4 &&
            regexec(&my_instance->re_host, my_session->clientHost, 1, NULL, 0) == 0)
            my_session->isLogging |= 1 << 2;
         else
            my_session->isLogging &= ~(1 << 2);
		}		
		my_session->current = NULL;
      my_session->writeBuffer = NULL;
      my_session->jsonObj = NULL;
		my_session->active = 1;
		
	}

	return my_session;
}

/**
 * Close a session with the filter, this is the mechanism
 * by which a filter may cleanup data structure etc.
 * In the case of the GENLOG filter we simple close the file descriptor.
 *
 * @param instance	The filter instance data
 * @param session	The session being closed
 */
static	void 	
closeSession(FILTER *instance, void *session)
{
	
}

/**
 * Free the memory associated with the session
 *
 * @param instance	The filter instance
 * @param session	The filter session
 */
static void
freeSession(FILTER *instance, void *session)
{
   GENLOG_SESSION	*my_session = (GENLOG_SESSION *)session;

   if (my_session->userName)
      free(my_session->userName);
   if (my_session->clientHost)
      free(my_session->clientHost);
   if (my_session->current)
      free(my_session->current);
   if (my_session->writeBuffer)
      free(my_session->writeBuffer);
   if (my_session->jsonObj)
      json_delete(my_session->jsonObj);
      
	free(session);
   
   return;
}


/**
 * Set the downstream filter or router to which queries will be
 * passed from this filter.
 *
 * @param instance	The filter instance data
 * @param session	The filter session 
 * @param downstream	The downstream filter or router.
 */
static void
setDownstream(FILTER *instance, void *session, DOWNSTREAM *downstream)
{
GENLOG_SESSION	*my_session = (GENLOG_SESSION *)session;

	my_session->down = *downstream;
}

/**
 * Set the upstream filter or session to which results will be
 * passed from this filter.
 *
 * @param instance	The filter instance data
 * @param session	The filter session 
 * @param upstream	The upstream filter or session.
 */
static void
setUpstream(FILTER *instance, void *session, UPSTREAM *upstream)
{
GENLOG_SESSION	*my_session = (GENLOG_SESSION *)session;

	my_session->up = *upstream;
}

/**
 * The routeQuery entry point. This is passed the query buffer
 * to which the filter should be applied. Once applied the
 * query should normally be passed to the downstream component
 * (filter or router) in the filter chain.
 *
 * @param instance	The filter instance data
 * @param session	The filter session
 * @param queue		The query data
 */
static	int	
routeQuery(FILTER *instance, void *session, GWBUF *queue)
{
	GENLOG_INSTANCE	*my_instance = (GENLOG_INSTANCE *)instance;
	GENLOG_SESSION	*my_session = (GENLOG_SESSION *)session;
	char		*ptr;
	int		length;

   LOGIF(LD, (skygw_log_write_flush(LOGFILE_DEBUG,
      "[GENLOG] routeQuery")));
      
	modutil_extract_SQL(queue, &ptr, &length);
	if (USE_RE) {
		if (my_session->active &&  my_session->isLogging >= 6 )
		{
			if (my_instance->sql_re_def != NULL) 
				if (regexec(&my_instance->re_sql, ptr, 0, NULL, 0) == 0) 
	      			{
		 			my_session->isLogging |= 1 << 1;
		 
		 			if (my_session->current)
		    				free(my_session->current);

			 		gettimeofday(&my_session->start, NULL);
			 		my_session->current = strndup(ptr, length);
	      			}
	      			else
			 		my_session->isLogging &= ~(1 << 1);
			else
				my_session->isLogging |= 1 << 1;
		}
	} else {
 			if (my_session->current)
    				free(my_session->current);

	 		gettimeofday(&my_session->start, NULL);
	 		my_session->current = strndup(ptr, length);
	}

        LOGIF(LD, (skygw_log_write_flush(
                LOGFILE_DEBUG,
                "%lu [genlogfilter:routeQuery] routing: '%s'\n ",
                pthread_self(),
                my_session->current)));

	/* Pass the query downstream */
	return my_session->down.routeQuery(my_session->down.instance,
			my_session->down.session, queue);
}

static int
clientReply(FILTER *instance, void *session, GWBUF *reply)
{
	GENLOG_INSTANCE	*my_instance = (GENLOG_INSTANCE *)instance;
	GENLOG_SESSION	*my_session = (GENLOG_SESSION *)session;
	struct		timeval		tv, diff;
	char		*printformat = "# TS: %lu   Query_time: %.6f   User:%s   Host: %s  Session_id: %d   Appl:%s\n%s\n";
      
        LOGIF(LD, (skygw_log_write_flush(LOGFILE_DEBUG,
                "[GENLOG] clientReply")));
      
	if (my_session->current && my_session->isLogging == 1)
	{
		gettimeofday(&tv, NULL);
		timersub(&tv, &(my_session->start), &diff);				

                if (hasKafka) {
                      
                        if (! my_session->jsonObj)
                        my_session->jsonObj = json_object();

                        json_object_set_new(my_session->jsonObj,"Type",json_string("query"));
                        json_object_set_new(my_session->jsonObj,"Ts",json_integer(tv.tv_sec));
                        json_object_set_new(my_session->jsonObj,"QueryTime",json_real((double)((diff.tv_sec * 1000000)+(diff.tv_usec)) / 1000000));
                        json_object_set_new(my_session->jsonObj,"User",json_string(my_session->userName));
                        json_object_set_new(my_session->jsonObj,"Host",json_string(my_session->clientHost));
                        json_object_set_new(my_session->jsonObj,"SessionId",json_integer(my_session->sessionId)); 
                        json_object_set_new(my_session->jsonObj,"Appl",json_string(kafkaGetApplName()));
                        json_object_set_new(my_session->jsonObj,"Sql",json_string(my_session->current));

                        if (kafkaProduce(json_dumps(my_session->jsonObj,JSON_COMPACT)))
                        __sync_fetch_and_add(&my_instance->queriesLogged, 1);

                        json_object_clear(my_session->jsonObj);


                }
                else
                {
                        asprintf(&my_session->writeBuffer,printformat,tv.tv_sec, 
                                (double)((diff.tv_sec * 1000000)+(diff.tv_usec)) / 1000000,
                        my_session->userName,my_session->clientHost,my_session->sessionId,
                        my_instance->applname,my_session->current);      

                        pthread_mutex_lock(&my_instance->WriteLock);
                        if (! my_instance->fp ) my_instance->fp = fopen(my_instance->filepath,"a");
                 
                        /* Every 2s, close/open the log file for easy manipulation/rotation */
                        if ((tv.tv_sec - my_instance->lastFlush.tv_sec) > 1) {			
                                fclose(my_instance->fp);
                                my_instance->fp = fopen(my_instance->filepath,"a");
                                gettimeofday(&my_instance->lastFlush, NULL);
                        }
                        fputs(my_session->writeBuffer,my_instance->fp);
                        pthread_mutex_unlock(&my_instance->WriteLock);
                        __sync_fetch_and_add(&my_instance->queriesLogged, 1);
                        free(my_session->writeBuffer);
                }
	}

        my_session->writeBuffer = NULL;
	free(my_session->current);
	my_session->current = NULL;

	/* Pass the result upstream */
	return my_session->up.clientReply(my_session->up.instance,
			my_session->up.session, reply);
}

/**
 * Diagnostics routine
 *
 * If fsession is NULL then print diagnostics on the filter
 * instance as a whole, otherwise print diagnostics for the
 * particular session.
 *
 * @param	instance	The filter instance
 * @param	fsession	Filter session, may be NULL
 * @param	dcb		The DCB for diagnostic output
 */
static	void
diagnostic(FILTER *instance, void *fsession, DCB *dcb)
{
   
	GENLOG_INSTANCE	*my_instance = (GENLOG_INSTANCE *)instance;

		dcb_printf(dcb, "\t\tBuffer size (KB)		%d\n",
				my_instance->buffer_size);
	if (my_instance->host_re_def)
		dcb_printf(dcb, "\t\tHost matching regex 	%s\n",
				my_instance->host_re_def);
  	if (my_instance->user_re_def)
		dcb_printf(dcb, "\t\tUser matching regex 	%s\n",
				my_instance->user_re_def);
	if (my_instance->sql_re_def)
		dcb_printf(dcb, "\t\tSql matching regex 	%s\n",
				my_instance->sql_re_def);
   if (hasKafka) {
      dcb_printf(dcb, "\t\tLogging to Kakfa\n");   
   }
   else
   {
      if (my_instance->filepath)
         dcb_printf(dcb, "\t\tLogging to file		%s\n",
               my_instance->filepath);
                  
      dcb_printf(dcb, "\t\tLast buffer flush		%s\n",
         asctime(localtime(&my_instance->lastFlush.tv_sec)));
   }
   dcb_printf(dcb, "\t\tNumber of queries logged:	%li\n",
      my_instance->queriesLogged);
   
}
