/*
 * Copyright 2008-2015 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 
 * 百度语音识别
   1. 获取获取token（判断token是否过期，如果过期重新获取，解析）
   2. 读取语音流
   3. 发送http请求,收到json
   4. 解析json写入文本
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mrcp_recog_engine.h"
#include "mpf_activity_detector.h"
#include "apt_consumer_task.h"
#include "apt_log.h"

#include <curl/curl.h>
#include <pthread.h>
#include <unistd.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlmemory.h>
#include <libxml/xpointer.h>
//xiaqingmu
#include<time.h>
#include <json/json.h>



#define RECOG_ENGINE_TASK_NAME "BD Recog Engine"
#define XML_FILENAME "/usr/local/unimrcp/conf/bd_recog.xml"
#define WAV_RESULT_FILENAME "/usr/local/unimrcp/bin/bd_result/asr_result.txt"
FILE* file = NULL;
typedef struct bd_recog_engine_t bd_recog_engine_t;
typedef struct bd_recog_channel_t bd_recog_channel_t;
typedef struct bd_recog_msg_t bd_recog_msg_t;

typedef struct bd_recog_token_t bd_recog_token_t;


typedef struct WriteThis WriteThis;
static const struct WriteThis {
  apt_bool_t bhead;
  apt_bool_t bvoice;
  apt_bool_t bend;
};
//xiaqingmu
struct bd_recog_token_t{
  char           api_key[40];
  char           secret_key[40];
  char           api_url[50];
  char           token_url[150];  
};

//声明全局的token
static char *token; 
static time_t  start_time;
static xmlDocPtr pdoc;

typedef enum{
  RECOGNITION_COMPLETE,
  RECOGNITION_INPUT
}stream_status;

//XML
xmlDocPtr getDocRoot(char * filename);
int freeDocRoot(xmlDocPtr pdoc);
char * getCurAttribute(xmlDocPtr pdoc,char * title,char* sttstr);

void get_xml_token(xmlDocPtr pdoc,bd_recog_token_t* recog_token);
void get_xml_asr(xmlDocPtr pdoc,bd_recog_channel_t* recog_channel);
void get_token_url(bd_recog_token_t* recog_token);

int is_token_overdue(bd_recog_channel_t* recog_channel);
int  get_token_post(bd_recog_token_t* recog_token);
//void get_asr_url(bd_recog_channel_t* recog_channel);
int asr_post(bd_recog_channel_t *recog_channel);
int parse_token(const char *response,char *token);

/** Declaration of recognizer engine methods */
static apt_bool_t bd_recog_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t bd_recog_engine_open(mrcp_engine_t *engine);
static apt_bool_t bd_recog_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* bd_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);



static const struct mrcp_engine_method_vtable_t engine_vtable = {
	bd_recog_engine_destroy,
	bd_recog_engine_open,
	bd_recog_engine_close,
	bd_recog_engine_channel_create
};


/** Declaration of recognizer channel methods */
static apt_bool_t bd_recog_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t bd_recog_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t bd_recog_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t bd_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	bd_recog_channel_destroy,
	bd_recog_channel_open,
	bd_recog_channel_close,
	bd_recog_channel_request_process
};

/** Declaration of recognizer audio stream methods */
static apt_bool_t bd_recog_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t bd_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t bd_recog_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t bd_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);

static apt_bool_t bd_recog_stream_recog(bd_recog_channel_t *recog_channel, const void *voice_data,  unsigned int voice_len); 

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	bd_recog_stream_destroy,
	NULL,
	NULL,
	NULL,
	bd_recog_stream_open,
	bd_recog_stream_close,
	bd_recog_stream_write,
	NULL
};

/** Declaration of bd recognizer engine */
struct bd_recog_engine_t {
	apt_consumer_task_t    *task;
};

/** Declaration of bd recognizer channel */
struct bd_recog_channel_t {
	/** Back pointer to engine */
	bd_recog_engine_t     *bd_engine;
	/** Engine channel base */
	mrcp_engine_channel_t   *channel;

	/** Active (in-progress) recognition request */
	mrcp_message_t          *recog_request;
	/** Pending stop response */
	mrcp_message_t          *stop_response;
	/** Indicates whether input timers are started */
	apt_bool_t               timers_started;
	/** Voice activity detector */
	mpf_activity_detector_t *detector;
	/** File to write utterance to */
	FILE                    *audio_out;
	
	
	//----------------------
	char                    *asr_url;
	char                    *result;
	apt_bool_t              recog_started;
	pthread_mutex_t         mutex;
	stream_status           status;
	unsigned char           *buffer;
	WriteThis               wt;
	int                     total_size;
	char                    url_info[10][40];
};

typedef enum {
	BD_RECOG_MSG_OPEN_CHANNEL,
	BD_RECOG_MSG_CLOSE_CHANNEL,
	BD_RECOG_MSG_REQUEST_PROCESS
} bd_recog_msg_type_e;

/** Declaration of bd recognizer task message */
struct bd_recog_msg_t {
	bd_recog_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};

static apt_bool_t bd_recog_msg_signal(bd_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t bd_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="RECOG-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN,"RECOG-PLUGIN")

/** Use custom log source mark */
#define RECOG_LOG_MARK   APT_LOG_MARK_DECLARE(RECOG_PLUGIN)

struct config {
	
};

//config global_config;

/** Create bd recognizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	bd_recog_engine_t *bd_engine = apr_palloc(pool,sizeof(bd_recog_engine_t));
	//xiaqingmu 创建引擎同时创建token
	
	//token = (char*)malloc(40); //记得释放
	//bd_recog_token_t *bd_token = (bd_recog_token_t *)malloc(sizeof(bd_recog_token_t));
	//xmlDocPtr pdoc = getDocRoot(XML_FILENAME);
	//get_xml_token(pdoc,bd_token);
	//get_token_url(bd_token);
	//get_token_post(bd_token);
	//freeDocRoot(pdoc);
    //free(bd_token);
		
	//xiaqingmu 
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(bd_recog_msg_t),pool);
	bd_engine->task = apt_consumer_task_create(bd_engine,msg_pool,pool);
	if(!bd_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(bd_engine->task);
	apt_task_name_set(task,RECOG_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = bd_recog_msg_process;
	}

	/* create engine base */
	return mrcp_engine_create(
				MRCP_RECOGNIZER_RESOURCE,  /* MRCP resource identifier */
				bd_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy recognizer engine */
static apt_bool_t bd_recog_engine_destroy(mrcp_engine_t *engine)
{
	
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] bd_recog_engine_destroy start-----------------------------\n");
	bd_recog_engine_t *bd_engine = engine->obj;
	if(bd_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(bd_engine->task);
		apt_task_destroy(task);
		bd_engine->task = NULL;
	}
	freeDocRoot(pdoc);
	free(token);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] bd_recog_engine_destroy end-----------------------------\n");	
	return TRUE;
}

/** Open recognizer engine */
static apt_bool_t bd_recog_engine_open(mrcp_engine_t *engine)
{
	bd_recog_engine_t *bd_engine = engine->obj;
	if(bd_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(bd_engine->task);
		apt_task_start(task);
	}
	//xiaqingmu 
	token = (char*)malloc(120); //记得释放
	//bd_recog_token_t *bd_token = (bd_recog_token_t *)malloc(sizeof(bd_recog_token_t));
	bd_recog_token_t bd_token ;
	pdoc = getDocRoot(XML_FILENAME);
	get_xml_token(pdoc,&bd_token);
	get_token_url(&bd_token);
	get_token_post(&bd_token);
    //free(bd_token);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] bd_recog_engine_open end-----------------------------\n");
	//xiqingmu
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close recognizer engine */
static apt_bool_t bd_recog_engine_close(mrcp_engine_t *engine)
{
	bd_recog_engine_t *bd_engine = engine->obj;
	if(bd_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(bd_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

static mrcp_engine_channel_t* bd_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] bd_recog_engine_channel_create start-----------------------------\n");
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create bd recog channel */
	bd_recog_channel_t *recog_channel = apr_palloc(pool,sizeof(bd_recog_channel_t));
	recog_channel->bd_engine = engine->obj;
	recog_channel->recog_request = NULL;
	recog_channel->stop_response = NULL;
	recog_channel->detector = mpf_activity_detector_create(pool);
	recog_channel->audio_out = NULL;
	//xiaqingmu  初始化recog_channel

	recog_channel->recog_started = FALSE;
    recog_channel->result = (char*)malloc(1024*1024);
	recog_channel->buffer = NULL;
	recog_channel->wt.bvoice = FALSE;
	pthread_mutex_init(&recog_channel->mutex, NULL);
	recog_channel->status = RECOGNITION_INPUT;
	
	//xmlDocPtr pdoc = getDocRoot(XML_FILENAME);
	recog_channel->asr_url = (char*)malloc(1000);
	get_xml_asr(pdoc,recog_channel);
	//get_asr_url(recog_channel);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] url  : %s",recog_channel->asr_url);
	//freeDocRoot(pdoc);
	
    //recog_channel->bvoice_readsize = 0;
	recog_channel->total_size = 0;
	
    //--------------------------------
	capabilities = mpf_sink_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			recog_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	recog_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			recog_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	return recog_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t bd_recog_channel_destroy(mrcp_engine_channel_t *channel)
{
	/* nothing to destrtoy */
	bd_recog_channel_t *recog_channel = channel->method_obj;
	free(recog_channel->asr_url);
	free(recog_channel->result);
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t bd_recog_channel_open(mrcp_engine_channel_t *channel)
{
	return bd_recog_msg_signal(BD_RECOG_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t bd_recog_channel_close(mrcp_engine_channel_t *channel)
{
	return bd_recog_msg_signal(BD_RECOG_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t bd_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	return bd_recog_msg_signal(BD_RECOG_MSG_REQUEST_PROCESS,channel,request);
}

/** Process RECOGNIZE request */
static apt_bool_t bd_recog_channel_recognize(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process RECOGNIZE request */
	mrcp_recog_header_t *recog_header;
	bd_recog_channel_t *recog_channel = channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_sink_stream_codec_get(channel);

	if(!descriptor) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	recog_channel->timers_started = TRUE;

	/* get recognizer header */
	recog_header = mrcp_resource_header_get(request);
	if(recog_header) {
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_START_INPUT_TIMERS) == TRUE) {
			recog_channel->timers_started = recog_header->start_input_timers;
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_NO_INPUT_TIMEOUT) == TRUE) {
			mpf_activity_detector_noinput_timeout_set(recog_channel->detector,recog_header->no_input_timeout);
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT) == TRUE) {
			mpf_activity_detector_silence_timeout_set(recog_channel->detector,recog_header->speech_complete_timeout);
		}
	}

	if(!recog_channel->audio_out) {
		const apt_dir_layout_t *dir_layout = channel->engine->dir_layout;
		char *file_name = apr_psprintf(channel->pool,"utter-%dkHz-%s.pcm",
							descriptor->sampling_rate/1000,
							request->channel_id.session_id.buf);
		char *file_path = apt_vardir_filepath_get(dir_layout,file_name,channel->pool);
		if(file_path) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Open Utterance Output File [%s] for Writing",file_path);
			recog_channel->audio_out = fopen(file_path,"wb");
			if(!recog_channel->audio_out) {
				apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Open Utterance Output File [%s] for Writing",file_path);
			}
		}
	}

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	//----------------------------------
	recog_channel->recog_started = FALSE;
	//recog_channel->result = NULL;
	memset(recog_channel->result,0,1024*1024);
	//----------------------------------
	recog_channel->recog_request = request;
	return TRUE;
}

/** Process STOP request */
static apt_bool_t bd_recog_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process STOP request */
	bd_recog_channel_t *recog_channel = channel->method_obj;
	/* store STOP request, make sure there is no more activity and only then send the response */
	recog_channel->stop_response = response;
	return TRUE;
}

/** Process START-INPUT-TIMERS request */
static apt_bool_t bd_recog_channel_timers_start(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	bd_recog_channel_t *recog_channel = channel->method_obj;
	recog_channel->timers_started = TRUE;
	return mrcp_engine_channel_message_send(channel,response);
}

/** Dispatch MRCP request */
static apt_bool_t bd_recog_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case RECOGNIZER_SET_PARAMS:
			break;
		case RECOGNIZER_GET_PARAMS:
			break;
		case RECOGNIZER_DEFINE_GRAMMAR:
			break;
		case RECOGNIZER_RECOGNIZE:
			processed = bd_recog_channel_recognize(channel,request,response);
			break;
		case RECOGNIZER_GET_RESULT:
			break;
		case RECOGNIZER_START_INPUT_TIMERS:
			processed = bd_recog_channel_timers_start(channel,request,response);
			break;
		case RECOGNIZER_STOP:
			processed = bd_recog_channel_stop(channel,request,response);
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t bd_recog_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t bd_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t bd_recog_stream_close(mpf_audio_stream_t *stream)
{
	 bd_recog_channel_t* recog_channel = stream->obj;
	return TRUE;
}

/* Raise bd START-OF-INPUT event */
static apt_bool_t bd_recog_start_of_input(bd_recog_channel_t *recog_channel)
{
	/* create START-OF-INPUT event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_START_OF_INPUT,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}
    apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] start_of_input start -----------------------------\n");
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

/* Load bd recognition result */
//static apt_bool_t bd_recog_result_load(bd_recog_channel_t *recog_channel, mrcp_message_t *message)
//{
//	FILE *file;
//	mrcp_engine_channel_t *channel = recog_channel->channel;
//	const apt_dir_layout_t *dir_layout = channel->engine->dir_layout;
//	char *file_path = apt_datadir_filepath_get(dir_layout,"result.xml",message->pool);
//	if(!file_path) {
//		return FALSE;
//	}
	
	/* read the bd result from file */
//	file = fopen(file_path,"r");
//	if(file) {
//		mrcp_generic_header_t *generic_header;
//		char text[1024];
//		apr_size_t size;
//		size = fread(text,1,sizeof(text),file);
//		apt_string_assign_n(&message->body,text,size,message->pool);
//		fclose(file);

		/* get/allocate generic header */
//		generic_header = mrcp_generic_header_prepare(message);
//		if(generic_header) {
			/* set content types */
//			apt_string_assign(&generic_header->content_type,"application/x-nlsml",message->pool);
//			mrcp_generic_header_property_add(message,GENERIC_HEADER_CONTENT_TYPE);
//		}
//	}
//	return TRUE;
//}

static apt_bool_t bd_recog_result_load(bd_recog_channel_t *recog_channel, mrcp_message_t *message)
{
	apt_str_t *body = &message->body;
	if(!recog_channel->result) {
		return FALSE;
	}

	body->buf = apr_psprintf(message->pool,
			"<?xml version=\"1.0\"?>\n"
			"<result>\n"
			"  <interpretation confidence=\"%d\">\n"
			"    <instance>%s</instance>\n"
			"    <input mode=\"speech\">%s</input>\n"
			"  </interpretation>\n"
			"</result>\n",
			99,
			recog_channel->result,
			recog_channel->result);
	if(body->buf) {
		mrcp_generic_header_t *generic_header;
		generic_header = mrcp_generic_header_prepare(message);
		if(generic_header) {
			/* set content type */
			apt_string_assign(&generic_header->content_type,"application/x-nlsml",message->pool);
			mrcp_generic_header_property_add(message,GENERIC_HEADER_CONTENT_TYPE);
		}

		body->length = strlen(body->buf);
	}
	return TRUE;
}




/* Raise bd RECOGNITION-COMPLETE event */
//识别结束
static apt_bool_t bd_recog_recognition_complete(bd_recog_channel_t *recog_channel, mrcp_recog_completion_cause_e cause)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] recognition_complete start ------------------------------------\n");
	recog_channel->status = RECOGNITION_COMPLETE;
	//xiaqingmu 发送post请求
	apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, " BD POST\r\n");
	apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, " BD POST\r\n");
	if(asr_post(recog_channel)<0) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post false -----------------------------\n");
	}else{
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post sucess -----------------------------\n");
	}
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] recognition_complete  status = RECOGNITION_COMPLETE----------------------------------\n");
	
	bd_recog_stream_recog(recog_channel, NULL, 0);
	
    //-----------------------------------
	mrcp_recog_header_t *recog_header;
	/* create RECOGNITION-COMPLETE event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_RECOGNITION_COMPLETE,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}

	/* get/allocate recognizer header */
	recog_header = mrcp_resource_header_prepare(message);
	if(recog_header) {
		/* set completion cause */
		recog_header->completion_cause = cause;
		mrcp_resource_header_property_add(message,RECOGNIZER_HEADER_COMPLETION_CAUSE);
	}
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

	if(cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS) {
		bd_recog_result_load(recog_channel,message);
	}
	//recog_channel->recog_request = NULL;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

/** Callback is called from MPF engine context to write/send new frame */
static apt_bool_t bd_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING," stream write ----------------------------------\n");
	bd_recog_channel_t *recog_channel = stream->obj;
	if(recog_channel->stop_response) {
		apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, " bd_recog_stream_recog stop_response");
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(recog_channel->channel,recog_channel->stop_response);
		recog_channel->stop_response = NULL;
		recog_channel->recog_request = NULL;
		return TRUE;
	}
    if(frame->codec_frame.size) {
		bd_recog_stream_recog(recog_channel, frame->codec_frame.buffer, frame->codec_frame.size);
	}
	if(recog_channel->recog_request) {
		mpf_detector_event_e det_event = mpf_activity_detector_process(recog_channel->detector,frame);
		switch(det_event) {
			case MPF_DETECTOR_EVENT_ACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Activity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				bd_recog_start_of_input(recog_channel);
				break;
			case MPF_DETECTOR_EVENT_INACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Inactivity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				bd_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
				break;
			case MPF_DETECTOR_EVENT_NOINPUT:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Noinput " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				if(recog_channel->timers_started == TRUE) {
					bd_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT);
				}
				break;
			default:
				break;
		}

		if(recog_channel->recog_request) {
			if((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
				if(frame->marker == MPF_MARKER_START_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Start of Event " APT_SIDRES_FMT " id:%d",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id);
				}
				else if(frame->marker == MPF_MARKER_END_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected End of Event " APT_SIDRES_FMT " id:%d duration:%d ts",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id,
						frame->event_frame.duration);
				}
			}
		}

		if(recog_channel->audio_out) {
			fwrite(frame->codec_frame.buffer,1,frame->codec_frame.size,recog_channel->audio_out);
		}
	}
	return TRUE;
}

static apt_bool_t bd_recog_msg_signal(bd_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	bd_recog_channel_t *bd_channel = channel->method_obj;
	bd_recog_engine_t *bd_engine = bd_channel->bd_engine;
	apt_task_t *task = apt_consumer_task_base_get(bd_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		bd_recog_msg_t *bd_msg;
		msg->type = TASK_MSG_USER;
		bd_msg = (bd_recog_msg_t*) msg->data;

		bd_msg->type = type;
		bd_msg->channel = channel;
		bd_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

static apt_bool_t bd_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	bd_recog_msg_t *bd_msg = (bd_recog_msg_t*)msg->data;
	switch(bd_msg->type) {
		case BD_RECOG_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(bd_msg->channel,TRUE);
			break;
		case BD_RECOG_MSG_CLOSE_CHANNEL:
		{
			/* close channel, make sure there is no activity and send asynch response */
			bd_recog_channel_t *recog_channel = bd_msg->channel->method_obj;
			if(recog_channel->audio_out) {
				fclose(recog_channel->audio_out);
				recog_channel->audio_out = NULL;
			}

			mrcp_engine_channel_close_respond(bd_msg->channel);
			break;
		}
		case BD_RECOG_MSG_REQUEST_PROCESS:
			bd_recog_channel_request_dispatch(bd_msg->channel,bd_msg->request);
			break;
		default:
			break;
	}
	return TRUE;
}
//$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$
xmlDocPtr getDocRoot(char * filename)
{
	xmlDocPtr pdoc = NULL;
	xmlKeepBlanksDefault(0);
	pdoc = xmlReadFile (filename, "UTF-8", XML_PARSE_RECOVER);
	if (pdoc == NULL)
    {
      printf ("error:can't open file!\n");
      return NULL;
	}
	return pdoc;
}
int freeDocRoot(xmlDocPtr pdoc)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] freeDocRoot start0 -----------------------------\n");
    xmlFreeDoc (pdoc);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] freeDocRoot start1 -----------------------------\n");
	xmlCleanupParser();
    apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] freeDocRoot start2 -----------------------------\n");
	xmlMemoryDump();
}
char * getCurAttribute(xmlDocPtr pdoc,char * title,char* sttstr)
{
    xmlNodePtr proot = NULL, pcur = NULL;	
	proot = xmlDocGetRootElement (pdoc);
    if (proot == NULL)
    {
	  printf ("error: file is empty!\n");
      return 0;
    }
    pcur = proot->xmlChildrenNode;
	while (pcur != NULL)
    {
		if (!xmlStrcmp(pcur->name, BAD_CAST(title)))
		{	
		  return (char *)xmlGetProp(pcur, sttstr);
		}
		pcur = pcur->next;
	}	
}
//获取token_info
void get_xml_token(xmlDocPtr pdoc,bd_recog_token_t* recog_token){
	const char *apikey = getCurAttribute(pdoc, "asr", "apikey");
	const char *secretkey = getCurAttribute(pdoc, "asr", "secretkey");
	const char *apitokenurl = getCurAttribute(pdoc, "asr", "apitokenurl");
	strcpy(recog_token->api_key,apikey);
	strcpy(recog_token->secret_key,secretkey);
	strcpy(recog_token->api_url,apitokenurl);
}

//xiugai 
void get_xml_asr(xmlDocPtr pdoc,bd_recog_channel_t* recog_channel){
	const char *apiasrurl = getCurAttribute(pdoc, "asr", "apiasrurl");
	const char *rate = getCurAttribute(pdoc, "asr", "rate");// 采样率固定值16000
	const char *format = getCurAttribute(pdoc, "asr", "format");// 格式wav
	const char *dev_pid = getCurAttribute(pdoc, "asr", "dev_pid");
	const char *cuid = getCurAttribute(pdoc, "asr", "cuid");//1234567C

	strcpy(recog_channel->url_info[0],apiasrurl);
	strcpy(recog_channel->url_info[1],rate);
	strcpy(recog_channel->url_info[2],format);
	strcpy(recog_channel->url_info[3],dev_pid);
	strcpy(recog_channel->url_info[4],cuid);
	
}
void get_token_url(bd_recog_token_t *recog_token){
	if(recog_token->token_url == NULL){
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] token_url == NULL can not sprintf\n");
		return;
	}
    sprintf(recog_token->token_url , "%s?grant_type=client_credentials&client_id=%s&client_secret=%s",recog_token->api_url,recog_token->api_key,recog_token->secret_key);
    apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] url: %s \n",recog_token->token_url);
	return;
}

//void get_asr_url(bd_recog_channel_t* recog_channel){
//	if (token == NULL){
//		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] token is NULL\n");
//		return;
//	}
//	if(recog_channel->asr_url == NULL){
//		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] asr_url == NULL can not sprintf\n");
//		return;
//	}
//    sprintf(recog_channel->asr_url, "%s?cuid=%s&token=%s&dev_pid=%d",recog_channel->url_info[0],recog_channel->url_info[4] ,token, atoi(recog_channel->url_info[3]));
//    apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] url: %s \n",recog_channel->asr_url);
//	return;
//}
size_t writefunc(void *ptr, size_t size, size_t nmemb, char **result) 
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] writefunc start-----------------\n");
    size_t result_len = size * nmemb;
    if (*result == NULL) {
        *result = (char *) malloc(result_len + 1);
    } else {
        *result = (char *) realloc(*result, result_len + 1);
    }
    if (*result == NULL) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"realloc failure!----------------\n");
        return 1;
    }
    memcpy(*result, ptr, result_len);
    (*result)[result_len] = '\0';
    return result_len;
}
//获取获取token
int get_token_post(bd_recog_token_t* recog_token){
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post start0 -----------------------------\n");
	if(recog_token->token_url == NULL){
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] token_url == NULL can not sprintf\n");
		return;
	}
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] token_url is %s\n",recog_token->token_url);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post start12333333 -----------------------------\n");
	CURLcode ret = curl_global_init(CURL_GLOBAL_ALL);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post start1 -----------------------------\n");
	CURL* curl = curl_easy_init();
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post start11 -----------------------------\n");
	if (!curl) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] curl_easy_init failed, handle nullptr\n");
    }
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post start2 -----------------------------\n");
	char *response = NULL;
    curl_easy_setopt(curl, CURLOPT_URL,recog_token->token_url); 
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60); 
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post start3 -----------------------------\n");
	CURLcode res_curl = curl_easy_perform(curl);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post start4 -----------------------------\n");
	int res = 0;
	if (res_curl != CURLE_OK) {
        apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] perform curl error:%s\n",curl_easy_strerror(res_curl));
		res = -1;
    } else {
        res = parse_token(response, token); // 解析token，结果保存在token里
        if (res == 0) {
			 start_time = time(NULL);
             apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] token: %s\n",token);
        }
    }
    free(response);
    curl_easy_cleanup(curl);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post_token end -----------------------------\n");
	return res;
}
//解析json
int parse_token(const char *response,char *token){
	struct json_object *my_obj;
	struct json_object *ret_obj;
	my_obj = json_tokener_parse(response);
    json_object_object_get_ex(my_obj,"access_token",&ret_obj);
	const char *start = json_object_to_json_string(ret_obj);
	++ start;
	char *end = strstr(start, "\"");
	int copy_size = end - start;
	snprintf(token, copy_size + 1, "%s", start);
	return 0;
}
//解析json_asr
int parse_asr(const char *response,char *buf){
	//判断err_no
  struct json_object *my_obj;
  struct json_object *ret_obj;
  my_obj = json_tokener_parse(response);
  json_object_object_get_ex(my_obj,"err_no",&ret_obj);
  const char *err_no = json_object_to_json_string(ret_obj);
  if(atoi(err_no)==0){
	  json_object_object_get_ex(my_obj,"result",&ret_obj);
	  const char * result= json_object_to_json_string(ret_obj)+'\0';
	  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] result is %s\n",result);
	  memcpy(buf,result+1,strlen(result)-2);
	  buf[strlen(buf)+1] = '\0';
	  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] result is %s\n",buf);
  }
  else{
	 buf = "one";
  }
  return 0;
}
//判断token是否过期
int is_token_overdue(bd_recog_channel_t* recog_channel){
	//一个月2419200秒
	time_t curr_time = time(NULL);
	if(curr_time - start_time >= 2419200){
		//过期重新获取token
	    //bd_recog_token_t *bd_token = (bd_recog_token_t *)malloc(sizeof(bd_recog_token_t));
	    //xmlDocPtr pdoc = getDocRoot(XML_FILENAME);
		bd_recog_token_t bd_token;
	    get_xml_token(pdoc,&bd_token);
	    get_token_url(&bd_token);
	    get_token_post(&bd_token);
	    //freeDocRoot(pdoc);
        //free(bd_token);
	}
	return 0;
}

size_t write_data(void *ptr, size_t size, size_t nmemb, void *stream){
	int written = fwrite(ptr, size, nmemb, stream);
	return written;
}
//static size_t read_callback(void *dest, size_t size, size_t nmemb, void *userp){
//	int tsize = 0;
  
//  struct bd_recog_channel_t *recog_channel = (struct bd_recog_channel_t *)userp;
 // struct WriteThis* wt = &(recog_channel->wt);
//  size_t buffer_size = size * nmemb;


//  if(!wt->bvoice){
//  	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] VOICE_DATA !wt->bvoice--------------------------------------\n");
//	if(recog_channel->status == RECOGNITION_COMPLETE){
//		if(recog_channel->bvoice_readsize == recog_channel->total_size){
//			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] RECOGNITION_COMPLETE && readsize == total_size--------------------------------------\n");
//			wt->bvoice = TRUE;
//			goto END_DATA;
//		}
//		if(recog_channel->total_size - recog_channel->bvoice_readsize <= size*nmemb){
//			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] total_size - bvoice_readsize <= size*nmemb\n");
//			memcpy(dest,recog_channel->buffer + recog_channel->bvoice_readsize, recog_channel->total_size-recog_channel->bvoice_readsize);
//			wt->bvoice = TRUE;
//			int temp = recog_channel->total_size - recog_channel->bvoice_readsize;
//			recog_channel->bvoice_readsize = recog_channel->total_size;
//			return temp;
//		}
//        else {
//			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] total_size - bvoice_readsize >= size*nmemb--------------\n");
//			memcpy(dest,recog_channel->buffer + recog_channel->bvoice_readsize, size*nmemb);
//			recog_channel->bvoice_readsize += size*nmemb;
//			return size*nmemb;
//		}
//	}
//	else{
//VOICE_START:
//	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] VOICE_START----------------------------\n");
//	pthread_mutex_lock(&(recog_channel->mutex));
//	if(recog_channel->bvoice_readsize == recog_channel->total_size){
//		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] bvoice_readsize == recog_channel->total_size   total_size:%d ,readsizeL%d--------------\n",recog_channel->total_size,recog_channel->bvoice_readsize);
//		pthread_mutex_unlock(&(recog_channel->mutex));
		
//		if(recog_channel->status == RECOGNITION_COMPLETE){
//			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] recog_channel->status == RECOGNITION_COMPLETE-----------------\n");
//			wt->bvoice = TRUE;
//			goto END_DATA;
//		}
//        sleep(1);
//		goto VOICE_START;
//	}
 //       else{
//		if(recog_channel->total_size - recog_channel->bvoice_readsize <= size*nmemb){
//			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] total_size - recog_channel->bvoice_readsize <= size*nmemb-----------------\n");
//			memcpy(dest,recog_channel->buffer+recog_channel->bvoice_readsize,recog_channel->total_size - recog_channel->bvoice_readsize);
//			int temp = recog_channel->bvoice_readsize;
//			recog_channel->bvoice_readsize = recog_channel->total_size;
//			pthread_mutex_unlock(&(recog_channel->mutex));
//			return recog_channel->total_size - temp;
//		}else{
//			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] total_size - recog_channel->bvoice_readsize >= size*nmemb-----------------\n");
//			memcpy(dest,recog_channel->buffer,size*nmemb);
//			recog_channel->bvoice_readsize += size*nmemb;
//			pthread_mutex_unlock(&(recog_channel->mutex));
//			return size*nmemb;
//		}
//            }
 //       }
//	}
//END_DATA:
 //   recog_channel->status = RECOGNITION_COMPLETE;
//	return 0; //no more data left to deliver
	
	
	
//}
static apt_bool_t bd_recog_stream_recog(bd_recog_channel_t *recog_channel, const void *voice_data, unsigned int voice_len) {
  //TODO
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] recog_stream_recog start-----------------------------\n");
  if(FALSE == recog_channel->recog_started){
	  apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[bd] start recog");
	  recog_channel->recog_started = TRUE;
  }
  
  pthread_mutex_lock(&(recog_channel->mutex));
  
  recog_channel->buffer = (unsigned char *)realloc(recog_channel->buffer,recog_channel->total_size+voice_len);
  memcpy(recog_channel->buffer+recog_channel->total_size,voice_data,voice_len);
  //存储语音流
  file = fopen("/usr/local/unimrcp/bin/1.wav","a+");
  size_t t = fwrite(voice_data,1,voice_len,file);
  fclose(file);
  
  recog_channel->total_size += voice_len;
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] recog_stream_recog mutex_unlock -----------------------------\n");
  pthread_mutex_unlock(&(recog_channel->mutex));
  //printf("bd_recog_stream_recog end , total_size: %d\n, voice_len: %d",recog_channel->total_size,voice_len);
  
}

//for http
//int asr_post(bd_recog_channel_t *recog_channel){
//	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post start -----------------------------\n");
//	CURL *curl = curl_easy_init();
//	char *cuid = curl_easy_escape(curl,recog_channel->url_info[7], strlen(recog_channel->url_info[7]));
    //判断token是否过期
 //   is_token_overdue(recog_channel);
//	free(cuid);
//	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] asr_url: %s \n",recog_channel->asr_url);
//	struct curl_slist *headerlist = NULL;
//	char header[50];
 //   snprintf(header, sizeof(header), "Content-Type: audio/%s; rate=%d", recog_channel->url_info[2],
 //            atoi(recog_channel->url_info[1]));
//    headerlist = curl_slist_append(headerlist, header);
//	FILE* fp = fopen(WAV_RESULT_FILENAME,"w");
//	if(fp == NULL){
//		apt_log(APT_LOG_MARK, APT_PRIO_WARNING,"[bd]  asr_post fp==NULL  failed ");
 //       return -1;
//	}
//   curl_easy_setopt(curl, CURLOPT_URL, recog_channel->asr_url);
//	curl_easy_setopt(curl, CURLOPT_POST, 1);
//	//curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
//	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5); // 连接5s超时
//	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60);
//	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
//	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
//	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);
//	curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
//	curl_easy_setopt(curl, CURLOPT_READDATA,(void*)recog_channel);
//	curl_easy_perform(curl);
//   curl_slist_free_all(headerlist);
//   curl_easy_cleanup(curl);
//    curl_global_cleanup();
//    fclose(fp);
//    return 0;	
//}
int asr_post(bd_recog_channel_t *recog_channel){
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post start -----------------------------\n");
  CURLcode ret = curl_global_init(CURL_GLOBAL_ALL);
  if (ret < 0) {
    return -1;
  }
  CURL* curl = curl_easy_init();
  if (!curl) {
    printf(" curl_easy_init failed, handle nullptr\n");
	return -1;
  }
  FILE* fp = fopen(WAV_RESULT_FILENAME,"w");
  if(fp == NULL){
	apt_log(APT_LOG_MARK, APT_PRIO_WARNING,"[bd]  asr_post fp==NULL  failed ");
    return -1;
  }
  if (token == NULL){
    apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] token is NULL\n");
	return;
  }
  if(recog_channel->asr_url == NULL){
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] asr_url == NULL can not sprintf\n");
	return;
  }
  char *cuid = curl_easy_escape(curl,recog_channel->url_info[4], strlen(recog_channel->url_info[4]));
  sprintf(recog_channel->asr_url, "%s?cuid=%s&token=%s&dev_pid=%d",recog_channel->url_info[0],cuid,token, atoi(recog_channel->url_info[3]));
  free(cuid);
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] asr_url: %s \n",recog_channel->asr_url);
  struct curl_slist *headerlist = NULL;
  char header[50];
  snprintf(header, sizeof(header), "Content-Type: audio/%s; rate=%d", recog_channel->url_info[2],
              atoi(recog_channel->url_info[1]));
  char * result = NULL;
  headerlist = curl_slist_append(headerlist, header);
  curl_easy_setopt(curl, CURLOPT_URL, recog_channel->asr_url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, recog_channel->buffer);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,recog_channel->total_size);
  CURLcode res_curl = curl_easy_perform(curl);
  if (res_curl != CURLE_OK) {
	  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] curl filed -----------------------------\n");
  }
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] result is %s\n",result);
  //解析json
  parse_asr(result,recog_channel->result);
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] json_result is %s\n",recog_channel->result);
  int written = fwrite(recog_channel->result, strlen(recog_channel->result), 1, fp);
  //char *result1 = " 留学回国人员引进办理流程 ";
  //recog_channel->result=" 无犯罪记录证明受理条件 ";
  //memcpy(recog_channel->result,result1,strlen(result1));
  curl_slist_free_all(headerlist);
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  fclose(fp);
  //free(result);
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[bd] post end -----------------------------\n");
  return 0;
}
