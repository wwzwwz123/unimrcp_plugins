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
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <json/json.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlmemory.h>
#include <libxml/xpointer.h>



#include "mrcp_recog_engine.h"
#include "mpf_activity_detector.h"
#include "apt_consumer_task.h"
#include "apt_log.h"

#define RECOG_ENGINE_TASK_NAME "PQ Recog Engine"
#define XML_FILENAME "/usr/local/unimrcp/conf/recog.xml"
#define NLP_XML_FILENAME "/usr/local/unimrcp/bin/nlp-result.rep"
FILE* file = NULL;
typedef struct pq_recog_engine_t pq_recog_engine_t;
typedef struct pq_recog_channel_t pq_recog_channel_t;
typedef struct pq_recog_msg_t pq_recog_msg_t;



typedef struct WriteThis WriteThis;
static const struct WriteThis {
  apt_bool_t bhead;
  apt_bool_t bvoice;
  apt_bool_t bend;
};

typedef enum{
  RECOGNITION_COMPLETE,
  RECOGNITION_INPUT
}stream_status;
static apt_bool_t pq_recog_start_of_input(pq_recog_channel_t *recog_channel);

//ME
xmlDocPtr getDocRoot(char * filename);
int freeDocRoot(xmlDocPtr pdoc);
void httpSplit(xmlDocPtr pdoc,char *httpstr);
char * getCurAttribute(xmlDocPtr pdoc,char * title,char* sttstr);
xmlXPathObjectPtr getNodeset(xmlDocPtr pdoc,const xmlChar *xpath);
void get_value(char* value,pq_recog_channel_t *recog_channel);
void nlp_post(pq_recog_channel_t *recog_channel);
const char* get_json_nlp(FILE* fp);





/** Declaration of recognizer engine methods */
static apt_bool_t pq_recog_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t pq_recog_engine_open(mrcp_engine_t *engine);
static apt_bool_t pq_recog_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* pq_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);
static int post(pq_recog_channel_t *recog_channel);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	pq_recog_engine_destroy,
	pq_recog_engine_open,
	pq_recog_engine_close,
	pq_recog_engine_channel_create
};


/** Declaration of recognizer channel methods */
static apt_bool_t pq_recog_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t pq_recog_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t pq_recog_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t pq_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	pq_recog_channel_destroy,
	pq_recog_channel_open,
	pq_recog_channel_close,
	pq_recog_channel_request_process
};

/** Declaration of recognizer audio stream methods */
static apt_bool_t pq_recog_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t pq_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t pq_recog_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t pq_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);

static apt_bool_t pq_recog_stream_recog(pq_recog_channel_t *recog_channel, const void *voice_data,  unsigned int voice_len); 

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	pq_recog_stream_destroy,
	NULL,
	NULL,
	NULL,
	pq_recog_stream_open,
	pq_recog_stream_close,
	pq_recog_stream_write,
	NULL
};

/** Declaration of pq recognizer engine */
struct pq_recog_engine_t {
	apt_consumer_task_t    *task;
};

/** Declaration of pq recognizer channel */
struct pq_recog_channel_t {
	/** Back pointer to engine */
	pq_recog_engine_t     *pq_engine;
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
	

    const char *            session_id;
    const char*             last_result;
    apt_bool_t              recog_started;

	char                    *head_str;
	char                    *end_str;
  	const char 				*url;
  	const char 				*host;
  	const char 				*filename;
	int                     bhead_sizeleft;
	int                     bvoice_readsize;
	int                     bend_sizeleft;
	const char              *result_filename;
	int                     total_size;
    unsigned char*          buffer;
	WriteThis               wt;
	pthread_mutex_t         mutex;
	stream_status           status;
	const char*             data_nlp;
       // pthread_t               post_thread;
	
	
};

typedef enum {
	PQ_RECOG_MSG_OPEN_CHANNEL,
	PQ_RECOG_MSG_CLOSE_CHANNEL,
	PQ_RECOG_MSG_REQUEST_PROCESS
} pq_recog_msg_type_e;

/** Declaration of pq recognizer task message */
struct pq_recog_msg_t {
	pq_recog_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};

static apt_bool_t pq_recog_msg_signal(pq_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t pq_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg);

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

/** Create pq recognizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	pq_recog_engine_t *pq_engine = apr_palloc(pool,sizeof(pq_recog_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(pq_recog_msg_t),pool);
	pq_engine->task = apt_consumer_task_create(pq_engine,msg_pool,pool);
	if(!pq_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(pq_engine->task);
	apt_task_name_set(task,RECOG_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = pq_recog_msg_process;
	}

	/* create engine base */
	return mrcp_engine_create(
				MRCP_RECOGNIZER_RESOURCE,  /* MRCP resource identifier */
				pq_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy recognizer engine */
static apt_bool_t pq_recog_engine_destroy(mrcp_engine_t *engine)
{
	pq_recog_engine_t *pq_engine = engine->obj;
	if(pq_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(pq_engine->task);
		apt_task_destroy(task);
		pq_engine->task = NULL;
	}
	return TRUE;
}

/** Open recognizer engine */
static apt_bool_t pq_recog_engine_open(mrcp_engine_t *engine)
{
	pq_recog_engine_t *pq_engine = engine->obj;
	if(pq_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(pq_engine->task);
		apt_task_start(task);
	}
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close recognizer engine */
static apt_bool_t pq_recog_engine_close(mrcp_engine_t *engine)
{
	pq_recog_engine_t *pq_engine = engine->obj;
	if(pq_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(pq_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

static mrcp_engine_channel_t* pq_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create pq recog channel */
	pq_recog_channel_t *recog_channel = apr_palloc(pool,sizeof(pq_recog_channel_t));
	recog_channel->pq_engine = engine->obj;
	recog_channel->recog_request = NULL;
	recog_channel->stop_response = NULL;
	recog_channel->detector = mpf_activity_detector_create(pool);
	
	recog_channel->audio_out = NULL;
	recog_channel->session_id = NULL;
	recog_channel->last_result = NULL;
	recog_channel->recog_started = FALSE;
	recog_channel->buffer = NULL;
	recog_channel->wt.bend = FALSE;
	recog_channel->wt.bhead = FALSE;
	recog_channel->wt.bvoice = FALSE;
	
	
	xmlDocPtr pdoc = getDocRoot(XML_FILENAME);
	
	recog_channel->filename = getCurAttribute(pdoc,"asr","filename");
	recog_channel->url = getCurAttribute(pdoc,"asr","url");
	recog_channel->host= getCurAttribute(pdoc,"asr","host");
	recog_channel->result_filename= getCurAttribute(pdoc,"recog_result","filename");
	
	//recog_channel->head_str = "------------V2ymHFg03ehbqgZCaKO6jy\r\nContent-Disposition: form-data;name=\"/root/tts.wav\";opcode=\"transcribe_audio\";sessionid=\"session:1\";tmp_entry_id=\"2107512132\";filename=\"/root/tts.wav\";type=\"11\";time=\"1528968385149\";reqid=\"485969d2-0c93-42cd-bcd5-4f3c1ccabccb\";latitude=\"-1\";location=\"-1\";language=\"chinese\";uId=\"null\";kId=\"102\";aId=\"TEST-APP-ID\";grammarname=\"\";contentId=\"session:1\";sceneId=\"-1\";sr=\"1\";isAddPunct=\"off\";isTransDigit=\"on\";isButterFly=\"off\";\r\n\r\n";
	//recog_channel->end_str = "\r\n------------V2ymHFg03ehbqgZCaKO6jy--\r\n";
	//recog_channel->head_str = (char*)malloc(1024);
	recog_channel->head_str = (char*)malloc(1024);
	recog_channel->end_str = (char*)malloc(100);
	httpSplit(pdoc,recog_channel->head_str);
	sprintf(recog_channel->end_str,"\r\n%s--\r\n",getCurAttribute(pdoc,"httpheads","head_str"));
	
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] head_str:%s,end_str:%s++++++++++++++++++++++++++++++\n",recog_channel->head_str,recog_channel->end_str);
	freeDocRoot(pdoc);
	
	
	
	
	
	recog_channel->bhead_sizeleft = strlen(recog_channel->head_str);
	recog_channel->bvoice_readsize = 0;
	recog_channel->bend_sizeleft = strlen(recog_channel->end_str);
	recog_channel->total_size = 0;
    recog_channel->data_nlp = "{\"endingId\": \"b04c10ce0bd65c77082b39a2cc6b02d4460c94766a31b01b8dea\",\"isNew\": 1,\"text\": \"%s\", \"time\": 1542696677,\"extra\": {\"user\" :{\"company\": \"dianxin\",\"name\": \"kexin\"}}}";//eventId:[\"answerCall\"]
	pthread_mutex_init(&recog_channel->mutex, NULL);
	recog_channel->status = RECOGNITION_INPUT;
	
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
static apt_bool_t pq_recog_channel_destroy(mrcp_engine_channel_t *channel)
{
	/* nothing to destrtoy */
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t pq_recog_channel_open(mrcp_engine_channel_t *channel)
{
	return pq_recog_msg_signal(PQ_RECOG_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t pq_recog_channel_close(mrcp_engine_channel_t *channel)
{
	return pq_recog_msg_signal(PQ_RECOG_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t pq_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	return pq_recog_msg_signal(PQ_RECOG_MSG_REQUEST_PROCESS,channel,request);
}

/** Process RECOGNIZE request */
static apt_bool_t pq_recog_channel_recognize(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process RECOGNIZE request */
	mrcp_recog_header_t *recog_header;
	pq_recog_channel_t *recog_channel = channel->method_obj;
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
        //TODO
        
        recog_channel->last_result = NULL;
        recog_channel->recog_started = FALSE;

	recog_channel->recog_request = request;
	//TODO  recog
	return TRUE;
}

/** Process STOP request */
static apt_bool_t pq_recog_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process STOP request */
	pq_recog_channel_t *recog_channel = channel->method_obj;
	/* store STOP request, make sure there is no more activity and only then send the response */
	recog_channel->stop_response = response;
	return TRUE;
}

/** Process START-INPUT-TIMERS request */
static apt_bool_t pq_recog_channel_timers_start(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	pq_recog_channel_t *recog_channel = channel->method_obj;
	recog_channel->timers_started = TRUE;
	return mrcp_engine_channel_message_send(channel,response);
}

/** Dispatch MRCP request */
static apt_bool_t pq_recog_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
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
			processed = pq_recog_channel_recognize(channel,request,response);
			break;
		case RECOGNIZER_GET_RESULT:
			break;
		case RECOGNIZER_START_INPUT_TIMERS:
			processed = pq_recog_channel_timers_start(channel,request,response);
			break;
		case RECOGNIZER_STOP:
			processed = pq_recog_channel_stop(channel,request,response);
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
static apt_bool_t pq_recog_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t pq_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
        pq_recog_channel_t* recog_channel = stream->obj;
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t pq_recog_stream_close(mpf_audio_stream_t *stream)
{
        pq_recog_channel_t* recog_channel = stream->obj;
	return TRUE;
}



/* Load pq recognition result */
static apt_bool_t pq_recog_result_load(pq_recog_channel_t *recog_channel, mrcp_message_t *message)
{
	apt_str_t *body = &message->body;
	if(!recog_channel->last_result) {
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
			recog_channel->last_result,
			recog_channel->last_result);
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

void pq_recog_end_session(pq_recog_channel_t *recog_channel){
	if(recog_channel->session_id) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[pq] QISRSessionEnd suceess!");
		recog_channel->session_id = NULL;
	}
}

/* Raise pq RECOGNITION-COMPLETE event */
static apt_bool_t pq_recog_recognition_complete(pq_recog_channel_t *recog_channel, mrcp_recog_completion_cause_e cause)
{
    apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] recognition_complete start ------------------------------------\n");

	recog_channel->status = RECOGNITION_COMPLETE;
    //NLP
	nlp_post(recog_channel);
	//获取last_result
	FILE* fp = fopen("/usr/local/unimrcp/bin/nlp-result.rep","r");
	recog_channel->last_result = get_json_nlp(fp);
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] head_str end_str shi fang  ------------------------------------\n");
	
	
	apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, " PQ POST\r\n");
	if(post(recog_channel)<0) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] post false -----------------------------\n");
	}else{
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] post sucess -----------------------------\n");
	}


	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] recognition_complete  status = RECOGNITION_COMPLETE----------------------------------\n");
	

	pq_recog_stream_recog(recog_channel, NULL, 0);
	pq_recog_end_session(recog_channel);


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
		pq_recog_result_load(recog_channel,message);
	}

	recog_channel->recog_request = NULL;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}
	

/** Callback is called from MPF engine context to write/send new frame */
static apt_bool_t pq_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING," stream write ----------------------------------\n");
        //apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, " pq_recog_stream_write ");
	pq_recog_channel_t *recog_channel = stream->obj;
	if(recog_channel->stop_response) {
                apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, " pq_recog_stream_recog stop_response");
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(recog_channel->channel,recog_channel->stop_response);
		recog_channel->stop_response = NULL;
		recog_channel->recog_request = NULL;
		return TRUE;
	}
    if(frame->codec_frame.size) {
		pq_recog_stream_recog(recog_channel, frame->codec_frame.buffer, frame->codec_frame.size);
	}
	if(recog_channel->recog_request) {
		mpf_detector_event_e det_event = mpf_activity_detector_process(recog_channel->detector,frame);
		switch(det_event) {
			case MPF_DETECTOR_EVENT_ACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Activity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				pq_recog_start_of_input(recog_channel);
				break;
			case MPF_DETECTOR_EVENT_INACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Inactivity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				pq_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
				break;
			case MPF_DETECTOR_EVENT_NOINPUT:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Noinput " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				if(recog_channel->timers_started == TRUE) {
					pq_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT);
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

static apt_bool_t pq_recog_msg_signal(pq_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	pq_recog_channel_t *pq_channel = channel->method_obj;
	pq_recog_engine_t *pq_engine = pq_channel->pq_engine;
	apt_task_t *task = apt_consumer_task_base_get(pq_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		pq_recog_msg_t *pq_msg;
		msg->type = TASK_MSG_USER;
		pq_msg = (pq_recog_msg_t*) msg->data;

		pq_msg->type = type;
		pq_msg->channel = channel;
		pq_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

static apt_bool_t pq_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	pq_recog_msg_t *pq_msg = (pq_recog_msg_t*)msg->data;
	switch(pq_msg->type) {
		case PQ_RECOG_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(pq_msg->channel,TRUE);
			break;
		case PQ_RECOG_MSG_CLOSE_CHANNEL:
		{
			/* close channel, make sure there is no activity and send asynch response */
			pq_recog_channel_t *recog_channel = pq_msg->channel->method_obj;
			if(recog_channel->audio_out) {
				fclose(recog_channel->audio_out);
				recog_channel->audio_out = NULL;
			}

			mrcp_engine_channel_close_respond(pq_msg->channel);
			break;
		}
		case PQ_RECOG_MSG_REQUEST_PROCESS:
			pq_recog_channel_request_dispatch(pq_msg->channel,pq_msg->request);
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
void httpSplit(xmlDocPtr pdoc,char *httpstr)
{
	char * head_str = getCurAttribute(pdoc,"httpheads","head_str");
	char * disposition = getCurAttribute(pdoc,"httpheads","disposition");
	char * name = getCurAttribute(pdoc,"httpheads","name");
	char * opcode = getCurAttribute(pdoc,"httpheads","opcode");
	char * sessionid = getCurAttribute(pdoc,"httpheads","sessionid");
	char * tmp_entry_id = getCurAttribute(pdoc,"httpheads","tmp_entry_id");
	char * type = getCurAttribute(pdoc,"httpheads","type");
	char * time = getCurAttribute(pdoc,"httpheads","time");
	char * reqid = getCurAttribute(pdoc,"httpheads","reqid");
	char * latitude = getCurAttribute(pdoc,"httpheads","latitude");
	char * location = getCurAttribute(pdoc,"httpheads","location");
	char * language = getCurAttribute(pdoc,"httpheads","language");
	char * uId = getCurAttribute(pdoc,"httpheads","uId");
	char * kId = getCurAttribute(pdoc,"httpheads","kId");
	char * aId = getCurAttribute(pdoc,"httpheads","aId");
	char * grammarname = getCurAttribute(pdoc,"httpheads","grammarname");
	char * contentId = getCurAttribute(pdoc,"httpheads","contentId");
	char * sceneId = getCurAttribute(pdoc,"httpheads","sceneId");
	char * sr = getCurAttribute(pdoc,"httpheads","sr");
	char * isAddPunct = getCurAttribute(pdoc,"httpheads","isAddPunct");
	char * isTransDigit = getCurAttribute(pdoc,"httpheads","isTransDigit");
	char * isButterFly = getCurAttribute(pdoc,"httpheads","isButterFly");	
		
	sprintf(httpstr, "%s\r\n%s;name=\"%s\";opcode=\"%s\";sessionid=\"%s\";tmp_entry_id=\"%s\";filename=\"%s\";type=\"%s\";time=\"%s\";reqid=\"%s\";latitude=\"%s\";location=\"%s\";language=\"%s\";uId=\"%s\";kId=\"%s\";aId=\"%s\";grammarname=\"%s\";contentId=\"%s\";sceneId=\"%s\";sr=\"%s\";isAddPunct=\"%s\";isTransDigit=\"%s\";isButterFly=\"%s\";\r\n\r\n", 
	        head_str,disposition,name,opcode,sessionid,tmp_entry_id,name,type,time,reqid,latitude,location,language,uId,kId,aId,grammarname,
			contentId,sceneId,sr,isAddPunct,isButterFly,isButterFly);
		return;
}
//for http write_data
size_t write_data(void *ptr, size_t size, size_t nmemb, void *stream)
{
    printf(" write_data size %u  nmemb %u\r\n", size, nmemb);
    int written = fwrite(ptr, size, nmemb, stream);
    return written;
}
//for http read_callback
static size_t read_callback(void *dest, size_t size, size_t nmemb, void *userp)
{
  int tsize = 0;
  
  struct pq_recog_channel_t *recog_channel = (struct pq_recog_channel_t *)userp;
  struct WriteThis* wt = &(recog_channel->wt);
  size_t buffer_size = size * nmemb;
  
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] read_callback start-----------------------head_str:%s\n",recog_channel->head_str);

  if(!wt->bhead) {
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] !wt->bhead--------------------------------------\n");
  	if(recog_channel->bhead_sizeleft == 0){
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] wt->bhead = TRUE go to VOICE_DATA--------------------------------------\n");
    	wt->bhead = TRUE;
		goto VOICE_DATA;
  	}
	if(recog_channel->bhead_sizeleft <= size*nmemb){
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] recog_channel->bhead_sizeleft <= size*nmemb--------------------------------------\n");
		memcpy(dest,recog_channel->head_str,recog_channel->bhead_sizeleft);
		wt->bhead = TRUE;
		return recog_channel->bhead_sizeleft;
	}else{
		memcpy(dest,recog_channel->buffer,size*nmemb);
		recog_channel->head_str += size*nmemb;
		recog_channel->bhead_sizeleft -= size*nmemb;
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] recog_channel->bhead_sizeleft >= size*nmemb--------------------------------------\n");
		printf("bhead_sizeleft: %d\n",recog_channel->bhead_sizeleft);
		return size*nmemb;
	}
  }
  VOICE_DATA:
  if(!wt->bvoice){
  	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] VOICE_DATA !wt->bvoice--------------------------------------\n");
	if(recog_channel->status == RECOGNITION_COMPLETE){
		if(recog_channel->bvoice_readsize == recog_channel->total_size){
			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] RECOGNITION_COMPLETE && readsize == total_size--------------------------------------\n");
			wt->bvoice = TRUE;
			goto END_DATA;
		}
		if(recog_channel->total_size - recog_channel->bvoice_readsize <= size*nmemb){
			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] total_size - bvoice_readsize <= size*nmemb\n");
			memcpy(dest,recog_channel->buffer + recog_channel->bvoice_readsize, recog_channel->total_size-recog_channel->bvoice_readsize);
			wt->bvoice = TRUE;
			recog_channel->bvoice_readsize - recog_channel->total_size;
			return recog_channel->total_size - recog_channel->bvoice_readsize;
		}
                else {
			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] total_size - bvoice_readsize >= size*nmemb--------------\n");
			memcpy(dest,recog_channel->buffer + recog_channel->bvoice_readsize, size*nmemb);
			recog_channel->bvoice_readsize += size*nmemb;
			return size*nmemb;
		}
	}
	else{
VOICE_START:
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] VOICE_START----------------------------\n");
	pthread_mutex_lock(&(recog_channel->mutex));
	if(recog_channel->bvoice_readsize == recog_channel->total_size){
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] bvoice_readsize == recog_channel->total_size   total_size:%d ,readsizeL%d--------------\n",recog_channel->total_size,recog_channel->bvoice_readsize);
		pthread_mutex_unlock(&(recog_channel->mutex));
		
		if(recog_channel->status == RECOGNITION_COMPLETE){
			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] recog_channel->status == RECOGNITION_COMPLETE-----------------\n");
			wt->bvoice = TRUE;
			goto END_DATA;
		}
                sleep(1);
		goto VOICE_START;
	}
        else{
		if(recog_channel->total_size - recog_channel->bvoice_readsize <= size*nmemb){
			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] total_size - recog_channel->bvoice_readsize <= size*nmemb-----------------\n");
			memcpy(dest,recog_channel->buffer+recog_channel->bvoice_readsize,recog_channel->total_size - recog_channel->bvoice_readsize);
			int temp = recog_channel->bvoice_readsize;
			recog_channel->bvoice_readsize = recog_channel->total_size;
			pthread_mutex_unlock(&(recog_channel->mutex));
			return recog_channel->total_size - temp;
		}else{
			apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] total_size - recog_channel->bvoice_readsize >= size*nmemb-----------------\n");
			memcpy(dest,recog_channel->buffer,size*nmemb);
			recog_channel->bvoice_readsize += size*nmemb;
			pthread_mutex_unlock(&(recog_channel->mutex));
			return size*nmemb;
		}
            }
          }
	}
END_DATA:
	
	if(!wt->bend){
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] END_DATA && !wt->bend-----------------\n");
		memcpy(dest,recog_channel->end_str,strlen(recog_channel->end_str));
		recog_channel->bend_sizeleft = 0;
		wt->bend = TRUE;
		return strlen(recog_channel->end_str);
	}
	return 0; //no more data left to deliver	
}


//for http
static int post(pq_recog_channel_t *recog_channel){
	
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] post start -----------------------------\n");
  /* CURLcode ret = curl_global_init(CURL_GLOBAL_ALL);
  if (ret < 0) {
    printf(" curl_global_init failed ret %d\r\n", ret); 
    return ret;
  } */
  //CURLM *multi = curl_multi_init();
  
  FILE* fp = fopen(recog_channel->result_filename,"w");
  CURL* curl = curl_easy_init();
  if (!curl) {
    printf(" curl_easy_init failed, handle nullptr");
  }
  //拼接host
  char* host_short = "Host: %s";
  char* host_long  = malloc(strlen(host_short)+strlen(recog_channel->host)+5);
  sprintf(host_long , host_short , recog_channel->host);
 
 
  struct curl_slist *chunk = NULL;
  chunk = curl_slist_append(chunk, "Content-Type: multipart/form-data;boundary=----------V2ymHFg03ehbqgZCaKO6jy");
  chunk = curl_slist_append(chunk, "Transfer-Encoding: chunked"); 
  chunk = curl_slist_append(chunk, "Connection: keep-alive");
  chunk = curl_slist_append(chunk, "Expect:"); 
  //chunk = curl_slist_append(chunk, host_long); //"Host: 1.202.136.28:1480"
  chunk = curl_slist_append(chunk, "apikey: 8e306cd0195da10795db96f911a3cf5411965941a54f634a5314e031b504193a80f1f5c50cf3f6225cc9fe63efa9f6bb51ba1ac2d82a2ab35c3e99dae2a8c6fc0a9eb647e01e6bb07e3eab48539df3a7b7e8cf6be74caf64ff5e28d94e973f7da48c5c38937f965080bdd28b64d72e52ddbd23cd260569777c4717e70f15cb06"); 
  chunk = curl_slist_append(chunk, "Cache-Control: no-cache");
  chunk = curl_slist_append(chunk, "Pragma: no-cache");
  chunk = curl_slist_append(chunk, "Accept: text/html, image/gif, image/jpeg, *; q=.2, */*; q=.2");
  chunk = curl_slist_append(chunk, "User-Agent: Java/1.x");

  curl_easy_setopt(curl, CURLOPT_URL, recog_channel->url);//"http://1.202.136.28:1480/QianYuSrv/uploader?aaa=1"
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  //curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
  //setup data 
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
  curl_easy_setopt(curl, CURLOPT_READDATA,(void*)recog_channel);
  //curl_easy_setopt(curl, CURLOPT_NOSIGNAL,1L);
  //单线程
  curl_easy_perform(curl);

  curl_easy_cleanup(curl);
  curl_global_cleanup();
  curl_slist_free_all(chunk); 
  //curl_slist_free_all(chunk);
  //多线程
  //int running_handle = 3; 
  //curl_multi_add_handle(multi, curl);
  //curl_multi_perform(multi,&running_handle);
  //curl_multi_remove_handle(multi,curl);
  //curl_easy_cleanup(curl);
  //curl_multi_cleanup(multi);
 
 
  //curl_easy_cleanup(curl);
  //curl_global_cleanup();
 
  
  //fclose(wt->fp);
  //fclose(fp);
  fclose(fp);
  free(host_long);
  free(recog_channel->head_str);
  free(recog_channel->end_str);
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] post end ------------------------------------\n");
  return 0;
}



//
static apt_bool_t pq_recog_stream_recog(pq_recog_channel_t *recog_channel, const void *voice_data, unsigned int voice_len) {
  //TODO
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] recog_stream_recog start-----------------------------\n");
  if(FALSE == recog_channel->recog_started){
	  apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"[pq] start recog");
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
  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] recog_stream_recog mutex_unlock -----------------------------\n");
  pthread_mutex_unlock(&(recog_channel->mutex));
  //printf("pq_recog_stream_recog end , total_size: %d\n, voice_len: %d",recog_channel->total_size,voice_len);
  
}

//
static apt_bool_t pq_recog_start_of_input(pq_recog_channel_t *recog_channel)
{
	/* create START-OF-INPUT event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_START_OF_INPUT,
						recog_channel->recog_request->pool);
	
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] start_of_input start -----------------------------\n");
	

	
	if(!message) {
		return FALSE;
	}

	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}



//NLP
static size_t write_data_nlp(void *ptr, size_t size, size_t nmemb, void *stream)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] write_data_nlp size %u nmemb %u\n",size,nmemb);
    int written = fwrite(ptr, size, nmemb, stream);
    return written;
}

void nlp_post(pq_recog_channel_t *recog_channel){
	
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] nlp_post start");
	FILE* fp = fopen(NLP_XML_FILENAME,"w");

//sprintf
    char* value = (char*)malloc(1024);
    get_value(value,recog_channel);
    char* data_all = (char*)malloc(strlen(recog_channel->data_nlp)+strlen(value)+10);
    sprintf(data_all,recog_channel->data_nlp,value);
	
	apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] nlp_post data_all:%s , value : %s \n",data_all,value);
    free(value);
    CURL* curl = curl_easy_init();
    if (!curl) {
        printf(" curl_easy_init failed, handle nullptr");
    }

    struct curl_slist *chunk = NULL;
    chunk = curl_slist_append(chunk, "Content-Type: application/json;charset=UTF-8");
    curl_easy_setopt(curl, CURLOPT_URL, "http://39.98.248.75:8080/DMService/api/v2/parser?appKey=7804fe8d41d04028bbb6579fa157cbab&userId=1");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    //curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data_nlp);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data_all);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(data_all));
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    fclose(fp);
	free(data_all);
    return 0;
}


xmlXPathObjectPtr getNodeset(xmlDocPtr pdoc,const xmlChar *xpath)
{
    xmlXPathContextPtr context=NULL;//XPath上下文指针
    xmlXPathObjectPtr result=NULL; //XPath结果指针
    context = xmlXPathNewContext(pdoc);

    if(pdoc==NULL){
	    apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] pdoc is NULL\n");
        return NULL;
    }

    if(xpath){
    if (context == NULL) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] context is NULL\n");
        return NULL;
    }

    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context); //释放上下文指针
    if (result == NULL) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] xmlXPathEvalExpression return NULL\n");
        return NULL;
    }

    if (xmlXPathNodeSetIsEmpty(result->nodesetval)) {
        xmlXPathFreeObject(result);
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] nodeset is empty\n");
        return NULL;
    }
}
    return result;
}


void get_value(char* value,pq_recog_channel_t *recog_channel){
    xmlDocPtr pdoc = NULL;
    xmlNodePtr proot = NULL;

    xmlKeepBlanksDefault(0);//必须加上，防止程序把元素前后的空白文本符号当作一个node
    pdoc = xmlReadFile (recog_channel->result_filename, "UTF-8", XML_PARSE_RECOVER);//libxml只能解析UTF-8格式数据
    if(pdoc == NULL){
	  apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] pdoc error!\n");
      return;
    }
    xmlChar* xpath = BAD_CAST("//value");
    xmlXPathObjectPtr result = getNodeset(pdoc, xpath);
    if(result == NULL){
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"[pq] result is NULL\n");
        return;
    }else{
    xmlNodeSetPtr nodeset = result->nodesetval;
    xmlNodePtr cur = nodeset->nodeTab[0];
        if(cur != NULL){
            strcpy(value,((char*)XML_GET_CONTENT(cur->xmlChildrenNode)));
        }
    }

//释放
    xmlXPathFreeObject(result);
    xmlFreeDoc (pdoc);
    xmlCleanupParser ();
    xmlMemoryDump ();
    return;
}
const char* get_json_nlp(FILE* fp){
    struct json_object* cmd = NULL;
    char* buffer = (char*)malloc(1024);
    struct json_object *pobj;
    size_t fp_len = fread(buffer,1,1023,fp);
    pobj = json_tokener_parse(buffer);
    cmd = json_object_object_get(json_object_object_get(json_object_object_get(pobj,"data"),"answer"),"tts");
    const char* tts = json_object_get_string(cmd);
	free(buffer);
	return tts;
}
































