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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "mpf_buffer.h"

#include <libxml/parser.h>
#include <libxml/tree.h>



#include "mrcp_synth_engine.h"
#include "apt_consumer_task.h"
#include "apt_log.h"

#define SYNTH_ENGINE_TASK_NAME "pq Synth Engine"

typedef struct pq_synth_engine_t pq_synth_engine_t;
typedef struct pq_synth_channel_t pq_synth_channel_t;
typedef struct pq_synth_msg_t pq_synth_msg_t;

//ME
//char*  head_str;
char strTemp[1024];
static unsigned char ToHex(unsigned char x);
static char* UrlEncode(char* str);
xmlDocPtr getDocRoot(char * filename);
int freeDocRoot(xmlDocPtr pdoc);
char * getCurAttribute(xmlDocPtr pdoc,char * title,char* sttstr);



/** Declaration of synthesizer engine methods */
static apt_bool_t pq_synth_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t pq_synth_engine_open(mrcp_engine_t *engine);
static apt_bool_t pq_synth_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* pq_synth_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);
static apt_bool_t pq_synth_text_to_speech(const char* src_text, mpf_buffer_t *buffer);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	pq_synth_engine_destroy,
	pq_synth_engine_open,
	pq_synth_engine_close,
	pq_synth_engine_channel_create
};


/** Declaration of synthesizer channel methods */
static apt_bool_t pq_synth_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t pq_synth_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t pq_synth_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t pq_synth_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	pq_synth_channel_destroy,
	pq_synth_channel_open,
	pq_synth_channel_close,
	pq_synth_channel_request_process
};

/** Declaration of synthesizer audio stream methods */
static apt_bool_t pq_synth_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t pq_synth_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t pq_synth_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t pq_synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	pq_synth_stream_destroy,
	pq_synth_stream_open,
	pq_synth_stream_close,
	pq_synth_stream_read,
	NULL,
	NULL,
	NULL,
	NULL
};

/** Declaration of pq synthesizer engine */
struct pq_synth_engine_t {
	apt_consumer_task_t    *task;
};

/** Declaration of pq synthesizer channel */
struct pq_synth_channel_t {
	/** Back pointer to engine */
	pq_synth_engine_t   *pq_engine;
	/** Engine channel base */
	mrcp_engine_channel_t *channel;

	/** Active (in-progress) speak request */
	mrcp_message_t        *speak_request;
	/** Pending stop response */
	mrcp_message_t        *stop_response;
	/** Estimated time to complete */
	apr_size_t             time_to_complete;
	/** Is paused */
	apt_bool_t             paused;
	/** Speech source (used instead of actual synthesis) */

	mpf_buffer_t          *audio_buffer;
	

};

typedef enum {
	pq_SYNTH_MSG_OPEN_CHANNEL,
	pq_SYNTH_MSG_CLOSE_CHANNEL,
	pq_SYNTH_MSG_REQUEST_PROCESS
} pq_synth_msg_type_e;

/** Declaration of pq synthesizer task message */
struct pq_synth_msg_t {
	pq_synth_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};


static apt_bool_t pq_synth_msg_signal(pq_synth_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t pq_synth_msg_process(apt_task_t *task, apt_task_msg_t *msg);
static void pq_synth_on_start(apt_task_t *task);
static void pq_synth_on_terminate(apt_task_t *task);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="SYNTH-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(SYNTH_PLUGIN,"SYNTH-PLUGIN")

/** Use custom log source mark */
#define SYNTH_LOG_MARK   APT_LOG_MARK_DECLARE(SYNTH_PLUGIN)

/** Create pq synthesizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] mrcp_plugin_create");
	/* create pq engine */
	pq_synth_engine_t *pq_engine = apr_palloc(pool,sizeof(pq_synth_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	/* create task/thread to run pq engine in the context of this task */
	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(pq_synth_msg_t),pool);
	pq_engine->task = apt_consumer_task_create(pq_engine,msg_pool,pool);
	if(!pq_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(pq_engine->task);
	apt_task_name_set(task,SYNTH_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = pq_synth_msg_process;
		// vtable->on_pre_run = pq_synth_on_start;
		// vtable->on_post_run = pq_synth_on_terminate;
	}
	/* create engine base */
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] mrcp_plugin_create end");
	return mrcp_engine_create(
				MRCP_SYNTHESIZER_RESOURCE, /* MRCP resource identifier */
				pq_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy synthesizer engine */
static apt_bool_t pq_synth_engine_destroy(mrcp_engine_t *engine)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_engine_destroy");
	pq_synth_engine_t *pq_engine = engine->obj;
	if(pq_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(pq_engine->task);
		//1o
		apt_task_msg_t *msg = apt_task_msg_get(task);
		apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq pq_synth_engine_destroy]  msg: [" APT_PTR_FMT "]",msg);
		
		//apt_task_destroy(task);
		//pq_engine->task = NULL;
	}
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_engine_destroy end");
	return TRUE;
}

/** Open synthesizer engine */
static apt_bool_t pq_synth_engine_open(mrcp_engine_t *engine)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_engine_open");
	pq_synth_engine_t *pq_engine = engine->obj;
	if(pq_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(pq_engine->task);
		apt_task_start(task);
		//2t
		apt_task_msg_t *msg = apt_task_msg_get(task);
		apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq pq_synth_engine_open]  msg: [" APT_PTR_FMT "]",msg);
	}
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_engine_open end");
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close synthesizer engine */
static apt_bool_t pq_synth_engine_close(mrcp_engine_t *engine)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_engine_close");

	pq_synth_engine_t *pq_engine = engine->obj;
	if(pq_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(pq_engine->task);
		//3t
		apt_task_msg_t *msg = apt_task_msg_get(task);
		apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq pq_synth_engine_close]  msg: [" APT_PTR_FMT "]",msg);
		
		apt_task_terminate(task,TRUE);
	}
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_engine_close end");
	return mrcp_engine_close_respond(engine);
}

/** Create pq synthesizer channel derived from engine channel base */
static mrcp_engine_channel_t* pq_synth_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_engine_channel_create");

	apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[pq tts] create synthesizer channel");
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create pq synth channel */
	pq_synth_channel_t *synth_channel = apr_palloc(pool,sizeof(pq_synth_channel_t));
	synth_channel->pq_engine = engine->obj;
	synth_channel->speak_request = NULL;
	synth_channel->stop_response = NULL;
	synth_channel->time_to_complete = 0;
	synth_channel->paused = FALSE;
	synth_channel->audio_buffer = NULL;
	
	
	//synth_channel->head_str = (char* )malloc(1024);
	//gethttpsplit(getdoc,synth_channel->head_str);
	
	
	
	//4f
	
	
	capabilities = mpf_source_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			synth_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	synth_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			synth_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	synth_channel->audio_buffer = mpf_buffer_create(pool);
	
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_engine_channel_create end");
	return synth_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t pq_synth_channel_destroy(mrcp_engine_channel_t *channel)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_destroy");
	/* nothing to destroy */
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t pq_synth_channel_open(mrcp_engine_channel_t *channel)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_open");
	return pq_synth_msg_signal(pq_SYNTH_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t pq_synth_channel_close(mrcp_engine_channel_t *channel)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_close");
	return pq_synth_msg_signal(pq_SYNTH_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t pq_synth_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_request_process");
	return pq_synth_msg_signal(pq_SYNTH_MSG_REQUEST_PROCESS,channel,request);
}



/** Process SPEAK request */
static apt_bool_t synth_response_construct(mrcp_message_t *response, mrcp_status_code_e status_code, mrcp_synth_completion_cause_e completion_cause)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] synth_response_construct");
	mrcp_synth_header_t *synth_header = mrcp_resource_header_prepare(response);
	if(!synth_header) {
		return FALSE;
	}

	response->start_line.status_code = status_code;
	synth_header->completion_cause = completion_cause;
	mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] synth_response_construct end");
	return TRUE;
}

/** Process SPEAK request */
static apt_bool_t pq_synth_channel_speak(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_speak");
	apt_str_t *body;
	const char* session_begin_params = "voice_name = xiaoyan, text_encoding = utf8, sample_rate = 8000, speed = 50, volume = 50, pitch = 50, rdn = 2";
	pq_synth_channel_t *synth_channel = channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_source_stream_codec_get(channel);

	if(!descriptor) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	synth_channel->speak_request = request;
	body = &synth_channel->speak_request->body;
	if(!body->length) {
		synth_channel->speak_request = NULL;
		synth_response_construct(response,MRCP_STATUS_CODE_MISSING_PARAM,SYNTHESIZER_COMPLETION_CAUSE_ERROR);
		mrcp_engine_channel_message_send(synth_channel->channel,response);
		return FALSE;
	}

	synth_channel->time_to_complete = 0;

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);

	pq_synth_text_to_speech(body->buf, synth_channel->audio_buffer); 
	mpf_buffer_event_write(synth_channel->audio_buffer, MEDIA_FRAME_TYPE_EVENT);
	
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_speak end");
	return TRUE;
}

static APR_INLINE pq_synth_channel_t* pq_synth_channel_get(apt_task_t *task)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_get");
	apt_consumer_task_t *consumer_task = apt_task_object_get(task);
	return apt_consumer_task_object_get(consumer_task);
}

static void pq_synth_on_start(apt_task_t *task)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_on_start");
	pq_synth_channel_t *synth_channel = pq_synth_channel_get(task);
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "Speak task started"); 
	mrcp_engine_channel_open_respond(synth_channel->channel,TRUE);
}

static void pq_synth_on_terminate(apt_task_t *task)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_on_terminate");
	pq_synth_channel_t *synth_channel = pq_synth_channel_get(task);
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "Speak task terminated");
	mrcp_engine_channel_close_respond(synth_channel->channel);
}

/** Process STOP request */
static apt_bool_t pq_synth_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_stop");
	pq_synth_channel_t *synth_channel = channel->method_obj;
	/* store the request, make sure there is no more activity and only then send the response */
	synth_channel->stop_response = response;
	return TRUE;
}

/** Process PAUSE request */
static apt_bool_t pq_synth_channel_pause(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_pause");
	pq_synth_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = TRUE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process RESUME request */
static apt_bool_t pq_synth_channel_resume(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_resume");
	pq_synth_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = FALSE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process SET-PARAMS request */
static apt_bool_t pq_synth_channel_set_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_set_params");
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Age [%"APR_SIZE_T_FMT"]",
				req_synth_header->voice_param.age);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Name [%s]",
				req_synth_header->voice_param.name.buf);
		}
	}
	
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_set_params end");
	return TRUE;
}

/** Process GET-PARAMS request */
static apt_bool_t pq_synth_channel_get_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_get_params");
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		mrcp_synth_header_t *res_synth_header = mrcp_resource_header_prepare(response);
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			res_synth_header->voice_param.age = 25;
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_AGE);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_string_set(&res_synth_header->voice_param.name,"David");
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_NAME);
		}
	}

	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_get_params end");
	return TRUE;
}

/** Dispatch MRCP request */
static apt_bool_t pq_synth_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_request_dispatch");
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case SYNTHESIZER_SET_PARAMS:
			processed = pq_synth_channel_set_params(channel,request,response);
			break;
		case SYNTHESIZER_GET_PARAMS:
			processed = pq_synth_channel_get_params(channel,request,response);
			break;
		case SYNTHESIZER_SPEAK:
            apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "pq_synth_channel_speak");
			processed = pq_synth_channel_speak(channel,request,response);
			break;
		case SYNTHESIZER_STOP:
			processed = pq_synth_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_PAUSE:
			processed = pq_synth_channel_pause(channel,request,response);
			break;
		case SYNTHESIZER_RESUME:
			processed = pq_synth_channel_resume(channel,request,response);
			break;
		case SYNTHESIZER_BARGE_IN_OCCURRED:
			processed = pq_synth_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_CONTROL:
			break;
		case SYNTHESIZER_DEFINE_LEXICON:
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_channel_request_dispatch end");
	return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t pq_synth_stream_destroy(mpf_audio_stream_t *stream)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_stream_destroy");
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t pq_synth_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_stream_open");
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t pq_synth_stream_close(mpf_audio_stream_t *stream)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_stream_close");
	return TRUE;
}

/** Raise SPEAK-COMPLETE event */
static apt_bool_t pq_synth_speak_complete_raise(pq_synth_channel_t *synth_channel)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_speak_complete_raise");
	mrcp_message_t *message = 0;
	mrcp_synth_header_t * synth_header = 0;
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "[pq tts] pq_synth_speak_complete_raise");

	if (!synth_channel->speak_request) {
		return FALSE;
	}

	message = mrcp_event_create(
						synth_channel->speak_request,
						SYNTHESIZER_SPEAK_COMPLETE,
						synth_channel->speak_request->pool);
	if (!message) {
		return FALSE;
	}

	/* get/allocate synthesizer header */
	synth_header = (mrcp_synth_header_t *) mrcp_resource_header_prepare(message);
	if (synth_header) {
		/* set completion cause */
		synth_header->completion_cause = SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
		mrcp_resource_header_property_add(message,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
	}
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

	synth_channel->speak_request = NULL;
	/* send asynch event */
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_speak_complete_raise end");
	return mrcp_engine_channel_message_send(synth_channel->channel,message);
}

/** Callback is called from MPF engine context to read/get new frame */
static apt_bool_t pq_synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
	//apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_stream_read");
	// apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[pq tts] stream read");
	pq_synth_channel_t *synth_channel = stream->obj;
	/* check if STOP was requested */
	if(synth_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(synth_channel->channel,synth_channel->stop_response);
		synth_channel->stop_response = NULL;
		synth_channel->speak_request = NULL;
		synth_channel->paused = FALSE;
		return TRUE;
	}

	/* check if there is active SPEAK request and it isn't in paused state */
	if(synth_channel->speak_request && synth_channel->paused == FALSE) {
		// apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[pq tts] read audio buffer to frame");
		/* normal processing */
		mpf_buffer_frame_read(synth_channel->audio_buffer,frame);
			/* raise SPEAK-COMPLETE event */
		if((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
			frame->type &= ~MEDIA_FRAME_TYPE_EVENT;
			pq_synth_speak_complete_raise(synth_channel);
		}
	}
	//apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_stream_read end");
	return TRUE;
}

static apt_bool_t pq_synth_msg_signal(pq_synth_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_msg_signal");
	apt_bool_t status = FALSE;
	pq_synth_channel_t *pq_channel = channel->method_obj;
	pq_synth_engine_t *pq_engine = pq_channel->pq_engine;
	apt_task_t *task = apt_consumer_task_base_get(pq_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq  pq_synth_msg_signal]  msg : [" APT_PTR_FMT "]",msg);
	if(msg) {
		pq_synth_msg_t *pq_msg;
		msg->type = TASK_MSG_USER;
		pq_msg = (pq_synth_msg_t*) msg->data;

		pq_msg->type = type;
		pq_msg->channel = channel;
		pq_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_msg_signal end");
	return status;
}

static apt_bool_t pq_synth_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] spq_synth_msg_process");
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq  pq_synth_msg_process]  msg : [" APT_PTR_FMT "], task : [" APT_PTR_FMT "]",msg,task);
	pq_synth_msg_t *pq_msg = (pq_synth_msg_t*)msg->data;

	switch(pq_msg->type) {
		case pq_SYNTH_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
            apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "pq_SYNTH_MSG_OPEN_CHANNEL");
			mrcp_engine_channel_open_respond(pq_msg->channel,TRUE);
			break;
		case pq_SYNTH_MSG_CLOSE_CHANNEL:
			/* close channel, make sure there is no activity and send asynch response */
            apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "pq_SYNTH_MSG_CLOSE_CHANNEL");
			mrcp_engine_channel_close_respond(pq_msg->channel);
			break;
		case pq_SYNTH_MSG_REQUEST_PROCESS:

            apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "pq_SYNTH_MSG_REQUEST_PROCESS");
		    pq_synth_channel_request_dispatch(pq_msg->channel,pq_msg->request);
			break;
		default:
            apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "Default  pq_synth_msg_process");
			break;
	}
    //apt_log(APT_LOG_MARK, " ")
    apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"[pq  pq_synth_msg_process]  msg : [" APT_PTR_FMT "]", msg);
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] pq_synth_msg_process end");
	return TRUE;
}
//@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
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
    xmlFreeDoc (pdoc);
	xmlCleanupParser();
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

//for http write_data
size_t write_data(void *ptr, size_t size, size_t nmemb, void *stream)
{
        printf("size:%d,nmemb:%d\n",size,nmemb);
        int nret = fwrite(ptr, size, nmemb, (FILE*)(stream));
        return nret;
}
static unsigned char ToHex(unsigned char x)
{
        return  x > 9 ? x + 55 : x + 48;
}


static char* UrlEncode(char* str)
{
        size_t strTemp_len = 0;
        size_t i;
        size_t length = strlen(str);
        printf("length: %d\n",length);
        for (i = 0; i < length; i++)
        {
                if (isalnum((unsigned char)str[i]) ||
                                (str[i]=='-') ||
                                (str[i]=='_') ||
                                (str[i]=='.') ||
                                (str[i]=='~')){
                        strTemp[strTemp_len++] = str[i];
                }
                else if (str[i]==' ') {
                        strTemp[strTemp_len++] = '+';
                }
                else
                {
                        strTemp[strTemp_len++] = '%';
                        strTemp[strTemp_len++] = ToHex((unsigned char)str[i] >> 4);
                        strTemp[strTemp_len++] = ToHex((unsigned char)str[i] % 16);
                }
        }
		apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq UrlEncode] strTemp_len : %d",strTemp_len);
        return strTemp;
}
static apt_bool_t get(char *url,const char* filename)
{
	CURL *curl = curl_easy_init(); 
	FILE *fp = fopen(filename,"wb");
	CURLcode res;
    if (fp == NULL)  //返回结果用文件二进制存储
        return FALSE;
    curl = curl_easy_init();    // 初始化
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL,url);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp); //将返回的http头输出到fp指向的文件
        res = curl_easy_perform(curl);   // 执行
	}
	curl_easy_cleanup(curl);
	if(res != CURLE_OK)
		{
			switch (res)
			{
				case CURLE_UNSUPPORTED_PROTOCOL:
					apt_log(APT_LOG_MARK, APT_PRIO_WARNING,"[pq get] 不支持的协议,由URL的头部指定");
				case CURLE_COULDNT_CONNECT:
					apt_log(APT_LOG_MARK, APT_PRIO_WARNING,"[pq get] 不能连接到remote主机或者代理");
				case CURLE_HTTP_RETURNED_ERROR:
					apt_log(APT_LOG_MARK, APT_PRIO_WARNING,"[pq get] http返回错误");
				case CURLE_READ_ERROR:
					apt_log(APT_LOG_MARK, APT_PRIO_WARNING,"[pq get] 读本地文件错误");
				default:
					apt_log(APT_LOG_MARK, APT_PRIO_WARNING,"[pq get] 返回值: %d",res);
			}
		return FALSE;
		}
		fclose(fp);
		return TRUE;
}
static apt_bool_t pq_synth_text_to_speech(const char* src_text, mpf_buffer_t *buffer)
{

        apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq pq_synth_text_to_speech] 开始合成");
        apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] src_text : %s", src_text);
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "[pq] buffer : %ld", buffer);
	

	FILE *t = NULL;
        void *data = NULL;
	
	
	
	//系统时间&&生成url
	time_t now = time(0);
	const char* src_text_copy = "王文泽你好";
	unsigned int audio_len =(unsigned int)strlen(src_text);//28084 8086 

    //xml解析
	xmlDocPtr pdoc = getDocRoot("/usr/local/unimrcp/conf/synth.xml");
	char* filename = getCurAttribute(pdoc,"file","filename");
	//url解码&存入全局strTemp
	UrlEncode(src_text);
	char* url = (char*)malloc(strlen(strTemp)+200);
	sprintf(url,"http://%s:%s/voice/tts?text=%s&format=%s&sample_rate=%s&volume=%s&speed=%s&pitch=%s&voice_name=%s&time=%ld",getCurAttribute(pdoc,"synth","host"),getCurAttribute(pdoc,"synth","port"),strTemp,getCurAttribute(pdoc,"synth","format"),getCurAttribute(pdoc,"synth","sample_rate"),getCurAttribute(pdoc,"synth","volume"),getCurAttribute(pdoc,"synth","speed"),getCurAttribute(pdoc,"synth","pitch"),getCurAttribute(pdoc,"synth","voice_name"),now);
        freeDocRoot(pdoc);
	
	//const char* url_short = "http://1.202.136.28:28084/voice/tts?text=%s&format=wav&sample_rate=16000&volume=1&speed=1&pitch=1&voice_name=xiaochang&time=%ld";
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq pq_synth_text_to_speech] url : %s",url);
	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq pq_synth_text_to_speech] strTemp : %s",strTemp);
	
	if(get(url,filename) == TRUE){
		apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] get成功");
        t = fopen(filename,"rb");
        fseek(t,0L,SEEK_END);
        audio_len=ftell(t);
        apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"[pq] audio_len值 ,  %d.",audio_len);
        fseek(t,0L,SEEK_SET);
        data = malloc(audio_len+10);
        size_t size = fread(data, 1, audio_len, t);
        apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"[pq] data大小 ,  %d.",size);
	}else
		apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] get失败");
	
	if(NULL != data)
		{
			mpf_buffer_audio_write(buffer, data, audio_len);
			apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] mpf_buffer_audio_write sucess");
			free(data);
		}
	else{
		apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq] mpf_buffer_audio_write failed");
		free(data);
	}
    apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,"[pq pq_synth_text_to_speech] 结束");
	//回收&&并且将strTemp清空
	memset(strTemp,'\0',sizeof(strTemp));
	free(url);
    return TRUE;
}
